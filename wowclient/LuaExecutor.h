/*
 * ============================================================== *
 *  OpenGlider                                                    *
 *  Copyright (c) 2026 Alexander Blohmè <alexander.blohme@gmail.com> *
 *  SPDX-License-Identifier: LGPL-3.0-or-later                    *
 *  Licensed under the GNU Lesser General Public License,         *
 *  version 3 or later. See COPYING.LESSER and COPYING.           *
 * ============================================================== *
 *
 *  Date: 2026-09-12
 *  File: LuaExecutor.h
 *
 *  Desc:
 *  Declarations and interface for LuaExecutor (WoW client memory
 *  reading, process inspection, and layout definitions).
 *
 *
 */
#pragma once

#include "og/wowclient/ClientLayout.h"
#include "og/wowclient/ProcessMemory.h"

#include <cstdint>
#include <string>
#include <string_view>

namespace og::wowclient
{

// Runs Lua, and calls client functions, inside the game client.
//
// The bot can only read and write the client's memory; it cannot create a
// thread there. So it borrows one: the D3D device's vtable entry for EndScene
// is repointed at a small stub written into an unused region of the client's
// address space, and the client's own render thread runs that stub once per
// frame. The stub checks a request flag, calls FrameScript::Execute with the
// script we left in a buffer - or a client function with an object in ecx and
// one argument, which is how a unit is right-clicked without the mouse -
// clears the flag, and tail-jumps to the real EndScene so rendering carries on.
//
// Swapping a vtable *pointer* (rather than detouring EndScene's code) avoids
// having to decode instruction lengths to relocate a prologue, and makes
// uninstalling a single pointer write.
//
// The bot's own writes ignore page protection, the client's do not: the stub
// has to sit where the client may execute, and the flag it clears where the
// client may write. Both are checked against the memory map before anything
// is written, and install() is a no-op on a build whose layout has no D3D
// device or code cave addresses.
class LuaExecutor
{
public:
    LuaExecutor(IProcessMemory& memory, const ClientLayout& layout);
    ~LuaExecutor();

    LuaExecutor(const LuaExecutor&) = delete;
    LuaExecutor& operator=(const LuaExecutor&) = delete;

    // True when this build has the addresses the stub needs.
    bool supported() const;
    // True when it can also run Lua (FrameScript::Execute is known).
    bool supportsLua() const;
    bool installed() const
    {
        return installed_;
    }

    // Writes the stub and swaps the vtable entry. Safe to call repeatedly.
    bool install();
    // Puts the original function back and clears the caves. Called by the destructor.
    void uninstall();

    // Queues one script for the render thread. Returns false if it could not be
    // written, or if a previous request has not been picked up yet.
    bool execute(std::string_view script);

    // Queues a thiscall: function(thisPointer, argument), the callee popping the
    // argument. Same rules as execute().
    bool call(std::uint32_t function, std::uint32_t thisPointer, std::uint32_t argument);

    // True once the client has run the last queued request.
    bool idle() const;

    const std::string& lastError() const
    {
        return error_;
    }

    // Longest script execute() accepts, given the data cave size.
    std::size_t maxScriptLength() const;

    // Resolved addresses, for probes and tests.
    std::uint32_t deviceAddress() const
    {
        return device_;
    }

    std::uint32_t vtableAddress() const
    {
        return vtable_;
    }

    std::uint32_t originalEndScene() const
    {
        return originalEndScene_;
    }

    std::uint32_t stubAddress() const
    {
        return caveStub();
    }

    // Where the stub and the buffers ended up, once install() has run. The
    // same address unless the layout gives the data a cave of its own.
    std::uint32_t caveAddress() const
    {
        return cave_;
    }

    std::uint32_t dataAddress() const
    {
        return data_;
    }

private:
    // Data: [flag][script...][call: function, this, argument][name]
    // Code: [...][stub] - the end of the code cave. Without a separate data
    // cave the data takes the code cave's start.
    std::uint32_t caveFlag() const
    {
        return data_;
    }

    std::uint32_t caveScript() const
    {
        return data_ + 4;
    }

    std::uint32_t caveCall() const;
    std::uint32_t caveName() const;
    std::uint32_t caveStub() const;

    bool separateData() const
    {
        return layout_.dataCave != 0;
    }

    std::uint32_t dataSize() const;

    bool resolveDevice();
    // Picks somewhere safe for the stub: the configured address when it is
    // genuinely unused, otherwise a run of zeros in an executable mapping
    // inside the image. Client builds move around, so a hard-coded cave cannot
    // be trusted on its own. A separate data cave is only ever the layout's.
    bool resolveCave();
    bool caveIsFree(std::uint32_t address, std::uint32_t size) const;
    // Whether the client itself may write / execute the whole range. True when
    // the platform cannot list mappings, as there is nothing to check.
    bool clientMay(std::uint32_t address, std::uint32_t size, bool write, bool execute) const;
    std::uint32_t imageEnd() const;
    std::string buildStub() const;
    bool fail(std::string message);

    IProcessMemory& memory_;
    const ClientLayout& layout_;

    std::uint32_t cave_ = 0;
    std::uint32_t data_ = 0;
    bool installed_ = false;
    std::uint32_t device_ = 0;
    std::uint32_t vtable_ = 0;
    std::uint32_t originalEndScene_ = 0;
    std::string error_;
};

} // namespace og::wowclient
