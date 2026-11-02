/*
 * Copyright (c) 2026, Aidan <JcbbcEnjoyer>
 * Copyright (c) 2026, Anya Rihtarshich <vesui@proton.me>
 *
 * SPDX-License-Identifier: AGPL-3.0-only
 *
*/
#include "entity_manager.h"
#include "entity_arrow.h"
#include "entity_boat.h"
#include "entity_chicken.h"
#include "entity_cow.h"
#include "entity_creeper.h"
#include "entity_egg.h"
#include "entity_falling_block.h"
#include "entity_item.h"
#include "entity_minecart.h"
#include "entity_painting.h"
#include "entity_pig.h"
#include "entity_sheep.h"
#include "entity_skeleton.h"
#include "entity_slime.h"
#include "entity_snowball.h"
#include "entity_spider.h"
#include "entity_tnt.h"
#include "entity_zombie.h"
#include "entity_squid.h"
#include "entity_wolf.h"
#include "entity_zombie_pig.h"
#include "entity_player.h"
#include "entity_ghast.h"
#include "gamerules.h"
#include "world.h"
#include "username.h"
#include <algorithm>
#include <unordered_set>

namespace {

void AddToBucket(Entity* _entity, EntityBucket& _bucket) {
	_entity->bucketEntityIndex = static_cast<int>(_bucket.entities.size());
	_bucket.entities.push_back(_entity);
	if (_entity->actsAsWorldCollider) {
		_entity->bucketColliderIndex = static_cast<int>(_bucket.worldCollidableEntities.size());
		_bucket.worldCollidableEntities.push_back(_entity);
	} else {
		_entity->bucketColliderIndex = -1;
	}
}

void RemoveFromBucket(Entity* _entity, EntityBucket& _bucket) {
	const size_t index = size_t(std::max(_entity->bucketEntityIndex, 0));
	if (_entity->bucketEntityIndex >= 0 && index < _bucket.entities.size() &&
	    _bucket.entities[index] == _entity) {
		Entity* moved = _bucket.entities.back();
		_bucket.entities[index] = moved;
		_bucket.entities.pop_back();
		moved->bucketEntityIndex = static_cast<int>(index);
	} else {
		auto it = std::find(_bucket.entities.begin(), _bucket.entities.end(), _entity);
		if (it != _bucket.entities.end()) {
			const size_t pos = size_t(it - _bucket.entities.begin());
			Entity* moved = _bucket.entities.back();
			*it = moved;
			_bucket.entities.pop_back();
			moved->bucketEntityIndex = static_cast<int>(pos);
		}
	}
	_entity->bucketEntityIndex = -1;

	if (_entity->actsAsWorldCollider) {
		const size_t colliderIndex = size_t(std::max(_entity->bucketColliderIndex, 0));
		if (_entity->bucketColliderIndex >= 0 && colliderIndex < _bucket.worldCollidableEntities.size() &&
		    _bucket.worldCollidableEntities[colliderIndex] == _entity) {
			Entity* moved = _bucket.worldCollidableEntities.back();
			_bucket.worldCollidableEntities[colliderIndex] = moved;
			_bucket.worldCollidableEntities.pop_back();
			moved->bucketColliderIndex = static_cast<int>(colliderIndex);
		} else {
			auto it = std::find(_bucket.worldCollidableEntities.begin(), _bucket.worldCollidableEntities.end(),
			                    _entity);
			if (it != _bucket.worldCollidableEntities.end()) {
				const size_t pos = size_t(it - _bucket.worldCollidableEntities.begin());
				Entity* moved = _bucket.worldCollidableEntities.back();
				*it = moved;
				_bucket.worldCollidableEntities.pop_back();
				moved->bucketColliderIndex = static_cast<int>(pos);
			}
		}
	}
	_entity->bucketColliderIndex = -1;
}

template <typename Filter>
void CollectEntitiesInAabb(std::unordered_map<Int2, EntityContainer>& _containers, const AABB& _box,
                           bool _collidables, std::vector<Entity*>& _out, Filter&& _filter) {
	_out.clear();
	const int blockMinX = MathHelper::FloorDouble((_box.minX - 2.0) / 16.0);
	const int blockMinZ = MathHelper::FloorDouble((_box.minZ - 2.0) / 16.0);
	const int blockMaxX = MathHelper::FloorDouble((_box.maxX + 2.0) / 16.0);
	const int blockMaxZ = MathHelper::FloorDouble((_box.maxZ + 2.0) / 16.0);
	int bucketMinY = MathHelper::FloorDouble((_box.minY - 2.0) / 16.0);
	int bucketMaxY = MathHelper::FloorDouble((_box.maxY + 2.0) / 16.0);
	bucketMinY = std::clamp(bucketMinY, 0, 9);
	bucketMaxY = std::clamp(bucketMaxY, 0, 9);

	auto process = [&](EntityContainer& container) {
		for (int by = bucketMinY; by <= bucketMaxY; ++by) {
			auto& list = _collidables ? container.buckets[by].worldCollidableEntities
			                          : container.buckets[by].entities;
			for (Entity* entity : list) {
				if (_filter(*entity) && entity->collider.Intersects(_box))
					_out.push_back(entity);
			}
		}
	};

	// For a large query over a sparse map, walking occupied containers is
	// cheaper than hashing every empty chunk coordinate in the AABB.
	const int64_t cells = (int64_t(blockMaxX) - blockMinX + 1) * (int64_t(blockMaxZ) - blockMinZ + 1);
	if (cells > int64_t(_containers.size())) {
		for (auto& [cpos, container] : _containers) {
			if (cpos.x < blockMinX || cpos.x > blockMaxX || cpos.y < blockMinZ || cpos.y > blockMaxZ)
				continue;
			process(container);
		}
		return;
	}

	for (int x = blockMinX; x <= blockMaxX; ++x) {
		for (int z = blockMinZ; z <= blockMaxZ; ++z) {
			auto it = _containers.find({ x, z });
			if (it != _containers.end())
				process(it->second);
		}
	}
}

template <typename Filter>
bool AnyEntityInAabb(std::unordered_map<Int2, EntityContainer>& _containers, const AABB& _box,
                     bool _collidables, Filter&& _filter) {
	const int blockMinX = MathHelper::FloorDouble((_box.minX - 2.0) / 16.0);
	const int blockMinZ = MathHelper::FloorDouble((_box.minZ - 2.0) / 16.0);
	const int blockMaxX = MathHelper::FloorDouble((_box.maxX + 2.0) / 16.0);
	const int blockMaxZ = MathHelper::FloorDouble((_box.maxZ + 2.0) / 16.0);
	int bucketMinY = MathHelper::FloorDouble((_box.minY - 2.0) / 16.0);
	int bucketMaxY = MathHelper::FloorDouble((_box.maxY + 2.0) / 16.0);
	bucketMinY = std::clamp(bucketMinY, 0, 9);
	bucketMaxY = std::clamp(bucketMaxY, 0, 9);

	auto process = [&](EntityContainer& container) -> bool {
		for (int by = bucketMinY; by <= bucketMaxY; ++by) {
			auto& list = _collidables ? container.buckets[by].worldCollidableEntities
			                          : container.buckets[by].entities;
			for (Entity* entity : list) {
				if (_filter(*entity) && entity->collider.Intersects(_box))
					return true;
			}
		}
		return false;
	};

	const int64_t cells = (int64_t(blockMaxX) - blockMinX + 1) * (int64_t(blockMaxZ) - blockMinZ + 1);
	if (cells > int64_t(_containers.size())) {
		for (auto& [cpos, container] : _containers) {
			if (cpos.x < blockMinX || cpos.x > blockMaxX || cpos.y < blockMinZ || cpos.y > blockMaxZ)
				continue;
			if (process(container))
				return true;
		}
		return false;
	}

	for (int x = blockMinX; x <= blockMaxX; ++x) {
		for (int z = blockMinZ; z <= blockMaxZ; ++z) {
			auto it = _containers.find({ x, z });
			if (it != _containers.end() && process(it->second))
				return true;
		}
	}
	return false;
}
} // namespace

