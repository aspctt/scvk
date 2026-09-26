/*
 * scvk - a native Vulkan renderer for SimCity 4
 *
 * Copyright (C) 2026 aspctt
 *
 * The simulation speed FPS caps and the addresses that hold them were
 * identified by caspervg's sc4-disable-fps-limits
 * (https://github.com/caspervg/sc4-disable-fps-limits), LGPL-2.1-or-later.
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

#include "FpsLimit.h"
#include "Logger.h"
#include "SC4Version.h"

#include <Windows.h>
#include <stdint.h>
#include <string.h>

namespace scvk
{
	//// Types

	namespace
	{
		struct SpeedLimit
		{
			char const* name;
			uintptr_t   address;
			uint8_t     originalCap;
		};
	}

	//// Constants

	namespace
	{
		// Each address points at the one-byte immediate of a MOV that stores the cap.
		// Verified against game version 641 only; the addresses are meaningless on any
		// other build, which is why the version gate below is not optional.
		constexpr SpeedLimit SPEED_LIMITS_641[] = {
			{ "Cheetah", 0x70244A, 15 },
			{ "Rhino",   0x702457, 20 },
			{ "Turtle",  0x702462, 30 },
		};

		constexpr uint16_t SUPPORTED_GAME_VERSION = 641;

		// The caps are one-byte immediates, so nothing above this fits.
		constexpr int LARGEST_CAP = 255;
	}

	//// Private Functions

	namespace
	{
		/** Absolute path of scvk.ini, beside the DLL. */
		bool SettingsPath(char* outPath, size_t capacity)
		{
			// Find the module this code lives in
			//
			// The flag makes the API read its second argument as an address inside the
			// module rather than as a name, which is why a function pointer is passed
			// where the signature asks for a string.
			HMODULE module = nullptr;
			DWORD const flags = GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT;
			if (!GetModuleHandleExA(flags, reinterpret_cast<LPCSTR>(&SettingsPath), &module))
			{
				return false;
			}

			// Replace the file name in its path with the settings file's
			DWORD const written = GetModuleFileNameA(module, outPath, capacity);
			if (written == 0 || written >= capacity)
			{
				return false;
			}

			char* const separator = strrchr(outPath, '\\');
			if (separator == nullptr)
			{
				return false;
			}

			separator[1] = '\0';
			if (strlen(outPath) + strlen("scvk.ini") >= capacity)
			{
				return false;
			}

			strcat_s(outPath, capacity, "scvk.ini");
			return true;
		}

		/** Writes one byte of the game's code, restoring the page protection after. */
		bool WriteByte(uintptr_t address, uint8_t value)
		{
			// The table holds plain addresses, which have to become pointers to be
			// written.
			void* const target = reinterpret_cast<void*>(address);

			// Make the byte writable
			DWORD oldProtection = 0;
			if (!VirtualProtect(target, sizeof(value), PAGE_EXECUTE_READWRITE, &oldProtection))
			{
				return false;
			}

			*static_cast<uint8_t*>(target) = value;

			// Put the original protection back
			//
			// Leaving a page of the game's code writable for the rest of the session is a
			// gratuitous risk when we only needed it for one byte.
			DWORD restoredProtection = 0;
			VirtualProtect(target, sizeof(value), oldProtection, &restoredProtection);
			return true;
		}
	}

	//// Public API

	void ApplyFpsLimitSettings(void)
	{
		// Read the requested cap, where absent or zero means do nothing
		char settingsPath[MAX_PATH];
		if (!SettingsPath(settingsPath, sizeof(settingsPath)))
		{
			return;
		}

		// The API hands the number back unsigned; it is range checked as a signed one.
		int frameRateCap = static_cast<int>(GetPrivateProfileIntA("scvk", "MaxFPS", 0, settingsPath));
		if (frameRateCap <= 0)
		{
			return;
		}

		if (frameRateCap > LARGEST_CAP)
		{
			LogNote("MaxFPS=%d exceeds the one-byte field; clamping to 255.", frameRateCap);
			frameRateCap = LARGEST_CAP;
		}

		// Only patch the build the addresses came from
		uint16_t const gameVersion = GetGameVersion();
		if (gameVersion != SUPPORTED_GAME_VERSION)
		{
			LogNote("MaxFPS requested but the FPS limit addresses are only known for game version %u (found %u). Leaving the limits alone.", SUPPORTED_GAME_VERSION, gameVersion);
			return;
		}

		// Check each byte before writing it
		//
		// If a byte does not hold the value we expect, something is wrong: the wrong
		// build, or another plugin has already patched it. Blind-writing in that
		// situation could corrupt an unrelated instruction, so it refuses instead and
		// says why. The cap was clamped to one byte above, so its conversion loses
		// nothing.
		uint8_t const newCap = static_cast<uint8_t>(frameRateCap);

		for (SpeedLimit const& limit : SPEED_LIMITS_641)
		{
			// The table holds plain addresses, which have to become pointers to be read.
			uint8_t const currentCap = *reinterpret_cast<uint8_t const*>(limit.address);

			if (currentCap == newCap)
			{
				LogNote("%s speed cap is already %d; nothing to do. Another plugin has probably set it.", limit.name, frameRateCap);
				continue;
			}

			if (currentCap != limit.originalCap)
			{
				LogNote("REFUSING to patch the %s speed cap at %08X: expected %u, found %u. Another plugin may already have changed it, or this is not the build these addresses came from.", limit.name, limit.address, limit.originalCap, currentCap);
				continue;
			}

			if (WriteByte(limit.address, newCap))
			{
				LogNote("%s speed cap raised from %u to %d.", limit.name, limit.originalCap, frameRateCap);
			}
			else
			{
				LogNote("Failed to write the %s speed cap at %08X, error %lu.", limit.name, limit.address, GetLastError());
			}
		}
	}
}
