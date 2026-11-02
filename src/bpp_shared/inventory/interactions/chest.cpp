/*
 * Copyright (c) 2026, Aidan <JcbbcEnjoyer>
 *
 * SPDX-License-Identifier: AGPL-3.0-only
*/
#include "chest.h"

ChestInventoryInteraction::ChestInventoryInteraction(InventoryPlayer* _pinv, std::shared_ptr<TileEntityChest> _chest)
    : InventoryInteraction(&sharedInventory), playerInventory(_pinv), chestHandle(_chest),
      chestInventory(&_chest->inventory) {
	sharedInventory.owner = this;
	MergeInventories();
}

ChestInventoryInteraction::~ChestInventoryInteraction() {
	if (!chestHandle.expired())
		WriteBack();
}

bool ChestInventoryInteraction::CanExist(PlayerEntity& _player) {
	if (chestHandle.expired())
		return false;
	auto locked = chestHandle.lock();
	if (!locked)
		return false;
	auto pos = locked->position;
	return (GetDistSquared(_player.position, { pos.x + 0.5, pos.y + 0.5, pos.z + 0.5 }) < 64.0);
}

void ChestInventoryInteraction::InitSnapshot() {
	MergeInventories();
	snapshot = sharedInventory.slots;
	mirrorDirty = false;
}

std::span<const DeltaSlot> ChestInventoryInteraction::TickDiff() {
	dirtySlots.clear();
	const bool changed = SyncSlots(sharedInventory.slots, 0, chestInventory->slots, 0, 27, dirtySlots) |
	                     SyncSlots(sharedInventory.slots, 27, playerInventory->slots, 9, 45, dirtySlots);
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

void ChestInventoryInteraction::MergeInventories() {
	size_t slotCount = 0;
	for (auto& slot : chestInventory->slots)
		sharedInventory.slots[slotCount++] = slot;
	for (size_t i = 9; i < 45; i++)
		sharedInventory.slots[slotCount++] = playerInventory->slots[i];
	mirrorDirty = true;
}

void ChestInventoryInteraction::WriteBack() {
	bool chestChanged = false;
	for (size_t i = 0; i < 27; i++) {
		if (chestInventory->slots[i] == sharedInventory.slots[i])
			continue;
		chestInventory->slots[i] = sharedInventory.slots[i];
		chestChanged = true;
	}
	// Writing slots directly skips OnInventoryChanged, so flag the chest for saving ourselves
	if (chestChanged)
		chestInventory->OnInventoryChanged();
	for (size_t i = 27; i < 63; i++)
		playerInventory->slots[i - 27 + 9] = sharedInventory.slots[i];
}

void ChestInventoryInteraction::OnShiftClick(int _slot) {
	auto stack = sharedInventory.GetStackInSlot(_slot);
	if (!stack)
		return;

	ItemStack copy = *stack;

	if (_slot <= 26) {
		// Chest -> inventory
		playerInventory->MergeItemStackInInventory(copy, true, 9, 44);
	} else {
		// Inventory -> Chest
		chestInventory->MergeItemStackInInventory(copy);
	}

	// Update the source in the real inventory before re-merging
	if (_slot <= 26) {
		chestInventory->slots[size_t(_slot)] = copy.count == 0 ? ItemStack{} : copy;
		chestInventory->OnInventoryChanged();
	} else {
		int playerSlot = _slot - 27 + 9;
		playerInventory->slots[size_t(playerSlot)] = copy.count == 0 ? ItemStack{} : copy;
	}

	// Re-sync sharedInventory from the real inventories
	MergeInventories();
}