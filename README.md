# scvk

A native Vulkan renderer for SimCity 4.

SimCity 4 shipped in 2003 with a DirectX 7 renderer and an unfinished OpenGL
one. Both have aged badly against modern drivers. scvk replaces the renderer
with one built on Vulkan, loaded as a DLL plugin through the game's own GZCOM
plugin system.

This project is not affiliated with, endorsed by, or supported by Electronic
Arts Inc. or Maxis.

## Status

**Early. Nothing is drawn yet, but the game does drive the driver.**

SimCity 4 selects scvk as its renderer, initialises it, sets a 1920x1080 video
mode, queries the buffer region and snapshot extensions, and runs frames
through it. Every renderer call is traced to `scvk.log`. That trace is the map
for the actual Vulkan work.

| Component | Status |
|---|---|
| GZCOM registration and driver selection | Working in game |
| Video mode enumeration and `SetVideoMode` | Working in game |
| Vertex format decoding | Working (vendored from SCGL) |
| Call tracing | Working |
| Vulkan instance, device, swapchain | Not started |
| 2D blits (startup and loading screens) | Not started, **next** |
| Fixed function emulation (ubershader, pipeline cache) | Not started |
| Textures and combiners | Not started |

What the first in-game trace established:

- The game reaches a steady render loop of roughly 169 driver calls per frame,
  built around a repeating 26-call state block.
- `Flush` is the frame boundary, confirmed.
- `InterleavedArrays(format 1, stride 16)` matches the vendored vertex format
  decoder exactly, so that code is correct.
- **2D blits are on the critical path, not incidental.** `StretchBlt` is how
  the startup and loading screens reach the display, so it needs a real
  implementation before the draw path does.
- The game calls `NewBufferRegion` without first calling
  `BufferRegionEnabled`, so declining an extension is not sufficient to stop
  it being used.

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

### SC4Fix: fully compatible

[SC4Fix](https://github.com/nsgomez/sc4fix) is safe to run alongside scvk, and
this is guaranteed by construction rather than by testing. The two touch
completely disjoint parts of the game.

SC4Fix works by patching machine code at hardcoded addresses. On game version
641 it touches four places:

| Address | What it fixes |
|---|---|
| `0x87B3D1` | Stops the game unloading plugin DLLs |
| `0x65EE3E`, `0x65EE66` | Null dereference on puzzle pieces over LE lots |
| `0x96DA1D` | The same crash, second site |
| `0x5D3DE0` | Prop pox, a save corruption bug |

**scvk patches no game code at all by default.** It registers a COM class and
implements an interface, which is the mechanism the game itself provides for
replacing a renderer. There is no address in common because scvk uses no
addresses. The only exception is the opt-in FPS setting below, which is off
unless you turn it on and touches three bytes in an unrelated function.

SC4Fix's DLL unload patch is mildly helpful to scvk: it stops the game
unloading plugins it does not recognise, which removes a class of shutdown
race entirely.

### FPS limits: built in, off by default

SimCity 4 caps its frame rate by simulation speed, at 30 for Turtle, 20 for
Rhino and 15 for Cheetah. scvk can raise those caps itself, so
[sc4-disable-fps-limits](https://github.com/caspervg/sc4-disable-fps-limits) is
not required, though it remains compatible.

Set `MaxFPS` in `scvk.ini` to enable it:

```ini
[scvk]
MaxFPS=120
```

This lives in scvk because frame pacing and presentation are one concern. Once
the swapchain exists, the present mode and this cap have to agree, and keeping
them in separate plugins means two settings files that can contradict one
another.

It is off by default for two reasons. It is the only part of scvk that writes
to game memory, and caspervg's plugin does the same job, so enabling both with
different values would be confusing. **If you already use that plugin, leave
`MaxFPS` at 0.**

scvk is more cautious than it strictly needs to be here. It requires game
version 641, and it reads each byte before changing it: if a byte does not hold
the value it expects, it declines and writes the reason to the log instead of
overwriting whatever is actually there. Running both plugins is therefore
untidy but not dangerous.

## Building

Requires Visual Studio 2022 or later with the desktop C++ workload. There are
no external dependencies.

```
msbuild scvk.sln /p:Configuration=Release /p:Platform=Win32
```

SimCity 4 is a 32-bit process, so **Win32 is the only supported platform**.
There is deliberately no x64 configuration.

## Installing

1. Copy `scvk.dll` into the `Plugins` folder of your SimCity 4 installation.
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
directory if the Plugins folder is not writable.

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
