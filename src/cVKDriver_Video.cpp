/*
 * scvk - a native Vulkan renderer for SimCity 4
 *
 * Copyright (C) 2026 aspctt
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU Lesser General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public License
 * along with this program.  If not, see <https://www.gnu.org/licenses/>.
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

		// The names the game's own OpenGL driver gives its window, whose class ID scvk
		// claims. SC4GraphicsOptions recognises the game's window by them, and its
		// borderless fullscreen mode reshapes only a window it recognises.
		constexpr char const* WINDOW_CLASS_NAME = "GDriverClass--OpenGL";
		constexpr char const* WINDOW_NAME       = "GDriverWindow--OpenGL";

		// The game's window: a fixed size with a caption, no resizing.
		constexpr DWORD WINDOW_STYLE          = WS_SYSMENU | WS_MINIMIZEBOX | WS_CAPTION | WS_CLIPSIBLINGS | WS_CLIPCHILDREN;
		constexpr DWORD WINDOW_EXTENDED_STYLE = WS_EX_APPWINDOW | WS_EX_WINDOWEDGE;

		// The fullscreen window: no frame, above every other window, as SCGL makes it.
		constexpr DWORD FULLSCREEN_STYLE          = WS_POPUP | WS_CLIPSIBLINGS | WS_CLIPCHILDREN | WS_MAXIMIZE;
		constexpr DWORD FULLSCREEN_EXTENDED_STYLE = WS_EX_APPWINDOW | WS_EX_TOPMOST;

		// Diagnostics that replace every colour on screen, each enabled by dropping a
		// marker file next to the driver. The pass marker names each pass by its blend
		// configuration. The channel markers show one shader input on its own, which says
		// whether a wrong colour arrived or was computed, in the order the backend
		// numbers them.
		constexpr char const* DEBUG_PASSES_MARKER       = "scvk-debug-passes";
		constexpr char const* SKIP_CLOUD_SHADOWS_MARKER = "scvk-skip-cloud-shadows";

		// Draws the fog the game's 3D view would send if its fog were switched on, which
		// no code in the game was found to do. It is the only way to see the fog at all.
		constexpr char const* FORCE_FOG_MARKER = "scvk-force-fog";

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

	//// State

	namespace
	{
		// The game's window and fullscreen, for the one window there is at a time. They
		// live here rather than on the driver because the window procedure has to reach
		// them, and Windows hands a procedure nothing but the window.
		HWND     gameWindow            = nullptr;
		WNDPROC  gameWindowProcedure   = nullptr;
		bool     isFullscreenWindow    = false;
		DEVMODEA fullscreenDisplayMode = {};
		bool     isDisplayModeChanged  = false;

		// How many more focus and size messages, and faults, the log describes.
		int  windowMessageReportsRemaining = 48;
		int  exceptionReportsRemaining     = 8;
		bool isExceptionLogInstalled       = false;
	}

	//// Private Functions

	namespace
	{
		// Switches the main display to the fullscreen mode, or says why it could not
		bool EnterDisplayMode(void)
		{
			if (isDisplayModeChanged)
			{
				return true;
			}

			// Marked temporary, as SCGL does. LeaveDisplayMode puts the desktop's back.
			LONG const result = ChangeDisplaySettingsExA(nullptr, &fullscreenDisplayMode, nullptr, CDS_FULLSCREEN, nullptr);

			if (result != DISP_CHANGE_SUCCESSFUL)
			{
				LogWarn("SetVideoMode: could not switch the display to %lux%lu %lubpp, error %ld.", fullscreenDisplayMode.dmPelsWidth, fullscreenDisplayMode.dmPelsHeight, fullscreenDisplayMode.dmBitsPerPel, result);
				return false;
			}

			isDisplayModeChanged = true;

			// Report the mode the display took
			//
			// The request names no refresh rate, as SCGL's does not, so Windows picks one.
			// Off the monitor's native size it is worth knowing which.
			DEVMODEA current{};
			current.dmSize = sizeof(current);

			if (EnumDisplaySettingsA(nullptr, ENUM_CURRENT_SETTINGS, &current) != 0)
			{
				LogInfo("Fullscreen: the display is now %lux%lu %lubpp at %lu Hz.", current.dmPelsWidth, current.dmPelsHeight, current.dmBitsPerPel, current.dmDisplayFrequency);
			}

			return true;
		}

		// Puts the desktop's own display mode back
		void LeaveDisplayMode(void)
		{
			if (!isDisplayModeChanged)
			{
				return;
			}

			// No mode and no flags restores the one in the registry.
			ChangeDisplaySettingsExA(nullptr, nullptr, nullptr, 0, nullptr);
			isDisplayModeChanged = false;
		}

		// Covers the main monitor with the window, above every other window
		void CoverMainMonitor(HWND window)
		{
			MONITORINFO monitor{};
			monitor.cbSize = sizeof(monitor);

			POINT const origin{ 0, 0 };
			if (GetMonitorInfoA(MonitorFromPoint(origin, MONITOR_DEFAULTTOPRIMARY), &monitor) == 0)
			{
				return;
			}

			RECT const& bounds = monitor.rcMonitor;
			SetWindowPos(window, HWND_TOPMOST, bounds.left, bounds.top, bounds.right - bounds.left, bounds.bottom - bounds.top, SWP_NOACTIVATE);
		}

		// Describes the window's style, place and focus, for working out how another
		// plugin has reshaped it
		void LogWindowState(HWND window, char const* moment)
		{
			RECT bounds{};
			RECT client{};
			GetWindowRect(window, &bounds);
			GetClientRect(window, &client);

			// The style words are bit sets the API returns as signed integers.
			unsigned long const style         = static_cast<unsigned long>(GetWindowLongA(window, GWL_STYLE));
			unsigned long const extendedStyle = static_cast<unsigned long>(GetWindowLongA(window, GWL_EXSTYLE));

			LogDebug("Window %s: style 0x%08lx extended 0x%08lx, bounds %ld,%ld..%ld,%ld, client %ldx%ld, visible %d minimised %d maximised %d foreground %d focus %d.", moment, style, extendedStyle, bounds.left, bounds.top, bounds.right, bounds.bottom, client.right, client.bottom, IsWindowVisible(window) ? 1 : 0, IsIconic(window) ? 1 : 0, IsZoomed(window) ? 1 : 0, (GetForegroundWindow() == window) ? 1 : 0, (GetFocus() == window) ? 1 : 0);
		}

		// Names the focus and size messages worth a log line, or returns null
		char const* DescribeWindowMessage(UINT message)
		{
			switch (message)
			{
			case WM_ACTIVATEAPP:      return "WM_ACTIVATEAPP";
			case WM_ACTIVATE:         return "WM_ACTIVATE";
			case WM_SETFOCUS:         return "WM_SETFOCUS";
			case WM_KILLFOCUS:        return "WM_KILLFOCUS";
			case WM_SIZE:             return "WM_SIZE";
			case WM_SHOWWINDOW:       return "WM_SHOWWINDOW";
			case WM_DISPLAYCHANGE:    return "WM_DISPLAYCHANGE";
			case WM_WINDOWPOSCHANGED: return "WM_WINDOWPOSCHANGED";
			default:                  return nullptr;
			}
		}

		// Sees the game window's messages before the game does
		LRESULT CALLBACK GameWindowProcedure(HWND window, UINT message, WPARAM wParam, LPARAM lParam)
		{
			// Report the first focus and size messages
			//
			// Both parameters are logged as raw words, whatever the message packs in them.
			char const* const name = DescribeWindowMessage(message);

			if (name != nullptr && windowMessageReportsRemaining > 0)
			{
				windowMessageReportsRemaining--;
				LogDebug("Window message %s, wParam 0x%lx, lParam 0x%lx.", name, static_cast<unsigned long>(wParam), static_cast<unsigned long>(lParam));
			}

			// Give the desktop back while a fullscreen game is in the background
			//
			// Switching away puts the desktop's mode back and minimises the game, since a
			// window above every other one would otherwise hide whatever was switched to.
			// Switching back sets the game's mode again.
			if (message == WM_ACTIVATEAPP && isFullscreenWindow && window == gameWindow)
			{
				LogDebug("Fullscreen: the game is %s.", (wParam != FALSE) ? "back in front" : "in the background");

				if (wParam != FALSE)
				{
					EnterDisplayMode();

					if (IsIconic(window))
					{
						ShowWindow(window, SW_RESTORE);
					}

					CoverMainMonitor(window);
					LogWindowState(window, "back in front");
				}
				else
				{
					LeaveDisplayMode();
					ShowWindow(window, SW_MINIMIZE);
				}
			}

			// Pass everything on to the game
			WNDPROC const next = (gameWindowProcedure != nullptr) ? gameWindowProcedure : DefWindowProcA;
			return CallWindowProcA(next, window, message, wParam, lParam);
		}

		// Stops treating the window as fullscreen and puts the desktop's mode back
		void EndFullscreen(void)
		{
			isFullscreenWindow = false;
			LeaveDisplayMode();
		}

		// Describes a fault in the log before the game's own handler ends the process
		//
		// The game installs an exception filter of its own and exits without a word, so
		// Windows records nothing either. A vectored handler runs before any filter. Only
		// faults are reported, not the exceptions C++ code throws and catches.
		LONG CALLBACK LogException(EXCEPTION_POINTERS* exception)
		{
			EXCEPTION_RECORD const* const record = exception->ExceptionRecord;
			DWORD const                   code   = record->ExceptionCode;

			bool const isFault = code == EXCEPTION_ACCESS_VIOLATION || code == EXCEPTION_INT_DIVIDE_BY_ZERO || code == EXCEPTION_FLT_DIVIDE_BY_ZERO || code == EXCEPTION_ILLEGAL_INSTRUCTION || code == EXCEPTION_PRIV_INSTRUCTION || code == EXCEPTION_STACK_OVERFLOW || code == EXCEPTION_ARRAY_BOUNDS_EXCEEDED;

			if (!isFault || exceptionReportsRemaining <= 0)
			{
				return EXCEPTION_CONTINUE_SEARCH;
			}

			exceptionReportsRemaining--;

			// Name the module the fault happened in
			HMODULE module               = nullptr;
			char    modulePath[MAX_PATH] = "unknown";

			if (GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT, static_cast<LPCSTR>(record->ExceptionAddress), &module) != 0)
			{
				GetModuleFileNameA(module, modulePath, MAX_PATH);
			}

			// Addresses are 32-bit in this process, so they fit an unsigned long. A module
			// handle is the module's base address.
			unsigned long const address = static_cast<unsigned long>(reinterpret_cast<uintptr_t>(record->ExceptionAddress));
			unsigned long const base    = static_cast<unsigned long>(reinterpret_cast<uintptr_t>(module));
			unsigned long const target  = (record->NumberParameters >= 2) ? static_cast<unsigned long>(record->ExceptionInformation[1]) : 0;

			LogCritical("Exception 0x%08lx at 0x%08lx (%s +0x%lx), data address 0x%08lx, thread %lu.", code, address, modulePath, address - base, target, GetCurrentThreadId());
			return EXCEPTION_CONTINUE_SEARCH;
		}
	}

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
			LogInfo("Diagnostic: skipping the cloud shadow pass.");
			shouldSkipCloudShadows = true;
		}

		// Fog the scene as the 3D view would
		if (HasMarkerFile(FORCE_FOG_MARKER))
		{
			LogInfo("Diagnostic: forcing the 3D view's own fog.");
			shouldForceFog = true;
			PushFog();
		}

		// Record the draws of the saved tiles
		//
		// Sized once, because the game calls Init more than once.
		if (HasMarkerFile(RECORD_TILE_DRAWS_MARKER) && drawRing.empty())
		{
			LogInfo("Diagnostic: recording the draws of saved tiles.");
			drawRing.resize(DRAW_RING_SIZE);
		}

		// Show one input channel, the first one marked
		int channel = 0;

		for (char const* marker : CHANNEL_MARKERS)
		{
			if (HasMarkerFile(marker))
			{
				LogInfo("Diagnostic: drawing %s only.", marker + strlen(CHANNEL_MARKER_PREFIX));
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
			LogError("SetVideoMode: RegisterClass failed, error %lu.", GetLastError());
			return false;
		}

		// Switch the display for fullscreen
		//
		// Exclusive, as the game's own drivers do: the display takes the mode's size, and
		// the window covers it. A display that refuses the mode leaves the game windowed,
		// as SCGL does.
		bool isFullscreen = false;

		if (mode.isFullscreen)
		{
			fullscreenDisplayMode              = {};
			fullscreenDisplayMode.dmSize       = sizeof(fullscreenDisplayMode);
			fullscreenDisplayMode.dmPelsWidth  = mode.width;
			fullscreenDisplayMode.dmPelsHeight = mode.height;
			fullscreenDisplayMode.dmBitsPerPel = mode.depth;
			fullscreenDisplayMode.dmFields     = DM_BITSPERPEL | DM_PELSWIDTH | DM_PELSHEIGHT;

			isFullscreen = EnterDisplayMode();

			if (!isFullscreen)
			{
				LogWarn("SetVideoMode: running windowed instead.");
			}
		}

		// Size the window
		//
		// Fullscreen covers the main monitor, which has just taken the mode's size. A
		// window is sized around its client area.
		RECT  rectangle{ 0, 0, windowWidth, windowHeight };
		DWORD style         = isFullscreen ? FULLSCREEN_STYLE : WINDOW_STYLE;
		DWORD extendedStyle = isFullscreen ? FULLSCREEN_EXTENDED_STYLE : WINDOW_EXTENDED_STYLE;

		MONITORINFO monitor{};
		monitor.cbSize = sizeof(monitor);

		POINT const origin{ 0, 0 };
		if (isFullscreen && GetMonitorInfoA(MonitorFromPoint(origin, MONITOR_DEFAULTTOPRIMARY), &monitor) != 0)
		{
			rectangle = monitor.rcMonitor;
		}
		else if (!isFullscreen)
		{
			AdjustWindowRectEx(&rectangle, style, FALSE, extendedStyle);
			OffsetRect(&rectangle, 0, GetSystemMetrics(SM_CYCAPTION));
		}

		// Create it
		HWND const window = CreateWindowExA(extendedStyle, WINDOW_CLASS_NAME, WINDOW_NAME, style, rectangle.left, rectangle.top, rectangle.right - rectangle.left, rectangle.bottom - rectangle.top, nullptr, nullptr, windowClass.hInstance, nullptr);

		if (window == nullptr)
		{
			LogError("SetVideoMode: CreateWindowEx failed, error %lu.", GetLastError());
			EndFullscreen();
			return false;
		}

		windowHandle = window;
		LogWindowState(window, "created");

		// Start hidden, whatever made the window visible
		//
		// SC4GraphicsOptions' borderless mode creates the game's window visible and
		// maximised. Shown during creation, the window was activated before the game's
		// procedure was in place, so the game never heard it was active: it showed no
		// interface, then crashed in its sound code the next time it was activated.
		// Hidden again here and shown below, the game gets the same messages as in a
		// window. The plugin turns every ShowWindow on this window into a maximise, so
		// this hides it through SetWindowPos instead.
		if (IsWindowVisible(window))
		{
			SetWindowPos(window, nullptr, 0, 0, 0, 0, SWP_HIDEWINDOW | SWP_NOMOVE | SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE);
		}

		// Route its messages to the game
		//
		// The game hands us its own window procedure and expects input to arrive through
		// it. Without this the window exists but the game never sees a message. Ours sees
		// them first, to give the desktop back when a fullscreen game is switched away
		// from, and to log the focus and size messages.
		//
		// The game passes the procedure as a plain pointer, and the API stores one as an
		// integer.
		gameWindow          = window;
		gameWindowProcedure = reinterpret_cast<WNDPROC>(windowProcedure);
		isFullscreenWindow  = isFullscreen;
		SetWindowLongPtrA(window, GWLP_WNDPROC, reinterpret_cast<LONG_PTR>(&GameWindowProcedure));

		// Show it, whatever the game's flags say
		//
		// The third parameter of SetVideoMode looks like it should mean "show the
		// window", and treating it that way is what an earlier build did. The game passes
		// 0 for it, so the window was hidden and the driver spent an entire run rendering
		// and presenting perfectly into something nobody could see. SCGL, which works,
		// also ignores both flags and always shows. Whatever they mean, it is not this.
		ShowWindow(window, SW_SHOWNORMAL);
		LogWindowState(window, "shown");
		return true;
	}

	void cVKDriver::DestroyRenderWindow(void)
	{
		// Give the desktop its mode back first, so the window's last messages are not
		// taken for a switch away.
		EndFullscreen();

		if (windowHandle == nullptr)
		{
			return;
		}

		// The interface carries the window as a plain pointer.
		DestroyWindow(static_cast<HWND>(windowHandle));
		windowHandle        = nullptr;
		gameWindow          = nullptr;
		gameWindowProcedure = nullptr;
	}

	//// Public API

	bool cVKDriver::Init(void)
	{
		LogOpen();
		SCVK_CALL("");

		LogInfo("scvk %s initialising.", SCVK_VERSION_STRING);

		// Log faults, once for the process
		//
		// First in the chain, so it runs before any handler the game or another plugin adds.
		if (!isExceptionLogInstalled)
		{
			isExceptionLogInstalled = AddVectoredExceptionHandler(1, &LogException) != nullptr;
		}

		// Decline when Vulkan is unavailable
		//
		// A Vulkan renderer that cannot reach Vulkan is of no use to anyone. Reporting
		// failure here lets the game fall back to a renderer that works, which is far
		// better than presenting a black window.
		if (!vulkan->CreateInstance())
		{
			LogError("Vulkan is unavailable, so scvk is declining to act as the renderer. The game will fall back to another driver.");
			SetLastError(DriverError::CREATE_CONTEXT_FAILED);
			return false;
		}

		BuildDriverInformation();
		ApplyDiagnosticMarkers();

		// List the video modes
		uint32_t const modeCount = EnumerateVideoModes();
		if (modeCount == 0)
		{
			LogCritical("FATAL: no usable video modes were enumerated. The game will fall back to software.");
			SetLastError(DriverError::CREATE_CONTEXT_FAILED);
			return false;
		}

		// Log them in full
		//
		// A mismatch here is a prime suspect if the game rejects the driver: it asks for
		// a specific width, height and colour depth, and modern Windows generally only
		// reports 32bpp modes. If the game wants 16bpp and every mode below says 32, that
		// is the answer.
		LogInfo("Enumerated %u video modes, in windowed and fullscreen pairs.", modeCount);

		for (uint32_t i = 0; i < modeCount; i += 2)
		{
			sGDMode const& mode = videoModes[i];
			LogDebug("    [%2u/%2u] %ux%u %ubpp", i, i + 1, mode.width, mode.height, mode.depth);
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
			LogWarn("  !! index %u is out of range, we only have %u modes", modeIndex, videoModes.size());
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
			LogWarn("  !! no video mode has been set yet");
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
			EndFullscreen();

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
			LogWarn("SetVideoMode: index %d out of range (have %u modes).", newModeIndex, videoModes.size());
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

		LogInfo("SetVideoMode: %ux%u %ubpp %s", mode.width, mode.height, mode.depth, mode.isFullscreen ? "fullscreen" : "windowed");

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
			LogCritical("Vulkan: could not attach to the window; nothing will be drawn.");
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
