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
 *  File: ChatLog.cpp
 *
 *  Desc:
 *  Implementation of ChatLog (WoW client memory reading, process
 *  inspection, and layout definitions).
 *
 *
 */
#include "og/wowclient/ChatLog.h"

#include <cctype>
#include <charconv>
#include <format>

namespace og::wowclient
{

namespace
{

// Text between "<key>: [" and the "]" that ends the field. The text field is
// last, so it runs to the final "]" and may itself contain brackets.
std::optional<std::string_view> fieldValue(std::string_view entry, std::string_view key, bool last)
{
    const std::string marker = std::string(key) + ": [";
    const auto start = entry.find(marker);

    if (start == std::string_view::npos)
    {
        return std::nullopt;
    }

    const auto begin = start + marker.size();
    const auto end = last ? entry.rfind(']') : entry.find("], ", begin);

    if (end == std::string_view::npos || end < begin)
    {
        return std::nullopt;
    }

    return entry.substr(begin, end - begin);
}

// CHAT_MSG_* (3.3.5a) -> label; nullptr = do not show.
const char* typeLabel(int type)
{
    switch (type)
    {
    case 0x00:
        return "System";
    case 0x01:
        return "Say";
    case 0x02:
        return "Party";
    case 0x03:
        return "Raid";
    case 0x04:
        return "Guild";
    case 0x05:
        return "Officer";
    case 0x06:
        return "Yell";
    case 0x07:
    case 0x08:
        return "Whisper";
    case 0x09:
        return "To";
    case 0x0A:
    case 0x0B:
        return "Emote";
    case 0x0C:
        return "Say"; // NPC say
    case 0x0D:
        return "Party"; // NPC party
    case 0x0E:
        return "Yell"; // NPC yell
    case 0x0F:
        return "Whisper"; // NPC whisper
    case 0x10:
        return "Emote"; // NPC emote
    case 0x17:
        return "AFK";
    case 0x18:
        return "DND";
    case 0x19:
    case 0x1D:
    case 0x1F:
        return "System";
    case 0x1A:
        return "Skill";
    case 0x1B:
    case 0x1C:
        return "Loot";
    case 0x1E:
        return "Tradeskill";
    case 0x21:
        return "Experience";
    case 0x22:
        return "Honor";
    case 0x23:
        return "Reputation";
    case 0x24:
    case 0x25:
    case 0x26:
        return "Battleground";
    case 0x27:
        return "Raid Leader";
    case 0x28:
        return "Raid Warning";
    default:
        return nullptr; // channel notices (0x12-0x16), combat info (0x20), unknown
    }
}

} // namespace

std::optional<ChatLine> parseChatEntry(std::string_view entry)
{
    const auto type = fieldValue(entry, "Type", false);
    const auto text = fieldValue(entry, "Text", true);

    if (!type || !text)
    {
        return std::nullopt;
    }

    ChatLine line;
    const auto [ptr, ec] = std::from_chars(type->data(), type->data() + type->size(), line.type);

    if (ec != std::errc())
    {
        return std::nullopt;
    }

    line.channel = std::string(fieldValue(entry, "Channel", false).value_or(""));
    line.sender = std::string(fieldValue(entry, "Player Name", false).value_or(""));
    line.text = std::string(*text);
    return line;
}

std::string formatChatLine(const ChatLine& line)
{
    std::string label;

    if (line.type == 0x11)
    {
        label = line.channel.empty() ? "Channel" : line.channel;
    }
    else if (const char* known = typeLabel(line.type))
    {
        label = known;
    }
    else
    {
        return {};
    }

    if (line.sender.empty())
    {
        return std::format("[{}] {}", label, line.text);
    }

    if (label == "Emote")
    {
        return std::format("[{}] {} {}", label, line.sender, line.text);
    }

    return std::format("[{}] {}: {}", label, line.sender, line.text);
}

std::string plainChatText(std::string_view text)
{
    std::string out;

    for (std::size_t i = 0; i < text.size();)
    {
        if (text[i] == '|' && i + 1 < text.size())
        {
            const char code = text[i + 1];

            if (code == 'c' && i + 10 <= text.size())
            {
                i += 10; // |cAARRGGBB
                continue;
            }

            if (code == 'r' || code == 'h')
            {
                i += 2;
                continue;
            }

            if (code == 'H')
            {
                // |Hkind:data|h - the data is not shown.
                const auto end = text.find("|h", i + 2);
                i = end == std::string_view::npos ? text.size() : end + 2;
                continue;
            }

            if (code == '|')
            {
                out += '|';
                i += 2;
                continue;
            }
        }

        out += text[i++];
    }

    return out;
}

std::optional<ChatLine> parseChatFrameLine(std::string_view line)
{
    const auto player = line.find("|Hplayer:");

    if (player == std::string_view::npos)
    {
        return std::nullopt;
    }

    ChatLine out;
    // The sender: up to the realm or the link's next field.
    const std::size_t nameAt = player + 9;
    const std::size_t nameEnd = line.find_first_of(":-|", nameAt);
    out.sender = std::string(line.substr(nameAt, nameEnd == std::string_view::npos ? std::string_view::npos : nameEnd - nameAt));

    // The player link ends with "|h"; what is said follows it.
    const auto linkText = line.find("|h", nameAt);
    const auto linkEnd = linkText == std::string_view::npos ? std::string_view::npos : line.find("|h", linkText + 2);

    if (out.sender.empty() || linkEnd == std::string_view::npos)
    {
        return std::nullopt;
    }

    std::string_view rest = line.substr(linkEnd + 2);
    const auto lower = [](std::string_view s)
    {
        std::string l(s);

        for (char& c : l)
        {
            c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
        }

        return l;
    };

    if (const std::string head = lower(line.substr(0, player)); head.find("|hchannel:party") != std::string::npos)
    {
        out.channel = "Party";
    }
    else if (const auto channel = head.find("|hchannel:"); channel != std::string::npos)
    {
        const auto shown = line.find("|h[", channel);
        const auto shownEnd = shown == std::string_view::npos ? shown : line.find("]|h", shown);
        out.channel = shown == std::string_view::npos || shownEnd == std::string_view::npos ? std::string("Channel") : std::string(line.substr(shown + 3, shownEnd - shown - 3));
    }
    else if (lower(rest).starts_with(" whispers:"))
    {
        out.channel = "Whisper";
        rest.remove_prefix(10);
    }
    else if (lower(rest).starts_with(" says:"))
    {
        out.channel = "Say";
        rest.remove_prefix(6);
    }
    else
    {
        out.channel = "Other";
    }

    if (rest.starts_with(":"))
    {
        rest.remove_prefix(1);
    }

    std::string text = plainChatText(rest);
    text.erase(0, text.find_first_not_of(' '));

    while (!text.empty() && text.back() == ' ')
    {
        text.pop_back();
    }

    out.text = std::move(text);
    return out;
}

} // namespace og::wowclient
