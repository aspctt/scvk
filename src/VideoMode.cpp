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

//// Dependencies

#include "VideoMode.h"
#include "Benchmark.h"
#include "Logger.h"
#include "VulkanBackend.h"

#include <cGZDisplayMetrics.h>
#include <cIGZApp.h>
#include <cIGZCmdLine.h>
#include <cIGZFrameWork.h>
#include <cIGZGraphicSystem.h>
#include <cIGZSystemService.h>
#include <cISC4App.h>
#include <cRZAutoRefCount.h>
#include <cRZBaseString.h>
#include <cRZCOMDllDirector.h>
#include <GZServPtrs.h>
#include <SC4Preferences.h>

#include <Windows.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <wchar.h>

namespace scvk
{
	//// Types

	namespace
	{
		struct LaunchSize
		{
			uint32_t width;
			uint32_t height;
		};
	}

	//// Constants

	namespace
	{
		constexpr uint32_t COLOUR_DEPTH = 32;

		// The preferences' depth before anything sets one. The game then takes the
		// desktop's depth (0x44ce40), which is 32 on every Windows scvk runs on.
		constexpr uint32_t DESKTOP_COLOUR_DEPTH = 0;

		// SC4GraphicsOptions' DLL, by the name its release ships it under.
		constexpr char const* GRAPHICS_OPTIONS_MODULE_NAME = "SC4GraphicsOptions.dll";

		// SC4GraphicsOptions' settings, which it reads from its own folder, and the one
		// that names its window mode. It takes any value starting with Borderless, in any
		// case, as its borderless mode.
		constexpr wchar_t const* GRAPHICS_OPTIONS_SETTINGS_NAME = L"SC4GraphicsOptions.ini";
		constexpr wchar_t const* GRAPHICS_OPTIONS_SECTION       = L"GraphicsOptions";
		constexpr wchar_t const* GRAPHICS_OPTIONS_WINDOW_MODE   = L"WindowMode";
		constexpr wchar_t const* GRAPHICS_OPTIONS_BORDERLESS    = L"Borderless";
		constexpr DWORD          WINDOW_MODE_LENGTH             = 64;

		// The launch options, as the game looks them up (0x44eed0, 0x44ba80).
		constexpr char        RESOLUTION_SWITCH        = 'r';
		constexpr char const* RESOLUTION_FORMAT        = "%ux%ux%u";
		constexpr char const* CUSTOM_RESOLUTION_SWITCH = "CustomResolution";

		// The game's switches for a window and for fullscreen (0x44f310, 0x44f2a3), then
		// scvk's longer names for them. The game reads -f before -w, so a window wins
		// when both are given.
		constexpr char const* WINDOWED_SWITCHES[]   = { "w", "windowed" };
		constexpr char const* FULLSCREEN_SWITCHES[] = { "f", "fullscreen" };

		// The -CustomResolution values the game takes as on (0x44ba80). It keeps its own
		// size for any other.
		constexpr char const* CUSTOM_RESOLUTION_ON_VALUES[] = { "on", "enabled" };

		// scvk's own launch options for a borderless window. The game matches the whole
		// name of a switch, in any case (0x914001), and reads none of these itself.
		constexpr char const* BORDERLESS_SWITCHES[] = { "b", "borderless" };

		// The preferences' renderer for the game's Software renderer, which its options
		// and -d:soft choose (0x44d029). Any other value picks a slot scvk serves.
		constexpr uint8_t SOFTWARE_DRIVER_TYPE = 0;

		// The service that corrects the mode. The graphics system is a service too, at
		// priority 1500000 (0x98fdc0), and the framework starts services from the highest
		// priority down, after every plugin's PreFrameWorkInit (0x879f15). One step above
		// it, the correction is the last change to the mode before the graphics system
		// reads it.
		constexpr uint32_t VIDEO_MODE_SERVICE_ID       = 0x5C4B0003;
		constexpr int32_t  VIDEO_MODE_SERVICE_PRIORITY = 1500001;
	}

	//// State

	namespace
	{
		bool isVideoModeApplied = false;
		bool isBorderless       = false;
		bool isServiceInstalled = false;
	}

	//// Private Functions

