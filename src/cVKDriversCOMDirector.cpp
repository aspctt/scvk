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

#include "cVKDriver.h"
#include "FpsLimit.h"
#include "Logger.h"
#include "SC4Version.h"
#include "version.h"

#include <cIGZCOM.h>
#include <cIGZFrameWork.h>
#include <cRZCOMDllDirector.h>

namespace scvk
{
	namespace
	{
		// Identifies this plugin to the GZCOM. Must be unique across every
		// installed DLL, and must not be SCGL's (0xCB6EC543).
		constexpr uint32_t kDirectorID = 0x5C4B0001;
	}

	/**
	 * Registers scvk's driver with the game.
	 *
	 * SimCity 4 chooses a renderer by class ID and only knows three: DirectX,
	 * OpenGL and Software. There is no way to add a fourth, so scvk registers
	 * itself under the OpenGL class ID and relies on the GZCOM preferring the
	 * higher version number when two libraries claim the same class.
	 *
	 * That is why EnumClassObjects is overridden. The inherited implementation
	 * reports version 0 for everything it registers, which would tie with the
	 * game's built-in driver rather than beat it.
	 */
	class cVKDriversCOMDirector final : public cRZCOMDllDirector
	{
	public:
		cVKDriversCOMDirector(void)
		{
			AddCls(cVKDriver::kDriverGZCLSID, &cVKDriver::FactoryFunction);
		}

		uint32_t GetDirectorID(void) const override
		{
			return kDirectorID;
		}

		void EnumClassObjects(ClassObjectEnumerationCallback pCallback, void* pContext) override
		{
			// Outranks the game's own OpenGL driver, which registers the same
			// class ID at a lower version.
			pCallback(cVKDriver::kDriverGZCLSID, cVKDriver::kDriverVersion, pContext);
		}

		bool OnStart(cIGZCOM* pCOM) override
		{
			LogOpen();
			LogNote("scvk %s loaded; claiming GZCLSID %08x at version %u.",
				SCVK_VERSION_STRING, cVKDriver::kDriverGZCLSID, cVKDriver::kDriverVersion);
			LogNote("Detected SimCity 4 version %u.", GetGameVersion());

			// Off unless scvk.ini asks for it. This is the only place scvk
			// writes to game memory, and it is opt-in for that reason.
			ApplyFpsLimitSettings();

			cIGZFrameWork* const framework = RZGetFrameWork();
			if (framework != nullptr)
			{
				// The graphics system is created during app init, so the hook
				// has to be in place before then. If we have already missed
				// that point, run the callback directly rather than never.
				if (framework->GetState() < cIGZFrameWork::kStatePreAppInit)
				{
					framework->AddHook(this);
				}
				else
				{
					PreAppInit();
				}
			}
			else
			{
				LogNote("WARNING: no framework available at OnStart.");
			}

			return true;
		}
	};
}

cRZCOMDllDirector* RZGetCOMDllDirector(void)
{
	static scvk::cVKDriversCOMDirector sDirector;
	return &sDirector;
}
