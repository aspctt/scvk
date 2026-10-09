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

#include "cVKDriver.h"
#include "FpsLimit.h"
#include "Logger.h"
#include "NativeShadowDiagnostics.h"
#include "NativeShadowExperiment.h"
#include "NativeShadowMasks.h"
#include "NativeShadowRegistry.h"
#include "PrintScreen.h"
#include "SC4Version.h"
#include "TerrainShadows.h"
#include "ThumbnailFocusGuard.h"
#include "version.h"

#include <cIGZCOM.h>
#include <cIGZFrameWork.h>
#include <cRZCOMDllDirector.h>

#include <windows.h>

namespace scvk
{
	//// Constants

	namespace
	{
		// Identifies this plugin to the GZCOM. Must be unique across every installed DLL,
		// and must not be SCGL's (0xCB6EC543).
		constexpr uint32_t DIRECTOR_ID = 0x5C4B0001;
	}

	//// Private Functions

	namespace
	{
		// Says whether the game runs on Windows or under Wine, which Proton is built on
		//
		// Wine's ntdll exports its version where Windows' has nothing of the kind, so a
		// log from Linux says so and which Wine it was.
		void LogPlatform(void)
		{
			using WineVersion     = char const* (__cdecl*)(void);
			using WineHostVersion = void (__cdecl*)(char const** system, char const** release);

			HMODULE const ntdll = GetModuleHandleA("ntdll.dll");
			WineVersion const wineVersion = (ntdll != nullptr) ? reinterpret_cast<WineVersion>(reinterpret_cast<void*>(GetProcAddress(ntdll, "wine_get_version"))) : nullptr;

			if (wineVersion == nullptr)
			{
				LogInfo("Running on Windows.");
				return;
			}

			char const* system  = "an unknown system";
			char const* release = "";
			WineHostVersion const hostVersion = reinterpret_cast<WineHostVersion>(reinterpret_cast<void*>(GetProcAddress(ntdll, "wine_get_host_version")));

			if (hostVersion != nullptr)
			{
				hostVersion(&system, &release);
			}

			LogInfo("Running under Wine %s on %s %s.", wineVersion(), system, release);
		}
	}

	//// Types

	/**
	 * Registers scvk's driver with the game.
	 *
	 * SimCity 4 chooses a renderer by class ID and only knows three: DirectX, OpenGL and
	 * Software. There is no way to add a fourth, so scvk registers itself under the
	 * OpenGL class ID and relies on the GZCOM preferring the higher version number when
	 * two libraries claim the same class.
	 *
	 * That is why EnumClassObjects is overridden. The inherited implementation reports
	 * version 0 for everything it registers, which would tie with the game's built-in
	 * driver rather than beat it.
	 */
	class cVKDriversCOMDirector final : public cRZCOMDllDirector
	{
	public:
		//// Public API

		cVKDriversCOMDirector(void)
		{
			AddCls(cVKDriver::DRIVER_GZCLSID, &cVKDriver::FactoryFunction);
		}

		uint32_t GetDirectorID(void) const override
		{
			return DIRECTOR_ID;
		}

		void EnumClassObjects(ClassObjectEnumerationCallback callback, void* context) override
		{
			// Outranks the game's own OpenGL driver, which registers the same class ID at
			// a lower version.
			callback(cVKDriver::DRIVER_GZCLSID, cVKDriver::DRIVER_VERSION, context);
		}

		bool OnStart([[maybe_unused]] cIGZCOM* com) override
		{
			LogOpen();
			LogInfo("scvk %s loaded; claiming GZCLSID %08x at version %u.", SCVK_VERSION_STRING, cVKDriver::DRIVER_GZCLSID, cVKDriver::DRIVER_VERSION);
			LogInfo("Detected SimCity 4 version %u.", GetGameVersion());
			LogPlatform();

			// Change the game's frame pacing: no padding while paused, an exact animation
			// clock, and speed caps at the display's refresh rate
			ApplyFpsLimitSettings();

			// Install the game-side shadow modules SCD3D11 has, each guarded by the exact
			// bytes it patches: the True3D shadows and the registry, on by default as
			// -NativeShadowMasks:replace unless -NativeShadowMasks:off or another mode
			// says otherwise, and the diagnostics only when asked for
			NativeShadowMasks::Install();
			NativeShadowRegistry::Install();
			TerrainShadows::Install();
			NativeShadowDiagnostics::Install();
			NativeShadowExperiment::Install();

			// Hear when the game shuts down, to put back every byte patched
			cIGZFrameWork* const framework = RZGetFrameWork();
			if (framework != nullptr && framework->GetState() < cIGZFrameWork::kStatePreAppInit)
			{
				framework->AddHook(this);
			}

			return true;
		}

		bool PreAppShutdown(void) override
		{
			NativeShadowExperiment::Uninstall();
			TerrainShadows::Uninstall();
			NativeShadowRegistry::Uninstall();
			NativeShadowMasks::Uninstall();
			NativeShadowDiagnostics::Uninstall();
			ThumbnailFocusGuard::Uninstall();
			PrintScreen::Uninstall();
			return true;
		}
	};
}

//// Exports

cRZCOMDllDirector* RZGetCOMDllDirector(void)
{
	static scvk::cVKDriversCOMDirector director;
	return &director;
}