	namespace
	{
		/** Whether -CustomResolution was given a value that turns it off, such as disabled. */
		bool IsCustomResolutionTurnedOff(cIGZCmdLine const& commandLine)
		{
			cRZBaseString value;
			if (!commandLine.IsSwitchPresent(cRZBaseString(CUSTOM_RESOLUTION_SWITCH), value, true) || value.Strlen() == 0)
			{
				return false;
			}

			for (char const* const onValue : CUSTOM_RESOLUTION_ON_VALUES)
			{
				if (_stricmp(value.ToChar(), onValue) == 0)
				{
					return false;
				}
			}

			return true;
		}

		/** The size the -r launch option asks for, when it names one. */
		bool ReadLaunchSize(cIGZCmdLine const& commandLine, LaunchSize& outSize)
		{
			cRZBaseString value;
			if (!commandLine.IsSwitchPresent(RESOLUTION_SWITCH, value, true))
			{
				return false;
			}

			// Read it as the game does (0x44f212), which keeps the preferences' size when
			// the width or the height is missing.
			unsigned int width  = 0;
			unsigned int height = 0;
			unsigned int depth  = 0;

			if (sscanf_s(value.ToChar(), RESOLUTION_FORMAT, &width, &height, &depth) < 2 || width == 0 || height == 0)
			{
				return false;
			}

			outSize = { width, height };
			return true;
		}

		/** Whether the launch options include any of the named switches. */
		template <size_t COUNT>
		bool HasAnySwitch(cIGZCmdLine const* commandLine, char const* const (&names)[COUNT])
		{
			if (commandLine == nullptr)
			{
				return false;
			}

			for (char const* const name : names)
			{
				if (commandLine->GetIndexOfSwitch(cRZBaseString(name), 0) >= 0)
				{
					return true;
				}
			}

			return false;
		}

		/** Whether scvk will be the renderer, as far as can be told this early. */
		bool IsScvkDrawing(SC4VideoPreferences const& video)
		{
			return video.driverType != SOFTWARE_DRIVER_TYPE && !IsBenchmarkEnabled() && IsVulkanAvailable();
		}

		/** Whether SC4GraphicsOptions is set to its borderless mode. */
		bool IsGraphicsOptionsBorderless(HMODULE graphicsOptions)
		{
			// Find its settings next to it
			//
			// Read through the wide functions, since the folder can be anywhere the
			// player keeps plugins, under any name.
			wchar_t     path[MAX_PATH] = {};
			DWORD const pathLength     = GetModuleFileNameW(graphicsOptions, path, MAX_PATH);

			if (pathLength == 0 || pathLength >= MAX_PATH)
			{
				return false;
			}

			wchar_t* const separator = wcsrchr(path, L'\\');
			if (separator == nullptr)
			{
				return false;
			}

			separator[1] = L'\0';
			if (wcscat_s(path, MAX_PATH, GRAPHICS_OPTIONS_SETTINGS_NAME) != 0)
			{
				return false;
			}

			// Read its window mode the way it does
			wchar_t windowMode[WINDOW_MODE_LENGTH] = {};
			GetPrivateProfileStringW(GRAPHICS_OPTIONS_SECTION, GRAPHICS_OPTIONS_WINDOW_MODE, L"", windowMode, WINDOW_MODE_LENGTH, path);

			return _wcsnicmp(windowMode, GRAPHICS_OPTIONS_BORDERLESS, wcslen(GRAPHICS_OPTIONS_BORDERLESS)) == 0;
		}

		/** Asks for a window the screen's size, at the screen's colour depth. */
		void RequestBorderlessWindow(cIGZGraphicSystem& graphicSystem, cGZDisplayMetrics const& screen)
		{
			// A window has to have the screen's colour depth and fit on it, or the game
			// makes it fullscreen (0x990a3a), so this is the largest window there is.
			cGZDisplayMetrics request{};
			request.width    = screen.width;
			request.height   = screen.height;
			request.bitDepth = screen.bitDepth;

			graphicSystem.PreInitSetDesiredGameResolution(request);
			graphicSystem.PreInitSetWindowedMode(true);

			LogInfo("Borderless: the game runs in a %ux%u window that covers the main monitor.", request.width, request.height);
		}

