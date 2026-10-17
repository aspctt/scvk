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

namespace scvk
{
	//// Public API

	/**
	 * Corrects the video mode SimCity 4 asks for at startup, unless SC4GraphicsOptions is
	 * installed.
	 *
	 * - The -r launch option's size is used as given. Without -CustomResolution:enabled the
	 *   game swaps a size it does not know for one of its own (0x44cd70), so -r1920x1080
	 *   became 1800x1440. A -CustomResolution value other than enabled or on, such as
	 *   disabled, keeps the game's size.
	 * - The colour depth becomes 32 bits. The game keeps one in its preferences, which its
	 *   options and -r can set to 16, and in a 16-bit mode it makes its textures 16-bit
	 *   too, so they band whatever depth scvk draws in.
	 *
	 * Both go into the preferences and into the mode the game asks its graphics system
	 * for, as SC4GraphicsOptions does, and a changed colour depth saves the preferences.
	 * SC4GraphicsOptions sets both from its own settings, so scvk leaves the mode to it
	 * when it is installed.
	 *
	 * Call from PreFrameWorkInit, where SC4GraphicsOptions does the same, before the
	 * graphics system starts. Calling it again changes nothing.
	 */
	void ApplyVideoMode(void);
}
