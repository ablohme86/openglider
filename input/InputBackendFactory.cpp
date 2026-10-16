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
 *  File: InputBackendFactory.cpp
 *
 *  Desc:
 *  Implementation of InputBackendFactory (low-level OS and Wine input
 *  simulation backends).
 *
 *
 */
#include "og/input/InputBackendFactory.h"
#include "og/common/FileUtil.h"

#include "og/input/VirtualInputBackend.h"
#include "og/input/WineBridgeProtocol.h" // the bridge's default port, asked for on every platform

#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <random>
#include <thread>

#ifdef _WIN32
#include "og/input/Win32InputBackend.h"
#else
#include "og/input/UInputBackend.h"
#include "og/input/WineBridgeInputBackend.h"

#include <arpa/inet.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>

#include <algorithm>
#include <cctype>
#include <csignal>
#include <fstream>
#include <string>
#include <string_view>
#endif
#ifdef OG_HAVE_X11
#include "og/input/X11InputBackend.h"
#endif

namespace og
{

#ifndef _WIN32
// Remembers the random port of the bridge the application started, so the next run,
// the GUI's picture of the game and bridge-ping find it while it is up.
static std::filesystem::path inputBridgePortFile()
{
    std::filesystem::path dir;

    if (const char* runtime = std::getenv("XDG_RUNTIME_DIR"); runtime && *runtime)
    {
        dir = std::filesystem::path(runtime) / applicationDirectoryName();
    }
    else
    {
        dir = std::filesystem::temp_directory_path() / (applicationDirectoryName() + "-" + std::to_string(getuid()));
    }

    return dir / "inputbridge.port";
}

static unsigned short rememberedInputBridgePort()
{
    const auto text = readTextFile(inputBridgePortFile());

    if (!text)
    {
        return 0;
    }

    const long port = std::strtol(text->c_str(), nullptr, 10);
    return port > 0 && port < 65536 ? static_cast<unsigned short>(port) : 0;
}

static void rememberInputBridgePort(unsigned short port)
{
    const auto file = inputBridgePortFile();
    std::error_code ec;
    std::filesystem::create_directories(file.parent_path(), ec);
    std::filesystem::permissions(file.parent_path(), std::filesystem::perms::owner_all, std::filesystem::perm_options::replace, ec);
    writeTextFile(file, std::to_string(port) + "\n");
}

// A random port that nothing on 127.0.0.1 listens on right now. Wine shares
// the host's network stack, so it is free for the bridge too. 0 if none.
static unsigned short randomFreePort()
{
    std::random_device seed;
    std::mt19937 rng(seed());
    std::uniform_int_distribution<int> pick(20000, 60999);

    for (int attempt = 0; attempt < 32; ++attempt)
    {
        const auto port = static_cast<unsigned short>(pick(rng));
        const int fd = ::socket(AF_INET, SOCK_STREAM, 0);

        if (fd < 0)
        {
            return 0;
        }

        sockaddr_in addr{};
        addr.sin_family = AF_INET;
        addr.sin_port = htons(port);
        addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        const bool free = ::bind(fd, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) == 0;
        ::close(fd);

        if (free)
        {
            return port;
        }
    }

    return 0;
}

// The bridges an earlier session left behind: every process of this user
// with og-inputbridge.exe as one of its arguments - the Wine process and the
// launchers around it (winbridge, umu-run, Proton) - except the one listening
// on `keepPort` (0: keep none). Each session started its own on a new port, so
// they add up.
void stopOtherInputBridges(unsigned short keepPort, std::string_view exeName)
{
    const auto isBridge = [exeName](std::string_view arg)
    {
        std::string lower(arg);

        for (char& c : lower)
        {
            c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
        }

        return lower.ends_with(exeName);
    };
    std::vector<pid_t> doomed;
    std::error_code ec;

    for (const auto& entry : std::filesystem::directory_iterator("/proc", ec))
    {
        const std::string name = entry.path().filename().string();

        if (name.empty() || !std::all_of(name.begin(), name.end(), ::isdigit))
        {
            continue;
        }

        const pid_t pid = static_cast<pid_t>(std::stol(name));

        if (pid == ::getpid())
        {
            continue;
        }

        struct stat st{};

        if (::stat(entry.path().c_str(), &st) != 0 || st.st_uid != ::getuid())
        {
            continue;
        }

        std::ifstream in(entry.path() / "cmdline", std::ios::binary);
        std::vector<std::string> args;

        for (std::string arg; std::getline(in, arg, '\0');)
        {
            args.push_back(arg);
        }

        if (std::none_of(args.begin(), args.end(), isBridge))
        {
            continue;
        }

        const auto port = std::find(args.begin(), args.end(), "--port");

        if (keepPort != 0 && port != args.end() && std::next(port) != args.end() && *std::next(port) == std::to_string(keepPort))
        {
            continue;
        }

        doomed.push_back(pid);
    }

    if (doomed.empty())
    {
        return;
    }

    for (const pid_t pid : doomed)
    {
        ::kill(pid, SIGTERM);
    }

    // Wine and Proton take a moment to go; what is left then is made to.
    std::this_thread::sleep_for(std::chrono::milliseconds(500));

    for (const pid_t pid : doomed)
    {
        if (::kill(pid, 0) == 0)
        {
            ::kill(pid, SIGKILL);
        }
    }
}

static bool trySpawnInputBridge(unsigned short port)
{
    // No bridge answered: whatever is left of old ones goes before a new one.
    stopOtherInputBridges(0);
    std::vector<std::filesystem::path> candidates;

    // Release packages ship the bridge beside the binary, and an AppImage
    // mounts somewhere different every run, so look there first.
    if (const auto exeDir = executableDirectory(); !exeDir.empty())
    {
        candidates.push_back(exeDir / "og-inputbridge.exe");
        candidates.push_back(exeDir / "tools" / "inputbridge" / "og-inputbridge.exe");
    }

    // Running out of a build tree.
    candidates.insert(candidates.end(), {
                                            "tools/inputbridge/og-inputbridge.exe",
                                            "../tools/inputbridge/og-inputbridge.exe",
                                            "../../tools/inputbridge/og-inputbridge.exe",
                                        });
    std::filesystem::path exePath;

    for (const auto& p : candidates)
    {
        std::error_code ec;

        if (std::filesystem::exists(p, ec))
        {
            exePath = std::filesystem::canonical(p, ec);
            break;
        }
    }

    if (exePath.empty())
    {
        return false;
    }

    // winbridge (a Wine launcher that picks the game's prefix) when there is one - the
    // user's own first - and plain wine otherwise.
    std::vector<std::filesystem::path> launchers;

    if (const char* home = std::getenv("HOME"); home && *home)
    {
        launchers.push_back(std::filesystem::path(home) / ".local" / "bin" / "winbridge");
    }

    launchers.insert(launchers.end(), {"/usr/local/bin/winbridge", "/usr/bin/winbridge", "/usr/local/bin/wine", "/usr/bin/wine"});
    std::string launcher;

    for (const auto& candidate : launchers)
    {
        std::error_code ec;

        if (std::filesystem::exists(candidate, ec))
        {
            launcher = candidate.string();
            break;
        }
    }

    if (launcher.empty())
    {
        return false;
    }

    const std::string portArg = std::to_string(port);

    pid_t pid = fork();

    if (pid < 0)
    {
        return false;
    }

    if (pid == 0)
    {
        setsid();
        pid_t pid2 = fork();

        if (pid2 == 0)
        {
            int nullFd = open("/dev/null", O_RDWR);

            if (nullFd >= 0)
            {
                dup2(nullFd, STDIN_FILENO);
                dup2(nullFd, STDOUT_FILENO);
                dup2(nullFd, STDERR_FILENO);
                close(nullFd);
            }

            // Wine's own fixme/err chatter has nowhere to go anyway.
            if (!std::getenv("WINEDEBUG"))
            {
                setenv("WINEDEBUG", "-all", 1);
            }

            // winbridge reads the options after the program as its own ("Unknown
            // option 'port'") and starts nothing: "--" ends its options.
            if (std::filesystem::path(launcher).filename() == "winbridge")
            {
                execl(launcher.c_str(), launcher.c_str(), "--", exePath.c_str(), "--port", portArg.c_str(), nullptr);
            }
            else
            {
                execl(launcher.c_str(), launcher.c_str(), exePath.c_str(), "--port", portArg.c_str(), nullptr);
            }

            _exit(1);
        }

        _exit(0);
    }

    waitpid(pid, nullptr, 0);
    return true;
}

// A bridge that answers, whether or not it sees the game yet: the one on the
// configured port, or with port 0 the remembered one, a hand-started one on the
// default port, or else a new one on a random port. Null when there is none.
static std::unique_ptr<WineBridgeInputBackend> connectInputBridge(const InputBackendConfig& config)
{
    std::vector<unsigned short> ports;

    if (config.port != 0)
    {
        ports.push_back(config.port);
    }
    else
    {
        if (const auto remembered = rememberedInputBridgePort(); remembered != 0)
        {
            ports.push_back(remembered);
        }

        ports.push_back(winebridge::kDefaultPort);
    }

    for (const auto port : ports)
    {
        auto bridge = std::make_unique<WineBridgeInputBackend>(config.host, port);

        if (bridge->connect() && bridge->answers())
        {
            stopOtherInputBridges(port);
            return bridge;
        }
    }

    const unsigned short port = config.port != 0 ? config.port : randomFreePort();

    if (port == 0 || !trySpawnInputBridge(port))
    {
        return nullptr;
    }

    if (config.port == 0)
    {
        rememberInputBridgePort(port);
    }

    auto bridge = std::make_unique<WineBridgeInputBackend>(config.host, port);

    for (int i = 0; i < 20; ++i)
    {
        std::this_thread::sleep_for(std::chrono::milliseconds(100));

        if (bridge->connect() && bridge->answers())
        {
            return bridge;
        }
    }

    return nullptr;
}

// Only whether something accepts connections: a bridge client says RELEASEALL
// on connecting, which would let go of the keys the bot is holding.
static bool somethingListensOn(unsigned short port)
{
    const int fd = ::socket(AF_INET, SOCK_STREAM, 0);

    if (fd < 0)
    {
        return false;
    }

    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_port = htons(port);
    addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    const bool listening = ::connect(fd, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) == 0;
    ::close(fd);
    return listening;
}
#endif

unsigned short knownInputBridgePort()
{
#ifndef _WIN32
    // The bridge quits on its own after a while and leaves the file behind.
    if (const auto remembered = rememberedInputBridgePort(); remembered != 0 && somethingListensOn(remembered))
    {
        return remembered;
    }
#endif
    return winebridge::kDefaultPort;
}

InputBridgeStatus ensureInputBridge()
{
    InputBridgeStatus status;
#ifdef _WIN32
    status.error = "not needed on Windows";
#else
    for (const unsigned short port : {rememberedInputBridgePort(), winebridge::kDefaultPort})
    {
        if (port != 0 && somethingListensOn(port))
        {
            // This one stays; any other left from an earlier session goes.
            stopOtherInputBridges(port);
            status.running = status.alreadyRunning = true;
            status.port = port;
            return status;
        }
    }

    const unsigned short port = randomFreePort();

    if (port == 0)
    {
        status.error = "no free port on 127.0.0.1";
        return status;
    }

    if (!trySpawnInputBridge(port))
    {
        status.error = "og-inputbridge.exe, or wine to run it with, was not found";
        return status;
    }

    rememberInputBridgePort(port);
    status.port = port;

    // Wine takes a moment to start, longer the first time in a prefix.
    for (int i = 0; i < 100 && !status.running; ++i)
    {
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
        status.running = somethingListensOn(port);
    }

    if (!status.running)
    {
        status.error = "og-inputbridge did not start listening on port " + std::to_string(port);
    }
#endif
    return status;
}

std::vector<std::string> availableInputBackends()
{
#ifdef _WIN32
    return {"win32", "virtual"};
#else
    std::vector<std::string> out{"winebridge", "uinput"};
#ifdef OG_HAVE_X11
    out.push_back("x11");
#endif
    out.push_back("virtual");
    return out;
#endif
}

std::unique_ptr<IInputBackend> createInputBackend(const InputBackendConfig& config, std::string* error)
{
    std::string type = config.type;

    if (type == "auto")
    {
#ifdef _WIN32
        type = "win32";
#else
        // Prefer winebridge if running or spawnable, otherwise use hardware-level uinput
        auto bridge = connectInputBridge(config);

        if (bridge && bridge->ping())
        {
            return bridge;
        }

        auto uinput = std::make_unique<UInputBackend>();

        if (uinput->isConnected())
        {
            return uinput;
        }

        if (bridge)
        {
            return bridge;
        }

        type = "winebridge";
#endif
    }

    if (type == "virtual")
    {
        return std::make_unique<VirtualInputBackend>();
    }
#ifdef _WIN32
    if (type == "win32")
    {
        return std::make_unique<Win32InputBackend>(config.processId);
    }
#else
    if (type == "uinput")
    {
        auto backend = std::make_unique<UInputBackend>();

        if (!backend->isConnected())
        {
            if (error)
            {
                *error = "cannot open /dev/uinput (permission denied or device missing)";
            }

            return nullptr;
        }

        return backend;
    }

    if (type == "winebridge")
    {
        auto backend = connectInputBridge(config);

        if (!backend || !backend->ping())
        {
            const std::string where = config.port != 0 ? config.host + ":" + std::to_string(config.port) : std::string("any port");

            if (error)
            {
                *error = backend ? "og-inputbridge is running but cannot find the WoW window" : "cannot connect to og-inputbridge on " + where + " (og-inputbridge.exe must run inside Wine for background input)";
            }

            return nullptr;
        }

        return backend;
    }
#endif
#ifdef OG_HAVE_X11
    if (type == "x11")
    {
        auto backend = std::make_unique<X11InputBackend>(config.windowTitle, config.windowId);

        if (!backend->isConnected())
        {
            if (error)
            {
                *error = "could not find window matching '" + (config.windowTitle.empty() ? "World of Warcraft" : config.windowTitle) + "'";
            }

            return nullptr;
        }

        return backend;
    }
#endif
    if (error)
    {
        *error = "input backend '" + type + "' is not available in this build";
    }

    return nullptr;
}

} // namespace og
