/*
 * scvk - a native Vulkan renderer for SimCity 4
 *
 * Copyright (C) 2026 aspctt
 *
 * The limit at twice the display's refresh rate, and the way its wait ends, follow
 * caspervg's scd3d11 (https://github.com/caspervg/scd3d11), LGPL-2.1-or-later.
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

#include <windows.h>
#include <stdint.h>

namespace scvk
{
	//// Types

	/**
	 * Holds presents to twice the display's refresh rate while VSync is off.
	 *
	 * Without VSync, a cheap view such as the region or a paused city presented well
	 * over a thousand frames a second, which the display cannot show and which only makes
	 * heat and coil whine. Twice the refresh rate, as in scd3d11, still gives every
	 * refresh a new frame: a limit at the rate itself drifts against the display, which
	 * then shows a frame twice now and then, and scrolling judders. -MaxFPS:off lifts it.
	 */
	class PresentLimiter
	{
	public:
		PresentLimiter(void);
		~PresentLimiter();

		PresentLimiter(PresentLimiter const&)            = delete;
		PresentLimiter& operator=(PresentLimiter const&) = delete;

		/** Waits until the next present is due. Call once a frame, just before presenting into the window. */
		void Wait(HWND window);

	private:
		/** The presents a second to hold to, 0 for none, with the display's rate read again every second. */
		uint32_t ChooseRate(HWND window);

		/** Returns once the next present is due at the rate given, and schedules the one after. */
		void WaitForNextPresent(uint32_t presentsPerSecond);

		int64_t ticksPerSecond = 0;

		// When the next present is due, in QueryPerformanceCounter ticks, 0 before the first
		// one and while nothing is held
		int64_t nextPresentTicks = 0;

		// The display's refresh rate and when it is read next, so a window moved to another
		// display gets that one's rate
		uint32_t  refreshRate      = 0;
		ULONGLONG nextRefreshCheck = 0;
		uint32_t  loggedRate       = UINT32_MAX;

		// A high resolution waitable timer, made on the first wait that needs one
		HANDLE timer         = nullptr;
		bool   hasTriedTimer = false;
	};
}
