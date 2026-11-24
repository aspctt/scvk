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

#pragma once

//// Dependencies

#include <OptionsAPIScreen.h>

/**
 * What SC4 Options API says about the game's screen, when it is installed.
 *
 * The plugin keeps the screen size and window mode the player chose in its Graphic Options
 * window. Once it is installed, the window mode is its to decide: scvk's own borderless
 * default and its launch options stand aside, as they do for SC4GraphicsOptions. The other
 * half of the contract, changing the screen while the game runs, is the driver's own
 * cIOptionsAPIScreenDriver (cVKDriver::ApplyScreen).
 */
namespace scvk::OptionsApiScreen
{
	//// Public API

	/** Whether SC4 Options API is installed and offers its screen interface. */
	bool IsInstalled(void);

	/** The window mode the player chose. False, with the mode unchanged, when the plugin is not installed. */
	bool ReadWindowMode(OptionsAPIWindowMode& outMode);

	/** The mode's name, for the log. */
	char const* WindowModeName(OptionsAPIWindowMode mode);
}
