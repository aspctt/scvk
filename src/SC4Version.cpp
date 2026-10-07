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

#include "SC4Version.h"
#include "Logger.h"

#include <Windows.h>
#include <vector>

#pragma comment(lib, "version.lib")

namespace scvk
{
	//// Constants

	namespace
	{
		// The signature every VS_FIXEDFILEINFO carries, from the Windows SDK
		// documentation.
		constexpr DWORD FIXED_FILE_INFORMATION_SIGNATURE = 0xfeef04bd;

		// A byte that differs between builds, used when the version resource is missing.
		// The address is only mapped inside SimCity 4 itself.
		constexpr uintptr_t SENTINEL_ADDRESS = 0x6E5000;
	}

	//// Private Functions

	namespace
	{
		/** File version of the running executable, packed into 64 bits. */
		uint64_t ExecutableFileVersion(void)
		{
			// Read the executable's version resource
			char path[MAX_PATH];
			if (GetModuleFileNameA(nullptr, path, MAX_PATH) == 0)
			{
				return 0;
			}

			DWORD handle = 0;
			DWORD const size = GetFileVersionInfoSizeA(path, &handle);
			if (size == 0)
			{
				return 0;
			}

			std::vector<uint8_t> versionData(size);
			if (!GetFileVersionInfoA(path, handle, size, versionData.data()))
			{
				return 0;
			}

			// Find the fixed file information in it
			//
			// The query hands back a pointer into the resource through a void pointer out
			// parameter, so the typed pointer has to be passed as one.
			VS_FIXEDFILEINFO* fileInformation = nullptr;
			UINT fileInformationSize = 0;
			if (!VerQueryValueA(versionData.data(), "\\", reinterpret_cast<LPVOID*>(&fileInformation), &fileInformationSize) || fileInformationSize == 0 || fileInformation == nullptr)
			{
				return 0;
			}

			if (fileInformation->dwSignature != FIXED_FILE_INFORMATION_SIGNATURE)
			{
				return 0;
			}

			return (uint64_t{ fileInformation->dwFileVersionMS } << 32) | fileInformation->dwFileVersionLS;
		}

		/** True if the address can be read without faulting. */
		bool IsReadable(uintptr_t address)
		{
			// The query takes a pointer, and the address is a plain number from the
			// table.
			MEMORY_BASIC_INFORMATION memoryInformation{};
			if (VirtualQuery(reinterpret_cast<LPCVOID>(address), &memoryInformation, sizeof(memoryInformation)) == 0)
			{
				return false;
			}

			if (memoryInformation.State != MEM_COMMIT)
			{
				return false;
			}

			DWORD const readable = PAGE_READONLY | PAGE_READWRITE | PAGE_WRITECOPY | PAGE_EXECUTE_READ | PAGE_EXECUTE_READWRITE | PAGE_EXECUTE_WRITECOPY;
			return (memoryInformation.Protect & readable) != 0 && (memoryInformation.Protect & PAGE_GUARD) == 0;
		}

		/** Works out the patch number from the version resource or the sentinel byte. */
		uint16_t DetermineGameVersion(void)
		{
			// Split the file version into its fields
			//
			// Each field is 16 bits wide, so the masked value always fits.
			uint64_t const fileVersion = ExecutableFileVersion();

			uint16_t const major    = static_cast<uint16_t>((fileVersion >> 48) & 0xFFFF);
			uint16_t const minor    = static_cast<uint16_t>((fileVersion >> 32) & 0xFFFF);
			uint16_t const revision = static_cast<uint16_t>((fileVersion >> 16) & 0xFFFF);

			if (fileVersion != 0 && major == 1 && minor == 1)
			{
				return revision;
			}

			// Fall back to the sentinel byte
			//
			// Some copies have had the version resource stripped, so this sniffs a byte
			// that happens to differ between builds. Less trustworthy, and it cannot tell
			// 610 and 613 apart, but it is better than giving up.
			//
			// Reading it blind would fault in any other host, which matters because a
			// crash here would happen during plugin load, before anything has had a
			// chance to write a log line explaining why.
			if (!IsReadable(SENTINEL_ADDRESS))
			{
				return 0;
			}

			// The table holds a plain address, which has to become a pointer to be read.
			uint8_t const sentinel = *reinterpret_cast<uint8_t const*>(SENTINEL_ADDRESS);

			switch (sentinel)
			{
			case 0x8B: return 610;
			case 0xFF: return 638;
			case 0x24: return 640;
			case 0x0F: return 641;
			default:   return 0;
			}
		}
	}

	//// Public API

	uint16_t GetGameVersion(void)
	{
		static uint16_t const version = DetermineGameVersion();
		return version;
	}
}
