#!/usr/bin/env bash
#
# Builds scvk.dll on Linux with the MinGW-w64 cross compiler.
#
# The DLL is the same 32-bit Windows library Visual Studio builds, the one SimCity 4
# loads on Windows and under Wine or Proton on Linux. The release is built with
# Visual Studio; this is for building and checking it from Linux.
#
# What it needs:
#   - i686-w64-mingw32-g++, the POSIX threads flavour (Debian and Ubuntu: g++-mingw-w64-i686)
#   - nothing else: the Vulkan headers are in vendor/vulkan (VULKAN_INCLUDE names others)
#
# Usage: tools/build-mingw.sh [Release|Debug]
#
# The output is Release-mingw/scvk.dll (or Debug-mingw/scvk.dll).
#
# NativeShadowExperiment's hooks are built from GCC assembly here, the same instructions as
# the MSVC ones. The other game-side shadow modules (-NativeShadowMasks, the registry and
# the live shadow diagnostics) still hook the game with MSVC inline assembly and structured
# exception handling, so they are left out of this build and stay off; everything else is
# the same.

set -euo pipefail

configuration="${1:-Release}"
repository="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
objects="$repository/obj/mingw/$configuration"
output="$repository/$configuration-mingw"

# The POSIX threads flavour where the system has both, for the standard library's threads
compiler="${CXX_MINGW:-}"
if [ -z "$compiler" ]; then
	compiler="i686-w64-mingw32-g++"
	command -v i686-w64-mingw32-g++-posix > /dev/null && compiler="i686-w64-mingw32-g++-posix"
fi
if ! command -v "$compiler" > /dev/null; then
	echo "build-mingw: $compiler not found; install the MinGW-w64 i686 cross compiler." >&2
	exit 1
fi

# The Vulkan headers: the ones in vendor/vulkan unless VULKAN_INCLUDE names others
vulkanInclude="${VULKAN_INCLUDE:-$repository/vendor/vulkan/Include}"

# The same definitions and language as the Visual Studio project
defines="-DWIN32 -D_WINDOWS -D_USRDLL -DWIN32_LEAN_AND_MEAN -DNOMINMAX -D_CRT_SECURE_NO_WARNINGS"
if [ "$configuration" = "Debug" ]; then
	defines="$defines -D_DEBUG"
	optimisation="-O0 -g"
else
	defines="$defines -DNDEBUG"
	optimisation="-O2"
fi

# The ReShade headers include <Windows.h>, which only a case-insensitive file system finds
# as MinGW's <windows.h>
mkdir -p "$repository/obj/mingw/compat"
echo '#include <windows.h>' > "$repository/obj/mingw/compat/Windows.h"

includes="-I$repository/obj/mingw/compat -I$repository/vendor/include -I$repository/vendor/reshade/include -I$repository/include -I$repository/src -I$vulkanInclude"
flags="-std=c++20 $optimisation $defines $includes -Wall -Wno-unknown-pragmas -Wno-unused-function -Wno-unused-variable -Wno-cast-function-type -Wno-attributes -Wno-ignored-qualifiers -Wno-missing-field-initializers -Wno-unused-parameter"

mkdir -p "$objects" "$output"

# Compile each source
objectFiles=()
for source in "$repository"/src/*.cpp "$repository"/vendor/src/*.cpp; do
	object="$objects/$(basename "${source%.cpp}").o"
	objectFiles+=("$object")
	"$compiler" $flags -c "$source" -o "$object"
done

# Link, with the exports the game and other plugins look up by name
#
# Statically, so the DLL needs no MinGW runtime DLLs beside it, which neither Windows
# nor Wine has.
"$compiler" -shared -o "$output/scvk.dll" "${objectFiles[@]}" "$repository/tools/scvk-mingw.def" \
	-static -static-libgcc -static-libstdc++ -Wl,--enable-stdcall-fixup \
	-luser32 -lgdi32 -lversion

echo "build-mingw: $output/scvk.dll"
