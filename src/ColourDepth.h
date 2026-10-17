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
	 * Makes SimCity 4 run in 32-bit colour, unless SC4GraphicsOptions is installed.
	 *
	 * The game keeps a colour depth in its preferences, which its options and the -r
	 * launch option (0x44eed0) can set to 16. In a 16-bit fullscreen mode the game makes
	 * its textures 16-bit too, so they band whatever depth scvk draws in. scvk sets the
	 * depth to 32 the way SC4GraphicsOptions does: in the saved preferences, and in the
	 * mode the game asks its graphics system for.
	 *
	 * SC4GraphicsOptions sets both from its own ColorDepth setting, so scvk leaves the
	 * depth to it when it is installed.
	 *
	 * Call from PreFrameWorkInit, where SC4GraphicsOptions does the same, before the
	 * graphics system starts. Calling it again changes nothing.
	 */
	void ApplyColourDepth(void);
}
