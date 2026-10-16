# OpenGlider

OpenGlider is a modular bot engine for World of Warcraft clients, written in
C++20.

## Modules

Each directory is a static library, `og_<module>`. A module only sees the
modules it lists as dependencies:

| Module       | What it holds                                                  |
|--------------|----------------------------------------------------------------|
| `common`     | math, clock, logging, events, JSON, blacklists                 |
| `state`      | `GameSnapshot`, `IGameStateProvider`, a mock provider          |
| `input`      | `IInputBackend` and its backends                               |
| `actions`    | semantic actions mapped to input                               |
| `navigation` | routes, the movement controller, stuck handling, client maps   |
| `targets`    | target filtering and scoring                                   |
| `profiles`   | Glider profiles: loader, validator, recorder                   |
| `combat`     | the combat class interface and the OGScript language           |
| `core`       | `BotEngine`, behaviors, the objective stack, `BotInstance`     |
| `tasks`      | task plans and the plan executor                               |
| `world`      | the quest database, quest givers, clustering and scoring       |
| `ai`         | strategic and tactical planners                                |
| `replay`     | snapshot recording and a replay provider                       |
| `sim`        | a simulated world for offline testing                          |
| `wowclient`  | reading the client's memory (1.12.1 and 3.3.5a layouts)        |
| `minimap`    | the client's minimap tiles (with StormLib or CascLib)          |

Headers are included as `"og/<module>/Header.h"`, so the repository is checked
out as a directory named `og`, for example as a `src/og` submodule.

## Building

`CMakeLists.txt` adds the modules to a parent CMake project, which provides
`og_add_module` and the dependencies: yaml-cpp, Recast/Detour, SQLite, OpenSSL,
and optionally StormLib or CascLib. The parent sets `OG_APP_NAME` before
`add_subdirectory(og)` to name its config and runtime directories
(`~/.config/<app>`, `%APPDATA%/<App>`); it is `OpenGlider` otherwise.

Client layouts other than 1.12.1 and 3.3.5a are not part of OpenGlider. A build
that has them adds their sources to `og_wowclient` and defines
`OG_EMBED_MODERN_LAYOUTS`; otherwise they can be registered at run time from
JSON (`registerLayout`).

## License

Copyright (c) 2026 Alexander Blohmè.

OpenGlider is free software: you can redistribute it and/or modify it under the
terms of the GNU Lesser General Public License as published by the Free
Software Foundation, either version 3 of the License, or (at your option) any
later version. See [COPYING.LESSER](COPYING.LESSER) and [COPYING](COPYING).
