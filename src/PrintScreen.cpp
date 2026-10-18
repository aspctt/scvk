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

#include "PrintScreen.h"
#include "Logger.h"

#include <atomic>
#include <cstring>

#include <windows.h>

namespace scvk::PrintScreen
{
	//// Constants

	namespace
	{
		// How long Uninstall waits for the hook's thread to finish.
		constexpr DWORD THREAD_EXIT_MILLISECONDS = 2000;

		// How long Install waits for the hook to be in place.
		constexpr DWORD THREAD_START_MILLISECONDS = 2000;

		// How often, and how far apart, opening a clipboard another program holds is tried.
		constexpr int   CLIPBOARD_ATTEMPTS     = 10;
		constexpr DWORD CLIPBOARD_RETRY_MILLISECONDS = 10;
	}

	//// Variables

	namespace
	{
		// The window the key is taken for, read on the hook's thread.
		std::atomic<HWND> watchedWindow{ nullptr };

		// Set on the hook's thread when the key is released, cleared by TakeRequest.
		std::atomic<bool> isRequested{ false };

		// The hook's thread, and the event that says its hook is in place.
		HANDLE hookThread   = nullptr;
		DWORD  hookThreadId = 0;
		HANDLE hookReady    = nullptr;
		bool   isHookInstalled = false;
	}

	//// Private Functions

	namespace
	{
		/** Whether either Windows key is held. */
		bool IsWindowsKeyHeld(void)
		{
			return (GetAsyncKeyState(VK_LWIN) & 0x8000) != 0 || (GetAsyncKeyState(VK_RWIN) & 0x8000) != 0;
		}

		/** Sees every key before any program does, and keeps PrintScreen for the game. */
		LRESULT CALLBACK KeyboardHook(int code, WPARAM wParam, LPARAM lParam)
		{
			if (code == HC_ACTION)
			{
				KBDLLHOOKSTRUCT const* const key = reinterpret_cast<KBDLLHOOKSTRUCT const*>(lParam);
				HWND const window = watchedWindow.load();

				// Only the physical key, only while the game is in front, and never with
				// a Windows key, whose capture goes through the compositor and works
				bool const isPrintScreen = key->vkCode == VK_SNAPSHOT && (key->flags & LLKHF_INJECTED) == 0;

				if (isPrintScreen && window != nullptr && GetForegroundWindow() == window && !IsIconic(window) && !IsWindowsKeyHeld())
				{
					// Act on the release, which is when Windows itself acts, and keep both
					// halves from everyone else
					if (wParam == WM_KEYUP || wParam == WM_SYSKEYUP)
					{
						isRequested.store(true);
					}

					return 1;
				}
			}

			return CallNextHookEx(nullptr, code, wParam, lParam);
		}

		/** Runs the hook: a low-level hook is called on the thread that set it, through its messages. */
		DWORD WINAPI HookThread([[maybe_unused]] void* parameter)
		{
			// Make the thread's message queue, so Uninstall's message cannot be lost
			MSG message{};
			PeekMessageW(&message, nullptr, WM_USER, WM_USER, PM_NOREMOVE);

			HMODULE module = nullptr;
			GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT, reinterpret_cast<LPCWSTR>(&KeyboardHook), &module);

			HHOOK const hook = SetWindowsHookExW(WH_KEYBOARD_LL, &KeyboardHook, module, 0);
			if (hook == nullptr)
			{
				LogWarn("PrintScreen: could not watch the key, error %lu; Windows keeps it.", GetLastError());
			}

			SetEvent(hookReady);

			if (hook == nullptr)
			{
				return 0;
			}

			while (GetMessageW(&message, nullptr, 0, 0) > 0)
			{
				DispatchMessageW(&message);
			}

