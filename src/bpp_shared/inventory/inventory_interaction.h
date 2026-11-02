/*
 * Copyright (c) 2026, Aidan <JcbbcEnjoyer>
 * Copyright (c) 2026, jwaxy <jwaxy.is-a.dev>
 *
 * SPDX-License-Identifier: AGPL-3.0-only
*/
#pragma once
#include "entities/entity_player.h"
#include "inventory.h"
#include "inventory/item_stack.h"
#include <span>

struct DeltaSlot {
	ItemStack stack;
	int slot = 0;
};

// Used for actually interacting with inventories, will typically wrap 1 or more inventory objects for things like chests, etc
struct InventoryInteraction {
	std::vector<ItemStack> snapshot;
	ItemStack carried;
	Inventory* inventory;

	InventoryInteraction(Inventory* _inv);
	double GetDistSquared(Vec3 p1, Vec3 p2) {
		Vec3 delta = p1 - p2;
		return (delta.x * delta.x + delta.y * delta.y + delta.z * delta.z);
	}
	virtual ~InventoryInteraction() = default;

	virtual bool CanExist(PlayerEntity& player);

	// Take a snapshot of this inventory
	virtual void InitSnapshot();

	// Analyze the snapshot vs the current inventory
	// Returns a view of the slots that are different. The buffer is reused
	// between ticks so an unchanged inventory performs no allocation.
	std::vector<DeltaSlot> diffScratch;
	// Set when the shared client-facing mirror may differ from the snapshot
	// (a click or a merge). Lets container TickDiff skip the diff scan on
	// ticks where neither the real inventory nor the mirror changed.
	bool mirrorDirty = true;
	std::vector<int> dirtySlots;
	virtual std::span<const DeltaSlot> TickDiff();

	// Copies only slots that differ from the mirror, recording their indices.
	static bool SyncSlots(std::vector<ItemStack>& _shared, size_t _offset, const std::vector<ItemStack>& _source,
	                      size_t _begin, size_t _end, std::vector<int>& _changedSlots) {
		bool changed = false;
		const size_t count = _end > _begin ? _end - _begin : 0;
		// Hoist the data pointers: pushing into _changedSlots could otherwise
		// force the compiler to reload the source arrays on every iteration.
		ItemStack* const shared = _shared.data();
		const ItemStack* const source = _source.data();
		for (size_t i = 0; i < count; ++i) {
			auto& dst = shared[_offset + i];
			const auto& src = source[_begin + i];
			if (dst == src)
				continue;
			dst = src;
			_changedSlots.push_back(int(_offset + i));
			changed = true;
		}
		return changed;
	}

	// Emits diffs only for slots recorded by SyncSlots instead of scanning the
	// whole snapshot again.
	std::span<const DeltaSlot> EmitTrackedDiffs() {
		diffScratch.clear();
		if (!inventory)
			return diffScratch;
		for (int index : dirtySlots) {
			if (index < 0 || size_t(index) >= snapshot.size() || size_t(index) >= inventory->slots.size())
				continue;
			auto& snap = snapshot[size_t(index)];
			const auto& value = inventory->slots[size_t(index)];
			if (snap == value)
				continue;
			snap = value;
			diffScratch.push_back({ snap, index });
		}
		return diffScratch;
	}

	virtual void OnLeftClick(int _slot);

	virtual void OnRightClick(int _slot);

	virtual void OnShiftClick(int _slot);

	virtual void OnInteractionClosed(PlayerEntity& player);
};