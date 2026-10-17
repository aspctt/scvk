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
	 * Starts correcting the video mode SimCity 4 asks for at startup.
	 *
	 * - The -b and -borderless launch options, and SC4GraphicsOptions' borderless mode,
	 *   ask for a window that covers the main monitor without a frame. A launch option
	 *   wins over any SC4GraphicsOptions setting, since it is given for one launch.
	 * - The -r launch option's size is used as given. Without -CustomResolution:enabled the
	 *   game swaps a size it does not know for one of its own (0x44cd70), so -r1920x1080
	 *   became 1800x1440. A -CustomResolution value other than enabled or on, such as
	 *   disabled, keeps the game's size. A borderless window takes the monitor's size.
	 * - The colour depth becomes 32 bits. The game keeps one in its preferences, which its
	 *   options and -r can set to 16, and in a 16-bit mode it makes its textures 16-bit
	 *   too, so they band whatever depth scvk draws in.
	 *
	 * The size and colour depth go into the preferences as SC4GraphicsOptions does, and
	 * a changed colour depth saves them. SC4GraphicsOptions sets both from its own
	 * settings, so scvk leaves them to it when it is installed. Borderless only changes
	 * the mode the game asks its graphics system for, so the next launch without it gets
	 * the game's own mode back.
	 *
	 * The correction runs as a system service started just before the graphics system,
	 * after every plugin's PreFrameWorkInit, so no plugin's request comes after it. Call
	 * from OnStart. Calling it again changes nothing.
	 */
	void InstallVideoModeService(void);

	/** Removes the service once the graphics system has started. Call from PreAppInit. */
	void RemoveVideoModeService(void);

	/**
	 * Whether the game's window should cover the main monitor without a frame. Decided
	 * once, just before the graphics system starts.
	 */
	bool IsBorderlessWindow(void);
}
