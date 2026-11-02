/*
 * Copyright (c) 2026, Aidan <JcbbcEnjoyer>
 * Copyright (c) 2026, Pixel Brush <pixelbrush.dev>
 * Copyright (c) 2026, Anya Rihtarshich <vesui@proton.me>
 *
 * SPDX-License-Identifier: AGPL-3.0-only
 *
*/

#include "world.h"
#include "blocks.h"
#include "blocks/block_behaviors.h"
#include "entities/entity_lightning_bolt.h"
#include "chunk.h"
#include "direction.h"
#include "entities/entity_item.h"
#include "generator/nether/chunk_gen.h"
#include "generator/overworld/chunk_gen.h"
#include "generator/shared/cave_gen.h"
#include "managers/redstone_manager.h"
#include "world_wrapper.h"
#include <limits>
#include <memory>
#include <unordered_set>

BiomeGenerator WorldManager::biomeGenerator;

Biome WorldManager::GetBiome(Int2 _wpos) {
	if (isHell)
		return Biome::BIOME_HELL;
	auto chunk = GetChunkShared(Int32_2{ _wpos.x >> 4, _wpos.z >> 4 });
	if (!chunk || chunk->state.load() < ChunkState::Generated)
		return biomeGenerator.GetBiomeAtPoint(_wpos);
	EnsureClimate(*chunk);

	const int32_t localX = _wpos.x & 15;
	const int32_t localZ = _wpos.z & 15;
	return static_cast<Biome>(chunk->biomes.Get(localX * CHUNK_WIDTH + localZ));
}

void WorldManager::EnsureClimate(Chunk& _chunk) {
	// The Nether has no climate
	if (isHell || _chunk.climateBaked)
		return;

	thread_local std::unique_ptr<BiomeGenerator> tlBiomeGen;
	thread_local int64_t tlBiomeSeed = std::numeric_limits<int64_t>::min();
	if (!tlBiomeGen || tlBiomeSeed != this->seed) {
		tlBiomeGen = std::make_unique<BiomeGenerator>(this->seed);
		tlBiomeSeed = this->seed;
	}
	thread_local std::unique_ptr<double[]> climateBuffer;
	if (!climateBuffer)
		climateBuffer = std::make_unique<double[]>(size_t(CHUNK_AREA) * 3);
	double* temp = climateBuffer.get();
	double* humi = temp + CHUNK_AREA;
	double* weird = humi + CHUNK_AREA;
	tlBiomeGen->GenerateBiomeMap(_chunk.biomes, std::span<double>(temp, CHUNK_AREA),
	                             std::span<double>(humi, CHUNK_AREA), std::span<double>(weird, CHUNK_AREA),
	                            Int2{ _chunk.cpos.x * CHUNK_WIDTH, _chunk.cpos.z * CHUNK_WIDTH });
	for (int i = 0; i < CHUNK_AREA; ++i) {
		_chunk.temperature[i] = float(temp[i]);
		_chunk.humidity[i] = float(humi[i]);
	}
	_chunk.climateBaked = true;
}

int WorldManager::GetBlockLightValue(Int3 _wpos, bool _offsetNonFullBlocks) {
	if (_offsetNonFullBlocks) {
		auto blockId = this->GetBlockId(_wpos);
		if (blockId == BLOCK_SLAB || blockId == BLOCK_FARMLAND || blockId == BLOCK_STAIRS_WOOD ||
		    blockId == BLOCK_STAIRS_COBBLESTONE) {
			int yP = GetBlockLightValue(_wpos.WithOffset(Cardinal::Value::Up), false);
			int xP = GetBlockLightValue(_wpos.WithOffset(Cardinal::Value::East), false);
			int xM = GetBlockLightValue(_wpos.WithOffset(Cardinal::Value::West), false);
			int zP = GetBlockLightValue(_wpos.WithOffset(Cardinal::Value::South), false);
			int zM = GetBlockLightValue(_wpos.WithOffset(Cardinal::Value::North), false);

			if (xP > yP)
				yP = xP;
			if (xM > yP)
				yP = xM;
			if (zP > yP)
				yP = zP;
			if (zM > yP)
				yP = zM;

			return yP;
		}
	}

	if (_wpos.y < 0) {
		return 0;
	} else {
		if (_wpos.y >= CHUNK_HEIGHT)
			_wpos.y = CHUNK_HEIGHT - 1;
		auto chunk = this->GetChunkRaw({ _wpos.x >> 4, _wpos.z >> 4 });
		if (!chunk)
			return 15;

		Int3 localPos = { _wpos.x & 15, _wpos.y, _wpos.z & 15 };
		int skylight = chunk->GetSkyLight(localPos) - this->skylightOffset;
		int blockLight = chunk->GetBlockLight(localPos);
		if (blockLight > skylight)
			return blockLight;
		else
			return skylight;
	}
}

void WorldManager::UpdateSkylightOffset() {
	float celestialAngle = GetCelestialAngle();
	float transformedAngle = 1.0f - (std::cos(celestialAngle * JavaMath::PI * 2.0f) * 2.0f + 0.5f);
	if (transformedAngle < 0.0f)
		transformedAngle = 0.0f;
	if (transformedAngle > 1.0f)
		transformedAngle = 1.0f;

	transformedAngle = 1.0f - transformedAngle;
	transformedAngle = float(double(transformedAngle) * (1.0 - double(weatherSystem.rainStrength * 5.0f) / 16.0));
	transformedAngle = float(double(transformedAngle) *
	                         (1.0 - double(weatherSystem.thunderStrength * weatherSystem.rainStrength * 5.0f) / 16.0));
	transformedAngle = 1.0f - transformedAngle;

	this->skylightOffset = int(transformedAngle * 11.0f);
}

float WorldManager::GetCelestialAngle() {
	int normalizedTime = int(this->elapsedTicks % DAY_LENGTH);

	// Subtract 1/4 of a day so sunrise = 0
	float timePercent = float(normalizedTime + 1.0f) / float(DAY_LENGTH) - 0.25f;
	if (timePercent < 0.0f)
		timePercent++;
	if (timePercent > 1.0f)
		timePercent--;

	// Merge linear time with cosine time
	float linearAngle = timePercent;
	timePercent = 1.0f - (std::cos(timePercent * JavaMath::PI) + 1.0f) / 2.0f;
	timePercent = linearAngle + (timePercent - linearAngle) / 3.0f;
	return timePercent;
}

bool WorldManager::IsAabbInFluidLevel(const AABB& _collider, const Material& _material) {
	int minX = MathHelper::FloorDouble(_collider.minX);
	int maxX = MathHelper::FloorDouble(_collider.maxX + 1.0);
	int minY = MathHelper::FloorDouble(_collider.minY);
	int maxY = MathHelper::FloorDouble(_collider.maxY + 1.0);
	int minZ = MathHelper::FloorDouble(_collider.minZ);
	int maxZ = MathHelper::FloorDouble(_collider.maxZ + 1.0);

	for (int x = minX; x < maxX; x++) {
		for (int z = minZ; z < maxZ; z++) {
			Chunk* chunk = GetChunkRaw({ x >> 4, z >> 4 });
			if (!IsChunkValid(chunk))
				continue;
			const int lx = x & 15;
			const int lz = z & 15;
			for (int y = minY; y < maxY; y++) {
				BlockType blockId = chunk->GetBlock({ lx, y, lz });
				if (!(Blocks::blockProperties[blockId].material == _material))
					continue;

				uint8_t meta = chunk->GetMeta({ lx, y, lz });
				double surface = double(y + 1);
				if (meta < 8)
					surface -= double(meta) / 8.0;

				if (surface >= _collider.minY)
					return true;
			}
		}
	}
	return false;
}

bool WorldManager::IsMaterialInAabb(const AABB& _collider, const Material& _material) {
	int minX = MathHelper::FloorDouble(_collider.minX);
	int maxX = MathHelper::FloorDouble(_collider.maxX + 1.0);
	int minY = MathHelper::FloorDouble(_collider.minY);
	int maxY = MathHelper::FloorDouble(_collider.maxY + 1.0);
	int minZ = MathHelper::FloorDouble(_collider.minZ);
	int maxZ = MathHelper::FloorDouble(_collider.maxZ + 1.0);

	// Check every block within the collider
	// We are looking to see if the materials match
	for (int x = minX; x < maxX; x++) {
		for (int z = minZ; z < maxZ; z++) {
			Chunk* chunk = GetChunkRaw({ x >> 4, z >> 4 });
			if (!IsChunkValid(chunk))
				continue;
			const int lx = x & 15;
			const int lz = z & 15;
			for (int y = minY; y < maxY; y++) {
				BlockType blockId = chunk->GetBlock({ lx, y, lz });
				const auto& block = Blocks::blockProperties[blockId];
				if (block.material == _material) {
					return true;
				}
			}
		}
	}
	return false;
}

