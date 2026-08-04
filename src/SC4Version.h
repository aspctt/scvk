/*
 * scvk - a native Vulkan renderer for SimCity 4
 *
 * Copyright (C) 2026 aspctt
 *
 * Game version detection follows the approach used by SC4Fix
 * (Copyright (c) 2015 Nelson Gomez, MIT License), including the sentinel byte
 * table used when the executable carries no version resource.
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
	 * Which build of SimCity 4 we are running inside.
	 *
	 * Returns the patch number (610, 638, 640, 641) or 0 if it could not be
	 * determined. The result is computed once and cached.
	 *
	 * scvk's renderer does not care about this, because it patches nothing and
	 * talks to the game only through a COM interface. It matters for optional
	 * features that do write to game memory, where a hardcoded address is only
	 * meaningful for one specific build.
	 */
	uint16_t GetGameVersion(void);
}
