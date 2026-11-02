/*
 * Copyright (c) 2026, Pixel Brush <pixelbrush.dev>
 * Copyright (c) 2026, Aidan <JcbbcEnjoyer>
 *
 * SPDX-License-Identifier: AGPL-3.0-only
 *
*/
#include "chunk_broadcaster.h"

#include "../server.h"

namespace {
struct BroadcastScratch {
	std::vector<PlayerSession*> sentOnly;
	NetworkStream stream{ -1 };
	Packet::SetMultipleBlocks multi;
};
} // namespace

void ChunkBroadcaster::BroadcastBlockChanges(Server& _server,
                                             std::unordered_map<Int32_2, std::vector<PendingBlock>>& _changes,
                                             Dimension _dimension, WorldManager& _dimWorld) {
	thread_local BroadcastScratch scratch;

	for (auto& [chunk, blockChanges] : _changes) {
		DeduplicateBlockChanges(blockChanges);
		// Find which sessions care about this chunk. The flushed list is owned
		// by the server index, so reference it instead of copying it per chunk.
		auto indexIt = _server.chunkSessions.find(Server::ChunkKey(chunk, _dimension));
		const std::vector<PlayerSession*>* flushed =
		    indexIt != _server.chunkSessions.end() ? &indexIt->second : nullptr;

		// Sessions that have the chunk in-flight (sentChunks but not flushedChunks) still need to queue the updates.
		auto& sentOnly = scratch.sentOnly;
		sentOnly.clear();
		for (auto& session : _server.players) {
			if (session->connState != ConnectionState::Playing &&
			    session->connState != ConnectionState::WaitingForSpawnChunks)
				continue;
			if (session->dimension != _dimension)
				continue;
			if (session->flushedChunks.contains(chunk))
				continue; // already in flushedSessions
			if (session->sentChunks.contains(chunk))
				sentOnly.push_back(session.get());
		}

		// Queue updates for sessions still waiting on the chunk to flush.
		for (auto* session : sentOnly) {
			auto& q = session->pendingBlockChanges[chunk];
			q.insert(q.end(), blockChanges.begin(), blockChanges.end());
		}
		if (!flushed || flushed->empty())
			continue;

		// Capture chunk ref once for sub-region jobs.
		std::shared_ptr<Chunk> chunkRef = _dimWorld.GetChunk(chunk);

		if (blockChanges.size() == 1) {
			// Single block change: serialise once, raw-copy to every session.
			const PendingBlock& pb = blockChanges[0];
			Packet::SetBlock sb;
			sb.block = { pb.block.type, pb.block.data };
			sb.position = { static_cast<int32_t>(pb.blockPos.x + (chunk.x * CHUNK_WIDTH)),
				            static_cast<int8_t>(pb.blockPos.y),
				            static_cast<int32_t>(pb.blockPos.z + (chunk.z * CHUNK_WIDTH)) };
			scratch.stream.ClearWriteBuffer();
			sb.Serialize(scratch.stream);
			const auto& buf = scratch.stream.GetRawWriteBuffer();
			for (auto* session : *flushed)
				session->stream.WriteRaw(buf.data(), buf.size());
		} else if (blockChanges.size() < 10) {
			// Multi-block packet; reuse the packet and its vectors between chunks.
			auto& smb = scratch.multi;
			smb.chunkPosition = { chunk.x, chunk.z };
			smb.blockCoordinates.clear();
			smb.blockMetadata.clear();
			smb.blockTypes.clear();
			smb.blockCoordinates.reserve(blockChanges.size());
			smb.blockMetadata.reserve(blockChanges.size());
			smb.blockTypes.reserve(blockChanges.size());
			for (const auto& pb : blockChanges) {
				const int16_t packed = static_cast<int16_t>(
				    ((int16_t(pb.blockPos.x) & 0x0F) << 12) | ((int16_t(pb.blockPos.z) & 0x0F) << 8) |
				    (int16_t(pb.blockPos.y) & 0xFF));
				smb.blockCoordinates.push_back(packed);
				smb.blockMetadata.push_back(int8_t(pb.block.data));
				smb.blockTypes.push_back(pb.block.type);
			}
			smb.numberOfBlocks = static_cast<int16_t>(smb.blockCoordinates.size());
			scratch.stream.ClearWriteBuffer();
			smb.Serialize(scratch.stream);
			const auto& buf = scratch.stream.GetRawWriteBuffer();
			for (auto* session : *flushed)
				session->stream.WriteRaw(buf.data(), buf.size());
		} else {
			// Sub-region: compression is async and shared across viewers via ChunkSender.
			for (auto* session : *flushed)
				_server.chunkSender.SendBlockUpdates(*session, chunk, blockChanges, chunkRef);
		}
	}
}
