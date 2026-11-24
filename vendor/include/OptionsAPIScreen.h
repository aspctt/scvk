/*
 * SC4 Options API - modern graphics, play and mod options for SimCity 4
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

#pragma once

//// Dependencies

#include "cIGZUnknown.h"

#include <stdint.h>

/**
 * The contract between SC4 Options API and a renderer for the game's screen: which window
 * mode the player chose, and changing the screen size and window mode while the game runs.
 *
 * A renderer copies this file unchanged. Both sides are optional: a renderer that never
 * asks keeps its own window settings, and one that offers no screen driver interface gets
 * its changes at the next start, as the game's own renderers do.
 *
 * Version 1.
 */

//// Types

/**
 * How the game's window covers the screen. The values are fixed, since they cross
 * between DLLs.
 */
enum class OptionsAPIWindowMode : uint32_t
{
	// The display switches to the screen size and the window covers it
	Fullscreen = 0,

	// A window without a frame covering the main monitor at the desktop's own size, the
	// display left as it is
	Borderless = 1,

	// A window with a frame, its client area the screen size
	Windowed = 2,
};

//// Constants

// The class SC4 Options API registers with the game's COM, and its interface
static const uint32_t GZCLSID_cOptionsAPIScreen = 0x4EE6E4D8;
static const uint32_t GZIID_cIOptionsAPIScreen = 0x5F52A74D;

// The interface a renderer's driver answers in its QueryInterface
static const uint32_t GZIID_cIOptionsAPIScreenDriver = 0x83CC0C15;

// The version of this file
static const uint32_t OPTIONS_API_SCREEN_VERSION = 1;

//// Interfaces

/**
 * What SC4 Options API offers a renderer.
 *
 * A renderer gets it from the game's COM once the plugins are loaded, which they are by the
 * first SetVideoMode: RZGetFramework()->GetCOMObject()->GetClassObject(
 * GZCLSID_cOptionsAPIScreen, GZIID_cIOptionsAPIScreen, &screen). The call fails when the
 * plugin is not installed. The object comes with a reference, to release once done.
 */
class cIOptionsAPIScreen : public cIGZUnknown
{
public:
	/** The version of this file the plugin was built with, at least 1. */
	virtual uint32_t GetVersion(void) = 0;

	/**
	 * The window mode for the game's window. Only Fullscreen and Borderless matter when
	 * the game asks for a fullscreen mode; a windowed mode from the game, as with the -w
	 * launch option, stays a window, and this answers Windowed then.
	 */
	virtual OptionsAPIWindowMode GetWindowMode(void) = 0;
};

/**
 * What a renderer offers SC4 Options API, through its driver's QueryInterface.
 *
 * The plugin finds the driver through the game's graphic system (cIGZGraphicSystem2
 * GetGDriver) and asks for this interface. A driver that answers lets the player's screen
 * size and window mode take effect at once.
 */
class cIOptionsAPIScreenDriver : public cIGZUnknown
{
public:
	/** The version of this file the renderer was built with, at least 1. */
	virtual uint32_t GetVersion(void) = 0;

	/**
	 * Gives the game's window a new screen size and window mode, keeping the device, its
	 * resources and the window itself.
	 *
	 * The plugin calls it on the game's thread between frames, never from inside the
	 * driver, with a size the driver lists among its video modes (a borderless window at
	 * the main monitor's own size). Once it returns true, the window's client area has the
	 * new size and the driver draws at it; the plugin then tells the game, which lays its
	 * interface out again. It returns false, with nothing changed, when it cannot.
	 */
	virtual bool ApplyScreen(uint32_t width, uint32_t height, OptionsAPIWindowMode mode) = 0;
};
