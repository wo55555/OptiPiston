<div align="center">
  <img src="assets/icon.png" alt="OptiPiston icon" width="160">
  <h1>OptiPiston</h1>
  <p><strong>Smooth pistons, adjustable time.</strong></p>
  <p>A client-only LeviLamina mod for Minecraft Bedrock on Windows that smooths piston animation and adds singleplayer world speed control.</p>

  <p>
    <img src="https://img.shields.io/badge/version-v0.1.0-4c8bf5?style=flat-square" alt="OptiPiston v0.1.0">
    <img src="https://img.shields.io/badge/Minecraft%20Bedrock-Windows%20x64-62b47a?style=flat-square" alt="Minecraft Bedrock for Windows x64">
    <img src="https://img.shields.io/badge/LeviLamina-26.20.*-7b68ee?style=flat-square" alt="LeviLamina 26.20">
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

1. Create or select a LeviLamina `26.20.*` client instance.
2. Install the OptiPiston `#client` release through LeviLauncher/Lip, or run this from the instance root:

   ```powershell
   lip install github.com/wo55555/OptiPiston@0.1.0-mc26.20#client
   ```

3. Launch the game and open a world. Piston animation is on by default; use `/optipiston status` to check the current state.

For manual installation, download `OptiPiston-client-windows-x64.zip` from the matching release, extract its `OptiPiston` directory into the instance's `mods` directory, and restart the client.

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

| Minecraft / LeviLamina | OptiPiston release | Status      |
| ---------------------- | ------------------ | ----------- |
| `26.20.*`              | `v0.1.0-mc26.20`   | Development |

OptiPiston targets Minecraft Bedrock for Windows x64 and is distributed as a client-only mod. Other Minecraft versions are not supported yet.

## Build From Source

OptiPiston builds on Windows x64 with xmake, LLVM (clang-cl, CI uses LLVM 22), and Git:

```powershell
xmake f -y -p windows -a x64 -m release --target_type=client --mc=26.20
xmake -y
xmake build optipiston-tests
xmake run optipiston-tests
```

The mod is written to `bin/OptiPiston/`.

## Known Limitations

- Only Minecraft 26.20 is supported.
- Tested in local worlds and Playback replays; multiplayer servers are not yet verified.
- Blocks moved by a piston can flicker darker while moving. Vanilla does the same; it is not fixed yet.
- World speed is singleplayer only and does not persist across restarts.

[Open an issue](https://github.com/wo55555/OptiPiston/issues) with logs and version details to report a reproducible problem.

## Acknowledgements

Special thanks to the [LeviLamina](https://github.com/LiteLDev/LeviLamina) maintainers and community for the native modding platform and tooling that make OptiPiston possible. OptiPiston grew out of the piston rendering work in [Playback](https://github.com/wo55555/Playback), and its core tests use [doctest](https://github.com/doctest/doctest).

## License

Copyright (C) 2026 [wo555](https://github.com/wo55555)

OptiPiston is released under the [GNU Affero General Public License v3.0](LICENSE). Distributed modifications must remain under AGPL-3.0 and provide their corresponding source code. Third-party components retain their own licenses.
