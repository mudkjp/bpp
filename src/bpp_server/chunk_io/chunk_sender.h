/*
 * Copyright (c) 2026, Pixel Brush <pixelbrush.dev>
 * Copyright (c) 2026, Aidan <JcbbcEnjoyer>
 * Copyright (c) 2026, Anya Rihtarshich <vesui@proton.me>
 *
 * SPDX-License-Identifier: AGPL-3.0-only
 *
*/
#pragma once
#include "../player_conn/player_session.h"
#include "BS_thread_pool.hpp"
#include "chunk_serializer.h"
#include "helpers/hash.h"
#include "networking/packets.h"
#include "tile_entities/tile_entity.h"
#include "world/world.h"
#include <algorithm>
#include <climits>
#include <cstdint>
#include <future>
#include <memory>
#include <thread>
#include <unordered_map>
#include <vector>

// Identifies one compressed block-update sub-region. Multiple viewers of the
// same chunk receive the same bbox and changes, so they can share one job.
struct SubRegionKey {
	Int32_2 chunk;
	const Chunk* chunkPtr;
	int32_t xmin, xmax, ymin, ymax, zmin, zmax;

	bool operator==(const SubRegionKey&) const = default;
};

struct SubRegionKeyHasher {
	size_t operator()(const SubRegionKey& _key) const noexcept {
		size_t h = 0;
		HashCombine(h, _key.chunk);
		HashCombine(h, reinterpret_cast<uintptr_t>(_key.chunkPtr));
		HashCombine(h, _key.xmin);
		HashCombine(h, _key.xmax);
		HashCombine(h, _key.ymin);
		HashCombine(h, _key.ymax);
		HashCombine(h, _key.zmin);
		HashCombine(h, _key.zmax);
		return h;
	}
};

// ChunkSender offloads zlib chunk serialization onto a thread-pool
struct ChunkSender {
	// One result slot per in-flight chunk.
	struct PendingChunk {
		Int32_2 pos;
		std::shared_future<Packet::ChunkData> data; // shared!! async compression result
		std::weak_ptr<Chunk> chunkRef;          // world may unload; lock only when flushing
	};

	// One result slot per in-flight sub-region block update (>= 10 changes).
	struct PendingSubRegion {
		Int32_2 chunkPos;
		Packet::ChunkData header; // pre-filled, payload comes from the shared future
		std::shared_future<std::shared_ptr<const std::vector<uint8_t>>> data;
	};

	struct ScanState {
		Int32_2 center{ INT32_MIN, INT32_MIN };
		int radius = -1;
		bool valid = false;
		std::vector<Int32_2> retry;
	};

	// Per-session queue of in-flight full-chunk serialization jobs.
	std::unordered_map<PlayerSession*, std::vector<PendingChunk>> inFlight;

	// Per-session queue of in-flight sub-region compression jobs.
	// Drained in-order by flush() after the full-chunk queue.
	std::unordered_map<PlayerSession*, std::vector<PendingSubRegion>> subRegionFlight;

	// Per-session cached chunk-radius scan state. With an unchanged center and
	// radius only the small retry list is checked, not the whole view square.
	std::unordered_map<PlayerSession*, ScanState> scanStates;
	// Diagnostic counter: number of radius cells examined by full scans.
	size_t radiusLookups = 0;

	BS::thread_pool<> pool{ 2 };

	// reuse chunk packets across clients for 1 tick.
	std::unordered_map<const Chunk*, std::shared_future<Packet::ChunkData>> tickPackets;
	// Reuse sub-region compression across all viewers for one tick.
	std::unordered_map<SubRegionKey, std::shared_future<std::shared_ptr<const std::vector<uint8_t>>>,
	                   SubRegionKeyHasher>
	    tickSubRegions;

	void BeginTick() {
		tickPackets.clear();
		tickSubRegions.clear();
	}

