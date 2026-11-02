/*
 * Copyright (c) 2026, Pixel Brush <pixelbrush.dev>
 *
 * SPDX-License-Identifier: AGPL-3.0-only
 *
*/

#include "item_map.h"
#include "blocks.h"
#include "world.h"
#include "../enums/network/packet_data.h"
#include "../logger/logger.h"
#include <cmath>

namespace Map {

MarkerPlacement MakeMarker(const Icon _icon, const float _rotation, const Byte2 _position) {
	uint8_t rot = static_cast<uint8_t>(std::round((std::fmod(_rotation, 360.0f) / 360.0f) * 16.0f));
	return MarkerPlacement{ .icon = _icon, .rotation = rot, .position = _position };
}

void AppendPixel(std::vector<uint8_t>& _mapData, const uint8_t _value) {
	if (_mapData.size() > INT8_MAX) {
		Logger::Warning << "Attempted to write too much map data in a single packet! Rejected! (" << _mapData.size()
		                    << "/" << INT8_MAX << ")\n";
		return;
	}
	_mapData.push_back(_value);
}

void InitGraphics(std::vector<uint8_t>& _mapData, const Byte2 _offset) {
	_mapData = { PacketData::MapDataType::GRAPHICS, static_cast<uint8_t>(_offset.x), static_cast<uint8_t>(_offset.z) };
}

void PlaceMarkers(std::vector<uint8_t>& _mapData, const std::vector<MarkerPlacement> _markers) {
	_mapData.clear();
	_mapData.reserve(1 + (_markers.size() * 3));
	_mapData.push_back(PacketData::MapDataType::ICON);
	for (const auto& m : _markers) {
		_mapData.push_back(static_cast<uint8_t>((m.rotation << 4) | static_cast<uint8_t>(m.icon)));
		_mapData.push_back(m.position.x);
		_mapData.push_back(m.position.z);
	}
}

// Held item maps
std::map<int16_t, Item> mapItems = {};
static int16_t globalId = 0;

void InitMaps() {
	// TODO: Find the highest map value from files (this can wrap around)
	globalId = 0;
}
void CreateMap(const Int32_2 _origin) {
	globalId++;
	auto newMap = Item();
	newMap.Init(_origin);
	//maps[globalId] = std::move(newMap);
}

void BuildMarkerData(std::vector<uint8_t>& _mapData, const MarkerPlacement& _marker) {
	_mapData.clear();
	_mapData.reserve(4);
	_mapData.push_back(PacketData::MapDataType::ICON);
	_mapData.push_back(
	    static_cast<uint8_t>((_marker.rotation << 4) | static_cast<uint8_t>(_marker.icon)));
	_mapData.push_back(_marker.position.x);
	_mapData.push_back(_marker.position.z);
}

void BuildColumnData(std::vector<uint8_t>& _mapData, WorldManager& _world, int _x) {
	// The column covers z = -1 (for the north slope) through 127, so at most
	// nine chunks: z chunks -1..7.
	constexpr int MIN_CHUNK_Z = -1;
	constexpr int MAX_CHUNK_Z = 7;
	Chunk* zChunks[MAX_CHUNK_Z - MIN_CHUNK_Z + 1] = {};
	const int chunkX = _x >> 4;
	for (int cz = MIN_CHUNK_Z; cz <= MAX_CHUNK_Z; ++cz) {
		Chunk* chunk = _world.GetChunkRaw({ chunkX, cz });
		if (chunk && chunk->state.load(std::memory_order_acquire) >= ChunkState::Generated)
			zChunks[cz - MIN_CHUNK_Z] = chunk;
	}

	auto chunkAt = [&](int _z) -> Chunk* { return zChunks[(_z >> 4) - MIN_CHUNK_Z]; };
	auto heightAt = [&](int _z) -> int {
		Chunk* chunk = chunkAt(_z);
		return chunk ? int(chunk->GetHeightValue({ _x & 15, _z & 15 })) : 0;
	};
	auto blockAt = [&](int _y, int _z) -> BlockType {
		if (_y < 0 || _y >= CHUNK_HEIGHT)
			return BLOCK_AIR;
		Chunk* chunk = chunkAt(_z);
		return chunk ? chunk->GetBlock({ _x & 15, _y, _z & 15 }) : BLOCK_AIR;
	};

	InitGraphics(_mapData, Byte2{ static_cast<int8_t>(_x), 0 });
	for (int z = 0; z < Item::MAP_ITEM_WIDTH; ++z) {
		const int northY = heightAt(z - 1);
		const int y = heightAt(z);
		const BlockType block = blockAt(y - 1, z);
		int8_t brightness = 1;
		if (northY > y)
			brightness--;
		if (northY < y)
			brightness++;
		AppendPixel(_mapData, uint8_t((Blocks::blockProperties[block].material.mapColor.index << 2) | brightness));
	}
}

void Item::Init(const Int32_2 _origin) {
	// TODO: Check file for existing map
	// Nope, this is a new map, so lets go
	origin = _origin;
	dirty = true;
	for (uint16_t i = 0; i < MAP_ITEM_AREA; i++)
		data[i] = MaterialType::Grass;
}

void Item::Tick() {
	if (!dirty)
		return;
}
}; // namespace Map