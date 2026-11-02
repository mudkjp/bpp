/*
 * Copyright (c) 2026, Pixel Brush <pixelbrush.dev>
 * Copyright (c) 2026, Aidan <JcbbcEnjoyer>
 *
 * SPDX-License-Identifier: AGPL-3.0-only
 *
*/
#pragma once
#include "dimensions.h"
#include "numeric_structs.h"
#include "world/world.h"
#include <cstdint>
#include <unordered_map>
#include <vector>

class Server;

// Sends accumulated per-Tick block changes out to whichever player sessions care about them
namespace ChunkBroadcaster {
inline void DeduplicateBlockChanges(std::vector<PendingBlock>& _changes) {
	if (_changes.size() <= 1)
		return;

	thread_local std::vector<PendingBlock> unique;
	thread_local std::unordered_map<int, size_t> indexOf;
	unique.clear();
	unique.reserve(_changes.size());

	if (_changes.size() <= 8) {
		for (const auto& pb : _changes) {
			bool replaced = false;
			for (auto& existing : unique) {
				if (existing.blockPos == pb.blockPos) {
					existing = pb;
					replaced = true;
					break;
				}
			}
			if (!replaced)
				unique.push_back(pb);
		}
	} else {
		indexOf.clear();
		indexOf.reserve(_changes.size());
		for (const auto& pb : _changes) {
			int key = (pb.blockPos.x & 15) | ((pb.blockPos.z & 15) << 4) | (pb.blockPos.y << 8);
			auto [it, inserted] = indexOf.try_emplace(key, unique.size());
			if (inserted)
				unique.push_back(pb);
			else
				unique[it->second] = pb;
		}
	}
	_changes.swap(unique);
}

void BroadcastBlockChanges(Server& _server, std::unordered_map<Int32_2, std::vector<PendingBlock>>& _changes,
                           Dimension _dimension, WorldManager& _dimWorld);
} // namespace ChunkBroadcaster
