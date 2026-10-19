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
 *  File: ProcessMemory.cpp
 *
 *  Desc:
 *  Implementation of ProcessMemory (WoW client memory reading, process
 *  inspection, and layout definitions).
 *
 *
 */
#include "og/wowclient/ProcessMemory.h"

#include <algorithm>
#include <array>
#include <cctype>
#include <cstring>
#include <format>

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
// windows.h must come first
#include <tlhelp32.h>
#else
#include <sys/uio.h>
#include <fcntl.h>
#include <signal.h>
#include <unistd.h>

#include <cerrno>
#include <cstring>
#include <filesystem>
#include <fstream>
#endif

namespace og::wowclient
{

namespace
{
// Where a client's main module loads: WoW.exe at 0x400000, the 64-bit
// WoWClassic.exe at 0x140000000.
constexpr std::array<std::uint64_t, 2> kImageBases{0x400000, 0x140000000};

std::string lower(std::string_view s)
{
    std::string out(s);
    std::transform(out.begin(), out.end(), out.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return out;
}

} // namespace

ProcessMemory::~ProcessMemory()
{
    detach();
}

std::string IProcessMemory::readString(std::uint64_t address, std::size_t maxLength) const
{
    std::string out;

    if (address == 0)
    {
        return out;
    }

    std::vector<char> buf(maxLength + 1, '\0');

    // Read in one go; if that fails (page boundary), fall back to small chunks.
    if (!read(address, buf.data(), maxLength))
    {
        for (std::size_t i = 0; i < maxLength; ++i)
        {
            char c = 0;

            if (!read(address + i, &c, 1) || c == '\0')
            {
                break;
            }

            out += c;
        }

        return out;
    }

    out.assign(buf.data(), strnlen(buf.data(), maxLength));
    return out;
}

#ifdef _WIN32

std::vector<ProcessMemory::ProcessInfo> ProcessMemory::findProcesses(std::string_view nameContains)
{
    std::vector<ProcessInfo> out;
    const std::string needle = lower(nameContains);
    HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);

    if (snap == INVALID_HANDLE_VALUE)
    {
        return out;
    }

    PROCESSENTRY32 pe{};
    pe.dwSize = sizeof(pe);

    if (Process32First(snap, &pe))
    {
        do
        {
            const std::string name = pe.szExeFile;

            if (lower(name).find(needle) != std::string::npos)
            {
                out.push_back({static_cast<int>(pe.th32ProcessID), name, name});
            }

        } while (Process32Next(snap, &pe));
    }

    CloseHandle(snap);
    return out;
}

namespace
{

void enableDebugPrivilege()
{
    HANDLE token = nullptr;
    if (OpenProcessToken(GetCurrentProcess(), TOKEN_ADJUST_PRIVILEGES | TOKEN_QUERY, &token))
    {
        TOKEN_PRIVILEGES tp{};
        if (LookupPrivilegeValue(nullptr, SE_DEBUG_NAME, &tp.Privileges[0].Luid))
        {
            tp.PrivilegeCount = 1;
            tp.Privileges[0].Attributes = SE_PRIVILEGE_ENABLED;
            AdjustTokenPrivileges(token, FALSE, &tp, sizeof(tp), nullptr, nullptr);
        }
        CloseHandle(token);
    }
}

// The first module of a process's module list is its .exe. The snapshot can
// fail while the process is still loading its modules (ERROR_BAD_LENGTH):
// tried again a few times.
std::uint64_t snapshotModuleBase(DWORD pid)
{
    for (int attempt = 0; attempt < 5; ++attempt)
    {
        HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPMODULE | TH32CS_SNAPMODULE32, pid);

        if (snap == INVALID_HANDLE_VALUE)
        {
            if (GetLastError() == ERROR_BAD_LENGTH)
            {
                continue;
            }

            return 0;
        }

        MODULEENTRY32 me{};
        me.dwSize = sizeof(me);
        const bool found = Module32First(snap, &me);
        CloseHandle(snap);
        return found ? static_cast<std::uint64_t>(reinterpret_cast<std::uintptr_t>(me.modBaseAddr)) : 0;
    }

    return 0;
}

} // namespace

