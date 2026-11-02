/*
 * Copyright (c) 2025-2026, Pixel Brush <pixelbrush.dev>
 * Copyright (c) 2026, Anya Rihtarshich <vesui@proton.me>
 *
 * SPDX-License-Identifier: AGPL-3.0-only
*/

#pragma once

#include "../player_conn/player_session.h"
#include "constants.h"
#include "inventory/item_stack.h"
#include "networking/packets.h"
#include "strategos.h"
#include "ucs2.h"
#include <optional>
#include <string>
#include <expected>
#include <vector>


#define ERROR_OPERATOR "Only operators can use this command!"
#define ERROR_PLAYER_REQUIRED "This command requires a player context. Use execute <player> <command>."
#define ERROR_PLAYER_NAME_REQUIRED "A player name is required from a non-player sender."
#define ERROR_RELATIVE_REQUIRES_PLAYER "Relative coordinates require a player context."
#define ERROR_CREATIVE "Only creative players can use this command!"
#define ERROR_PERMISSIONS "You lack the required permissions for this command!"
#define ERROR_WHITELIST "Only whitelisted players can use this command!"
#define ERROR_REASON_SYNTAX "Invalid Syntax"
#define ERROR_REASON_NO_EXIST "Command does not exist!"
#define ERROR_REASON_PARAMETERS "Invalid Parameters"
#define ERROR_REASON_TOO_FEW_PARAMETERS "Not enough parameters!"
#define ERROR_REASON_ERROR "Error"
#define ERROR_REASON_NO_CMD "No command passed"

#define MAX_CHAT_LINE_SIZE 60
// Classic client disconnects/crashes if a ChatMessage exceeds this UCS-2 length.
#define MAX_CHAT_MESSAGE_SIZE 119

class Server;
struct CommandContext;

bool IsOperator(PlayerSession& _session, Server& _server);
bool IsOperator(const CommandContext& _ctx);
void SendCommandFeedback(PlayerSession* _recipient, const std::string& _message);
void SendFeedback(const CommandContext& _ctx, const std::string& _message);
ItemStack ParseItemStack(const std::string& _itemArg, std::optional<int> _count = std::nullopt);
void SendChunkedList(const CommandContext& _ctx, const std::string& _header,
                     const std::vector<std::string>& _entries);
std::optional<std::string> ResolveCmdTargetName(const strategos::CmdNode& _cmd, const CommandContext& _ctx);
std::expected<Vec3, std::string> ResolveCmdPos(const CommandContext& _ctx, const strategos::Vec3& _position);

inline bool IsSimpleChatText(std::string_view _text) {
	for (size_t i = 0; i < _text.size();) {
		const auto c = static_cast<unsigned char>(_text[i]);
		if (c < 0x80) {
			++i;
		} else if (c == 0xC2 && i + 1 < _text.size() && static_cast<unsigned char>(_text[i + 1]) == 0xA7) {
			i += 2;
		} else {
			return false;
		}
	}
	return true;
}

inline void SendChat(PlayerSession& _session, const std::string& _message) {
	// Feedback is overwhelmingly ASCII plus the section-sign colour code. In
	// that case the UCS-2 length equals the number of code points, so chunk and
	// encode it straight into the stream instead of doing UTF-8 -> UCS-2 -> UTF-8
	// and a second UCS-2 conversion inside WriteString16.
	if (IsSimpleChatText(_message)) {
		const std::string_view text(_message);
		size_t totalUnits = 0;
		for (size_t i = 0; i < text.size();) {
			if (static_cast<unsigned char>(text[i]) < 0x80)
				++i;
			else
				i += 2;
			++totalUnits;
		}

		size_t prefixBytes = 0;
		size_t prefixUnits = 0;
		if (totalUnits >= 2 && text.size() >= 2 && static_cast<unsigned char>(text[0]) == 0xC2 &&
		    static_cast<unsigned char>(text[1]) == 0xA7) {
			// Old behaviour repeats the first two UCS-2 units on continuation lines.
			if (static_cast<unsigned char>(text[2]) < 0x80) {
				prefixBytes = 3;
			} else {
				prefixBytes = 4;
			}
			prefixUnits = 2;
		}

		size_t offset = 0;
		while (offset < text.size()) {
			size_t chunkUnits = MAX_CHAT_MESSAGE_SIZE;
			std::string_view prefix;
			if (offset > 0 && prefixBytes > 0) {
				prefix = text.substr(0, prefixBytes);
				chunkUnits = MAX_CHAT_MESSAGE_SIZE > prefixUnits ? MAX_CHAT_MESSAGE_SIZE - prefixUnits
				                                                : MAX_CHAT_MESSAGE_SIZE;
			}

			size_t end = offset;
			size_t units = 0;
			while (end < text.size() && units < chunkUnits) {
				end += static_cast<unsigned char>(text[end]) < 0x80 ? 1 : 2;
				++units;
			}

			_session.stream.Write(PacketId::ChatMessage);
			_session.stream.WriteString16Simple(prefix, text.substr(offset, end - offset));
			offset = end;
		}
		return;
	}

	std::u16string ucs2 = ToUCS2(_message);
	std::u16string colorPrefix;
	if (ucs2.size() >= 2 && ucs2[0] == u'\u00A7')
		colorPrefix = ucs2.substr(0, 2);

	size_t offset = 0;
	while (offset < ucs2.size()) {
		size_t chunk = MAX_CHAT_MESSAGE_SIZE;
		std::u16string line;
		if (offset > 0 && !colorPrefix.empty()) {
			line = colorPrefix;
			chunk = MAX_CHAT_MESSAGE_SIZE > colorPrefix.size() ? MAX_CHAT_MESSAGE_SIZE - colorPrefix.size()
			                                                   : MAX_CHAT_MESSAGE_SIZE;
		}
		line += ucs2.substr(offset, chunk);
		const size_t consumed = chunk < ucs2.size() - offset ? chunk : ucs2.size() - offset;
		offset += consumed;

		Packet::ChatMessage pkt;
		pkt.message = ToUTF8(line);
		pkt.Serialize(_session.stream);
	}
}

inline Vec3 ResolveCmdVec3(const strategos::Vec3& _cmd, const Vec3& _origin) {
	Vec3 out{ static_cast<double>(_cmd.pos[0]), static_cast<double>(_cmd.pos[1]), static_cast<double>(_cmd.pos[2]) };
	if (_cmd.is_relative(strategos::Axis::X))
		out.x += _origin.x;
	if (_cmd.is_relative(strategos::Axis::Y))
		out.y += _origin.y;
	if (_cmd.is_relative(strategos::Axis::Z))
		out.z += _origin.z;
	return out;
}

inline void SendTeleport(PlayerSession& _target, Vec3 _position, float _yaw = 0.0f, float _pitch = 0.0f) {
	_target.entity->Teleport(_position, { _yaw, _pitch });

	_target.pendingTeleport = _position;
	_target.position.pos = _position;
	_target.pendingPosition.reset();

	Packet::PlayerPositionAndRotation pkt;
	pkt.position.x = _position.x;
	pkt.position.y = _position.y + PLAYER_EYE_HEIGHT;
	pkt.cameraY = _position.y;
	pkt.position.z = _position.z;
	pkt.yaw = _yaw;
	pkt.pitch = _pitch;
	pkt.onGround = false;
	pkt.Serialize(_target.stream);
}
