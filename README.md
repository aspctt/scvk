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
| Performance | Faster than DirectX 7 in every [benchmark](#benchmark) scene, most of all zoomed out |

Known issues:

- On one AMD RX 570, thin lines can appear around ground lights after scrolling
  slowly, until a zoom redraws the scene. Other graphics cards have not shown them.
- PrintScreen did not capture the game in exclusive fullscreen on at least one AMD
  graphics card. 0.1.2 should fix it, but that is not confirmed yet.
- On Windows 11, PrintScreen opens the Snipping Tool, which takes focus and so takes
  the game out of exclusive fullscreen. Win+PrintScreen captures the game.

## How it works

SimCity 4 selects a renderer by GZCOM class ID and knows exactly three:
DirectX (`0xBADB6906`), OpenGL (`0xC4554841`), and Software (`0x7ACA35C6`).
There is no way to register a fourth. scvk therefore claims the **OpenGL class
ID** and registers at a higher version so the GZCOM prefers it over the game's
built-in driver. It claims the **DirectX class ID**, the game's default, as well,
one version lower, so the game picks scvk without any settings while a renderer
built for that slot, such as [scd3d11](https://github.com/caspervg/scd3d11), still
takes it.

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
  compatible, and the easiest way to choose between renderers. See
  [Installing](#installing).
- [scd3d11](https://github.com/caspervg/scd3d11): can be installed alongside, and
  keeps the DirectX slot. See [Installing](#installing).

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

Set `MaxFPS` in `scvk.ini` to raise the caps:

```ini
[scvk]
MaxFPS=120
```

A running city still gets at least 15 ms of simulation every frame, which keeps
it near 60 frames a second whatever `MaxFPS` says. `UnlockRunningFPS=true`
lowers that to 3 ms. The time comes out of the simulation, so the city
simulates more slowly, most of all at Cheetah. It is off by default.

This lives in scvk because frame pacing and presentation are one concern. Once
the swapchain exists, the present mode and this cap have to agree, and keeping
them in separate plugins means two settings files that can contradict one
another.

`MaxFPS` is off by default because caspervg's plugin does the same job, and
enabling both with different values would be confusing. **If you already use
that plugin, leave `MaxFPS` at 0.**

scvk is more cautious than it strictly needs to be here. It requires game
version 641, and it reads each byte before changing it: if a byte does not hold
the value it expects, it declines and writes the reason to the log instead of
overwriting whatever is actually there. Running both plugins is therefore
untidy but not dangerous.

[scd3d11](https://github.com/caspervg/scd3d11) limits each frame's simulation time
by changing the same code, and the two work together. With `UnlockRunningFPS=true`,
only the plugin that starts first gets that change.

### Drawing over the game from another plugin

scvk.dll exports two functions that let another plugin draw over every frame, such as
an overlay made with Dear ImGui. They work like scd3d11's frame callback, with Vulkan in
place of Direct3D 11:

```c
BOOL __stdcall SCVKRegisterFrameCallback(SCVKFrameCallback callback, void* userData);
BOOL __stdcall SCVKUnregisterFrameCallback(SCVKFrameCallback callback, void* userData);
```

The callback runs on the game's main thread once a frame, after the game has drawn
everything including its interface. It gets the instance, device, queue and loader, and a
command buffer already inside a render pass on the frame's image. It runs once more,
with the device idle, before scvk destroys a device, which it does for every video mode
the game sets. [`src/SCVKFrameCallback.h`](src/SCVKFrameCallback.h) is the whole contract
and depends on nothing else, so a plugin copies it as it is. There is no import library:
find `scvk.dll` with `GetModuleHandleW` and look the functions up with `GetProcAddress`.

## Building

Requires Visual Studio 2022 or later with the desktop C++ workload, and the
Vulkan SDK for its headers.

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

The build looks for the SDK in this order: an explicit
`/p:VulkanSdkDir=<root>`, then a `VULKAN_SDK_32` environment variable, then a
local `1.3.296.0` install, then `VULKAN_SDK`.

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

### Shaders

GLSL lives in `shaders/`. It is compiled to SPIR-V and embedded into
`src/ShaderBinaries.h`, which is **committed**, so building scvk needs no
shader compiler and the DLL ships no external shader files.

Regenerate only after changing a shader:

```
pwsh shaders/compile.ps1
```

## Diagnostics

`tools/run-sc4.ps1` deploys a build, runs the game for a set time and collects
the log. Options are listed at the top of the script.

Set `LogLevel=trace` in `scvk.ini` to see everything the game asks of the
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
  (`scvk-key-N-draws.bin`), which `tools/draw-records.py` reads, and its log
  lists what kinds of draws made up each redrawn part (slow)
- `scvk-small-texture-pools`: makes room for only 64 textures at a time before
  scvk has to set aside more, so an ordinary session tests that it does
- `scvk-prefer-integrated-gpu`: runs on the processor's built-in graphics instead
  of the graphics card, to test another maker's driver on the same machine

## Benchmark

`tools/benchmark.ps1` compares scvk with the game's own DirectX 7 driver on a city
of your choice:

```
pwsh tools/benchmark.ps1 -Region "Blackfall" -City "Ostton" -Runs 2
```

Each run starts the game through Steam, which asks for a click, and the rest
happens by itself. scvk opens the region and the city, pauses it and times every
frame through eight camera scenes: a still view, scrolling at each zoom level, a
full turn and a run of zoom changes. It then writes `scvk-benchmark.csv` and
quits. The runs alternate between the two renderers, and
`tools/benchmark-report.py` turns them into average fps, 1% and 0.1% lows and
load times for each.

DirectX runs keep scvk loaded without its renderer, so both get the same frame
pacing fixes. Without them a paused city sits at 30 frames a second on DirectX,
and the comparison would measure that instead. A single run can also be started
by hand from the `[Benchmark]` section of `scvk.ini`.

## Installing

scvk needs Windows 10 or 11, a graphics card with Vulkan support and game version
641.

Download the zip from the [releases page](https://github.com/aspctt/scvk/releases)
and copy `scvk.dll` and `scvk.ini` into the `Plugins` folder of your SimCity 4
installation. That is all: the game picks scvk by default. On a machine without
Vulkan, the game keeps its own DirectX renderer.

With [sc4-graphics-options](https://github.com/0xC0000054/sc4-graphics-options),
both `Driver=DirectX` and `Driver=OpenGL` select scvk. With scd3d11 installed as
well, `DirectX` selects scd3d11 and `OpenGL` selects scvk. scvk draws in 32-bit
whatever `ColorDepth` says. Set it to 32 for windowed play: at 16, SC4's default,
the game switches to fullscreen whenever the desktop runs at a different depth, with
any renderer.

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
> The game starts a driver for every slot and keeps them all until it exits, so
> the log shows a second scvk driver that starts, never draws and shuts down at
> exit. That is the slot the game is not using.

A `scvk.log` file is written next to the DLL, falling back to the temp
directory if the Plugins folder is not writable. `LogLevel` in `scvk.ini` sets
how much goes into it, from `trace` to `off`, and is `info` by default.

To turn scvk off without removing it, keep `scvk.ini` next to the DLL and set
`Enabled=false` under `[Admin]`. The game itself reads that setting, so scvk
is never loaded at all. That is also the way back to the game's DirectX renderer.

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