// Without the module list: the image in the address space that is no DLL.
std::uint64_t ProcessMemory::imageOfTheExe() const
{
    MEMORY_BASIC_INFORMATION info{};
    std::uint64_t address = 0;

    while (VirtualQueryEx(static_cast<HANDLE>(handle_), reinterpret_cast<LPCVOID>(address), &info, sizeof(info)) == sizeof(info))
    {
        const auto base = static_cast<std::uint64_t>(reinterpret_cast<std::uintptr_t>(info.BaseAddress));
        const auto size = static_cast<std::uint64_t>(info.RegionSize);

        if (size == 0)
        {
            break;
        }

        if (info.Type == MEM_IMAGE && info.BaseAddress == info.AllocationBase)
        {
            const auto mz = read<std::uint16_t>(base);
            const auto lfanew = read<std::uint32_t>(base + 0x3C);

            if (mz == 0x5A4D && lfanew && *lfanew < 0x1000 && read<std::uint32_t>(base + *lfanew) == 0x4550) // "MZ", "PE\0\0"
            {
                // COFF characteristics: IMAGE_FILE_DLL
                if ((read<std::uint16_t>(base + *lfanew + 22).value_or(IMAGE_FILE_DLL) & IMAGE_FILE_DLL) == 0)
                {
                    return base;
                }
            }
        }

        address = base + size;
    }

    return 0;
}

bool ProcessMemory::attach(int pid)
{
    detach();
    enableDebugPrivilege();

    const DWORD processId = static_cast<DWORD>(pid);
    HANDLE h = OpenProcess(PROCESS_VM_READ | PROCESS_QUERY_INFORMATION, FALSE, processId);

    if (!h)
    {
        const DWORD firstError = GetLastError();

        h = OpenProcess(PROCESS_VM_READ | PROCESS_QUERY_LIMITED_INFORMATION, FALSE, processId);

        if (!h)
        {
            const DWORD fallbackError = GetLastError();

            // Try with only PROCESS_VM_READ
            h = OpenProcess(PROCESS_VM_READ, FALSE, processId);

            if (!h)
            {
                const DWORD vmReadError = GetLastError();

                // Diagnose: kan vi hente grunnleggende prosessinformasjon?
                HANDLE probe = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, processId);
                const DWORD queryError = probe ? ERROR_SUCCESS : GetLastError();

                if (probe)
                {
                    CloseHandle(probe);
                }

                lastError_ = std::format(
                    "OpenProcess({}): read+query={}, read+limited={}, read-only={}, limited-only={}",
                    pid, firstError, fallbackError, vmReadError, queryError);
                return false;
            }
        }
    }

    handle_ = h;
    pid_ = pid;
    // A 64-bit client is loaded where ASLR puts it, not at its own base.
    moduleBase_ = snapshotModuleBase(static_cast<DWORD>(pid));

    if (moduleBase_ == 0)
    {
        moduleBase_ = imageOfTheExe();
    }

    if (moduleBase_ != 0 && read<std::uint16_t>(moduleBase_) == 0x5A4D)
    {
        return true;
    }

    for (const std::uint64_t base : kImageBases)
    {
        char mz[2] = {};

        if (read(base, mz, 2) && mz[0] == 'M' && mz[1] == 'Z')
        {
            moduleBase_ = base;
            return true;
        }
    }

    const std::uint64_t seen = moduleBase_;
    CloseHandle(static_cast<HANDLE>(handle_));
    handle_ = nullptr;
    pid_ = -1;
    moduleBase_ = 0;
    lastError_ = seen ? std::format("process {} has no MZ header at its module base 0x{:X}, 0x400000 or 0x140000000", pid, seen) : std::format("process {} has no MZ header at 0x400000 or 0x140000000, and its module base could not be read", pid);
    return false;
}

void ProcessMemory::detach()
{
    if (handle_)
    {
        CloseHandle(static_cast<HANDLE>(handle_));
    }

    handle_ = nullptr;
    pid_ = -1;
    moduleBase_ = 0;
}

bool ProcessMemory::exists(int pid)
{
    if (pid <= 0)
    {
        return false;
    }

    HANDLE process = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, static_cast<DWORD>(pid));

    // Running, but not ours to look at.
    if (!process)
    {
        return GetLastError() == ERROR_ACCESS_DENIED;
    }

    DWORD code = 0;
    const bool running = GetExitCodeProcess(process, &code) && code == STILL_ACTIVE;
    CloseHandle(process);
    return running;
}

bool ProcessMemory::isAlive() const
{
    if (!handle_)
    {
        return false;
    }

    DWORD code = 0;
    return GetExitCodeProcess(static_cast<HANDLE>(handle_), &code) && code == STILL_ACTIVE;
}

bool ProcessMemory::read(std::uint64_t address, void* buffer, std::size_t size) const
{
    if (!handle_ || address == 0)
    {
        return false;
    }

    SIZE_T got = 0;

    if (!ReadProcessMemory(static_cast<HANDLE>(handle_), reinterpret_cast<LPCVOID>(address), buffer, size, &got) || got != size)
    {
        lastError_ = std::format("ReadProcessMemory(0x{:X}, {}) failed", address, size);
        return false;
    }

    return true;
}