		/** Corrects the mode the game is about to ask its graphics system for. */
		void ApplyVideoMode(void)
		{
			if (isVideoModeApplied)
			{
				return;
			}

			isVideoModeApplied = true;

			// Find the game's preferences and its graphics system
			cIGZApp* const application = RZGetFrameWork()->Application();

			cRZAutoRefCount<cISC4App> game;
			if (application == nullptr || !application->QueryInterface(GZIID_cISC4App, game.AsPPVoid()))
			{
				LogWarn("Video mode: the game is not available yet, so its video mode is left alone.");
				return;
			}

			SC4Preferences* const preferences = game->GetPreferences();
			if (preferences == nullptr)
			{
				LogWarn("Video mode: the game has no preferences yet, so its video mode is left alone.");
				return;
			}

			cIGZGraphicSystemPtr graphicSystem;
			if (!graphicSystem)
			{
				LogWarn("Video mode: the graphics system is not available, so the video mode is left alone.");
				return;
			}

			SC4VideoPreferences& video = preferences->videoPreferences;

			cGZDisplayMetrics screen{};
			graphicSystem->GetScreenMetrics(screen);

			cIGZCmdLine* const commandLine = RZGetFrameWork()->CommandLine();

			// See whether a borderless window was asked for
			//
			// The game loads every plugin before it starts any service, so
			// SC4GraphicsOptions is in the process by now if it is installed at all.
			HMODULE const graphicsOptions = GetModuleHandleA(GRAPHICS_OPTIONS_MODULE_NAME);

			if (HasAnySwitch(commandLine, BORDERLESS_SWITCHES))
			{
				isBorderless = true;
				LogInfo("Borderless: asked for by a launch option.");
			}
			else if (graphicsOptions != nullptr && IsGraphicsOptionsBorderless(graphicsOptions))
			{
				isBorderless = true;
				LogInfo("Borderless: asked for by SC4GraphicsOptions' window mode.");
			}

			// Leave the size and colour depth to SC4GraphicsOptions when it is installed
			//
			// Borderless still takes the window it needs. SC4GraphicsOptions asks for the
			// same window in its own borderless mode, but at its own colour depth, and a
			// depth unlike the screen's makes the game fullscreen.
			if (graphicsOptions != nullptr)
			{
				LogInfo("SC4GraphicsOptions is installed, so its settings decide the size and colour depth.");

				if (isBorderless)
				{
					RequestBorderlessWindow(*graphicSystem, screen);
				}

				return;
			}

			// Go borderless unless a launch option chose the mode or the size
			//
			// Only while scvk draws. The game's own renderers would make a framed window
			// the desktop's size, and a benchmark compares the renderers in the player's
			// mode.
			LaunchSize size{};
			bool const hasLaunchSize      = commandLine != nullptr && ReadLaunchSize(*commandLine, size);
			bool const isWindowWanted     = HasAnySwitch(commandLine, WINDOWED_SWITCHES);
			bool const isFullscreenWanted = HasAnySwitch(commandLine, FULLSCREEN_SWITCHES);

			if (!isBorderless && !hasLaunchSize && !isWindowWanted && !isFullscreenWanted)
			{
				if (IsScvkDrawing(video))
				{
					isBorderless = true;
					LogInfo("Borderless: the default, since no launch option chose a window mode or size.");
				}
				else
				{
					LogInfo("Borderless: not the default, since scvk is not the renderer this time.");
				}
			}

			// Use the -r size as given
			//
			// The game read -r and replaced its size in its own PreFrameWorkInit, which
			// runs before the plugins' hooks (0x87afdd), so it is read again here. A
			// borderless window keeps the monitor's size.
			bool const isSizeChanged = hasLaunchSize && !isBorderless && !IsCustomResolutionTurnedOff(*commandLine) && (size.width != video.width || size.height != video.height);

			if (hasLaunchSize && isBorderless)
			{
				LogInfo("Borderless: leaving out the -r size %ux%u, since the window covers the main monitor.", size.width, size.height);
			}

			if (isSizeChanged)
			{
				LogInfo("Using the -r size %ux%u instead of the game's %ux%u.", size.width, size.height, video.width, video.height);

				video.width  = size.width;
				video.height = size.height;
			}

			// Choose a window or fullscreen
			//
			// The game sets its own -w and -f in the preferences (0x44f367, 0x44f2ff) but
			// knows nothing of the longer names, and a size its -r swap made larger than
			// the screen was made fullscreen too, so the choice is made again here. A
			// window larger than the screen becomes fullscreen, as the game would make it
			// (0x44cf6a).
			bool const wasFullscreen = video.bFullScreen != 0;
			bool       isFullscreen  = wasFullscreen;

			if (isWindowWanted)
			{
				isFullscreen = false;
			}
			else if (isFullscreenWanted)
			{
				isFullscreen = true;
			}

			if (video.width > screen.width || video.height > screen.height)
			{
				isFullscreen = true;
			}

			bool const isModeChanged = isFullscreen != wasFullscreen && !isBorderless;

			if (isModeChanged)
			{
				LogInfo("Running %s instead of %s.", isFullscreen ? "fullscreen" : "in a window", wasFullscreen ? "fullscreen" : "in a window");
				video.bFullScreen = isFullscreen ? 1 : 0;
			}

			// Use 32-bit colour
			uint32_t const previousDepth  = video.bitDepth;
			bool const     isDepthChanged = previousDepth != COLOUR_DEPTH && previousDepth != DESKTOP_COLOUR_DEPTH;

			if (isDepthChanged)
			{
				video.bitDepth = COLOUR_DEPTH;
				game->SavePreferences();

				LogInfo("Changed the game's colour depth from %u to %u bits.", previousDepth, COLOUR_DEPTH);
			}

			// Ask for the mode again
			//
			// The game has already turned its preferences into the mode it asks the
			// graphics system for (0x44ce40), so the request is set again the way it
			// does: a window takes the screen's depth, and so does fullscreen while the
			// preferences hold none.
			if (isBorderless)
			{
				RequestBorderlessWindow(*graphicSystem, screen);
				return;
			}

			if (!isSizeChanged && !isModeChanged && !isDepthChanged)
			{
				return;
			}

			cGZDisplayMetrics request{};
			request.width    = video.width;
			request.height   = video.height;
			request.bitDepth = (isFullscreen && video.bitDepth != DESKTOP_COLOUR_DEPTH) ? video.bitDepth : screen.bitDepth;

			graphicSystem->PreInitSetDesiredGameResolution(request);
			graphicSystem->PreInitSetWindowedMode(!isFullscreen);
		}