std::shared_ptr<PlayerEntity> EntityManager::GetPlayerWithName(std::string _name) {
	for (const auto& playerWeak : players) {
		if (auto playerEntity = playerWeak.lock()) {
			// The players list only ever contains PLAYER entities.
			auto player = std::static_pointer_cast<PlayerEntity>(playerEntity);
			if (UsernameEquals(player->username, _name)) {
				return player;
			}
		}
	}
	return nullptr;
}

void EntityManager::AlertWolves(PlayerEntity& _player, Entity& _target, bool _ignoreSitting) {
	if (_target.type == EntityType::CREEPER || _target.type == EntityType::GHAST)
		return;

	auto wolf = dynamic_cast<WolfEntity*>(&_target);
	if (wolf) {
		if (wolf->IsTamed() && UsernameEquals(_player.username, wolf->ownerUsername))
			return;
	}

	auto searchCollider = AABB{ _player.position.x,       _player.position.y,       _player.position.z,
		                        _player.position.x + 1.0, _player.position.y + 1.0, _player.position.z + 1.0 };
	searchCollider = searchCollider.Expand(16.0, 4.0, 16.0);

	EntityType wolfType = EntityType::WOLF;
	auto candidates = GetEntitiesWithinAabbOfType(searchCollider, wolfType);

	for (auto& candidate : candidates) {
		auto wolfCandidate = dynamic_cast<WolfEntity*>(candidate);
		if (!wolfCandidate)
			continue;
		if (!wolfCandidate->IsTamed())
			continue;
		if (wolfCandidate->attackTarget.lock())
			continue;
		if (!UsernameEquals(_player.username, wolfCandidate->ownerUsername))
			continue;
		if (_ignoreSitting && wolfCandidate->isSitting)
			continue;
		wolfCandidate->SetWolfSitting(false);
		wolfCandidate->attackTarget = GetEntityByIdShared(_target.id);
	}
}

