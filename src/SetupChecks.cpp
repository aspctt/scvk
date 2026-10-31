/*
 * scvk - a native Vulkan renderer for SimCity 4
 *
 * Copyright (C) 2026 aspctt
 *
 * Game version detection follows the approach used by SC4Fix
 * (Copyright (c) 2015 Nelson Gomez, MIT License), including the sentinel byte
 * table used when the executable carries no version resource.
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

#include "SetupChecks.h"
#include "Logger.h"

#include <windows.h>
#include <tlhelp32.h>
#include <stdint.h>
#include <string.h>
#include <vector>

#if defined(_MSC_VER)
#pragma comment(lib, "version.lib")
#endif

namespace scvk
{
	//// Types

	namespace
	{
		/** The first three fields of a file version, which is what plugins release by. */
		struct PluginVersion
		{
			uint32_t major;
			uint32_t minor;
			uint32_t patch;
		};
	}

	//// Constants

	namespace
	{
		constexpr char const* RENDER_SERVICES_FILE_NAME = "SC4RenderServices.dll";

		// The first version that sends the game's own stencil operation numbers. Older
		// ones send SCGL's, one lower, so scvk's stencil marks nothing and no terrain decal
		// is drawn back over the shadows.
		constexpr PluginVersion RENDER_SERVICES_MINIMUM = { 0, 8, 3 };

		// The signature every VS_FIXEDFILEINFO carries, from the Windows SDK
		// documentation.
		constexpr DWORD FIXED_FILE_INFORMATION_SIGNATURE = 0xfeef04bd;
	}

	//// Private Functions

	namespace
	{
		/** Reads a DLL's file version. False when it has no version resource. */
		bool ReadPluginVersion(char const* path, PluginVersion& outVersion)
		{
			// Read the version resource
			DWORD handle = 0;
			DWORD const size = GetFileVersionInfoSizeA(path, &handle);
			if (size == 0)
			{
				return false;
			}

			std::vector<uint8_t> versionData(size);
			if (!GetFileVersionInfoA(path, handle, size, versionData.data()))
			{
				return false;
			}

			// Find the fixed file information in it
			//
			// The query hands back a pointer into the resource through a void pointer out
			// parameter, so the typed pointer has to be passed as one.
			VS_FIXEDFILEINFO* fileInformation = nullptr;
			UINT fileInformationSize = 0;
			if (!VerQueryValueA(versionData.data(), "\\", reinterpret_cast<LPVOID*>(&fileInformation), &fileInformationSize) || fileInformation == nullptr)
			{
				return false;
			}

			if (fileInformation->dwSignature != FIXED_FILE_INFORMATION_SIGNATURE)
			{
				return false;
			}

			outVersion.major = HIWORD(fileInformation->dwFileVersionMS);
			outVersion.minor = LOWORD(fileInformation->dwFileVersionMS);
			outVersion.patch = HIWORD(fileInformation->dwFileVersionLS);
			return true;
		}

		bool IsOlderThan(PluginVersion const& version, PluginVersion const& minimum)
		{
			if (version.major != minimum.major)
			{
				return version.major < minimum.major;
			}

			if (version.minor != minimum.minor)
			{
				return version.minor < minimum.minor;
			}

			return version.patch < minimum.patch;
		}

		void CheckRenderServices(char const* path)
		{
			PluginVersion version{};
			if (!ReadPluginVersion(path, version))
			{
				return;
			}

			// Leave builds from source alone
			//
			// SC4RenderServices only stamps its real version into release builds, and any
			// other build says 0.0.1 whatever its code is.
			if (version.major == 0 && version.minor == 0)
			{
				return;
			}

			if (IsOlderThan(version, RENDER_SERVICES_MINIMUM))
			{
				LogWarn("SC4RenderServices %u.%u.%u is older than %u.%u.%u, so terrain decals under shadows stay hidden. Update it.", version.major, version.minor, version.patch, RENDER_SERVICES_MINIMUM.major, RENDER_SERVICES_MINIMUM.minor, RENDER_SERVICES_MINIMUM.patch);
			}
		}
	}

	//// Public API

	void CheckPluginSetup(void)
	{
		// Find this copy of scvk
		//
		// By its address rather than its name, since the point is to tell two files of the
		// same name apart. The call takes the address as if it were a name, which the flag
		// tells it to read as an address.
		HMODULE ownModule = nullptr;
		DWORD const flags = GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT;
		if (!GetModuleHandleExA(flags, reinterpret_cast<LPCSTR>(&CheckPluginSetup), &ownModule))
		{
			return;
		}

		char ownPath[MAX_PATH];
		if (GetModuleFileNameA(ownModule, ownPath, MAX_PATH) == 0)
		{
			return;
		}

		char const* const lastSeparator = strrchr(ownPath, '\\');
		char const* const ownFileName = (lastSeparator != nullptr) ? (lastSeparator + 1) : ownPath;

		// Look through the loaded modules
		//
		// A snapshot is the one way to see two modules of the same name, since asking for a
		// module by name only ever finds the first.
		HANDLE const snapshot = CreateToolhelp32Snapshot(TH32CS_SNAPMODULE, GetCurrentProcessId());
		if (snapshot == INVALID_HANDLE_VALUE)
		{
			return;
		}

		MODULEENTRY32 entry{};
		entry.dwSize = sizeof(entry);

		for (BOOL hasEntry = Module32First(snapshot, &entry); hasEntry; hasEntry = Module32Next(snapshot, &entry))
		{
			if (entry.hModule == ownModule)
			{
				continue;
			}

			if (_stricmp(entry.szModule, ownFileName) == 0)
			{
				LogWarn("Two copies of scvk are loaded, %s and %s. Delete one of them.", ownPath, entry.szExePath);
			}
			else if (_stricmp(entry.szModule, RENDER_SERVICES_FILE_NAME) == 0)
			{
				CheckRenderServices(entry.szExePath);
			}
		}

		CloseHandle(snapshot);
	}
}
