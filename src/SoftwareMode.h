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

#include <string>

class SC4VideoPreferences;

/**
 * Takes SimCity 4 off its Software driver after a failed start, as scd3d11 does.
 *
 * When a hardware driver fails to start, the game switches to its Software driver and
 * lets its graphics rules choose again at the next start. Where they choose Software, the
 * game stays there, so a player who once had a broken setup never gets scvk until they
 * find -d:DirectX. scvk tries the hardware slot again once for each failure. A marker
 * file next to scvk.dll guards the try, so a start that fails every time leaves the game
 * on Software after one more failure rather than never starting.
 */
namespace scvk::SoftwareMode
{
	//// Public API

	/**
	 * Asks the game for its DirectX slot again when the preferences hold the Software
	 * driver and neither -d nor Driver= in SC4GraphicsOptions.ini chose it. Call just
	 * before the graphics system starts. Returns whether the game was switched.
	 */
	bool Leave(SC4VideoPreferences& video);

	/** Notes why scvk could not start, so the next start stays on Software instead of failing again. */
	void NoteStartFailure(char const* reason);

	/** Forgets an earlier failure, once scvk has started. */
	void NoteStarted(void);

	/**
	 * Whether Leave kept the game on Software because the last try failed, with the reason
	 * scvk noted then. The reason is empty when another driver failed, since only scvk
	 * notes one.
	 */
	bool FindEarlierFailure(std::string& reason);
}