void EntityManager::RemoveEntity(EntityId _id) {
	// Find the entity for this ID
	auto it = std::find_if(entities.begin(), entities.end(),
	                       [_id](const std::shared_ptr<Entity>& _e) { return _e->id == _id; });
	if (it == entities.end()) {
		return; // Not found, nothing to do
	}

	std::shared_ptr<Entity> entity = *it;
	entities.erase(it);
	DetachEntity(entity);
}

void EntityManager::DetachEntity(const std::shared_ptr<Entity>& entity) {
	// Remove player
	if (entity->type == EntityType::PLAYER)
		std::erase_if(players, [&](const std::weak_ptr<Entity>& _w) {
			auto p = _w.lock();
			return !p || p == entity;
		});

	// Remove from its bucket
	Int2 cpos{ entity->bucketPos.x, entity->bucketPos.y };
	auto containerIt = entityContainers.find(cpos);
	if (containerIt != entityContainers.end()) {
		auto& bucket = containerIt->second.buckets[entity->bucketPos.z];
		RemoveFromBucket(entity.get(), bucket);
		PruneEmptyContainer(cpos);
	} else {
		entity->bucketEntityIndex = -1;
		entity->bucketColliderIndex = -1;
	}

	// Unbind ourselves
	entity->entityManager = nullptr;
	auto indexIt = entitiesById.find(entity->id);
	if (indexIt != entitiesById.end() && indexIt->second == entity)
		entitiesById.erase(indexIt);

	// Mark the chunk we died in as dirty
	if (auto chunk = world->GetChunkRaw({ entity->bucketPos.x, entity->bucketPos.y }))
		chunk->MarkModified();

	// Set as dead for cleanup
	entity->isDead = true;
	if (onEntityDespawn)
		onEntityDespawn(entity);
}

