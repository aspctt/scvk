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

/*
 * The render watchdog, carried over from SCD3D11.
 */

//// Dependencies

#include "Watchdog.h"
#include "Logger.h"

#include <windows.h>
#include <algorithm>
#include <atomic>
#include <mutex>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <thread>

namespace scvk
{
	//// Constants

	namespace
	{
		// No frame for this long, once frames were flowing, is a stall.
		constexpr ULONGLONG STALL_MILLISECONDS = 5000;

		// A stall still going is reported again this far apart, a few times at most.
		constexpr ULONGLONG STALL_REPEAT_MILLISECONDS = 15000;
		constexpr unsigned  STALL_REPORTS_PER_STALL   = 5;

		// Likely return addresses listed from the stalled thread's stack.
		constexpr size_t STACK_CANDIDATES = 12;

		// More for a fault, which happens once and is worth the lines.
		constexpr size_t FAULT_STACK_CANDIDATES = 20;

		// The game loads plugins on the render thread after its first frames; that is not
		// a stall. The watchdog arms once frames have kept coming for this many of its
		// half-second ticks.
		constexpr unsigned FLOWING_TICKS = 10;
	}

	//// State

	namespace
	{
		std::atomic<char const*> renderPhase{ "none" };
		std::atomic<ULONGLONG>   lastRenderFrame{ 0 };
		std::atomic<DWORD>       renderThreadId{ 0 };
		std::atomic<HWND>        watchedWindow{ nullptr };
		std::mutex               watchdogMutex;
		std::thread              watchdogThread;
		HANDLE                   watchdogStop = nullptr;
	}

	//// Private Functions

	namespace
	{
		/** "module+0xRVA", or the bare address outside any module. */
		void DescribeAddress(uintptr_t address, char* text, size_t size)
		{
			HMODULE module = nullptr;
			char    path[MAX_PATH] = {};

			if (!GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT, reinterpret_cast<LPCSTR>(address), &module) || GetModuleFileNameA(module, path, MAX_PATH) == 0)
			{
				sprintf_s(text, size, "%p", reinterpret_cast<void*>(address));
				return;
			}

			char const* const slash = strrchr(path, '\\');
			sprintf_s(text, size, "%p %s+0x%lx", reinterpret_cast<void*>(address), (slash != nullptr) ? slash + 1 : path, static_cast<unsigned long>(address - reinterpret_cast<uintptr_t>(module)));
		}

		/** Whether a stack word points just after a call instruction in loaded code. */
		bool IsLikelyReturnAddress(uintptr_t address)
		{
			MEMORY_BASIC_INFORMATION memory{};
			if (address < 6 || VirtualQuery(reinterpret_cast<LPCVOID>(address - 6), &memory, sizeof(memory)) != sizeof(memory))
			{
				return false;
			}

			DWORD const executable = PAGE_EXECUTE | PAGE_EXECUTE_READ | PAGE_EXECUTE_READWRITE | PAGE_EXECUTE_WRITECOPY;
			if (memory.State != MEM_COMMIT || memory.Type != MEM_IMAGE || (memory.Protect & executable) == 0)
			{
				return false;
			}

			// call rel32, call [reg+disp8] or call reg, call [mem] or call [reg+disp32]
			uint8_t const* const code = reinterpret_cast<uint8_t const*>(address);
			return code[-5] == 0xE8 || code[-2] == 0xFF || code[-3] == 0xFF || code[-6] == 0xFF;
		}

		void LogRenderThreadLocation(ULONGLONG stalled)
		{
			uintptr_t instruction  = 0;
			uintptr_t stack[512]   = {};
			SIZE_T    stackBytes   = 0;
			DWORD const threadId   = renderThreadId.load();
			HANDLE const thread    = OpenThread(THREAD_SUSPEND_RESUME | THREAD_GET_CONTEXT, FALSE, threadId);

			if (thread != nullptr)
			{
				// Only copy while suspended: the thread may hold the heap or loader lock
				if (SuspendThread(thread) != static_cast<DWORD>(-1))
				{
					CONTEXT context{};
					context.ContextFlags = CONTEXT_CONTROL;

					if (GetThreadContext(thread, &context))
					{
#if defined(_M_IX86) || defined(__i386__)
						instruction = context.Eip;
						uintptr_t const stackPointer = context.Esp;
#else
						instruction = context.Rip;
						uintptr_t const stackPointer = context.Rsp;
#endif
						ReadProcessMemory(GetCurrentProcess(), reinterpret_cast<LPCVOID>(stackPointer), stack, sizeof(stack), &stackBytes);
					}

					ResumeThread(thread);
				}

				CloseHandle(thread);
			}

			HWND const window     = watchedWindow.load();
			HWND const foreground = GetForegroundWindow();
			DWORD foregroundProcess = 0;

			if (foreground != nullptr)
			{
				GetWindowThreadProcessId(foreground, &foregroundProcess);
			}

			char location[MAX_PATH + 64] = {};
			DescribeAddress(instruction, location, sizeof(location));

			char const* const owner = (foreground == window) ? "driver window" : ((foregroundProcess == GetCurrentProcessId()) ? "this process" : "other process");
			LogWarn("watchdog: no frame for %llu ms; phase=%s thread=%lu at %s; window=%p iconic=%d visible=%d foreground=%p (%s)", stalled, renderPhase.load(), threadId, location, static_cast<void*>(window), (window != nullptr && IsIconic(window)) ? 1 : 0, (window != nullptr && IsWindowVisible(window)) ? 1 : 0, static_cast<void*>(foreground), owner);

			size_t found = 0;
			for (size_t index = 0; index < stackBytes / sizeof(uintptr_t) && found < STACK_CANDIDATES; index++)
			{
				if (!IsLikelyReturnAddress(stack[index]))
				{
					continue;
				}

				DescribeAddress(stack[index], location, sizeof(location));
				LogWarn("watchdog:   [esp+0x%zX] %s", index * sizeof(uintptr_t), location);
				found++;
			}
		}

