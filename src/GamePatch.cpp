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

#include "GamePatch.h"

#include <windows.h>
#include <string.h>

namespace scvk
{
	//// Constants

	namespace
	{
		constexpr uintptr_t IMAGE_BASE    = 0x00400000;
		constexpr uint8_t   JUMP_OPCODE   = 0xE9;
		constexpr size_t    JUMP_SIZE     = 5;
		constexpr uint8_t   NO_OPERATION  = 0x90;
		constexpr size_t    POINTER_SIZE  = sizeof(uint32_t);
	}

	//// Public API

	GamePatch GamePatch::MakeJump(uintptr_t address, std::initializer_list<uint8_t> original, uintptr_t destination)
	{
		GamePatch patch;
		if (original.size() < JUMP_SIZE || original.size() > MAXIMUM_SIZE)
		{
			return patch;
		}

		patch.gameAddress = address;
		patch.target      = ResolveGameAddress(address);
		patch.size        = original.size();
		memcpy(patch.original, original.begin(), patch.size);

		// The jump's offset counts from its own end. Both ends are in this 32-bit process,
		// so the difference always fits the 32 bits the instruction holds, and wraps the
		// same way the processor adds it back.
		memset(patch.replacement, NO_OPERATION, patch.size);
		patch.replacement[0] = JUMP_OPCODE;
		uint32_t const offset = static_cast<uint32_t>(destination - (reinterpret_cast<uintptr_t>(patch.target) + JUMP_SIZE));
		memcpy(patch.replacement + 1, &offset, sizeof(offset));
		return patch;
	}

	GamePatch GamePatch::MakePointer(uintptr_t address, uintptr_t originalTarget, uintptr_t replacement)
	{
		GamePatch patch;
		patch.gameAddress = address;
		patch.target      = ResolveGameAddress(address);
		patch.size        = POINTER_SIZE;

		// The slot holds the game function's address wherever the image was loaded. A
		// pointer is 32 bits in this process, so it fits the slot exactly.
		uint32_t const expected = static_cast<uint32_t>(reinterpret_cast<uintptr_t>(ResolveGameAddress(originalTarget)));
		uint32_t const value    = static_cast<uint32_t>(replacement);
		memcpy(patch.original, &expected, sizeof(expected));
		memcpy(patch.replacement, &value, sizeof(value));
		return patch;
	}

	GamePatch GamePatch::Bytes(uintptr_t address, std::initializer_list<uint8_t> original, std::initializer_list<uint8_t> replacement)
	{
		GamePatch patch;
		if (original.size() != replacement.size() || original.size() == 0 || original.size() > MAXIMUM_SIZE)
		{
			return patch;
		}

		patch.gameAddress = address;
		patch.target      = ResolveGameAddress(address);
		patch.size        = original.size();
		memcpy(patch.original, original.begin(), patch.size);
		memcpy(patch.replacement, replacement.begin(), patch.size);
		return patch;
	}

	unsigned long GamePatch::GameAddress(void) const
	{
		return static_cast<unsigned long>(gameAddress);
	}

	bool GamePatch::IsOriginal(void) const
	{
		uint8_t current[MAXIMUM_SIZE] = {};
		return target != nullptr && size != 0 && CopyGameMemory(target, current, size) && memcmp(current, original, size) == 0;
	}

	bool GamePatch::Write(void)
	{
		if (isWritten)
		{
			return true;
		}

		if (target == nullptr || size == 0 || !WriteCode(replacement))
		{
			return false;
		}

		isWritten = true;
		return true;
	}

	bool GamePatch::Restore(void)
	{
		if (!isWritten)
		{
			return true;
		}

		// Whatever happens next, this patch no longer owns the bytes
		isWritten = false;

		uint8_t current[MAXIMUM_SIZE] = {};
		if (!CopyGameMemory(target, current, size) || memcmp(current, replacement, size) != 0)
		{
			return false;
		}

		return WriteCode(original);
	}

	bool GamePatch::WriteCode(uint8_t const* bytes)
	{
		DWORD protection = 0;
		if (!VirtualProtect(target, size, PAGE_EXECUTE_READWRITE, &protection))
		{
			return false;
		}

		memcpy(target, bytes, size);

		DWORD ignored = 0;
		VirtualProtect(target, size, protection, &ignored);
		FlushInstructionCache(GetCurrentProcess(), target, size);
		return true;
	}

	uint8_t* ResolveGameAddress(uintptr_t address)
	{
		// The module handle is the image's address in memory
		uint8_t* const image = reinterpret_cast<uint8_t*>(GetModuleHandleW(nullptr));
		return (image != nullptr) ? image + (address - IMAGE_BASE) : nullptr;
	}

	void* ReadVirtualFunction(void const* object, size_t slot)
	{
		// An object of the game's classes starts with its virtual table's address
		void* const* table    = nullptr;
		void*        function = nullptr;

		if (object == nullptr || !ReadGameMemory(object, table) || table == nullptr || !ReadGameMemory(table + slot, function))
		{
			return nullptr;
		}

		return function;
	}

#if defined(_MSC_VER)
	bool CopyGameMemory(void const* source, void* destination, size_t size)
	{
		__try
		{
			memcpy(destination, source, size);
			return true;
		}
		__except (EXCEPTION_EXECUTE_HANDLER)
		{
			return false;
		}
	}
#else
	bool CopyGameMemory(void const*, void*, size_t)
	{
		return false;
	}
#endif
}
