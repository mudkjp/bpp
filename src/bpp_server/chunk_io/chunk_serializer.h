/*
 * Copyright (c) 2026, Pixel Brush <pixelbrush.dev>
 * Copyright (c) 2026, Aidan <JcbbcEnjoyer>
 *
 * SPDX-License-Identifier: AGPL-3.0-only
 *
*/

#pragma once
#include "chunk.h"
#include "subchunk.h"
#include "helpers/deflate_pool.h"
#include <cstring>
#include <vector>

namespace ChunkSerializer {
inline std::vector<uint8_t> Serialize(const Chunk& _chunk, int _xmin = 0, int _xmax = CHUNK_WIDTH, int _ymin = 0,
                                      int _ymax = CHUNK_HEIGHT, int _zmin = 0, int _zmax = CHUNK_WIDTH) {
	const int sizeX = _xmax - _xmin;
	const int sizeY = _ymax - _ymin;
	const int sizeZ = _zmax - _zmin;

	const int blocks = sizeX * sizeY * sizeZ;
	const int nibbles = (blocks + 1) / 2;
	const int total = blocks + nibbles * 3;

	// NOTE: This means each per-thread chunk serializer grows as big as its biggest allocation.
	// So for a long-running server, which gets abused to the max (via explosions, for example),
	// this'd be ~5MB in the worst case (128x128x128) per thread.
	// TODO: Maybe shrink these after a few ticks?
	// Most chunk serializations should only take a few KB on average!
	thread_local std::vector<uint8_t> raw;
	raw.resize(size_t(total));
	uint8_t* blockData = raw.data();
	uint8_t* metaData = blockData + blocks;
	uint8_t* blockLight = metaData + nibbles;
	uint8_t* skyLight = blockLight + nibbles;

	// Block and light bytes are always fully written below. Metadata is the
	// only layer that may be skipped (a zero fill with no allocated layer),
	// so only clear that small region instead of the whole buffer.
	std::memset(metaData, 0, size_t(nibbles));

	auto packNibble = [](uint8_t& _byte, uint8_t _val, bool _high) {
		if (_high)
			_byte = uint8_t((_byte & 0x0F) | ((_val & 0x0F) << 4));
		else
			_byte = uint8_t((_byte & 0xF0) | (_val & 0x0F));
	};

	// The common full-chunk / sub-region case has an even y range, so two
	// vertical voxels share one output nibble byte. Handle them together:
	// fewer iterations, no per-voxel layer-presence branches, and one direct
	// write per nibble byte instead of read-modify-write twice.
	if ((_ymin & 1) == 0 && (_ymax & 1) == 0) {
		for (int x = _xmin; x < _xmax; x++) {
			for (int z = _zmin; z < _zmax; z++) {
				for (int s = 0; s < SUB_CHUNK_COUNT; s++) {
					const int sy0 = std::max<int>(_ymin, s * SUB_CHUNK_SIZE);
					const int sy1 = std::min<int>(_ymax, (s + 1) * SUB_CHUNK_SIZE);
					if (sy0 >= sy1)
						continue;

					const SubChunk& sub = _chunk.subChunks[size_t(s)];
					const SubChunk::BlockLayer* blockLayer = sub.blocks.get();
					const SubChunk::NibbleLayer* metaLayer = sub.meta.get();
					const SubChunk::NibbleLayer* blockLightLayer = sub.blockLight.get();
					const SubChunk::NibbleLayer* skyLightLayer = sub.skyLight.get();

					int i = sy0 + (z * CHUNK_HEIGHT) + (x * CHUNK_HEIGHT * CHUNK_WIDTH);
					int li = SubChunk::LocalIndex({ x, sy0, z });

					if (blockLayer && metaLayer && blockLightLayer && skyLightLayer) {
						// Fully allocated slice: no branches inside the hot loop.
						for (int y = sy0; y < sy1; y += 2, i += 2, li += SUB_CHUNK_SIZE * SUB_CHUNK_SIZE * 2) {
							const int li1 = li + SUB_CHUNK_SIZE * SUB_CHUNK_SIZE;
							blockData[i] = uint8_t(blockLayer->types[size_t(li)]);
							blockData[i + 1] = uint8_t(blockLayer->types[size_t(li1)]);
							metaData[i >> 1] = uint8_t((metaLayer->Get(li) & 0x0F) |
							                           ((metaLayer->Get(li1) & 0x0F) << 4));
							blockLight[i >> 1] = uint8_t((blockLightLayer->Get(li) & 0x0F) |
							                             ((blockLightLayer->Get(li1) & 0x0F) << 4));
							skyLight[i >> 1] = uint8_t((skyLightLayer->Get(li) & 0x0F) |
						                           ((skyLightLayer->Get(li1) & 0x0F) << 4));
						}
					} else if (!blockLayer && !metaLayer && !blockLightLayer && !skyLightLayer) {
						// Uniform slice: fill whole pairs without reading any layer.
						const uint8_t blockFill = uint8_t(sub.blockFill);
						const uint8_t metaFill = uint8_t(sub.metaFill | (sub.metaFill << 4));
						const uint8_t blockLightFill = uint8_t(sub.blockLightFill | (sub.blockLightFill << 4));
						const uint8_t skyLightFill = uint8_t(sub.skyLightFill | (sub.skyLightFill << 4));
						for (int y = sy0; y < sy1; y += 2, i += 2) {
							blockData[i] = blockFill;
							blockData[i + 1] = blockFill;
							metaData[i >> 1] = metaFill;
							blockLight[i >> 1] = blockLightFill;
							skyLight[i >> 1] = skyLightFill;
						}
					} else {
						for (int y = sy0; y < sy1; y += 2, i += 2, li += SUB_CHUNK_SIZE * SUB_CHUNK_SIZE * 2) {
							const int li1 = li + SUB_CHUNK_SIZE * SUB_CHUNK_SIZE;
							blockData[i] = blockLayer ? uint8_t(blockLayer->types[size_t(li)]) : uint8_t(sub.blockFill);
							blockData[i + 1] =
							    blockLayer ? uint8_t(blockLayer->types[size_t(li1)]) : uint8_t(sub.blockFill);

							if (metaLayer) {
								metaData[i >> 1] = uint8_t((metaLayer->Get(li) & 0x0F) |
								                           ((metaLayer->Get(li1) & 0x0F) << 4));
							} else {
								metaData[i >> 1] = uint8_t(sub.metaFill | (sub.metaFill << 4));
							}

							if (blockLightLayer) {
								blockLight[i >> 1] = uint8_t((blockLightLayer->Get(li) & 0x0F) |
								                             ((blockLightLayer->Get(li1) & 0x0F) << 4));
							} else {
								blockLight[i >> 1] = uint8_t(sub.blockLightFill | (sub.blockLightFill << 4));
							}

							if (skyLightLayer) {
								skyLight[i >> 1] = uint8_t((skyLightLayer->Get(li) & 0x0F) |
							                           ((skyLightLayer->Get(li1) & 0x0F) << 4));
							} else {
								skyLight[i >> 1] = uint8_t(sub.skyLightFill | (sub.skyLightFill << 4));
							}
						}
					}
				}
			}
		}
		return Deflate::Compress(Deflate::Format::Zlib, raw, 1);
	}

	int i = 0;
	for (int x = _xmin; x < _xmax; x++) {
		for (int z = _zmin; z < _zmax; z++) {
			for (int y = _ymin; y < _ymax;) {
				const int slabEnd = std::min<int>(_ymax, (y | (SUB_CHUNK_SIZE - 1)) + 1);
				const SubChunk& sub = _chunk.subChunks[size_t(y >> 4)];

				const SubChunk::BlockLayer* blockLayer = sub.blocks.get();
				const SubChunk::NibbleLayer* metaLayer = sub.meta.get();
				const SubChunk::NibbleLayer* blockLightLayer = sub.blockLight.get();
				const SubChunk::NibbleLayer* skyLightLayer = sub.skyLight.get();
				const uint8_t blockFill = uint8_t(sub.blockFill);
				const uint8_t metaFill = sub.metaFill;
				const uint8_t blockLightFill = sub.blockLightFill;
				const uint8_t skyLightFill = sub.skyLightFill;

				for (; y < slabEnd; y++, i++) {
					const int idx = SubChunk::LocalIndex({ x, y, z });
					blockData[i] = blockLayer ? uint8_t(blockLayer->types[size_t(idx)]) : blockFill;
					// The buffer is zero-initialised, so a zero meta fill needs no write
					if (metaLayer)
						packNibble(metaData[i >> 1], metaLayer->Get(idx), i & 1);
					else if (metaFill != 0)
						packNibble(metaData[i >> 1], metaFill, i & 1);
					packNibble(blockLight[i >> 1],
					           blockLightLayer ? blockLightLayer->Get(idx) : blockLightFill, i & 1);
					packNibble(skyLight[i >> 1],
					           skyLightLayer ? skyLightLayer->Get(idx) : skyLightFill, i & 1);
				}
			}
		}
	}

	return Deflate::Compress(Deflate::Format::Zlib, raw, 1);
}
} // namespace ChunkSerializer