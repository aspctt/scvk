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

#include <OptionsAPIRenderer.h>

/**
 * What SC4 Options API says about the renderer the player chose, when it is installed.
 *
 * The plugin asks the game for the player's renderer at every start, in the slot that
 * renderer draws from, so a game on another driver is then the player's choice rather than
 * a failed start. With a launch option such as -d it asks for nothing and answers None, and
 * the driver is left to the game and scvk as before.
 */
namespace scvk::OptionsApiRenderer
{
	//// Public API

	/** The renderer the player chose, or None when the plugin is not installed or its answer is not one the contract names. */
	OptionsAPIRenderer ReadRenderer(void);

	/** The renderer's name, for the log. */
	char const* RendererName(OptionsAPIRenderer renderer);
}