		/**
		 * Corrects the video mode when the framework starts it. Static, and never deleted,
		 * so its reference count only answers the framework.
		 */
		class VideoModeService final : public cIGZSystemService
		{
		public:
			bool QueryInterface(uint32_t riid, void** ppvObj) override
			{
				if (riid == GZIID_cIGZSystemService || riid == GZIID_cIGZUnknown)
				{
					*ppvObj = static_cast<cIGZSystemService*>(this);
					AddRef();
					return true;
				}

				*ppvObj = nullptr;
				return false;
			}

			uint32_t AddRef(void) override
			{
				return ++referenceCount;
			}

			uint32_t Release(void) override
			{
				if (referenceCount > 0)
				{
					--referenceCount;
				}

				return referenceCount;
			}

			uint32_t GetServiceID(void) override
			{
				return serviceId;
			}

			cIGZSystemService* SetServiceID(uint32_t newServiceId) override
			{
				serviceId = newServiceId;
				return this;
			}

			int32_t GetServicePriority(void) override
			{
				return VIDEO_MODE_SERVICE_PRIORITY;
			}

			bool IsServiceRunning(void) override
			{
				return isRunning;
			}

			cIGZSystemService* SetServiceRunning(bool shouldRun) override
			{
				isRunning = shouldRun;
				return this;
			}

			bool Init(void) override
			{
				ApplyVideoMode();
				return true;
			}

			bool Shutdown(void) override
			{
				return true;
			}

			bool OnTick([[maybe_unused]] uint32_t unknown) override
			{
				return true;
			}

			bool OnIdle([[maybe_unused]] uint32_t unknown) override
			{
				return true;
			}

			int32_t GetServiceTickPriority(void) override
			{
				return 0;
			}

		private:
			uint32_t referenceCount = 0;
			uint32_t serviceId      = VIDEO_MODE_SERVICE_ID;
			bool     isRunning      = false;
		};

		VideoModeService& Service(void)
		{
			static VideoModeService service;
			return service;
		}
	}

	//// Public API

	void InstallVideoModeService(void)
	{
		if (isServiceInstalled)
		{
			return;
		}

		cIGZFrameWork* const frameWork = RZGetFrameWork();
		if (frameWork == nullptr || !frameWork->AddSystemService(&Service()))
		{
			LogWarn("Video mode: could not add the service that corrects it, so the game's video mode is left alone.");
			return;
		}

		isServiceInstalled = true;
	}

	void RemoveVideoModeService(void)
	{
		if (!isServiceInstalled)
		{
			return;
		}

		isServiceInstalled = false;

		cIGZFrameWork* const frameWork = RZGetFrameWork();
		if (frameWork != nullptr)
		{
			frameWork->RemoveSystemService(&Service());
		}
	}

	bool IsBorderlessWindow(void)
	{
		return isBorderless;
	}
}
