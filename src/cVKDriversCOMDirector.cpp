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

#include "Benchmark.h"
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
#include "VideoMode.h"
#include "VulkanBackend.h"
#include "version.h"

#include <cIGZCOM.h>
#include <cIGZFrameWork.h>
#include <cRZCOMDllDirector.h>
#include <stdint.h>
#include <string.h>

#include <windows.h>

namespace scvk
{
	//// Constants

	namespace
	{
		// Identifies this plugin to the GZCOM. Must be unique across every installed DLL,
		// and must not be SCGL's (0xCB6EC543).
		constexpr uint32_t DIRECTOR_ID = 0x5C4B0001;

		// The DirectX slot, the one the game asks for unless told otherwise. scvk claims it
		// too, so selecting scvk needs no settings, but one below the OpenGL claim:
		// scd3d11 claims this slot at 1000000, and a renderer made for it should win it.
		// The game's own drivers register at version 0.
		constexpr uint32_t DIRECTX_GZCLSID = 0xBADB6906;
		constexpr uint32_t DIRECTX_VERSION = 999999;

		constexpr uint16_t SUPPORTED_GAME_VERSION = 641;

		// The game's DirectX 7 driver factory, which its own director registers for the
		// DirectX slot (0x466250). It takes nothing and returns the new driver with no
		// reference held, or null. It starts with PUSH 0x2E0, the driver's size, which is
		// checked before it is called.
		constexpr uintptr_t GAME_DIRECTX_FACTORY_ADDRESS_641 = 0x886700;
		constexpr uint8_t   GAME_DIRECTX_FACTORY_START[]      = { 0x68, 0xE0, 0x02, 0x00, 0x00 };
	}

	//// Private Functions

	namespace
	{
		/** The game's DirectX 7 driver, when this is the build its factory address came from. */
		bool CreateGameDirectXDriver(uint32_t interfaceId, void** outInterface)
		{
			// Only call code that is where it should be
			if (GetGameVersion() != SUPPORTED_GAME_VERSION)
			{
				LogError("The game's DirectX driver is only known for game version %u, so the DirectX slot has no driver.", SUPPORTED_GAME_VERSION);
				return false;
			}

			// The address is a plain number, which has to become a pointer to be read.
			uint8_t const* const factoryStart = reinterpret_cast<uint8_t const*>(GAME_DIRECTX_FACTORY_ADDRESS_641);
			if (memcmp(factoryStart, GAME_DIRECTX_FACTORY_START, sizeof(GAME_DIRECTX_FACTORY_START)) != 0)
			{
				LogError("The game's DirectX driver factory at %08X is not the code expected, so the DirectX slot has no driver.", GAME_DIRECTX_FACTORY_ADDRESS_641);
				return false;
			}

			// Create it the way the game's own director does
			//
			// The address is code, which has to become a function pointer to be called. A
			// new driver holds no reference until it is asked for an interface, so one it
			// refuses is deleted by taking a reference and releasing it.
			using GameFactory = cIGZUnknown* (*)(void);
			GameFactory const factory = reinterpret_cast<GameFactory>(GAME_DIRECTX_FACTORY_ADDRESS_641);

			cIGZUnknown* const driver = factory();
			if (driver == nullptr)
			{
				return false;
			}

			if (driver->QueryInterface(interfaceId, outInterface))
			{
				return true;
			}

			driver->AddRef();
			driver->Release();
			return false;
		}

		/**
		 * Serves the DirectX slot with scvk, or with the game's DirectX 7 driver when Vulkan
		 * is unavailable. Declining would drop the game to its Software renderer rather than
		 * to DirectX, since the game falls back by slot.
		 */
		bool CreateDirectXSlotDriver(uint32_t interfaceId, void** outInterface)
		{
			if (IsVulkanAvailable())
			{
				LogInfo("Serving the DirectX slot with scvk.");
				return cVKDriver::FactoryFunction(interfaceId, outInterface);
			}

			LogWarn("Vulkan is unavailable, so the DirectX slot goes to the game's own DirectX driver.");
			return CreateGameDirectXDriver(interfaceId, outInterface);
		}

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
	 * two libraries claim the same class. It claims the DirectX class ID as well, at a
	 * lower version, so the game's default choice is scvk too.
	 *
	 * The driver reports the OpenGL class ID from either slot. Plugins such as caspervg's
	 * render services read the DirectX 7 driver's internals when the driver reports the
	 * DirectX one, and the game only uses it to look up the OpenGL slot, which scvk also
	 * fills. The game creates and initialises every slot's driver at startup and keeps
	 * them until it exits, so the slot it does not use holds an idle scvk driver, just as
	 * an unused DirectX driver sat idle before.
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
			AddCls(DIRECTX_GZCLSID, &CreateDirectXSlotDriver);
		}

		uint32_t GetDirectorID(void) const override
		{
			return DIRECTOR_ID;
		}

		void EnumClassObjects(ClassObjectEnumerationCallback callback, void* context) override
		{
			// Leave both slots to the game's own drivers when a benchmark measures DirectX
			if (IsBenchmarkOnDirectX())
			{
				return;
			}

			// Outranks the game's own OpenGL and DirectX drivers, which register the same
			// class IDs at version 0.
			callback(cVKDriver::DRIVER_GZCLSID, cVKDriver::DRIVER_VERSION, context);
			callback(DIRECTX_GZCLSID, DIRECTX_VERSION, context);
		}

		bool OnStart(cIGZCOM* com) override
		{
			LogOpen();

			if (IsBenchmarkOnDirectX())
			{
				LogInfo("scvk %s loaded for a benchmark of the game's DirectX driver; claiming no driver.", SCVK_VERSION_STRING);
			}
			else
			{
				LogInfo("scvk %s loaded; claiming GZCLSID %08x at version %u.", SCVK_VERSION_STRING, cVKDriver::DRIVER_GZCLSID, cVKDriver::DRIVER_VERSION);
			}

			LogInfo("Detected SimCity 4 version %u.", GetGameVersion());
			LogPlatform();

			// Change the game's frame pacing: no padding while paused, an exact animation
			// clock, and speed caps at the display's refresh rate
			ApplyFpsLimitSettings();

			// Correct the video mode before the graphics system starts
			InstallVideoModeService();

			// Install the game-side shadow modules SCD3D11 has, each guarded by the exact
			// bytes it patches: the True3D shadows and the registry, on by default as
			// -NativeShadowMasks:replace unless -NativeShadowMasks:off or another mode
			// says otherwise, and the diagnostics only when asked for. They draw through
			// scvk, so a benchmark of the game's DirectX driver leaves them out.
			if (!IsBenchmarkOnDirectX())
			{
				NativeShadowMasks::Install();
				NativeShadowRegistry::Install();
				TerrainShadows::Install();
				NativeShadowDiagnostics::Install();
				NativeShadowExperiment::Install();
			}

			// Hear when the game starts, is up and shuts down
			//
			// The video mode service is done once the game starts, a benchmark run starts
			// once the game is up, and every byte patched goes back at shutdown.
			PrepareBenchmark();
			com->FrameWork()->AddHook(this);

			return true;
		}

		bool PreAppInit(void) override
		{
			RemoveVideoModeService();
			return true;
		}

		bool PostAppInit(void) override
		{
			StartBenchmark();
			return true;
		}

		bool PreAppShutdown(void) override
		{
			StopBenchmark();

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
