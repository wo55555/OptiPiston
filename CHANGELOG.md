# Changelog

All notable changes to this project will be documented in this file.

The format is based on [Keep a Changelog](https://keepachangelog.com/en/1.0.0/),
and this project adheres to [Semantic Versioning](https://semver.org/spec/v2.0.0.html).

## [Unreleased]

### Added

- A `piston.turn` option and `/optipiston piston turn diagonal|corner` command for a block that another piston pushes before its first push has finished. `Corner` (default) speeds the block up to finish its first push, then turns at a right angle. `Diagonal` plays both pushes at once, so the block cuts the corner. It lands at the same time either way.
- A `piston.light` option and `/optipiston piston light on|off` command for the pushed-block light fixes below, on by default.

### Fixed

- A pushed block no longer looks darker or brighter while it moves than once it lands, with Fancy and Vibrant Visuals graphics alike.
- While a block is pushed, its cell now blocks and gives off light like the block itself instead of letting all light through and giving none: a pushed stone shades its surroundings from the start of the push, glass still lets light through, and a pushed glowstone keeps lighting the area. This covers the world light used by Simple and Fancy graphics and the light of Vibrant Visuals. Tested in game on 26.30 and 26.50; the other lines build and pass unit tests.

## [0.3.1] - 2026-11-11

> Prerelease for every Minecraft line (26.10, 26.20, 26.30, 26.40, 26.50, 26.60). Install the variant for your line, for example `lip install github.com/wo55555/OptiPiston#client_mc26_50@0.3.1`. The configuration format and C API v1 are unchanged. The piston-wall fix has been tested in game on 26.50 only; the other lines build and pass unit tests.

### Changed

- The arm lighting fix (no black arm or head after retracting with a block) now works on every line instead of 26.50 only. Tested in game on 26.10, 26.20, 26.40, and 26.50; 26.30 and 26.60 build and pass unit tests.

### Removed

- Removed the `observe` diagnostic build option. Published builds never included it.

### Fixed

- Fixed a pushed block disappearing when another piston pushed it again before its first push had finished.
- Fixed large piston machines briefly leaving holes, or seeming to finish at once, at high world speeds or while the game was slow to rebuild chunks. A landed block now keeps its animation until a chunk rebuild shows it, with no time limit.

## [0.3.0] - 2026-11-10

> Prerelease for every Minecraft line (26.10, 26.20, 26.30, 26.40, 26.50, 26.60). Install the variant for your line, for example `lip install github.com/wo55555/OptiPiston#client_mc26_50@0.3.0`. The configuration format and C API v1 are unchanged. The fixes have been tested in game on 26.50 only; the other lines build and pass unit tests. 26.60 is new and has not been run in game yet.

### Added

- Added support for Minecraft 26.60 (LeviLamina 26.60) as the Lip variant `client_mc26_60`. The arm lighting fix stays 26.50 only.

### Changed

- Widened the singleplayer world-speed range from 0.1x–10x to 0.01x–100x. New installs step through 0.01, 0.05, 0.1, 0.25, 0.5, 1, 2, 4, 10, 20, 50, and 100.

### Removed

- Removed the diagnostic `/optipiston mark` command. It did nothing in published builds.

### Fixed

- Fixed a piston pulled back by another piston fighting a block pushed into its old cell. The pulled piston no longer takes over the newer block's animation.
- Fixed a pushed block vanishing for a frame when the chunk rebuild produced no geometry for it yet.
- Fixed a piston head briefly not drawn when its moving body and the head were claimed in different render frames of the same tick.
- Fixed pushed blocks flickering on 26.50 when a finished chunk rebuild was shown a few frames later than it was built. The animation now holds until the game shows the rebuilt chunk.
- Fixed a landed block whose chunk mesh never reported it staying drawn by the animation forever.

## [0.2.0] - 2026-10-22

> Prerelease for every Minecraft line (26.10, 26.20, 26.30, 26.40, 26.50). Install the variant for your line, for example `lip install github.com/wo55555/OptiPiston#client_mc26_50@0.2.0`. If you installed an earlier `mc26_xx` variant, uninstall it first. The configuration format and C API v1 are unchanged. The render rewrite and both lighting fixes have been tested in game on 26.50 only; the other lines build and pass unit tests.

### Changed

- Rewrote the render ownership of piston arms, heads, and pushed blocks. Visuals now follow one timeline, and a pushed block is handed over to the landed block only after the real block has been drawn.
- Elect the piston head on the drawing thread's own table, and sample the animation clock once per render pass.
- Renamed the Lip variants to `client_mc26_10` through `client_mc26_50` so launchers that only list variants whose label contains `client` show OptiPiston.
- Added `probe` and `observe` build options for diagnostics. Published builds turn them off.

### Fixed

- Fixed the piston arm and head turning black for a moment after retracting with a block, on 26.50. The head cell briefly reports no light while the block lands there; the arm now uses the surrounding light only in that case. Other lines keep the vanilla lighting and are not changed by this fix.
- Fixed a sliding block disappearing for a frame on 26.50. The cell briefly reports no light once the real block is there; the moving copy keeps that cell's last lit value. Other lines are not changed by this fix.

## [0.1.4] - 2026-10-01

> Bug-fix prerelease for every Minecraft line (26.10, 26.20, 26.30, 26.40, 26.50). Install the variant for your line, for example `lip install github.com/wo55555/OptiPiston#mc26_50@0.1.4`. The configuration format and C API v1 are unchanged. These fixes have been tested in game on 26.50; the other lines build and pass unit tests.

### Fixed

- Fixed pushed blocks briefly disappearing just as they finish sliding on 26.30 and later.
- Fixed the piston head disappearing for a moment when a pushed piston is about to arrive.
- Fixed pushed blocks with block entities (such as chests) briefly vanishing when they hand over to the landed block.

## [0.1.3] - 2026-10-01

> Bug-fix prerelease. From this version one release carries every Minecraft line (26.10, 26.20, 26.30, 26.40, 26.50), and Lip installs a line by its variant, for example `lip install github.com/wo55555/OptiPiston#mc26_20@0.1.3`. If you installed `0.1.2` or earlier with `#client`, uninstall it first (`lip uninstall github.com/wo55555/OptiPiston#client`), then install the variant for your line. The configuration format and C API v1 are unchanged. The fix has been tested in game on 26.20; the other lines build and pass unit tests.

### Changed

- Published every Minecraft line under a single release tag, with one Lip variant (`mc26_10` to `mc26_50`) and one asset (`OptiPiston-mc<line>-windows-x64.zip`) per line.

### Fixed

- Fixed pushed blocks sometimes disappearing for part of the slide, introduced in 0.1.2.

## [0.1.2-mc26.20] - 2026-09-30

> Bug-fix prerelease, published separately for each Minecraft line (26.10, 26.20, 26.30, 26.40, 26.50); these notes apply to all of them. Update by installing the release for your line; the configuration format and C API v1 are unchanged. These fixes build and pass unit tests on every line but have not yet been tested in game.

### Fixed

- Fixed a crash, seen in multiplayer, when a pushed block was destroyed while its animation was still held.
- Fixed pushed chests and other blocks with block entities (such as trapped chests, shulker boxes, spawners, lecterns, enchanting tables, bells and decorated pots) appearing at their destination before the slide finished.

## [0.1.1-mc26.20] - 2026-09-30

> Performance prerelease, published separately for each Minecraft line (26.10, 26.20, 26.30, 26.40, 26.50); these notes apply to all of them. Update by installing the release for your line; the configuration format and C API v1 are unchanged. These changes are covered by unit tests and builds on every line but have not yet been tested in game.

### Changed

- Looked up moving blocks by cell instead of scanning every active piston move, reducing per-block work on the render and mesh threads when many blocks move at once.
- Read the external animation clock without taking a lock, and skipped the per-block mesh checks without locking while no pushed block is waiting for its hand-off.
- Derived the mod version and the `tooth.json` LeviLamina range from the `--mc` build option.

### Fixed

- Fixed held piston data accumulating for the whole session in a live world; entries older than the longest animation are now dropped.

## [0.1.0-mc26.20] - 2026-09-30

> First prerelease. Each Minecraft line (26.10, 26.20, 26.30, 26.40, 26.50) has its own release that only loads on the matching LeviLamina version. Only MC 26.20 has been tested in game.

### Added

- Piston animation with a fractional 2–4 game tick duration.
- Singleplayer world speed with optional audio and particle scaling.
- `/optipiston` client command and a C API for Playback's replay clock.

[Unreleased]: https://github.com/wo55555/OptiPiston/compare/v0.3.1...HEAD
[0.3.1]: https://github.com/wo55555/OptiPiston/compare/v0.3.0...v0.3.1
[0.3.0]: https://github.com/wo55555/OptiPiston/compare/v0.2.0...v0.3.0
[0.2.0]: https://github.com/wo55555/OptiPiston/compare/v0.1.4...v0.2.0
[0.1.3]: https://github.com/wo55555/OptiPiston/compare/v0.1.2-mc26.20...v0.1.3
[0.1.2-mc26.20]: https://github.com/wo55555/OptiPiston/compare/v0.1.1-mc26.20...v0.1.2-mc26.20
[0.1.1-mc26.20]: https://github.com/wo55555/OptiPiston/compare/v0.1.0-mc26.20...v0.1.1-mc26.20
[0.1.0-mc26.20]: https://github.com/wo55555/OptiPiston/releases/tag/v0.1.0-mc26.20
