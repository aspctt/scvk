/*
 *  SCD3D11 - a free Direct3D 11 driver for SimCity 4's SimGL interface
 *  Copyright (C) 2026
 *
 *  This library is free software; you can redistribute it and/or
 *  modify it under the terms of the GNU Lesser General Public
 *  License as published by the Free Software Foundation, under
 *  version 2.1 of the License, or (at your option) any later version.
 */

#pragma once

//// Dependencies

#include <stdint.h>

/**
 * Keeps the region view thumbnail when the game loses the focus while saving.
 *
 * When a city is saved, the game renders its region view thumbnail and then checks that
 * its window kept the focus the whole time. If it did not, through Alt+Tab, a click
 * elsewhere or Windows ghosting a window that stopped responding during a long save, the
 * game waits up to about 10 seconds for the focus to come back and otherwise writes the
 * blank default thumbnail over the one it rendered. That check only made sense for the
 * stock DirectX 7 driver, which read the capture back from a surface other windows could
 * cover. scvk reads it back from its own frame image, so the guard keeps the thumbnail
 * unless the window was minimised during the render.
 *
 * Carried over from SCD3D11. Only game version 641 is changed.
 */
namespace scvk::ThumbnailFocusGuard
{
	//// Public API

	/**
	 * Patches the game's check, once the driver owns the game window.
	 *
	 * Later calls only change the window. Returns whether the patch is in place afterwards.
	 */
	bool Install(void* window);

	/** Puts the game's check back as it was. */
	void Uninstall(void);

	/** Counts the minimises and deactivations the check cares about, from the window procedure. */
	void NoteWindowMessage(uint32_t message, uintptr_t wParam);
}
