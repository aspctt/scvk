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

#include "FpsLimit.h"
#include "Logger.h"
#include "SC4Version.h"

#include <Windows.h>
#include <string.h>

namespace scvk
{
	namespace
	{
		struct SpeedLimit
		{
			char const* name;
			uintptr_t   address;
			uint8_t     original;
		};

		// Each address points at the one-byte immediate of a MOV that stores
		// the cap. Verified against game version 641 only; the addresses are
		// meaningless on any other build, which is why the version gate below
		// is not optional.
		constexpr SpeedLimit kLimits641[] = {
			{ "Cheetah", 0x70244A, 15 },
			{ "Rhino",   0x702457, 20 },
			{ "Turtle",  0x702462, 30 },
		};

		constexpr uint16_t kSupportedGameVersion = 641;

		/** Absolute path of scvk.ini, beside the DLL. */
		bool SettingsPath(char* out, size_t size)
		{
			HMODULE self = nullptr;
			if (!GetModuleHandleExA(
					GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
					reinterpret_cast<LPCSTR>(&SettingsPath),
					&self))
			{
				return false;
			}

			DWORD written = GetModuleFileNameA(self, out, static_cast<DWORD>(size));
			if (written == 0 || written >= size)
			{
				return false;
			}

			char* separator = strrchr(out, '\\');
			if (separator == nullptr)
			{
				return false;
			}

			separator[1] = '\0';
			if (strlen(out) + strlen("scvk.ini") >= size)
			{
				return false;
			}

			strcat_s(out, size, "scvk.ini");
			return true;
		}

		bool WriteByte(uintptr_t address, uint8_t value)
		{
			DWORD oldProtect = 0;
			if (!VirtualProtect(reinterpret_cast<LPVOID>(address), sizeof(value), PAGE_EXECUTE_READWRITE, &oldProtect))
			{
				return false;
			}

			*reinterpret_cast<uint8_t*>(address) = value;

			// Put the original protection back. Leaving a page of the game's
			// code writable for the rest of the session is a gratuitous risk
			// when we only needed it for one byte.
			DWORD restored = 0;
			VirtualProtect(reinterpret_cast<LPVOID>(address), sizeof(value), oldProtect, &restored);
			return true;
		}
	}

	void ApplyFpsLimitSettings(void)
	{
		char iniPath[MAX_PATH];
		if (!SettingsPath(iniPath, sizeof(iniPath)))
		{
			return;
		}

		// Absent or zero means do nothing, which is the default.
		int maxFps = GetPrivateProfileIntA("scvk", "MaxFPS", 0, iniPath);
		if (maxFps <= 0)
		{
			return;
		}

		if (maxFps > 255)
		{
			LogNote("MaxFPS=%d exceeds the one-byte field; clamping to 255.", maxFps);
			maxFps = 255;
		}

		uint16_t gameVersion = GetGameVersion();
		if (gameVersion != kSupportedGameVersion)
		{
			LogNote("MaxFPS requested but the FPS limit addresses are only known for game version %u (found %u). Leaving the limits alone.",
				kSupportedGameVersion, gameVersion);
			return;
		}

		// Check before writing. If a byte does not hold the value we expect,
		// something is wrong: the wrong build, or another plugin has already
		// patched it. Blind-writing in that situation could corrupt an
		// unrelated instruction, so refuse instead and say why.
		for (SpeedLimit const& limit : kLimits641)
		{
			uint8_t current = *reinterpret_cast<uint8_t const*>(limit.address);

			if (current == static_cast<uint8_t>(maxFps))
			{
				LogNote("%s speed cap is already %d; nothing to do. Another plugin has probably set it.",
					limit.name, maxFps);
				continue;
			}

			if (current != limit.original)
			{
				LogNote("REFUSING to patch the %s speed cap at %08X: expected %u, found %u. "
					"Another plugin may already have changed it, or this is not the build these addresses came from.",
					limit.name, limit.address, limit.original, current);
				continue;
			}

			if (WriteByte(limit.address, static_cast<uint8_t>(maxFps)))
			{
				LogNote("%s speed cap raised from %u to %d.", limit.name, limit.original, maxFps);
			}
			else
			{
				LogNote("Failed to write the %s speed cap at %08X, error %lu.",
					limit.name, limit.address, GetLastError());
			}
		}
	}
}