void EntityManager::AddEntity(std::shared_ptr<Entity> _entity, EntityId _forceEntityId) {
	if (!world) {
		Logger::Error << "Attempted to add an entity before EntityManager was bound to a world!\n";
		return;
	}
	// TODO: Run this on gamerule change too?
	if (IsAnimal(_entity->type) && !Gamerule::Get<bool>(Gamerule::Id::SpawnMobs))
		return;
	if (IsMonster(_entity->type) && !Gamerule::Get<bool>(Gamerule::Id::SpawnMonsters))
		return;
	_entity->id = _forceEntityId == -1 ? GetNextEntityId()
	                                   : _forceEntityId; // Assign an ID if we weren't forced to use one
	_entity->world = world; // Bind the world pointer so the entity can interact with the world
	_entity->entityManager = this;
	_entity->dim = world->GetDimension();

	// Register the entity into its initial bucket
	_entity->bucketPos = ComputeBucketPos(_entity->position);
	auto& container = entityContainers[{ _entity->bucketPos.x, _entity->bucketPos.y }];
	AddToBucket(_entity.get(), container.buckets[_entity->bucketPos.z]);

	// If we are a player register for easy lookup
	if (_entity->type == EntityType::PLAYER) {
		std::weak_ptr<Entity> weakEntity = _entity;
		players.push_back(weakEntity);
	}

	entities.push_back(std::move(_entity));
	entitiesById.try_emplace(entities.back()->id, entities.back());
	if (onEntitySpawn)
		onEntitySpawn(entities.back());
}

void EntityManager::TickEntityAndPassenger(const std::shared_ptr<Entity>& _entity, TickProfiler* _profiler) {
	if (_profiler) {
		auto start = std::chrono::steady_clock::now();
		_entity->Tick();
		_profiler->RecordEntity(_entity->dim, _entity->id, _entity->type, std::chrono::steady_clock::now() - start);
	} else {
		_entity->Tick();
	}

	// Check to see if this entity went into another container or bucket
	Int3 newBucketPos = ComputeBucketPos(_entity->position);
	Entity* raw = _entity.get();

	if (newBucketPos != _entity->bucketPos) {
		Int2 oldCpos{ _entity->bucketPos.x, _entity->bucketPos.y };
		// Remove from the old bucket and mark the old chunk as modified
		if (auto chunk = world->GetChunkRaw(oldCpos))
			chunk->MarkModified();
		auto& oldContainer = entityContainers[oldCpos];
		auto& b = oldContainer.buckets[_entity->bucketPos.z];
		RemoveFromBucket(raw, b);
		PruneEmptyContainer(oldCpos);

		// Put in the new bucket
		auto& newContainer = entityContainers[{ newBucketPos.x, newBucketPos.y }];
		auto& newB = newContainer.buckets[newBucketPos.z];
		AddToBucket(raw, newB);
		_entity->bucketPos = newBucketPos;
	}

	// If something is riding us, tick it
	if (auto lockPassenger = _entity->passenger.lock()) {
		auto passengersVehicle = lockPassenger->vehicle.lock();
		if (!lockPassenger->isDead && passengersVehicle.get() == raw) {
			TickEntityAndPassenger(lockPassenger, _profiler);
		} else {
			_entity->passenger.reset();
		}
	}
}

void EntityManager::Tick(TickProfiler* _profiler) {
	// Snapshot weak refs so mid-tick spawn/despawn cannot invalidate iteration.
	// The snapshot and dead-list scratch are reused across ticks.
	auto& toTick = tickSnapshot;
	toTick.clear();
	if (toTick.capacity() < entities.size())
		toTick.reserve(entities.size());
	for (const auto& entity : entities)
		toTick.emplace_back(entity);

	for (const auto& weak : toTick) {
		std::shared_ptr<Entity> entity = weak.lock();
		if (!entity || entity->isDead)
			continue;

		// Skip entities that are riding something
		if (auto lockVehicle = entity->vehicle.lock()) {
			if (!lockVehicle->isDead && lockVehicle->passenger.lock().get() == entity.get())
				continue;
			entity->vehicle.reset();
		}

		TickEntityAndPassenger(entity, _profiler);
	}

	// Remove dead entities after ticking. Detach them all first, then erase
	// them from the master list in one pass.
	auto& dead = tickDead;
	auto& deadSet = tickDeadSet;
	dead.clear();
	deadSet.clear();
	for (const auto& entity : entities) {
		if (entity->isDead) {
			dead.push_back(entity);
			deadSet.insert(entity.get());
		}
	}
	for (const auto& entity : dead)
		DetachEntity(entity);
	// Only erase the snapshot we detached. Entities killed by a despawn
	// callback stay in the world for a later tick so they can be detached
	// normally instead of leaving stale bucket/index entries.
	if (!deadSet.empty()) {
		std::erase_if(entities, [&](const std::shared_ptr<Entity>& _entity) {
			return deadSet.contains(_entity.get());
		});
	}

	// Clear players that have expired
	for (auto it = players.begin(); it != players.end();) {
		if (it->expired()) {
			it = players.erase(it);
		} else {
			++it;
		}
	}
}

