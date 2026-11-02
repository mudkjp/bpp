/*
 * Copyright (c) 2026, Aidan <JcbbcEnjoyer>
 * Copyright (c) 2026, jwaxy <jwaxy.is-a.dev>
 *
 * SPDX-License-Identifier: AGPL-3.0-only
 *
*/
#include "inventory_tracker.h"
#include "window0_sync.h"

#include "inventory/interactions/furnace.h"
#include "logger.h"
#include "packet/packet_utils.h"
#include "packet_data.h"
#include "tile_entities/tile_entity.h"
#include <memory>
#include <vector>

void InventoryTracker::Tick(Server& _server) {
	for (auto& session : _server.GetPlayers()) {
		// Diff window 0 only when no container window covers it. The container
		// diff already handles the shared player slots; anything else (armor,
		// crafting grid) is caught up on the first tick after it closes.
		if (InventoryTracker::ShouldDiffWindow0(*session))
			InventoryTracker::SendWindow0Diff(*session);

		// Held item updates (i.e. maps)
		{
			auto heldItem = session->inventory.GetHeldItem();
			if (heldItem) {
				if (auto fn = Items::itemBehavior[heldItem->id].whileHeld) {
					fn(heldItem, *session, _server);
				}
			}
		}

		if (!session->activeInteraction)
			continue;

		// Force close windows that reference tile entities that have been deleted
		if (!session->activeInteraction->CanExist(*session->entity)) {
			session->activeInteraction->OnInteractionClosed(*session->entity);
			PacketUtilities::CloseContainer(*session);
			continue;
		}

		// Send each differing slot
		auto diffs = session->activeInteraction->TickDiff();
		if (diffs.size() <= 5) {
			for (const auto& difference : diffs) {
				PacketUtilities::SendSlot(*session, session->openWindowId, difference.slot, &difference.stack);
			}
		} else {
			// Too many changes, just resend the whole inventory
			PacketUtilities::SendInventory(*session, session->openWindowId, *session->activeInteraction->inventory);
		}

		// Sync container data
		if (auto* furnaceInteraction = dynamic_cast<FurnaceInventoryInteraction*>(session->activeInteraction.get())) {
			auto furnace = furnaceInteraction->tile.lock();
			if (furnace) {
				if ((furnace->dirtyFlags & TileEntityFurnace::FLAG_BURN_TIME) != 0) {
					Packet::ContainerData pkt;
					pkt.windowId = session->openWindowId;
					pkt.containerData.type = PacketData::ContainerDataType::FUEL_REMAINING;
					pkt.containerData.value = furnace->GetBurnTime();
					pkt.Serialize(session->stream);
				}

				if ((furnace->dirtyFlags & TileEntityFurnace::FLAG_MAX_BURN_TIME) != 0) {
					Packet::ContainerData pkt;
					pkt.windowId = session->openWindowId;
					pkt.containerData.type = PacketData::ContainerDataType::FUEL_DURATION;
					pkt.containerData.value = furnace->GetMaxBurnTime();
					pkt.Serialize(session->stream);
				}

				if ((furnace->dirtyFlags & TileEntityFurnace::FLAG_COOK_TIME) != 0) {
					Packet::ContainerData pkt;
					pkt.windowId = session->openWindowId;
					pkt.containerData.type = PacketData::ContainerDataType::SMELTING_PROGRESS;
					pkt.containerData.value = furnace->GetCookTime();
					pkt.Serialize(session->stream);
				}
			}
		}

	}
}