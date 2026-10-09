# <p align=center> scvk </p>

A native Vulkan renderer for SimCity 4.

SimCity 4 shipped in 2003 with a DirectX 7 renderer and an unfinished OpenGL
one. Both have aged badly against modern drivers. scvk replaces the renderer
with one built on Vulkan, loaded as a DLL plugin through the game's own GZCOM
plugin system.

This project is not affiliated with, endorsed by, or supported by Electronic
Arts Inc. or Maxis.

<p align=center>
	<a alt="BuyMeACoffee" href="https://buymeacoffee.com/aspctt"><img src="https://cdn.jsdelivr.net/npm/@intergrav/devins-badges@3/assets/cozy/donate/buymeacoffee-singular_vector.svg"></a>
</p>

## Status

**Early, but playable.** The region view, cities and menus all draw, with a
few visual bugs left.

| Area | Status |
|---|---|
| Startup, video modes, swapchain | Working, windowed and fullscreen |
| Terrain, buildings, interface | Working |
| Textures, blending, depth | Working |
| Day and night lighting, cloud shadows | Working |
| Loading screens | Working |
| Fog | Working, though the game never turns it on |
| In-game photos | Working |
| Stencil, polygon offset, flat shading | Working |
| Blits (all six, scaled, alpha, colour key) | Working |
| Terrain vertex buffer extension | Working |
| Lost device recovery | Working |
| ReShade (effects under the interface, depth) | Working with ReShade 6 or later |
| Live True3D shadows of props and networks | Working, on by default; `-NativeShadowMasks:off` turns them off |
| Registry and terrain shadows | Experimental, as in SCD3D11; `-NativeShadowMasks:replace` turns them on |

scvk does what SCD3D11, the
Direct3D 11 renderer, does, apart from SCD3D11's processor modules (CPU
scheduling, parallel render culling and the simulation tick budget), which are
not part of a renderer's job here. See [Parity with SCD3D11](#parity-with-scd3d11).

Known issues, each with a fix that still has to be confirmed in play:

- Scrolling while zoomed out was slower than DirectX, by roughly 40% on average in a
  large city. Since then a draw no longer pays for the diagnostics, the call trace or
  the phase clock below the debug level, binds no vertex buffer of its own, and two
  frames are in flight; it has to be measured again. The occasional frame of a tenth
  of a second or more there is the game's own: DirectX has them just as often.
- The interface flickered for a single frame now and then. The back buffer now keeps
  what the game drew from one frame to the next, as a DirectX one does.
- PrintScreen did not capture the game in exclusive fullscreen on at least one AMD
  graphics card, and on Windows 11 it opened the Snipping Tool, whose focus took the
  game out of fullscreen. In exclusive fullscreen scvk now takes the key itself and
  puts the frame it drew on the clipboard, without the game losing the focus, and
  the graphics driver may no longer take exclusive control of the screen.
  Win+PrintScreen is left to Windows and saves a file as usual.

## How it works

SimCity 4 selects a renderer by GZCOM class ID and knows exactly three:
DirectX (`0xBADB6906`), OpenGL (`0xC4554841`), and Software (`0x7ACA35C6`).
There is no way to register a fourth. scvk therefore claims the **OpenGL class
ID** and registers at a higher version so the GZCOM prefers it over the game's
built-in driver.

