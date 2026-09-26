/*
 * scvk - a native Vulkan renderer for SimCity 4
 *
 * Copyright (C) 2026 aspctt
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation, under
 * version 2.1 of the License, or (at your option) any later version.
 *
 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with this library; if not, see <https://www.gnu.org/licenses/>.
 */

/*
 * The driver's lifecycle, video modes, the window, frames and the viewport.
 */

//// Dependencies

#include "cVKDriver.h"
#include "Logger.h"
#include "VulkanBackend.h"
#include "version.h"

#include <Windows.h>
#include <string.h>

namespace scvk
{
	//// Constants

	namespace
	{
		// The fixed function interface exposes two texture stages, and the combiner state
		// the game sends is written against that assumption.
		constexpr uint32_t TEXTURE_STAGE_COUNT = 2;

		constexpr char const* WINDOW_CLASS_NAME = "GDriverClass--scvk";
		constexpr char const* WINDOW_NAME       = "GDriverWindow--scvk";

		// The game's window: a fixed size with a caption, no resizing.
		constexpr DWORD WINDOW_STYLE          = WS_SYSMENU | WS_MINIMIZEBOX | WS_CAPTION | WS_CLIPSIBLINGS | WS_CLIPCHILDREN;
		constexpr DWORD WINDOW_EXTENDED_STYLE = WS_EX_APPWINDOW | WS_EX_WINDOWEDGE;

		// Diagnostics that replace every colour on screen, each enabled by dropping a
		// marker file next to the driver. The pass marker names each pass by its blend
		// configuration. The channel markers show one shader input on its own, which says
		// whether a wrong colour arrived or was computed, in the order the backend
		// numbers them.
		constexpr char const* DEBUG_PASSES_MARKER       = "scvk-debug-passes";
		constexpr char const* SKIP_CLOUD_SHADOWS_MARKER = "scvk-skip-cloud-shadows";

		// Keeps every draw of the saved tiles for Scroll Lock to write out. It changes
		// nothing on screen but costs time and memory, so it is opt-in too.
		constexpr char const* RECORD_TILE_DRAWS_MARKER = "scvk-record-tile-draws";

		// The log names a channel by its marker without this common prefix.
		constexpr char const* CHANNEL_MARKER_PREFIX = "scvk-debug-";
		constexpr char const* CHANNEL_MARKERS[] = {
			"scvk-debug-texture-colour",
			"scvk-debug-texture-alpha",
			"scvk-debug-vertex-colour",
			"scvk-debug-vertex-alpha",
		};
	}

	//// Private Functions

	void cVKDriver::BuildDriverInformation(void)
	{
		// Shaped to match what the game's own drivers report, because we do not know how
		// this string is parsed. SCGL, which works, produces eight newline-separated
		// fields: a three field header, then five describing the device.
		driverInformation.clear();
		driverInformation.append("Maxis 3D GDriver\n");
		driverInformation.append("Vulkan\n");
		driverInformation.append(vulkan->ApiVersion()).append("\n");
		driverInformation.append("UnknownDriverName\n");
		driverInformation.append("scvk " SCVK_VERSION_STRING "\n");
		driverInformation.append(vulkan->DeviceName()).append("\n");
		driverInformation.append("UnknownCardVersion\n");
		driverInformation.append(vulkan->DeviceName()).append("\n");
	}

	void cVKDriver::ApplyDiagnosticMarkers(void)
	{
		// Identify the passes
		if (HasMarkerFile(DEBUG_PASSES_MARKER))
		{
			vulkan->SetDebugPassColours(true);
		}

		// Leave out the cloud shadows
		if (HasMarkerFile(SKIP_CLOUD_SHADOWS_MARKER))
		{
			LogNote("Diagnostic: skipping the cloud shadow pass.");
			shouldSkipCloudShadows = true;
		}

		// Record the draws of the saved tiles
		//
		// Sized once, because the game calls Init more than once.
		if (HasMarkerFile(RECORD_TILE_DRAWS_MARKER) && drawRing.empty())
		{
			LogNote("Diagnostic: recording the draws of saved tiles.");
			drawRing.resize(DRAW_RING_SIZE);
		}

		// Show one input channel, the first one marked
		int channel = 0;

		for (char const* marker : CHANNEL_MARKERS)
		{
			if (HasMarkerFile(marker))
			{
				LogNote("Diagnostic: drawing %s only.", marker + strlen(CHANNEL_MARKER_PREFIX));
				vulkan->SetDebugChannel(channel);
				break;
			}

			channel++;
		}
	}

