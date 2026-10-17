# Changelog

All notable changes to this project will be documented in this file.

The format is based on [Keep a Changelog](https://keepachangelog.com/en/1.1.0/),
and this project adheres to [Semantic Versioning](https://semver.org/spec/v2.0.0.html).

## [Unreleased]

### Added

- `-b` and `-borderless` launch options, which run the game in a borderless window covering the main monitor. sc4-graphics-options' borderless mode gives the same window

### Changed

- The game runs in 32-bit colour even when its options or `-r` ask for 16, which made its textures band
- A `-r` size is used as given, without needing `-CustomResolution:enabled`. `-CustomResolution:disabled` keeps the game's own size
- With sc4-graphics-options installed, its settings still decide both

## [0.2.2] - 2026-10-17

### Added

- A `Use32BitDepth` setting in `scvk.ini` that brings back the 32-bit depth buffer of 0.2.0, off by default

### Changed

- The depth buffer is 16-bit, as on the game's DirectX renderer. Surfaces that sit very close together now band where DirectX bands too

### Removed

- The 0.2.1 change for the lines around ground lights, which turned other lines into patches of texture

### Fixed

- Thin lines around ground lights that sit exactly on their model after scrolling slowly, seen on an AMD RX 570

## [0.2.1] - 2026-10-11 [YANKED]

### Fixed

- Thin lines could appear around ground lights after scrolling slowly, seen on an AMD RX 570

## [0.2.0] - 2026-10-10

### Added

- scvk also takes the game's default DirectX slot, so installing it is copying two files and sc4-graphics-options is optional
- On a machine without Vulkan, the game keeps its own DirectX renderer
- A frame callback that lets other plugins draw over the game
- A benchmark mode in `scvk.ini` that compares scvk with the game's DirectX renderer, off by default

### Changed

- Faster drawing, most of all zoomed out, where scvk used to be slower than DirectX

### Fixed

- The game crashed at startup, or showed an empty region view, at 16-bit colour
- A paused city's frame rate was not unlocked when scd3d11 was installed too

## [0.1.2] - 2026-10-08

### Changed

- In fullscreen, the graphics driver can no longer take exclusive control of the screen, which should let PrintScreen capture the game on AMD graphics cards

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

[Unreleased]: https://github.com/aspctt/scvk/compare/0.2.2...HEAD
[0.2.2]: https://github.com/aspctt/scvk/compare/0.2.1...0.2.2
[0.2.1]: https://github.com/aspctt/scvk/compare/0.2.0...0.2.1
[0.2.0]: https://github.com/aspctt/scvk/compare/0.1.2...0.2.0
[0.1.2]: https://github.com/aspctt/scvk/compare/0.1.1...0.1.2
[0.1.1]: https://github.com/aspctt/scvk/compare/0.1.0...0.1.1
[0.1.0]: https://github.com/aspctt/scvk/releases/tag/0.1.0
