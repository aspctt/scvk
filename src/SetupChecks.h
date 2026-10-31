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
	 * Looks at the rest of the game for setups that load without an error but go wrong
	 * later: another driver drawing, a second copy of scvk or of any plugin, plugins that
	 * cannot work, the 4GB patch missing, compatibility mode, mistyped launch switches and
	 * settings, and SC4GraphicsOptions.ini lines its plugin cannot read. Every finding
	 * goes into scvk.log, and the serious ones into a dialog when the region view opens.
	 *
	 * Call it once the game is up, so it also runs when scvk is not the driver in use.
	 */
	void RunSetupChecks(void);

	/** Stops listening for the region view. */
	void StopSetupChecks(void);

	/**
	 * Logs a serious problem and adds it to the dialog. For what only the renderer can
	 * see. It may be called before RunSetupChecks. Takes printf arguments.
	 */
	void ReportSetupProblem(char const* format, ...);
}