	uint32_t cVKDriver::EnumerateVideoModes(void)
	{
		videoModes.clear();

		DEVMODEA displayMode{};
		displayMode.dmSize = sizeof(DEVMODEA);

		for (DWORD i = 0; EnumDisplaySettingsA(nullptr, i, &displayMode) != 0; i++)
		{
			// Skip palettised modes and repeats of one already listed
			uint32_t const depth = displayMode.dmBitsPerPel;
			if (depth < 15)
			{
				continue;
			}

			bool isDuplicate = false;
			for (sGDMode const& existing : videoModes)
			{
				if (existing.width == displayMode.dmPelsWidth && existing.height == displayMode.dmPelsHeight && existing.depth == depth)
				{
					isDuplicate = true;
					break;
				}
			}

			if (isDuplicate)
			{
				continue;
			}

			// Describe what the device can do
			//
			// Without isInitialized the game reports "Could not initialize the hardware
			// driver" and silently drops to software rendering. The capabilities are
			// advertised against what a Vulkan implementation can do; claiming less would
			// steer the game down fallback paths.
			sGDMode mode{};
			mode.isInitialized     = true;
			mode.textureStageCount = TEXTURE_STAGE_COUNT;

			mode.supportsStencilBuffer        = true;
			mode.supportsMultitexture         = true;
			mode.supportsTextureEnvCombine    = true;
			mode.supportsFogCoord             = true;
			mode.supportsDxtTextures          = true;
			mode.supportsNvTextureEnvCombine4 = false;

			// Purpose unknown; the game's own OpenGL driver sets them this way.
			mode.__unknown2    = 1;
			mode.__unknown5[0] = 0;
			mode.__unknown5[1] = 0;
			mode.__unknown5[2] = 0;

			// Describe the pixel layout
			if (depth > 16)
			{
				mode.alphaColorMask = 0xff000000;
				mode.redColorMask   = 0x00ff0000;
				mode.greenColorMask = 0x0000ff00;
				mode.blueColorMask  = 0x000000ff;
			}
			else
			{
				mode.alphaColorMask = 0x1;
				mode.redColorMask   = 0xf800;
				mode.greenColorMask = 0x7c0;
				mode.blueColorMask  = 0x3e;
			}

			mode.width  = displayMode.dmPelsWidth;
			mode.height = displayMode.dmPelsHeight;
			mode.depth  = depth;

			// Offer it twice, fullscreen and windowed
			//
			// That is the shape the game expects the mode list to have.
			mode.index        = videoModes.size();
			mode.isFullscreen = true;
			videoModes.push_back(mode);

			mode.index        = videoModes.size();
			mode.isFullscreen = false;
			videoModes.push_back(mode);
		}

		return videoModes.size();
	}

	bool cVKDriver::CreateRenderWindow(sGDMode const& mode, void* windowProcedure)
	{
		DestroyRenderWindow();

		// Register the window class
		WNDCLASSA windowClass{};
		windowClass.style         = CS_OWNDC;
		windowClass.lpfnWndProc   = DefWindowProcA;
		windowClass.hInstance     = GetModuleHandleA(nullptr);
		windowClass.lpszClassName = WINDOW_CLASS_NAME;

		UnregisterClassA(WINDOW_CLASS_NAME, windowClass.hInstance);

		if (RegisterClassA(&windowClass) == 0 && GetLastError() != ERROR_CLASS_ALREADY_EXISTS)
		{
			LogNote("SetVideoMode: RegisterClass failed, error %lu.", GetLastError());
			return false;
		}

		// Size the window around the client area
		//
		// Fullscreen is left alone for now. A mode change would take the desktop with it,
		// and recovering from a crash in a driver that has just switched resolution is
		// needlessly unpleasant. Windowed is enough to play.
		if (mode.isFullscreen)
		{
			LogNote("SetVideoMode: fullscreen requested; running windowed instead at this stage.");
		}

		RECT rectangle{ 0, 0, windowWidth, windowHeight };
		AdjustWindowRectEx(&rectangle, WINDOW_STYLE, FALSE, WINDOW_EXTENDED_STYLE);
		OffsetRect(&rectangle, 0, GetSystemMetrics(SM_CYCAPTION));

		// Create it
		HWND const window = CreateWindowExA(WINDOW_EXTENDED_STYLE, WINDOW_CLASS_NAME, WINDOW_NAME, WINDOW_STYLE, rectangle.left, rectangle.top, rectangle.right - rectangle.left, rectangle.bottom - rectangle.top, nullptr, nullptr, windowClass.hInstance, nullptr);

		if (window == nullptr)
		{
			LogNote("SetVideoMode: CreateWindowEx failed, error %lu.", GetLastError());
			return false;
		}

		windowHandle = window;

		// Route its messages to the game
		//
		// The game hands us its own window procedure and expects input to arrive through
		// it. Without this the window exists but the game never sees a message. The API
		// stores the procedure as an integer.
		if (windowProcedure != nullptr)
		{
			SetWindowLongPtrA(window, GWLP_WNDPROC, reinterpret_cast<LONG_PTR>(windowProcedure));
		}

		// Show it, whatever the game's flags say
		//
		// The third parameter of SetVideoMode looks like it should mean "show the
		// window", and treating it that way is what an earlier build did. The game passes
		// 0 for it, so the window was hidden and the driver spent an entire run rendering
		// and presenting perfectly into something nobody could see. SCGL, which works,
		// also ignores both flags and always shows. Whatever they mean, it is not this.
		ShowWindow(window, SW_SHOWNORMAL);
		return true;
	}

