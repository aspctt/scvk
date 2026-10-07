/*
 * scvk - a native Vulkan renderer for SimCity 4
 *
 * Copyright (C) 2026 aspctt
 *
 * The simulation speed FPS caps and the addresses that hold them were
 * identified by caspervg's sc4-disable-fps-limits
 * (https://github.com/caspervg/sc4-disable-fps-limits), LGPL-2.1-or-later.
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU Lesser General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public License
 * along with this program.  If not, see <https://www.gnu.org/licenses/>.
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
	 * Rhino and 15 at Cheetah, and a fixed 30 while the city is paused. Four changes, each
	 * a few bytes of the game's code:
	 *
	 * - A paused city's frames are no longer padded, always. The padding only gives the
	 *   idle agents time while nothing is simulated, and it held a paused city at 30
	 *   frames a second that no setting could raise.
	 * - The animation clock counts frames shorter than 2 ms as they are, always. It used
	 *   to round them up, so lot animations ran fast above 500 frames a second.
	 * - The speed caps become MaxFPS, when it is set. Each is a one-byte immediate.
	 * - The 15 ms of padding every running frame gets becomes 3 ms, when UnlockRunningFPS
	 *   is true, so MaxFPS can go past about 60. That time is the simulation's, so the
	 *   city simulates more slowly. Off by default.
	 *
	 * This lives in scvk because frame pacing and presentation are the same concern. Once
	 * the swapchain exists, the present mode and the caps have to agree, and splitting
	 * them across two plugins means two settings files that can contradict each other.
	 * caspervg's standalone plugin raises the same caps, so MaxFPS is off by default:
	 * running both with different values would be needlessly confusing.
	 *
	 * Reads MaxFPS and UnlockRunningFPS from scvk.ini beside the DLL. Only game version 641
	 * is changed, and only after the bytes at the target addresses are confirmed to hold
	 * what we expect. Calling it again changes nothing, since code already changed is left
	 * alone.
	 */
	void ApplyFpsLimitSettings(void);
}