std::vector<Entity*> EntityManager::GetEntitiesWithinAabbExcluding(const AABB& _box, const EntityId _entityId) {
	std::vector<Entity*> out;
	GetEntitiesWithinAabbExcluding(_box, _entityId, out);
	return out;
}

void EntityManager::GetEntitiesWithinAabbExcluding(const AABB& _box, EntityId _entityId,
                                                   std::vector<Entity*>& _out) {
	CollectEntitiesInAabb(entityContainers, _box, false, _out,
	                      [&](const Entity& _entity) { return _entity.id != _entityId; });
}

std::vector<Entity*> EntityManager::GetEntitiesWithinAabbExcludingTypes(
    const AABB& _box, const std::vector<EntityType>& _excludedTypes) {
	std::vector<Entity*> out;
	GetEntitiesWithinAabbExcludingTypes(_box, _excludedTypes, out);
	return out;
}

void EntityManager::GetEntitiesWithinAabbExcludingTypes(const AABB& _box,
                                                        const std::vector<EntityType>& _excludedTypes,
                                                        std::vector<Entity*>& _out) {
	CollectEntitiesInAabb(entityContainers, _box, false, _out, [&](const Entity& _entity) {
		return std::find(_excludedTypes.begin(), _excludedTypes.end(), _entity.type) == _excludedTypes.end();
	});
}

std::vector<Entity*> EntityManager::GetCollidablesWithinAabb(const AABB& _box, EntityId _excludedId) {
	std::vector<Entity*> out;
	GetCollidablesWithinAabb(_box, _excludedId, out);
	return out;
}

void EntityManager::GetCollidablesWithinAabb(const AABB& _box, EntityId _excludedId,
                                             std::vector<Entity*>& _out) {
	CollectEntitiesInAabb(entityContainers, _box, true, _out,
	                      [&](const Entity& _entity) { return _entity.id != _excludedId; });
}

std::vector<Entity*> EntityManager::GetEntitiesWithinAabb(const AABB& _box) {
	std::vector<Entity*> out;
	GetEntitiesWithinAabb(_box, out);
	return out;
}

void EntityManager::GetEntitiesWithinAabb(const AABB& _box, std::vector<Entity*>& _out) {
	CollectEntitiesInAabb(entityContainers, _box, false, _out, [](const Entity&) { return true; });
}

bool EntityManager::AnyEntityWithinAabb(const AABB& _box) {
	return AnyEntityInAabb(entityContainers, _box, false, [](const Entity&) { return true; });
}

std::vector<Entity*> EntityManager::GetEntitiesWithinAabbOfType(const AABB& _box, EntityType& _type) {
	std::vector<Entity*> out;
	GetEntitiesWithinAabbOfType(_box, _type, out);
	return out;
}

void EntityManager::GetEntitiesWithinAabbOfType(const AABB& _box, EntityType& _type,
                                                std::vector<Entity*>& _out) {
	CollectEntitiesInAabb(entityContainers, _box, false, _out,
	                      [&](const Entity& _entity) { return _entity.type == _type; });
}

std::vector<Entity*> EntityManager::GetLivingEntitiesWithinAabb(const AABB& _box) {
	std::vector<Entity*> out;
	GetLivingEntitiesWithinAabb(_box, out);
	return out;
}