> **scvk and [SCGL](https://github.com/nsgomez/scgl) cannot be installed at the
> same time.** Both claim the same class ID, and whichever registers the higher
> version silently wins. Install one or the other.

The interface the game expects, `cIGZGDriver`, is a fixed function API: roughly
OpenGL 1.2 with extensions, including a matrix stack, alpha test, fog, and
two-stage texture environment combiners. Vulkan has none of that. So scvk is
not a thin translation layer but a fixed function emulator, which is why the
design centres on a packed state key selecting a cached pipeline, with an
ubershader reproducing the combiner network in fragment code.

## Compatibility with other mods

- [SC4Fix](https://github.com/nsgomez/sc4fix): fully compatible.
- [sc4-graphics-options](https://github.com/0xC0000054/sc4-graphics-options): fully
  compatible, and the easiest way to select scvk. See [Installing](#installing).

### FPS limits: built in

SimCity 4 caps its frame rate by simulation speed, at 30 for Turtle, 20 for
Rhino and 15 for Cheetah, by filling the rest of each frame with simulation.
A paused city is held at 30 the same way, though there is nothing to simulate.
That is the 30 a still camera settles at; the game skips the padding while the
camera moves.

scvk always lifts the paused 30. A paused city can then run at hundreds of
frames a second, where the game's animation clock rounded each frame up to 2 ms
and sped lot animations up, so scvk also lets it count short frames as they
are. It can also raise the speed caps itself, so
[sc4-disable-fps-limits](https://github.com/caspervg/sc4-disable-fps-limits) is
not required, though it remains compatible.

There is nothing to set. scvk raises the speed caps to the refresh rate of the
primary display (60 on a 60 Hz screen, 144 on a 144 Hz one), so the game never
draws more frames than the screen can show. It leaves the simulation's own time
alone, so a running city simulates as fast as it always did.

This lives in scvk because frame pacing and presentation are one concern. Once
the swapchain exists, the present mode and this cap have to agree, and keeping
them in separate plugins would mean two places that can contradict one another.

scvk is more cautious than it strictly needs to be here. It requires game
version 641, and it reads each byte before changing it: if a byte does not hold
the value it expects, it declines and writes the reason to the log instead of
overwriting whatever is actually there. Running both plugins is therefore
untidy but not dangerous.

## Parity with SCD3D11

scvk follows SCD3D11 wherever the two can share behaviour, so either renderer
can be dropped in with the same launcher and the same expectations.

**Frames.** The game draws into a back buffer of scvk's own, the size of the
video mode, that keeps its contents from frame to frame like a DirectX back
buffer; presenting copies or scales it into the swapchain. A window or borderless
fullscreen presents with the flip model, one swapchain image more than the surface's
minimum, and exclusive fullscreen with the legacy minimum, as SCD3D11 chooses
between DXGI's swap effects; `-FlipModel:off` keeps windows on the legacy model too.
Two frames are in
flight, and per-frame memory comes from rings that wait for the GPU only when
they would overwrite something it still reads. A frame keeps drawing while the
window is minimised. A lost device is torn down and rebuilt at the next frame
boundary, its textures and saved regions coming back empty under their old
names, as SCD3D11 does.

**Settings.** SCD3D11's switches work on the command line: `-VSync:off`,
`-GPU:default`, `-Borderless` or `-FullscreenMode:Borderless`, `-ReShade:off`,
`-FlipModel:off`,
`-LogLevel:<level>`, `-GridDebug`, `-NativeShadowMasks:network|props|all|replace`,
`-LiveShadowDiag`, `-LiveShadowAllFaces` and `-LiveShadowKeepPartial`. There is
no settings file: without a switch, every setting takes its automatic value
(vertical sync on, the high-performance graphics card, exclusive fullscreen,
ReShade integration on, the True3D and registry shadows on, the pipeline cache
and the terrain vertex buffer on,
the log at `info`, the speed caps at the display's refresh rate).

**ReShade.** Installed as its Vulkan layer, ReShade wraps scvk's device. scvk
then registers as an add-on and draws the effects at the end of the 3D view,
before the interface, with the city's depth bound to `DEPTH` (encoded for
`RESHADE_DEPTH_LINEARIZATION_FAR_PLANE`) and the `sc4_sun_valid`,
`sc4_depth_scale` and `sc4_world_per_screen_height` uniforms filled in.

**Lighting.** The lighting extension works as SCD3D11's: up to eight lights,
directional or positional, with ambient and diffuse colours, and a material with
ambient, diffuse and emissive colours, lit with Direct3D 7's equation, no specular
term and no diffuse term for a vertex without a normal. While no light is on and the
material is as the game leaves it, the shader lights as before; otherwise each light's
diffuse term is added per pixel, as SCD3D11 does, where per vertex a positional
light stretched across long triangles.
A device that cannot take the lit shader's 29 vertex attributes, and a draw without
normals, are lit per vertex on the CPU instead. The colour multiplier is the global
ambient light, clamped to 0 to 1 as Direct3D holds it, and switching the lighting off
draws the vertex colour as it is.

**Shadows.** The game-side shadow modules are SCD3D11's, and the shadow map, its
casters and the composite are drawn the same way in Vulkan. A caster drawn as
points or lines casts as points and lines, as SCD3D11 draws it. They are on by
default as `all`: every True3D prop and network piece drawn live in the shadow map,
the rest of the shadows left to SC4. `-NativeShadowMasks:replace` adds what SCD3D11
keeps experimental and off by default too: the registry, which takes over the shadow
records SC4 draws as flat decals, and the terrain's own shadows in place of SC4's
baked self-shading. In a city of dense parks beside raised roads the registry laid
blocks of shadow across the roads. `-NativeShadowMasks:off` leaves SC4's own
shadows, and `:network` or `:props` choose part of them. `-ShadowRegistry:off` leaves the shadow records,
and the terrain shadows with them, to SC4, and `-TerrainShadows:off` leaves only the
terrain's shading to SC4, for telling apart which part a picture comes from.

**Frame callback.** Another plugin can draw into each frame through
`SCVKRegisterFrameCallback` and `SCVKUnregisterFrameCallback`, scvk's
counterparts of SCD3D11's frame callback, declared in `include/scvk_frame.h`.
The callback receives the Vulkan device, the queue, a command buffer being
recorded and the back buffer, and hears before the device is destroyed.

**Diagnostics.** A render watchdog logs where the render thread is when frames
stop for five seconds, and every message that moves the window's focus,
visibility or size goes to the log.

Not carried over: SCD3D11's processor modules.

## Building

Requires Visual Studio 2022 or later with the desktop C++ workload. The Vulkan
headers are in `vendor/vulkan`, so the Vulkan SDK is not needed; a Debug build
uses its validation layers when one is installed.

```
msbuild scvk.sln /p:Configuration=Release /p:Platform=Win32
```

SimCity 4 is a 32-bit process, so **Win32 is the only supported platform**.
There is deliberately no x64 configuration.

Warnings are strict and treated as errors, so a build that warns does not
finish.

### Vulkan SDK

scvk loads `vulkan-1.dll` by name rather than linking `vulkan-1.lib`, so the
SDK is only needed at build time and users do not need it installed. Nothing
Vulkan-specific ships with the DLL.

The build looks for the headers in this order: an explicit
`/p:VulkanSdkDir=<root>`, then a `VULKAN_SDK_32` environment variable, then a
local `1.3.296.0` install, then `VULKAN_SDK`, and otherwise takes the 1.3.296
headers in `vendor/vulkan`.

> **For validation layers, use SDK 1.3.296.0.** SimCity 4 is a 32-bit process,
> so the layers must be 32-bit to load into it, and 1.3.296.0 is the last
> release that ships 32-bit components at all. Later SDKs are 64-bit only and
> their layers will silently not load. Only the layers care; the headers are
> architecture independent, so a newer SDK still builds fine, just without
> validation.

Debug builds enable the validation layers when they are present and route
their output into `scvk.log`, which is the only channel visible when running
inside the game. Release builds enable neither. The layers check every draw,
which makes a Debug build many times slower, so measure performance in Release.

### On Linux

`tools/build-mingw.sh` builds the same 32-bit Windows DLL with the MinGW-w64 cross
compiler (`g++-mingw-w64-i686` on Debian and Ubuntu), with the Vulkan headers in
`vendor/vulkan` unless `VULKAN_INCLUDE` names others. The output is `Release-mingw/scvk.dll`.

`-NativeShadowExperiment` is in that build too, its hooks written as GCC assembly
with the same instructions as the MSVC ones. The other game-side shadow modules
(`-NativeShadowMasks`, the registry and the live shadow diagnostics) hook the game
with MSVC inline assembly and structured exception handling, so they are left out of
it. The release is built with Visual Studio.

`tests/wine/run.sh` then checks the DLL under Wine, without the game: on a virtual
display with Mesa's software Vulkan driver, a small program loads scvk the way the game
does, draws a grid of squares through every draw path, half of it lit through the lighting
extension, and checks each pixel, then
runs in exclusive fullscreen, presses PrintScreen and checks the frame on the
clipboard. It needs Wine with 32-bit support, Xvfb, xdotool, and the 32-bit
`mesa-vulkan-drivers` and `libvulkan1`.

### Shaders

GLSL lives in `shaders/`. It is compiled to SPIR-V and embedded into
`src/ShaderBinaries.h`, which is **committed**, so building scvk needs no
shader compiler and the DLL ships no external shader files.

Regenerate only after changing a shader:

```
pwsh shaders/compile.ps1
```

or, where there is Python rather than PowerShell, `python3 shaders/compile.py`,
which takes `glslc` or `glslangValidator` (with `spirv-opt` and `spirv-val`
when present).

## Diagnostics

`tools/run-sc4.ps1` deploys a build, runs the game for a set time and collects
the log. Options are listed at the top of the script.

Start the game with `-LogLevel:trace` to see everything the game asks of the
renderer in `scvk.log`, and `debug` for the statistics below without that. At
either level a few screenshots (`scvk-frame-N.bmp`, `scvk-region-N.bmp`) are
saved beside it during a session, and Scroll Lock in game saves one on demand,
along with the saved scene and its depth (`scvk-key-N-*`).

Every 300 frames the log also notes the frame rate, the slowest frame and how
its time split between the game and scvk's own work (drawing, copying geometry,
textures, and waiting for the graphics card), how many textures are loaded and
how many were created, deleted or uploaded since the last note, how much
geometry was copied, how many blocks of graphics memory the textures share, how
many memory allocations scvk holds against the graphics driver's limit, and how
strong the game asked the building shadows to be.

To see a debug view, put an empty file with one of these names next to
`scvk.dll`, and delete it to go back to normal:

- `scvk-debug-passes`: colours each draw by how it blends
- `scvk-debug-texture-colour` or `scvk-debug-texture-alpha`: the texture alone
- `scvk-debug-vertex-colour` or `scvk-debug-vertex-alpha`: the lit vertex
  colour alone
- `scvk-skip-cloud-shadows`: leaves out the cloud shadows
- `scvk-force-fog`: draws the fog the city view would use if its fog were on, which
  fades the far distance to white
- `scvk-validate-sync`: in Debug builds, also checks the GPU work is correctly
  ordered (slow)
- `scvk-record-tile-draws`: Scroll Lock also saves every recent draw of the
  parts of the scene the game redrew, and of the moving things drawn over it
  (`scvk-key-N-draws.bin`), which `tools/draw-records.py` reads (slow)
- `scvk-small-texture-pools`: makes room for only 64 textures at a time before
  scvk has to set aside more, so an ordinary session tests that it does

## Installing

scvk needs Windows 10 or 11, or Linux with Wine or Proton (Steam Play), a graphics
card with Vulkan support and game version 641.

On Linux the same `scvk.dll` goes in the same `Plugins` folder, inside the Wine prefix
or Proton's. Wine needs its 32-bit Vulkan support: the 32-bit Vulkan loader and Mesa or
NVIDIA's 32-bit driver (on Debian and Ubuntu, `libvulkan1:i386` and
`mesa-vulkan-drivers:i386`); Proton brings what it needs. The log says it runs under
Wine and which version. PrintScreen is usually taken by the Linux desktop before the
game sees it; when it does reach the game in fullscreen, scvk copies the frame to the
clipboard as on Windows.

1. Download the zip from the [releases page](https://github.com/aspctt/scvk/releases)
   and copy `scvk.dll` into the `Plugins` folder of your SimCity 4
   installation.
2. **Select the OpenGL renderer.** The easiest way is
   [sc4-graphics-options](https://github.com/0xC0000054/sc4-graphics-options):

```ini
[GraphicsOptions]
Driver=OpenGL
ColorDepth=32
```

> **`Driver=OpenGL` does not select SC4's own unfinished OpenGL renderer.**
>
> This trips people up, because that renderer is famously broken and cannot
> start a game. But the game resolves a renderer by class ID, and the GZCOM
> hands out the highest-version registrant for a given class. scvk registers
> the OpenGL class ID at version 1000000 against the built-in driver's 0, so
> asking for OpenGL gets you scvk and the built-in driver never runs. SCGL
> works the same way, which is why graphics-options accepts `Driver=SCGL` as a
> literal alias for the same entry.
>
> If you leave this set to `DirectX`, SC4 will still load scvk and may still
> call `Init` on it while enumerating drivers, then quietly use DirectX
> instead. The log will show a short burst of activity ending in `Shutdown`,
> which looks like a failure but is just scvk not being the chosen renderer.

`ColorDepth=32` matters too: Windows 8 and later no longer report 16-bit
display modes, so every mode scvk can enumerate is 32bpp, while SC4 defaults to
16.

A `scvk.log` file is written next to the DLL, falling back to the temp
directory if the Plugins folder is not writable. It is written at `info`; the
`-LogLevel:<level>` command line switch changes that, from `trace` to `off`.

scvk has no settings file and needs no configuration. To turn it off, remove
`scvk.dll` from the Plugins folder.

## License

scvk is licensed under the **GNU Lesser General Public License, version 2.1 or
(at your option) any later version**. See [LICENSE](LICENSE).

You may link it dynamically with proprietary software such as SimCity 4, which
is the entire point of the LGPL. Changes to scvk itself must be shared under
the same terms.

Third-party sources are vendored in [`vendor/`](vendor/README.md), each
retaining its original notice, all LGPL-2.1-or-later:

- [gzcom-dll](https://github.com/nsgomez/gzcom-dll) for the plugin ABI and
  driver interface declarations
- [Scion](https://github.com/nsgomez/scion) for reference counting
- [SCGL](https://github.com/nsgomez/scgl) for the driver extension interfaces
  and vertex format decoding

Particular thanks to Nelson Gomez, whose SCGL is the reference implementation
that made the shape of this interface legible at all. See [NOTICE](NOTICE).