	size_t Enqueue(PlayerSession& _session, WorldManager& _world, int _batchSize = -1) {
		if (_batchSize < 0)
			_batchSize = static_cast<int>(pool.get_thread_count()) * 2;
		int cx = int(std::floor(_session.position.pos.x)) >> 4;
		int cz = int(std::floor(_session.position.pos.z)) >> 4;

		int radius = _session.position.viewDistanceOverride
		                 ? std::min(_session.position.viewDistanceOverride, _world.GetViewRadius())
		                 : _world.GetViewRadius();

		ScanState& scan = scanStates[&_session];
		const Int32_2 center{ cx, cz };
		const bool rebuild = !scan.valid || scan.center != center || scan.radius != radius;
		std::vector<Int32_2> toSend;

		if (rebuild) {
			scan.valid = true;
			scan.center = center;
			scan.radius = radius;
			scan.retry.clear();
			const size_t side = size_t(radius) * 2 + 1;
			radiusLookups += side * side;

			for (int dx = -radius; dx <= radius; dx++) {
				for (int dz = -radius; dz <= radius; dz++) {
					Int32_2 p{ cx + dx, cz + dz };
					if (_session.sentChunks.contains(p))
						continue;
					auto worldIt = _world.chunks.find(p);
					if (worldIt == _world.chunks.end() || !worldIt->second ||
					    worldIt->second->state.load() < ChunkState::Generated) {
						scan.retry.push_back(p);
						continue;
					}
					toSend.push_back(p);
				}
			}
		} else {
			// Same center and radius: only positions that were missing or not
			// generated last time can have become sendable.
			std::vector<Int32_2> retry;
			retry.reserve(scan.retry.size());
			for (const Int32_2& p : scan.retry) {
				if (_session.sentChunks.contains(p))
					continue;
				auto worldIt = _world.chunks.find(p);
				if (worldIt == _world.chunks.end() || !worldIt->second ||
				    worldIt->second->state.load() < ChunkState::Generated) {
					retry.push_back(p);
					continue;
				}
				toSend.push_back(p);
			}
			scan.retry.swap(retry);
		}

		// Unload all out-of-range chunks immediately
		std::vector<Int32_2> toUnload;
		for (auto& p : _session.sentChunks) {
			if (std::abs(p.x - cx) > radius || std::abs(p.z - cz) > radius)
				toUnload.push_back(p);
		}
		for (auto& p : toUnload) {
			Packet::SetChunkVisibility vis;
			vis.pos.x = p.x;
			vis.pos.z = p.z;
			vis.visible = false;
			vis.Serialize(_session.stream);
			_session.sentChunks.erase(p);
			_session.flushedChunks.erase(p);
			_session.pendingBlockChanges.erase(p); // drop queued updates for unloaded chunk
			_session.newlyUnloaded.push_back(p);
		}

		// Also cancel any in-flight jobs for chunks that are now out of range
		auto& queue = inFlight[&_session];
		queue.erase(std::remove_if(queue.begin(), queue.end(),
		                           [&](const PendingChunk& _pc) {
			                           return std::abs(_pc.pos.x - cx) > radius || std::abs(_pc.pos.z - cz) > radius;
		                           }),
		            queue.end());
		auto subIt = subRegionFlight.find(&_session);
		if (subIt != subRegionFlight.end()) {
			std::erase_if(subIt->second, [&](const PendingSubRegion& _psr) {
				return std::abs(_psr.chunkPos.x - cx) > radius || std::abs(_psr.chunkPos.z - cz) > radius;
			});
		}

		// Rebuild toSend, excluding chunks that already have an in-flight job.
		int submitted = 0;
		size_t sendIndex = 0;
		for (; sendIndex < toSend.size(); ++sendIndex) {
			if (_batchSize > 0 && queue.size() >= static_cast<size_t>(_batchSize))
				break;
			const Int32_2& p = toSend[sendIndex];
			PendingChunk pc;
			std::shared_ptr<Chunk> chunkRef = _world.chunks.at(p);
			pc.pos = p;
			pc.chunkRef = chunkRef;
			auto [packet, inserted] = tickPackets.try_emplace(chunkRef.get());
			if (inserted) {
				packet->second = pool.submit_task([chunkRef]() {
					Packet::ChunkData data;
					data.pos.x = chunkRef->cpos.x * CHUNK_WIDTH;
					data.pos.z = chunkRef->cpos.z * CHUNK_WIDTH;
					data.compressedData = ChunkSerializer::Serialize(*chunkRef);
					return data;
				}).share();
			}
			pc.data = packet->second;
			queue.push_back(std::move(pc));
			_session.sentChunks.insert(p);
			_session.newlySent.push_back(p);
			++submitted;
		}
		// Anything we didn't submit this tick must be retried on the next one,
		// even if the player has not moved.
		for (; sendIndex < toSend.size(); ++sendIndex)
			scan.retry.push_back(toSend[sendIndex]);
		return static_cast<size_t>(submitted);
	}

