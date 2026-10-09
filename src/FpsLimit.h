/*
 * scvk - a native Vulkan renderer for SimCity 4
 *
 * Copyright (C) 2026 aspctt
 *
 * The simulation speed FPS caps and the addresses that hold them were
 * identified by caspervg's sc4-disable-fps-limits
 * (https://github.com/caspervg/sc4-disable-fps-limits), LGPL-2.1-or-later.
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
	 * Changes how SimCity 4 paces its frames.
	 *
	 * The game's simulator pads every frame with simulation and idle work until it has
	 * lasted 1000 / rate milliseconds. The rate is the speed's cap, 30 at Turtle, 20 at
	 * Rhino and 15 at Cheetah, and a fixed 30 while the city is paused. Three changes,
	 * each a few bytes of the game's code, none of them needing a setting:
	 *
	 * - A paused city's frames are no longer padded. The padding only gives the idle
	 *   agents time while nothing is simulated, and it held a paused city at 30 frames a
	 *   second.
	 * - The animation clock counts frames shorter than 2 ms as they are. It used to round
	 *   them up, so lot animations ran fast above 500 frames a second.
	 * - The speed caps become the main display's refresh rate, when it is above 30 Hz, so
	 *   a running city shows every frame the display can. Each is a one-byte immediate.
	 *   The 15 ms of simulation every running frame gets is left alone, so the city
	 *   simulates as fast as before.
	 *
	 * This lives in scvk because frame pacing and presentation are the same concern. A cap
	 * another plugin, such as caspervg's sc4-disable-fps-limits, has already changed is
	 * left as that plugin set it.
	 *
	 * Only game version 641 is changed, and only after the bytes at the target addresses
	 * are confirmed to hold what we expect. Calling it again changes nothing, since code
	 * already changed is left alone.
	 */
	void ApplyFpsLimitSettings(void);
}