// TODO: Duplicated code, merge with IsMaterialInAabb and IsAabbInFluidLevel
bool WorldManager::IsLiquidInAabb(const AABB& _collider) {
	int minX = MathHelper::FloorDouble(_collider.minX);
	int maxX = MathHelper::FloorDouble(_collider.maxX + 1.0);
	int minY = MathHelper::FloorDouble(_collider.minY);
	int maxY = MathHelper::FloorDouble(_collider.maxY + 1.0);
	int minZ = MathHelper::FloorDouble(_collider.minZ);
	int maxZ = MathHelper::FloorDouble(_collider.maxZ + 1.0);

	// Check every block within the collider
	for (int x = minX; x < maxX; x++) {
		for (int z = minZ; z < maxZ; z++) {
			Chunk* chunk = GetChunkRaw({ x >> 4, z >> 4 });
			if (!IsChunkValid(chunk))
				continue;
			const int lx = x & 15;
			const int lz = z & 15;
			for (int y = minY; y < maxY; y++) {
				BlockType blockId = chunk->GetBlock({ lx, y, lz });
				const auto& block = Blocks::blockProperties[blockId];
				if (block.material.isLiquid) {
					return true;
				}
			}
		}
	}
	return false;
}

bool WorldManager::HandleFluidAcceleration(const AABB& _collider, const Material& _material, Entity& _entity) {
	// Handles the fluid push physics, only counts fluids of the same material
	// Returns whether the entity is in the material
	// This is almost entirely used for water
	int minX = MathHelper::FloorDouble(_collider.minX);
	int maxX = MathHelper::FloorDouble(_collider.maxX + 1.0);
	int minY = MathHelper::FloorDouble(_collider.minY);
	int maxY = MathHelper::FloorDouble(_collider.maxY + 1.0);
	int minZ = MathHelper::FloorDouble(_collider.minZ);
	int maxZ = MathHelper::FloorDouble(_collider.maxZ + 1.0);
	if (!this->AABBinValidChunks({ double(minX), double(minY), double(minZ), double(maxX), double(maxY), double(maxZ) }))
		return false;

	bool inMaterial = false;
	Vec3 pushVector = { 0, 0, 0 };

	// Check every block within the collider
	// We are looking to see if the materials match
	for (int x = minX; x < maxX; x++)
		for (int y = minY; y < maxY; y++)
			for (int z = minZ; z < maxZ; z++) {
				auto blockId = this->GetBlockId({ x, y, z });
				const auto& block = Blocks::blockProperties[blockId];
				if (block.material == _material) {
					double fluidHeight = double(float(y + 1) -
					                            Blocks::GetFluidPercentAir(this->GetMetadata({ x, y, z })));
					if (double(maxY) >= fluidHeight) {
						// We are definitely in this material
						// Lets get how this material contributes to our flow vector
						inMaterial = true;
						auto velocityFunction = Blocks::blockBehaviors[blockId].velocityToAddToEntity;
						if (velocityFunction)
							velocityFunction(*this, { x, y, z }, pushVector);
					}
				}
			}

	// Normalize the vector
	auto magnitude = std::sqrt(pushVector.x * pushVector.x + pushVector.y * pushVector.y + pushVector.z * pushVector.z);
	if (magnitude > 0.0) {
		pushVector.x /= magnitude;
		pushVector.y /= magnitude;
		pushVector.z /= magnitude;

		// Apply the vector
		double pushForce = 0.014;
		_entity.velocity += pushVector * pushForce;
	}

	return inMaterial;
}

// Get colliders for an area
std::vector<AABB> WorldManager::GetCollidingBoundingBoxes(const AABB& _area, Entity* _mover) {
	std::vector<AABB> collidingBoxes;

	int minX = Java::DoubleToInt32(std::floor(_area.minX));
	int maxX = Java::DoubleToInt32(std::floor(_area.maxX + 1.0));
	int minY = Java::DoubleToInt32(std::floor(_area.minY));
	int maxY = Java::DoubleToInt32(std::floor(_area.maxY + 1.0));
	int minZ = Java::DoubleToInt32(std::floor(_area.minZ));
	int maxZ = Java::DoubleToInt32(std::floor(_area.maxZ + 1.0));

	int startY = std::max<int>(0, minY - 1);
	int endY = std::min<int>(127, maxY);

	// Iterate for our potential grid
	for (int x = minX; x < maxX; x++) {
		for (int z = minZ; z < maxZ; z++) {
			// Get the chunk once for this X/Z column
			Int2 cpos = { x >> 4, z >> 4 };
			Chunk* chunk = GetChunkRaw(cpos);

			// If chunk isn't loaded, treat it as air
			if (!IsChunkValid(chunk))
				continue;

			// local coords inside the chunk
			int localX = x & 15;
			int localZ = z & 15;

			for (int y = startY; y <= endY; ++y) {
				BlockType blockId = chunk->GetBlock({ localX, y, localZ });
				// Air isn't collidable
				if (blockId == BlockType::BLOCK_AIR)
					continue;
				if (!Blocks::blockProperties[blockId].isCollidable)
					continue;
				uint8_t blockMeta = chunk->GetMeta({ localX, y, localZ });
				if (blockId == BLOCK_PISTON_MOVING) {
					// Moving pistons get a special case
					// They use a full block collider instead of the multi segment collider
					auto* te = GetTileEntityAs<TileEntityPistonMoving>({ x, y, z });
					if (!te)
						continue;
					float p = te->lastProgress;
					if (te->extending)
						p = 1.0f - p;
					if (auto box = te->GetCollider(p).GetBounds(); box && box->Intersects(_area))
						collidingBoxes.push_back(*box);
					continue;
				}

				// Most blocks use the plain unit-cube collider. Handle that
				// directly instead of allocating a CollisionShape and an
				// offset copy for every solid voxel touched by the sweep.
				if (Blocks::colliderTable[blockId] == Blocks::DefaultCollider) {
					const AABB box{ double(x), double(y), double(z), double(x + 1), double(y + 1), double(z + 1) };
					if (box.Intersects(_area))
						collidingBoxes.push_back(box);
					continue;
				}

				const CollisionShape& collider = Blocks::colliderTable[blockId](blockMeta);
				for (size_t i = 0; i < collider.boxes.size(); ++i) {
					const AABB offset = collider.boxes[i].Offset(x, y, z);
					if (offset.Intersects(_area))
						collidingBoxes.push_back(offset);
				}
			}
		}
	}
	// Do we have the mover flag set?
	bool needsFullEntityScan = _mover ? _mover->GetMoverCollisionOverride(*_mover).has_value() : false;

	// Collect entities in this area, excluding the mover itself
	AABB entitySearchArea = { double(minX), double(minY), double(minZ), double(maxX), double(maxY), double(maxZ) };
	thread_local std::vector<Entity*> entitiesInArea;
	entitiesInArea.clear();

	if (needsFullEntityScan)
		entityManager.GetEntitiesWithinAabbExcluding(entitySearchArea, _mover->id, entitiesInArea);
	else
		entityManager.GetCollidablesWithinAabb(entitySearchArea, _mover ? _mover->id : EntityId(-1), entitiesInArea);

	for (auto& entity : entitiesInArea) {
		if (entity->actsAsWorldCollider && entity->collider.Intersects(_area))
			collidingBoxes.push_back(entity->collider.Expand(-0.1, -0.1, -0.1));

		if (needsFullEntityScan) {
			auto moverOverrideBox = _mover->GetMoverCollisionOverride(*entity);
			if (moverOverrideBox && moverOverrideBox->Intersects(_area))
				collidingBoxes.push_back(*moverOverrideBox);
		}
	}

	return collidingBoxes;
}

void WorldManager::ProcessDeferredUpdates() {
	if (deferredUpdates.empty())
		return;

	// Process the whole batch against the final world state. Replaying is
	// suppressed so callbacks cannot recursively append to this vector while
	// it is being iterated.
	const bool wasReplaying = replayingDeferred;
	replayingDeferred = true;
	for (const auto& update : deferredUpdates) {
		if (update.type == DeferredUpdateType::Neighbors)
			NotifyNeighborsOfUpdate(update.position, update.block);
		else
			RedstoneManager::TriggerRedstoneUpdate(*this, update.position, update.block, update.oldBlock);
	}
	deferredUpdates.clear();
	replayingDeferred = wasReplaying;
}

