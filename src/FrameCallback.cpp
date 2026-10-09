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

#include "FrameCallback.h"
#include "Logger.h"

#include <atomic>
#include <condition_variable>
#include <mutex>

namespace scvk
{
	//// State

	namespace
	{
		// The registered callback, and how many calls to it are running
		//
		// One at a time, as scd3d11 allows, since the plugins that draw over the game share
		// one overlay between them. Calls only come from the game's main thread, but a
		// plugin may unregister from another, so the registration is guarded and
		// unregistering waits for a running call to return before the plugin can unload.
		std::mutex              callbackMutex;
		std::condition_variable callbackIdle;
		SCVKFrameCallback       registeredCallback = nullptr;
		void*                   registeredUserData = nullptr;
		uint32_t                runningCalls       = 0;

		// Whether this thread is inside a call. Unregistering from there cannot wait for
		// the call to return, since it is that call.
		thread_local bool isInsideCallback = false;

		std::atomic<uint32_t> lastDeviceGeneration{ 0 };
	}

	//// Private Functions

	namespace
	{
		bool HasNoRunningCalls(void)
		{
			return runningCalls == 0;
		}
	}

	//// Public API

	uint32_t NextDeviceGeneration(void)
	{
		return lastDeviceGeneration.fetch_add(1, std::memory_order_relaxed) + 1;
	}

	bool IsFrameCallbackRegistered(void)
	{
		std::lock_guard<std::mutex> const lock(callbackMutex);
		return registeredCallback != nullptr;
	}

	bool InvokeFrameCallback(SCVKFrameContext const& frame)
	{
		// Take the registration, and count the call so unregistering waits for it
		SCVKFrameCallback callback = nullptr;
		void*             userData = nullptr;

		{
			std::lock_guard<std::mutex> const lock(callbackMutex);
			if (registeredCallback == nullptr)
			{
				return false;
			}

			callback = registeredCallback;
			userData = registeredUserData;
			runningCalls++;
		}

		// Call it
		isInsideCallback = true;
		callback(&frame, userData);
		isInsideCallback = false;

		// Let a waiting unregister finish
		{
			std::lock_guard<std::mutex> const lock(callbackMutex);
			runningCalls--;
		}

		callbackIdle.notify_all();
		return true;
	}
}

//// Exports

// Export the two functions under their plain names
//
// A __stdcall function's own symbol carries a leading underscore and its argument size,
// and that is the name GetProcAddress would need without this.
#pragma comment(linker, "/EXPORT:SCVKRegisterFrameCallback=_SCVKRegisterFrameCallback@8")
#pragma comment(linker, "/EXPORT:SCVKUnregisterFrameCallback=_SCVKUnregisterFrameCallback@8")

extern "C" BOOL __stdcall SCVKRegisterFrameCallback(SCVKFrameCallback callback, void* userData)
{
	if (callback == nullptr)
	{
		return FALSE;
	}

	std::lock_guard<std::mutex> const lock(scvk::callbackMutex);

	// Accept the same pair again, and refuse another while one is registered
	if (scvk::registeredCallback != nullptr)
	{
		bool const isSame = scvk::registeredCallback == callback && scvk::registeredUserData == userData;
		if (!isSame)
		{
			scvk::LogWarn("A plugin's frame callback was refused, since another plugin's is registered.");
		}

		return isSame ? TRUE : FALSE;
	}

	// Register it
	scvk::registeredCallback = callback;
	scvk::registeredUserData = userData;
	scvk::LogInfo("A plugin registered a frame callback.");
	return TRUE;
}

extern "C" BOOL __stdcall SCVKUnregisterFrameCallback(SCVKFrameCallback callback, void* userData)
{
	std::unique_lock<std::mutex> lock(scvk::callbackMutex);
	if (callback == nullptr || callback != scvk::registeredCallback || userData != scvk::registeredUserData)
	{
		return FALSE;
	}

	// Remove it
	scvk::registeredCallback = nullptr;
	scvk::registeredUserData = nullptr;
	scvk::LogInfo("A plugin unregistered its frame callback.");

	// Wait for a running call to return, unless this is it
	if (!scvk::isInsideCallback)
	{
		scvk::callbackIdle.wait(lock, &scvk::HasNoRunningCalls);
	}

	return TRUE;
}