			UnhookWindowsHookEx(hook);
			return 0;
		}
	}

	//// Public API

	void Install(void* window)
	{
		watchedWindow.store(static_cast<HWND>(window));

		if (isHookInstalled)
		{
			return;
		}

		// Start the hook's thread and wait for its hook
		hookReady = CreateEventW(nullptr, TRUE, FALSE, nullptr);
		if (hookReady == nullptr)
		{
			return;
		}

		hookThread = CreateThread(nullptr, 0, &HookThread, nullptr, 0, &hookThreadId);
		if (hookThread == nullptr)
		{
			CloseHandle(hookReady);
			hookReady = nullptr;
			return;
		}

		WaitForSingleObject(hookReady, THREAD_START_MILLISECONDS);
		isHookInstalled = true;
		LogInfo("PrintScreen: copies the game's own frame to the clipboard while it is fullscreen.");
	}

	void Uninstall(void)
	{
		watchedWindow.store(nullptr);
		isRequested.store(false);

		if (!isHookInstalled)
		{
			return;
		}

		// End the thread's message loop, which takes the hook out
		PostThreadMessageW(hookThreadId, WM_QUIT, 0, 0);

		if (WaitForSingleObject(hookThread, THREAD_EXIT_MILLISECONDS) != WAIT_OBJECT_0)
		{
			LogWarn("PrintScreen: the key's thread did not finish in time.");
		}

		CloseHandle(hookThread);
		CloseHandle(hookReady);
		hookThread      = nullptr;
		hookReady       = nullptr;
		hookThreadId    = 0;
		isHookInstalled = false;
	}

	bool TakeRequest(void)
	{
		return isRequested.exchange(false);
	}

	bool CopyToClipboard(void* window, uint8_t const* bgraPixels, uint32_t width, uint32_t height)
	{
		if (bgraPixels == nullptr || width == 0 || height == 0)
		{
			return false;
		}

		// Lay the picture out as a DIB: a header, then the rows bottom first, which every
		// program reads, where top-first ones are not
		size_t const rowBytes   = size_t{ width } * 4u;
		size_t const pixelBytes = rowBytes * height;

		HGLOBAL const memory = GlobalAlloc(GMEM_MOVEABLE, sizeof(BITMAPINFOHEADER) + pixelBytes);
		if (memory == nullptr)
		{
			return false;
		}

		void* const locked = GlobalLock(memory);
		if (locked == nullptr)
		{
			GlobalFree(memory);
			return false;
		}

		BITMAPINFOHEADER header{};
		header.biSize        = sizeof(BITMAPINFOHEADER);
		header.biWidth       = static_cast<LONG>(width);
		header.biHeight      = static_cast<LONG>(height);
		header.biPlanes      = 1;
		header.biBitCount    = 32;
		header.biCompression = BI_RGB;
		header.biSizeImage   = static_cast<DWORD>(pixelBytes);
		memcpy(locked, &header, sizeof(header));

		uint8_t* const rows = static_cast<uint8_t*>(locked) + sizeof(BITMAPINFOHEADER);
		for (uint32_t row = 0; row < height; row++)
		{
			memcpy(rows + rowBytes * (height - 1u - row), bgraPixels + rowBytes * row, rowBytes);
		}

		GlobalUnlock(memory);

		// Hand it to the clipboard, waiting briefly for a program that has it open
		bool isOpen = false;
		for (int attempt = 0; attempt < CLIPBOARD_ATTEMPTS && !isOpen; attempt++)
		{
			isOpen = OpenClipboard(static_cast<HWND>(window)) != 0;
			if (!isOpen)
			{
				Sleep(CLIPBOARD_RETRY_MILLISECONDS);
			}
		}

		if (!isOpen)
		{
			GlobalFree(memory);
			return false;
		}

		// The clipboard owns the memory once it takes it
		EmptyClipboard();
		bool const isSet = SetClipboardData(CF_DIB, memory) != nullptr;
		CloseClipboard();

		if (!isSet)
		{
			GlobalFree(memory);
		}

		return isSet;
	}
}
