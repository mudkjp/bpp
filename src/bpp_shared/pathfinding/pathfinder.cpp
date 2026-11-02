/*
 * Copyright (c) 2026, Aidan <JcbbcEnjoyer>
 * Copyright (c) 2026, jwaxy <jwaxy.is-a.dev>
 *
 * SPDX-License-Identifier: AGPL-3.0-only
 * 
*/
#include <algorithm>
#include <cmath>
#include <optional>
#include <queue>
#include <vector>

#include "blocks/block_properties.h"
#include "numeric_structs.h"
#include "pathfinder.h"

uint32_t Pathfinder::OpenNode(Int3 _pos) {
	auto [it, inserted] = nodeIndex.try_emplace(_pos);

	if (inserted || it->second.generation != nodeGeneration) {
		it->second.index = uint32_t(nodes.size());
		it->second.generation = nodeGeneration;
		nodes.emplace_back();
		nodes.back().pos = _pos;
		nodes.back().h = _pos.Distance(searchGoal);
	}

	return it->second.index;
}

void Pathfinder::Reset() {
	while (!open.empty())
		open.pop();

	nodes.clear();
	++nodeGeneration;
	if (nodeGeneration == 0 || nodeIndex.size() > MAX_RETAINED_NODES) {
		nodeIndex.clear();
		nodeGeneration = 1;
	}
}

Pathfinder::ColumnResult Pathfinder::GetVerticalOffset(Int3 _origin) {
	for (int x = _origin.x; x < _origin.x + footprint.x; x++) {
		for (int y = _origin.y; y < _origin.y + footprint.y; y++) {
			for (int z = _origin.z; z < _origin.z + footprint.z; z++) {
				Int3 pos{ x, y, z };
				BlockType block = world->GetBlockId(pos);
				if (block == BlockType::BLOCK_AIR)
					continue;

				if (block == BlockType::BLOCK_DOOR_WOOD || block == BlockType::BLOCK_DOOR_IRON) {
					// Bit 0x4 is the open/closed flag on either half of the door.
					// An open door doesn't block movement; a closed one does.
					if (!(world->GetMetadata(pos) & 4))
						return ColumnResult::Blocked;
					continue;
				}

				const Material& mat = Blocks::blockProperties[block].material;
				if (mat.isSolid)
					return ColumnResult::Blocked;
				if (mat.type == MaterialType::Water)
					return ColumnResult::Water;
				if (mat.type == MaterialType::Lava)
					return ColumnResult::Lava;
			}
		}
	}

	return ColumnResult::Open;
}

std::optional<Int3> Pathfinder::GetSafePoint(Int3 _pos, int _stepUp) {
	std::optional<Int3> found;

	if (GetVerticalOffset(_pos) == ColumnResult::Open) {
		found = _pos;
	} else if (_stepUp > 0) {
		Int3 stepped = _pos;
		stepped.y += _stepUp;
		if (GetVerticalOffset(stepped) == ColumnResult::Open)
			found = stepped;
	}

	if (!found)
		return std::nullopt;

	// Let the entity fall through open space below
	Int3 p = *found;
	ColumnResult below = ColumnResult::Blocked;
	int fallSteps = 0;

	while (p.y > 0) {
		Int3 belowPos = p;
		belowPos.y -= 1;
		below = GetVerticalOffset(belowPos);
		if (below != ColumnResult::Open)
			break;

		p.y -= 1;
		if (++fallSteps >= 4)
			return std::nullopt;
	}

	if (below == ColumnResult::Lava)
		return std::nullopt;

	return p;
}

// z+1, x-1, x+1, z-1
const Int2 CARDINAL_DIRS[4] = { { 0, 1 }, { -1, 0 }, { 1, 0 }, { 0, -1 } };

int Pathfinder::FindPathOptions(Int3 _current, float _maxDistance, uint32_t* _options) {
	int count = 0;

	// Stepping up a block is only allowed at all if there's headroom
	int stepUp = 0;
	Int3 headroom = _current;
	headroom.y += 1;
	if (GetVerticalOffset(headroom) == ColumnResult::Open)
		stepUp = 1;

	const double maxDistanceSq = double(_maxDistance) * double(_maxDistance);

	for (const auto& dir : CARDINAL_DIRS) {
		Int3 candidatePos = _current;
		candidatePos.x += dir.x;
		candidatePos.z += dir.y;

		auto safe = GetSafePoint(candidatePos, stepUp);
		if (!safe)
			continue;

		if (safe->DistanceSquared(searchGoal) >= maxDistanceSq)
			continue;

		// Create or fetch the node once here; the caller no longer has to
		// hash the same position again during relaxation.
		const uint32_t next = OpenNode(*safe);
		if (nodes[next].closed)
			continue;

		_options[count++] = next;
	}

	return count;
}

std::vector<Int3> Pathfinder::FindPath(Int3 _start, Int3 _goal, float _width, float _height, float _maxDistance) {
	std::vector<Int3> path;
	FindPathInto(_start, _goal, path, _width, _height, _maxDistance);
	return path;
}

void Pathfinder::FindPathInto(Int3 _start, Int3 _goal, std::vector<Int3>& _out, float _width, float _height,
                              float _maxDistance) {
	_out.clear();
	Reset();

	footprint = { std::max(1, int(std::floor(_width + 1.0f))), std::max(1, int(std::floor(_height + 1.0f))),
		          std::max(1, int(std::floor(_width + 1.0f))) };
	searchGoal = _goal;

	const uint32_t startNode = OpenNode(_start);
	nodes[startNode].g = 0.0f;
	nodes[startNode].f = nodes[startNode].h;

	open.push({ nodes[startNode].f, startNode });

	// Tracks the closest to goal node seen so far
	uint32_t bestNode = startNode;
	float bestDistance = nodes[startNode].h;

	uint32_t options[4];

	while (!open.empty()) {
		const uint32_t current = open.top().node;
		open.pop();

		Node& currentNode = nodes[current];
		if (currentNode.closed)
			continue;

		currentNode.closed = true;

		if (currentNode.h < bestDistance) {
			bestDistance = currentNode.h;
			bestNode = current;
		}

		if (currentNode.pos == _goal)
			break;

		// FindPathOptions can append nodes and reallocate the arena, so do not
		// keep a Node reference alive across it.
		const Int3 currentPos = currentNode.pos;
		const float currentG = currentNode.g;
		const int optionCount = FindPathOptions(currentPos, _maxDistance, options);
		for (int i = 0; i < optionCount; i++) {
			const uint32_t nextIndex = options[i];
			Node& next = nodes[nextIndex];
			if (next.closed)
				continue;

			const float g = currentG + currentPos.Distance(next.pos);
			if (g < next.g) {
				next.g = g;
				next.f = g + next.h;
				next.parent = int32_t(current);
				open.push({ next.f, nextIndex });
			}
		}
	}

	if (bestNode == startNode)
		return;

	size_t length = 0;
	for (int32_t n = int32_t(bestNode); n >= 0 && n != int32_t(startNode); n = nodes[size_t(n)].parent)
		++length;

	_out.resize(length);
	size_t write = length;
	for (int32_t n = int32_t(bestNode); n >= 0 && n != int32_t(startNode); n = nodes[size_t(n)].parent)
		_out[--write] = nodes[size_t(n)].pos;
}
