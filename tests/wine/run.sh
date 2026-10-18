#!/usr/bin/env bash
#
# Checks scvk.dll under Wine on Linux, without the game.
#
# Builds the DLL and the harness with MinGW, then, on a virtual X display with Mesa's
# software Vulkan driver (lavapipe) unless a real one is there:
#   1. draws 300 frames in a window at the default log level;
#   2. draws 2001 frames at the debug level and checks every square of scvk's frame
#      capture against the colour it was drawn with, through every draw path, the
#      upper half lit by a directional light through the lighting extension;
#   3. draws a quad through the texture environment's blend mode and checks its colour
#      and alpha;
#   4. draws one large quad under a point light and checks that its light falls off per
#      pixel, from the middle to the edge;
#   5. draws in exclusive fullscreen, presses PrintScreen as a real key, and checks the
#      frame scvk put on the clipboard.
#
# Needs: wine with 32-bit support, Xvfb, xdotool, mesa-vulkan-drivers:i386 and
# libvulkan1:i386, and what tools/build-mingw.sh needs.
#
# Usage: tests/wine/run.sh

set -euo pipefail

repository="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
work="$repository/obj/wine"
mkdir -p "$work"

# Build
"$repository/tools/build-mingw.sh" Release
i686-w64-mingw32-g++-posix -std=c++20 -O1 -I"$repository/vendor/include" -o "$work/harness.exe" "$repository/tests/wine/harness.cpp" "$repository/vendor/src/VertexFormatUtils.cpp" -static
cp "$repository/Release-mingw/scvk.dll" "$work/"

# A display and a 32-bit Wine prefix of their own
export WINEPREFIX="${WINEPREFIX:-$work/prefix}"
export WINEARCH=win32
export WINEDEBUG=-all

if [ -z "${DISPLAY:-}" ]; then
	export DISPLAY=:97
	Xvfb "$DISPLAY" -screen 0 1024x768x24 > /dev/null 2>&1 &
	display=$!
	trap 'kill $display 2> /dev/null || true' EXIT
	sleep 2
fi

[ -d "$WINEPREFIX" ] || wineboot -i > /dev/null 2>&1

cd "$work"
status=0

echo "== windowed, default log level"
rm -f scvk.log
wine harness.exe 300 || status=1

echo "== windowed, debug log level, every square checked"
rm -f scvk.log scvk-*.bmp
wine harness.exe 2001 -lighting -LogLevel:debug || status=1

echo "== texture environment blend"
rm -f scvk.log scvk-*.bmp
wine harness.exe 2001 -texenvblend -LogLevel:debug || status=1

echo "== a point light lit per pixel"
rm -f scvk.log scvk-*.bmp
wine harness.exe 2001 -pointlight -LogLevel:debug || status=1

echo "== exclusive fullscreen, PrintScreen to the clipboard"
rm -f scvk.log
(sleep 4; xdotool key Print) &
keyPress=$!
wine harness.exe 500 -fullscreen -checkclipboard || status=1
wait "$keyPress"
grep -q "PrintScreen: copied the frame" scvk.log || { echo "FAIL: no PrintScreen copy in the log"; status=1; }

if grep -E "^Exception 0x" scvk.log; then
	echo "FAIL: a fault in the log"
	status=1
fi

echo
[ "$status" -eq 0 ] && echo "ALL PASSED" || echo "SOME FAILED"
exit "$status"
