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

#include <stdint.h>

namespace scvk
{
	//// Public API

	/**
	 * Whether every vertex lies beyond the same edge of clip space, so triangles made from
	 * them cannot reach a pixel of the viewport.
	 *
	 * The transform is column major, and the edges sit at plus and minus limitX and
	 * limitY. Positions are three floats at the start of each stride. Only an orthographic
	 * transform is tested, which is the only kind the game was seen using: any other
	 * answers false, and so does a position that is not a number, so a draw is only ever
	 * dropped on proof.
	 */
	bool AreVerticesBeyondClipEdge(float const* transform, float limitX, float limitY, uint8_t const* positions, uint32_t stride, uint32_t count);
}
