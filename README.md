<div align="center">
  <img src="assets/icon.png" alt="OptiPiston icon" width="160">
  <h1>OptiPiston</h1>
  <p><strong>Smooth pistons, adjustable time.</strong></p>
  <p>A client-only LeviLamina mod for Minecraft Bedrock on Windows that smooths piston animation and adds singleplayer world speed control.</p>

  <p>
    <img src="https://img.shields.io/badge/version-v0.1.1-4c8bf5?style=flat-square" alt="OptiPiston v0.1.1">
    <img src="https://img.shields.io/badge/Minecraft%20Bedrock-Windows%20x64-62b47a?style=flat-square" alt="Minecraft Bedrock for Windows x64">
    <img src="https://img.shields.io/badge/LeviLamina-26.10%E2%80%9326.51-7b68ee?style=flat-square" alt="LeviLamina 26.10 to 26.51">
    <a href="LICENSE"><img src="https://img.shields.io/badge/license-AGPL--3.0-blue?style=flat-square" alt="AGPL-3.0 license"></a>
  </p>

  <p>
    <a href="#quick-start">Get started</a>
    ·
    <a href="https://github.com/wo55555/OptiPiston/releases">Releases</a>
    ·
    <a href="CHANGELOG.md">Changelog</a>
    ·
    <a href="https://github.com/wo55555/OptiPiston/issues">Report an issue</a>
    ·
    <a href="README_ZH.md">简体中文</a>
  </p>
</div>

> [!WARNING]
> OptiPiston is in early development. Configuration and API details may still change between releases.

OptiPiston redraws piston arms and the blocks they push on a smooth, configurable timeline. It only changes what the client renders: native piston progress, block states, and redstone timing stay untouched. In singleplayer it can also speed up or slow down the whole world, including sounds and particles.

## Quick Start

> [!IMPORTANT]
> Use a clean LeviLamina client instance when possible. Broad compatibility with other mods is not currently guaranteed.

### Install with LeviLauncher (recommended)

