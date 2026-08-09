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
 * Vulkan is loaded at runtime rather than linked against vulkan-1.lib.
 *
 * A renderer plugin has to cope with being installed on a machine that has no
 * Vulkan driver at all. If scvk imported vulkan-1.dll statically, the loader
 * would fail to resolve the imports and the DLL would not load, which in this
 * context means SimCity 4 loses its renderer with no explanation. Loading by
 * name lets scvk detect the situation, write a comprehensible line to the log,
 * and decline, leaving the game to fall back to software rendering.
 *
 * The entry points are declared at global scope under their real names so that
 * call sites read like ordinary Vulkan code.
 */

#define VK_NO_PROTOTYPES
#define VK_USE_PLATFORM_WIN32_KHR
#include <vulkan/vulkan.h>

/** Resolved from vulkan-1.dll itself. Everything else comes through it. */
extern PFN_vkGetInstanceProcAddr vkGetInstanceProcAddr;

/** Resolvable before an instance exists. */
#define SCVK_VK_GLOBAL_FUNCTIONS(X)           \
	X(vkCreateInstance)                       \
	X(vkEnumerateInstanceLayerProperties)     \
	X(vkEnumerateInstanceExtensionProperties)

/** Need a VkInstance. */
#define SCVK_VK_INSTANCE_FUNCTIONS(X)               \
	X(vkDestroyInstance)                            \
	X(vkEnumeratePhysicalDevices)                   \
	X(vkGetPhysicalDeviceProperties)                \
	X(vkGetPhysicalDeviceQueueFamilyProperties)     \
	X(vkGetPhysicalDeviceMemoryProperties)          \
	X(vkGetPhysicalDeviceFormatProperties)          \
	X(vkGetPhysicalDeviceSurfaceSupportKHR)         \
	X(vkGetPhysicalDeviceSurfaceCapabilitiesKHR)    \
	X(vkGetPhysicalDeviceSurfaceFormatsKHR)         \
	X(vkGetPhysicalDeviceSurfacePresentModesKHR)    \
	X(vkCreateWin32SurfaceKHR)                      \
	X(vkDestroySurfaceKHR)                          \
	X(vkCreateDevice)                               \
	X(vkGetDeviceProcAddr)

/** Need a VkDevice. Fetched through vkGetDeviceProcAddr to skip loader
 *  dispatch on the calls made every frame. */
#define SCVK_VK_DEVICE_FUNCTIONS(X)      \
	X(vkDestroyDevice)                   \
	X(vkGetDeviceQueue)                  \
	X(vkDeviceWaitIdle)                  \
	X(vkQueueWaitIdle)                   \
	X(vkCreateSwapchainKHR)              \
	X(vkDestroySwapchainKHR)             \
	X(vkGetSwapchainImagesKHR)           \
	X(vkAcquireNextImageKHR)             \
	X(vkQueuePresentKHR)                 \
	X(vkCreateCommandPool)               \
	X(vkDestroyCommandPool)              \
	X(vkAllocateCommandBuffers)          \
	X(vkBeginCommandBuffer)              \
	X(vkEndCommandBuffer)                \
	X(vkResetCommandBuffer)              \
	X(vkCmdPipelineBarrier)              \
	X(vkCmdClearColorImage)              \
	X(vkCmdClearDepthStencilImage)       \
	X(vkCmdCopyBufferToImage)            \
	X(vkCmdCopyImage)                    \
	X(vkFreeDescriptorSets)              \
	X(vkCmdCopyImageToBuffer)            \
	X(vkCreateRenderPass)                \
	X(vkDestroyRenderPass)               \
	X(vkCreateImageView)                 \
	X(vkDestroyImageView)                \
	X(vkCreateFramebuffer)               \
	X(vkDestroyFramebuffer)              \
	X(vkCreateShaderModule)              \
	X(vkDestroyShaderModule)             \
	X(vkCreatePipelineLayout)            \
	X(vkDestroyPipelineLayout)           \
	X(vkCreateGraphicsPipelines)         \
	X(vkDestroyPipeline)                 \
	X(vkCmdBeginRenderPass)              \
	X(vkCmdEndRenderPass)                \
	X(vkCmdSetViewport)                  \
	X(vkCmdSetScissor)                   \
	X(vkCmdBindPipeline)                 \
	X(vkCmdBindVertexBuffers)            \
	X(vkCmdBindIndexBuffer)              \
	X(vkCmdPushConstants)                \
	X(vkCmdDraw)                         \
	X(vkCmdDrawIndexed)                  \
	X(vkCreateImage)                     \
	X(vkDestroyImage)                    \
	X(vkGetImageMemoryRequirements)      \
	X(vkBindImageMemory)                 \
	X(vkCreateSampler)                   \
	X(vkDestroySampler)                  \
	X(vkCreateDescriptorSetLayout)       \
	X(vkDestroyDescriptorSetLayout)      \
	X(vkCreateDescriptorPool)            \
	X(vkDestroyDescriptorPool)           \
	X(vkAllocateDescriptorSets)          \
	X(vkUpdateDescriptorSets)            \
	X(vkCmdBindDescriptorSets)           \
	X(vkQueueSubmit)                     \
	X(vkCreateSemaphore)                 \
	X(vkDestroySemaphore)                \
	X(vkCreateFence)                     \
	X(vkDestroyFence)                    \
	X(vkWaitForFences)                   \
	X(vkResetFences)                     \
	X(vkCreateBuffer)                    \
	X(vkDestroyBuffer)                   \
	X(vkGetBufferMemoryRequirements)     \
	X(vkAllocateMemory)                  \
	X(vkFreeMemory)                      \
	X(vkBindBufferMemory)                \
	X(vkMapMemory)                       \
	X(vkUnmapMemory)

#define SCVK_VK_DECLARE(name) extern PFN_##name name;
SCVK_VK_GLOBAL_FUNCTIONS(SCVK_VK_DECLARE)
SCVK_VK_INSTANCE_FUNCTIONS(SCVK_VK_DECLARE)
SCVK_VK_DEVICE_FUNCTIONS(SCVK_VK_DECLARE)
#undef SCVK_VK_DECLARE

/*
 * Optional, and kept out of the required lists above.
 *
 * These only exist when VK_EXT_debug_utils is enabled, which happens in Debug
 * builds when the layer is installed. A null pointer here is normal, not a
 * failure, so nothing may treat it as one.
 */
extern PFN_vkCreateDebugUtilsMessengerEXT  vkCreateDebugUtilsMessengerEXT;
extern PFN_vkDestroyDebugUtilsMessengerEXT vkDestroyDebugUtilsMessengerEXT;

namespace scvk
{
	/** Loads vulkan-1.dll and the entry points usable without an instance. */
	bool LoadVulkanLoader(void);

	bool LoadVulkanInstanceFunctions(VkInstance instance);
	bool LoadVulkanDeviceFunctions(VkDevice device);

	void UnloadVulkan(void);

	/** Human-readable VkResult, for the log. */
	char const* VkResultName(VkResult result);
}
