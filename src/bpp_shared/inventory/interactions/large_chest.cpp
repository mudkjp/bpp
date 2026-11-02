/*
 * Copyright (c) 2026, Aidan <JcbbcEnjoyer>
 *
 * SPDX-License-Identifier: AGPL-3.0-only
*/
#include "large_chest.h"

LargeChestInventoryInteraction::LargeChestInventoryInteraction(InventoryPlayer* _pinv,
                                                               std::shared_ptr<TileEntityChest> _upper,
                                                               std::shared_ptr<TileEntityChest> _lower)
    : InventoryInteraction(&sharedInventory), playerInventory(_pinv), upperChest(_upper), lowerChest(_lower),
      chestInventory(&_upper->inventory, &_lower->inventory) {
	sharedInventory.owner = this;
	MergeInventories();
}

LargeChestInventoryInteraction::~LargeChestInventoryInteraction() {
	if (!upperChest.expired() && !lowerChest.expired())
		WriteBack();
}

bool LargeChestInventoryInteraction::CanExist(PlayerEntity& _player) {
	auto lockedUpper = upperChest.lock();
	auto lockedLower = lowerChest.lock();
	if (!lockedUpper || !lockedLower)
		return false;
	auto avgPos = lockedUpper->position + lockedLower->position;
	return (GetDistSquared(_player.position, { avgPos.x / 2.0, avgPos.y / 2.0, avgPos.z / 2.0 }) < 64.0);
}

void LargeChestInventoryInteraction::InitSnapshot() {
	MergeInventories();
	snapshot = sharedInventory.slots;
	mirrorDirty = false;
}

std::span<const DeltaSlot> LargeChestInventoryInteraction::TickDiff() {
	dirtySlots.clear();
	const bool changed =
	    SyncSlots(sharedInventory.slots, 0, chestInventory.upper->slots, 0, 27, dirtySlots) |
	    SyncSlots(sharedInventory.slots, 27, chestInventory.lower->slots, 0, 27, dirtySlots) |
	    SyncSlots(sharedInventory.slots, 54, playerInventory->slots, 9, 45, dirtySlots);
	if (!changed && !mirrorDirty) {
		diffScratch.clear();
		return {};
	}
	if (mirrorDirty) {
		mirrorDirty = false;
		return InventoryInteraction::TickDiff();
	}
	return EmitTrackedDiffs();
}

void LargeChestInventoryInteraction::MergeInventories() {
	// Access the two chest inventories directly instead of going through the
	// InventoryLargeChest virtual wrappers for every slot.
	ItemStack* dst = sharedInventory.slots.data();
	const auto& upperSlots = chestInventory.upper->slots;
	const auto& lowerSlots = chestInventory.lower->slots;
	size_t slotCount = 0;
	for (const auto& stack : upperSlots)
		dst[slotCount++] = stack;
	for (const auto& stack : lowerSlots)
		dst[slotCount++] = stack;
	for (size_t i = 9; i < 45; i++)
		dst[slotCount++] = playerInventory->slots[i];
	mirrorDirty = true;
}

void LargeChestInventoryInteraction::WriteBack() {
	ItemStack* src = sharedInventory.slots.data();
	auto& upper = *chestInventory.upper;
	auto& lower = *chestInventory.lower;

	// Only touch slots that actually changed, and notify each chest once
	// instead of going through a virtual setter per slot.
	bool upperChanged = false;
	for (size_t i = 0; i < upper.slots.size(); ++i) {
		if (upper.slots[i] == src[i])
			continue;
		upper.slots[i] = src[i];
		upperChanged = true;
	}
	bool lowerChanged = false;
	for (size_t i = 0; i < lower.slots.size(); ++i) {
		auto& dst = lower.slots[i];
		const auto& value = src[upper.slots.size() + i];
		if (dst == value)
			continue;
		dst = value;
		lowerChanged = true;
	}
	if (upperChanged)
		upper.OnInventoryChanged();
	if (lowerChanged)
		lower.OnInventoryChanged();

	const size_t chestSlots = upper.slots.size() + lower.slots.size();
	for (size_t i = 0; i < 36; ++i) {
		auto& dst = playerInventory->slots[9 + i];
		const auto& value = src[chestSlots + i];
		if (dst != value)
			dst = value;
	}
}

void LargeChestInventoryInteraction::OnShiftClick(int _slot) {
	auto stack = sharedInventory.GetStackInSlot(_slot);
	if (!stack)
		return;

	ItemStack copy = *stack;

	if (_slot <= 53) {
		// Chest -> inventory
		playerInventory->MergeItemStackInInventory(copy, true, 9, 44);
	} else {
		// Inventory -> Chest
		chestInventory.MergeItemStackInInventory(copy);
	}

	// Update the source in the real inventory before re-merging
	if (_slot <= 53) {
		ItemStack* ptr = copy.count == 0 ? nullptr : &copy;
		chestInventory.SetInventorySlotContents(_slot, ptr);
	} else {
		int playerSlot = _slot - 54 + 9;
		playerInventory->slots[size_t(playerSlot)] = copy.count == 0 ? ItemStack{} : copy;
	}

	// Re-sync sharedInventory from the real inventories
	MergeInventories();
}