// Tick
void WorldManager::Tick(const std::vector<ClientPosition>& _players, bool _advanceSimulation, TickProfiler* _profiler) {
	using Clock = std::chrono::steady_clock;

	if (_advanceSimulation) {
		auto environmentStart = _profiler ? Clock::now() : Clock::time_point{};
		++elapsedTicks;
		if (_profiler)
			_profiler->Record(TickTask::Environment, Clock::now() - environmentStart);
	}
	if (!regionManager) {
		Logger::Error << "No region manager while trying to Tick!\n";
		return;
	}
	if (_advanceSimulation) {
		auto environmentStart = _profiler ? Clock::now() : Clock::time_point{};
		UpdateSkylightOffset();
		if (_profiler)
			_profiler->Record(TickTask::Environment, Clock::now() - environmentStart);
	}

	auto loadingStart = _profiler ? Clock::now() : Clock::time_point{};
	DrainGenQueue();  // process generation results first
	DrainLoadQueue(); // integrate finished loads
	
	if (_profiler)
		_profiler->Record(TickTask::ChunkLoading, Clock::now() - loadingStart);
	if (_advanceSimulation) {
		auto blockStart = _profiler ? Clock::now() : Clock::time_point{};
		replayingDeferred = true;
		ProcessDeferredUpdates();
		replayingDeferred = false;
		tickScheduler.Tick();
		if (_profiler)
			_profiler->Record(TickTask::BlockUpdates, Clock::now() - blockStart);
		auto start = _profiler ? Clock::now() : Clock::time_point{};
		entitySpawner.TrySpawnEntities(*this, _players);
		if (_profiler)
			_profiler->Record(TickTask::MobSpawning, Clock::now() - start);
		blockStart = _profiler ? Clock::now() : Clock::time_point{};
		PerformRandomTicks(_players);
		if (_profiler)
			_profiler->Record(TickTask::BlockUpdates, Clock::now() - blockStart);
		start = _profiler ? Clock::now() : Clock::time_point{};
		entityManager.Tick(_profiler && _profiler->EntityProfilingEnabled() ? _profiler : nullptr);
		if (_profiler)
			_profiler->Record(TickTask::EntityTicks, Clock::now() - start);
		start = _profiler ? Clock::now() : Clock::time_point{};
		tileEntityManager.TickTileEntities(*this);
		if (_profiler)
			_profiler->Record(TickTask::BlockEntityTicks, Clock::now() - start);
		start = _profiler ? Clock::now() : Clock::time_point{};
		if (!isHell)
			weatherSystem.Tick(rand);
		if (_profiler)
			_profiler->Record(TickTask::Environment, Clock::now() - start);
		// Saving
		if (tickScheduler.currentTick % 40 == 0) {
			auto saveStart = _profiler ? Clock::now() : Clock::time_point{};
			SaveChunks(tickScheduler.currentTick % 600 == 0);
			if (_profiler)
				_profiler->Record(TickTask::Autosave, Clock::now() - saveStart);
		}
	}
	lightManager.ProcessLightQueue(*this, INT_MAX);
	auto unloadingStart = _profiler ? Clock::now() : Clock::time_point{};
	UpdateLoadRadius(_players);
	if (_profiler)
		_profiler->Record(TickTask::ChunkUnloading, Clock::now() - unloadingStart);
	auto pipelineStart = _profiler ? Clock::now() : Clock::time_point{};
	regionManager->PumpPipeline();
	PopulateReady();
	if (_profiler)
		_profiler->Record(TickTask::ChunkLoading, Clock::now() - pipelineStart);
}

void WorldManager::RebuildRandomTickCandidates(const std::vector<ClientPosition>& _players) {
	const int simulationDist = GetSimulationDistance();
	const int simulationWidth = (simulationDist * 2) + 1;
	randomTickCandidates.clear();

	if (_players.size() == 1) {
		// Single player: no overlap to deduplicate, so skip the hash set.
		const auto anchor = _players[0].GetChunkPos();
		randomTickCandidates.reserve(size_t(simulationWidth * simulationWidth));
		for (int dx = -simulationDist; dx <= simulationDist; dx++)
			for (int dz = -simulationDist; dz <= simulationDist; dz++)
				randomTickCandidates.push_back({ dx + anchor.x, dz + anchor.z });
	} else {
		// Reuse the scratch set across rebuilds instead of allocating one.
		randomTickSeen.clear();
		randomTickSeen.reserve(_players.size() * size_t(simulationWidth * simulationWidth));
		for (auto& player : _players) {
			auto anchor = player.GetChunkPos();
			for (int dx = -simulationDist; dx <= simulationDist; dx++) {
				for (int dz = -simulationDist; dz <= simulationDist; dz++) {
					Int32_2 chunkPos = { dx + anchor.x, dz + anchor.z };
					if (randomTickSeen.insert(chunkPos).second)
						randomTickCandidates.push_back(chunkPos);
				}
			}
		}
	}

	cachedRandomTickPlayerChunks.clear();
	cachedRandomTickPlayerChunks.reserve(_players.size());
	for (const auto& player : _players)
		cachedRandomTickPlayerChunks.push_back(player.GetChunkPos());
	cachedRandomTickDistance = simulationDist;
	randomTickCacheValid = true;
	++randomTickRebuildCount;
}

void WorldManager::PerformRandomTicks(const std::vector<ClientPosition>& _players) {
	const int simulationDist = GetSimulationDistance();
	bool rebuild =
	    !randomTickCacheValid || cachedRandomTickDistance != simulationDist ||
	    cachedRandomTickPlayerChunks.size() != _players.size();
	if (!rebuild) {
		for (size_t i = 0; i < _players.size(); ++i) {
			if (cachedRandomTickPlayerChunks[i] != _players[i].GetChunkPos()) {
				rebuild = true;
				break;
			}
		}
	}
	if (rebuild)
		RebuildRandomTickCandidates(_players);

	// Weather cannot change while random ticks run, so resolve it once.
	const bool raining = weatherSystem.IsRaining();
	const bool thundering = raining && weatherSystem.IsThundering();

	for (const Int32_2& cpos : randomTickCandidates) {
		Chunk* chunk = GetChunkRaw(cpos);
		if (!chunk || chunk->state.load() != ChunkState::Populated)
			continue;
		Int2 chunkOrigin = { cpos.x * 16, cpos.z * 16 };

		// Lightning
		if (rand.NextInt(100000) == 0 && raining && thundering) {
			distHashCounter = distHashCounter * 3 + 1013904223;
			uint32_t r = distHashCounter >> 2;
			int lx = int(r & 15);
			int lz = int((r >> 8) & 15);
			Int2 column = { chunkOrigin.x + lx, chunkOrigin.z + lz };
			int y = FindTopSolidBlock(column.x, column.z);
			if (CanRainHitSpot({ column.x, y, column.z })) {
				auto lEntityRegular = LightningEntity(this, { double(column.x), double(y), double(column.z) });
				std::shared_ptr<LightningEntity> lightningEntity = std::make_shared<LightningEntity>(lEntityRegular);
				entityManager.AddEntity(lightningEntity);
			}
		}

		// Snowfall and freezing
		if (!isHell && rand.NextInt(16) == 0) {
			distHashCounter = distHashCounter * 3 + 1013904223;
			uint32_t r = distHashCounter >> 2;
			int lx = int(r & 15);
			int lz = int((r >> 8) & 15);
			Int2 column = { chunkOrigin.x + lx, chunkOrigin.z + lz };

			// Reject warm biomes before walking the whole column.
			if (BiomeGenerator::BiomeHasSnow(GetBiome(column))) {
				int y = FindTopSolidBlock(column.x, column.z);
				if (y >= 0 && y < CHUNK_HEIGHT && chunk->GetBlockLight({ lx, y, lz }) < 10) {
					Int3 top = { column.x, y, column.z };
					Int3 below = { column.x, y - 1, column.z };
					BlockType atId = chunk->GetBlock({ lx, y, lz });
					BlockType belowId = chunk->GetBlock({ lx, y - 1, lz });
					const auto& belowProps = Blocks::blockProperties[belowId];

					if (raining && atId == BLOCK_AIR && belowId != BLOCK_AIR && belowId != BLOCK_ICE &&
					    belowProps.isOpaqueCube && belowProps.material.isSolid)
						SetBlock(top, BLOCK_SNOW_LAYER);

					if (belowId == BLOCK_WATER_STILL && chunk->GetMeta({ lx, y - 1, lz }) == 0)
						SetBlock(below, BLOCK_ICE);
				}
			}
		}

		// 80 random block ticks
		for (int i = 0; i < 80; i++) {
			distHashCounter = distHashCounter * 3 + 1013904223;
			uint32_t randomBlockPos = distHashCounter >> 2;
			int posX = int(randomBlockPos & 15);
			int posZ = int((randomBlockPos >> 8) & 15);
			int posY = int((randomBlockPos >> 16) & 127);
			auto block = chunk->GetBlock({ posX, posY, posZ });
			if (!Blocks::ticksOnLoadTable[block])
				continue;
			if (auto func = Blocks::onTickTable[block]) {
				Int3 worldPos = { chunkOrigin.x + posX, posY, chunkOrigin.z + posZ };
				func(*this, worldPos, chunk->GetMeta({ posX, posY, posZ }), this->rand);
			}
		}
	}
}

void WorldManager::SaveChunks(const bool _saveIfEntities, const bool _deleteEntities) {
	for (auto& [pos, chunk] : chunks) {
		ChunkState s = chunk->state.load();
		if (s < ChunkState::Generated)
			continue;
		if (s == ChunkState::Generating || s == ChunkState::Loading)
			continue;
		if (chunk->isModified || (entityManager.ChunkHasEntities(pos) && _saveIfEntities))
			regionManager->SaveChunk(chunk, _deleteEntities);
		chunk->isModified = false;
	}
}

void WorldManager::Update(const std::vector<ClientPosition>& _players) {
	PumpPipeline(_players);
}

