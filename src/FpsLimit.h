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
#include <stdint.h>

namespace scvk
{
	/**
	 * Raises SimCity 4's per-simulation-speed frame rate caps.
	 *
	 * The game clamps its frame rate by simulation speed: 30 at Turtle, 20 at
	 * Rhino, 15 at Cheetah. Each limit is a one-byte immediate operand in the
	 * instruction stream, so raising them is three byte writes.
	 *
	 * This lives in scvk because frame pacing and presentation are the same
	 * concern. Once the swapchain exists, the present mode and this cap have to
	 * agree, and splitting them across two plugins means two settings files
	 * that can contradict each other.
	 *
	 * Off by default, for two reasons. It writes to game memory, which the rest
	 * of scvk never does, and caspervg's standalone plugin does the same job:
	 * running both with different values would be needlessly confusing.
	 *
	 * Reads MaxFPS from scvk.ini beside the DLL. Zero or absent means leave the
	 * game alone. Only game version 641 is patched, and only after the bytes
	 * at the target addresses are confirmed to hold the values we expect.
	 */
	void ApplyFpsLimitSettings(void);
}
