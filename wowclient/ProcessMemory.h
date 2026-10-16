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
 *  File: ProcessMemory.h
 *
 *  Desc:
 *  Declarations and interface for ProcessMemory (WoW client memory
 *  reading, process inspection, and layout definitions).
 *
 *
 */
#pragma once

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace og::wowclient
{

// Read access to the game's memory. ProcessMemory reads a real process; tests
// substitute an in-memory image.
class IProcessMemory
{
public:
    virtual ~IProcessMemory() = default;

    virtual bool attach(int pid) = 0;
    virtual void detach() = 0;
    virtual int pid() const = 0;
    virtual bool isAlive() const = 0;
    virtual bool read(std::uint64_t address, void* buffer, std::size_t size) const = 0;
    // Every write goes through here. Once locked nothing reaches the process,
    // whatever asked: a read-only client (2.5.6) is locked the moment it is
    // recognised, below all the checks the provider makes of its own.
    bool write(std::uint64_t address, const void* buffer, std::size_t size) const
    {
        return !writesLocked_ && writeMemory(address, buffer, size);
    }

    // For good: there is no unlocking.
    void lockWrites()
    {
        writesLocked_ = true;
    }

    bool writesLocked() const
    {
        return writesLocked_;
    }

    virtual const std::string& lastError() const = 0;

    // One mapped range of the target's address space.
    struct Region
    {
        std::uint64_t start = 0;
        std::uint64_t end = 0;
        bool readable = false;
        bool writable = false;
        bool executable = false;
    };
    // The target's memory map. Empty when the platform cannot report it, in
    // which case callers fall back to hard-coded addresses.
    virtual std::vector<Region> regions() const
    {
        return {};
    }

    // Where the process's main module (the .exe) is loaded, 0 when not known:
    // it is then looked for at the default bases (findImageBase). Windows
    // loads a 64-bit client wherever ASLR puts it (0x7FF6...); wine keeps the
    // image's own base.
    virtual std::uint64_t mainModuleBase() const
    {
        return 0;
    }

    bool attached() const
    {
        return pid() > 0;
    }

    template <class T> std::optional<T> read(std::uint64_t address) const
    {
        T value{};

        if (!read(address, &value, sizeof(T)))
        {
            return std::nullopt;
        }

        return value;
    }

    template <class T> bool write(std::uint64_t address, const T& value) const
    {
        return write(address, &value, sizeof(T));
    }

    // The supported clients are 32-bit: pointers are 4 bytes.
    std::optional<std::uint32_t> readPointer(std::uint64_t address) const
    {
        return read<std::uint32_t>(address);
    }

    std::string readString(std::uint64_t address, std::size_t maxLength = 64) const;

protected:
    virtual bool writeMemory(std::uint64_t address, const void* buffer, std::size_t size) const = 0;

private:
    bool writesLocked_ = false;
};

// Read-only access to another process' memory.
//
// Linux: process_vm_readv on the wine process hosting WoW.exe. Wine maps the
// PE image at its preferred base, so the client's 32-bit addresses can be used
// as-is. Requires ptrace permission: run the engine as the parent of wine
// (launch the game from it), give the binary CAP_SYS_PTRACE, or set
// /proc/sys/kernel/yama/ptrace_scope to 0.
//
// Windows: OpenProcess + ReadProcessMemory.
class ProcessMemory final : public IProcessMemory
{
public:
    struct ProcessInfo
    {
        int pid = 0;
        std::string name;
        std::string commandLine;
    };

    ProcessMemory() = default;
    ~ProcessMemory() override;
    ProcessMemory(const ProcessMemory&) = delete;
    ProcessMemory& operator=(const ProcessMemory&) = delete;

    // Case-insensitive match on the process name / command line.
    static std::vector<ProcessInfo> findProcesses(std::string_view nameContains = "wow.exe");
    // Whether a process with this id is still running (an exited one that has
    // not been reaped yet counts as gone). Needs no attach.
    static bool exists(int pid);

    bool attach(int pid) override;
    void detach() override;
    int pid() const override
    {
        return pid_;
    }

    bool isAlive() const override;

    using IProcessMemory::read;
    using IProcessMemory::write;
    bool read(std::uint64_t address, void* buffer, std::size_t size) const override;
    std::vector<Region> regions() const override;

    std::uint64_t mainModuleBase() const override
    {
        return moduleBase_;
    }

    const std::string& lastError() const override
    {
        return lastError_;
    }

protected:
    bool writeMemory(std::uint64_t address, const void* buffer, std::size_t size) const override;

private:
#ifdef _WIN32
    std::uint64_t imageOfTheExe() const;
#endif

    int pid_ = -1;
    void* handle_ = nullptr; // Windows process handle
    std::uint64_t moduleBase_ = 0; // Windows: the .exe's base, read at attach
    mutable int memFd_ = -1; // /proc/<pid>/mem, opened on the first write
    mutable std::string lastError_;
};

} // namespace og::wowclient