void WorldManager::DropInventory(Inventory& _inventory, Int3 _wpos) {
	// Drop an inventory at a given block coordinate
	for (auto& stack : _inventory.slots) {
		if (stack.id != Items::INVALID) {
			float offsetX = rand.NextFloat() * 0.8f + 0.1f;
			float offsetY = rand.NextFloat() * 0.8f + 0.1f;
			float offsetZ = rand.NextFloat() * 0.8f + 0.1f;

			while (stack.count > 0) {
				int countDecrement = rand.NextInt(21) + 10;
				countDecrement = std::min(countDecrement, int(stack.count));
				stack.count -= countDecrement;

				Vec3 spawnPos = { _wpos.x + offsetX, _wpos.y + offsetY, _wpos.z + offsetZ };

				std::shared_ptr<ItemEntity> newItem = std::make_shared<ItemEntity>(spawnPos);
				newItem->itemStack = { .id = stack.id, .count = ItemAmount(countDecrement), .data = stack.data };

				// Random velocity
				float velocity = 0.05f;
				newItem->velocity.x = rand.NextDouble() * velocity;
				newItem->velocity.y = rand.NextDouble() * velocity + 0.2f;
				newItem->velocity.z = rand.NextDouble() * velocity;
				this->entityManager.AddEntity(newItem);
			}

			// Clear this stack
			stack = {};
		}
	}
}

void WorldManager::Shutdown() {
	if (!regionManager)
		return;
	if (isHell) {
		Logger::Info << "Saving chunks for level -1\n";
	} else {
		Logger::Info << "Saving chunks for level 0\n";
	}

	// Make sure lighting is up to date
	lightManager.ProcessLightQueue(*this);

	// Save
	SaveChunks(/*SaveIfEntities=*/true, /*Delete Entities On Save=*/true);

	// For every position that still has pending bleed writes, forceload or forcegenerate the chunk, apply the writes, then save it.
	while (!pendingBleedWrites.empty()) {
		auto it = pendingBleedWrites.begin();
		Int32_2 cpos = it->first;

		// Insert a placeholder if not already in the map
		if (!chunks.contains(cpos)) {
			auto c = std::make_shared<Chunk>();
			c->cpos = cpos;
			chunks.emplace(cpos, std::move(c));
			TrackUnloadedChunk(cpos);
			populateFrontier.emplace(cpos.x, cpos.z);
		}

		// Wait until the chunk is ready
		while (chunks[cpos]->state.load() < ChunkState::Generated) {
			PumpPipeline({});
			pool.wait();
			DrainGenQueue();
			regionManager->iopool.wait();
			DrainLoadQueue();
		}

		// Apply the pending writes
		FlushBleedWrites();

		// Make sure lighting is up to date
		lightManager.ProcessLightQueue(*this);

		// Save it
		auto& chunk = chunks[cpos];
		if (chunk->isModified) {
			regionManager->SaveChunk(chunk);
			chunk->isModified = false;
		}
	}

	// Flush everything to disk and wait for IO to finish
	regionManager->FlushAll();
}

void WorldManager::DrainGenQueue() {
	// Integrate chunks that finished generating
	std::deque<std::shared_ptr<Chunk>> ready;
	{
		std::lock_guard lk(genDoneMutex);
		ready.swap(genDoneQueue);
	}
	for (auto& c : ready) {
		Int32_2 pos = c->cpos;
		auto it = chunks.find(pos);

		// Chunk was unloaded while generating, or placeholder was replaced
		if (it == chunks.end() || !it->second)
			continue;

		// Something overwrote our placeholder
		if (it->second->state.load(std::memory_order_acquire) != ChunkState::Generating)
			continue;

		if (it != chunks.end()) {
			bool wasSpawnChunk = it->second->spawnChunk;
			it->second = std::move(c);
			it->second->spawnChunk = wasSpawnChunk;

			// Replay any writes that arrived while this chunk was unloaded.
			auto pit = pendingBleedWrites.find(pos);
			if (pit != pendingBleedWrites.end()) {
				for (auto& [wpos, block] : pit->second)
					SetBlock(wpos, block.type, block.data);
				pendingBleedWrites.erase(pit);
			}

			// The generator already built the heightmap and skylight map on
			// its worker thread. Rebuilding them here would do the same full
			// 32K-voxel pass again on the tick thread.
			this->SeedChunkLighting(it->second->cpos); // Reseed our lighting
		}
	}
}

void WorldManager::DrainLoadQueue() {
	// Recover chunks whose load never produced a result (stuck Loading).
	for (const Int32_2& pos : regionManager->TakeFailedLoads()) {
		loadingChunks.erase(pos);
		auto it = chunks.find(pos);
		if (it == chunks.end())
			continue;
		if (it->second->state.load(std::memory_order_acquire) == ChunkState::Loading) {
			it->second->state.store(ChunkState::Unloaded, std::memory_order_release);
			TrackUnloadedChunk(pos);
		}
	}

	// Only in-flight loads need checking. This used to scan every chunk in the
	// world twice per tick even when nothing was loading.
	for (auto loadingIt = loadingChunks.begin(); loadingIt != loadingChunks.end();) {
		const Int32_2 pos = *loadingIt;
		auto chunkIt = chunks.find(pos);
		if (chunkIt == chunks.end() || !chunkIt->second ||
		    chunkIt->second->state.load(std::memory_order_acquire) != ChunkState::Loading) {
			loadingIt = loadingChunks.erase(loadingIt);
			continue;
		}

		auto loaded = regionManager->GetChunk(pos);
		if (!loaded) {
			++loadingIt;
			continue;
		}

		auto& chunk = chunkIt->second;
		bool wasSpawnChunk = chunk->spawnChunk;
		chunk = std::move(loaded);
		chunk->spawnChunk = wasSpawnChunk;

		bool needsLightingRefresh = chunk->refreshLighting;
		chunk->refreshLighting = false;

		// Replay any writes that arrived while this chunk was loading.
		auto pit = pendingBleedWrites.find(pos);
		if (pit != pendingBleedWrites.end()) {
			for (auto& [wpos, block] : pit->second)
				SetBlock(wpos, block.type, block.data);
			pendingBleedWrites.erase(pit);
		}

		// Register our tile entities
		RegisterChunkTileEntities(chunk.get());

		// Register our entities
		for (auto& entityTag : chunk->entityTags) {
			this->entityManager.CreateEntityFromNbt(entityTag);
		}
		chunk->entityTags.clear();
		chunk->entityTags.shrink_to_fit();

		// If we need a lighting update then do so
		if (needsLightingRefresh) {
			chunk->GenerateSkylightMap();
			SeedChunkLighting(chunk->cpos);
			chunk->isModified = true;
		}

		loadingIt = loadingChunks.erase(loadingIt);
	}
}

void WorldManager::SeedChunkLighting(Int32_2 _pos) {
	auto* chunk = GetChunkRaw(_pos);
	if (!chunk)
		return;

	// We check each column in the chunk's height against its neighbors, if they differ then we schedule light updates for the vertical column between them.
	// This works like 99% of the time but can miss some edge cases; its fast though!
	int bx = _pos.x * CHUNK_WIDTH;
	int bz = _pos.z * CHUNK_WIDTH;
	for (int x = 0; x < CHUNK_WIDTH; ++x) {
		for (int z = 0; z < CHUNK_WIDTH; ++z) {
			int wx = bx + x, wz = bz + z;
			int thisH = chunk->GetHeightValue({ x, z });
			const int ndx[] = { -1, 1, 0, 0 };
			const int ndz[] = { 0, 0, -1, 1 };
			for (int i = 0; i < 4; ++i) {
				int nx = wx + ndx[i], nz = wz + ndz[i];
				int neighborH = GetHeightValue(nx, nz);
				if (neighborH == thisH)
					continue;
				int minY = std::min<int>(thisH, neighborH);
				int maxY = std::max<int>(thisH, neighborH);
				lightManager.ScheduleLightRegion({ nx, minY, nz }, { nx, maxY, nz }, LightType::Sky);
			}
		}
	}

	// Determine block light emitters in one pass over the chunk. Scanning all
	// emitting types separately meant one full 32K-voxel walk per type, and
	// FindBlocks() returns chunk-local x/z, so emitters used to be scheduled
	// near the origin instead of at their world position.
	bool hasEmitter = false;
	for (int i = 1; i < BLOCK_MAX; ++i) {
		if (Blocks::lightEmissionTable[i] > 0 && chunk->HasBlock(BlockType(i))) {
			hasEmitter = true;
			break;
		}
	}
	if (hasEmitter) {
		for (int s = 0; s < SUB_CHUNK_COUNT; ++s) {
			const SubChunk& sub = chunk->subChunks[size_t(s)];
			if (sub.blocks) {
				for (int i = 0; i < SubChunk::VOLUME; ++i) {
					const BlockType block = sub.blocks->types[size_t(i)];
					if (Blocks::lightEmissionTable[block] <= 0)
						continue;
					const int lx = i & (SUB_CHUNK_SIZE - 1);
					const int ly = s * SUB_CHUNK_SIZE + (i >> 8);
					const int lz = (i >> 4) & (SUB_CHUNK_SIZE - 1);
					lightManager.ScheduleLightUpdate({ bx + lx, ly, bz + lz }, LightType::Block);
				}
			} else if (Blocks::lightEmissionTable[sub.blockFill] > 0) {
				for (int i = 0; i < SubChunk::VOLUME; ++i) {
					const int lx = i & (SUB_CHUNK_SIZE - 1);
					const int ly = s * SUB_CHUNK_SIZE + (i >> 8);
					const int lz = (i >> 4) & (SUB_CHUNK_SIZE - 1);
					lightManager.ScheduleLightUpdate({ bx + lx, ly, bz + lz }, LightType::Block);
				}
			}
		}
	}
	PropagateChunkLightBorders(_pos);
}