	void SendBlockUpdates(PlayerSession& _session, const Int32_2& _chunk, const std::vector<PendingBlock>& _changes,
	                      std::shared_ptr<const Chunk> _chunkRef = nullptr) {
		if (_changes.empty() || !_chunkRef)
			return;

		// Find bounding box in chunk-local space
		auto& p0 = _changes[0].blockPos;
		int xmin = p0.x, xmax = p0.x;
		int ymin = p0.y, ymax = p0.y;
		int zmin = p0.z, zmax = p0.z;
		for (const auto& pb : _changes) {
			const auto& pos = pb.blockPos;
			if (pos.x > xmax)
				xmax = pos.x;
			if (pos.x < xmin)
				xmin = pos.x;
			if (pos.y > ymax)
				ymax = pos.y;
			if (pos.y < ymin)
				ymin = pos.y;
			if (pos.z > zmax)
				zmax = pos.z;
			if (pos.z < zmin)
				zmin = pos.z;
		}
		// Force even ySize so the client's nibble copy doesn't desync
		ymin = (ymin / 2) * 2;
		ymax = (ymax / 2 + 1) * 2 - 1;
		ymin = std::max<int>(ymin, 0);
		ymax = std::min<int>(ymax, CHUNK_HEIGHT - 1);

		PendingSubRegion psr;
		psr.chunkPos = _chunk;
		psr.header.pos.x = _chunk.x * CHUNK_WIDTH + xmin;
		psr.header.pos.z = _chunk.z * CHUNK_WIDTH + zmin;
		psr.header.pos.y = static_cast<int16_t>(ymin);
		psr.header.size.x = static_cast<uint8_t>(xmax - xmin);
		psr.header.size.y = static_cast<uint8_t>(ymax - ymin);
		psr.header.size.z = static_cast<uint8_t>(zmax - zmin);

		const SubRegionKey key{ _chunk, _chunkRef.get(), xmin, xmax, ymin, ymax, zmin, zmax };
		auto [job, inserted] = tickSubRegions.try_emplace(key);
		if (inserted) {
			std::shared_ptr<const Chunk> ref = _chunkRef;
			job->second = pool.submit_task([ref, xmin, xmax, ymin, ymax, zmin, zmax]() {
				return std::make_shared<const std::vector<uint8_t>>(
				    ChunkSerializer::Serialize(*ref, xmin, xmax + 1, ymin, ymax + 1, zmin, zmax + 1));
			}).share();
		}
		psr.data = job->second;
		subRegionFlight[&_session].push_back(std::move(psr));
	}

	// Drains every job that is already done and writes the resulting
	// SetChunkVisibility + ChunkData packets to the session stream.
	// Jobs that aren't finished yet stay in the queue for the next Tick.
	void Flush(PlayerSession& _session) {
		auto it = inFlight.find(&_session);
		if (it == inFlight.end())
			return;

		auto& queue = it->second;
		std::vector<PendingChunk> stillPending;

		for (auto& pc : queue) {
			// Non-blocking check: only consume results that are ready now.
			if (pc.data.wait_for(std::chrono::seconds(0)) != std::future_status::ready) {
				stillPending.push_back(std::move(pc));
				continue;
			}

			Packet::SetChunkVisibility vis;
			vis.pos.x = pc.pos.x;
			vis.pos.z = pc.pos.z;
			vis.visible = true;
			vis.Serialize(_session.stream);

			pc.data.get().Serialize(_session.stream);

			_session.flushedChunks.insert(pc.pos);
			_session.newlyFlushed.push_back(pc.pos);

			auto chunkLocked = pc.chunkRef.lock();

			// Drain any block updates that queued up while this chunk
			// was in-flight. They go out immediately after the chunk
			// data in the same Tick, so the client receives them in
			// order and applies them to freshly loaded terrain.
			auto pending = _session.pendingBlockChanges.find(pc.pos);
			if (pending != _session.pendingBlockChanges.end()) {
				SendBlockUpdates(_session, pc.pos, pending->second,
				                 chunkLocked ? std::shared_ptr<const Chunk>(chunkLocked) : nullptr);
				_session.pendingBlockChanges.erase(pending);
			}

			// Signs need to inform the client when they are loaded
			if (chunkLocked) {
				for (auto te : chunkLocked->tileEntities) {
					if (te->type != TileType::SIGN)
						continue;
					auto sign = std::static_pointer_cast<TileEntitySign>(te);

					Packet::UpdateSign pkt;
					pkt.position = SlimInt3<int16_t>{ sign->position.x, static_cast<short>(sign->position.y),
						                              sign->position.z };
					pkt.lines[0] = sign->text1;
					pkt.lines[1] = sign->text2;
					pkt.lines[2] = sign->text3;
					pkt.lines[3] = sign->text4;

					pkt.Serialize(_session.stream);
				}
			}
		}

		queue = std::move(stillPending);

		// Drain in-flight sub-region compression jobs in submission order.
		auto srIt = subRegionFlight.find(&_session);
		if (srIt != subRegionFlight.end()) {
			auto& srQueue = srIt->second;
			size_t ready = 0;
			while (ready < srQueue.size()) {
				auto& psr = srQueue[ready];
				if (!psr.data.valid()) {
					++ready;
					continue;
				}
				if (psr.data.wait_for(std::chrono::seconds(0)) != std::future_status::ready)
					break;
				psr.header.sharedCompressedData = psr.data.get();
				psr.header.Serialize(_session.stream);
				++ready;
			}
			// Erase the whole finished prefix at once; erasing front() in a
			// loop moves every remaining packet on each iteration.
			if (ready > 0)
				srQueue.erase(srQueue.begin(), srQueue.begin() + static_cast<std::ptrdiff_t>(ready));
		}
	}

	// Remove all in-flight state for a disconnected session.
	void Remove(PlayerSession& _session) {
		inFlight.erase(&_session);
		subRegionFlight.erase(&_session);
		scanStates.erase(&_session);
	}
};