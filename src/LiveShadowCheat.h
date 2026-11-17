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

/**
 * The LiveShadows cheat, typed into the game's cheat box (Ctrl+X) as in scd3d11. It
 * pauses live shadows and brings back the game's own, and resumes them, so the two can be
 * compared in a city. With live shadows off it says how to turn them on.
 */
namespace scvk::LiveShadowCheat
{
	//// Public API

	/** Registers the cheat. Call once the game is up, and only when scvk draws. Calling it again changes nothing. */
	void Install(void);

	/** Removes the cheat and stops listening for it. */
	void Uninstall(void);
}
