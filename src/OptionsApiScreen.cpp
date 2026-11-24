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

//// Dependencies

#include "OptionsApiScreen.h"
#include "Logger.h"

#include <cIGZCOM.h>
#include <cIGZFrameWork.h>
#include <cRZCOMDllDirector.h>

namespace scvk::OptionsApiScreen
{
	//// Private Functions

	namespace
	{
		/** The plugin's screen interface with a reference to release, or null when it is not installed. */
		cIOptionsAPIScreen* AcquireScreen(void)
		{
			cIGZFrameWork* const frameWork = RZGetFramework();
			cIGZCOM* const       com       = (frameWork != nullptr) ? frameWork->GetCOMObject() : nullptr;

			if (com == nullptr)
			{
				return nullptr;
			}

			// GZCOM hands the object back through an untyped pointer, and the interface asked
			// for is what it points at.
			cIOptionsAPIScreen* screen = nullptr;
			if (!com->GetClassObject(GZCLSID_cOptionsAPIScreen, GZIID_cIOptionsAPIScreen, reinterpret_cast<void**>(&screen)))
			{
				return nullptr;
			}

			return screen;
		}

		/** Whether a mode is one of the three the contract names, since it crosses between DLLs. */
		bool IsKnownWindowMode(OptionsAPIWindowMode mode)
		{
			return mode == OptionsAPIWindowMode::Fullscreen || mode == OptionsAPIWindowMode::Borderless || mode == OptionsAPIWindowMode::Windowed;
		}
	}

	//// Public API

	bool IsInstalled(void)
	{
		cIOptionsAPIScreen* const screen = AcquireScreen();
		if (screen == nullptr)
		{
			return false;
		}

		screen->Release();
		return true;
	}

	bool ReadWindowMode(OptionsAPIWindowMode& outMode)
	{
		cIOptionsAPIScreen* const screen = AcquireScreen();
		if (screen == nullptr)
		{
			return false;
		}

		uint32_t const             version = screen->GetVersion();
		OptionsAPIWindowMode const mode    = screen->GetWindowMode();
		screen->Release();

		// Say once what the plugin answered, which a window in the wrong mode is checked
		// against
		static bool hasLogged = false;
		if (!hasLogged)
		{
			hasLogged = true;
			LogInfo("SC4 Options API: window mode %s (its screen interface version %u, scvk's %u).", WindowModeName(mode), version, OPTIONS_API_SCREEN_VERSION);
		}

		if (!IsKnownWindowMode(mode))
		{
			return false;
		}

		outMode = mode;
		return true;
	}

	char const* WindowModeName(OptionsAPIWindowMode mode)
	{
		switch (mode)
		{
		case OptionsAPIWindowMode::Fullscreen: return "fullscreen";
		case OptionsAPIWindowMode::Borderless: return "borderless";
		case OptionsAPIWindowMode::Windowed:   return "windowed";
		default:                               return "unknown";
		}
	}
}