void EntityManager::GetLivingEntitiesWithinAabb(const AABB& _box, std::vector<Entity*>& _out) {
	CollectEntitiesInAabb(entityContainers, _box, false, _out,
	                      [](const Entity& _entity) { return dynamic_cast<const MobileEntity*>(&_entity) != nullptr; });
}

void EntityManager::CreateEntityFromNbt(Tag& _nbt) {
	// Load an entity from the nbt list
	std::string id = _nbt.compound["id"].GetString();
	if (id == "PigZombie") {
		ZombiePigEntity entity;
		entity.LoadFromNbt(_nbt);
		AddEntity(std::make_shared<ZombiePigEntity>(entity));
	}
	if (id == "Painting") {
		PaintingEntity entity;
		entity.LoadFromNbt(_nbt);
		AddEntity(std::make_shared<PaintingEntity>(entity));
	}
	if (id == "Minecart") {
		MinecartEntity entity;
		entity.LoadFromNbt(_nbt);
		AddEntity(std::make_shared<MinecartEntity>(entity));
	}
	if (id == "Boat") {
		BoatEntity entity;
		entity.LoadFromNbt(_nbt);
		AddEntity(std::make_shared<BoatEntity>(entity));
	}
	if (id == "Item") {
		ItemEntity entity({});
		entity.LoadFromNbt(_nbt);
		AddEntity(std::make_shared<ItemEntity>(entity));
	}
	if (id == "Zombie") {
		ZombieEntity entity;
		entity.LoadFromNbt(_nbt);
		AddEntity(std::make_shared<ZombieEntity>(entity));
	}
	if (id == "Cow") {
		CowEntity entity;
		entity.LoadFromNbt(_nbt);
		AddEntity(std::make_shared<CowEntity>(entity));
	}
	if (id == "Pig") {
		PigEntity entity;
		entity.LoadFromNbt(_nbt);
		AddEntity(std::make_shared<PigEntity>(entity));
	}
	if (id == "Sheep") {
		SheepEntity entity;
		entity.LoadFromNbt(_nbt);
		AddEntity(std::make_shared<SheepEntity>(entity));
	}
	if (id == "Chicken") {
		ChickenEntity entity;
		entity.LoadFromNbt(_nbt);
		AddEntity(std::make_shared<ChickenEntity>(entity));
	}
	if (id == "Skeleton") {
		SkeletonEntity entity;
		entity.LoadFromNbt(_nbt);
		AddEntity(std::make_shared<SkeletonEntity>(entity));
	}
	if (id == "Creeper") {
		CreeperEntity entity;
		entity.LoadFromNbt(_nbt);
		AddEntity(std::make_shared<CreeperEntity>(entity));
	}
	if (id == "Spider") {
		SpiderEntity entity;
		entity.LoadFromNbt(_nbt);
		AddEntity(std::make_shared<SpiderEntity>(entity));
	}
	if (id == "Arrow") {
		ArrowEntity entity;
		entity.LoadFromNbt(_nbt);
		AddEntity(std::make_shared<ArrowEntity>(entity));
	}
	if (id == "Snowball") {
		SnowballEntity entity;
		entity.LoadFromNbt(_nbt);
		AddEntity(std::make_shared<SnowballEntity>(entity));
	}
	if (id == "Slime") {
		SlimeEntity entity;
		entity.LoadFromNbt(_nbt);
		AddEntity(std::make_shared<SlimeEntity>(entity));
	}
	if (id == "FallingSand") {
		FallingBlockEntity entity(Vec3{}, BLOCK_AIR);
		entity.LoadFromNbt(_nbt);
		if (entity.block == BLOCK_SAND)
			entity.type = EntityType::FALLING_SAND;
		else if (entity.block == BLOCK_GRAVEL)
			entity.type = EntityType::FALLING_GRAVEL;
		AddEntity(std::make_shared<FallingBlockEntity>(entity));
	}
	if (id == "PrimedTnt") {
		TntEntity entity;
		entity.LoadFromNbt(_nbt);
		AddEntity(std::make_shared<TntEntity>(entity));
	}
	if (id == "Squid") {
		SquidEntity entity;
		entity.LoadFromNbt(_nbt);
		AddEntity(std::make_shared<SquidEntity>(entity));
	}
	if (id == "Wolf") {
		WolfEntity entity;
		entity.LoadFromNbt(_nbt);
		AddEntity(std::make_shared<WolfEntity>(entity));
	}
	if (id == "Ghast") {
		GhastEntity entity;
		entity.LoadFromNbt(_nbt);
		AddEntity(std::make_shared<GhastEntity>(entity));
	}
}

