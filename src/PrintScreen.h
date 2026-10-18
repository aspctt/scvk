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

#include <cstdint>

/**
 * PrintScreen in exclusive fullscreen.
 *
 * Left to Windows, the key goes wrong there in two ways. On Windows 11 it opens the
 * Snipping Tool, whose overlay takes the focus, and losing the focus takes the game out
 * of fullscreen: the desktop's mode comes back and the game is minimised, so the snip
 * shows the desktop. And where the graphics driver presents a fullscreen window without
 * the desktop compositor, as one AMD card did, the classic copy to the clipboard sees the
 * desktop instead of the game.
 *
 * So while the game is in front in exclusive fullscreen, scvk takes the key itself and
 * puts the frame it drew on the clipboard, as PrintScreen always did. Alt+PrintScreen
 * does the same, since the game's window is the whole screen. Win+PrintScreen, which
 * saves a file through the compositor, is left to Windows, as is the key in a window or
 * in borderless fullscreen, where nothing goes wrong.
 *
 * The key is watched by a low-level keyboard hook on a thread of its own, so a game
 * thread busy loading a city never holds up the keyboard.
 */
namespace scvk::PrintScreen
{
	/** Starts taking the key for the window, while it is in front. Later calls change the window. */
	void Install(void* window);

	/** Stops taking the key. */
	void Uninstall(void);

	/** Whether the key was pressed since the last call, which clears it. */
	bool TakeRequest(void);

	/**
	 * Puts a picture on the clipboard as a 32 bit DIB, owned by the window.
	 *
	 * The pixels are BGRA rows, top row first, as VulkanBackend::ReadFramePixels returns
	 * them.
	 */
	bool CopyToClipboard(void* window, uint8_t const* bgraPixels, uint32_t width, uint32_t height);
}
