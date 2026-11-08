# Changelog

All notable changes to this project will be documented in this file.

The format is based on [Keep a Changelog](https://keepachangelog.com/en/1.1.0/),
and this project adheres to [Semantic Versioning](https://semver.org/spec/v2.0.0.html).

## [Unreleased]

### Changed

- Live shadows come with one switch, `-LiveShadows:on`, as in scd3d11: True3D props and network pieces cast live, the shadow registry takes over the game's building, tree and pole shadows, and hills cast terrain shadows
- Live shadows darken the scene by multiplying, so large terrain shadows no longer wash out in blue
- Terrain shadows get softer edges further from the hill casting them
- Under live shadows prebuilt network pieces lose their flat shadow masks, since they cast live, and `-NetworkShadowMasks:on` keeps them

### Removed

- `-NativeShadowMasks` and its modes, which `-LiveShadows:on` replaces
- The live shadow diagnostics and experiments behind `-LiveShadowDiag`, `-NativeShadowDiag`, `-LiveShadowAllFaces`, `-LiveShadowKeepPartial` and `-NativeShadowExperiment`

## [0.6.0] - 2026-11-05

### Added

- `-Depth16:off` and `-Depth16:on`, scd3d11's name for `-DepthBuffer:32` and `16`
- `-borderless:off` and `-b:off` keep the window mode in the game's options instead of the borderless default
- `scvk.log` names the `scvk.ini` it read, or says there is none, and says when no `MaxFPS` is set
- `scvk.log` shows the game's command line
- `scvk.dll` carries its version and description, which Windows shows on the file's Details tab
- In borderless and fullscreen the cursor stays on the main monitor while the game is in front

### Changed

- Launch options that turn something on or off also take `true`, `false`, `enabled`, `disabled`, `1` and `0`, as in scd3d11, and mean on by themselves
- `scvk.log` and the setup dialog show the Windows account in paths as `%USERPROFILE%` or `%USERNAME%`
- With live shadows off, the default, scvk no longer hooks the game's static city view for them

### Fixed

- The city scrolls at the screen's edges in borderless, as it does in exclusive fullscreen
- The Rhino speed cap follows the others again. scvk read it one byte off, took the 0 there for another plugin's change, logged it as `Rhino 0` and left the game's 20

## [0.5.0] - 2026-11-01

### Added

- Setup checks at startup, which write to `scvk.log`: another driver drawing, any plugin loaded twice, SC4RenderServices older than 0.8.3, plugins built for SC4RenderServices before 0.8, plugins missing the Visual C++ runtime, no 4GB patch, compatibility mode, launch switch and `scvk.ini` typos, an unquoted `-UserDir` and unreadable lines in `SC4GraphicsOptions.ini`
- Serious setup problems also show in the game's own dialog when the region view opens. `DoctorDialog=false` in `scvk.ini` or `-DoctorDialog:off` keeps them in the log
- `scvk.log` names each graphics card's driver version and video memory, every DLL loaded with its version, and free address space, RAM and commit at startup and when an allocation fails
- `-GPU:<n>` picks a graphics card by its number in `scvk.log`, and scvk warns when a much faster card sits idle
- `-DepthBuffer:32` and `-DepthBuffer:16` launch options, ahead of `Use32BitDepth` in `scvk.ini`
- `-LiveShadows:on` and `-LiveShadows:off`, the name scd3d11 uses now, the same as `-NativeShadowMasks:replace` and `off`
- `-d:Vulkan` picks scvk for a launch, whatever scd3d11 or `Driver=` in `SC4GraphicsOptions.ini` would choose
- The setup checks tell a plugin that is a debug build from one missing the Visual C++ runtime, since only Visual Studio has what a debug build needs
- `scvk.log` notes a depth offset other than 0, the first time each one is asked for

### Changed

- Scrolling is lighter: draws wholly outside the strip the game is redrawing are left out before their vertices are copied. `-ViewportCull:off` sends them all, as before
- Less work around each draw when the game repeats a matrix or texture setting it already made
- scvk stays out of the way when it isn't the renderer. With scd3d11 or the Software driver drawing, it doesn't start Vulkan, patch the game, install its fault log, change the video mode or show its setup dialog
- One Vulkan instance less at startup, since the slot the game does not use no longer makes one

### Fixed

- After a lost graphics device the game is asked to upload its textures again, as its DirectX driver does
- A stalled graphics card no longer hangs the game while it saves, takes a photo or uploads textures. That read or upload is skipped instead
- A comment after a value in `scvk.ini`, such as `Use32BitDepth=true ; note`, no longer stops the value being read
- Live shadows redraw when a camera plugin changes the yaw or pitch
- Live shadows no longer force a full redraw on every scroll frame along the city's edge
- Live shadows no longer shade the backdrop beyond the city

## [0.4.0] - 2026-10-31

### Added

- A stencil buffer, so SC4RenderServices 0.8.3 or newer draws terrain decals back over shadows once per pixel and overlapping decals no longer come out darker

### Changed

- Cards without a 16-bit depth format that has a stencil use a 24-bit or 32-bit one, with every depth rounded to 16 bits
- The zip has a `scvk.ini` again, for anyone who wants to change a setting such as `Use32BitDepth`. scvk still runs without it

## [0.3.0] - 2026-10-28

Most of this release is UlisseWolf's port of scd3d11's features.

### Added

- ReShade's effects draw under the interface, with the city's depth, when ReShade is installed with full add-on support. `-ReShade:off` leaves it drawing over everything
- Live shadows for True3D props and network pieces, off by default. `-NativeShadowMasks:all` turns them on, and `-NativeShadowMasks:replace` adds the experimental shadow registry and terrain shadows
- PrintScreen in exclusive fullscreen puts the game's own picture on the clipboard, instead of opening the Snipping Tool and dropping the game out of fullscreen
- Region thumbnails no longer go blank when the game loses focus while saving
- `-MaxFPS:<n>` and `-MaxFPS:off` for the speed caps, ahead of `MaxFPS` in `scvk.ini`
- scd3d11's launch options: `-VSync:off`, `-GPU:default`, `-FlipModel:off`, `-LogLevel:<level>` and `-FullscreenMode:Borderless`
- `-b` and `-borderless` launch options for a borderless window, which sc4-graphics-options' borderless mode gives too
- `-windowed` and `-fullscreen` launch options, the same as `-w` and `-f`
- A pipeline cache beside the DLL, so later sessions build their pipelines faster
- A watchdog and fault reports in `scvk.log`, which say where a frozen or crashed game was
- The log notes Wine and its version when the game runs on Linux
- A MinGW build and a Wine test harness, for building and testing on Linux

### Changed

- The game starts borderless, in a frameless window covering the main monitor at the desktop's size. `-w`, `-f` or a `-r` size pick another mode, and sc4-graphics-options' settings still decide when it is installed
- The game runs in 32-bit colour even when its options or `-r` ask for 16, which made its textures band
- A `-r` size is used as given, without needing `-CustomResolution:enabled`. `-CustomResolution:disabled` keeps the game's own size
- With sc4-graphics-options installed, its settings still decide both
- The speed caps follow the display's refresh rate. A `scvk.ini` from 0.2, which says `MaxFPS=0`, keeps the game's caps
- Faster scrolling, with 13 to 21% more frames a second in the benchmark's close, near and far scenes
- The zip no longer has a `scvk.ini`. scvk still reads one when it is there

### Removed

- `UnlockRunningFPS`, which changed the same code as SC4CPUOptions and scd3d11

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

[Unreleased]: https://github.com/aspctt/scvk/compare/0.6.0...HEAD
[0.6.0]: https://github.com/aspctt/scvk/compare/0.5.0...0.6.0
[0.5.0]: https://github.com/aspctt/scvk/compare/0.4.0...0.5.0
[0.4.0]: https://github.com/aspctt/scvk/compare/0.3.0...0.4.0
[0.3.0]: https://github.com/aspctt/scvk/compare/0.2.2...0.3.0
[0.2.2]: https://github.com/aspctt/scvk/compare/0.2.1...0.2.2
[0.2.1]: https://github.com/aspctt/scvk/compare/0.2.0...0.2.1
[0.2.0]: https://github.com/aspctt/scvk/compare/0.1.2...0.2.0
[0.1.2]: https://github.com/aspctt/scvk/compare/0.1.1...0.1.2
[0.1.1]: https://github.com/aspctt/scvk/compare/0.1.0...0.1.1
[0.1.0]: https://github.com/aspctt/scvk/releases/tag/0.1.0