std::vector<Tag> EntityManager::CollectEntitiesForSave(Int2 _cpos, bool _clearCollectedEntities) {
	// Collect entities for this chunk coordinates (basically our entity container)
	// We then serialize these entities and return the vector of nbt tags
	// We mark the entities as dead for cleanup afterwards
	std::vector<Tag> collectedEntities;

	auto it = entityContainers.find(_cpos);
	if (it == entityContainers.end())
		return collectedEntities;

	auto& container = it->second;
	for (size_t i = 0; i < container.buckets.size(); i++) {
		auto& bucket = container.buckets[i];
		for (auto& entityPtr : bucket.entities) {
			// Is this entity dead but not collected?
			if (entityPtr->isDead)
				continue; // We are dead so no save
			if (entityPtr->type == EntityType::PLAYER)
				continue; // players cannot be saved
			if (_clearCollectedEntities)
				entityPtr->isDead = true; // Mark the entity as dead for cleanup
			auto compound = entityPtr->SerializeToNbt();
			if (!compound)
				continue; // If something went wrong abort save
			collectedEntities.push_back(std::move(*compound));
		}
	}

	if (_clearCollectedEntities)
		EraseContainer(_cpos);
	else
		PruneEmptyContainer(_cpos);

	return collectedEntities;
}

std::optional<std::string> EntityManager::GetEntityNbtId(EntityType _type) {
	switch (_type) {
	case EntityType::ITEM:
		return "Item";
	case EntityType::BOAT:
		return "Boat";
	case EntityType::LIT_TNT:
		return "PrimedTnt";
	case EntityType::ARROW:
		return "Arrow";
	case EntityType::THROWN_SNOWBALL:
		return "Snowball";
	case EntityType::PAINTING:
		return "Painting";
	case EntityType::CREEPER:
		return "Creeper";
	case EntityType::SKELETON:
		return "Skeleton";
	case EntityType::SPIDER:
		return "Spider";
	case EntityType::GIANT_ZOMBIE:
		return "Giant";
	case EntityType::ZOMBIE:
		return "Zombie";
	case EntityType::SLIME:
		return "Slime";
	case EntityType::GHAST:
		return "Ghast";
	case EntityType::ZOMBIE_PIGMAN:
		return "PigZombie";
	case EntityType::PIG:
		return "Pig";
	case EntityType::SHEEP:
		return "Sheep";
	case EntityType::COW:
		return "Cow";
	case EntityType::CHICKEN:
		return "Chicken";
	case EntityType::SQUID:
		return "Squid";
	case EntityType::WOLF:
		return "Wolf";

	// Vanilla only has ONE minecart entity/string
	// There is a type field in the nbt itself
	case EntityType::MINECART:
	case EntityType::STORAGE_MINECART:
	case EntityType::FURNACE_MINECART:
		return "Minecart";

	// Same deal as minecarts
	// not a separate string.
	case EntityType::FALLING_SAND:
	case EntityType::FALLING_GRAVEL:
		return "FallingSand";

	// Players have a special case!
	case EntityType::PLAYER:
		return "Player";

	// These have no mapping!
	case EntityType::NONE:
	case EntityType::FISH:
	case EntityType::FIREBALL:
	case EntityType::THROWN_EGG:
	case EntityType::FISHING_BOBBER:
	case EntityType::LIGHTNING_BOLT:
	default:
		return std::nullopt;
	}
}