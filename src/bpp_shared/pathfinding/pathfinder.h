/*
 * Copyright (c) 2026, Aidan <JcbbcEnjoyer>
 * Copyright (c) 2026, jwaxy <jwaxy.is-a.dev>
 *
 * SPDX-License-Identifier: AGPL-3.0-only
 * 
*/
#pragma once

#include "numeric_structs.h"
#include "world.h"
#include <cstdint>
#include <limits>
#include <optional>
#include <queue>
#include <unordered_map>
#include <vector>

struct Node {
	Int3 pos;

	float g = std::numeric_limits<float>::max();
	float h = 0.0f;
	float f = std::numeric_limits<float>::max();

	bool closed = false;

	int32_t parent = -1;
};

class Pathfinder {
public:
	WorldManager* world = nullptr;

	Pathfinder(){};
	Pathfinder(WorldManager* _world) : world(_world) {};

	[[nodiscard]] std::vector<Int3> FindPath(Int3 _start, Int3 _goal, float _width = 0.6f,
	                                          float _height = 1.8f, float _maxDistance = 16.0f);
	// Reuses the caller's vector, avoiding a temporary path per search.
	void FindPathInto(Int3 _start, Int3 _goal, std::vector<Int3>& _out, float _width = 0.6f,
	                  float _height = 1.8f, float _maxDistance = 16.0f);

private:
	enum class ColumnResult {
		Blocked = 0,
		Open = 1,
		Water = -1,
		Lava = -2,
	};

	struct PQNode {
		float f;
		uint32_t node;

		bool operator>(const PQNode& _rhs) const {
			return f > _rhs.f;
		}
	};

	std::priority_queue<PQNode, std::vector<PQNode>, std::greater<PQNode>> open;

	struct NodeRef {
		uint32_t index = 0;
		uint32_t generation = 0;
	};

	// Flat node arena reused across searches. Node links are indices so vector
	// growth cannot invalidate the open set or parent chain.
	std::vector<Node> nodes;
	// Retained across searches. A generation stamp makes entries from older
	// searches reusable without clearing the map (which would free every node).
	std::unordered_map<Int3, NodeRef> nodeIndex;
	uint32_t nodeGeneration = 1;
	static constexpr size_t MAX_RETAINED_NODES = 16384;

	Int3 footprint{ 1, 2, 1 };
	Int3 searchGoal{ 0, 0, 0 };

	uint32_t OpenNode(Int3 _pos);
	void Reset();

	ColumnResult GetVerticalOffset(Int3 _origin);

	std::optional<Int3> GetSafePoint(Int3 _pos, int _stepUp);

	int FindPathOptions(Int3 _current, float _maxDistance, uint32_t* _options);
};