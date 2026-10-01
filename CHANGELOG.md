# Changelog

All notable changes to this project will be documented in this file.

The format is based on [Keep a Changelog](https://keepachangelog.com/en/1.0.0/),
and this project adheres to [Semantic Versioning](https://semver.org/spec/v2.0.0.html).

## [Unreleased]

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

[Unreleased]: https://github.com/wo55555/OptiPiston/compare/v0.1.3...HEAD
[0.1.3]: https://github.com/wo55555/OptiPiston/compare/v0.1.2-mc26.20...v0.1.3
[0.1.2-mc26.20]: https://github.com/wo55555/OptiPiston/compare/v0.1.1-mc26.20...v0.1.2-mc26.20
[0.1.1-mc26.20]: https://github.com/wo55555/OptiPiston/compare/v0.1.0-mc26.20...v0.1.1-mc26.20
[0.1.0-mc26.20]: https://github.com/wo55555/OptiPiston/releases/tag/v0.1.0-mc26.20
