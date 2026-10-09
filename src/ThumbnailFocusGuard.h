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

#include <cstdint>

namespace scvk::ThumbnailFocusGuard {

	// When a city is saved, SC4 renders its region view thumbnail and then checks that the game
	// window kept focus the whole time. If it did not (Alt+Tab, a click elsewhere, or Windows
	// "not responding" ghosting during a long save), SC4 waits up to about 10 s for focus to
	// return and otherwise replaces the thumbnail with the blank default one. That guard only
	// makes sense for the stock DirectX 7 driver, which read the capture back from a surface
	// that other windows could cover. SCD3D11 reads it back from its own off-screen texture, so
	// this patch keeps the thumbnail unless the window was minimized during the render.
	//
	// Installed by the driver once it owns the game window, so it only applies when SCD3D11 is
	// the active renderer. Requires SimCity 4 Deluxe 1.1.641. Later calls just update the window.
	// Returns true if the patch is active afterwards.
	bool Install(void *window);
	void Uninstall(void);

	// Fed from the driver window procedure; tracks minimizes and deactivations.
	void NoteWindowMessage(uint32_t message, uintptr_t wParam);

}
