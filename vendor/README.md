# Vendored third-party sources

Every file in this directory is an **unmodified verbatim copy** from an upstream
project, with its original copyright and license notice intact. All are
**LGPL-2.1-or-later**, matching scvk's own license.

Nothing here is scvk's own work. Do not edit these files in place. See
[Updating](#updating) below.

## Why these files exist

SimCity 4 loads renderers through its GZCOM plugin system, and `cIGZGDriver` is
an ABI contract with the game's 22-year-old binary: the vtable order and
parameter types are fixed by SC4.exe, not by us. These headers are the
reverse-engineered description of that contract. They are a dependency in the
strictest sense: scvk cannot invent its own version and still load.

The compiled code here is deliberately tiny: three `.cpp` files that do COM
registration, string handling, and reference counting. Everything else is pure
interface declaration.

## Provenance

### gzcom-dll (19 files)

<https://github.com/nsgomez/gzcom-dll> · Copyright (C) 2016 Nelson Gomez,
(C) 2024 memo, and contributors · LGPL-2.1-or-later

The GZCOM plugin SDK. Source of the plugin ABI and the graphics-driver
interfaces.

| Path | Role |
|---|---|
| `include/cIGZUnknown.h` | COM base interface |
| `include/cIGZCOM.h`, `include/cIGZCOMDirector.h` | COM registration |
| `include/cRZCOMDllDirector.h`, `src/cRZCOMDllDirector.cpp` | DLL entry point and class-object registration |
| `include/cIGZFrameWork.h`, `include/cIGZFrameWorkHooks.h` | Game lifecycle hooks |
| `include/cIGZGDriver.h` | **The renderer interface scvk implements** (~90 methods) |
| `include/sGDMode.h` | Video mode descriptor, with field offsets documented |
| `include/cIGZGraphicSystem.h`, `include/cIGZGraphicSystem2.h` | Driver selection (`SetDefaultDriverClassID`) |
| `include/cIGZBuffer.h`, `include/cGZGPixelFormatDesc.h` | Pixel buffer and format description |
| `include/cIGZString.h`, `include/cRZBaseString.h`, `src/cRZBaseString.cpp` | Game-compatible string type |
| `include/cRZAutoRefCount.h` | Scoped refcount helper |
| `include/GZCLSIDDefs.h` | Well-known class IDs |
| `include/cRZRect.h` | Rectangle type |

### Scion (2 files)

<https://github.com/nsgomez/scion> · Copyright (C) 2021 Nelson Gomez ·
LGPL-2.1-or-later

| Path | Role |
|---|---|
| `include/cRZRefCount.h`, `src/cRZRefCount.cpp` | Reference-counting base for the driver object |

> **Taken from Scion upstream, deliberately.** SCGL vendors its own copy of
> `cRZRefCount`, but that copy is stale: its notice reads "version 2.1 of the
> License" with no "or (at your option) any later version" clause. Scion's
> current upstream version *is* or-later. Using upstream keeps the whole
> vendored tree uniformly relicensable.

Scion's implementation (`cGZFramework`, `cRZString`, …) and its bundled STLport
are **not** vendored and are not needed, because SimCity 4 already provides the
framework at runtime. Scion is otherwise reference material only.

### SCGL (7 files)

<https://github.com/nsgomez/scgl> · Copyright (C) 2025 Nelson Gomez ·
LGPL-2.1-or-later

Declarations for the optional driver extensions, which do not appear in
gzcom-dll, plus the vertex format decoder.

| Path | Role |
|---|---|
| `include/cGDCombiner.h` | Texture combiner state passed to `SetCombiner` |
| `include/ext/cIGZGDriverVertexBufferExtension.h` | Vertex buffer objects |
| `include/ext/cIGZGBufferRegionExtension.h` | Buffer region save/restore |
| `include/ext/cIGZGDriverLightingExtension.h` | Fixed-function lighting |
| `include/ext/cIGZGSnapshotExtension.h` | Framebuffer capture |
| `include/VertexFormatUtils.h`, `src/VertexFormatUtils.cpp` | Vertex format decoding |

> `VertexFormatUtils` is the one piece of vendored code that is an
> *implementation* rather than a declaration, and it is here because it is
> irreplaceable. SimCity 4 packs its vertex formats into a bitfield
> (`nRZSimGL::PackStandardVertexFormat`) and asks the driver to decode strides,
> element offsets and element counts back out of it. The game then does pointer
> arithmetic with the answers, so a wrong stride is not a rendering glitch, it
> is a crash. The layout was recovered by reverse engineering and would be very
> hard to re-derive independently.
>
> This is the concrete payoff of scvk being LGPL: the license is what makes
> reusing it legitimate rather than something to be worked around.

> SCGL's `vendor/framework/` headers are gzcom-dll's headers **with the LGPL
> notice comments removed**. scvk takes those files from gzcom-dll upstream
> instead, so notices stay intact as the license requires. Only the five files
> above, which are genuinely SCGL's own, are taken from SCGL.

## Build integration

Add `vendor/include` to the compiler's include path and compile the three files
in `vendor/src`. The layout matters: `ext/` must remain a subdirectory of
`include/`, because `cIGZGSnapshotExtension.h` includes `../cIGZBuffer.h`.
Keeping that structure is what lets every file stay byte-identical to upstream.

Only `stdint.h` and a handful of C++ standard headers (`cstdint`, `list`,
`string`, `type_traits`, `vector`, `unordered_map`) are needed from outside this
directory.

## Updating

Re-copy from upstream rather than patching in place, and re-check two things:

1. Every file still carries an intact LGPL notice **including** the "or (at your
   option) any later version" clause. A file that is 2.1-only would pin scvk's
   relicensing options.
2. `cIGZGDriver.h` still matches the vtable SC4 expects. A change in method
   order upstream is a silent ABI break: the game will call the wrong function
   rather than fail to load.

If a vendored file ever *does* need modifying, LGPL §2(a) requires the changed
file carry prominent notice of the change and its date. Prefer adding a wrapper
in scvk's own sources instead.
