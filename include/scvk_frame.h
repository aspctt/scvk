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
 * The frame callback API, scvk's counterpart of SCD3D11's SCD3D11RegisterFrameCallback.
 *
 * Another plugin finds the two functions with GetProcAddress on scvk.dll and registers
 * one callback. It is called once a frame, after the game has drawn everything and before
 * the frame is presented, with the Vulkan objects to draw with: a command buffer being
 * recorded, outside any render pass, and the back buffer in COLOR_ATTACHMENT_OPTIMAL,
 * which is the layout it has to be left in. Everything is valid only during the call.
 *
 * Just before scvk destroys its device (a lost device being rebuilt, a new video mode, the
 * game closing) the callback is called once more with SCVK_EVENT_BEFORE_DEVICE_DESTROY and
 * no command buffer, after the device has gone idle, so it can destroy what it made with
 * that device. The generation counts the devices made; a new one means new objects.
 *
 *     typedef BOOL (__stdcall* Register)(SCVKFrameCallback, void*);
 *     Register registerCallback = (Register)GetProcAddress(GetModuleHandleA("scvk.dll"), "SCVKRegisterFrameCallback");
 */

#pragma once

#include <stdint.h>
#include <vulkan/vulkan.h>
#include <windows.h>

enum SCVKEvent : uint32_t
{
	SCVK_EVENT_RENDER                = 1,
	SCVK_EVENT_BEFORE_DEVICE_DESTROY = 2,
};

struct SCVKFrameContext
{
	uint32_t         structSize;
	uint32_t         apiVersion;        // 1
	SCVKEvent        event;
	uint32_t         deviceGeneration;

	VkInstance       instance;
	VkPhysicalDevice physicalDevice;
	VkDevice         device;
	VkQueue          queue;
	uint32_t         queueFamilyIndex;

	// Null for SCVK_EVENT_BEFORE_DEVICE_DESTROY
	VkCommandBuffer  commandBuffer;
	VkImage          backBuffer;
	VkImageView      backBufferView;
	VkFormat         backBufferFormat;
	uint32_t         width;
	uint32_t         height;

	HWND             window;
};

typedef void(__stdcall* SCVKFrameCallback)(SCVKFrameContext const* frame, void* userData);

/** Registers the one callback. Registering the same one again succeeds; another fails. */
extern "C" BOOL __stdcall SCVKRegisterFrameCallback(SCVKFrameCallback callback, void* userData);

/** Unregisters it, waiting for a call in progress on another thread to return. */
extern "C" BOOL __stdcall SCVKUnregisterFrameCallback(SCVKFrameCallback callback, void* userData);