void WorldManager::RebuildUnloadedCandidates() {
	unloadedChunkIndex.clear();
	loadingChunks.clear();
	randomTickCacheValid = false;
	for (const auto& [pos, chunk] : chunks) {
		if (!chunk)
			continue;
		const auto state = chunk->state.load(std::memory_order_acquire);
		if (state == ChunkState::Unloaded)
			TrackUnloadedChunk(pos);
		else if (state == ChunkState::Loading)
			loadingChunks.insert(pos);
	}
}

void WorldManager::RebuildPopulateFrontier() {
	populateFrontier.clear();
	for (const auto& [pos, chunk] : chunks) {
		if (chunk && !chunk->isTerrainPopulated)
			populateFrontier.emplace(pos.x, pos.z);
	}
}

void WorldManager::UpdateLoadRadius(const std::vector<ClientPosition>& _players) {
	// A stationary player has the same chunk-interest set every tick. Rebuilding
	// it allocates and hashes the whole view square for no benefit.
	bool sameInterest = chunkInterestCacheValid && cachedPlayerChunks.size() == _players.size();
	if (sameInterest) {
		for (size_t i = 0; i < _players.size(); ++i) {
			const int viewDist =
			    _players[i].viewDistanceOverride ? _players[i].viewDistanceOverride : viewRadius;
			if (cachedPlayerChunks[i] != _players[i].GetChunkPos() || cachedViewDistances[i] != viewDist) {
				sameInterest = false;
				break;
			}
		}
	}
	if (sameInterest)
		return;

	cachedPlayerChunks.resize(_players.size());
	cachedViewDistances.resize(_players.size());
	for (size_t i = 0; i < _players.size(); ++i) {
		cachedPlayerChunks[i] = _players[i].GetChunkPos();
		cachedViewDistances[i] =
		    _players[i].viewDistanceOverride ? _players[i].viewDistanceOverride : viewRadius;
	}
	chunkInterestCacheValid = true;

	std::unordered_set<Int32_2> wanted;
	const size_t viewSide = size_t(viewRadius) * 2 + 1;
	const size_t viewArea = viewSide * viewSide;
	wanted.reserve(_players.size() * viewArea);
	for (const auto& player : _players) {
		Int2 center = player.GetChunkPos();
		int viewDist = (player.viewDistanceOverride) ? player.viewDistanceOverride : viewRadius;
		for (int dx = -viewDist; dx <= viewDist; dx++)
			for (int dz = -viewDist; dz <= viewDist; dz++)
				wanted.insert({ center.x + dx, center.z + dz });
	}

	// Get chunks we want
	for (const auto& pos : wanted) {
		if (!chunks.contains(pos)) {
			auto c = std::make_shared<Chunk>();
			c->cpos = pos;
			chunks.emplace(pos, std::move(c));
			TrackUnloadedChunk(pos);
			populateFrontier.emplace(pos.x, pos.z);
		}
	}

	// Remove chunks we don't want
	for (auto it = chunks.begin(); it != chunks.end();) {
		if (wanted.contains(it->first)) {
			++it;
			continue;
		}
		if (it->second->spawnChunk) {
			++it;
			continue;
		}

		ChunkState s = it->second->state.load();
		if (s < ChunkState::Generated) {
			if (regionManager)
				regionManager->DiscardChunk(it->first);
			entityManager.PruneEmptyContainer(it->first);
			InvalidateChunkCache();
			unloadedChunkIndex.erase({ it->first.x, it->first.z });
			populateFrontier.erase({ it->first.x, it->first.z });
			loadingChunks.erase(it->first);
			it = chunks.erase(it);
			continue;
		}

		// This chunk is actually leaving simulation so force unload entities
		if (it->second->isModified || this->entityManager.ChunkHasEntities(it->second->cpos)) {
			regionManager->SaveChunk(it->second, /*unloadingEntities=*/true);
			it->second->isModified = false;
		}

		entityManager.EraseContainer(it->first);
		InvalidateChunkCache();
		unloadedChunkIndex.erase({ it->first.x, it->first.z });
		populateFrontier.erase({ it->first.x, it->first.z });
		loadingChunks.erase(it->first);
		it = chunks.erase(it);
	}
}

void WorldManager::PumpPipeline(const std::vector<ClientPosition>& _players) {
	const int playerCount = int(_players.size());
	const int slicePerPlayer = 16;

	// The index is maintained sorted by (x, z), so this is a copy rather than
	// a full chunk-map scan plus sort every tick.
	std::vector<Int32_2> unloadedCandidates;
	unloadedCandidates.reserve(unloadedChunkIndex.size());
	for (const auto& [x, z] : unloadedChunkIndex)
		unloadedCandidates.push_back({ x, z });

	std::unordered_set<Int32_2> startedThisTick;

	auto startLoading = [&](const Int32_2& _pos) -> bool {
		if (startedThisTick.contains(_pos))
			return false;
		auto it = chunks.find(_pos);
		if (it == chunks.end()) {
			unloadedChunkIndex.erase({ _pos.x, _pos.z });
			return false;
		}
		if (it->second->state.load(std::memory_order_acquire) != ChunkState::Unloaded) {
			unloadedChunkIndex.erase({ _pos.x, _pos.z });
			return false;
		}
		it->second->state.store(ChunkState::Loading, std::memory_order_release);
		unloadedChunkIndex.erase({ _pos.x, _pos.z });
		loadingChunks.insert(_pos);
		regionManager->LoadChunk(_pos);
		startedThisTick.insert(_pos);
		return true;
	};

	auto startGeneration = [&](const Int32_2& _pos) -> bool {
		// Check if already started this Tick (can happen with multiple players), and if chunk is still Unloaded (can be changed by another thread).
		if (startedThisTick.contains(_pos))
			return false;
		auto it = chunks.find(_pos);
		if (it == chunks.end()) {
			unloadedChunkIndex.erase({ _pos.x, _pos.z });
			return false;
		}
		if (it->second->state.load(std::memory_order_acquire) != ChunkState::Unloaded) {
			unloadedChunkIndex.erase({ _pos.x, _pos.z });
			return false;
		}

		// Actually generate this chunk
		it->second->state.store(ChunkState::Generating, std::memory_order_release);
		unloadedChunkIndex.erase({ _pos.x, _pos.z });
		pool.detach_task([_pos, this]() {
			// We make a new chunk here instead of modifying the existing chunk because multithreading is a pain
			// The placeholder chunk in the map will be replaced by this one when we push to genDoneQueue
			auto chunk = std::make_shared<Chunk>();
			chunk->cpos = _pos;
			// The generators are cached per thread, so rebuild them if the world seed has changed since this
			// thread last generated a chunk (e.g. switching worlds, or the chunk-gen benchmark cycling seeds).
			const int64_t genSeed = this->seed;
			if (isHell) {
				thread_local std::unique_ptr<NetherGenerator> tlGen;
				thread_local int64_t tlGenSeed = 0;
				if (!tlGen || tlGenSeed != genSeed) {
					tlGen = std::make_unique<NetherGenerator>(genSeed);
					tlGenSeed = genSeed;
				}
				tlGen->GenerateChunk(*chunk);
			} else {
				thread_local std::unique_ptr<OverworldGenerator> tlGen;
				thread_local int64_t tlGenSeed = 0;
				if (!tlGen || tlGenSeed != genSeed) {
					tlGen = std::make_unique<OverworldGenerator>(genSeed);
					tlGenSeed = genSeed;
				}
				tlGen->GenerateChunk(*chunk);
			}
			chunk->isModified = true;
			chunk->GenerateSkylightMap();
			chunk->state.store(ChunkState::Generated, std::memory_order_release);

			// This just posts the result so we can start lighting and check for population
			this->PostGenResult(std::move(chunk));
		});
		startedThisTick.insert(_pos);
		return true;
	};

	if (playerCount == 0) {
		int started = 0;
		for (const Int32_2& pos : unloadedCandidates) {
			if (started >= slicePerPlayer)
				break;
			if (regionManager->ChunkExists(pos)) {
				if (startLoading(pos))
					++started;
				continue;
			}
			if (startGeneration(pos))
				++started;
		}
	} else {
		// Make sure everyone gets their share of the budget.
		// Every player's cursor walks the same shared,
		// pre-sorted candidate list.
		std::vector<size_t> cursors(size_t(playerCount), 0);
		int totalStarted = 0;
		const int totalBudget = slicePerPlayer * playerCount;
		bool anyProgress = true;
		while (totalStarted < totalBudget && anyProgress) {
			anyProgress = false;
			for (int i = 0; i < playerCount && totalStarted < totalBudget; ++i) {
				size_t& cur = cursors[size_t(i)];
				int playerConsumed = 0;
				while (playerConsumed < slicePerPlayer && cur < unloadedCandidates.size()) {
					Int32_2 cpos = unloadedCandidates[cur];
					++cur;
					if (regionManager->ChunkExists(cpos)) {
						if (startLoading(cpos)) {
							++playerConsumed;
							++totalStarted;
							anyProgress = true;
						}
						continue;
					}
					if (startGeneration(cpos)) {
						++playerConsumed;
						++totalStarted;
						anyProgress = true;
					}
				}
			}
		}
	}
}

