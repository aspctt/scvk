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

//// Dependencies

#include "PresentLimit.h"
#include "FpsLimit.h"
#include "Logger.h"

namespace scvk
{
	//// Constants

	namespace
	{
		// Windows 10 1803 and later make this timer good to well under a millisecond. Older
		// SDKs lack the name, and older systems refuse the flag, which leaves Sleep.
#ifndef CREATE_WAITABLE_TIMER_HIGH_RESOLUTION
		constexpr DWORD CREATE_WAITABLE_TIMER_HIGH_RESOLUTION = 0x00000002;
#endif

		// How often the display's refresh rate is read again
		constexpr ULONGLONG REFRESH_CHECK_MILLISECONDS = 1000;

		// Presents a second for each refresh, so every refresh has a new frame
		constexpr uint32_t PRESENTS_PER_REFRESH = 2;

		// The timer and Sleep can both wake late, so the end of a wait spins: half a
		// millisecond after the timer, two after Sleep, which is good to a millisecond at
		// best
		constexpr int64_t TIMER_SPIN_DIVISOR = 2000;
		constexpr int64_t SLEEP_SPIN_DIVISOR = 500;

		// The longest a single timer wait may take before it counts as broken
		constexpr DWORD TIMER_WAIT_LIMIT_MILLISECONDS = 100;

		// The timer counts in 100 ns units, and a negative time is one from now
		constexpr int64_t TIMER_UNITS_PER_SECOND = 10000000;
	}

	//// Private Functions

	namespace
	{
		int64_t ReadTicks(void)
		{
			LARGE_INTEGER value{};
			QueryPerformanceCounter(&value);
			return value.QuadPart;
		}

		/** The refresh rate of the display the window is on, or 0 when Windows does not say. */
		uint32_t ReadRefreshRate(HWND window)
		{
			HMONITOR const monitor = MonitorFromWindow(window, MONITOR_DEFAULTTOPRIMARY);

			MONITORINFOEXW information{};
			information.cbSize = sizeof(information);

			if (monitor == nullptr || !GetMonitorInfoW(monitor, &information))
			{
				return 0;
			}

			DEVMODEW mode{};
			mode.dmSize = sizeof(mode);

			if (!EnumDisplaySettingsW(information.szDevice, ENUM_CURRENT_SETTINGS, &mode))
			{
				return 0;
			}

			// 0 and 1 stand for the hardware's default rate, which says nothing
			return mode.dmDisplayFrequency > 1 ? mode.dmDisplayFrequency : 0;
		}
	}

	uint32_t PresentLimiter::ChooseRate(HWND window)
	{
		// Read the display's rate again now and then
		//
		// With VSync on as well. FIFO should hold presents to the refresh rate, which leaves
		// this limit idle, but a driver setting can turn VSync off underneath it. On the test
		// PC FIFO presents came back at once and the region view ran at about 1300 frames a
		// second, where the game's region camera moved at most a pixel a frame and stepped
		// back and forth, which looked like lag and shaking. At twice the refresh rate it
		// scrolled smoothly. An earlier version skipped the limit whenever VSync was on.
		ULONGLONG const now = GetTickCount64();
		if (now >= nextRefreshCheck)
		{
			nextRefreshCheck = now + REFRESH_CHECK_MILLISECONDS;
			refreshRate      = ReadRefreshRate(window);
		}

		uint32_t const presentsPerSecond = IsMaxFpsOff() ? 0 : refreshRate * PRESENTS_PER_REFRESH;

		// Say what presents are held to, once for each change
		//
		// A window moved to a display with another rate changes it.
		if (presentsPerSecond != loggedRate)
		{
			loggedRate = presentsPerSecond;

			if (IsMaxFpsOff())
			{
				LogInfo("Vulkan: presents are not held to a rate, as MaxFPS off asks.");
			}
			else if (presentsPerSecond == 0)
			{
				LogInfo("Vulkan: the display gives no refresh rate, so presents are not held to one.");
			}
			else
			{
				LogInfo("Vulkan: presents are held to %u a second, twice the display's %u Hz.", presentsPerSecond, refreshRate);
			}
		}

		return presentsPerSecond;
	}

	void PresentLimiter::WaitForNextPresent(uint32_t presentsPerSecond)
	{
		int64_t const interval = ticksPerSecond / presentsPerSecond;
		int64_t       now      = ReadTicks();

		// Start a new schedule after a late frame, or one that would wait past an interval
		//
		// Catching up instead would show a burst of fast frames. The second case is a
		// schedule left from a slower rate, before the window moved to a faster display.
		if (nextPresentTicks == 0 || now >= nextPresentTicks || nextPresentTicks - now > interval)
		{
			nextPresentTicks = now + interval;
			return;
		}

		// Make the timer on the first wait
		//
		// Without it the wait falls back to Sleep, which is only good to a millisecond.
		if (!hasTriedTimer)
		{
			hasTriedTimer = true;
			timer = CreateWaitableTimerExW(nullptr, nullptr, CREATE_WAITABLE_TIMER_HIGH_RESOLUTION, TIMER_ALL_ACCESS);
		}

		int64_t const spin = ticksPerSecond / (timer != nullptr ? TIMER_SPIN_DIVISOR : SLEEP_SPIN_DIVISOR);

		// Sleep through most of the wait, then spin the rest
		while ((now = ReadTicks()) < nextPresentTicks)
		{
			int64_t const remaining = nextPresentTicks - now;

			if (remaining <= spin)
			{
				YieldProcessor();
				continue;
			}

			if (timer == nullptr)
			{
				Sleep(remaining > spin * 2 ? 1 : 0);
				continue;
			}

			// Wait on the timer, and drop it for Sleep should it ever fail
			LARGE_INTEGER due{};
			due.QuadPart = -((remaining - spin) * TIMER_UNITS_PER_SECOND / ticksPerSecond);

			if (!SetWaitableTimer(timer, &due, 0, nullptr, nullptr, FALSE) || WaitForSingleObject(timer, TIMER_WAIT_LIMIT_MILLISECONDS) == WAIT_FAILED)
			{
				CloseHandle(timer);
				timer = nullptr;
			}
		}

		nextPresentTicks += interval;
	}

	//// Public API

	PresentLimiter::PresentLimiter(void)
	{
		LARGE_INTEGER frequency{};
		QueryPerformanceFrequency(&frequency);
		ticksPerSecond = frequency.QuadPart;
	}

	PresentLimiter::~PresentLimiter()
	{
		if (timer != nullptr)
		{
			CloseHandle(timer);
		}
	}

	void PresentLimiter::Wait(HWND window)
	{
		uint32_t const presentsPerSecond = ChooseRate(window);

		// Forget the schedule while nothing is held, so the first held present starts a new one
		if (presentsPerSecond == 0)
		{
			nextPresentTicks = 0;
			return;
		}

		WaitForNextPresent(presentsPerSecond);
	}
}
