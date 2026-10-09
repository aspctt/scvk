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

/*
 * The frame callback scvk.dll exports, for other plugins that draw over the game.
 *
 * This header is the whole contract and depends on nothing in scvk, so a plugin copies it
 * as it is. It follows scd3d11's frame callback, with Vulkan handles in place of the
 * Direct3D 11 ones, so a plugin can support both with one code path.
 *
 * scvk.dll has no import library. Find it with GetModuleHandleW(L"scvk.dll") once the
 * game has picked its renderer, and look the two functions up by name with
 * GetProcAddress. scvk reports the OpenGL driver class ID (0xC4554841) from
 * cIGZGDriver::GetGZCLSID, whichever slot the game took it from.
 *
 * The callback runs on the game's main thread, once for every presented frame and once
 * before a device is destroyed. Everything in the context is borrowed: none of it is the
 * plugin's to destroy, and none of it should be kept past the call except to compare
 * deviceGeneration later.
 */

//// Dependencies

#include <stdint.h>
#include <vulkan/vulkan.h>
#include <windows.h>

//// Constants

/** The context layout described here. A later layout only adds fields at the end. */
#define SCVK_FRAME_API_VERSION 1u

/**
 * Why the callback runs.
 *
 * SCVK_EVENT_RENDER: the game has finished drawing a frame and it is about to be
 * presented. commandBuffer is recording inside renderPass, begun on the frame's image
 * over the whole of it. Record into it there: do not end the render pass, begin another
 * or end the command buffer, and set the viewport, scissor and every other bit of state
 * a draw uses, since scvk's are still bound. What is drawn shows in this frame, and in
 * any screenshot taken of it.
 *
 * SCVK_EVENT_BEFORE_DEVICE_DESTROY: the device is idle and about to be destroyed, so this
 * is the time to destroy everything made with it. commandBuffer is null.
 */
#define SCVK_EVENT_RENDER                 1u
#define SCVK_EVENT_BEFORE_DEVICE_DESTROY  2u

//// Types

typedef struct SCVKFrameContext
{
	/** sizeof(SCVKFrameContext) as scvk was built, and SCVK_FRAME_API_VERSION. */
	uint32_t structSize;
	uint32_t apiVersion;

	/** SCVK_EVENT_RENDER or SCVK_EVENT_BEFORE_DEVICE_DESTROY. */
	uint32_t event;

	/**
	 * Changes whenever scvk creates a device, as it does for every video mode the game
	 * sets. Anything made with an older generation's device is gone.
	 */
	uint32_t deviceGeneration;

	/**
	 * The loader scvk uses, and the Vulkan version its instance was created for. Device
	 * functions come from vkGetDeviceProcAddr, looked up through this. Count on the device
	 * having VK_KHR_swapchain and no optional features.
	 */
	PFN_vkGetInstanceProcAddr getInstanceProcAddr;
	uint32_t                  vulkanApiVersion;

	VkInstance       instance;
	VkPhysicalDevice physicalDevice;
	VkDevice         device;

	/**
	 * The queue scvk submits to, which also presents. Submitting to it is safe during a
	 * callback, and nowhere else.
	 */
	uint32_t queueFamilyIndex;
	VkQueue  queue;

	/**
	 * The command buffer and the render pass it is inside, for SCVK_EVENT_RENDER. The
	 * pass has one subpass with a colour attachment in format and a depth attachment,
	 * both single sampled, and it stays the same for a device generation, so pipelines
	 * built for it keep working until deviceGeneration changes.
	 */
	VkCommandBuffer commandBuffer;
	VkRenderPass    renderPass;

	/**
	 * The swapchain image being drawn and its size. imageCount is how many images the
	 * swapchain has; scvk keeps one frame in flight.
	 */
	VkImage     image;
	VkImageView imageView;
	VkFormat    format;
	VkExtent2D  extent;
	uint32_t    imageCount;

	/** The game's window. */
	HWND window;
} SCVKFrameContext;

typedef void (__stdcall* SCVKFrameCallback)(SCVKFrameContext const* frame, void* userData);

/**
 * Registers the callback, which receives userData back on every call. One callback can be
 * registered at a time. Registering the same pair again succeeds, and anything else fails
 * while another one is registered.
 */
typedef BOOL (__stdcall* PFN_SCVKRegisterFrameCallback)(SCVKFrameCallback callback, void* userData);

/**
 * Removes a registered callback, given the same pair. Called outside the callback, it
 * returns once no call to it is still running, so the plugin can then unload. A callback
 * may also unregister itself.
 */
typedef BOOL (__stdcall* PFN_SCVKUnregisterFrameCallback)(SCVKFrameCallback callback, void* userData);