void WorldManager::PopulateReady(int _maxPopulates) {
	// Reuse generators across chunks: PopulateChunk resets its RNG from the world seed,
	// so perm tables only need rebuilding when the world seed changes.
	thread_local std::unique_ptr<OverworldGenerator> tlOverworld;
	thread_local std::unique_ptr<NetherGenerator> tlNether;
	thread_local int64_t tlOverworldSeed = std::numeric_limits<int64_t>::min();
	thread_local int64_t tlNetherSeed = std::numeric_limits<int64_t>::min();
	if (!isHell && (!tlOverworld || tlOverworldSeed != this->seed)) {
		tlOverworld = std::make_unique<OverworldGenerator>(this->seed);
		tlOverworldSeed = this->seed;
	} else if (isHell && (!tlNether || tlNetherSeed != this->seed)) {
		tlNether = std::make_unique<NetherGenerator>(this->seed);
		tlNetherSeed = this->seed;
	}

	// Make sure we don't try to populate the same chunk multiple times in one Tick (can happen with the weird population order and multiple players)
	// Also make sure we populate in the right order!
	// We break if the target chunk isn't ready yet so population order is guaranteed
	std::unordered_set<Int32_2> populatedThisTick;
	int populatedCount = 0;
	auto frontierIt = populateFrontier.begin();
	while (frontierIt != populateFrontier.end()) {
		if (populatedCount >= _maxPopulates)
			break;

		const Int32_2 pos{ frontierIt->first, frontierIt->second };
		auto cit = chunks.find(pos);
		if (cit == chunks.end() || cit->second->isTerrainPopulated) {
			frontierIt = populateFrontier.erase(frontierIt);
			continue;
		}

		// Border chunks are not candidates until their positive neighbours exist.
		if (!chunks.contains({ pos.x + 1, pos.z }) || !chunks.contains({ pos.x, pos.z + 1 }) ||
		    !chunks.contains({ pos.x + 1, pos.z + 1 })) {
			++frontierIt;
			continue;
		}

		if (!CanPopulateDirect(pos))
			break;
		if (populatedThisTick.contains(pos)) {
			++frontierIt;
			continue;
		}

		cit->second->state.store(ChunkState::Populating, std::memory_order_release);
		WorldWrapper wrapper(*this, pos);
		wrapper.centerChunkPos = pos;
		wrapper.GetChunkRegion();
		if (isHell)
			tlNether->PopulateChunk(*cit->second, wrapper);
		else
			tlOverworld->PopulateChunk(*cit->second, wrapper);
		auto& chunk = cit->second;
		chunk->isTerrainPopulated = true;
		chunk->isModified = true;
		chunk->state.store(ChunkState::Populated, std::memory_order_release);
		populatedThisTick.insert(pos);
		++populatedCount;
		wrapper.FreeChunkRegion();
		FlushBleedWrites();
		frontierIt = populateFrontier.erase(frontierIt);
	}
}

void WorldManager::SetMeta(const Int3 _wpos, const uint8_t _metadata, const bool _updateNeighbors) {
	if (!InBounds(_wpos.y))
		return;
	Int32_2 cp{ _wpos.x >> 4, _wpos.z >> 4 };
	auto* chunk = GetChunkRaw(cp);
	if (!IsChunkValid(chunk))
		return;
	Int3 local{ _wpos.x & 15, _wpos.y, _wpos.z & 15 };
	auto oldMeta = chunk->GetMeta(local);
	auto blockId = chunk->GetBlock(local);
	chunk->SetMeta(local, _metadata);

	// Update our neighbors
	if (_updateNeighbors)
		this->NotifyNeighborsOfUpdate(_wpos, blockId);

	// Callback for the client and server to know about this block update
	if (onBlockUpdate && (oldMeta != _metadata && Blocks::blockProperties[blockId].notifySelfOnMetaChange))
		onBlockUpdate(PendingBlock{ .block{ chunk->GetBlock(local), _metadata },
		                            .blockPos{ _wpos.x, _wpos.y, _wpos.z },
		                            .light{ chunk->GetBlockLight(local), chunk->GetSkyLight(local) } },
		              chunk->cpos);
}

void WorldManager::SetBlockRaw(const Int3 _wpos, const BlockType _blockType, const uint8_t _metadata) {
	// Don't trigger any of the fancy stuff normal set block does, just replace this blocks id in the chunk array
	if (!InBounds(_wpos.y))
		return;
	Int32_2 cp{ _wpos.x >> 4, _wpos.z >> 4 };
	auto* chunk = GetChunkRaw(cp);
	if (!IsChunkValid(chunk)) {
		// Target chunk isn't valid
		return;
	}
	Int3 local{ _wpos.x & 15, _wpos.y, _wpos.z & 15 };

	chunk->SetBlockRaw(local, _blockType);
	chunk->SetMetaRaw(local, _metadata);
	chunk->isModified = true;
}

void WorldManager::SetBlock(const Int3 _wpos, const BlockType _blockType, const uint8_t _metadata,
                            const bool _keepTileEntity, const bool _updateNeighbors) {
	if (!InBounds(_wpos.y))
		return;
	Int32_2 cp{ _wpos.x >> 4, _wpos.z >> 4 };
	auto* chunk = GetChunkRaw(cp);
	if (!IsChunkValid(chunk)) {
		// Target chunk isn't ready; cache the write for replay (bounded).
		constexpr size_t MAX_BLEED_WRITES_PER_CHUNK = 1024;
		auto& queue = pendingBleedWrites[cp];
		if (queue.size() >= MAX_BLEED_WRITES_PER_CHUNK)
			queue.erase(queue.begin());
		queue.push_back({ _wpos, Block{ _blockType, _metadata } });
		return;
	}

	// Get the local coordinates of this block within the chunk, and check what block we're replacing
	const Int2 localXz{ _wpos.x & 15, _wpos.z & 15 };
	const Int3 local{ localXz.x, _wpos.y, localXz.z };
	const auto oldBlock = chunk->GetBlock(local);
	const auto oldMeta = chunk->GetMeta(local);

	// If nothing changed dont do any updates
	if (oldBlock == _blockType && oldMeta == _metadata)
		return;

	// Making the assumption here that certain metadatas of
	// blocks don't have differing light properties
	const bool changesLighting = (Blocks::lightOpacityTable[_blockType] !=
	                              Blocks::lightOpacityTable[oldBlock]) ||
	                             (Blocks::lightEmissionTable[_blockType] !=
	                              Blocks::lightEmissionTable[oldBlock]);

	// Unlight before changing the block
	if (changesLighting) {
		lightManager.UnlightAt(_wpos.x, _wpos.y, _wpos.z, LightType::Block, *this, chunk);
		lightManager.UnlightAt(_wpos.x, _wpos.y, _wpos.z, LightType::Sky, *this, chunk);
	}

	// Then finally set the new block
	chunk->SetBlockRaw(local, _blockType);

	if (oldBlock != BLOCK_AIR && !_keepTileEntity) {
		auto function = Blocks::blockBehaviors[oldBlock].onBlockRemoval;
		if (function)
			function(*this, _wpos);
	}

	chunk->SetMetaRaw(local, _metadata);
	chunk->isModified = true;

	const Int3 pos = _wpos;
	const int oldHeight = chunk->GetHeightValue(localXz);

	// Placing opaque block; heightmap may rise
	if (changesLighting) {
		chunk->RelightColumn(localXz);
		int newHeight = chunk->GetHeightValue(localXz);
		if (newHeight > oldHeight) {
			// Notify the BFS that all blocks from y down to oldHeight need updating
			for (int sy = oldHeight; sy <= newHeight; ++sy) {
				lightManager.UnlightAt(pos.x, sy, pos.z, LightType::Sky, *this, chunk);
			}
		} else if (newHeight < oldHeight) {
			// Height fell
			for (int sy = newHeight; sy < oldHeight; ++sy) {
				lightManager.ScheduleLightUpdate({ pos.x, sy, pos.z }, LightType::Sky);
			}
		}

		// Always re-evaluate the edited block and its 4 horizontal neighbours
		lightManager.ScheduleLightUpdate(pos, LightType::Sky);
		lightManager.ScheduleLightUpdate(pos, LightType::Block);
		int extendedBottom = std::min<int>(newHeight, oldHeight);
		while (extendedBottom > 0 &&
		       Blocks::lightOpacityTable[chunk->GetBlock({ localXz.x, extendedBottom - 1, localXz.z })] == 0)
			--extendedBottom;

		if (newHeight != oldHeight) {
			lightManager.ScheduleLightRegion({ pos.x - 1, extendedBottom, pos.z - 1 },
			                                 { pos.x + 1, std::max<int>(newHeight, oldHeight), pos.z + 1 },
			                                 LightType::Sky);
		}
	}

	// Remove any tile entities that exist at this spot
	if (!_keepTileEntity) {
		auto& tes = chunk->tileEntities;
		tes.erase(std::remove_if(tes.begin(), tes.end(),
		                         [&](const std::shared_ptr<TileEntity>& _te) { return _te && _te->position == _wpos; }),
		          tes.end());
	}

	// Call our on placed function
	if (_blockType != BLOCK_AIR) {
		// Java has this functionality in the chunk setters themselves, but
		// in my opinion that is stupid
		auto function = Blocks::blockBehaviors[_blockType].onBlockAdded;
		if (function)
			function(*this, _wpos);
	}

	// Update our neighbors
	if (_updateNeighbors)
		this->NotifyNeighborsOfUpdate(_wpos, _blockType);

	// Trigger redstone updates
	if ((RedstoneManager::CanTriggerRedstoneUpdate(_blockType) || RedstoneManager::CanTriggerRedstoneUpdate(oldBlock)) &&
	    _updateNeighbors) {
		if (frozen && !replayingDeferred)
			deferredUpdates.push_back({ DeferredUpdateType::Redstone, _wpos, _blockType, oldBlock });
		else
			RedstoneManager::TriggerRedstoneUpdate(*this, _wpos, _blockType, oldBlock);
	}

	// Callback for the client and server to know about this block update
	const auto newBlock = chunk->GetBlock(local);
	const auto newMeta = chunk->GetMeta(local);
	if (onBlockUpdate &&
	    (oldBlock != newBlock || (oldMeta != newMeta && Blocks::blockProperties[newBlock].notifySelfOnMetaChange)))
		onBlockUpdate(PendingBlock{ .block{ newBlock, newMeta },
		                            .blockPos{ _wpos.x, _wpos.y, _wpos.z },
		                            .light{ chunk->GetBlockLight(local), chunk->GetSkyLight(local) } },
		              chunk->cpos);
}

