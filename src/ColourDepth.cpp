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

#include "ColourDepth.h"
#include "Logger.h"

#include <cGZDisplayMetrics.h>
#include <cIGZApp.h>
#include <cIGZFrameWork.h>
#include <cIGZGraphicSystem.h>
#include <cISC4App.h>
#include <cRZAutoRefCount.h>
#include <cRZCOMDllDirector.h>
#include <GZServPtrs.h>
#include <SC4Preferences.h>

#include <Windows.h>
#include <stdint.h>

namespace scvk
{
	//// Constants

	namespace
	{
		constexpr uint32_t COLOUR_DEPTH = 32;

		// The preferences' depth before anything sets one. The game then takes the
		// desktop's depth (0x44ce40), which is 32 on every Windows scvk runs on.
		constexpr uint32_t DESKTOP_COLOUR_DEPTH = 0;

		// SC4GraphicsOptions' DLL, by the name its release ships it under.
		constexpr char const* GRAPHICS_OPTIONS_MODULE_NAME = "SC4GraphicsOptions.dll";
	}

	//// State

	namespace
	{
		bool isColourDepthApplied = false;
	}

	//// Public API

	void ApplyColourDepth(void)
	{
		if (isColourDepthApplied)
		{
			return;
		}

		isColourDepthApplied = true;

		// Leave the depth to SC4GraphicsOptions when it is installed
		//
		// The game loads every plugin before the first PreFrameWorkInit, so it is in the
		// process by now if it is installed at all.
		if (GetModuleHandleA(GRAPHICS_OPTIONS_MODULE_NAME) != nullptr)
		{
			LogInfo("SC4GraphicsOptions is installed, so its ColorDepth setting decides the colour depth.");
			return;
		}

		// Find the game's preferences
		cIGZApp* const application = RZGetFrameWork()->Application();

		cRZAutoRefCount<cISC4App> game;
		if (application == nullptr || !application->QueryInterface(GZIID_cISC4App, game.AsPPVoid()))
		{
			LogWarn("Colour depth: the game is not available yet, so its colour depth is left alone.");
			return;
		}

		SC4Preferences* const preferences = game->GetPreferences();
		if (preferences == nullptr)
		{
			LogWarn("Colour depth: the game has no preferences yet, so its colour depth is left alone.");
			return;
		}

		// Leave a depth that is already 32 bits
		SC4VideoPreferences& video = preferences->videoPreferences;
		uint32_t const previousDepth = video.bitDepth;

		if (previousDepth == COLOUR_DEPTH || previousDepth == DESKTOP_COLOUR_DEPTH)
		{
			LogDebug("Colour depth: the game's preferences already give 32-bit colour.");
			return;
		}

		// Set it in the preferences and in the requested mode
		//
		// The game has already turned its preferences into the mode it asks the graphics
		// system for (0x44ce40): its own PreFrameWorkInit runs before the plugins' hooks
		// (0x87afdd). So the request is set again with the preferences' size, as
		// SC4GraphicsOptions does. Saving makes the game's own options show the depth it
		// runs in.
		video.bitDepth = COLOUR_DEPTH;
		game->SavePreferences();

		cIGZGraphicSystemPtr graphicSystem;
		if (!graphicSystem)
		{
			LogWarn("Colour depth: the graphics system is not available, so only the saved preferences changed to 32-bit colour.");
			return;
		}

		cGZDisplayMetrics metrics{};
		metrics.width    = video.width;
		metrics.height   = video.height;
		metrics.bitDepth = COLOUR_DEPTH;
		graphicSystem->PreInitSetDesiredGameResolution(metrics);

		LogInfo("Changed the game's colour depth from %u to %u bits.", previousDepth, COLOUR_DEPTH);
	}
}
