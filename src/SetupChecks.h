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

namespace scvk
{
	//// Public API

	/**
	 * Warns in scvk.log about plugin setups that load without an error but go wrong
	 * later: a second copy of scvk, and a SC4RenderServices too old for scvk's stencil.
	 *
	 * Call it once every plugin has loaded. The first frame is late enough, since the
	 * game loads them all before it starts drawing.
	 */
	void CheckPluginSetup(void);
}