void WorldManager::NotifyRegionChanged(Chunk& _chunk, Int3 _localMin, Int3 _localMax) {
	if (!onBlockUpdate)
		return;

	std::vector<Int3> picked;
	auto add = [&](int _x, int _y, int _z) {
		for (const auto& p : picked)
			if (p.x == _x && p.y == _y && p.z == _z)
				return;
		picked.push_back({ _x, _y, _z });
	};
	for (int y : { _localMin.y, _localMax.y })
		for (int z : { _localMin.z, _localMax.z })
			for (int x : { _localMin.x, _localMax.x })
				add(x, y, z);
	for (int y = _localMin.y; y <= _localMax.y && picked.size() < 10; y++)
		for (int z = _localMin.z; z <= _localMax.z && picked.size() < 10; z++)
			for (int x = _localMin.x; x <= _localMax.x && picked.size() < 10; x++)
				add(x, y, z);

	for (const auto& p : picked) {
		const Int3 local{ p.x, p.y, p.z };
		onBlockUpdate(PendingBlock{ .block{ _chunk.GetBlock(local), _chunk.GetMeta(local) },
		                            .blockPos{ _chunk.cpos.x * CHUNK_WIDTH + p.x, p.y,
		                                       _chunk.cpos.z * CHUNK_WIDTH + p.z },
		                            .light{ _chunk.GetBlockLight(local), _chunk.GetSkyLight(local) } },
		              _chunk.cpos);
	}
}

void WorldManager::FillVolume(Int3 _posA, Int3 _posB, BlockType _type, uint8_t _meta) {
	const Int3 mn{ std::min<int>(_posA.x, _posB.x),
		           std::max<int>(std::min<int>(_posA.y, _posB.y), 0),
		           std::min<int>(_posA.z, _posB.z) };
	const Int3 mx{ std::max<int>(_posA.x, _posB.x),
		           std::min<int>(std::max<int>(_posA.y, _posB.y), CHUNK_HEIGHT - 1),
		           std::max<int>(_posA.z, _posB.z) };
	if (mn.y > mx.y)
		return;

	// Not worth the bulk path, and this keeps neighbor updates, block callbacks and lighting exact
	constexpr int64_t SMALL_FILL_VOLUME = 64;
	const int64_t volume = int64_t(mx.x - mn.x + 1) * (mx.y - mn.y + 1) * (mx.z - mn.z + 1);
	if (volume <= SMALL_FILL_VOLUME) {
		for (int y = mn.y; y <= mx.y; y++)
			for (int z = mn.z; z <= mx.z; z++)
				for (int x = mn.x; x <= mx.x; x++)
					SetBlock({ x, y, z }, _type, _meta);
		return;
	}

	const uint8_t emission = uint8_t(Blocks::lightEmissionTable[_type]);

	for (int cx = mn.x >> 4; cx <= (mx.x >> 4); cx++) {
		for (int cz = mn.z >> 4; cz <= (mx.z >> 4); cz++) {
			const Int32_2 cp{ cx, cz };
			Chunk* chunk = GetChunkRaw(cp);
			if (!IsChunkValid(chunk))
				continue;

			// This chunk's share of the box, chunk-local and inclusive
			const int baseX = cx * CHUNK_WIDTH, baseZ = cz * CHUNK_WIDTH;
			const int x0 = std::max<int>(mn.x, baseX) - baseX;
			const int x1 = std::min<int>(mx.x, baseX + CHUNK_WIDTH - 1) - baseX;
			const int z0 = std::max<int>(mn.z, baseZ) - baseZ;
			const int z1 = std::min<int>(mx.z, baseZ + CHUNK_WIDTH - 1) - baseZ;

			// Blocks
			if (x0 == 0 && x1 == CHUNK_WIDTH - 1 && z0 == 0 && z1 == CHUNK_WIDTH - 1)
				chunk->FillSlices(mn.y, mx.y, _type, _meta);
			else if (x0 == x1 && z0 == z1)
				chunk->FillColumn(mn.y, mx.y, { x0, z0 }, _type, _meta);
			else
				chunk->FillRegion({ x0, mn.y, z0 }, { x1, mx.y, z1 }, _type, _meta);

			// Tile entities in the box no longer have a block, same as SetBlock without _keepTileEntity
			auto& tes = chunk->tileEntities;
			tes.erase(std::remove_if(tes.begin(), tes.end(),
			                         [&](const std::shared_ptr<TileEntity>& _te) {
				                         return _te && _te->position.x >= mn.x && _te->position.x <= mx.x &&
				                                _te->position.y >= mn.y && _te->position.y <= mx.y &&
				                                _te->position.z >= mn.z && _te->position.z <= mx.z &&
				                                (_te->position.x >> 4) == cx && (_te->position.z >> 4) == cz;
			                         }),
			          tes.end());

			// Light. Whatever was in the box before is meaningless now, so set what the new block implies
			chunk->FillBlockLightRegion({ x0, mn.y, z0 }, { x1, mx.y, z1 }, emission);
			for (int x = x0; x <= x1; x++)
				for (int z = z0; z <= z1; z++)
					chunk->RecalculateSkyLightColumn({ x, z });
			chunk->Compact();
			chunk->isModified = true;

			// Let the light engine fix up whatever borders the fill. Faces between two chunks that are both
			// part of the fill are skipped, those are filled anyway.
			auto scheduleBoth = [&](Int3 _a, Int3 _b) {
				lightManager.ScheduleLightRegion(_a, _b, LightType::Sky);
				lightManager.ScheduleLightRegion(_a, _b, LightType::Block);
			};
			const int wx0 = baseX + x0, wx1 = baseX + x1, wz0 = baseZ + z0, wz1 = baseZ + z1;
			if (wx0 == mn.x)
				scheduleBoth({ wx0 - 1, mn.y, wz0 }, { wx0 - 1, mx.y, wz1 });
			if (wx1 == mx.x)
				scheduleBoth({ wx1 + 1, mn.y, wz0 }, { wx1 + 1, mx.y, wz1 });
			if (wz0 == mn.z)
				scheduleBoth({ wx0, mn.y, wz0 - 1 }, { wx1, mx.y, wz0 - 1 });
			if (wz1 == mx.z)
				scheduleBoth({ wx0, mn.y, wz1 + 1 }, { wx1, mx.y, wz1 + 1 });
			if (mx.y + 1 < CHUNK_HEIGHT)
				scheduleBoth({ wx0, mx.y + 1, wz0 }, { wx1, mx.y + 1, wz1 });
			if (mn.y > 0)
				scheduleBoth({ wx0, mn.y - 1, wz0 }, { wx1, mn.y - 1, wz1 });

			// Sky light can change anywhere in the column, so resend the full height
			NotifyRegionChanged(*chunk, { x0, 0, z0 }, { x1, CHUNK_HEIGHT - 1, z1 });
		}
	}
}

int WorldManager::FindTopSolidBlock(int _wx, int _wz) {
	auto* chunk = GetChunkRaw({ _wx >> 4, _wz >> 4 });
	if (!chunk || chunk->state.load() < ChunkState::Generated)
		return -1;
	int lx = _wx & 15, lz = _wz & 15;
	for (int y = CHUNK_HEIGHT - 1; y > 0; --y) {
		BlockType block = chunk->GetBlock({ lx, y, lz });
		if (block == BlockType::BLOCK_AIR)
			continue;
		Material mat = Blocks::blockProperties[block].material;
		if (mat.isSolid || mat.isLiquid)
			return y + 1;
	}
	return -1;
}

BlockType WorldManager::GetFirstUncoveredBlock(int _wx, int _wz) {
	auto* chunk = GetChunkRaw({ _wx >> 4, _wz >> 4 });
	if (!chunk || chunk->state.load() < ChunkState::Generated)
		return BlockType(-1);
	int lx = _wx & 15, lz = _wz & 15;
	int y = 63;
	while (y < 127 && chunk->GetBlock({ lx, y + 1, lz }) != BlockType::BLOCK_AIR) {
		++y;
	}
	return chunk->GetBlock({ lx, y, lz });
}

