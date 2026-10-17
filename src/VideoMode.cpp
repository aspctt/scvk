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
#include "Logger.h"

#include <cGZDisplayMetrics.h>
#include <cIGZApp.h>
#include <cIGZCmdLine.h>
#include <cIGZFrameWork.h>
#include <cIGZGraphicSystem.h>
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

		// The launch options, as the game looks them up (0x44eed0, 0x44ba80).
		constexpr char        RESOLUTION_SWITCH        = 'r';
		constexpr char const* RESOLUTION_FORMAT        = "%ux%ux%u";
		constexpr char const* CUSTOM_RESOLUTION_SWITCH = "CustomResolution";
		constexpr char const* WINDOWED_SWITCH          = "w";

		// The -CustomResolution values the game takes as on (0x44ba80). It keeps its own
		// size for any other.
		constexpr char const* CUSTOM_RESOLUTION_ON_VALUES[] = { "on", "enabled" };
	}

	//// State

	namespace
	{
		bool isVideoModeApplied = false;
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
	}

	//// Public API

	void ApplyVideoMode(void)
	{
		if (isVideoModeApplied)
		{
			return;
		}

		isVideoModeApplied = true;

		// Leave the mode to SC4GraphicsOptions when it is installed
		//
		// The game loads every plugin before the first PreFrameWorkInit, so it is in the
		// process by now if it is installed at all.
		if (GetModuleHandleA(GRAPHICS_OPTIONS_MODULE_NAME) != nullptr)
		{
			LogInfo("SC4GraphicsOptions is installed, so its settings decide the video mode.");
			return;
		}

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

		bool isFullscreen = video.bFullScreen != 0;

		// Use the -r size as given
		//
		// The game read -r and replaced its size in its own PreFrameWorkInit, which runs
		// before the plugins' hooks (0x87afdd), so it is read again here. A window larger
		// than the screen becomes fullscreen, as the game would make it (0x44cf6a). With
		// the game's size, that can also have happened to a window that fits, so -w
		// windows it again.
		cIGZCmdLine* const commandLine = RZGetFrameWork()->CommandLine();

		LaunchSize size{};
		bool const isSizeChanged = commandLine != nullptr && !IsCustomResolutionTurnedOff(*commandLine) && ReadLaunchSize(*commandLine, size) && (size.width != video.width || size.height != video.height);

		if (isSizeChanged)
		{
			LogInfo("Using the -r size %ux%u instead of the game's %ux%u.", size.width, size.height, video.width, video.height);

			video.width  = size.width;
			video.height = size.height;

			if (size.width > screen.width || size.height > screen.height)
			{
				isFullscreen = true;
			}
			else if (commandLine->GetIndexOfSwitch(cRZBaseString(WINDOWED_SWITCH), 0) >= 0)
			{
				isFullscreen = false;
			}

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
		// The game has already turned its preferences into the mode it asks the graphics
		// system for (0x44ce40), so the request is set again the way it does: a window
		// takes the screen's depth, and so does fullscreen while the preferences hold none.
		if (!isSizeChanged && !isDepthChanged)
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
}