		/** Copies what is readable of a stack upward from a pointer, at most the buffer. */
		SIZE_T ReadStack(uintptr_t stackPointer, uintptr_t* out, SIZE_T capacity)
		{
			// Stop at the end of the committed region, past which the stack is not
			MEMORY_BASIC_INFORMATION memory{};
			if (VirtualQuery(reinterpret_cast<LPCVOID>(stackPointer), &memory, sizeof(memory)) != sizeof(memory) || memory.State != MEM_COMMIT)
			{
				return 0;
			}

			uintptr_t const regionEnd = reinterpret_cast<uintptr_t>(memory.BaseAddress) + memory.RegionSize;
			SIZE_T const    bytes     = std::min<SIZE_T>(capacity, regionEnd - stackPointer);
			SIZE_T          read      = 0;

			if (!ReadProcessMemory(GetCurrentProcess(), reinterpret_cast<LPCVOID>(stackPointer), out, bytes, &read))
			{
				return 0;
			}

			return read;
		}

		void RunRenderWatchdog(HANDLE stop)
		{
			ULONGLONG stalledFrame  = 0;
			ULONGLONG nextReport    = 0;
			unsigned  reports       = 0;
			ULONGLONG previousFrame = 0;
			unsigned  framesFlowing = 0;

			while (WaitForSingleObject(stop, 500) == WAIT_TIMEOUT)
			{
				ULONGLONG const frame = lastRenderFrame.load();
				if (frame == 0)
				{
					continue;
				}

				// Arm once frames have kept coming for a while
				if (framesFlowing < FLOWING_TICKS)
				{
					framesFlowing = (frame != previousFrame) ? framesFlowing + 1 : 0;
					previousFrame = frame;
					continue;
				}

				ULONGLONG const now = GetTickCount64();

				if (stalledFrame != 0 && frame != stalledFrame)
				{
					LogWarn("watchdog: frames resumed after %llu ms", frame - stalledFrame);
					stalledFrame = 0;
				}

				if (now - frame < STALL_MILLISECONDS)
				{
					continue;
				}

				if (stalledFrame == 0)
				{
					stalledFrame = frame;
					reports      = 0;
					nextReport   = now;
				}

				if (now >= nextReport && reports < STALL_REPORTS_PER_STALL)
				{
					LogRenderThreadLocation(now - frame);
					reports++;
					nextReport = now + STALL_REPEAT_MILLISECONDS;
				}
			}
		}
	}

	//// Public API

	void NoteRenderPhase(char const* phase)
	{
		renderPhase.store(phase, std::memory_order_relaxed);
	}

	void NoteRenderFrame(void)
	{
		renderThreadId.store(GetCurrentThreadId(), std::memory_order_relaxed);
		lastRenderFrame.store(GetTickCount64(), std::memory_order_relaxed);
	}

	void StartRenderWatchdog(void* window)
	{
		watchedWindow.store(static_cast<HWND>(window));

		std::lock_guard<std::mutex> const lock(watchdogMutex);
		if (watchdogThread.joinable())
		{
			return;
		}

		watchdogStop = CreateEventW(nullptr, TRUE, FALSE, nullptr);
		if (watchdogStop == nullptr)
		{
			return;
		}

		watchdogThread = std::thread(RunRenderWatchdog, watchdogStop);
	}

	void StopRenderWatchdog(void)
	{
		std::lock_guard<std::mutex> const lock(watchdogMutex);

		if (watchdogThread.joinable())
		{
			SetEvent(watchdogStop);
			watchdogThread.join();
		}

		if (watchdogStop != nullptr)
		{
			CloseHandle(watchdogStop);
			watchdogStop = nullptr;
		}

		watchedWindow.store(nullptr);
		lastRenderFrame.store(0);
	}

	void LogStackFrom(unsigned long stackPointer, char const* prefix)
	{
		uintptr_t stack[512] = {};
		SIZE_T const bytes   = ReadStack(stackPointer, stack, sizeof(stack));

		char   location[MAX_PATH + 64] = {};
		size_t found = 0;

		// The system's own libraries are left out: a fault leaves their stale frames
		// below the game's, and the frames that matter are the game's and the plugins'
		char const* const systemModules[] = { "ntdll.dll", "kernel32.dll", "kernelbase.dll", "msvcrt.dll", "ucrtbase.dll", "user32.dll", "win32u.dll" };

		for (size_t index = 0; index < bytes / sizeof(uintptr_t) && found < FAULT_STACK_CANDIDATES; index++)
		{
			if (!IsLikelyReturnAddress(stack[index]))
			{
				continue;
			}

			DescribeAddress(stack[index], location, sizeof(location));

			bool isSystem = false;
			for (char const* const name : systemModules)
			{
				char const* const at = strstr(location, " ");
				if (at != nullptr && _strnicmp(at + 1, name, strlen(name)) == 0 && at[1 + strlen(name)] == '+')
				{
					isSystem = true;
				}
			}

			if (isSystem)
			{
				continue;
			}

			LogCritical("%s  [esp+0x%zX] %s", prefix, index * sizeof(uintptr_t), location);
			found++;
		}
	}
}
