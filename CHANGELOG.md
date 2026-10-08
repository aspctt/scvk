# Changelog

All notable changes to this project will be documented in this file.

The format is based on [Keep a Changelog](https://keepachangelog.com/en/1.1.0/),
and this project adheres to [Semantic Versioning](https://semver.org/spec/v2.0.0.html).

## [Unreleased]

## [0.1.1] - 2026-10-08

### Added

- A line in `scvk.log` once the first frame is on screen, confirming scvk is running

### Fixed

- Thin dark seams along the edges of street and lot tiles
- Two warnings `scvk.log` showed on every startup

## [0.1.0] - 2026-10-07

### Added

- A Vulkan renderer for SimCity 4, used when the game's driver is set to OpenGL
- Draws the region view, cities, menus and loading screens, windowed, full screen or borderless
- Lighting that matches the DirectX renderer, with cloud shadows, building shadows and night lights
- In-game photos
- A paused city is no longer held at 30 frames a second
- `MaxFPS` and `UnlockRunningFPS` settings in `scvk.ini` to raise the game's frame rate caps
- A `LogLevel` setting in `scvk.ini` for `scvk.log`, written beside the DLL

[Unreleased]: https://github.com/aspctt/scvk/compare/0.1.1...HEAD
[0.1.1]: https://github.com/aspctt/scvk/compare/0.1.0...0.1.1
[0.1.0]: https://github.com/aspctt/scvk/releases/tag/0.1.0
