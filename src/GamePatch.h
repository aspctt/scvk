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

#pragma once

//// Dependencies

#include <initializer_list>
#include <stddef.h>
#include <stdint.h>

/**
 * The game-side shadow modules' way into the game: its code and data by the addresses
 * Ghidra shows, patches guarded by the exact bytes they replace, and reads of the game's
 * memory that survive a bad pointer.
 *
 * Addresses are those of version 641 at its preferred image base, 0x00400000. Every patch
 * checks the bytes it expects before writing, so a plugin that changed them first, or
 * another build of the game, leaves the patch out rather than half applied.
 */
namespace scvk
{
	//// Types

	/** A few bytes of the game's code or data that a hook replaces, and puts back. */
	class GamePatch
	{
	public:
		//// Constants

		/** The longest patch: the 35 bytes of the network shadow mask test. */
		static constexpr size_t MAXIMUM_SIZE = 35;

		//// Public API

		/** An empty patch, which writes and restores nothing. */
		GamePatch(void) = default;

		/**
		 * Replaces the instructions at address with a jump to destination, a hook or a place
		 * in the game's own code, padded with no-ops so nothing lands in the middle of an
		 * instruction.
		 */
		template <typename Target>
		static GamePatch Jump(uintptr_t address, std::initializer_list<uint8_t> original, Target* destination)
		{
			// Only the destination's address goes into the jump. It is a function or code in
			// this 32-bit process, whose address fits a uintptr_t.
			return MakeJump(address, original, reinterpret_cast<uintptr_t>(destination));
		}

		/** Replaces a pointer to the game's own function originalTarget, such as a virtual table slot. */
		template <typename Function>
		static GamePatch Pointer(uintptr_t address, uintptr_t originalTarget, Function* replacement)
		{
			// As in Jump: the slot only holds the function's address
			return MakePointer(address, originalTarget, reinterpret_cast<uintptr_t>(replacement));
		}

		/** Replaces the bytes at address with others of the same length. */
		static GamePatch Bytes(uintptr_t address, std::initializer_list<uint8_t> original, std::initializer_list<uint8_t> replacement);

		/** The patched address as the game's image places it at 0x00400000, for the log. */
		unsigned long GameAddress(void) const;

		/** Whether the game still has the bytes this patch expects to replace. */
		bool IsOriginal(void) const;

		/** Writes the patch over the game's bytes. Returns false when the memory could not be written. */
		bool Write(void);

		/**
		 * Puts the game's bytes back.
		 *
		 * Returns false, leaving them alone, when something else changed them after this
		 * patch was written: restoring then would undo that change instead.
		 */
		bool Restore(void);

	private:
		//// Private Functions

		static GamePatch MakeJump(uintptr_t address, std::initializer_list<uint8_t> original, uintptr_t destination);
		static GamePatch MakePointer(uintptr_t address, uintptr_t originalTarget, uintptr_t replacement);

		/** Copies replacement or the original over the target, with the protection put back after. */
		bool WriteCode(uint8_t const* bytes);

		//// State

		uintptr_t gameAddress = 0;
		uint8_t*  target      = nullptr;
		size_t    size        = 0;
		uint8_t   original[MAXIMUM_SIZE]    = {};
		uint8_t   replacement[MAXIMUM_SIZE] = {};
		bool      isWritten = false;
	};

	//// Public API

	/** Where an address of the game's image, as Ghidra shows it, lies in this process. */
	uint8_t* ResolveGameAddress(uintptr_t address);

	/**
	 * Copies from the game's memory, returning false instead of crashing where the source
	 * cannot be read.
	 *
	 * The game's objects are reached through offsets found by reverse engineering, and an
	 * object freed while a hook still holds its address must not take the game down. Only
	 * MSVC builds can catch the fault, so other builds always return false, which leaves
	 * their shadow modules off.
	 */
	bool CopyGameMemory(void const* source, void* destination, size_t size);

	/** Reads one value from the game's memory, as CopyGameMemory copies it. */
	template <typename Type>
	bool ReadGameMemory(void const* source, Type& value)
	{
		return CopyGameMemory(source, &value, sizeof(Type));
	}

	/** The function in a slot of a game object's virtual table, or null where either cannot be read. */
	void* ReadVirtualFunction(void const* object, size_t slot);
}
