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

/*
 * Which renderer the game will use, and what scvk holds back until it knows.
 *
 * scvk loads whether it draws or not. With scd3d11 installed the game draws with that
 * unless told otherwise, and the player can choose the Software driver, so anything scvk
 * changes before the game has picked a driver changes a game scvk may not be drawing.
 */

//// Dependencies

#include "RendererChoice.h"
#include "FpsLimit.h"
#include "NativeShadowDiagnostics.h"
#include "NativeShadowExperiment.h"
#include "NativeShadowMasks.h"
#include "NativeShadowRegistry.h"
#include "SC4Version.h"
#include "TerrainShadows.h"
#include "VulkanBackend.h"

#include <cIGZGraphicSystem2.h>
#include <GZServPtrs.h>

#include <windows.h>
#include <string.h>

namespace scvk
{
	//// Constants

	namespace
	{
		constexpr uint16_t SUPPORTED_GAME_VERSION = 641;

		// What ReadRequestedDriver gives when it cannot tell. No driver has this class.
		constexpr uint32_t UNKNOWN_DRIVER = 0;

		// The class SetDefaultDriverClassID stores, which the interface has no way to read
		// back, as an offset from the interface. The game asks for it before it starts any
		// driver (0x44ce40), and sc4-graphics-options does the same from its own plugin.
		constexpr size_t REQUESTED_DRIVER_OFFSET = 0x64;

		// SetDefaultDriverClassID's place in the interface's table of functions, and its
		// code (0x990010): MOV EAX,[ESP+4], MOV [ECX+0x64],EAX, RET 4. Reading the code
		// proves the offset above, whatever else has changed.
		constexpr size_t  SET_DEFAULT_DRIVER_TABLE_INDEX = 8;
		constexpr uint8_t SET_DEFAULT_DRIVER_CODE[]      = { 0x8B, 0x44, 0x24, 0x04, 0x89, 0x41, 0x64, 0xC2, 0x04, 0x00 };

		// scd3d11's DLL, by the name its release ships it under. It outranks scvk in the
		// DirectX slot, and the game loads every plugin before it starts any service.
		constexpr char const* SCD3D11_MODULE_NAME = "SCD3D11.dll";
	}

	//// State

	namespace
	{
		bool isGameChanged = false;
	}

	//// Private Functions

	namespace
	{
		/** The driver class the game will ask for first, or UNKNOWN_DRIVER when that cannot be read. */
		uint32_t ReadRequestedDriver(void)
		{
			// Only read a field of the build it came from
			if (GetGameVersion() != SUPPORTED_GAME_VERSION)
			{
				return UNKNOWN_DRIVER;
			}

			cIGZGraphicSystem2Ptr graphics;
			if (!graphics)
			{
				return UNKNOWN_DRIVER;
			}

			// Check the setter still stores it where this reads it
			//
			// An object starts with a pointer to its table of functions, and the table's
			// entries are code, so each step takes a cast. The compiler's layout of the
			// interface is the game's as well, or no plugin could call it.
			cIGZGraphicSystem2* const system = graphics;
			void const* const* const table = *reinterpret_cast<void const* const* const*>(system);

			if (memcmp(table[SET_DEFAULT_DRIVER_TABLE_INDEX], SET_DEFAULT_DRIVER_CODE, sizeof(SET_DEFAULT_DRIVER_CODE)) != 0)
			{
				return UNKNOWN_DRIVER;
			}

			// Read the class
			//
			// The field sits behind the interface, in the graphics system's own data. It
			// stays empty when nothing asked (0x44ce40 leaves it alone for a preference it
			// does not know), and the game then starts its DirectX slot (0x9904d0).
			uint8_t const* const data = reinterpret_cast<uint8_t const*>(system);
			uint32_t const requested = *reinterpret_cast<uint32_t const*>(data + REQUESTED_DRIVER_OFFSET);

			return (requested != 0) ? requested : DIRECTX_DRIVER_GZCLSID;
		}
	}

	//// Public API

	bool IsDriverSlotRequested(uint32_t slotClass)
	{
		uint32_t const requested = ReadRequestedDriver();
		return requested == UNKNOWN_DRIVER || requested == slotClass;
	}

	bool IsSoftwareDriverRequested(void)
	{
		return ReadRequestedDriver() == SOFTWARE_DRIVER_GZCLSID;
	}

	bool WillScvkDraw(void)
	{
		uint32_t const requested = ReadRequestedDriver();

		if (requested == SOFTWARE_DRIVER_GZCLSID)
		{
			return false;
		}

		if (requested == DIRECTX_DRIVER_GZCLSID && GetModuleHandleA(SCD3D11_MODULE_NAME) != nullptr)
		{
			return false;
		}

		return IsVulkanAvailable();
	}

	void ApplyGameChanges(void)
	{
		if (isGameChanged)
		{
			return;
		}

		isGameChanged = true;

		// Change the game's frame pacing: no padding while paused, an exact animation
		// clock, and speed caps at the display's refresh rate
		//
		// The first SetVideoMode is early enough. The speeds and the paused path only run
		// in a city, and the game builds its animation clock when one loads (0x7f41b0).
		ApplyFpsLimitSettings();

		// Install the game-side shadow modules SCD3D11 has, each guarded by the exact
		// bytes it patches. They are opt-in, as in SCD3D11: the live True3D shadows with
		// -NativeShadowMasks:network, props or all, the registry and the terrain's shadows
		// on top of them with replace, and the diagnostics only when asked for. scd3d11
		// reads the same switches and patches the same code, so these wait until scvk is
		// the one drawing.
		NativeShadowMasks::Install();
		NativeShadowRegistry::Install();
		TerrainShadows::Install();
		NativeShadowDiagnostics::Install();
		NativeShadowExperiment::Install();
	}
}