bool ProcessMemory::writeMemory(std::uint64_t address, const void* buffer, std::size_t size) const
{
    if (!handle_ || address == 0)
    {
        return false;
    }

    SIZE_T written = 0;

    if (!WriteProcessMemory(static_cast<HANDLE>(handle_), reinterpret_cast<LPVOID>(address), buffer, size, &written) || written != size)
    {
        lastError_ = std::format("WriteProcessMemory(0x{:X}, {}) failed", address, size);
        return false;
    }

    return true;
}

// VirtualQueryEx walks the address space a region at a time.
std::vector<IProcessMemory::Region> ProcessMemory::regions() const
{
    std::vector<Region> out;

    if (!handle_)
    {
        return out;
    }

    constexpr DWORD kReadable = PAGE_READONLY | PAGE_READWRITE | PAGE_WRITECOPY | PAGE_EXECUTE_READ | PAGE_EXECUTE_READWRITE | PAGE_EXECUTE_WRITECOPY;
    constexpr DWORD kWritable = PAGE_READWRITE | PAGE_WRITECOPY | PAGE_EXECUTE_READWRITE | PAGE_EXECUTE_WRITECOPY;
    constexpr DWORD kExecutable = PAGE_EXECUTE | PAGE_EXECUTE_READ | PAGE_EXECUTE_READWRITE | PAGE_EXECUTE_WRITECOPY;

    MEMORY_BASIC_INFORMATION info{};
    std::uint64_t address = 0;

    while (VirtualQueryEx(static_cast<HANDLE>(handle_), reinterpret_cast<LPCVOID>(address), &info, sizeof(info)) == sizeof(info))
    {
        const auto base = static_cast<std::uint64_t>(reinterpret_cast<std::uintptr_t>(info.BaseAddress));
        const auto size = static_cast<std::uint64_t>(info.RegionSize);

        if (size == 0)
        {
            break;
        }

        // Guard pages fault on the first touch, so they are not readable to us.
        if (info.State == MEM_COMMIT && (info.Protect & PAGE_GUARD) == 0)
        {
            out.push_back(Region{base, base + size, (info.Protect & kReadable) != 0, (info.Protect & kWritable) != 0, (info.Protect & kExecutable) != 0});
        }

        address = base + size;
    }

    return out;
}

#else

static bool hasMzHeader(int pid)
{
    for (const std::uint64_t base : kImageBases)
    {
        char mz[2] = {};
        iovec local{mz, sizeof(mz)};
        iovec remote{reinterpret_cast<void*>(base), sizeof(mz)};

        if (::process_vm_readv(pid, &local, 1, &remote, 1, 0) == sizeof(mz) && mz[0] == 'M' && mz[1] == 'Z')
        {
            return true;
        }
    }

    return false;
}

std::vector<ProcessMemory::ProcessInfo> ProcessMemory::findProcesses(std::string_view nameContains)
{
    std::vector<ProcessInfo> out;
    const std::string needle = lower(nameContains);
    std::error_code ec;

    for (const auto& entry : std::filesystem::directory_iterator("/proc", ec))
    {
        const std::string pidStr = entry.path().filename().string();

        if (pidStr.empty() || !std::all_of(pidStr.begin(), pidStr.end(), ::isdigit))
        {
            continue;
        }

        std::string comm;
        std::ifstream(entry.path() / "comm") >> comm;
        const std::string lc = lower(comm);

        if (lc.find("wineserver") != std::string::npos)
        {
            continue;
        }

        // Require comm to match the target process name (e.g. "wow.exe")
        if (lc != needle && lc.find(needle) == std::string::npos)
        {
            continue;
        }

        const int pid = std::stoi(pidStr);

        // Verify that the process actually has a PE image where a client loads
        if (!hasMzHeader(pid))
        {
            continue;
        }

        std::ifstream cmdFile(entry.path() / "cmdline", std::ios::binary);
        std::string cmdline((std::istreambuf_iterator<char>(cmdFile)), std::istreambuf_iterator<char>());
        std::replace(cmdline.begin(), cmdline.end(), '\0', ' ');

        out.push_back({pid, comm, cmdline});
    }

    std::sort(out.begin(), out.end(), [](const ProcessInfo& a, const ProcessInfo& b) { return a.pid < b.pid; });
    return out;
}