void WorldManager::InitSpawn() {
	int sx = 0;
	int sz = 0;
	auto canCoordinateBeSpawn = [&](int _x, int _z) -> bool {
		auto b = GetFirstUncoveredBlock(_x, _z);
		if (b == BlockType::BLOCK_INVALID) {
			// Force generate this chunk so we can check the block type.
			auto pos = Int32_2{ _x >> 4, _z >> 4 };
			auto chunk = std::make_shared<Chunk>();
			chunk->cpos = pos;
			chunks[pos] = std::move(chunk);
			TrackUnloadedChunk(pos);
			populateFrontier.emplace(pos.x, pos.z);
			while (GetFirstUncoveredBlock(_x, _z) == BlockType::BLOCK_INVALID) {
				this->PumpPipeline({});
				this->DrainGenQueue();
				this->DrainLoadQueue();
				this->pool.wait();
			}
			b = GetFirstUncoveredBlock(_x, _z);
		}
		return GetFirstUncoveredBlock(_x, _z) == BlockType::BLOCK_SAND;
	};
	int tries = 0;
	for (; !canCoordinateBeSpawn(sx, sz); sz += this->rand.NextInt(64) - this->rand.NextInt(64)) {
		sx += this->rand.NextInt(64) - this->rand.NextInt(64);
		if (tries > 1000) {
			sx = 0;
			sz = 0;
			break;
		}
		tries++;
	}
	this->spawnPoint = { sx, 64, sz };
	chunks.clear(); // Clear all chunks so we can start fresh from the spawn area
	unloadedChunkIndex.clear();
	populateFrontier.clear();
	loadingChunks.clear();
	randomTickCacheValid = false;
	InvalidateChunkInterest();
	InvalidateChunkCache();
}

void WorldManager::PropagateChunkLightBorders(Int32_2 _cpos) {
	Chunk* chunk = GetChunkRaw(_cpos);
	if (!chunk)
		return;

	// Iterate through our chunk borders
	const Cardinal::Value dirs[4] = { Cardinal::Value::West, Cardinal::Value::East, Cardinal::Value::North,
		                               Cardinal::Value::South };
	const Int32_2 bpos = _cpos * CHUNK_WIDTH;
	for (auto dir : dirs) {
		Chunk* neighborChunk = GetChunkRaw(_cpos.WithOffset(dir));
		if (!neighborChunk)
			continue;

		// Walk the border edge of this chunk that faces the neighbor
		for (int t = 0; t < CHUNK_WIDTH; ++t) {
			// Pick the border column of this chunk facing direction i
			int lx, lz, nx, nz;
			switch (dir) {
			case Cardinal::Value::West:
				lx = 0;
				lz = t;
				nx = CHUNK_WIDTH - 1;
				nz = t;
				break;
			case Cardinal::Value::East:
				lx = CHUNK_WIDTH - 1;
				lz = t;
				nx = 0;
				nz = t;
				break;
			case Cardinal::Value::North:
				lx = t;
				lz = 0;
				nx = t;
				nz = CHUNK_WIDTH - 1;
				break;
			case Cardinal::Value::South:
			default:
				lx = t;
				lz = CHUNK_WIDTH - 1;
				nx = t;
				nz = 0;
				break;
			}

			for (int y = 0; y < CHUNK_HEIGHT; ++y) {
				const Int3 ownPos = { lx, y, lz };
				const Int3 neighborPos = { nx, y, nz };

				// Only the neighbor being brighter can change our cell. Most
				// border cells are already equal or brighter, so skipping
				// them avoids thousands of no-op propagation steps per chunk.
				if (neighborChunk->GetBlockLight(neighborPos) > chunk->GetBlockLight(ownPos))
					lightManager.ScheduleLightUpdate({ bpos.x + lx, y, bpos.z + lz }, LightType::Block);
				if (neighborChunk->GetSkyLight(neighborPos) > chunk->GetSkyLight(ownPos))
					lightManager.ScheduleLightUpdate({ bpos.x + lx, y, bpos.z + lz }, LightType::Sky);
			}
		}
	}
}

void WorldManager::FlushBleedWrites() {
	for (auto it = pendingBleedWrites.begin(); it != pendingBleedWrites.end();) {
		auto* target = GetChunkRaw(it->first);
		if (target && target->state.load() >= ChunkState::Generated && !target->inUse.load()) {
			for (auto& [wpos, block] : it->second)
				SetBlock(wpos, block.type, block.data);
			it = pendingBleedWrites.erase(it);
		} else {
			++it;
		}
	}
}

void WorldManager::SetViewRadius(int _viewRadius) {
	int newViewRadius = std::max(3, _viewRadius);
	viewRadius = newViewRadius;
	simulationRadius = std::min(9, newViewRadius);
}

void WorldManager::NotifyNeighborsOfUpdate(Int3 _globalPos, BlockType _blockId) {
	if (frozen && !replayingDeferred) {
		deferredUpdates.push_back({ DeferredUpdateType::Neighbors, _globalPos, _blockId, BLOCK_AIR });
		return;
	}
	// Update our six neighbors.
	const Cardinal::Value dirs[6] = { Cardinal::Value::West, Cardinal::Value::East,  Cardinal::Value::Down,
		                               Cardinal::Value::Up,   Cardinal::Value::North, Cardinal::Value::South };

	// Most neighbors are in the same chunk. Resolve that chunk once instead of
	// doing six separate chunk-map lookups through GetBlockId().
	const Int32_2 centerPos{ _globalPos.x >> 4, _globalPos.z >> 4 };
	Chunk* centerChunk = GetChunkRaw(centerPos);
	if (centerChunk && centerChunk->state.load() < ChunkState::Generated)
		centerChunk = nullptr;

	auto getBlock = [&](Int3 _pos) -> BlockType {
		if (centerChunk && (_pos.x >> 4) == centerPos.x && (_pos.z >> 4) == centerPos.z)
			return centerChunk->GetBlock({ _pos.x & 15, _pos.y, _pos.z & 15 });
		return GetBlockId(_pos);
	};

	// Notify neighbors
	for (auto dir : dirs) {
		Int3 newPos = _globalPos.WithOffset(dir);
		auto block = getBlock(newPos);
		auto updateFunction = Blocks::onNeighborTable[block];
		if (updateFunction)
			updateFunction(*this, newPos, _blockId);
	}
}

void WorldManager::CreateTileEntity(std::shared_ptr<TileEntity> _tileEntity) {
	Int32_2 cpos{ _tileEntity->position.x >> 4, _tileEntity->position.z >> 4 };
	Chunk* chunk = GetChunkRaw(cpos);
	if (!chunk)
		return;
	_tileEntity->chunk = chunk;
	tileEntityManager.InitializeTileEntity(_tileEntity);   // weak_ptr added if canTick
	chunk->tileEntities.push_back(std::move(_tileEntity)); // chunk takes ownership
}

void WorldManager::RegisterChunkTileEntities(Chunk* _chunk) {
	for (auto& te : _chunk->tileEntities) {
		if (te) {
			tileEntityManager.InitializeTileEntity(te);
			te->chunk = _chunk;
		}
	}
}

TileEntity* WorldManager::GetTileEntity(Int3 _pos) {
	Chunk* chunk = GetChunkRaw({ _pos.x >> 4, _pos.z >> 4 });
	if (!chunk)
		return nullptr;
	for (auto& te : chunk->tileEntities) {
		if (te && te->position.x == _pos.x && te->position.y == _pos.y && te->position.z == _pos.z)
			return te.get();
	}
	return nullptr;
}

void WorldManager::RemoveTileEntity(Int3 _pos) {
	Chunk* chunk = GetChunkRaw({ _pos.x >> 4, _pos.z >> 4 });
	if (!chunk)
		return;
	auto& tes = chunk->tileEntities;
	tes.erase(std::remove_if(tes.begin(), tes.end(),
	                         [&](const std::shared_ptr<TileEntity>& _te) {
		                         return _te && _te->position.x == _pos.x && _te->position.y == _pos.y &&
		                                _te->position.z == _pos.z;
	                         }),
	          tes.end());
}

BlockType WorldManager::GetBlockId(Int3 _wpos) {
	if (!InBounds(_wpos.y))
		return BlockType::BLOCK_AIR;
	auto* chunk = GetChunkRaw({ _wpos.x >> 4, _wpos.z >> 4 });
	if (!chunk || chunk->state.load() < ChunkState::Generated)
		return BlockType::BLOCK_AIR;
	return chunk->GetBlock({ _wpos.x & 15, _wpos.y, _wpos.z & 15 });
}

uint8_t WorldManager::GetMetadata(Int3 _wpos) {
	if (!InBounds(_wpos.y))
		return 0;
	auto* chunk = GetChunkRaw({ _wpos.x >> 4, _wpos.z >> 4 });
	if (!chunk || chunk->state.load() < ChunkState::Generated)
		return 0;
	return chunk->GetMeta({ _wpos.x & 15, _wpos.y, _wpos.z & 15 });
}