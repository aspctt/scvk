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

#include <stdint.h>

namespace scvk
{
	//// Constants

	// The game's DirectX and Software slots. cVKDriver::DRIVER_GZCLSID is its OpenGL one.
	constexpr uint32_t DIRECTX_DRIVER_GZCLSID  = 0xBADB6906;
	constexpr uint32_t SOFTWARE_DRIVER_GZCLSID = 0x7ACA35C6;

	//// Public API

	/**
	 * Whether the game asked for the driver in a slot. The game sets up every slot before
	 * it picks one, so a driver in any other slot should stay idle. True for every slot when
	 * the game's request cannot be read, so scvk then starts as if it would draw.
	 */
	bool IsDriverSlotRequested(uint32_t slotClass);

	/** Whether the game asked for its Software driver. */
	bool IsSoftwareDriverRequested(void);

	/**
	 * Whether scvk will be the renderer, as far as can be told before the graphics system
	 * starts. Leaves Vulkan alone when the answer is already no.
	 */
	bool WillScvkDraw(void);

	/**
	 * Makes scvk's changes to the game's code: its frame pacing and the live shadows. Call
	 * when scvk starts to draw, so a game drawing with another renderer is left as it is.
	 * Calling it again changes nothing.
	 */
	void ApplyGameChanges(void);
}
