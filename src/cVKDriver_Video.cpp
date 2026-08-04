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

#include "cVKDriver.h"
#include "Logger.h"
#include "version.h"

#include <Windows.h>

namespace scvk
{
	namespace
	{
		// The fixed function interface exposes two texture stages, and the
		// combiner state the game sends is written against that assumption.
		constexpr uint32_t kTextureStageCount = 2;

		char const* const kWindowClassName = "GDriverClass--scvk";
		char const* const kWindowName      = "GDriverWindow--scvk";
	}

	bool cVKDriver::Init(void)
	{
		LogOpen();
		SCVK_CALL("");

		LogNote("scvk %s initialising. No Vulkan device is created at this stage;", SCVK_VERSION_STRING);
		LogNote("this build exists to record how SimCity 4 drives the interface.");

		// Shaped to match what the game's own drivers report, because we do not
		// know how this string is parsed. SCGL, which works, produces eight
		// newline-separated fields: a three field header, then five describing
		// the device. The first build of scvk emitted only five fields total,
		// which is a candidate explanation for the game rejecting it. Cheap to
		// align, so align it.
		driverInfo.clear();
		driverInfo.append("Maxis 3D GDriver\n");
		driverInfo.append("Vulkan\n");
		driverInfo.append("1.0\n");
		driverInfo.append("UnknownDriverName\n");
		driverInfo.append("scvk " SCVK_VERSION_STRING "\n");
		driverInfo.append("scvk stub driver\n");
		driverInfo.append("UnknownCardVersion\n");
		driverInfo.append("scvk stub driver\n");

		int modes = EnumerateVideoModes();
		if (modes == 0)
		{
			LogNote("FATAL: no usable video modes were enumerated. The game will fall back to software.");
			SetLastError(DriverError::CreateContextFailed);
			return false;
		}

		LogNote("Enumerated %d video modes (windowed and fullscreen pairs):", modes);

		// Dumped in full because a mismatch here is a prime suspect if the game
		// rejects the driver: it asks for a specific width, height and colour
		// depth, and modern Windows generally only reports 32bpp modes. If the
		// game wants 16bpp and every mode below says 32, that is the answer.
		for (int i = 0; i < videoModeCount; i += 2)
		{
			sGDMode const& mode = videoModes[i];
			LogNote("    [%2d/%2d] %ux%u %ubpp", i, i + 1, mode.width, mode.height, mode.depth);
		}

		SetLastError(DriverError::OK);
		return true;
	}

	bool cVKDriver::Shutdown(void)
	{
		SCVK_CALL("");

		DestroyRenderWindow();
		UnregisterClassA(kWindowClassName, GetModuleHandleA(nullptr));

		// Summary only. The log stays open, because the game may well shut this
		// driver down as part of probing it and then come back for a second
		// lifecycle, and that second pass is the interesting one.
		LogSummary("driver Shutdown");
		return true;
	}

	int cVKDriver::EnumerateVideoModes(void)
	{
		videoModes.clear();
		videoModeCount = 0;

		DEVMODEA displayMode{};
		displayMode.dmSize = sizeof(DEVMODEA);

		for (DWORD i = 0; EnumDisplaySettingsA(nullptr, i, &displayMode) != 0; i++)
		{
			uint32_t depth = displayMode.dmBitsPerPel;
			if (depth < 15)
			{
				continue;
			}

			bool duplicate = false;
			for (sGDMode const& existing : videoModes)
			{
				if (existing.width == displayMode.dmPelsWidth &&
					existing.height == displayMode.dmPelsHeight &&
					existing.depth == depth)
				{
					duplicate = true;
					break;
				}
			}

			if (duplicate)
			{
				continue;
			}

			sGDMode mode{};

			// Without this the game reports "Could not initialize the hardware
			// driver" and silently drops to software rendering, which would
			// make this whole experiment produce an empty log.
			mode.isInitialized = true;

			mode.textureStageCount = kTextureStageCount;

			// Advertised against what a Vulkan implementation will actually be
			// able to do, not against what these stubs do. Claiming less would
			// steer the game down fallback paths we do not want to map.
			mode.supportsStencilBuffer        = true;
			mode.supportsMultitexture         = true;
			mode.supportsTextureEnvCombine    = true;
			mode.supportsFogCoord             = true;
			mode.supportsDxtTextures          = true;
			mode.supportsNvTextureEnvCombine4 = false;

			// Purpose unknown; the game's own OpenGL driver sets them this way.
			mode.__unknown2   = true;
			mode.__unknown5[0] = false;
			mode.__unknown5[1] = false;
			mode.__unknown5[2] = false;

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

			// Each resolution is offered twice, fullscreen and windowed, which
			// is the shape the game expects the mode list to have.
			mode.index        = videoModeCount++;
			mode.isFullscreen = true;
			videoModes.push_back(mode);

			mode.index        = videoModeCount++;
			mode.isFullscreen = false;
			videoModes.push_back(mode);
		}

		return videoModeCount;
	}

	uint32_t cVKDriver::CountVideoModes(void) const
	{
		SCVK_CALL("");
		return static_cast<uint32_t>(videoModeCount);
	}

	void cVKDriver::GetVideoModeInfo(uint32_t dwIndex, sGDMode& gdMode)
	{
		SCVK_CALL("%u", dwIndex);

		if (dwIndex >= static_cast<uint32_t>(videoModeCount))
		{
			LogNote("  !! index %u is out of range, we only have %d modes", dwIndex, videoModeCount);
			SetLastError(DriverError::OutOfRange);
			return;
		}

		gdMode = videoModes[dwIndex];
	}