bool ProcessMemory::attach(int pid)
{
    detach();

    if (::kill(pid, 0) != 0 && errno == ESRCH)
    {
        lastError_ = std::format("process {} does not exist", pid);
        return false;
    }

    pid_ = pid;
    // Probe the PE header of the main module.
    bool readable = false;

    for (const std::uint64_t base : kImageBases)
    {
        char mz[2] = {};

        if (!read(base, mz, 2))
        {
            continue;
        }

        readable = true;

        if (mz[0] == 'M' && mz[1] == 'Z')
        {
            return true;
        }
    }

    pid_ = -1;

    // An unreadable process keeps the read error, which says why (e.g. ptrace permission).
    if (readable)
    {
        lastError_ = std::format("process {} has no MZ header at 0x400000 or 0x140000000", pid);
    }

    return false;
}

void ProcessMemory::detach()
{
    pid_ = -1;

    if (memFd_ >= 0)
    {
        ::close(memFd_);
    }

    memFd_ = -1;
}

bool ProcessMemory::isAlive() const
{
    return pid_ > 0 && ::kill(pid_, 0) == 0;
}

bool ProcessMemory::exists(int pid)
{
    if (pid <= 0 || (::kill(pid, 0) != 0 && errno != EPERM))
    {
        return false;
    }

    // A zombie has exited; only its parent has yet to collect it. The state
    // follows the name in parentheses, which may itself hold spaces.
    std::ifstream stat("/proc/" + std::to_string(pid) + "/stat");
    std::string line;

    if (!std::getline(stat, line))
    {
        return true;
    }

    const auto close = line.rfind(')');
    return close == std::string::npos || close + 2 >= line.size() || line[close + 2] != 'Z';
}

bool ProcessMemory::read(std::uint64_t address, void* buffer, std::size_t size) const
{
    if (pid_ <= 0 || address == 0)
    {
        return false;
    }

    iovec local{buffer, size};
    iovec remote{reinterpret_cast<void*>(address), size};
    const ssize_t n = ::process_vm_readv(pid_, &local, 1, &remote, 1, 0);

    if (n == static_cast<ssize_t>(size))
    {
        return true;
    }

    if (n < 0 && errno == EPERM)
    {
        lastError_ = "permission denied - see ptrace_scope / CAP_SYS_PTRACE notes in docs/HOWTOUSE.md";
    }
    else
    {
        lastError_ = std::format("process_vm_readv(0x{:X}, {}) failed: {}", address, size, n < 0 ? std::strerror(errno) : "short read");
    }

    return false;
}

bool ProcessMemory::writeMemory(std::uint64_t address, const void* buffer, std::size_t size) const
{
    if (pid_ <= 0 || address == 0)
    {
        return false;
    }

    // /proc/<pid>/mem first: unlike process_vm_writev it ignores page
    // protection, which is what lets us patch a read-only page such as a
    // library's vtable or padding in a code section.
    if (memFd_ < 0)
    {
        memFd_ = ::open(std::format("/proc/{}/mem", pid_).c_str(), O_RDWR);
    }

    if (memFd_ >= 0)
    {
        const ssize_t n = ::pwrite(memFd_, buffer, size, static_cast<off_t>(address));

        if (n == static_cast<ssize_t>(size))
        {
            return true;
        }
    }

    iovec local{const_cast<void*>(buffer), size};
    iovec remote{reinterpret_cast<void*>(address), size};
    const ssize_t n = ::process_vm_writev(pid_, &local, 1, &remote, 1, 0);

    if (n == static_cast<ssize_t>(size))
    {
        return true;
    }

    if (n < 0 && errno == EPERM)
    {
        lastError_ = "permission denied - see ptrace_scope / CAP_SYS_PTRACE notes in docs/HOWTOUSE.md";
    }
    else
    {
        lastError_ = std::format("write(0x{:X}, {}) failed: {}", address, size, n < 0 ? std::strerror(errno) : "short write");
    }

    return false;
}

// /proc/<pid>/maps: "start-end perms offset dev inode path".
std::vector<IProcessMemory::Region> ProcessMemory::regions() const
{
    std::vector<Region> out;

    if (pid_ <= 0)
    {
        return out;
    }

    std::ifstream maps(std::format("/proc/{}/maps", pid_));
    std::string line;

    while (std::getline(maps, line))
    {
        std::uint64_t start = 0, end = 0;
        char perms[5] = {};

        if (std::sscanf(line.c_str(), "%llx-%llx %4s", reinterpret_cast<unsigned long long*>(&start), reinterpret_cast<unsigned long long*>(&end), perms) != 3)
        {
            continue;
        }

        out.push_back(Region{start, end, perms[0] == 'r', perms[1] == 'w', perms[2] == 'x'});
    }

    return out;
}

#endif

} // namespace og::wowclient