	void cVKDriver::DestroyRenderWindow(void)
	{
		if (windowHandle == nullptr)
		{
			return;
		}

		// The interface carries the window as a plain pointer.
		DestroyWindow(static_cast<HWND>(windowHandle));
		windowHandle = nullptr;
	}

	//// Public API

	bool cVKDriver::Init(void)
	{
		LogOpen();
		SCVK_CALL("");

		LogNote("scvk %s initialising.", SCVK_VERSION_STRING);

		// Decline when Vulkan is unavailable
		//
		// A Vulkan renderer that cannot reach Vulkan is of no use to anyone. Reporting
		// failure here lets the game fall back to a renderer that works, which is far
		// better than presenting a black window.
		if (!vulkan->CreateInstance())
		{
			LogNote("Vulkan is unavailable, so scvk is declining to act as the renderer. The game will fall back to another driver.");
			SetLastError(DriverError::CREATE_CONTEXT_FAILED);
			return false;
		}

		BuildDriverInformation();
		ApplyDiagnosticMarkers();

		// List the video modes
		uint32_t const modeCount = EnumerateVideoModes();
		if (modeCount == 0)
		{
			LogNote("FATAL: no usable video modes were enumerated. The game will fall back to software.");
			SetLastError(DriverError::CREATE_CONTEXT_FAILED);
			return false;
		}

		// Log them in full
		//
		// A mismatch here is a prime suspect if the game rejects the driver: it asks for
		// a specific width, height and colour depth, and modern Windows generally only
		// reports 32bpp modes. If the game wants 16bpp and every mode below says 32, that
		// is the answer.
		LogNote("Enumerated %u video modes (windowed and fullscreen pairs):", modeCount);

		for (uint32_t i = 0; i < modeCount; i += 2)
		{
			sGDMode const& mode = videoModes[i];
			LogNote("    [%2u/%2u] %ux%u %ubpp", i, i + 1, mode.width, mode.height, mode.depth);
		}

		SetLastError(DriverError::OK);
		return true;
	}

	bool cVKDriver::Shutdown(void)
	{
		SCVK_CALL("");

		// Release the device and the window
		vulkan->Destroy();

		DestroyRenderWindow();
		UnregisterClassA(WINDOW_CLASS_NAME, GetModuleHandleA(nullptr));

		// Write a summary but keep the log open
		//
		// The game may well shut this driver down as part of probing it and then come
		// back for a second lifecycle, and that second pass is the interesting one.
		LogSummary("driver Shutdown");
		return true;
	}

	uint32_t cVKDriver::CountVideoModes(void) const
	{
		SCVK_CALL("");
		return videoModes.size();
	}

	void cVKDriver::GetVideoModeInfo(uint32_t modeIndex, sGDMode& outMode)
	{
		SCVK_CALL("%u", modeIndex);

		if (modeIndex >= videoModes.size())
		{
			LogNote("  !! index %u is out of range, we only have %u modes", modeIndex, videoModes.size());
			SetLastError(DriverError::OUT_OF_RANGE);
			return;
		}

		outMode = videoModes[modeIndex];
	}

