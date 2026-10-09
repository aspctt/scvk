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

		// The simulator's frame tick (0x703E70) pads every frame with simulation and idle
		// work until the frame has lasted 1000 / rate milliseconds, at least 15 of them
		// padding. While the city is paused, or hidden-paused while the camera moves, the
		// rate is a fixed 30 rather than the speed's cap, so a paused city sits at 30 frames
		// a second that MaxFPS never reaches.
		//
		// The short budget both patches use instead. The idle agents each get a third of the
		// budget, so this is the smallest that still hands each of them a whole millisecond.
		constexpr uint8_t TICK_BUDGET_MILLISECONDS = 3;

		// A relative JMP: the opcode, then a 32-bit offset counted from the end of the jump.
		constexpr uint8_t JUMP_OPCODE = 0xE9;
		constexpr size_t  JUMP_SIZE   = 5;

		// MOV ECX,30 on the paused path becomes a jump to a few instructions of scvk's, which
		// store the short budget where the tick keeps it ([ESI+0x70]) and rejoin the tick
		// just after the store (0x703EFE), past the division and the minimum. A hidden pause
		// leaves the padding loop before reading the budget, so camera movement is unchanged.
		//
		// The first version fit in the five bytes on its own, as PUSH 3, POP EAX and a jump to
		// the game's own store at 0x703EFB. scd3d11's sim tick budget replaces the minimum and
		// that store with a jump to its own code and fills the rest with no-ops, though, so
		// with both plugins installed a paused frame stored nothing and kept the last running
		// frame's budget. Rejoining past those bytes works whichever plugin changes them.
		constexpr uintptr_t PAUSED_RATE_ADDRESS_641   = 0x703EE2;
		constexpr uintptr_t PAUSED_REJOIN_ADDRESS_641 = 0x703EFE;
		constexpr uint8_t   PAUSED_RATE_ORIGINAL[]    = { 0xB9, 0x1E, 0x00, 0x00, 0x00 };

		// MOV DWORD PTR [ESI+0x70],3, then the jump back, whose offset is filled in once the
		// code's own address is known.
		constexpr uint8_t PAUSED_BUDGET_CODE[]      = { 0xC7, 0x46, 0x70, TICK_BUDGET_MILLISECONDS, 0x00, 0x00, 0x00, JUMP_OPCODE, 0x00, 0x00, 0x00, 0x00 };
		constexpr size_t  PAUSED_BUDGET_JUMP_OFFSET = 7;

		// CMP EAX,15, JG and MOV EAX,15 clamp the running budget to at least 15 ms, which
		// keeps a padded frame near 60 frames a second whatever the cap. Lowering both
		// immediates lets MaxFPS go past that, at the cost of the simulation's time.
		constexpr uintptr_t MINIMUM_BUDGET_ADDRESS_641 = 0x703EF1;
		constexpr uint8_t   MINIMUM_BUDGET_ORIGINAL[]  = { 0x83, 0xF8, 0x0F, 0x7F, 0x05, 0xB8, 0x0F, 0x00, 0x00, 0x00 };
		constexpr uint8_t   MINIMUM_BUDGET_PATCHED[]   = { 0x83, 0xF8, TICK_BUDGET_MILLISECONDS, 0x7F, 0x05, 0xB8, TICK_BUDGET_MILLISECONDS, 0x00, 0x00, 0x00 };

		// The animation clock (cSC4AnimationTickManager) hands lot animations the time since
		// its last tick, once a frame, but counts any tick shorter than 2 ms as 2 ms. Above
		// 500 frames a second, which an unpadded paused city reaches, the animations run
		// ahead of real time, up to twice as fast. Its constructor (0x448E30) stores that
		// floor in microseconds with MOV [ESI+0x50],2000, and nothing changes it after.
		//
		// The floor it gets instead, which keeps the time exact up to 10,000 frames a second.
		// Some floor stays, so a tick never reports no time at all.
		constexpr uint8_t ANIMATION_FLOOR_MICROSECONDS = 100;

		constexpr uintptr_t ANIMATION_FLOOR_ADDRESS_641 = 0x448EA2;
		constexpr uint8_t   ANIMATION_FLOOR_ORIGINAL[]  = { 0xC7, 0x46, 0x50, 0xD0, 0x07, 0x00, 0x00 };
		constexpr uint8_t   ANIMATION_FLOOR_PATCHED[]   = { 0xC7, 0x46, 0x50, ANIMATION_FLOOR_MICROSECONDS, 0x00, 0x00, 0x00 };

		static_assert(sizeof(PAUSED_RATE_ORIGINAL) == JUMP_SIZE, "a patch must replace exactly the bytes it checked");
		static_assert(sizeof(PAUSED_BUDGET_CODE) == PAUSED_BUDGET_JUMP_OFFSET + JUMP_SIZE, "the jump back must end the paused code");
		static_assert(sizeof(MINIMUM_BUDGET_ORIGINAL) == sizeof(MINIMUM_BUDGET_PATCHED), "a patch must replace exactly the bytes it checked");
		static_assert(sizeof(ANIMATION_FLOOR_ORIGINAL) == sizeof(ANIMATION_FLOOR_PATCHED), "a patch must replace exactly the bytes it checked");
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

		/** Whether a true or false setting in scvk.ini is true, or the default when it is absent. */
		bool IsSettingTrue(char const* settingsPath, char const* key, bool isTrueByDefault)
		{
			char value[16] = {};
			GetPrivateProfileStringA("scvk", key, isTrueByDefault ? "true" : "false", value, sizeof(value), settingsPath);

			return _stricmp(value, "true") == 0 || strcmp(value, "1") == 0;
		}

		/** Writes bytes of the game's code, restoring the page protection after. */
		bool WriteBytes(uintptr_t address, uint8_t const* bytes, size_t count)
		{
			// The tables hold plain addresses, which have to become pointers to be
			// written.
			void* const target = reinterpret_cast<void*>(address);

			// Make the bytes writable
			DWORD oldProtection = 0;
			if (!VirtualProtect(target, count, PAGE_EXECUTE_READWRITE, &oldProtection))
			{
				return false;
			}

			memcpy(target, bytes, count);

			// Put the original protection back
			//
			// Leaving a page of the game's code writable for the rest of the session is a
			// gratuitous risk when we only needed it for a few bytes. Changed instructions
			// also have to reach the instruction cache before they next run.
			DWORD restoredProtection = 0;
			VirtualProtect(target, count, oldProtection, &restoredProtection);
			FlushInstructionCache(GetCurrentProcess(), target, count);
			return true;
		}

		/**
		 * Replaces a run of the game's instructions, after checking it holds exactly what
		 * the patch expects. Anything else, another plugin's patch or another build, is left
		 * alone and logged, since writing over it could break an unrelated instruction.
		 */
		void PatchCode(char const* name, uintptr_t address, uint8_t const* original, uint8_t const* patched, size_t count)
		{
			// The table holds plain addresses, which have to become pointers to be read.
			uint8_t const* const current = reinterpret_cast<uint8_t const*>(address);

			if (memcmp(current, patched, count) == 0)
			{
				LogInfo("%s is already changed; nothing to do.", name);
				return;
			}

			if (memcmp(current, original, count) != 0)
			{
				LogWarn("REFUSING to change the %s at %08X: its bytes are not the ones expected. Another plugin may already have changed them, or this is not the build these addresses came from.", name, address);
				return;
			}

			if (!WriteBytes(address, patched, count))
			{
				LogError("Failed to change the %s at %08X, error %lu.", name, address, GetLastError());
				return;
			}

			LogInfo("Changed the %s.", name);
		}

		/** Writes a relative JMP from one address to another into five bytes. */
		void EncodeJump(uint8_t* outBytes, uintptr_t from, uintptr_t to)
		{
			// The offset may be negative. Addresses are 32 bits in this process, so the
			// unsigned subtraction wraps to the same bits a signed one would give.
			uint32_t const offset = to - (from + JUMP_SIZE);

			outBytes[0] = JUMP_OPCODE;
			memcpy(outBytes + 1, &offset, sizeof(offset));
		}

		/**
		 * Puts the paused path's few instructions in memory of their own and returns their
		 * address, or 0 when that fails. Built once and never freed: the game jumps there on
		 * every paused frame until the process ends, possibly after this DLL is unloaded.
		 */
		uintptr_t PausedBudgetCode(void)
		{
			static uintptr_t codeAddress = 0;
			if (codeAddress != 0)
			{
				return codeAddress;
			}

			// Write the instructions while the memory can be written but not run
			void* const memory = VirtualAlloc(nullptr, sizeof(PAUSED_BUDGET_CODE), MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
			if (memory == nullptr)
			{
				return 0;
			}

			// The jump back is counted from the memory's own address, which has to become a
			// number for that.
			uint8_t* const bytes = static_cast<uint8_t*>(memory);
			uintptr_t const address = reinterpret_cast<uintptr_t>(memory);

			memcpy(bytes, PAUSED_BUDGET_CODE, sizeof(PAUSED_BUDGET_CODE));
			EncodeJump(bytes + PAUSED_BUDGET_JUMP_OFFSET, address + PAUSED_BUDGET_JUMP_OFFSET, PAUSED_REJOIN_ADDRESS_641);

			// Then let them run, and no longer be written
			DWORD oldProtection = 0;
			if (!VirtualProtect(memory, sizeof(PAUSED_BUDGET_CODE), PAGE_EXECUTE_READ, &oldProtection))
			{
				VirtualFree(memory, 0, MEM_RELEASE);
				return 0;
			}

			FlushInstructionCache(GetCurrentProcess(), memory, sizeof(PAUSED_BUDGET_CODE));
			codeAddress = address;
			return codeAddress;
		}

		/**
		 * Sends the paused path to the code above, with the checks every patch makes. A
		 * refused patch leaves that code built but unused, which costs one page of memory.
		 */
		void PatchPausedPadding(void)
		{
			uintptr_t const codeAddress = PausedBudgetCode();
			if (codeAddress == 0)
			{
				LogError("Failed to set aside memory for the paused frame padding, error %lu.", GetLastError());
				return;
			}

			uint8_t patched[JUMP_SIZE] = {};
			EncodeJump(patched, PAUSED_RATE_ADDRESS_641, codeAddress);
			PatchCode("paused frame padding", PAUSED_RATE_ADDRESS_641, PAUSED_RATE_ORIGINAL, patched, sizeof(patched));
		}
	}

	//// Public API

	void ApplyFpsLimitSettings(void)
	{
		// Read the settings
		//
		// Without a settings path both stay as if absent: no cap, and the running padding
		// left alone. The API hands the cap back unsigned; it is range checked as a signed
		// one.
		int  frameRateCap              = 0;
		bool shouldUnlockRunningFrames = false;

		char settingsPath[MAX_PATH];
		if (SettingsPath(settingsPath, sizeof(settingsPath)))
		{
			frameRateCap              = static_cast<int>(GetPrivateProfileIntA("scvk", "MaxFPS", 0, settingsPath));
			shouldUnlockRunningFrames = IsSettingTrue(settingsPath, "UnlockRunningFPS", false);
		}

		// Only patch the build the addresses came from
		uint16_t const gameVersion = GetGameVersion();
		if (gameVersion != SUPPORTED_GAME_VERSION)
		{
			LogWarn("The frame pacing addresses are only known for game version %u (found %u). Leaving the game's frame rate alone.", SUPPORTED_GAME_VERSION, gameVersion);
			return;
		}

		// Stop padding the frames of a paused city
		//
		// Always, since the padding only gives the idle agents time while nothing is being
		// simulated.
		PatchPausedPadding();

		// Let the animation clock count short frames as they are
		//
		// Always, since without the paused padding a paused city easily runs fast enough
		// for the old floor to speed its animations up. The game builds its clocks after
		// scvk starts, so they all get the new floor.
		PatchCode("animation clock floor", ANIMATION_FLOOR_ADDRESS_641, ANIMATION_FLOOR_ORIGINAL, ANIMATION_FLOOR_PATCHED, sizeof(ANIMATION_FLOOR_ORIGINAL));

		// Lower the running city's minimum padding, when asked
		if (shouldUnlockRunningFrames)
		{
			PatchCode("running frame padding minimum", MINIMUM_BUDGET_ADDRESS_641, MINIMUM_BUDGET_ORIGINAL, MINIMUM_BUDGET_PATCHED, sizeof(MINIMUM_BUDGET_ORIGINAL));
		}

		// Raise the speed caps, when asked
		if (frameRateCap <= 0)
		{
			return;
		}

		if (frameRateCap > LARGEST_CAP)
		{
			LogWarn("MaxFPS=%d exceeds the one-byte field; clamping to 255.", frameRateCap);
			frameRateCap = LARGEST_CAP;
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
				LogInfo("%s speed cap is already %d; nothing to do. Another plugin has probably set it.", limit.name, frameRateCap);
				continue;
			}

			if (currentCap != limit.originalCap)
			{
				LogWarn("REFUSING to patch the %s speed cap at %08X: expected %u, found %u. Another plugin may already have changed it, or this is not the build these addresses came from.", limit.name, limit.address, limit.originalCap, currentCap);
				continue;
			}

			if (WriteBytes(limit.address, &newCap, sizeof(newCap)))
			{
				LogInfo("%s speed cap raised from %u to %d.", limit.name, limit.originalCap, frameRateCap);
			}
			else
			{
				LogError("Failed to write the %s speed cap at %08X, error %lu.", limit.name, limit.address, GetLastError());
			}
		}
	}
}
