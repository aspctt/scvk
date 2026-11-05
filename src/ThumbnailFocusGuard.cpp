/*
 *  SCD3D11 - a free Direct3D 11 driver for SimCity 4's SimGL interface
 *  Copyright (C) 2026
 *
 *  This library is free software; you can redistribute it and/or
 *  modify it under the terms of the GNU Lesser General Public
 *  License as published by the Free Software Foundation, under
 *  version 2.1 of the License, or (at your option) any later version.
 */

/*
 * How the game guards its thumbnail, and the two patches that change it.
 *
 * SC4WriteCityRegionViewThumbnail (0x005DDEC0) renders the city four times through the
 * normal 3D view and reads each render back with CopyColorBuffer. The Windows build, not
 * the Mac one, wraps this in a focus check:
 *
 * - 0x005F85F0 returns GetActiveWindow() == GetForegroundWindow(). Only this function
 *   calls it, from four places: before rendering, after rendering and twice in the wait
 *   loop below.
 * - A listener for the app activation message 0x4348B111 sets a "focus was lost" byte at
 *   [listener+9] on every deactivation. A failed check before or after rendering sets it
 *   too.
 * - If that byte is set after rendering, the game polls the focus every 500 ms for about
 *   10 seconds. When the focus comes back it renders again, at most 3 levels deep, and
 *   otherwise it writes the blank default thumbnail over the one it rendered.
 *
 * scvk renders into its own frame image and CopyColorBuffer reads that image, so neither
 * the focus nor other windows can spoil the capture. Two patches make the check after
 * rendering ask about that instead:
 *
 *   0x005F85F0  push esi; call [GetForegroundWindow]     becomes  jmp IsCaptureUsable
 *   0x005DE976  mov al,[esi+9]; test al,al; jz 005DE9F7  becomes  mov al,[esi+8]; test al,al; jnz 005DE9F7
 *
 * [esi+8] is the result of the check after rendering, stored two instructions earlier. A
 * minimised window still counts as unusable, since the game's WM_SIZE handler passes its
 * 0x0 client area on to the game's window system, and falls back to the game's own wait
 * and retry.
 */

//// Dependencies

#include "ThumbnailFocusGuard.h"
#include "ModuleLog.h"
#include "SC4Version.h"

#include <windows.h>
#include <string.h>

// The address a function returns to, which MSVC and GCC (MinGW, as Linux builds use)
// spell differently
#if defined(_MSC_VER)
#include <intrin.h>
#pragma intrinsic(_ReturnAddress)
#define SCVK_RETURN_ADDRESS() _ReturnAddress()
#else
#define SCVK_RETURN_ADDRESS() __builtin_return_address(0)
#endif

namespace scvk::ThumbnailFocusGuard
{
	//// Constants

	namespace
	{
		constexpr uint16_t  SUPPORTED_GAME_VERSION = 641;
		constexpr uintptr_t IMAGE_BASE             = 0x00400000;

		// The focus check, which becomes a jump to scvk's own. The jump takes five bytes and
		// two no-ops fill the rest, so nothing jumps into the middle of an instruction.
		constexpr uintptr_t FOCUS_CHECK_ADDRESS_641 = 0x005F85F0;
		constexpr size_t    FOCUS_CHECK_SIZE        = 7;
		constexpr uint8_t   FOCUS_CHECK_ORIGINAL[]  = { 0x56, 0xFF, 0x15, 0x0C, 0x05, 0xA8, 0x00 };
		constexpr uint8_t   JUMP_OPCODE             = 0xE9;
		constexpr size_t    JUMP_SIZE               = 5;
		constexpr uint8_t   NO_OPERATION            = 0x90;

		// The retry decision after rendering, which reads the check's result instead of the
		// "focus was lost" byte and retries when it failed rather than when it passed.
		constexpr uintptr_t RETRY_CHECK_ADDRESS_641 = 0x005DE976;
		constexpr size_t    RETRY_CHECK_SIZE        = 7;
		constexpr uint8_t   RETRY_CHECK_ORIGINAL[]  = { 0x8A, 0x46, 0x09, 0x84, 0xC0, 0x74, 0x7A };
		constexpr uint8_t   RETRY_CHECK_PATCHED[]   = { 0x8A, 0x46, 0x08, 0x84, 0xC0, 0x75, 0x7A };

		// Where the checks before and after rendering return to. The other two calls are
		// the game's wait loop, which keeps the plain answer.
		constexpr uintptr_t BEFORE_RENDER_RETURN_ADDRESS_641 = 0x005DDF07;
		constexpr uintptr_t AFTER_RENDER_RETURN_ADDRESS_641  = 0x005DE967;

		static_assert(sizeof(FOCUS_CHECK_ORIGINAL) == FOCUS_CHECK_SIZE);
		static_assert(sizeof(RETRY_CHECK_ORIGINAL) == RETRY_CHECK_SIZE && sizeof(RETRY_CHECK_PATCHED) == RETRY_CHECK_SIZE);
	}

	//// State

	namespace
	{
		uint8_t*  focusCheck  = nullptr;
		uint8_t*  retryCheck  = nullptr;
		uintptr_t imageOffset = 0;
		bool      isInstalled = false;
		bool      wasTried    = false;

		// The main thread only touches these: the driver window and the game's thumbnail
		// code both run there.
		HWND     gameWindow                = nullptr;
		unsigned minimises                 = 0;
		unsigned deactivations             = 0;
		unsigned minimisesBeforeRender     = 0;
		unsigned deactivationsBeforeRender = 0;
	}

	//// Private Functions

