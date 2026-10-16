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
 *  File: LuaExecutor.cpp
 *
 *  Desc:
 *  Implementation of LuaExecutor (WoW client memory reading, process
 *  inspection, and layout definitions).
 *
 *
 */
#include "og/wowclient/LuaExecutor.h"

#include <algorithm>
#include <array>
#include <chrono>
#include <cstring>
#include <format>
#include <thread>

namespace og::wowclient
{

namespace
{

// Room reserved at the end of the code cave for the stub.
constexpr std::uint32_t kStubSize = 96;
constexpr std::uint32_t kNameSize = 8;
constexpr std::uint32_t kCallSize = 12; // function, this pointer, argument
// flag + at least a short script + call + name.
constexpr std::uint32_t kMinDataSize = 4 + 64 + kCallSize + kNameSize;

// What the request flag asks the stub to do.
constexpr std::uint8_t kRequestLua = 1;
constexpr std::uint8_t kRequestCall = 2;

void emit8(std::string& out, std::uint8_t v)
{
    out.push_back(static_cast<char>(v));
}

void emit32(std::string& out, std::uint32_t v)
{
    for (int i = 0; i < 4; ++i)
    {
        emit8(out, static_cast<std::uint8_t>((v >> (i * 8)) & 0xFF));
    }
}

} // namespace

LuaExecutor::LuaExecutor(IProcessMemory& memory, const ClientLayout& layout) : memory_(memory), layout_(layout)
{
}

LuaExecutor::~LuaExecutor()
{
    uninstall();
}

std::uint32_t LuaExecutor::dataSize() const
{
    if (separateData())
    {
        return layout_.dataCaveSize;
    }

    return layout_.codeCaveSize > kStubSize ? layout_.codeCaveSize - kStubSize : 0;
}

bool LuaExecutor::supported() const
{
    return layout_.d3dDevicePointer != 0 && layout_.codeCave != 0 && layout_.codeCaveSize >= kStubSize && dataSize() >= kMinDataSize;
}

bool LuaExecutor::supportsLua() const
{
    return supported() && layout_.luaDoString != 0;
}

std::size_t LuaExecutor::maxScriptLength() const
{
    if (!supportsLua())
    {
        return 0;
    }

    return dataSize() - 4 - kCallSize - kNameSize - 1; // -1 for the terminator
}

std::uint32_t LuaExecutor::caveName() const
{
    return data_ + dataSize() - kNameSize;
}

std::uint32_t LuaExecutor::caveCall() const
{
    return caveName() - kCallSize;
}

std::uint32_t LuaExecutor::caveStub() const
{
    return cave_ + layout_.codeCaveSize - kStubSize;
}

// A cave is usable only if every byte of it is currently unused padding.
bool LuaExecutor::caveIsFree(std::uint32_t address, std::uint32_t size) const
{
    std::vector<std::uint8_t> existing(size, 0xFF);

    if (!memory_.read(address, existing.data(), existing.size()))
    {
        return false;
    }

    for (const auto byte : existing)
    {
        if (byte != 0x00 && byte != 0xCC)
        {
            return false;
        }
    }

    return true;
}

bool LuaExecutor::clientMay(std::uint32_t address, std::uint32_t size, bool write, bool execute) const
{
    const auto regions = memory_.regions();

    if (regions.empty())
    {
        return true;
    }

    const std::uint64_t end = static_cast<std::uint64_t>(address) + size;

    for (const auto& region : regions)
    {
        if (address < region.start || end > region.end)
        {
            continue;
        }

        return region.readable && (!write || region.writable) && (!execute || region.executable);
    }

    return false; // unmapped, or spread over mappings that may differ
}

// End of the client's image, from the PE header's SizeOfImage. The cave has to
// live inside the image: anonymous memory elsewhere belongs to the allocator,
// which would happily hand our stub's address out to something else.
std::uint32_t LuaExecutor::imageEnd() const
{
    const auto lfanew = memory_.read<std::uint32_t>(consts::ImageBase + 0x3C).value_or(0);

    if (lfanew == 0 || lfanew > 0x1000)
    {
        return 0;
    }

    const std::uint32_t optional = consts::ImageBase + lfanew + 24; // PE sig + COFF header

    if (memory_.read<std::uint16_t>(optional).value_or(0) != 0x10B)
    {
        return 0; // PE32
    }

    const auto size = memory_.read<std::uint32_t>(optional + 0x38).value_or(0); // SizeOfImage

    if (size == 0 || size > 0x40000000)
    {
        return 0;
    }

    return consts::ImageBase + size;
}

bool LuaExecutor::resolveCave()
{
    // The stub clears its request flag itself. The bot's writes ignore page
    // protection, the client's do not, so a cave that also holds the flag has
    // to be writable by the client - a flag on a read-only page crashes it.
    const bool shared = !separateData();
    const std::uint32_t size = layout_.codeCaveSize;

    // The layout's address is only a hint; use it when it really is free.
    if (caveIsFree(layout_.codeCave, size) && clientMay(layout_.codeCave, size, shared, true))
    {
        cave_ = layout_.codeCave;
    }
    else
    {
        // Otherwise hunt for padding the render thread can execute. Without a
        // memory map there is nothing safe to guess.
        const auto regions = memory_.regions();

        if (regions.empty())
        {
            return fail("no usable code cave and this platform cannot list mappings");
        }

        const std::uint32_t end = imageEnd();

        if (end == 0)
        {
            return fail("cannot read SizeOfImage to bound the code cave search");
        }

        // Take the largest run of padding, not the first: a long stretch of zeros
        // inside the image is unused space, while a short one may be a live buffer
        // that merely happens to be zero right now.
        std::uint32_t best = 0;
        std::uint32_t bestRun = 0;

        for (const auto& region : regions)
        {
            if (!region.executable || !region.readable || (shared && !region.writable))
            {
                continue;
            }

            if (region.end <= consts::ImageBase || region.start >= end)
            {
                continue;
            }

            const std::uint64_t from = std::max<std::uint64_t>(region.start, consts::ImageBase);
            const std::uint64_t to = std::min<std::uint64_t>(region.end, end);
            std::uint32_t runStart = 0;
            std::uint32_t run = 0;

            for (std::uint64_t at = from; at + size <= to; at += size)
            {
                const auto candidate = static_cast<std::uint32_t>(at);

                if (caveIsFree(candidate, size))
                {
                    if (run == 0)
                    {
                        runStart = candidate;
                    }

                    run += size;

                    if (run > bestRun)
                    {
                        bestRun = run;
                        best = runStart;
                    }
                }
                else
                {
                    run = 0;
                }
            }
        }

        if (best == 0)
        {
            return fail(shared ? "found no padding inside the client image the client can both execute and write" : "found no executable padding inside the client image for the code cave");
        }

        cave_ = best;
    }

    if (shared)
    {
        data_ = cave_;
        return true;
    }

    if (!caveIsFree(layout_.dataCave, layout_.dataCaveSize))
    {
        return fail("the data cave is in use");
    }

    if (!clientMay(layout_.dataCave, layout_.dataCaveSize, true, false))
    {
        return fail("the client cannot write to the data cave");
    }

    data_ = layout_.dataCave;
    return true;
}

bool LuaExecutor::fail(std::string message)
{
    error_ = std::move(message);
    return false;
}

bool LuaExecutor::resolveDevice()
{
    const auto container = memory_.readPointer(layout_.d3dDevicePointer).value_or(0);

    if (container == 0)
    {
        return fail("D3D device container pointer is null (client not rendering yet?)");
    }

    const auto device = memory_.readPointer(container + layout_.d3dDeviceOffset).value_or(0);

    if (device == 0)
    {
        return fail("D3D device pointer is null");
    }

    const auto vtable = memory_.readPointer(device).value_or(0);

    if (vtable == 0)
    {
        return fail("D3D device vtable pointer is null");
    }

    device_ = device;
    vtable_ = vtable;
    return true;
}

// The stub the client's render thread runs every frame. Registers and flags are
// preserved, so EndScene's caller cannot tell it ran. The request flag picks
// what it does: run the queued Lua script, or call a client function with an
// object in ecx and one argument on the stack (thiscall: the callee cleans up).
std::string LuaExecutor::buildStub() const
{
    std::string lua;

    if (layout_.luaDoString != 0 && layout_.luaFastcall)
    {
        emit8(lua, 0xB9); // mov ecx, script
        emit32(lua, caveScript());
        emit8(lua, 0xBA); // mov edx, name
        emit32(lua, caveName());
        emit8(lua, 0xB8); // mov eax, FrameScript::Execute
        emit32(lua, layout_.luaDoString);
        emit8(lua, 0xFF); // call eax      (fastcall: nothing on the stack)
        emit8(lua, 0xD0);
    }
    else if (layout_.luaDoString != 0)
    {
        emit8(lua, 0x6A); // push 0        (lua state)
        emit8(lua, 0x00);
        emit8(lua, 0x68); // push name
        emit32(lua, caveName());
        emit8(lua, 0x68); // push script
        emit32(lua, caveScript());
        emit8(lua, 0xB8); // mov eax, FrameScript::Execute
        emit32(lua, layout_.luaDoString);
        emit8(lua, 0xFF); // call eax
        emit8(lua, 0xD0);
        emit8(lua, 0x83); // add esp, 12   (cdecl: caller cleans up)
        emit8(lua, 0xC4);
        emit8(lua, 0x0C);
    }

    std::string call;
    emit8(call, 0xFF); // push dword [argument]
    emit8(call, 0x35);
    emit32(call, caveCall() + 8);
    emit8(call, 0x8B); // mov ecx, [this]
    emit8(call, 0x0D);
    emit32(call, caveCall() + 4);
    emit8(call, 0xA1); // mov eax, [function]
    emit32(call, caveCall());
    emit8(call, 0xFF); // call eax
    emit8(call, 0xD0);

    constexpr std::size_t kClearSize = 10; // mov dword [flag], 0

    std::string code;
    emit8(code, 0x60); // pushad
    emit8(code, 0x9C); // pushfd
    emit8(code, 0xA1); // mov eax, [flag]
    emit32(code, caveFlag());

    emit8(code, 0x83); // cmp eax, kRequestLua
    emit8(code, 0xF8);
    emit8(code, kRequestLua);
    emit8(code, 0x75); // jne next     (over the Lua call and its jmp)
    emit8(code, static_cast<std::uint8_t>(lua.size() + 2));
    code += lua;
    emit8(code, 0xEB); // jmp clear    (over the next check and the call)
    emit8(code, static_cast<std::uint8_t>(5 + call.size()));

    // next:
    emit8(code, 0x83); // cmp eax, kRequestCall
    emit8(code, 0xF8);
    emit8(code, kRequestCall);
    emit8(code, 0x75); // jne skip     (over the call and the clear)
    emit8(code, static_cast<std::uint8_t>(call.size() + kClearSize));
    code += call;

    // clear:
    emit8(code, 0xC7); // mov dword [flag], 0
    emit8(code, 0x05);
    emit32(code, caveFlag());
    emit32(code, 0);
    // skip:
    emit8(code, 0x9D); // popfd
    emit8(code, 0x61); // popad
    emit8(code, 0xB8); // mov eax, original EndScene
    emit32(code, originalEndScene_);
    emit8(code, 0xFF); // jmp eax
    emit8(code, 0xE0);
    return code;
}

bool LuaExecutor::install()
{
    if (installed_)
    {
        return true;
    }

    if (!supported())
    {
        return fail("this client build has no D3D device or code cave addresses");
    }

    if (!memory_.attached())
    {
        return fail("not attached to a client");
    }

    if (!resolveDevice())
    {
        return false;
    }

    // The caves have to be known before caveStub() and friends mean anything.
    if (!resolveCave())
    {
        return false;
    }

    const std::uint32_t slot = vtable_ + static_cast<std::uint32_t>(layout_.d3dEndSceneIndex) * 4;
    const auto endScene = memory_.readPointer(slot).value_or(0);

    if (endScene == 0)
    {
        return fail("EndScene vtable entry is null");
    }

    if (endScene == caveStub())
    { // a previous run left the hook in place
        originalEndScene_ = 0;
        return fail("EndScene already points at our stub; restart the client");
    }

    originalEndScene_ = endScene;

    const std::string stub = buildStub();

    if (stub.size() > kStubSize)
    {
        return fail("stub does not fit the reserved space");
    }

    const std::uint32_t zero = 0;

    if (!memory_.write<std::uint32_t>(caveFlag(), zero))
    {
        return fail("cannot write the request flag");
    }

    if (!memory_.write(caveName(), "og\0\0\0\0\0", kNameSize))
    {
        return fail("cannot write the script name");
    }

    if (!memory_.write(caveStub(), stub.data(), stub.size()))
    {
        return fail("cannot write the stub");
    }

    // Read the stub back: process_vm_writev can silently fail on a mapping we
    // may not write, and a half-written stub would crash the client.
    std::vector<std::uint8_t> verify(stub.size(), 0);

    if (!memory_.read(caveStub(), verify.data(), verify.size()) || std::memcmp(verify.data(), stub.data(), stub.size()) != 0)
    {
        return fail("stub did not read back; the cave is not writable");
    }

    // Last: point the client at the stub. Nothing before this changed behaviour.
    if (!memory_.write<std::uint32_t>(slot, caveStub()))
    {
        return fail("cannot write the vtable entry");
    }

    installed_ = true;
    error_.clear();
    return true;
}

void LuaExecutor::uninstall()
{
    if (!installed_)
    {
        return;
    }

    installed_ = false;

    if (!memory_.attached() || originalEndScene_ == 0)
    {
        return;
    }

    const std::uint32_t slot = vtable_ + static_cast<std::uint32_t>(layout_.d3dEndSceneIndex) * 4;

    // Only restore if it is still ours, so we never clobber another hook.
    if (memory_.readPointer(slot).value_or(0) != caveStub())
    {
        return;
    }

    if (!memory_.write<std::uint32_t>(slot, originalEndScene_))
    {
        return;
    }

    // Give a frame that already entered the stub time to leave it, then wipe the
    // caves so the next run finds the layout's caves free instead of searching
    // for another - a search that may land on data the client uses.
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
    const std::vector<std::uint8_t> code(layout_.codeCaveSize, 0);
    memory_.write(cave_, code.data(), code.size());

    if (separateData())
    {
        const std::vector<std::uint8_t> data(layout_.dataCaveSize, 0);
        memory_.write(data_, data.data(), data.size());
    }
}

bool LuaExecutor::idle() const
{
    if (!installed_)
    {
        return false;
    }

    return memory_.read<std::uint32_t>(caveFlag()).value_or(1) == 0;
}

bool LuaExecutor::execute(std::string_view script)
{
    if (!installed_)
    {
        return fail("Lua executor is not installed");
    }

    if (!supportsLua())
    {
        return fail("this client build cannot run Lua");
    }

    if (script.empty())
    {
        return fail("empty script");
    }

    if (script.size() > maxScriptLength())
    {
        return fail("script is longer than the code cave allows");
    }

    if (!idle())
    {
        return fail("the client has not run the previous script yet");
    }

    if (!memory_.write(caveScript(), script.data(), script.size()))
    {
        return fail("cannot write the script");
    }

    const std::uint8_t terminator = 0;

    if (!memory_.write(caveScript() + static_cast<std::uint32_t>(script.size()), &terminator, 1))
    {
        return fail("cannot terminate the script");
    }

    // Setting the flag last is what makes the request visible to the stub.
    if (!memory_.write<std::uint32_t>(caveFlag(), kRequestLua))
    {
        return fail("cannot raise the request flag");
    }

    error_.clear();
    return true;
}

bool LuaExecutor::call(std::uint32_t function, std::uint32_t thisPointer, std::uint32_t argument)
{
    if (!installed_)
    {
        return fail("Lua executor is not installed");
    }

    if (function == 0 || thisPointer == 0)
    {
        return fail("no function or object to call");
    }

    if (!idle())
    {
        return fail("the client has not run the previous request yet");
    }

    const std::array<std::uint32_t, 3> request{function, thisPointer, argument};

    if (!memory_.write(caveCall(), request.data(), sizeof(request)))
    {
        return fail("cannot write the call");
    }

    if (!memory_.write<std::uint32_t>(caveFlag(), kRequestCall))
    {
        return fail("cannot raise the request flag");
    }

    error_.clear();
    return true;
}

} // namespace og::wowclient
