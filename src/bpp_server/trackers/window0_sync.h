/*
 * Copyright (c) 2026, Aidan <JcbbcEnjoyer>
 *
 * SPDX-License-Identifier: AGPL-3.0-only
 *
*/
#pragma once
#include "../packet/packet_utils.h"
#include "../player_conn/player_session.h"

// Window 0 is the player's own inventory. It is not visible while a container
// window is open, and the container mirrors handle the shared player slots, so
// the tracker can skip this diff until the container closes.
namespace InventoryTracker {

inline bool ShouldDiffWindow0(const PlayerSession& _session) {
	return _session.activeInteraction == nullptr;
}

inline void SendWindow0Diff(PlayerSession& _session) {
	auto diffs = _session.inventoryInteraction.TickDiff();
	if (diffs.size() <= 5) {
		for (const auto& difference : diffs)
			PacketUtilities::SendSlot(_session, 0, difference.slot, &difference.stack);
	} else {
		PacketUtilities::SendInventory(_session, 0, *_session.inventoryInteraction.inventory);
	}
}

} // namespace InventoryTracker
