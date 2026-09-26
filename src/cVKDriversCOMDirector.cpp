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
#include "SC4Version.h"
#include "version.h"

#include <cIGZCOM.h>
#include <cRZCOMDllDirector.h>

namespace scvk
{
	//// Constants

	namespace
	{
		// Identifies this plugin to the GZCOM. Must be unique across every installed DLL,
		// and must not be SCGL's (0xCB6EC543).
		constexpr uint32_t DIRECTOR_ID = 0x5C4B0001;
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
			LogNote("scvk %s loaded; claiming GZCLSID %08x at version %u.", SCVK_VERSION_STRING, cVKDriver::DRIVER_GZCLSID, cVKDriver::DRIVER_VERSION);
			LogNote("Detected SimCity 4 version %u.", GetGameVersion());

			// Off unless scvk.ini asks for it. This is the only place scvk writes to game
			// memory, and it is opt-in for that reason.
			ApplyFpsLimitSettings();
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
