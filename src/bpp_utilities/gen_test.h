/*
 * Copyright (c) 2026, Pixel Brush <pixelbrush.dev>
 *
 * SPDX-License-Identifier: AGPL-3.0-only
 */

#pragma once

#include "logger.h"
#include "world/storage/region_manager.h"
#include "world/world.h"

#include <algorithm>
#include <array>
#include <chrono>
#include <climits>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <format>
#include <fstream>
#include <limits>
#include <memory>
#include <optional>
#include <string>
#include <thread>
#include <unordered_map>
#include <utility>
#include <vector>

// Overworld chunk generation speed test.
//
// Usage:  ChunkBenchmark::PerformChunkGenSpeedTest(gameRuntime.world);
//
// For every seed in GEN_TEST_SEEDS the overworld is generated GEN_TEST_RUNS_PER_SEED times at render distance
// GEN_TEST_RENDER_DISTANCE (a 41x41 square of chunks centred on 0,0). One "run" is one finished world: every chunk
// generated, every populatable chunk populated, and the light queue drained. min / max / average are reported
// per finished world.
//
// This is purely terrain generation churn: nothing is saved or loaded, and no entities are created or touched.
// The world is swapped over to an empty, uninitialised RegionManager (no folder, so no chunk ever "exists" on
// disk and everything gets generated) and nothing is ever written. The world's seed, chunks and region manager
// are put back afterwards. It still shares the world's thread pool and light queue, so call it at startup before
// players are connected.
namespace ChunkBenchmark {

inline constexpr int GEN_TEST_RENDER_DISTANCE = 20;
inline constexpr int GEN_TEST_RUNS_PER_SEED = 3;
// Untimed worlds generated first so cold-start costs (page faults, allocator growth, first use of the generators)
// don't land in the min/max. Set to 0 to disable.
inline constexpr int GEN_TEST_WARMUP_RUNS = 1;
// Give up on a world if it makes no progress for this long, so a stuck pipeline can't hang startup forever.
inline constexpr double GEN_TEST_STALL_TIMEOUT_SECONDS = 60.0;

// Generation workers to benchmark with. The world keeps its normal (server) pool size; the test only
// borrows more threads for its own run and puts the pool back afterwards. Half the hardware threads,
// clamped to [2, 8]: past 8 the remaining generation is serialized behind population and lighting
// anyway, so more workers mostly add scheduling noise.
inline const int GEN_TEST_THREADS = std::clamp(int(std::thread::hardware_concurrency()) / 2, 2, 8);

inline const std::array<std::string, 8> GEN_TEST_SEEDS = {
	"404", 
	"3257840388504953787", // Pack.png
	"Gargamel",
	"Glacier",
    "2409838883250561605", // (From Minecraft Seeds Tumblr)
    "worstseedever",
    "JASON",
    "Doom",
};

// Timings for a set of finished worlds.
struct GenSpeedStats {
	double minTime = std::numeric_limits<double>::max(); // seconds, fastest finished world
	double maxTime = 0.0;                                // seconds, slowest finished world
	double totalTime = 0.0;                              // seconds, sum of every finished world
	int worlds = 0;                                      // number of finished worlds
	int chunksPerWorld = 0;

	void Add(double _seconds) {
		minTime = std::min(minTime, _seconds);
		maxTime = std::max(maxTime, _seconds);
		totalTime += _seconds;
		worlds++;
	}

	double AverageTime() const {
		return worlds ? totalTime / worlds : 0.0;
	}

