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
 * The LiveShadows cheat, carried over from SCD3D11.
 *
 * The game's cheat manager refuses a name that is already registered (RegisterCheatCode,
 * 0x00921B38, compares every name before adding one), so the first plugin to register
 * LiveShadows keeps it. scd3d11 registers its own whether it draws or not. When scvk
 * draws, scd3d11's cheat could only say that its live shadows are off, so scvk takes the
 * name back from it (UnregisterCheatCode, 0x00921817, drops the entry with an id).
 *
 * With live shadows off the cheat only says how to turn them on. Their patches go in at
 * startup, and the game makes its shadow records as a city loads, so they cannot start
 * in the middle of a game.
 */

//// Dependencies

#include "LiveShadowCheat.h"
#include "LiveShadowHooks.h"
#include "ModuleLog.h"
#include "Settings.h"
#include "SetupChecks.h"

#include <GZServPtrs.h>
#include <cIGZCheatCodeManager.h>
#include <cIGZMessage2Standard.h>
#include <cIGZMessageTarget2.h>
#include <cISC4App.h>
#include <cRZBaseString.h>

#include <windows.h>
#include <stdint.h>

namespace scvk::LiveShadowCheat
{
	//// Constants

	namespace
	{
		// What the cheat box sends for a registered cheat, with the cheat's id as its first
		// value
		constexpr uint32_t MESSAGE_CHEAT_ISSUED  = 0x230E27AC;
		constexpr uint32_t GZIID_MESSAGE_TARGET2 = 0x452294AA;

		// scvk's id for the cheat, and the one scd3d11 registers the same name under
		constexpr uint32_t LIVE_SHADOWS_CHEAT         = 0x5C7A11E0;
		constexpr uint32_t SCD3D11_LIVE_SHADOWS_CHEAT = 0x3D11C4EA;

		constexpr char const* CHEAT_NAME          = "LiveShadows";
		constexpr char const* SCD3D11_MODULE_NAME = "SCD3D11.dll";
	}

	//// State

	namespace
	{
		bool isInstalled       = false;
		bool isCheatRegistered = false;
	}

	//// Private Functions

	namespace
	{
		/** Says why live shadows cannot be paused, in the game's own dialog. */
		void ShowWhyOff(void)
		{
			if (GetSettings().areLiveShadowsEnabled)
			{
				ShowGameDialog("-LiveShadows:on is set, but another plugin has changed the game code it patches, so live shadows will stay turned off. Check \"live shadows:\" lines in scvk.log for details.");
				return;
			}

			ShowGameDialog("Live shadows are off. Start SimCity 4 with -LiveShadows:on, this cheat only works if live shadows are active.");
		}

		/** Hears the cheat box. Static, so its count is only for show. */
		class CheatListener final : public cIGZMessageTarget2
		{
		public:
			bool QueryInterface(uint32_t riid, void** ppvObj) override
			{
				if (riid == GZIID_MESSAGE_TARGET2 || riid == GZIID_cIGZUnknown)
				{
					*ppvObj = static_cast<cIGZMessageTarget2*>(this);
					AddRef();
					return true;
				}

				*ppvObj = nullptr;
				return false;
			}

			uint32_t AddRef(void) override
			{
				return ++referenceCount;
			}

			uint32_t Release(void) override
			{
				if (referenceCount > 0)
				{
					--referenceCount;
				}

				return referenceCount;
			}

			bool DoMessage(cIGZMessage2* message) override
			{
				if (message == nullptr || message->GetType() != MESSAGE_CHEAT_ISSUED)
				{
					return true;
				}

				// SendCheatNotifications (0x00920ECE) makes every message of this type as a
				// cIGZMessage2Standard, so the downcast holds. The id it carries is a 32-bit
				// value stored in a pointer-sized field.
				cIGZMessage2Standard const* const standard = static_cast<cIGZMessage2Standard const*>(message);
				uint32_t const cheat = static_cast<uint32_t>(standard->GetData1());
				if (cheat != LIVE_SHADOWS_CHEAT)
				{
					return true;
				}

				if (!LiveShadowHooks::Enabled())
				{
					ShowWhyOff();
					return true;
				}

				LiveShadowHooks::TogglePause();
				return true;
			}

		private:
			uint32_t referenceCount = 0;
		};

		CheatListener cheatListener;

		/** Registers the cheat's name, taking it back from scd3d11, which is idle while scvk draws. */
		bool RegisterCheat(cIGZCheatCodeManager* cheats)
		{
			cRZBaseString const name(CHEAT_NAME);
			if (cheats->RegisterCheatCode(LIVE_SHADOWS_CHEAT, name))
			{
				return true;
			}

			// Leave the name to any other plugin that has it
			if (GetModuleHandleA(SCD3D11_MODULE_NAME) == nullptr || !cheats->UnregisterCheatCode(SCD3D11_LIVE_SHADOWS_CHEAT) || !cheats->RegisterCheatCode(LIVE_SHADOWS_CHEAT, name))
			{
				Log(LogCategory::Initialization, "live shadows: another plugin has the LiveShadows cheat, so scvk's is not registered");
				return false;
			}

			Log(LogCategory::Initialization, "live shadows: the LiveShadows cheat taken over from SCD3D11, which is not drawing");
			return true;
		}
	}

	//// Public API

	void Install(void)
	{
		if (isInstalled)
		{
			return;
		}

		cISC4AppPtr app;
		cIGZCheatCodeManager* const cheats = app ? app->GetCheatCodeManager() : nullptr;
		if (cheats == nullptr || !cheats->AddNotification2(&cheatListener, 0))
		{
			Log(LogCategory::Initialization, "live shadows: the game's cheat manager could not be reached, so there is no LiveShadows cheat");
			return;
		}

		isInstalled       = true;
		isCheatRegistered = RegisterCheat(cheats);
	}

	void Uninstall(void)
	{
		if (!isInstalled)
		{
			return;
		}

		cISC4AppPtr app;
		cIGZCheatCodeManager* const cheats = app ? app->GetCheatCodeManager() : nullptr;
		if (cheats != nullptr)
		{
			if (isCheatRegistered)
			{
				cheats->UnregisterCheatCode(LIVE_SHADOWS_CHEAT);
			}

			cheats->RemoveNotification2(&cheatListener, 0);
		}

		isInstalled       = false;
		isCheatRegistered = false;
	}
}