1. In LeviLauncher, select **Download**, find a Minecraft version listed in [Compatibility](#compatibility), and install it as an instance with the **LeviLamina** loader.
2. Select **Launch**, choose that instance, then select **lip** under **Content Download**.
3. Search for **OptiPiston** and open the package published by `wo55555`.
4. Choose the release whose **LL Requirement** and **Game Versions** match your instance, then select **Install** in that row. Lip does not pick a release from the installed LeviLamina version.
5. Launch the game and open a world. Piston animation is on by default; use `/optipiston status` to check the current state.

### Install with the Lip CLI

Run this from the instance root, with `<version>` and `<line>` taken from the release for your instance in [Compatibility](#compatibility):

```powershell
lip install github.com/wo55555/OptiPiston@<version>-mc<line>#client

# Example: Minecraft 26.20 / LeviLamina 26.20.*
lip install github.com/wo55555/OptiPiston@0.1.1-mc26.20#client
```

The `#client` variant is required.

### Manual Installation

Download `OptiPiston-client-windows-x64.zip` from the matching release, extract its `OptiPiston` directory into the instance's `mods` directory, and restart the client.

## Features

- **Smooth piston animation**: Arms and pushed blocks move over 2–4 game ticks, fractions included. Rendering only; game logic keeps its native timing.
- **Clean hand-off**: A pushed block stays drawn until its real block has been re-meshed, which avoids gaps, ghost copies, and overlap where it lands.
- **Stable heads**: Each cell draws exactly one piston head, and a piston that is itself being pushed follows its moving body.
- **World speed**: Runs a singleplayer world at 0.1x–10x, with optional sound pitch and particle scaling.
- **External clock API**: Replay and recording mods can drive the animation clock through a small C API, so pause, seek, and speed changes stay in sync.
- **Bilingual**: English and Simplified Chinese command output.

## Commands

| Command                               | Description                                                    |
| ------------------------------------- | -------------------------------------------------------------- |
| `/optipiston status`                  | Show piston animation state and requested/applied world speed. |
| `/optipiston piston on`               | Turn piston animation on.                                      |
| `/optipiston piston off`              | Turn piston animation off.                                     |
| `/optipiston piston duration <ticks>` | Set the animation length, from `2` to `4` game ticks.          |
| `/optipiston speed <value>`           | Set world speed, from `0.1` to `10`.                           |
| `/optipiston speed next`              | Switch to the next world speed preset.                         |
| `/optipiston speed prev`              | Switch to the previous world speed preset.                     |
| `/optipiston speed reset`             | Return to normal speed (`1x`).                                 |

Piston settings are saved to the configuration file. World speed applies only in a local world with no other players connected and while no other mod drives the clock; `/optipiston status` shows the reason when it is not applied.

## Configuration

The configuration lives at `mods/OptiPiston/config/config.json` and is kept across Lip updates.

```json
{
    "version": 1,
    "locateName": "zh_CN",
    "piston": {
        "enabled": true,
        "durationTicks": 4.0
    },
    "worldSpeed": {
        "presets": [0.1, 0.25, 0.5, 1.0, 2.0, 4.0],
        "audio": true,
        "particles": true
    },
    "command": {
        "enabled": true,
        "command": "optipiston"
    }
}
```

| Key                    | Description                                      |
| ---------------------- | ------------------------------------------------ |
| `piston.enabled`       | Turn piston animation on or off.                 |
| `piston.durationTicks` | Animation length in game ticks, from `2` to `4`. |
| `worldSpeed.presets`   | Speeds cycled by `speed next` / `speed prev`.    |
| `worldSpeed.audio`     | Scale sound playback with world speed.           |
| `worldSpeed.particles` | Scale particles with world speed.                |
| `command.command`      | Name of the client command.                      |

## For Mod Developers

OptiPiston exports a C API named `optipiston_get_api`. Include [`include/optipiston/api.hpp`](include/optipiston/api.hpp) and look it up at runtime, so OptiPiston stays an optional dependency:

```cpp
#include "optipiston/api.hpp"

if (auto const* api = optipiston::Api::find()) {
    optipiston::ExternalClockLease clock{api};
    // Bump epoch on every jump: seek, clock switch, or export start.
    clock.push(tick, epoch);
    api->set_external_partial(partialTick);
}
```

API v1 also reads and writes the piston toggle and duration. Setters clamp to the supported range and persist to the configuration.

## Compatibility

| Minecraft line | LeviLamina | Release                                                                               | Status                                |
| -------------- | ---------- | ------------------------------------------------------------------------------------- | ------------------------------------- |
| 26.10          | `26.10.*`  | [`v0.1.1-mc26.10`](https://github.com/wo55555/OptiPiston/releases/tag/v0.1.1-mc26.10) | Prerelease; not yet tested in game    |
| 26.20          | `26.20.*`  | [`v0.1.1-mc26.20`](https://github.com/wo55555/OptiPiston/releases/tag/v0.1.1-mc26.20) | Prerelease; tested in game on `0.1.0` |
| 26.30          | `26.32.*`  | [`v0.1.1-mc26.30`](https://github.com/wo55555/OptiPiston/releases/tag/v0.1.1-mc26.30) | Prerelease; not yet tested in game    |
| 26.40          | `26.40.*`  | [`v0.1.1-mc26.40`](https://github.com/wo55555/OptiPiston/releases/tag/v0.1.1-mc26.40) | Prerelease; not yet tested in game    |
| 26.50          | `26.51.*`  | [`v0.1.1-mc26.50`](https://github.com/wo55555/OptiPiston/releases/tag/v0.1.1-mc26.50) | Prerelease; not yet tested in game    |

Each line has its own release, tagged `v<version>-mc<line>` (for example `v0.1.1-mc26.20`), which only loads on the listed LeviLamina version. All currently published releases are prereleases. OptiPiston targets Minecraft Bedrock for Windows x64 and is distributed as a client-only mod.

## Build From Source

OptiPiston builds on Windows x64 with xmake, LLVM (clang-cl, CI uses LLVM 22), and Git. `--mc` selects the release line from [Compatibility](#compatibility): `26.10`, `26.20`, `26.30`, `26.40`, or `26.50`. The `26.10` build uses MSVC (Visual Studio 2022) instead, because the rapidjson bundled with LeviLamina 26.10 does not compile under current clang.

```powershell
xmake f -c -y -p windows -a x64 -m release --target_type=client --mc=<line>
xmake -y
xmake build optipiston-tests
xmake run optipiston-tests
```

For example, `--mc=26.20` builds the MC 26.20 / LeviLamina 26.20 line. The mod is written to `bin/OptiPiston/`; switching `--mc` needs a fresh configure (`-c`) and overwrites the previous build there.

### Packaging a Release

The mod version is `optipiston_version` in `xmake.lua` plus the line: `<version>-mc<line>`, for example `0.1.1-mc26.20`. Configuring rewrites the version and LeviLamina range in `tooth.json` to match `--mc`. Lip installs from the `tooth.json` at the release tag, so each line is tagged on its own commit that only changes `tooth.json`. The **Publish** workflow creates those commits:

1. Bump `optipiston_version` in `xmake.lua`, add a `## [<version>-mc<line>]` section to `CHANGELOG.md`, and push to `main`.
2. Run the workflow with the lines to release (all five by default):

   ```powershell
   gh workflow run publish.yml -f lines="<line> <line> ..."

   # Example: release only Minecraft 26.20
   gh workflow run publish.yml -f lines="26.20"
   ```

3. For each line it tags `v<version>-mc<line>` (for example `v0.1.1-mc26.20`), checks that `tooth.json` matches the tag, builds and tests the line, and publishes a prerelease with its notes from `CHANGELOG.md` and `OptiPiston-client-windows-x64.zip`.

## Known Limitations

- Only 26.20 has been tested in game. The other versions are ported from the LeviLamina headers and only verified to build.
- Outside 26.20, some hooks use different entry points: the frame counter follows `LevelRenderer::renderLevel`, and the arm's visual progress applies only while the arm is drawn. From 26.32 the camera no longer looks up a block per block actor, so the step that keeps a held moving block drawn after its real block lands is skipped. These paths may differ slightly at the start or end of a move.
- Tested in local worlds and Playback replays; multiplayer servers are not yet verified.
- Blocks moved by a piston can flicker darker while moving. Vanilla does the same; it is not fixed yet.
- World speed is singleplayer only and does not persist across restarts.

[Open an issue](https://github.com/wo55555/OptiPiston/issues) with logs and version details to report a reproducible problem.

## Acknowledgements

Special thanks to the [LeviLamina](https://github.com/LiteLDev/LeviLamina) maintainers and community for the native modding platform and tooling that make OptiPiston possible. OptiPiston grew out of the piston rendering work in [Playback](https://github.com/wo55555/Playback), and its core tests use [doctest](https://github.com/doctest/doctest).

## License

Copyright (C) 2026 [wo555](https://github.com/wo55555)

OptiPiston is released under the [GNU Affero General Public License v3.0](LICENSE). Distributed modifications must remain under AGPL-3.0 and provide their corresponding source code. Third-party components retain their own licenses.