	double ChunksPerSecond() const {
		return totalTime > 0.0 ? double(chunksPerWorld) * worlds / totalTime : 0.0;
	}
};

struct GenSpeedResult {
	GenSpeedStats overall;
	std::vector<std::pair<std::string, GenSpeedStats>> perSeed;
	bool completed = false; // false if the test bailed out early (setup failure or stalled pipeline)
};

// A finished world, plus a fingerprint of everything it generated
struct FinishedWorld {
	double seconds = 0.0;
	uint64_t hash = 0;
};

// Wall time of each pipeline phase of one world. Temporary diagnostics, not part of the score.
struct PhaseTimes {
	double gen = 0.0;      // PumpPipeline + pool.wait: generation on the pool
	double queue = 0.0;    // DrainGenQueue / DrainLoadQueue / iopool.wait
	double populate = 0.0; // PopulateReady
	double light = 0.0;    // ProcessLightQueue
	double scan = 0.0;     // completion check
};

// Fingerprint of one chunk: blocks, meta, both light layers and climate. Only getters are used, so it
// describes the blocks/light the game would read back, not the storage layout.
inline uint64_t HashChunk(const Chunk& _chunk) {
	uint64_t h = 1469598103934665603ULL; // FNV-1a offset basis
	auto mix32 = [&h](uint32_t _v) {
		h ^= _v + 0x9e3779b97f4a7c15ULL + (h << 6) + (h >> 2);
	};
	auto mixBytes = [&](const void* _data, size_t _size) {
		const auto* bytes = static_cast<const uint8_t*>(_data);
		for (size_t i = 0; i < _size; i++)
			mix32(bytes[i]);
	};

	mix32(uint32_t(_chunk.cpos.x));
	mix32(uint32_t(_chunk.cpos.z));
	mix32(_chunk.isTerrainPopulated ? 1u : 0u);
	mixBytes(_chunk.heightMap, sizeof(_chunk.heightMap));
	mixBytes(_chunk.temperature, sizeof(_chunk.temperature));
	mixBytes(_chunk.humidity, sizeof(_chunk.humidity));
	for (uint8_t byte : _chunk.biomes.data)
		mix32(byte);
	// Blocks, meta and both light layers, exactly as the getters would read them back
	for (const SubChunk& sub : _chunk.subChunks) {
		for (int i = 0; i < SubChunk::VOLUME; i++) {
			uint32_t voxel = uint32_t(uint8_t(sub.GetBlock(i))) | (uint32_t(sub.GetMeta(i)) << 8) |
			                 (uint32_t(sub.GetBlockLight(i)) << 12) | (uint32_t(sub.GetSkyLight(i)) << 16);
			mix32(voxel);
		}
	}

	return h;
}

// Folds every chunk of a finished world into one hash, so two builds can be diffed for exact output
// equality. Chunk order doesn't matter (positions are sorted first). Computed after the timer stops.
inline uint64_t HashFinishedWorld(WorldManager& _world) {
	std::vector<Int32_2> positions;
	positions.reserve(_world.chunks.size());
	for (const auto& [pos, chunk] : _world.chunks)
		positions.push_back(pos);
	std::sort(positions.begin(), positions.end(), [](const Int32_2& _a, const Int32_2& _b) {
		if (_a.x != _b.x)
			return _a.x < _b.x;
		return _a.z < _b.z;
	});

	uint64_t h = 1469598103934665603ULL; // FNV-1a offset basis
	auto mix64 = [&h](uint64_t _v) {
		h ^= _v + 0x9e3779b97f4a7c15ULL + (h << 6) + (h >> 2);
	};

	mix64(positions.size());
	for (const Int32_2& pos : positions)
		mix64(HashChunk(*_world.chunks.at(pos)));

	return h;
}

// Writes a "cx cz hash" line per chunk, so differing chunks between two runs can be located.
// With BPP_CHUNK_HASH_DUMP_PARTS set, each line also carries per-component hashes (blocks, meta,
// block light, sky light, height, climate, biome) so a mismatch can be narrowed down.
inline void DumpChunkHashes(WorldManager& _world, const std::string& _path) {
	std::vector<Int32_2> positions;
	positions.reserve(_world.chunks.size());
	for (const auto& [pos, chunk] : _world.chunks)
		positions.push_back(pos);
	std::sort(positions.begin(), positions.end(), [](const Int32_2& _a, const Int32_2& _b) {
		if (_a.x != _b.x)
			return _a.x < _b.x;
		return _a.z < _b.z;
	});

	const bool parts = std::getenv("BPP_CHUNK_HASH_DUMP_PARTS") != nullptr;
	// Optional "cx,cz": also write the full voxel list of that chunk next to the hash file
	const char* dumpPosEnv = std::getenv("BPP_CHUNK_DUMP_POS");
	Int32_2 dumpPos{ 0, 0 };
	bool dumpPosSet = false;
	if (dumpPosEnv) {
		Int32_2 parsed{ 0, 0 };
		if (std::sscanf(dumpPosEnv, "%d,%d", &parsed.x, &parsed.z) == 2) {
			dumpPos = parsed;
			dumpPosSet = true;
		}
	}

	std::ofstream out(_path);
	std::ofstream voxels;
	if (dumpPosSet)
		voxels.open(_path + ".voxels.txt");
	for (const Int32_2& pos : positions) {
		const Chunk& chunk = *_world.chunks.at(pos);
		out << std::format("{} {} 0x{:016x}", pos.x, pos.z, HashChunk(chunk));
		if (parts) {
			uint64_t blocks = 1469598103934665603ULL, meta = blocks, blockLight = blocks, skyLight = blocks;
			auto mix = [](uint64_t& _h, uint64_t _v) {
				_h ^= _v + 0x9e3779b97f4a7c15ULL + (_h << 6) + (_h >> 2);
			};
			for (const SubChunk& sub : chunk.subChunks)
				for (int i = 0; i < SubChunk::VOLUME; i++) {
					mix(blocks, uint8_t(sub.GetBlock(i)));
					mix(meta, sub.GetMeta(i));
					mix(blockLight, sub.GetBlockLight(i));
					mix(skyLight, sub.GetSkyLight(i));
				}
			uint64_t height = 1469598103934665603ULL, climate = height, biome = height;
			for (uint8_t v : chunk.heightMap)
				mix(height, v);
			for (size_t i = 0; i < CHUNK_AREA; i++) {
				uint32_t temp = 0, humi = 0;
				std::memcpy(&temp, &chunk.temperature[i], sizeof(temp));
				std::memcpy(&humi, &chunk.humidity[i], sizeof(humi));
				mix(climate, temp);
				mix(climate, humi);
				mix(biome, chunk.biomes.Get(i));
			}
			out << std::format(" blocks=0x{:016x} meta=0x{:016x} blocklight=0x{:016x} skylight=0x{:016x} height=0x{:016x} climate=0x{:016x} biome=0x{:016x} popped={}", blocks, meta, blockLight, skyLight, height, climate, biome, chunk.isTerrainPopulated ? 1 : 0);
		}
		if (dumpPosSet && pos == dumpPos) {
			for (int y = 0; y < CHUNK_HEIGHT; y++)
				for (int x = 0; x < CHUNK_WIDTH; x++)
					for (int z = 0; z < CHUNK_WIDTH; z++) {
						Int3 p{ x, y, z };
						voxels << std::format("{} {} {} {} {} {} {}\n", x, y, z, int(chunk.GetBlock(p)),
						                      chunk.GetMeta(p), chunk.GetBlockLight(p), chunk.GetSkyLight(p));
					}
		}
		out << "\n";
	}
}

namespace Detail {

// Stashes the parts of the world the test is about to clobber, swaps in an empty region manager, and puts
// everything back when it goes out of scope (including on early return).
class WorldStateGuard {
public:
	WorldStateGuard(WorldManager& _world, RegionManager& _emptyRegions)
	    : world(_world), savedRegions(_world.regionManager), savedSeed(_world.seed),
	      savedChunks(std::move(_world.chunks)) {
		world.chunks.clear(); // A moved-from map is only guaranteed to be valid, not empty
		world.InvalidateChunkCache();
		world.regionManager = &_emptyRegions;
	}

