#!/usr/bin/env bash
#
# Builds scvk.dll on Linux with the MinGW-w64 cross compiler.
#
# The DLL is the same 32-bit Windows library Visual Studio builds, the one SimCity 4
# loads on Windows and under Wine or Proton on Linux. The release is built with
# Visual Studio; this is for building and checking it from Linux.
#
# What it needs:
#   - i686-w64-mingw32-g++, the POSIX threads flavour (Debian and Ubuntu: g++-mingw-w64-i686),
#     and the i686-w64-mingw32-windres that package brings along (WINDRES_MINGW names another)
#   - nothing else: the Vulkan headers are in vendor/vulkan (VULKAN_INCLUDE names others)
#
# It also runs on Windows from Git Bash, with WinLibs' i686 GCC on the PATH and
# WINDRES_MINGW=windres, since that toolchain's windres has no prefix.
#
# Usage: tools/build-mingw.sh [Release|Debug]
#
# The output is Release-mingw/scvk.dll (or Debug-mingw/scvk.dll).
#
# The game-side shadow modules (-LiveShadows: LiveShadowHooks, ShadowRegistry and
# TerrainShadows) hook the game with MSVC inline assembly and structured exception
# handling, so they are left out of this build and stay off; everything else is the same.

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

# The flags are arrays rather than strings, since the include paths hold the repository's
# path, which may have spaces in it ("SimCity 4"). A string expanded unquoted splits there.

# The same definitions and language as the Visual Studio project
#
# The project names no Windows version, so the Windows SDK takes its newest, Windows 10
# (0x0A00). MinGW-w64's headers take theirs from how they were built: Ubuntu's are new
# enough, while WinLibs' say XP (0x0501) and hide GetTickCount64 and CreateWaitableTimerExW.
defines=(-DWIN32 -D_WINDOWS -D_USRDLL -DWIN32_LEAN_AND_MEAN -DNOMINMAX -D_CRT_SECURE_NO_WARNINGS -D_WIN32_WINNT=0x0A00)
if [ "$configuration" = "Debug" ]; then
	defines+=(-D_DEBUG)
	optimisation=(-O0 -g)
else
	defines+=(-DNDEBUG)
	optimisation=(-O2)
fi

# The ReShade headers include <Windows.h>, which only a case-insensitive file system finds
# as MinGW's <windows.h>
#
# The shim passes on with #include_next, which searches only the directories after its
# own. On a case-insensitive file system, as on Windows, every <windows.h> finds the shim
# first, and a plain #include would find it again and nest until the compiler gives up.
mkdir -p "$repository/obj/mingw/compat"
echo '#include_next <windows.h>' > "$repository/obj/mingw/compat/Windows.h"

includes=("-I$repository/obj/mingw/compat" "-I$repository/vendor/include" "-I$repository/vendor/reshade/include" "-I$repository/src" "-I$vulkanInclude")

# SSE2 for floats and vectors, which Visual Studio's 32-bit compiler uses unless told
# otherwise. The index range scan uses SSE2 intrinsics, which GCC's i686 target refuses
# without it. MinGW's GCC realigns the stack wherever SSE needs it, since 32-bit Windows
# only keeps it 4-byte aligned.
instructions=(-msse2 -mfpmath=sse)

warnings=(-Wall -Wno-unknown-pragmas -Wno-unused-function -Wno-unused-variable -Wno-cast-function-type -Wno-attributes -Wno-ignored-qualifiers -Wno-missing-field-initializers -Wno-unused-parameter)

flags=(-std=c++20 "${optimisation[@]}" "${instructions[@]}" "${defines[@]}" "${includes[@]}" "${warnings[@]}")

mkdir -p "$objects" "$output"

# Compile each source
objectFiles=()
for source in "$repository"/src/*.cpp "$repository"/vendor/src/*.cpp; do
	object="$objects/$(basename "${source%.cpp}").o"
	objectFiles+=("$object")
	"$compiler" "${flags[@]}" -c "$source" -o "$object"
done

# Compile the version resource with the windres beside the compiler
#
# The same prefix names both, and MinGW-w64's binutils, which the compiler package
# depends on, holds it.
#
# windres hands the input file and its include paths to the preprocessor as one command
# line without quoting them, so a space in the repository's path breaks it. It runs from
# inside src/ with a relative name instead, where version.h is found beside the script.
windres="${WINDRES_MINGW:-${compiler%g++*}windres}"
resource="$objects/scvk.res.o"
(cd "$repository/src" && "$windres" -O coff scvk.rc -o "$resource")
objectFiles+=("$resource")

# Link, with the exports the game and other plugins look up by name
#
# Statically, so the DLL needs no MinGW runtime DLLs beside it, which neither Windows
# nor Wine has.
"$compiler" -shared -o "$output/scvk.dll" "${objectFiles[@]}" "$repository/tools/scvk-mingw.def" \
	-static -static-libgcc -static-libstdc++ -Wl,--enable-stdcall-fixup \
	-luser32 -lgdi32 -lversion

echo "build-mingw: $output/scvk.dll"
