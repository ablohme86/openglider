# Building OpenGlider as a library

OpenGlider builds into one shared library: `libopenglider.so` on Linux,
`OpenGlider.dll` on Windows. Your own programs can build against its headers
and run with it, and you can change OpenGlider and build it again for them.

This guide covers how to build it, how a program runs with a build, which
changes a program built against the headers survives, and why.

> **AllGlider** has the engine compiled into it and runs with no other: it
> refuses an OpenGlider library that is not its own build. This guide is for
> programs of your own.

- [Quick start](#quick-start)
- [Running a program with your build](#running-a-program-with-your-build)
- [Which changes work](#which-changes-work)
- [Where things are](#where-things-are)
- [Recipes](#recipes)
- [How it works](#how-it-works)
- [When something goes wrong](#when-something-goes-wrong)
- [For OpenGlider maintainers](#for-openglider-maintainers)
- [License](#license)

## Quick start

A program built against these headers runs with a library built from the same
OpenGlider version. A newer or older checkout may not fit it; see
[Which changes work](#which-changes-work).

### Linux

You need CMake 3.24 or newer and GCC or Clang with C++20. Dependencies that
are missing from the system (SQLite, yaml-cpp, StormLib, CascLib,
Recast/Detour) are downloaded and built by CMake.

```sh
git clone https://github.com/ablohme86/openglider
cd openglider
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build --parallel
```

The library is `build/lib/libopenglider.so`. The checkout directory can have
any name.

### Windows

You need Visual Studio 2022 with *Desktop development with C++* (it comes with
CMake), building for x64 in Release, and **Developer Mode** turned on
(*Settings → System → For developers*). The build links an `og` directory to
the sources, and Windows only allows that with Developer Mode on.

```bat
git clone https://github.com/ablohme86/openglider
cd openglider
cmake -S . -B build
cmake --build build --config Release --parallel
```

The library is `build\lib\Release\OpenGlider.dll`.

Build the library and the program with the same compiler family: on Windows,
C++ libraries built by different compilers (MSVC and MinGW, say) cannot be
mixed. On Linux, GCC and Clang with libstdc++ mix.

## Running a program with your build

Link your program against the library with CMake: add OpenGlider with
`add_subdirectory()` and `OG_SHARED_ENGINE=ON` (see
[The build on its own](#the-build-on-its-own)), and link `openglider`. The
program finds the library by its RUNPATH, by the system's library path, or,
on Windows, beside the `.exe`.

To try another build without relinking, put it where the program looks first.
On Linux:

```sh
LD_LIBRARY_PATH=/path/to/openglider/build/lib ./my-program
```

A program should check at start that the library fits it, with `checkEngine()`
from `abi/EngineAbi.h`:

```cpp
#include "og/abi/EngineAbi.h"

int main()
{
    if (const auto engine = og::checkEngine(""); !engine.compatible)
    {
        std::fprintf(stderr, "%s\n", engine.message.c_str());
        return 1; // it would crash or misbehave
    }
    // ...
}
```

`checkEngine()` also says whether the library is the build the program came
with (`custom`), when the program passes the build id it was built with
(`OG_ENGINE_BUILD_ID`). What to do with another build is the program's choice:
run with it, say so, or refuse it.

## Which changes work

The program and the engine share C++ types. The program creates them, copies
them and reads their fields, using the *headers it was built with*. Your
library has to agree with those headers wherever the two meet. Code inside
the library, behind those headers, is yours to change.

### Works

| Change | Example |
|---|---|
| The body of any function in a `.cpp` file | How `DeathBehavior` picks a safe spot; how `MovementController` turns |
| New functions, classes and files used inside the engine | A helper in `navigation/`, a new module |
| A new behavior, registered in `createDefaultBehaviors()` | "Look around after a kill" |
| New fields in classes only the engine creates | A member of `Humanizer`, of a behavior class |
| New OGScript functions or members | Another `globalFunctions()` entry in `SnapshotEnvironment.cpp` |
| Behavior priorities (`core/IBotBehavior.h`, `priority::`) | Rest before Gather |
| New log lines, log categories, messages | |

### Refused at start

The program checks a fingerprint of the sizes of the types it shares with the
engine (`abi/EngineAbi.h`). Any change that alters one of them makes the
library not fit, and the program refuses it.

| Change | Why |
|---|---|
| A field added to, removed from or retyped in `GameSnapshot`, `PlayerState`, `UnitState`, `GameObjectState` | The program reads snapshots, for its radar for one |
| The same in `BotConfig` or any config struct inside it (`HumanizerConfig`, `RoamConfig`, ...) | The program fills the config from its settings |
| The same in `BotContext`, `BotEngine`, `BotInstance`, `InstanceStatus` | The program creates the bot and reads its status |
| The same in `Navigator`, `MovementController`, `ActionService`, ... | They are members of `BotInstance`, so its size changes |
| The same in `GliderProfile`, `ProfileEditor`, `QuestSource`, `ClientLayout`, `ModernLayout`, `WowMemoryStateProvider` | The program uses them directly |

This applies to private fields as well. The program allocates these objects,
so it has to know their size.

A common case: a new behavior needs state that several behaviors share, and
the obvious home for it is a new field in `BotContext`. That is refused. Keep
the state in the behavior object, or in a class of your own that the engine
owns.

### Not caught, and wrong

These changes pass the check but break at run time.

| Change | What happens |
|---|---|
| Removing, renaming or changing the parameters of a function the program calls | The system's loader stops the program with `undefined symbol: ...`, at start or when the function is first called |
| A virtual function added to, removed from or moved in a class the program uses or implements | The two sides disagree about the object's layout: crashes or wrong calls |
| Fields of the same size swapped or retyped (a `float` for an `int`) | The program reads the wrong values |
| A layout change to a shared type that is not in the fingerprint | As above. The fingerprint covers the main types, not all of them |

### No effect, without a warning

| Change | Why |
|---|---|
| A default value in a header (`float runSpeed = 7.0f;` in `MovementConfig`, `Duration jumpMinInterval{20000};` in `HumanizerConfig`) | The program builds these structs itself, with its own compiled-in defaults, and fills them from its settings |
| The body of an inline function in a header, as the program calls it | The program has its own compiled copy. Your change only applies where the engine calls it |
| A `constexpr` constant in a header that the program uses | Same reason |

Make such changes where the value is used, in the `.cpp` file.

### In short

> Change **how** things are done (`.cpp` files) and **add** new things freely.
> Do not change the **shape** of types in headers (fields, virtual functions)
> or the **signatures** of functions declared in them.

## Where things are

Each directory is one module. A module only uses the modules above it in this
list.

| Module | What it holds |
|---|---|
| `common` | Math, clock, logging, events, JSON, SQLite, blacklists, file lookup |
| `state` | `GameSnapshot` and the unit, player and object states; `IGameStateProvider` |
| `input` | Keyboard and mouse backends: X11, Win32, the Wine input bridge, virtual |
| `actions` | Semantic actions mapped to keys, cooldowns, spell costs |
| `navigation` | Routes, `Navigator`, `MovementController`, stuck detection and recovery, obstacle memory, client-map pathfinding |
| `targets` | `TargetSelector`: which mob to pull |
| `profiles` | Profiles: loading, validation, saving, `ProfileEditor` |
| `combat` | The combat class interface and OGScript (parser, validator, interpreter) |
| `core` | `BotEngine`, `BotInstance`, `BotContext`, the behaviors, `Humanizer`, playstyle |
| `tasks` | Task plans and the plan executor |
| `world` | The quest database, quest givers, quest scoring, zones |
| `ai` | Strategic and tactical planners |
| `replay` | Snapshot recording and replay |
| `sim` | A simulated world for offline testing |
| `wowclient` | Reading the client's memory: the legacy layouts, the 64-bit reader, layout JSON |
| `minimap` | The client's minimap tiles (StormLib, CascLib) |
| `abi` | Whether the engine running fits the program |

### What to change for what

| You want to change | Look in |
|---|---|
| Which behavior runs when | `core/IBotBehavior.h` (`priority::`), each behavior's `canRun()` |
| Fighting | `core/behaviors/CombatBehavior.cpp`; the class scripts themselves are `.ogc` files, see below |
| Which mobs are pulled | `targets/TargetSelector.cpp` |
| Looting and skinning | `core/behaviors/LootBehavior.cpp` |
| Resting, eating, drinking | `RestBehavior` in `core/behaviors/SimpleBehaviors.cpp` |
| Dying, the corpse run, resurrecting | `core/behaviors/DeathBehavior.cpp`, `ResurrectEscapeBehavior.cpp` |
| Walking a route, roaming | `NavigationBehavior` in `SimpleBehaviors.cpp`, `navigation/Navigator.cpp`, `navigation/Roaming.cpp` |
| Turning, running, how keys are pressed | `navigation/MovementController.cpp` |
| Getting unstuck | `navigation/StuckDetector.cpp`, `navigation/StuckRecovery.cpp`, `navigation/ObstacleMemory.cpp` |
| Random jumps, strafes, pauses | `core/Humanizer.cpp` |
| Herbs and ore | `core/behaviors/GatherBehavior.cpp` |
| Vendors, trainers, quests | `VendorBehavior.cpp`, `TrainerBehavior.cpp`, `QuestHandInBehavior.cpp` in `core/behaviors/` |
| Campfires, fires | `CampfireBehavior` and `LeaveFireBehavior` in `SimpleBehaviors.cpp` |
| What is read from the client | `wowclient/WowMemoryStateProvider.cpp`, `wowclient/ModernClientReader.cpp` |

Much can be changed without building anything: combat classes are OGScript
(`.ogc` files), and profiles and action metadata (`actions.yaml`) are data
files the program reads at run time.

## Recipes

### Change what a behavior does

Edit its `.cpp` file, build, and replace the library. As an example, to make
the ghost resurrect even closer to mobs, change the radius test in
`DeathBehavior::tick` (`core/behaviors/DeathBehavior.cpp`). Leave the config
fields as they are: `BotConfig` is shared.

### Add a behavior

Declare it in `core/behaviors/Behaviors.h`. A new class does not change any
existing type.

```cpp
// 590: a look round after a kill, out of combat.
class LookAroundBehavior final : public IBotBehavior
{
public:
    std::string_view name() const override
    {
        return "LookAround";
    }

    int priority() const override
    {
        return 590;
    }

    bool canRun(const GameSnapshot& s, BotContext& ctx) override;
    BehaviorStatus tick(const GameSnapshot& s, BotContext& ctx) override;

private:
    // The behavior's own state: free to grow, the engine alone creates it.
    std::optional<TimePoint> until_;
};
```

Write `canRun` and `tick` in a `.cpp` file (`SimpleBehaviors.cpp`, or a new
file listed in `core/CMakeLists.txt`). Register the behavior in
`createDefaultBehaviors()`:

```cpp
out.push_back(std::make_unique<LookAroundBehavior>());
```

Through `BotContext` a behavior has the actions (`ctx.actions`), movement
(`ctx.movement`, `ctx.navigator`), targets, the log and the config. It can
read them all. It must not add fields to `BotContext`.

### Add an OGScript function

`SnapshotEnvironment.cpp` lists the names a script can use
(`globalFunctions()`, `unitMembers()`, `playerMembers()`, ...) and evaluates
them. Add the name to its list and handle it where the existing ones (`ready`,
`cooldown`, `enemiesNear`) are handled. The program's script editor validates
scripts through the engine (`ScriptValidator`), so it accepts the new name
too.

### Tune movement

Change the code in `navigation/MovementController.cpp`, or the constants it
uses there. A new value in `MovementConfig` would be refused, and a new
default there would have no effect.

## How it works

### The ABI check

`abi/EngineAbi.h` makes a fingerprint of the size and alignment of the types
a program shares with the engine, plus `kEngineAbiVersion`.

- **The library** works it out when it is built, from the headers it is built
  with, and reports it through `engineInfo()`.
- **The program** works it out when *it* is built, from *its* headers, and
  compares the two at start with `checkEngine()`.

Both values are computed at compile time. That matters on Linux: a call to an
inline function from inside a shared library can be bound to the program's
copy of that function (symbol interposition). A fingerprint computed at run
time could then come out the same on both sides however different the headers
were.

The check catches size changes. It cannot catch a virtual function added at
the end of a class, two fields swapped, or a type the fingerprint does not
list. That is why the rules above matter.

### Custom builds

A program built together with an engine gives both the same build id
(`OG_ENGINE_BUILD_ID`). A library built on its own has none. `checkEngine()`
compares the two and calls a library that fits but is not the program's own
`custom`. The build id says which build a library is; it does not
authenticate it.

### Names and data the program supplies

The engine looks for its files in the user's config directory, beside the
program, and in a data directory. The program names these at start with
`setApplicationName()` (AllGlider's settings live in `~/.config/allglider/`,
not `~/.config/openglider/`) and `setDataDirectory()`. A library built on its
own does not know them.

### Client layouts

OpenGlider reads the 3.3.5a and 1.12.1 clients with layouts it includes. The
64-bit clients' layouts (memory offsets) are not part of OpenGlider. A program
supplies them, either from its license server at run time (`registerLayout()`)
or compiled into its development builds (`addCompiledLayout()`). An engine you
build reads whatever layouts the program gives it.

### Exported symbols

On Linux the engine is compiled with default symbol visibility, including
inline functions, even when the program around it hides its own symbols. On
Windows, MSVC exports nothing by default, so the build exports every function
and class (`WINDOWS_EXPORT_ALL_SYMBOLS`). That cannot export data symbols, and
the engine therefore keeps no global variables or static data members that a
program reaches into directly. Keep it that way.

### The build on its own

A checkout's sources include `og/<module>/...`. Built on its own, the build
directory gets an `include/og` link to the checkout, which is why the
directory name does not matter and why Windows needs Developer Mode. Added to
a program with `add_subdirectory()`, the program includes
`cmake/OpenGliderHelpers.cmake` and `cmake/OpenGliderDependencies.cmake` first
and sets `OG_INCLUDE_ROOT`. The modules are then static libraries, or the same
shared library with `OG_SHARED_ENGINE=ON`.

## When something goes wrong

| What you see | Meaning |
|---|---|
| `the OpenGlider engine in ... was built from other OpenGlider headers than this program` | A shared type's size differs. Build from the program's OpenGlider version, without header changes to shared types |
| `undefined symbol: _ZN2og...` (Linux) or "entry point not found" (Windows) | Your library lacks a function the program calls: removed, renamed or with other parameters. `c++filt` turns the name into the function |
| The program crashes soon after start, with no message | Most likely a change the check does not see: a virtual function, swapped fields. Undo header changes one at a time |
| Your change does nothing | It went into a header default or an inline function the program has its own copy of; or the program loaded another library. On Linux, `ldd ./my-program \| grep openglider` shows which |
| `cannot link .../include/og to the sources` (Windows) | Turn on Developer Mode |

For a debugger, build your library with `-DCMAKE_BUILD_TYPE=RelWithDebInfo` and
attach to the program. On Linux, `gdb` with `LD_LIBRARY_PATH` set as above
loads your library's symbols.

## For OpenGlider maintainers

- **Raise `kEngineAbiVersion`** (`abi/EngineAbi.h`) on any change a program
  must be rebuilt for that the sizes do not show: a virtual function added or
  removed, fields reordered, a function's signature changed.
- **Add a type to `engineAbiFingerprint()`** when programs start to share it.
- **Keep global and static member data out of the engine's API.** Windows
  cannot export it.
- **Changing what programs share costs every user's build.** A type that
  changes often is better hidden behind a pointer to its implementation
  (pimpl), with only functions in its header.

## License

OpenGlider is under the GNU Lesser General Public License, version 3 or later
([COPYING.LESSER](../COPYING.LESSER), [COPYING](../COPYING)). You may change
it and use your build with a program that uses the library. If you distribute
a modified OpenGlider, its source goes with it under the same license.