	namespace
	{
		bool IsWindowUsable(void)
		{
			return gameWindow == nullptr || !IsWindow(gameWindow) || !IsIconic(gameWindow);
		}

		/**
		 * Replaces the game's focus check.
		 *
		 * The game reaches it through the patched jump, so its return address is the game's
		 * own call site, which tells the checks before and after rendering apart.
		 */
		bool __cdecl IsCaptureUsable(void)
		{
			// The return address is a pointer only to be compared with the game's addresses,
			// moved by wherever the game was loaded.
			uintptr_t const caller = reinterpret_cast<uintptr_t>(SCVK_RETURN_ADDRESS()) - imageOffset;
			bool const isUsable = IsWindowUsable();

			// Remember the counts before rendering, to compare with after
			if (caller == BEFORE_RENDER_RETURN_ADDRESS_641)
			{
				minimisesBeforeRender     = minimises;
				deactivationsBeforeRender = deactivations;
				return isUsable;
			}

			if (caller != AFTER_RENDER_RETURN_ADDRESS_641)
			{
				return isUsable;
			}

			// Let the game wait and retry when the window was minimised
			if (!isUsable || minimises != minimisesBeforeRender)
			{
				Log(LogCategory::Window, "thumbnail: window minimized while rendering, leaving SC4 to wait and retry");
				return false;
			}

			// Keep the render whatever happened to the focus
			if (deactivations != deactivationsBeforeRender || GetActiveWindow() != GetForegroundWindow())
			{
				Log(LogCategory::Window, "thumbnail: kept the render although SC4 lost focus during it (%u deactivations, foreground %p)", deactivations - deactivationsBeforeRender, static_cast<void*>(GetForegroundWindow()));
			}

			return true;
		}

		/** Writes over a few bytes of the game's code, putting their protection back after. */
		bool WriteCode(uint8_t* target, uint8_t const* bytes, size_t size)
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
	}

	//// Public API

	bool Install(void* window)
	{
		// The window is the game's HWND, passed as void* so the header needs no windows.h.
		gameWindow = static_cast<HWND>(window);

		if (isInstalled)
		{
			return true;
		}

		if (wasTried)
		{
			return false;
		}

		wasTried = true;

		if (GetGameVersion() != SUPPORTED_GAME_VERSION)
		{
			Log(LogCategory::Initialization, "thumbnail focus guard: requires SimCity 4 1.1.641, skipping");
			return false;
		}

		// Find the code, wherever the game was loaded
		//
		// The module handle is the image's address in memory, which turns the fixed
		// addresses above into pointers.
		uint8_t* const image = reinterpret_cast<uint8_t*>(GetModuleHandleW(nullptr));
		if (image == nullptr)
		{
			return false;
		}

		imageOffset = reinterpret_cast<uintptr_t>(image) - IMAGE_BASE;
		focusCheck  = image + (FOCUS_CHECK_ADDRESS_641 - IMAGE_BASE);
		retryCheck  = image + (RETRY_CHECK_ADDRESS_641 - IMAGE_BASE);

		// Leave code another plugin already changed alone
		if (memcmp(focusCheck, FOCUS_CHECK_ORIGINAL, FOCUS_CHECK_SIZE) != 0 || memcmp(retryCheck, RETRY_CHECK_ORIGINAL, RETRY_CHECK_SIZE) != 0)
		{
			Log(LogCategory::Initialization, "thumbnail focus guard: SC4WriteCityRegionViewThumbnail already modified, skipping");
			return false;
		}

		// Build the jump to scvk's check
		//
		// The offset counts from the end of the jump. Both ends are in this 32-bit process,
		// so the difference always fits in the 32 bits the instruction holds.
		uint8_t jump[FOCUS_CHECK_SIZE] = { JUMP_OPCODE, 0, 0, 0, 0, NO_OPERATION, NO_OPERATION };
		int32_t const offset = static_cast<int32_t>(reinterpret_cast<uintptr_t>(&IsCaptureUsable) - (reinterpret_cast<uintptr_t>(focusCheck) + JUMP_SIZE));
		memcpy(jump + 1, &offset, sizeof(offset));

		// Patch the retry decision first, putting it back if the jump cannot be written
		//
		// The new decision reads the result the jump's check leaves, so it never runs
		// without it.
		if (!WriteCode(retryCheck, RETRY_CHECK_PATCHED, RETRY_CHECK_SIZE))
		{
			return false;
		}

		if (!WriteCode(focusCheck, jump, FOCUS_CHECK_SIZE))
		{
			WriteCode(retryCheck, RETRY_CHECK_ORIGINAL, RETRY_CHECK_SIZE);
			return false;
		}

		isInstalled = true;
		Log(LogCategory::Initialization, "thumbnail focus guard installed: losing focus while saving no longer blanks the region thumbnail");
		return true;
	}

	void Uninstall(void)
	{
		if (!isInstalled)
		{
			return;
		}

		WriteCode(focusCheck, FOCUS_CHECK_ORIGINAL, FOCUS_CHECK_SIZE);
		WriteCode(retryCheck, RETRY_CHECK_ORIGINAL, RETRY_CHECK_SIZE);
		isInstalled = false;
		gameWindow  = nullptr;
		Log(LogCategory::Initialization, "thumbnail focus guard uninstalled");
	}

	void NoteWindowMessage(uint32_t message, uintptr_t wParam)
	{
		if (message == WM_SIZE && wParam == SIZE_MINIMIZED)
		{
			minimises++;
		}
		else if (message == WM_ACTIVATEAPP && wParam == FALSE)
		{
			deactivations++;
		}
	}
}
