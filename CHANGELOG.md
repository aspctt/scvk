# Changelog

All notable changes to this project will be documented in this file.

The format is based on [Keep a Changelog](https://keepachangelog.com/en/1.1.0/),
and this project adheres to [Semantic Versioning](https://semver.org/spec/v2.0.0.html).

## [Unreleased]

The features, stability and performance of SCD3D11, the Direct3D 11 renderer,
brought to Vulkan. SCD3D11's processor modules (CPU scheduling, parallel render
culling, the simulation tick budget) are not part of scvk.

### Added

- `-ShadowRegistry:off` and `-TerrainShadows:off`, which hand the shadow records or the terrain's shading back to SC4 while the rest of the shadows stay, to tell apart which part of the shadows a picture comes from
- `Lighting extension:` lines in the log: the first draw the extension lights, and at shutdown how many it lit per pixel and per vertex and how many light or glow draws were left out of the shadow map
- Automatic settings: the speed caps follow the display's refresh rate, and everything else has a default that needs no configuring
- A back buffer of scvk's own that keeps what the game drew from one frame to the next, as a DirectX back buffer does, scaled to the window when presented
- Two frames in flight, with per-frame vertex, index, staging and uniform memory in rings, and resizable BAR memory where the device has it
- Recovery from a lost device: the device, textures and saved regions are rebuilt at the next frame boundary, backing off between failed attempts
- Rendering continues while the window is minimised; presentation pauses and resumes and says so in the log
- SCD3D11's command line switches (`-VSync:off`, `-GPU:default`, `-Borderless`, `-FullscreenMode:Borderless`, `-ReShade:off`, `-LogLevel:`, `-GridDebug`)
- Borderless fullscreen without a display mode change
- The 1920x1080, 2048x1152, 2560x1600 and 3200x1800 windowed modes, whatever the display lists
- A pipeline cache kept in `scvk-pipelines.bin`
- The stencil buffer and stencil test, polygon offset, flat shading and stencil clears
- All six blits, scaled, with constant or modulated alpha, the source alpha and colour keys, and the Punt that switches the plain blits to the source alpha
- The game's vertex buffer extension, through which it hands over the terrain
- `GenTextures`, `BindTexture` and `TexImage2D`, mipmap levels included, and texture uploads in every format SCD3D11 reads
- ReShade integration: effects under the interface, the city's depth for the `DEPTH` semantic, and the `sc4_sun_valid`, `sc4_depth_scale` and `sc4_world_per_screen_height` uniforms
- Live True3D and network shadows, SCD3D11's `-NativeShadowMasks` modes, the shadow registry and the terrain's own shadows
- A frame callback API, `SCVKRegisterFrameCallback` and `SCVKUnregisterFrameCallback`, declared in `include/scvk_frame.h`
- A render watchdog that logs where the render thread is when frames stop for five seconds
- The thumbnail focus guard, which keeps the city thumbnail drawn on save from taking the focus
- Every message that moves the window's focus, visibility or size, in the log
- `tools/build-mingw.sh`, which builds scvk.dll on Linux with MinGW-w64, and `tests/wine/run.sh`, which checks it under Wine without the game: every draw path pixel by pixel, exclusive fullscreen and PrintScreen
- The log says whether the game runs on Windows or under Wine, and which Wine
- A fault in the game writes its registers and the likely return addresses on its stack to `scvk.log`, each as module+offset, so a crash can be placed in scvk, the game or another plugin without the minidump
- The lighting extension, as SCD3D11 has it: eight lights with ambient and diffuse colours, directional or positional, and the ambient, diffuse and emissive material, lit with Direct3D 7's equation when the game uses them, each light's diffuse term per pixel as SCD3D11 lights
- SCD3D11's flip model: one swapchain image more than the minimum in a window or borderless, the legacy minimum in exclusive fullscreen and under `-FlipModel:off`
- Casters drawn as points or lines cast shadows as points and lines, as under SCD3D11
- `-NativeShadowExperiment` in MinGW builds, its hooks written as GCC assembly
- PrintScreen and Alt+PrintScreen in exclusive fullscreen copy the frame scvk drew to the clipboard, so the Snipping Tool no longer takes the game out of fullscreen on Windows 11

### Changed

- True3D shadows of props and networks are on by default, as `-NativeShadowMasks:all`; `-NativeShadowMasks:off` turns them off. The registry and the terrain shadows stay experimental and off, as in SCD3D11, and `-NativeShadowMasks:replace` turns them on
- Uncompressed textures are B8G8R8A8, the game's own order, so most uploads are a plain copy straight into the upload batch
- A draw binds only what changed since the last, and pipelines are found by hash
- The transform and texture coordinates are recomputed only when the game changes them
- In fullscreen, the graphics driver can no longer take exclusive control of the screen, as in 0.1.2, which should let PrintScreen capture the game on AMD graphics cards
- Below the debug log level a draw no longer runs the per-draw diagnostics, counts its calls or reads the phase clock, which a city redraw at the widest zoom did over a hundred thousand times
- The colour multiplier is clamped to 0 to 1, as Direct3D 7 holds the ambient light, and turns the lighting on as under SCD3D11
- Vertices are copied at a whole number of vertices into their block, which is bound once, so a draw no longer binds a vertex buffer of its own

### Fixed

- Blocks of shadow across raised roads beside dense parks: the shadow registry, which SCD3D11 keeps experimental and off by default, was on by default. It is now off unless `-NativeShadowMasks:replace` asks for it
- Draws blended over the scene that are added or write no depth, which is how light and glow are drawn, are left out of the shadow map
- The lighting extension's lights add their diffuse term per pixel, as SCD3D11 does, rather than per vertex, which stretched a positional light across long triangles. SimCity 4 itself was not seen to use the extension; the strips of light along True3D highways at night are the same under SCD3D11
- The texture environment's blend mode was taken as modulate: it now weighs the environment colour against the primary colour by the texture's colour and multiplies the alphas, as OpenGL defines it and SCD3D11 draws it, on one stage and through the combiner network
- Raised roads, ramps and other casters lit by the sun shadowed themselves in bands: the casters are drawn into the shadow map with a slope-scaled depth bias, and the composite looks the shadow up from just off each surface along its normal, so only what lies below or behind a caster is darkened
- A build failed with "Vulkan headers not found" on a machine without the Vulkan SDK: the Vulkan 1.3.296 headers are now in `vendor/vulkan`, which the project falls back to
- Every frame submitted a command buffer handle that had already been cleared, which Wine's Vulkan layer refused outright
- Headers named as `<windows.h>`, and the return address and the version library asked for portably, so the sources build with MinGW as well as Visual Studio
- The interface flickering for a frame, from a back buffer that did not keep its contents
- `BitBltAlphaModulate` and the vertex buffer extension's `GetVertices` and `DrawPrimsIndexed` had the wrong signatures

### Removed

- `scvk.ini`: there is no settings file any more. `MaxFPS` is replaced by caps at the display's refresh rate, `LogLevel` by the `-LogLevel:` switch, and `UnlockRunningFPS`, which slowed the simulation, is gone

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
