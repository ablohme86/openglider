/*
 * ============================================================== *
 *  OpenGlider                                                    *
 *  Copyright (c) 2026 Alexander Blohmè <alexander.blohme@gmail.com> *
 *  SPDX-License-Identifier: LGPL-3.0-or-later                    *
 *  Licensed under the GNU Lesser General Public License,         *
 *  version 3 or later. See COPYING.LESSER and COPYING.           *
 * ============================================================== *
 *
 *  Date: 2026-09-11
 *  File: ChatLog.h
 *
 *  Desc:
 *  Declarations and interface for ChatLog (WoW client memory reading,
 *  process inspection, and layout definitions).
 *
 *
 */
#pragma once

#include <optional>
#include <string>
#include <string_view>

namespace og::wowclient
{

// One line from the client's chat buffer.
struct ChatLine
{
    int type = 0;        // CHAT_MSG_* value
    std::string channel; // "General - Elwynn Forest" for channel messages
    std::string sender;
    std::string text;
};

// Parses the client's formatted chat entry:
//   Type: [7], Channel: [], Player Name: [Bob], Sender GUID: [..], Active player: [..], Text: [hi]
std::optional<ChatLine> parseChatEntry(std::string_view entry);

// "[Whisper] Bob: hi", "[Trade - City] Bob: WTS", "[System] You are now AFK."
// Empty for lines not worth showing (channel join/leave notices, combat spam).
std::string formatChatLine(const ChatLine& line);

// A line of the chat window as the 64-bit clients show it, links and colours
// and all:
//   |Hchannel:PARTY|h[Party]|h |Hplayer:Tank-Realm:12:PARTY|h[|cff..Tank|r]|h: Must vendor
// Its channel ("Party", "Whisper", or the channel's name), the sender without
// its realm, and the text with the colours and links taken out. Nothing for
// a line no player said (a system message, "Changed Channel: ...").
std::optional<ChatLine> parseChatFrameLine(std::string_view line);

// The text a player sees of a line with links and colours: "|cffff0000x|r"
// is "x", "|Hitem:..|h[Linen Cloth]|h" is "[Linen Cloth]".
std::string plainChatText(std::string_view text);

} // namespace og::wowclient
