/*
 * Copyright (c) 2026, Aidan <JcbbcEnjoyer>
 * Copyright (c) 2026, Pixel Brush <pixelbrush.dev>
 *
 * SPDX-License-Identifier: AGPL-3.0-only
 *
*/

#pragma once
#include "world.h"
#include "world_access.h"

// 3x3 region of chunk pointers, centered on the chunk being populated
struct ChunkPtrRegion {
	std::shared_ptr<Chunk> chunks[3][3];

	Chunk* GetChunk(Int2 _pos) const {
		if (_pos.x < -1 || _pos.x > 1 || _pos.z < -1 || _pos.z > 1)
			return nullptr;
		return chunks[_pos.x + 1][_pos.z + 1].get();
	}
};

// Wrapper for world access during chunk population.
// Holds a 3x3 region of chunk pointers centered on the chunk being populated.
// Chunks are marked inUse on acquire and released on free.
class WorldWrapper : public WorldAccess {
public:
	WorldManager& manager;
	ChunkPtrRegion chunkRegion;
	Int2 centerChunkPos;

	WorldWrapper(WorldManager& _manager, Int2 _centerChunkPos) : manager(_manager), centerChunkPos(_centerChunkPos) {}

	// Grab the 3x3 region. Any chunk that is already inUse is left as nullptr
	// (writes to it will fall through to the deferred path via the manager).
	void GetChunkRegion();
	void FreeChunkRegion();
	// Convert a world-space position to a region-local chunk offset (-1..1, -1..1)
	inline Int2 GetRegionChunkPos(const Int3 _wPos) const {
		return { (_wPos.x >> 4) - centerChunkPos.x, (_wPos.z >> 4) - centerChunkPos.z };
	}

	int FindTopSolidBlock(const int _wx, const int _wz) override;
	int GetHeightValue(const int _wx, const int _wz) override;
	double GetTemperatureAt(const int _wx, const int _wz) override;

	double GetHumidityAt(const int _wx, const int _wz) override;
	// Inlined on purpose: feature generation calls this hundreds of millions of times while populating.
	inline BlockType GetBlockId(const Int3 _wpos) override {
		if (!InBounds(_wpos.y))
			return BlockType::BLOCK_AIR;
		Chunk* chunk = chunkRegion.GetChunk(GetRegionChunkPos(_wpos));
		// Falls outside our grabbed region -> ask the manager directly (read-only, safe)
		if (!chunk)
			return manager.GetBlockId(_wpos);
		if (chunk->state.load(std::memory_order_acquire) < ChunkState::Generated)
			return BlockType::BLOCK_AIR;
		return chunk->GetBlock({ _wpos.x & 15, _wpos.y, _wpos.z & 15 });
	}
	void SetBlock(const Int3 _wpos, const BlockType _type, const uint8_t _meta = 0, const bool _keepTileEntity = false,
	              const bool _updateNeighbors = true) override;

	// Feature-generation helper: same as GetBlockId(_wpos) == _required ? SetBlock(_wpos, _type) : nothing,
	// but the chunk lookup and block read are only done once. Returns true when the block was replaced.
	inline bool SetBlockIf(const Int3 _wpos, const BlockType _required, const BlockType _type) {
		if (!InBounds(_wpos.y))
			return false;
		Chunk* chunk = chunkRegion.GetChunk(GetRegionChunkPos(_wpos));
		if (!chunk || chunk->state.load(std::memory_order_acquire) < ChunkState::Generated) {
			// Same fallback order the plain GetBlockId + SetBlock pair would take
			if (manager.GetBlockId(_wpos) != _required)
				return false;
			SetBlock(_wpos, _type);
			return true;
		}
		const Int3 local{ _wpos.x & 15, _wpos.y, _wpos.z & 15 };
		const BlockType oldBlock = chunk->GetBlock(local);
		if (oldBlock != _required)
			return false;
		ApplyBlockChange(*chunk, local, _wpos, _type, 0, oldBlock);
		return true;
	}
	uint8_t GetSkyLight(const Int3 _wpos) override;
	int GetBlockLightRaw(const Int3 _wpos) override;

	int64_t GetSeed() const {
		return manager.seed;
	}

	bool CanBlockSeeSky(const Int3 _pos) {
		auto chunk = chunkRegion.GetChunk(GetRegionChunkPos(_pos));
		if (!chunk)
			return false;
		Int3 localPos = { _pos.x & 15, _pos.y, _pos.z & 15 };

		return chunk->CanBlockSeeSky(localPos);
	}

	// Returns true when the world-space Y is within valid chunk bounds.
	static constexpr bool InBounds(int _y) {
		return _y >= 0 && _y < CHUNK_HEIGHT;
	}

private:
	// Shared implementation of the block update in SetBlock/SetBlockIf; `_oldBlock` is what
	// currently sits at `_local`.
	void ApplyBlockChange(Chunk& _chunk, Int3 _local, Int3 _wpos, BlockType _type, uint8_t _meta,
	                      BlockType _oldBlock);
};