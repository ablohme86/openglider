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
 *  File: ClientLayouts.cpp
 *
 *  Desc:
 *  Implementation of ClientLayouts (WoW client memory reading, process
 *  inspection, and layout definitions).
 *
 *
 */
#include "og/wowclient/ClientLayout.h"

#include "og/wowclient/LayoutJson.h"

#include <algorithm>
#include <cctype>
#include <charconv>
#include <deque>
#include <mutex>
#include <string>

namespace og::wowclient
{

namespace
{

// A layout added at run time. Entries are only ever hidden, never destroyed:
// the bot thread may still be reading through a pointer to one.
struct DynamicLayout
{
    std::string name;
    std::string clientVersion;
    std::string json; // to tell a repeat registration from a changed layout
    ModernLayout modern;
    ClientLayout layout;
    bool active = true;
};

std::mutex& registryMutex()
{
    static std::mutex mutex;
    return mutex;
}

std::deque<DynamicLayout>& registry()
{
    static std::deque<DynamicLayout> entries;
    return entries;
}

} // namespace

const std::vector<const ClientLayout*>& legacyLayouts()
{
    static const std::vector<const ClientLayout*> layouts{&layout335a(), &layout1121()};
    return layouts;
}

std::vector<const ClientLayout*> knownLayouts()
{
    std::vector<const ClientLayout*> out(legacyLayouts());
    const auto listed = [&](std::uint32_t build) { return std::any_of(out.begin(), out.end(), [build](const ClientLayout* l) { return l->build == build; }); };
    {
        std::scoped_lock lock(registryMutex());

        for (const DynamicLayout& entry : registry())
        {
            if (entry.active && !listed(entry.layout.build))
            {
                out.push_back(&entry.layout);
            }
        }
    }
#ifdef OG_EMBED_MODERN_LAYOUTS
    // Development builds carry modern layouts; one from the server wins.
    if (!listed(layout256().build))
    {
        out.push_back(&layout256());
    }

    if (!listed(layout1159().build))
    {
        out.push_back(&layout1159());
    }

    if (!listed(layout1601().build))
    {
        out.push_back(&layout1601());
    }
#endif
    return out;
}

const ClientLayout* registerLayout(const LayoutDocument& document)
{
    // The free clients are compiled in and not the server's to replace.
    for (const ClientLayout* l : legacyLayouts())
    {
        if (l->build == document.build)
        {
            return nullptr;
        }
    }

    std::string json = layoutToJson(document).dump();
    std::scoped_lock lock(registryMutex());

    for (DynamicLayout& entry : registry())
    {
        if (!entry.active || entry.layout.build != document.build)
        {
            continue;
        }

        if (entry.json == json)
        {
            return &entry.layout; // validated again, nothing changed
        }

        entry.active = false; // superseded, but kept alive
    }

    DynamicLayout& entry = registry().emplace_back();
    entry.name = document.name;
    entry.json = std::move(json);
    entry.modern = document.modern;
    entry.clientVersion = document.clientVersion;
    entry.layout.name = entry.name;
    entry.layout.clientVersion = entry.clientVersion;
    entry.layout.build = document.build;
    entry.layout.modern = &entry.modern;
    // Whatever the document says: a client described from outside is never written to.
    entry.layout.readOnly = true;
    return &entry.layout;
}

void clearDynamicLayouts()
{
    std::scoped_lock lock(registryMutex());

    for (DynamicLayout& entry : registry())
    {
        entry.active = false;
    }
}

const ClientLayout* layoutForBuild(std::uint32_t build)
{
    for (const ClientLayout* l : knownLayouts())
    {
        if (l->build == build)
        {
            return l;
        }
    }

    return nullptr;
}

const ClientLayout* layoutByName(std::string_view name)
{
    std::uint32_t build = 0;
    const auto [ptr, ec] = std::from_chars(name.data(), name.data() + name.size(), build);

    if (ec == std::errc() && ptr == name.data() + name.size())
    {
        return layoutForBuild(build);
    }

    const auto same = [&](std::string_view other) { return other.size() == name.size() && std::equal(name.begin(), name.end(), other.begin(), [](char a, char b) { return std::tolower(static_cast<unsigned char>(a)) == std::tolower(static_cast<unsigned char>(b)); }); };

    for (const ClientLayout* l : knownLayouts())
    {
        if (same(l->name) || (!l->clientVersion.empty() && same(l->clientVersion)))
        {
            return l;
        }
    }

    return nullptr;
}

} // namespace og::wowclient