	~WorldStateGuard() {
		world.pool.wait();
		world.chunks.clear();
		world.chunks = std::move(savedChunks);
		world.regionManager = savedRegions;
		world.InitWorldSeed(savedSeed); // Also restores the shared biome generator
		world.InvalidateChunkCache();
	}

	WorldStateGuard(const WorldStateGuard&) = delete;
	WorldStateGuard& operator=(const WorldStateGuard&) = delete;

private:
	WorldManager& world;
	RegionManager* savedRegions;
	int64_t savedSeed;
	std::unordered_map<Int32_2, std::shared_ptr<Chunk>> savedChunks;
};

// Generates one finished world at `_radius` and returns how long it took plus a hash of the result, or nullopt
// if the pipeline stalled. The world's chunk map is left populated; the caller clears it.
inline std::optional<FinishedWorld> GenerateFinishedWorld(WorldManager& _world, int _radius, PhaseTimes* _phases = nullptr) {
	using Clock = std::chrono::steady_clock;

	const int diameter = _radius * 2 + 1;
	const int totalChunks = diameter * diameter;
	// A chunk can only be populated once its +X, +Z and +X+Z neighbours exist, so the outer +X/+Z edge never is.
	const int populatableChunks = (diameter - 1) * (diameter - 1);

	// Request every chunk in the square
	for (int dx = -_radius; dx <= _radius; dx++) {
		for (int dz = -_radius; dz <= _radius; dz++) {
			Int32_2 pos{ dx, dz };
			auto chunk = std::make_shared<Chunk>();
			chunk->cpos = pos;
			_world.chunks.emplace(pos, std::move(chunk));
		}
	}

	auto start = Clock::now();
	auto lastProgress = start;
	int lastProgressCount = -1;

	// Same pump the server uses for spawn chunks, minus the logging. Waiting for the whole batch keeps
	// population decisions identical to the server's spawn path; draining as tasks finish is faster but
	// changes which light updates have been applied when a chunk decides where to place plants.
	while (true) {
		auto t0 = Clock::now();
		_world.PumpPipeline({});
		_world.pool.wait();
		auto t1 = Clock::now();
		_world.DrainGenQueue();
		_world.regionManager->iopool.wait();
		_world.DrainLoadQueue();
		auto t2 = Clock::now();
		_world.PopulateReady(INT_MAX);
		auto t3 = Clock::now();
		_world.lightManager.ProcessLightQueue(_world);
		auto t4 = Clock::now();

		int generated = 0;
		int populated = 0;
		for (auto& [pos, chunk] : _world.chunks) {
			if (chunk->state.load() >= ChunkState::Generated)
				generated++;
			if (chunk->isTerrainPopulated)
				populated++;
		}
		auto t5 = Clock::now();

		if (_phases) {
			_phases->gen += std::chrono::duration<double>(t1 - t0).count();
			_phases->queue += std::chrono::duration<double>(t2 - t1).count();
			_phases->populate += std::chrono::duration<double>(t3 - t2).count();
			_phases->light += std::chrono::duration<double>(t4 - t3).count();
			_phases->scan += std::chrono::duration<double>(t5 - t4).count();
		}

		if (generated >= totalChunks && populated >= populatableChunks)
			break;

		auto now = Clock::now();
		if (generated + populated != lastProgressCount) {
			lastProgressCount = generated + populated;
			lastProgress = now;
		} else if (std::chrono::duration<double>(now - lastProgress).count() > GEN_TEST_STALL_TIMEOUT_SECONDS) {
			GlobalLogger().error << std::format("Chunk gen test stalled with {}/{} generated and {}/{} populated.\n",
			                                    generated, totalChunks, populated, populatableChunks);
			return std::nullopt;
		}
	}

	FinishedWorld finished;
	finished.seconds = std::chrono::duration<double>(Clock::now() - start).count();
	// Hashing happens after the clock stops, so it can't skew the measurement
	finished.hash = HashFinishedWorld(_world);
	return finished;
}

// Throw away every chunk so the next run starts from nothing.
inline void DeleteAllChunks(WorldManager& _world) {
	_world.pool.wait();
	_world.chunks.clear();
	_world.InvalidateChunkCache();
}

inline void LogStats(const std::string& _label, const GenSpeedStats& _stats) {
	GlobalLogger().info << std::format("{} min {:.3f}s | max {:.3f}s | avg {:.3f}s | {:.0f} chunks/s ({} worlds)\n",
	                                   _label, _stats.minTime, _stats.maxTime, _stats.AverageTime(),
	                                   _stats.ChunksPerSecond(), _stats.worlds);
}

} // namespace Detail

inline GenSpeedResult PerformChunkGenSpeedTest(WorldManager& _world) {
	GenSpeedResult result;
	auto& log = GlobalLogger();

	if (_world.GetDimension() != Dimension::Overworld) {
		log.warn << "Chunk gen test only supports the overworld, skipping.\n";
		return result;
	}

	// Never initialised, so it has no folder and ChunkExists() is always false: every chunk gets generated.
	RegionManager emptyRegions;
	emptyRegions.world = &_world;
	// Destroyed before emptyRegions, so the world never points at a dead manager
	Detail::WorldStateGuard guard(_world, emptyRegions);

	const int radius = GEN_TEST_RENDER_DISTANCE;
	const int chunksPerWorld = (radius * 2 + 1) * (radius * 2 + 1);
	result.overall.chunksPerWorld = chunksPerWorld;

	log.info << std::format("Chunk gen test: {} seeds x {} worlds at render distance {} ({} chunks each), {} generator threads\n",
	                        GEN_TEST_SEEDS.size(), GEN_TEST_RUNS_PER_SEED, radius, chunksPerWorld, GEN_TEST_THREADS);

	// Benchmark with a dedicated worker count, then put the pool back the way the server had it.
	const std::size_t savedThreads = _world.pool.get_thread_count();
	_world.pool.reset(std::size_t(GEN_TEST_THREADS));

	bool ok = true;

	// Warmup, not counted
	for (int i = 0; ok && i < GEN_TEST_WARMUP_RUNS; i++) {
		log.info << "Warming up...\n";
		_world.InitWorldSeed(HashCode(GEN_TEST_SEEDS[0]));
		ok = Detail::GenerateFinishedWorld(_world, radius).has_value();
		Detail::DeleteAllChunks(_world);
	}

	for (std::string seed : GEN_TEST_SEEDS) {
		if (!ok)
			break;
		GenSpeedStats seedStats;
		seedStats.chunksPerWorld = chunksPerWorld;
		_world.InitWorldSeed(HashCode(seed));
		std::optional<uint64_t> seedHash;

		for (int run = 1; run <= GEN_TEST_RUNS_PER_SEED; run++) {
			PhaseTimes phases;
			auto finished = Detail::GenerateFinishedWorld(_world, radius, &phases);
			if (finished)
				if (const char* dumpDir = std::getenv("BPP_CHUNK_HASH_DUMP"))
					DumpChunkHashes(_world, std::format("{}/seed-{}-run-{}.txt", dumpDir, seed, run));
			Detail::DeleteAllChunks(_world);
			if (!finished) {
				ok = false;
				break;
			}
			seedStats.Add(finished->seconds);
			result.overall.Add(finished->seconds);
			log.info << std::format("  seed \"{}\" run {}/{}: {:.3f}s hash 0x{:016x}\n", seed, run,
			                        GEN_TEST_RUNS_PER_SEED, finished->seconds, finished->hash);
			log.info << std::format("    phases: gen {:.3f}s | queue {:.3f}s | populate {:.3f}s | light {:.3f}s | scan {:.3f}s\n",
			                        phases.gen, phases.queue, phases.populate, phases.light, phases.scan);
			if (seedHash && *seedHash != finished->hash)
				log.warn << std::format("  seed \"{}\" produced different output across runs!\n", seed);
			seedHash = finished->hash;
		}

		if (seedStats.worlds > 0) {
			Detail::LogStats(std::format("  seed \"{}\":", seed), seedStats);
			result.perSeed.emplace_back(seed, seedStats);
		}
	}

	result.completed = ok;
	if (result.overall.worlds > 0)
		Detail::LogStats(ok ? "Chunk gen test overall:" : "Chunk gen test (INCOMPLETE) so far:", result.overall);

	// Give the server its own pool size back
	_world.pool.wait();
	_world.pool.reset(savedThreads);
	return result;
}

} // namespace ChunkBenchmark