	void cVKDriver::GetVideoModeInfo(sGDMode& outMode)
	{
		SCVK_CALL("current");

		if (currentVideoMode < 0)
		{
			LogNote("  !! no video mode has been set yet");
			SetLastError(DriverError::OUT_OF_RANGE);
			return;
		}

		// Not negative, checked just above.
		GetVideoModeInfo(static_cast<uint32_t>(currentVideoMode), outMode);
	}

	void cVKDriver::SetVideoMode(int32_t newModeIndex, void* windowProcedure, bool isUnknownFlag1Set, bool isUnknownFlag2Set)
	{
		SCVK_CALL("%d, %p, %d, %d", newModeIndex, windowProcedure, isUnknownFlag1Set, isUnknownFlag2Set);

		// Hide the window when the game unsets the mode
		if (newModeIndex == -1)
		{
			if (windowHandle != nullptr)
			{
				// The interface carries the window as a plain pointer.
				ShowWindow(static_cast<HWND>(windowHandle), SW_HIDE);
			}

			currentVideoMode = -1;
			windowWidth      = 0;
			windowHeight     = 0;

			SetLastError(DriverError::OK);
			return;
		}

		// Refuse a mode that does not exist
		//
		// A negative index is refused first, so the rest can use it unsigned.
		if (newModeIndex < 0 || static_cast<uint32_t>(newModeIndex) >= videoModes.size())
		{
			LogNote("SetVideoMode: index %d out of range (have %u modes).", newModeIndex, videoModes.size());
			SetLastError(DriverError::OUT_OF_RANGE);
			return;
		}

		// Adopt the mode
		//
		// Mode sizes come from the display settings, far below INT_MAX.
		sGDMode const& mode = videoModes[static_cast<uint32_t>(newModeIndex)];

		currentVideoMode = newModeIndex;
		windowWidth      = static_cast<int>(mode.width);
		windowHeight     = static_cast<int>(mode.height);

		LogNote("SetVideoMode: %ux%u %ubpp %s", mode.width, mode.height, mode.depth, mode.isFullscreen ? "fullscreen" : "windowed");

		// Create the window, then attach Vulkan to it
		//
		// The surface has to come after the window is created and shown, and the
		// swapchain after that, so this is the earliest point any of it can exist.
		if (!CreateRenderWindow(mode, windowProcedure))
		{
			SetLastError(DriverError::CREATE_CONTEXT_FAILED);
			return;
		}

		if (!vulkan->CreateSurfaceAndDevice(windowHandle, mode.width, mode.height))
		{
			LogNote("Vulkan: could not attach to the window; nothing will be drawn.");
			SetLastError(DriverError::CREATE_CONTEXT_FAILED);
			return;
		}

		SetViewport();
		SetLastError(DriverError::OK);
	}

	bool cVKDriver::IsDeviceReady(void)
	{
		SCVK_CALL("");
		return windowHandle != nullptr && vulkan->IsReady();
	}

	void cVKDriver::Flush(void)
	{
		// The frame boundary. The game believes this swaps buffers, and for us it submits
		// the recorded commands and presents.
		SCVK_CALL("");

		EndFrameDiagnostics();
		vulkan->Present();
	}

	void cVKDriver::SetViewport(void)
	{
		SCVK_CALL("");

		viewportX      = 0;
		viewportY      = 0;
		viewportWidth  = windowWidth;
		viewportHeight = windowHeight;

		vulkan->SetFullViewport();
	}

	void cVKDriver::SetViewport(int32_t x, int32_t y, int32_t width, int32_t height)
	{
		SCVK_CALL("%d, %d, %d, %d", x, y, width, height);

		viewportX      = x;
		viewportY      = y;
		viewportWidth  = width;
		viewportHeight = height;

		NoteRegionSubViewport();

		// The game pairs each sub-viewport with a projection matched to it. Dropping this
		// on the floor was what stretched a 667 pixel wide region across the whole 1920
		// pixel window.
		vulkan->SetViewport(x, y, width, height);
	}

	void cVKDriver::GetViewport(int32_t dimensions[4])
	{
		SCVK_CALL("");

		// Reported as edges rather than extents, matching the game's driver.
		dimensions[0] = viewportX;
		dimensions[1] = viewportY;
		dimensions[2] = viewportX + viewportWidth;
		dimensions[3] = viewportY + viewportHeight;
	}
}