	void cVKDriver::GetVideoModeInfo(sGDMode& gdMode)
	{
		SCVK_CALL("current");

		if (currentVideoMode < 0)
		{
			LogNote("  !! no video mode has been set yet");
			SetLastError(DriverError::OutOfRange);
			return;
		}

		GetVideoModeInfo(static_cast<uint32_t>(currentVideoMode), gdMode);
	}

	void cVKDriver::SetVideoMode(int32_t newModeIndex, void* hwndProc, bool showWindow, bool unknown)
	{
		SCVK_CALL("%d, %p, %d, %d", newModeIndex, hwndProc, showWindow, unknown);

		if (newModeIndex == -1)
		{
			if (windowHandle != nullptr)
			{
				ShowWindow(static_cast<HWND>(windowHandle), SW_HIDE);
			}

			currentVideoMode = -1;
			windowWidth      = 0;
			windowHeight     = 0;

			SetLastError(DriverError::OK);
			return;
		}

		if (newModeIndex < 0 || newModeIndex >= videoModeCount)
		{
			LogNote("SetVideoMode: index %d out of range (have %d modes).", newModeIndex, videoModeCount);
			SetLastError(DriverError::OutOfRange);
			return;
		}

		sGDMode const& mode = videoModes[newModeIndex];

		currentVideoMode = newModeIndex;
		windowWidth      = mode.width;
		windowHeight     = mode.height;

		LogNote("SetVideoMode: %ux%u %ubpp %s",
			mode.width, mode.height, mode.depth, mode.isFullscreen ? "fullscreen" : "windowed");

		DestroyRenderWindow();

		WNDCLASSA wc{};
		wc.style         = CS_OWNDC;
		wc.lpfnWndProc   = DefWindowProcA;
		wc.hInstance     = GetModuleHandleA(nullptr);
		wc.lpszClassName = kWindowClassName;

		UnregisterClassA(kWindowClassName, wc.hInstance);

		if (RegisterClassA(&wc) == 0 && GetLastError() != ERROR_CLASS_ALREADY_EXISTS)
		{
			LogNote("SetVideoMode: RegisterClass failed, error %lu.", GetLastError());
			SetLastError(DriverError::CreateContextFailed);
			return;
		}

		// Fullscreen is left alone at this stage. A mode change would take the
		// desktop with it, and recovering from a crash in a stub driver that
		// has just switched resolution is needlessly unpleasant. Windowed is
		// enough to reach a first frame.
		DWORD style    = WS_SYSMENU | WS_MINIMIZEBOX | WS_CAPTION | WS_CLIPSIBLINGS | WS_CLIPCHILDREN;
		DWORD extStyle = WS_EX_APPWINDOW | WS_EX_WINDOWEDGE;

		if (mode.isFullscreen)
		{
			LogNote("SetVideoMode: fullscreen requested; running windowed instead at this stage.");
		}

		RECT rect{ 0, 0, windowWidth, windowHeight };
		AdjustWindowRectEx(&rect, style, FALSE, extStyle);
		OffsetRect(&rect, 0, GetSystemMetrics(SM_CYCAPTION));

		HWND hwnd = CreateWindowExA(
			extStyle,
			kWindowClassName,
			kWindowName,
			style,
			rect.left,
			rect.top,
			rect.right - rect.left,
			rect.bottom - rect.top,
			nullptr,
			nullptr,
			wc.hInstance,
			nullptr);

		if (hwnd == nullptr)
		{
			LogNote("SetVideoMode: CreateWindowEx failed, error %lu.", GetLastError());
			SetLastError(DriverError::CreateContextFailed);
			return;
		}

		windowHandle = hwnd;

		// The game hands us its own window procedure and expects input to
		// arrive through it. Without this the window exists but the game never
		// sees a message.
		if (hwndProc != nullptr)
		{
			SetWindowLongPtrA(hwnd, GWLP_WNDPROC, reinterpret_cast<LONG_PTR>(hwndProc));
		}

		ShowWindow(hwnd, showWindow ? SW_SHOWNORMAL : SW_HIDE);

		SetViewport();
		SetLastError(DriverError::OK);
	}

	void cVKDriver::DestroyRenderWindow(void)
	{
		if (windowHandle != nullptr)
		{
			DestroyWindow(static_cast<HWND>(windowHandle));
			windowHandle = nullptr;
		}
	}

	bool cVKDriver::IsDeviceReady(void)
	{
		SCVK_CALL("");
		return windowHandle != nullptr;
	}

	void cVKDriver::Flush(void)
	{
		// The frame boundary. The game believes this swaps buffers, so this is
		// where the swapchain present will go.
		SCVK_CALL("");
	}

	void cVKDriver::SetViewport(void)
	{
		SCVK_CALL("");

		viewportX      = 0;
		viewportY      = 0;
		viewportWidth  = windowWidth;
		viewportHeight = windowHeight;
	}

	void cVKDriver::SetViewport(int32_t x, int32_t y, int32_t width, int32_t height)
	{
		SCVK_CALL("%d, %d, %d, %d", x, y, width, height);

		viewportX      = x;
		viewportY      = y;
		viewportWidth  = width;
		viewportHeight = height;
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
