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

#include "SC4Version.h"
#include "Logger.h"

#include <Windows.h>
#include <vector>

#pragma comment(lib, "version.lib")

namespace scvk
{
	namespace
	{
		/** File version of the running executable, packed into 64 bits. */
		uint64_t ExecutableFileVersion(void)
		{
			char path[MAX_PATH];
			if (GetModuleFileNameA(nullptr, path, MAX_PATH) == 0)
			{
				return 0;
			}

			DWORD handle = 0;
			DWORD size = GetFileVersionInfoSizeA(path, &handle);
			if (size == 0)
			{
				return 0;
			}

			std::vector<uint8_t> data(size);
			if (!GetFileVersionInfoA(path, handle, size, data.data()))
			{
				return 0;
			}

			VS_FIXEDFILEINFO* info = nullptr;
			UINT infoSize = 0;
			if (!VerQueryValueA(data.data(), "\\", reinterpret_cast<LPVOID*>(&info), &infoSize) ||
				infoSize == 0 || info == nullptr)
			{
				return 0;
			}

			if (info->dwSignature != 0xfeef04bd)
			{
				return 0;
			}

			return (static_cast<uint64_t>(info->dwFileVersionMS) << 32) | info->dwFileVersionLS;
		}

		/** True if the address can be read without faulting. */
		bool IsReadable(uintptr_t address)
		{
			MEMORY_BASIC_INFORMATION info{};
			if (VirtualQuery(reinterpret_cast<LPCVOID>(address), &info, sizeof(info)) == 0)
			{
				return false;
			}

			if (info.State != MEM_COMMIT)
			{
				return false;
			}

			DWORD const readable = PAGE_READONLY | PAGE_READWRITE | PAGE_WRITECOPY |
				PAGE_EXECUTE_READ | PAGE_EXECUTE_READWRITE | PAGE_EXECUTE_WRITECOPY;

			return (info.Protect & readable) != 0 && (info.Protect & PAGE_GUARD) == 0;
		}

		uint16_t DetermineGameVersion(void)
		{
			uint64_t fileVersion = ExecutableFileVersion();

			uint16_t major    = (fileVersion >> 48) & 0xFFFF;
			uint16_t minor    = (fileVersion >> 32) & 0xFFFF;
			uint16_t revision = (fileVersion >> 16) & 0xFFFF;

			if (fileVersion != 0 && major == 1 && minor == 1)
			{
				return revision;
			}

			// Some copies have had the version resource stripped, so fall back
			// to sniffing a byte that happens to differ between builds. Less
			// trustworthy, and it cannot tell 610 and 613 apart, but it is
			// better than giving up.
			//
			// The address is only mapped inside SimCity 4 itself. Reading it
			// blind would fault in any other host, which matters because a
			// crash here would happen during plugin load, before anything has
			// had a chance to write a log line explaining why.
			constexpr uintptr_t kSentinelAddress = 0x6E5000;

			if (!IsReadable(kSentinelAddress))
			{
				return 0;
			}

			uint8_t sentinel = *reinterpret_cast<uint8_t const*>(kSentinelAddress);

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

	uint16_t GetGameVersion(void)
	{
		static uint16_t const version = DetermineGameVersion();
		return version;
	}
}
