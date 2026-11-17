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

class cISC4RenderProperties;

/**
 * Reads the game's render properties, the values Graphics Rules.sgr and the graphics
 * options set, by the names the game registers them under without their leading k, such
 * as RenderShadows for kRenderShadows.
 *
 * Every call is guarded, and the MinGW build, which has no guard, reads nothing.
 */
namespace scvk::RenderProperties
{
	//// Public API

	/** The game's render properties, or null before the game has them. */
	cISC4RenderProperties* Find(void);

	/**
	 * The id of a bool property. The render property interface had not been called from a
	 * DLL before, so the id is only handed out once its name reads back.
	 */
	bool FindBool(cISC4RenderProperties* properties, char const* name, int32_t& id);

	/** The id of an int property, as FindBool finds a bool one. */
	bool FindInt(cISC4RenderProperties* properties, char const* name, int32_t& id);

	bool ReadBool(cISC4RenderProperties* properties, int32_t id, bool& value);

	bool ReadInt(cISC4RenderProperties* properties, int32_t id, int32_t& value);
}
