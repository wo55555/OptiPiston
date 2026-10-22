# Changelog

All notable changes to this project will be documented in this file.

The format is based on [Keep a Changelog](https://keepachangelog.com/en/1.0.0/),
and this project adheres to [Semantic Versioning](https://semver.org/spec/v2.0.0.html).

## [Unreleased]

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

[Unreleased]: https://github.com/wo55555/OptiPiston/compare/v0.2.0...HEAD
[0.2.0]: https://github.com/wo55555/OptiPiston/compare/v0.1.4...v0.2.0
[0.1.3]: https://github.com/wo55555/OptiPiston/compare/v0.1.2-mc26.20...v0.1.3
[0.1.2-mc26.20]: https://github.com/wo55555/OptiPiston/compare/v0.1.1-mc26.20...v0.1.2-mc26.20
[0.1.1-mc26.20]: https://github.com/wo55555/OptiPiston/compare/v0.1.0-mc26.20...v0.1.1-mc26.20
[0.1.0-mc26.20]: https://github.com/wo55555/OptiPiston/releases/tag/v0.1.0-mc26.20
