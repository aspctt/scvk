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
 * The instance, the device, the swapchain and the frame: everything that exists once per
 * window rather than once per draw, plus the captures, which read the frame back.
 */

//// Dependencies

#include "VulkanBackend.h"
#include "Logger.h"
#include "version.h"

#include <Windows.h>
#include <algorithm>
#include <stdio.h>
#include <string.h>

namespace scvk
{
	//// Constants

	namespace
	{
		// 16 MB. One full-screen 1920x1080 BGRA image is about 8 MB and the startup
		// screen is under 2 MB, so this covers several uploads in a single frame without
		// ever reallocating mid-flight.
		constexpr VkDeviceSize STAGING_BUFFER_SIZE = 16u * 1024u * 1024u;

		constexpr char const* VALIDATION_LAYER = "VK_LAYER_KHRONOS_validation";

		// Switches on synchronisation validation, which is slow.
		constexpr char const* SYNC_VALIDATION_MARKER = "scvk-validate-sync";

		// Both waits of a frame are bounded rather than UINT64_MAX. See EnsureFrame.
		constexpr uint64_t WAIT_TIMEOUT_NANOSECONDS = 1000ull * 1000ull * 1000ull;

		// A heartbeat line this many frames apart.
		constexpr uint64_t HEARTBEAT_FRAMES = 300;

		// The smallest limits every Vulkan implementation has to accept: a viewport at
		// least 4096 wide and high, and bounds of at least -8192 to 8191.
		constexpr int32_t MAXIMUM_VIEWPORT_DIMENSION = 4096;
		constexpr int32_t VIEWPORT_BOUNDS_MINIMUM    = -8192;
		constexpr int32_t VIEWPORT_BOUNDS_MAXIMUM    = 8191;

		// The same validation hazard repeats every frame once synchronisation validation
		// is on, so each message ID is written a few times and then only counted.
		constexpr int DEBUG_MESSAGE_IDS     = 64;
		constexpr uint32_t DEBUG_MESSAGE_REPEATS = 5;
	}

	//// State

	namespace
	{
		int32_t  debugMessageIds[DEBUG_MESSAGE_IDS]    = {};
		uint32_t debugMessageCounts[DEBUG_MESSAGE_IDS] = {};
		int      debugMessageIdCount                   = 0;
	}

	//// Private Functions

	namespace
	{
		bool HasValidationLayer(void)
		{
			// List the installed layers
			uint32_t count = 0;
			if (vkEnumerateInstanceLayerProperties(&count, nullptr) != VK_SUCCESS || count == 0)
			{
				return false;
			}

			std::vector<VkLayerProperties> layers(count);
			if (vkEnumerateInstanceLayerProperties(&count, layers.data()) != VK_SUCCESS)
			{
				return false;
			}

			// Look for the validation layer among them
			for (VkLayerProperties const& layer : layers)
			{
				if (strcmp(layer.layerName, VALIDATION_LAYER) == 0)
				{
					return true;
				}
			}

			return false;
		}

		/** Whether the loader, or the named layer when one is given, offers an extension. */
		bool HasInstanceExtension(char const* wanted, char const* layer = nullptr)
		{
			// List the extensions
			uint32_t count = 0;
			if (vkEnumerateInstanceExtensionProperties(layer, &count, nullptr) != VK_SUCCESS || count == 0)
			{
				return false;
			}

			std::vector<VkExtensionProperties> extensions(count);
			if (vkEnumerateInstanceExtensionProperties(layer, &count, extensions.data()) != VK_SUCCESS)
			{
				return false;
			}

			// Look for the wanted one among them
			for (VkExtensionProperties const& extension : extensions)
			{
				if (strcmp(extension.extensionName, wanted) == 0)
				{
					return true;
				}
			}

			return false;
		}

		/**
		 * Routes validation output into scvk.log.
		 *
		 * Without this the layers write to stdout and the debugger, neither of which is
		 * visible when the driver is running inside SimCity 4. The log is the only
		 * channel that survives, so validation is close to useless in the real
		 * environment unless it ends up there.
		 */
		VKAPI_ATTR VkBool32 VKAPI_CALL OnDebugMessage(VkDebugUtilsMessageSeverityFlagBitsEXT severity, [[maybe_unused]] VkDebugUtilsMessageTypeFlagsEXT types, VkDebugUtilsMessengerCallbackDataEXT const* data, [[maybe_unused]] void* userData)
		{
			// Find or add the message's slot
			int32_t const id = (data != nullptr) ? data->messageIdNumber : 0;
			int slot = -1;

			for (int i = 0; i < debugMessageIdCount; i++)
			{
				if (debugMessageIds[i] == id)
				{
					slot = i;
					break;
				}
			}

			if (slot < 0 && debugMessageIdCount < DEBUG_MESSAGE_IDS)
			{
				slot = debugMessageIdCount++;
				debugMessageIds[slot]    = id;
				debugMessageCounts[slot] = 0;
			}

			// Count a message that has repeated enough, reporting at powers of two
			if (slot >= 0)
			{
				debugMessageCounts[slot]++;
				uint32_t const seen = debugMessageCounts[slot];

				if (seen > DEBUG_MESSAGE_REPEATS)
				{
					if ((seen & (seen - 1)) == 0)
					{
						LogNote("Vulkan: message %s has now been reported %u times.", (data != nullptr && data->pMessageIdName != nullptr) ? data->pMessageIdName : "?", seen);
					}

					return VK_FALSE;
				}
			}

			// Write the message
			char const* level = "info";
			if ((severity & VK_DEBUG_UTILS_MESSAGE_SEVERITY_ERROR_BIT_EXT) != 0)
			{
				level = "ERROR";
			}
			else if ((severity & VK_DEBUG_UTILS_MESSAGE_SEVERITY_WARNING_BIT_EXT) != 0)
			{
				level = "warning";
			}

			LogNote("Vulkan %s: %s", level, (data != nullptr && data->pMessage != nullptr) ? data->pMessage : "(no message)");

			// False means "do not abort the offending call", which is what the spec
			// requires here.
			return VK_FALSE;
		}

		/** Width, height, then four bytes a texel, tightly packed. */
		bool WriteRaw(char const* path, void const* texels, uint32_t width, uint32_t height)
		{
			FILE* file = nullptr;
			if (fopen_s(&file, path, "wb") != 0 || file == nullptr)
			{
				return false;
			}

			fwrite(&width, 4, 1, file);
			fwrite(&height, 4, 1, file);
			fwrite(texels, 4, size_t{ width } * height, file);
			fclose(file);
			return true;
		}

		/** Writes one little-endian field of a BMP header. */
		void WriteField(FILE* file, uint16_t value)
		{
			fwrite(&value, sizeof(value), 1, file);
		}

		void WriteField(FILE* file, uint32_t value)
		{
			fwrite(&value, sizeof(value), 1, file);
		}

		void WriteField(FILE* file, int32_t value)
		{
			fwrite(&value, sizeof(value), 1, file);
		}
	}

	void VulkanBackend::Fail(char const* what, VkResult result)
	{
		if (isDead)
		{
			return;
		}

		LogNote("Vulkan: %s failed with %s. Falling back to doing nothing; the game will keep running but nothing will be drawn.", what, VkResultName(result));
		isDead = true;
	}

	bool VulkanBackend::PickPhysicalDevice(void)
	{
		// List the devices
		uint32_t count = 0;
		VkResult result = vkEnumeratePhysicalDevices(instance, &count, nullptr);
		if (result != VK_SUCCESS || count == 0)
		{
			LogNote("Vulkan: no physical devices reported.");
			isDead = true;
			return false;
		}

		std::vector<VkPhysicalDevice> devices(count);
		result = vkEnumeratePhysicalDevices(instance, &count, devices.data());
		if (result != VK_SUCCESS)
		{
			Fail("vkEnumeratePhysicalDevices", result);
			return false;
		}

		// Prefer a discrete GPU
		//
		// This machine class often has an integrated adapter listed first, and picking it
		// would work but would be a poor default for a game.
		VkPhysicalDevice best = VK_NULL_HANDLE;
		int bestScore = -1;

		for (VkPhysicalDevice candidate : devices)
		{
			VkPhysicalDeviceProperties properties{};
			vkGetPhysicalDeviceProperties(candidate, &properties);

			int score = 0;
			if (properties.deviceType == VK_PHYSICAL_DEVICE_TYPE_DISCRETE_GPU)
			{
				score = 3;
			}
			else if (properties.deviceType == VK_PHYSICAL_DEVICE_TYPE_INTEGRATED_GPU)
			{
				score = 2;
			}
			else if (properties.deviceType == VK_PHYSICAL_DEVICE_TYPE_VIRTUAL_GPU)
			{
				score = 1;
			}

			LogNote("Vulkan: found device \"%s\" (type %d, API %u.%u.%u)", properties.deviceName, properties.deviceType, VK_VERSION_MAJOR(properties.apiVersion), VK_VERSION_MINOR(properties.apiVersion), VK_VERSION_PATCH(properties.apiVersion));

			if (score > bestScore)
			{
				bestScore = score;
				best = candidate;
			}
		}

		// Record what was chosen
		physicalDevice = best;

		VkPhysicalDeviceProperties properties{};
		vkGetPhysicalDeviceProperties(physicalDevice, &properties);
		deviceName = properties.deviceName;

		char version[32];
		sprintf_s(version, sizeof(version), "%u.%u.%u", VK_VERSION_MAJOR(properties.apiVersion), VK_VERSION_MINOR(properties.apiVersion), VK_VERSION_PATCH(properties.apiVersion));
		apiVersion = version;

		LogNote("Vulkan: selected \"%s\".", deviceName.c_str());
		return true;
	}

	bool VulkanBackend::CreateLogicalDevice(void)
	{
		// Find one queue family that can both draw and present
		//
		// Every real GPU has such a family, and requiring it avoids all cross-queue
		// ownership transfer handling for no practical loss.
		uint32_t familyCount = 0;
		vkGetPhysicalDeviceQueueFamilyProperties(physicalDevice, &familyCount, nullptr);

		std::vector<VkQueueFamilyProperties> families(familyCount);
		vkGetPhysicalDeviceQueueFamilyProperties(physicalDevice, &familyCount, families.data());

		queueFamily = UINT32_MAX;
		for (uint32_t i = 0; i < familyCount; i++)
		{
			if ((families[i].queueFlags & VK_QUEUE_GRAPHICS_BIT) == 0)
			{
				continue;
			}

			VkBool32 canPresent = VK_FALSE;
			vkGetPhysicalDeviceSurfaceSupportKHR(physicalDevice, i, surface, &canPresent);
			if (canPresent == VK_TRUE)
			{
				queueFamily = i;
				break;
			}
		}

		if (queueFamily == UINT32_MAX)
		{
			LogNote("Vulkan: no queue family supports both graphics and presenting to this window.");
			isDead = true;
			return false;
		}

		// Create the device with the swapchain extension
		float priority = 1.0f;
		VkDeviceQueueCreateInfo queueInformation{ VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO };
		queueInformation.queueFamilyIndex = queueFamily;
		queueInformation.queueCount       = 1;
		queueInformation.pQueuePriorities = &priority;

		char const* extensions[] = { VK_KHR_SWAPCHAIN_EXTENSION_NAME };

		VkDeviceCreateInfo deviceInformation{ VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO };
		deviceInformation.queueCreateInfoCount    = 1;
		deviceInformation.pQueueCreateInfos       = &queueInformation;
		deviceInformation.enabledExtensionCount   = _countof(extensions);
		deviceInformation.ppEnabledExtensionNames = extensions;
		deviceInformation.pEnabledFeatures        = nullptr;

		VkResult const result = vkCreateDevice(physicalDevice, &deviceInformation, nullptr, &device);
		if (result != VK_SUCCESS)
		{
			Fail("vkCreateDevice", result);
			return false;
		}

		// Load its entry points and fetch the queue
		if (!LoadVulkanDeviceFunctions(device))
		{
			isDead = true;
			return false;
		}

		vkGetDeviceQueue(device, queueFamily, 0, &queue);
		return true;
	}

	bool VulkanBackend::CreateFrameResources(void)
	{
		// Create the command pool and the frame's command buffer
		VkCommandPoolCreateInfo poolInformation{ VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO };
		poolInformation.flags            = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
		poolInformation.queueFamilyIndex = queueFamily;

		VkResult result = vkCreateCommandPool(device, &poolInformation, nullptr, &commandPool);
		if (result != VK_SUCCESS)
		{
			Fail("vkCreateCommandPool", result);
			return false;
		}

		VkCommandBufferAllocateInfo allocationInformation{ VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO };
		allocationInformation.commandPool        = commandPool;
		allocationInformation.level              = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
		allocationInformation.commandBufferCount = 1;

		result = vkAllocateCommandBuffers(device, &allocationInformation, &commandBuffer);
		if (result != VK_SUCCESS)
		{
			Fail("vkAllocateCommandBuffers", result);
			return false;
		}

		// Create the semaphores that order acquire, submit and present
		VkSemaphoreCreateInfo semaphoreInformation{ VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO };
		if (vkCreateSemaphore(device, &semaphoreInformation, nullptr, &imageAvailableSemaphore) != VK_SUCCESS || vkCreateSemaphore(device, &semaphoreInformation, nullptr, &renderFinishedSemaphore) != VK_SUCCESS)
		{
			Fail("vkCreateSemaphore", VK_ERROR_INITIALIZATION_FAILED);
			return false;
		}

		// Create the frame fence signalled
		//
		// So the first frame does not wait on a submit that never happened.
		VkFenceCreateInfo fenceInformation{ VK_STRUCTURE_TYPE_FENCE_CREATE_INFO };
		fenceInformation.flags = VK_FENCE_CREATE_SIGNALED_BIT;

		result = vkCreateFence(device, &fenceInformation, nullptr, &frameFence);
		if (result != VK_SUCCESS)
		{
			Fail("vkCreateFence", result);
			return false;
		}

		return true;
	}

	bool VulkanBackend::CreateStagingBuffer(VkDeviceSize size)
	{
		// Create the buffer
		VkBufferCreateInfo bufferInformation{ VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO };
		bufferInformation.size        = size;
		bufferInformation.usage       = VK_BUFFER_USAGE_TRANSFER_SRC_BIT;
		bufferInformation.sharingMode = VK_SHARING_MODE_EXCLUSIVE;

		VkResult result = vkCreateBuffer(device, &bufferInformation, nullptr, &stagingBuffer);
		if (result != VK_SUCCESS)
		{
			Fail("vkCreateBuffer", result);
			return false;
		}

		// Back it with host-visible coherent memory
		VkMemoryRequirements requirements{};
		vkGetBufferMemoryRequirements(device, stagingBuffer, &requirements);

		uint32_t typeIndex = 0;
		if (!FindMemoryType(requirements.memoryTypeBits, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT, typeIndex))
		{
			LogNote("Vulkan: no host-visible coherent memory type available for staging.");
			isDead = true;
			return false;
		}

		VkMemoryAllocateInfo allocationInformation{ VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO };
		allocationInformation.allocationSize  = requirements.size;
		allocationInformation.memoryTypeIndex = typeIndex;

		result = vkAllocateMemory(device, &allocationInformation, nullptr, &stagingMemory);
		if (result != VK_SUCCESS)
		{
			Fail("vkAllocateMemory", result);
			return false;
		}

		result = vkBindBufferMemory(device, stagingBuffer, stagingMemory, 0);
		if (result != VK_SUCCESS)
		{
			Fail("vkBindBufferMemory", result);
			return false;
		}

		// Map it once and leave it mapped
		//
		// Coherent memory needs no flushing, and mapping per upload would be pure
		// overhead on a path that runs every frame.
		result = vkMapMemory(device, stagingMemory, 0, requirements.size, 0, &stagingMapped);
		if (result != VK_SUCCESS)
		{
			Fail("vkMapMemory", result);
			return false;
		}

		stagingSize = requirements.size;
		stagingUsed = 0;
		return true;
	}

	bool VulkanBackend::CreateSwapchain(uint32_t width, uint32_t height)
	{
		// Check the surface allows transfers into its images
		//
		// TRANSFER_DST is what the clears and pixel copies need. COLOR_ATTACHMENT is what
		// the draws need, and overlay layers (Steam, GPU vendor overlays, and in testing
		// the Rockstar Social Club layer) wrap the swapchain in their own render pass and
		// need it too. The spec guarantees COLOR_ATTACHMENT is in supportedUsageFlags for
		// any surface, so only TRANSFER_DST is worth testing.
		VkSurfaceCapabilitiesKHR capabilities{};
		VkResult result = vkGetPhysicalDeviceSurfaceCapabilitiesKHR(physicalDevice, surface, &capabilities);
		if (result != VK_SUCCESS)
		{
			Fail("vkGetPhysicalDeviceSurfaceCapabilitiesKHR", result);
			return false;
		}

		if ((capabilities.supportedUsageFlags & VK_IMAGE_USAGE_TRANSFER_DST_BIT) == 0)
		{
			LogNote("Vulkan: the surface does not allow swapchain images to be transfer destinations.");
			isDead = true;
			return false;
		}

		// Choose B8G8R8A8_UNORM
		//
		// The game hands us BGRA8 pixels, so matching it means the blits are a straight
		// copy with no conversion and no shader.
		uint32_t formatCount = 0;
		vkGetPhysicalDeviceSurfaceFormatsKHR(physicalDevice, surface, &formatCount, nullptr);
		if (formatCount == 0)
		{
			LogNote("Vulkan: the surface reports no formats.");
			isDead = true;
			return false;
		}

		std::vector<VkSurfaceFormatKHR> formats(formatCount);
		vkGetPhysicalDeviceSurfaceFormatsKHR(physicalDevice, surface, &formatCount, formats.data());

		VkSurfaceFormatKHR chosen = formats[0];
		for (VkSurfaceFormatKHR const& candidate : formats)
		{
			if (candidate.format == VK_FORMAT_B8G8R8A8_UNORM && candidate.colorSpace == VK_COLOR_SPACE_SRGB_NONLINEAR_KHR)
			{
				chosen = candidate;
				break;
			}
		}

		if (chosen.format != VK_FORMAT_B8G8R8A8_UNORM)
		{
			LogNote("Vulkan: B8G8R8A8_UNORM is unavailable; using format %d instead. Blits will have the wrong channel order until a conversion step exists.", chosen.format);
		}

		// Size it to the surface, or to the window when the surface leaves it open
		VkExtent2D extent = capabilities.currentExtent;
		if (extent.width == UINT32_MAX)
		{
			extent.width  = width;
			extent.height = height;
		}

		if (extent.width == 0 || extent.height == 0)
		{
			LogNote("Vulkan: the window has no area yet; deferring swapchain creation.");
			return false;
		}

		uint32_t imageCount = capabilities.minImageCount + 1;
		if (capabilities.maxImageCount > 0 && imageCount > capabilities.maxImageCount)
		{
			imageCount = capabilities.maxImageCount;
		}

		// Create it
		//
		// FIFO is the only mode the spec guarantees, and it is vsync, which is what this
		// game expects anyway.
		VkSwapchainCreateInfoKHR information{ VK_STRUCTURE_TYPE_SWAPCHAIN_CREATE_INFO_KHR };
		information.surface          = surface;
		information.minImageCount    = imageCount;
		information.imageFormat      = chosen.format;
		information.imageColorSpace  = chosen.colorSpace;
		information.imageExtent      = extent;
		information.imageArrayLayers = 1;
		information.imageUsage       = VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT;
		information.imageSharingMode = VK_SHARING_MODE_EXCLUSIVE;
		information.preTransform     = capabilities.currentTransform;
		information.compositeAlpha   = VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR;
		information.presentMode      = VK_PRESENT_MODE_FIFO_KHR;
		information.clipped          = VK_TRUE;
		information.oldSwapchain     = VK_NULL_HANDLE;

		result = vkCreateSwapchainKHR(device, &information, nullptr, &swapchain);
		if (result != VK_SUCCESS)
		{
			Fail("vkCreateSwapchainKHR", result);
			return false;
		}

		swapchainFormat = chosen.format;
		swapchainExtent = extent;

		// Fetch its images
		uint32_t actualCount = 0;
		vkGetSwapchainImagesKHR(device, swapchain, &actualCount, nullptr);
		swapchainImages.resize(actualCount);
		vkGetSwapchainImagesKHR(device, swapchain, &actualCount, swapchainImages.data());

		LogNote("Vulkan: swapchain ready, %ux%u, %u images, format %d.", extent.width, extent.height, actualCount, chosen.format);

		// Create what depends on the format, the images and the extent
		//
		// The render pass depends on the swapchain format, and the framebuffers on the
		// images and extent, so both belong here rather than in one-time setup.
		return CreateDepthResources() && CreateRenderPass() && CreateFramebuffers();
	}

	bool VulkanBackend::CreateRenderPass(void)
	{
		if (renderPass != VK_NULL_HANDLE)
		{
			return true;
		}

		// Describe the colour attachment
		//
		// LOAD rather than CLEAR. The game issues its own Clear, which is a transfer
		// operation outside the pass, and a pass may be opened and closed several times
		// in one frame as draws and blits interleave. Clearing on load would erase
		// earlier work each time.
		VkAttachmentDescription colour{};
		colour.format         = swapchainFormat;
		colour.samples        = VK_SAMPLE_COUNT_1_BIT;
		colour.loadOp         = VK_ATTACHMENT_LOAD_OP_LOAD;
		colour.storeOp        = VK_ATTACHMENT_STORE_OP_STORE;
		colour.stencilLoadOp  = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
		colour.stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
		colour.initialLayout  = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
		colour.finalLayout    = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;

		VkAttachmentReference colourReference{};
		colourReference.attachment = 0;
		colourReference.layout     = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;

		// Describe the depth attachment
		//
		// It loads and stores like the colour attachment, because the game clears it
		// explicitly and may open and close several passes per frame.
		VkAttachmentDescription depth{};
		depth.format         = depthFormat;
		depth.samples        = VK_SAMPLE_COUNT_1_BIT;
		depth.loadOp         = VK_ATTACHMENT_LOAD_OP_LOAD;
		depth.storeOp        = VK_ATTACHMENT_STORE_OP_STORE;
		depth.stencilLoadOp  = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
		depth.stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
		depth.initialLayout  = VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL;
		depth.finalLayout    = VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL;

		VkAttachmentReference depthReference{};
		depthReference.attachment = 1;
		depthReference.layout     = VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL;

		// Create the single subpass pass
		VkSubpassDescription subpass{};
		subpass.pipelineBindPoint       = VK_PIPELINE_BIND_POINT_GRAPHICS;
		subpass.colorAttachmentCount    = 1;
		subpass.pColorAttachments       = &colourReference;
		subpass.pDepthStencilAttachment = &depthReference;

		VkAttachmentDescription attachments[] = { colour, depth };

		VkRenderPassCreateInfo information{ VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO };
		information.attachmentCount = _countof(attachments);
		information.pAttachments    = attachments;
		information.subpassCount    = 1;
		information.pSubpasses      = &subpass;

		VkResult const result = vkCreateRenderPass(device, &information, nullptr, &renderPass);
		if (result != VK_SUCCESS)
		{
			Fail("vkCreateRenderPass", result);
			return false;
		}

		return true;
	}

	bool VulkanBackend::CreateFramebuffers(void)
	{
		swapchainImageViews.resize(swapchainImages.size());
		framebuffers.resize(swapchainImages.size());

		for (size_t i = 0; i < swapchainImages.size(); i++)
		{
			// Create a view of the image
			VkImageViewCreateInfo viewInformation{ VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO };
			viewInformation.image    = swapchainImages[i];
			viewInformation.viewType = VK_IMAGE_VIEW_TYPE_2D;
			viewInformation.format   = swapchainFormat;
			viewInformation.subresourceRange.aspectMask     = VK_IMAGE_ASPECT_COLOR_BIT;
			viewInformation.subresourceRange.baseMipLevel   = 0;
			viewInformation.subresourceRange.levelCount     = 1;
			viewInformation.subresourceRange.baseArrayLayer = 0;
			viewInformation.subresourceRange.layerCount     = 1;

			VkResult result = vkCreateImageView(device, &viewInformation, nullptr, &swapchainImageViews[i]);
			if (result != VK_SUCCESS)
			{
				Fail("vkCreateImageView", result);
				return false;
			}

			// Pair it with the depth view in a framebuffer
			VkImageView attachments[] = { swapchainImageViews[i], depthView };

			VkFramebufferCreateInfo framebufferInformation{ VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO };
			framebufferInformation.renderPass      = renderPass;
			framebufferInformation.attachmentCount = _countof(attachments);
			framebufferInformation.pAttachments    = attachments;
			framebufferInformation.width           = swapchainExtent.width;
			framebufferInformation.height          = swapchainExtent.height;
			framebufferInformation.layers          = 1;

			result = vkCreateFramebuffer(device, &framebufferInformation, nullptr, &framebuffers[i]);
			if (result != VK_SUCCESS)
			{
				Fail("vkCreateFramebuffer", result);
				return false;
			}
		}

		return true;
	}

	void VulkanBackend::DestroyFramebuffers(void)
	{
		for (VkFramebuffer framebuffer : framebuffers)
		{
			if (framebuffer != VK_NULL_HANDLE)
			{
				vkDestroyFramebuffer(device, framebuffer, nullptr);
			}
		}

		framebuffers.clear();

		for (VkImageView view : swapchainImageViews)
		{
			if (view != VK_NULL_HANDLE)
			{
				vkDestroyImageView(device, view, nullptr);
			}
		}

		swapchainImageViews.clear();
	}

	void VulkanBackend::DestroySwapchain(void)
	{
		if (device == VK_NULL_HANDLE)
		{
			return;
		}

		// Wait for the GPU, then destroy what is sized to the swapchain
		//
		// Regions are sized to the swapchain too, so a resize invalidates them. The game
		// reallocates on its own once its old handles stop working.
		vkDeviceWaitIdle(device);

		DestroyFramebuffers();
		DestroyDepthResources();
		DestroyAllBufferRegions();

		if (swapchain != VK_NULL_HANDLE)
		{
			vkDestroySwapchainKHR(device, swapchain, nullptr);
			swapchain = VK_NULL_HANDLE;
		}

		// Forget the frame in progress
		swapchainImages.clear();
		isFrameActive      = false;
		isRenderPassActive = false;
		currentLayout      = VK_IMAGE_LAYOUT_UNDEFINED;
	}

	void VulkanBackend::DestroyDevice(void)
	{
		if (device != VK_NULL_HANDLE)
		{
			// Destroy what renders, then the textures and their descriptors
			vkDeviceWaitIdle(device);

			DestroySwapchain();
			DestroyPipelines();
			DestroyTextures();

			if (uploadFence != VK_NULL_HANDLE) { vkDestroyFence(device, uploadFence, nullptr); uploadFence = VK_NULL_HANDLE; }

			for (SamplerEntry const& entry : samplers)
			{
				if (entry.sampler != VK_NULL_HANDLE) { vkDestroySampler(device, entry.sampler, nullptr); }
			}

			samplers.clear();

			if (samplerSetLayout != VK_NULL_HANDLE) { vkDestroyDescriptorSetLayout(device, samplerSetLayout, nullptr); samplerSetLayout = VK_NULL_HANDLE; }
			if (descriptorPool != VK_NULL_HANDLE)   { vkDestroyDescriptorPool(device, descriptorPool, nullptr); descriptorPool = VK_NULL_HANDLE; }
			if (imageSetLayout != VK_NULL_HANDLE)   { vkDestroyDescriptorSetLayout(device, imageSetLayout, nullptr); imageSetLayout = VK_NULL_HANDLE; }

			// Destroy the shaders, the pass and the layout
			for (VkShaderModule& module : vertexModules)
			{
				if (module != VK_NULL_HANDLE) { vkDestroyShaderModule(device, module, nullptr); module = VK_NULL_HANDLE; }
			}

			if (renderPass != VK_NULL_HANDLE)     { vkDestroyRenderPass(device, renderPass, nullptr); renderPass = VK_NULL_HANDLE; }
			if (pipelineLayout != VK_NULL_HANDLE) { vkDestroyPipelineLayout(device, pipelineLayout, nullptr); pipelineLayout = VK_NULL_HANDLE; }
			if (fragmentModule != VK_NULL_HANDLE) { vkDestroyShaderModule(device, fragmentModule, nullptr); fragmentModule = VK_NULL_HANDLE; }

			// Destroy the buffers
			DestroyArena(vertexArena);
			DestroyArena(indexArena);

			if (quadIndexMemory != VK_NULL_HANDLE) { vkFreeMemory(device, quadIndexMemory, nullptr); quadIndexMemory = VK_NULL_HANDLE; }
			if (quadIndexBuffer != VK_NULL_HANDLE) { vkDestroyBuffer(device, quadIndexBuffer, nullptr); quadIndexBuffer = VK_NULL_HANDLE; }

			DestroyReadbackBuffer();

			if (stagingMapped != nullptr)          { vkUnmapMemory(device, stagingMemory); stagingMapped = nullptr; }
			if (stagingMemory != VK_NULL_HANDLE)   { vkFreeMemory(device, stagingMemory, nullptr); stagingMemory = VK_NULL_HANDLE; }
			if (stagingBuffer != VK_NULL_HANDLE)   { vkDestroyBuffer(device, stagingBuffer, nullptr); stagingBuffer = VK_NULL_HANDLE; }

			// Destroy the frame's synchronisation and commands, then the device
			if (frameFence != VK_NULL_HANDLE)              { vkDestroyFence(device, frameFence, nullptr); frameFence = VK_NULL_HANDLE; }
			if (imageAvailableSemaphore != VK_NULL_HANDLE) { vkDestroySemaphore(device, imageAvailableSemaphore, nullptr); imageAvailableSemaphore = VK_NULL_HANDLE; }
			if (renderFinishedSemaphore != VK_NULL_HANDLE) { vkDestroySemaphore(device, renderFinishedSemaphore, nullptr); renderFinishedSemaphore = VK_NULL_HANDLE; }
			if (commandPool != VK_NULL_HANDLE)             { vkDestroyCommandPool(device, commandPool, nullptr); commandPool = VK_NULL_HANDLE; }

			commandBuffer       = VK_NULL_HANDLE;
			uploadCommandBuffer = VK_NULL_HANDLE;

			vkDestroyDevice(device, nullptr);
			device = VK_NULL_HANDLE;
			queue  = VK_NULL_HANDLE;
		}

		// Destroy the window's surface
		if (surface != VK_NULL_HANDLE && instance != VK_NULL_HANDLE)
		{
			vkDestroySurfaceKHR(instance, surface, nullptr);
			surface = VK_NULL_HANDLE;
		}
	}

	bool VulkanBackend::FindMemoryType(uint32_t typeBits, VkMemoryPropertyFlags properties, uint32_t& outIndex) const
	{
		VkPhysicalDeviceMemoryProperties memory{};
		vkGetPhysicalDeviceMemoryProperties(physicalDevice, &memory);

		for (uint32_t i = 0; i < memory.memoryTypeCount; i++)
		{
			bool const isTypeAllowed  = (typeBits & (1u << i)) != 0;
			bool const hasProperties  = (memory.memoryTypes[i].propertyFlags & properties) == properties;

			if (isTypeAllowed && hasProperties)
			{
				outIndex = i;
				return true;
			}
		}

		return false;
	}

	bool VulkanBackend::CreateHostBuffer(VkDeviceSize size, VkBufferUsageFlags usage, VkBuffer& outBuffer, VkDeviceMemory& outMemory, void*& outMapped)
	{
		// Create the buffer
		VkBufferCreateInfo bufferInformation{ VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO };
		bufferInformation.size        = size;
		bufferInformation.usage       = usage;
		bufferInformation.sharingMode = VK_SHARING_MODE_EXCLUSIVE;

		VkResult result = vkCreateBuffer(device, &bufferInformation, nullptr, &outBuffer);
		if (result != VK_SUCCESS)
		{
			Fail("vkCreateBuffer", result);
			return false;
		}

		// Back it with host-visible coherent memory
		VkMemoryRequirements requirements{};
		vkGetBufferMemoryRequirements(device, outBuffer, &requirements);

		uint32_t typeIndex = 0;
		if (!FindMemoryType(requirements.memoryTypeBits, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT, typeIndex))
		{
			LogNote("Vulkan: no host-visible coherent memory type available.");
			isDead = true;
			return false;
		}

		VkMemoryAllocateInfo allocationInformation{ VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO };
		allocationInformation.allocationSize  = requirements.size;
		allocationInformation.memoryTypeIndex = typeIndex;

		result = vkAllocateMemory(device, &allocationInformation, nullptr, &outMemory);
		if (result != VK_SUCCESS)
		{
			Fail("vkAllocateMemory", result);
			return false;
		}

		result = vkBindBufferMemory(device, outBuffer, outMemory, 0);
		if (result != VK_SUCCESS)
		{
			Fail("vkBindBufferMemory", result);
			return false;
		}

		// Map it for its whole life
		result = vkMapMemory(device, outMemory, 0, requirements.size, 0, &outMapped);
		if (result != VK_SUCCESS)
		{
			Fail("vkMapMemory", result);
			return false;
		}

		return true;
	}

	int32_t VulkanBackend::SwapchainWidth(void) const
	{
		// Window sizes are far below INT32_MAX, so the conversion loses nothing.
		return static_cast<int32_t>(swapchainExtent.width);
	}

	int32_t VulkanBackend::SwapchainHeight(void) const
	{
		// Window sizes are far below INT32_MAX, so the conversion loses nothing.
		return static_cast<int32_t>(swapchainExtent.height);
	}

	bool VulkanBackend::EnsureFrame(void)
	{
		if (isDead || swapchain == VK_NULL_HANDLE)
		{
			return false;
		}

		if (isFrameActive)
		{
			return true;
		}

		// Wait for the previous frame
		//
		// One frame in flight. The CPU waits for the previous submit before starting the
		// next, which costs throughput but removes every question about which resources
		// are still being read by the GPU.
		//
		// Both waits are bounded rather than UINT64_MAX. These run on the game's main
		// thread, which is also the thread that pumps its window messages, so blocking
		// here indefinitely makes the whole game unresponsive and unkillable except from
		// Task Manager. A minimised or occluded window can legitimately stall an acquire
		// under FIFO, so this is a reachable state, not a theoretical one. Dropping a
		// frame is always better than wedging the process.
		VkResult result = vkWaitForFences(device, 1, &frameFence, VK_TRUE, WAIT_TIMEOUT_NANOSECONDS);
		if (result == VK_TIMEOUT)
		{
			LogNote("Vulkan: timed out waiting for the previous frame; skipping this one.");
			return false;
		}

		// Acquire the next image
		result = vkAcquireNextImageKHR(device, swapchain, WAIT_TIMEOUT_NANOSECONDS, imageAvailableSemaphore, VK_NULL_HANDLE, &imageIndex);

		if (result == VK_TIMEOUT || result == VK_NOT_READY)
		{
			// Normal when the window is minimised or hidden. Not an error, and not worth
			// a log line every frame.
			return false;
		}

		if (result == VK_ERROR_OUT_OF_DATE_KHR)
		{
			LogNote("Vulkan: swapchain out of date; rebuilding.");
			DestroySwapchain();
			CreateSwapchain(swapchainExtent.width, swapchainExtent.height);
			return false;
		}

		if (result != VK_SUCCESS && result != VK_SUBOPTIMAL_KHR)
		{
			Fail("vkAcquireNextImageKHR", result);
			return false;
		}

		// Begin recording
		//
		// The fence is deliberately not reset here. It is reset immediately before the
		// submit that signals it, so that failing out of this function cannot leave it
		// unsignalled forever, which would make every later frame time out.
		vkResetCommandBuffer(commandBuffer, 0);

		VkCommandBufferBeginInfo beginInformation{ VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO };
		beginInformation.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;

		result = vkBeginCommandBuffer(commandBuffer, &beginInformation);
		if (result != VK_SUCCESS)
		{
			Fail("vkBeginCommandBuffer", result);
			return false;
		}

		// Release what the previous frame was using
		//
		// Safe here: the fence wait above means the previous submit is done.
		FlushRetiredTextures();

		isFrameActive = true;
		stagingUsed   = 0;
		ArenaRewind(vertexArena);
		ArenaRewind(indexArena);

		// Start from UNDEFINED
		//
		// Every pixel that matters is overwritten, and discarding the previous contents
		// is cheaper than preserving them.
		currentLayout = VK_IMAGE_LAYOUT_UNDEFINED;
		TransitionTo(VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);

		return true;
	}

	void VulkanBackend::LayoutAccess(VkImageLayout layout, VkAccessFlags& outAccess, VkPipelineStageFlags& outStages)
	{
		switch (layout)
		{
		case VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL:
			outAccess = VK_ACCESS_TRANSFER_WRITE_BIT;
			outStages = VK_PIPELINE_STAGE_TRANSFER_BIT;
			break;

		case VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL:
			outAccess = VK_ACCESS_TRANSFER_READ_BIT;
			outStages = VK_PIPELINE_STAGE_TRANSFER_BIT;
			break;

		case VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL:
			outAccess = VK_ACCESS_COLOR_ATTACHMENT_READ_BIT | VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
			outStages = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
			break;

		case VK_IMAGE_LAYOUT_PRESENT_SRC_KHR:
			outAccess = 0;
			outStages = VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT;
			break;

		case VK_IMAGE_LAYOUT_UNDEFINED:
		default:
			outAccess = 0;
			outStages = VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT;
			break;
		}
	}

	void VulkanBackend::TransitionTo(VkImageLayout newLayout)
	{
		// Staying in the transfer destination layout still needs a barrier: two transfers
		// writing the same image are not ordered against each other without one, and the
		// game copies then clears, or copies twice, into the same place.
		if (currentLayout == newLayout && newLayout != VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL)
		{
			return;
		}

		// Work out the two sides of the dependency
		VkAccessFlags        sourceAccess      = 0;
		VkAccessFlags        destinationAccess = 0;
		VkPipelineStageFlags sourceStages      = 0;
		VkPipelineStageFlags destinationStages = 0;

		LayoutAccess(currentLayout, sourceAccess, sourceStages);
		LayoutAccess(newLayout, destinationAccess, destinationStages);

		// Start the frame's first barrier where the submit waits for the image
		//
		// Otherwise its layout transition can run while the presentation engine is still
		// reading the previous frame from it.
		if (currentLayout == VK_IMAGE_LAYOUT_UNDEFINED)
		{
			sourceStages = ACQUIRE_STAGES;
		}

		// Record the barrier
		VkImageMemoryBarrier barrier{ VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER };
		barrier.srcAccessMask       = sourceAccess;
		barrier.dstAccessMask       = destinationAccess;
		barrier.oldLayout           = currentLayout;
		barrier.newLayout           = newLayout;
		barrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
		barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
		barrier.image               = swapchainImages[imageIndex];
		barrier.subresourceRange.aspectMask     = VK_IMAGE_ASPECT_COLOR_BIT;
		barrier.subresourceRange.baseMipLevel   = 0;
		barrier.subresourceRange.levelCount     = 1;
		barrier.subresourceRange.baseArrayLayer = 0;
		barrier.subresourceRange.layerCount     = 1;

		vkCmdPipelineBarrier(commandBuffer, sourceStages, destinationStages, 0, 0, nullptr, 0, nullptr, 1, &barrier);
		currentLayout = newLayout;
	}

	void VulkanBackend::BeginRenderPassIfNeeded(void)
	{
		if (isRenderPassActive)
		{
			return;
		}

		// Move the colour image into the attachment layout
		//
		// Drawing needs the colour attachment layout; clears and blits need the transfer
		// layout. The game interleaves them freely, so the transition is driven by what
		// is about to happen rather than fixed once per frame.
		TransitionTo(VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL);

		// Move a fresh depth image into its attachment layout
		//
		// A freshly created depth image is UNDEFINED, and the render pass declares its
		// attachment layout as the initial one. Moving it here covers the case where the
		// game opens a pass before it has asked for any depth clear.
		if (isDepthLayoutPending && depthImage != VK_NULL_HANDLE)
		{
			BarrierDepthImage(VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL, 0, VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_READ_BIT | VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, DEPTH_STAGES);
			isDepthLayoutPending = false;
		}

		// Begin the pass
		//
		// No clear values: the attachments load what is already there, because the game
		// clears through its own Clear call, which may or may not have happened this
		// frame.
		VkRenderPassBeginInfo beginInformation{ VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO };
		beginInformation.renderPass        = renderPass;
		beginInformation.framebuffer       = framebuffers[imageIndex];
		beginInformation.renderArea.offset = { 0, 0 };
		beginInformation.renderArea.extent = swapchainExtent;
		beginInformation.clearValueCount   = 0;

		vkCmdBeginRenderPass(commandBuffer, &beginInformation, VK_SUBPASS_CONTENTS_INLINE);
		isRenderPassActive = true;

		// Apply the viewport
		//
		// Draws apply it again, because the game changes it between draws within a
		// single pass.
		ApplyViewport();
	}

	void VulkanBackend::EndRenderPassIfActive(void)
	{
		if (!isRenderPassActive)
		{
			return;
		}

		vkCmdEndRenderPass(commandBuffer);
		isRenderPassActive = false;

		// The render pass declares this as its final layout, so it is recorded rather
		// than emitting a redundant barrier.
		currentLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
	}

	void VulkanBackend::ApplyViewport(void)
	{
		if (!isRenderPassActive)
		{
			return;
		}

		// Map the game's rectangle as given, even where it runs off the window
		//
		// OpenGL only clips what falls outside; shrinking the viewport to fit would
		// squeeze the geometry instead. The limits are the smallest range every Vulkan
		// implementation has to accept.
		VkRect2D rectangle{};
		ViewportRectangle(rectangle);

		int32_t x      = 0;
		int32_t y      = 0;
		int32_t width  = SwapchainWidth();
		int32_t height = SwapchainHeight();

		if (viewportWidth > 0 && viewportHeight > 0)
		{
			x      = viewportX;
			width  = std::min(viewportWidth, MAXIMUM_VIEWPORT_DIMENSION);
			height = std::min(viewportHeight, MAXIMUM_VIEWPORT_DIMENSION);
			y      = SwapchainHeight() - viewportY - viewportHeight;
		}

		x = std::max(VIEWPORT_BOUNDS_MINIMUM, std::min(x, VIEWPORT_BOUNDS_MAXIMUM - width));
		y = std::max(VIEWPORT_BOUNDS_MINIMUM, std::min(y, VIEWPORT_BOUNDS_MAXIMUM - height));

		// Vulkan takes the viewport in floats; pixel coordinates convert exactly.
		VkViewport viewport{};
		viewport.x        = static_cast<float>(x);
		viewport.y        = static_cast<float>(y);
		viewport.width    = static_cast<float>(width);
		viewport.height   = static_cast<float>(height);
		viewport.minDepth = 0.0f;
		viewport.maxDepth = 1.0f;
		vkCmdSetViewport(commandBuffer, 0, 1, &viewport);

		// Scissor to the viewport, clamped to the window
		//
		// The game's OpenGL driver enables its scissor test with the same rectangle for
		// every sub-viewport, and Vulkan always scissors, so matching the viewport is the
		// same thing. A rectangle wholly off the window leaves nothing to draw, which an
		// empty scissor says directly.
		if (rectangle.extent.width == 0 || rectangle.extent.height == 0)
		{
			rectangle.offset = { 0, 0 };
			rectangle.extent = { 0, 0 };
		}

		vkCmdSetScissor(commandBuffer, 0, 1, &rectangle);

		// Log what actually reached Vulkan
		//
		// As opposed to what the driver believes it asked for. The arithmetic in the
		// frame dump said these rectangles should already be producing a correct picture,
		// so a discrepancy is either here or downstream of here.
		bool const hasChanged = x != loggedViewport[0] || y != loggedViewport[1] || width != loggedViewport[2] || height != loggedViewport[3];

		if (viewportLogsRemaining > 0 && hasChanged)
		{
			viewportLogsRemaining--;
			loggedViewport[0] = x;
			loggedViewport[1] = y;
			loggedViewport[2] = width;
			loggedViewport[3] = height;

			LogNote("Vulkan: viewport %d,%d %dx%d (from game %d,%d %dx%d, swapchain %ux%u)", x, y, width, height, viewportX, viewportY, viewportWidth, viewportHeight, swapchainExtent.width, swapchainExtent.height);
		}
	}

	bool VulkanBackend::ViewportRectangle(VkRect2D& outRectangle) const
	{
		// Start from the game's rectangle, or the whole window
		int32_t x      = 0;
		int32_t y      = 0;
		int32_t width  = SwapchainWidth();
		int32_t height = SwapchainHeight();

		bool const isSubViewport = viewportWidth > 0 && viewportHeight > 0;

		if (isSubViewport)
		{
			x      = viewportX;
			width  = viewportWidth;
			height = viewportHeight;

			// OpenGL measures the viewport from the bottom of the window and Vulkan from
			// the top, so the origin has to be reflected.
			y = SwapchainHeight() - viewportY - viewportHeight;
		}

		// Clamp it to the window
		//
		// A viewport outside the framebuffer is invalid, and the game can name one while
		// the window is being resized.
		if (x < 0) { width += x; x = 0; }
		if (y < 0) { height += y; y = 0; }
		if (x + width > SwapchainWidth())   { width  = SwapchainWidth() - x; }
		if (y + height > SwapchainHeight()) { height = SwapchainHeight() - y; }
		if (width < 0)  { width = 0; }
		if (height < 0) { height = 0; }

		// Both extents were clamped to zero or more just above.
		outRectangle.offset = { x, y };
		outRectangle.extent = { static_cast<uint32_t>(width), static_cast<uint32_t>(height) };
		return isSubViewport;
	}

	bool VulkanBackend::ClipToScissor(int32_t& sourceX, int32_t& sourceY, int32_t& destinationX, int32_t& destinationY, int32_t& width, int32_t& height) const
	{
		VkRect2D scissor{};
		if (!ViewportRectangle(scissor))
		{
			return width > 0 && height > 0;
		}

		// Intersect the destination with the scissor
		//
		// The scissor test is one of only two fragment operations that apply to
		// glBlitFramebuffer, which is how the game's OpenGL driver copies regions, so
		// only the part of the destination inside it is written. The scissor's extent
		// came from signed numbers clamped to the window, so it converts back without
		// loss.
		int32_t const scissorRight  = scissor.offset.x + static_cast<int32_t>(scissor.extent.width);
		int32_t const scissorBottom = scissor.offset.y + static_cast<int32_t>(scissor.extent.height);

		int32_t const left   = std::max(destinationX, scissor.offset.x);
		int32_t const top    = std::max(destinationY, scissor.offset.y);
		int32_t const right  = std::min(destinationX + width, scissorRight);
		int32_t const bottom = std::min(destinationY + height, scissorBottom);

		if (right <= left || bottom <= top)
		{
			return false;
		}

		// Move the source by as much as the destination moved
		sourceX      += left - destinationX;
		sourceY      += top - destinationY;
		destinationX  = left;
		destinationY  = top;
		width         = right - left;
		height        = bottom - top;
		return true;
	}

	bool VulkanBackend::EnsureReadbackBuffer(VkDeviceSize size)
	{
		if (readbackSize >= size)
		{
			return true;
		}

		DestroyReadbackBuffer();

		if (!CreateHostBuffer(size, VK_BUFFER_USAGE_TRANSFER_DST_BIT, readbackBuffer, readbackMemory, readbackMapped))
		{
			return false;
		}

		readbackSize = size;
		return true;
	}

	void VulkanBackend::DestroyReadbackBuffer(void)
	{
		if (readbackMapped != nullptr)        { vkUnmapMemory(device, readbackMemory); readbackMapped = nullptr; }
		if (readbackMemory != VK_NULL_HANDLE) { vkFreeMemory(device, readbackMemory, nullptr); readbackMemory = VK_NULL_HANDLE; }
		if (readbackBuffer != VK_NULL_HANDLE) { vkDestroyBuffer(device, readbackBuffer, nullptr); readbackBuffer = VK_NULL_HANDLE; }

		readbackSize = 0;
	}

	bool VulkanBackend::RecordRegionCapture(uint32_t& outWidth, uint32_t& outHeight)
	{
		// Take the first live region of the kind asked for
		//
		// That is the one the game keeps the scene in. A region that has never been
		// written holds nothing worth reading.
		for (BufferRegion const& region : bufferRegions)
		{
			if (!region.isLive || region.isDepth != isRegionCaptureDepth || !region.hasContent)
			{
				continue;
			}

			// Four bytes a texel either way, which for depth only holds while it is
			// 32-bit float.
			if (region.isDepth && region.format != VK_FORMAT_D32_SFLOAT)
			{
				LogNote("Vulkan: the depth region is format %d, not D32_SFLOAT; not capturing it.", region.format);
				return false;
			}

			if (!EnsureReadbackBuffer(VkDeviceSize{ region.width } * region.height * 4u))
			{
				return false;
			}

			// Copy it out
			//
			// Saved regions are left in the transfer source layout, ready to be restored,
			// which is also what a read needs.
			VkBufferImageCopy copy{};
			copy.imageSubresource.aspectMask = region.isDepth ? VK_IMAGE_ASPECT_DEPTH_BIT : VK_IMAGE_ASPECT_COLOR_BIT;
			copy.imageSubresource.layerCount = 1;
			copy.imageExtent = { region.width, region.height, 1 };

			vkCmdCopyImageToBuffer(commandBuffer, region.image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, readbackBuffer, 1, &copy);

			outWidth  = region.width;
			outHeight = region.height;
			return true;
		}

		return false;
	}

	bool VulkanBackend::RecordFrameCapture(void)
	{
		if (!EnsureReadbackBuffer(VkDeviceSize{ swapchainExtent.width } * swapchainExtent.height * 4u))
		{
			return false;
		}

		TransitionTo(VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL);

		VkBufferImageCopy copy{};
		copy.bufferOffset      = 0;
		copy.bufferRowLength   = 0;
		copy.bufferImageHeight = 0;
		copy.imageSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
		copy.imageSubresource.layerCount = 1;
		copy.imageOffset = { 0, 0, 0 };
		copy.imageExtent = { swapchainExtent.width, swapchainExtent.height, 1 };

		vkCmdCopyImageToBuffer(commandBuffer, swapchainImages[imageIndex], VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, readbackBuffer, 1, &copy);
		return true;
	}

	void VulkanBackend::WriteCapture(bool isRegion, uint32_t regionWidth, uint32_t regionHeight)
	{
		// Wait for the GPU to finish the copy
		//
		// This stalls the pipeline, which is fine: captures are deliberate, rare, and the
		// alternative is reading a buffer the GPU is still writing.
		vkWaitForFences(device, 1, &frameFence, VK_TRUE, UINT64_MAX);

		// The readback memory holds bytes, whichever kind of capture it is.
		uint8_t const* const pixels = static_cast<uint8_t const*>(readbackMapped);

		// Write a region capture
		//
		// One readback buffer, so a region capture and a frame capture never share a
		// frame.
		if (isRegion)
		{
			bool const isWritten = isRegionCaptureDepth ? WriteRaw(regionCapturePath.c_str(), readbackMapped, regionWidth, regionHeight) : WriteBmp(regionCapturePath.c_str(), pixels, regionWidth, regionHeight, regionWidth * 4u);

			if (isWritten)
			{
				LogNote("Vulkan: wrote the saved region to %s (%ux%u)", regionCapturePath.c_str(), regionWidth, regionHeight);
			}
			else
			{
				LogNote("Vulkan: could not write the region capture to %s", regionCapturePath.c_str());
			}

			return;
		}

		// Write a frame capture
		if (WriteBmp(capturePath.c_str(), pixels, swapchainExtent.width, swapchainExtent.height, swapchainExtent.width * 4u))
		{
			LogNote("Vulkan: captured frame %llu to %s (%ux%u)", presentedFrames + 1, capturePath.c_str(), swapchainExtent.width, swapchainExtent.height);
		}
		else
		{
			LogNote("Vulkan: could not write the capture to %s", capturePath.c_str());
		}
	}

	void VulkanBackend::LogHeartbeat(void)
	{
		// A heartbeat, so the log answers "is it still presenting?" without needing the
		// trace. Cheap at one line per few hundred frames.
		LogNote("Vulkan: %llu frames presented.", presentedFrames);

		if (drawsBeforeUpload != 0 || uploadsAfterDraw != 0)
		{
			LogNote("Vulkan: texture hazards so far: %llu draws before an upload, %llu uploads after a draw in the same frame.", drawsBeforeUpload, uploadsAfterDraw);
		}
	}

	bool VulkanBackend::WriteBmp(char const* path, uint8_t const* pixels, uint32_t width, uint32_t height, uint32_t rowPitch)
	{
		FILE* file = nullptr;
		if (fopen_s(&file, path, "wb") != 0 || file == nullptr)
		{
			return false;
		}

		// Write the file and information headers
		//
		// Top-down, signalled by a negative height, so the rows can go straight out in
		// the order the GPU produced them. Image heights are far below INT32_MAX, so the
		// signed height loses nothing.
		uint32_t const imageBytes    = width * height * 4u;
		uint32_t const headerBytes   = 14u + 40u;
		int32_t const  topDownHeight = -static_cast<int32_t>(height);

		fwrite("BM", 1, 2, file);
		WriteField(file, headerBytes + imageBytes);
		WriteField(file, uint32_t{ 0 });
		WriteField(file, headerBytes);

		WriteField(file, uint32_t{ 40 });
		WriteField(file, width);
		WriteField(file, topDownHeight);
		WriteField(file, uint16_t{ 1 });
		WriteField(file, uint16_t{ 32 });
		WriteField(file, uint32_t{ 0 });
		WriteField(file, imageBytes);
		WriteField(file, uint32_t{ 2835 });
		WriteField(file, uint32_t{ 2835 });
		WriteField(file, uint32_t{ 0 });
		WriteField(file, uint32_t{ 0 });

		// Write the rows
		for (uint32_t y = 0; y < height; y++)
		{
			fwrite(pixels + size_t{ y } * rowPitch, 4, width, file);
		}

		fclose(file);
		return true;
	}

	//// Public API

	VulkanBackend::VulkanBackend(void) = default;

	VulkanBackend::~VulkanBackend(void)
	{
		Destroy();
	}

	bool VulkanBackend::CreateInstance(void)
	{
		if (instance != VK_NULL_HANDLE)
		{
			return !isDead;
		}

		if (!LoadVulkanLoader())
		{
			isDead = true;
			return false;
		}

		// Describe the application
		//
		// Vulkan 1.0 deliberately. Nothing here needs a later core version, and asking
		// for more would exclude older drivers for no benefit.
		VkApplicationInfo application{ VK_STRUCTURE_TYPE_APPLICATION_INFO };
		application.pApplicationName   = "SimCity 4";
		application.applicationVersion = VK_MAKE_VERSION(1, 0, 0);
		application.pEngineName        = "scvk";
		application.engineVersion      = VK_MAKE_VERSION(SCVK_VERSION_MAJOR, SCVK_VERSION_MINOR, SCVK_VERSION_PATCH);
		application.apiVersion         = VK_API_VERSION_1_0;

		// Ask for the window surface extensions, and debug messages in Debug builds
		std::vector<char const*> extensions{
			VK_KHR_SURFACE_EXTENSION_NAME,
			VK_KHR_WIN32_SURFACE_EXTENSION_NAME,
		};

		bool shouldCreateMessenger = false;
#ifndef NDEBUG
		if (HasInstanceExtension(VK_EXT_DEBUG_UTILS_EXTENSION_NAME))
		{
			extensions.push_back(VK_EXT_DEBUG_UTILS_EXTENSION_NAME);
			shouldCreateMessenger = true;
		}
#endif

		VkInstanceCreateInfo information{ VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO };
		information.pApplicationInfo        = &application;
		information.enabledExtensionCount   = extensions.size();
		information.ppEnabledExtensionNames = extensions.data();

#ifndef NDEBUG
		// Enable the validation layer when it is installed
		//
		// Only in Debug. SimCity 4 is a 32-bit process, so this needs the 32-bit
		// validation layers; a default SDK install only provides 64-bit ones and the
		// layer will be absent here.
		char const* layers[] = { VALIDATION_LAYER };

		// Synchronisation validation reports hazards between commands, such as a copy
		// reading an image before the draws that write it are done, which the default
		// checks do not. It is slow, so it waits for a marker file next to the driver.
		VkValidationFeatureEnableEXT const synchronisationFeature = VK_VALIDATION_FEATURE_ENABLE_SYNCHRONIZATION_VALIDATION_EXT;
		VkValidationFeaturesEXT features{ VK_STRUCTURE_TYPE_VALIDATION_FEATURES_EXT };
		features.enabledValidationFeatureCount = 1;
		features.pEnabledValidationFeatures    = &synchronisationFeature;

		if (HasValidationLayer())
		{
			information.enabledLayerCount   = _countof(layers);
			information.ppEnabledLayerNames = layers;
			LogNote("Vulkan: validation layers enabled.");

			if (HasMarkerFile(SYNC_VALIDATION_MARKER) && HasInstanceExtension(VK_EXT_VALIDATION_FEATURES_EXTENSION_NAME, VALIDATION_LAYER))
			{
				extensions.push_back(VK_EXT_VALIDATION_FEATURES_EXTENSION_NAME);
				information.enabledExtensionCount   = extensions.size();
				information.ppEnabledExtensionNames = extensions.data();
				information.pNext                   = &features;
				LogNote("Vulkan: synchronisation validation enabled.");
			}
		}
		else
		{
			LogNote("Vulkan: validation layers not available to this 32-bit process. Install the 32-bit components of the Vulkan SDK to get them.");
		}
#endif

		// Create the instance and load its entry points
		VkResult const result = vkCreateInstance(&information, nullptr, &instance);
		if (result != VK_SUCCESS)
		{
			Fail("vkCreateInstance", result);
			return false;
		}

		if (!LoadVulkanInstanceFunctions(instance))
		{
			isDead = true;
			return false;
		}

		// Route validation messages into the log
		if (shouldCreateMessenger && vkCreateDebugUtilsMessengerEXT != nullptr)
		{
			VkDebugUtilsMessengerCreateInfoEXT messengerInformation{ VK_STRUCTURE_TYPE_DEBUG_UTILS_MESSENGER_CREATE_INFO_EXT };
			messengerInformation.messageSeverity = VK_DEBUG_UTILS_MESSAGE_SEVERITY_WARNING_BIT_EXT | VK_DEBUG_UTILS_MESSAGE_SEVERITY_ERROR_BIT_EXT;
			messengerInformation.messageType     = VK_DEBUG_UTILS_MESSAGE_TYPE_GENERAL_BIT_EXT | VK_DEBUG_UTILS_MESSAGE_TYPE_VALIDATION_BIT_EXT | VK_DEBUG_UTILS_MESSAGE_TYPE_PERFORMANCE_BIT_EXT;
			messengerInformation.pfnUserCallback = &OnDebugMessage;

			if (vkCreateDebugUtilsMessengerEXT(instance, &messengerInformation, nullptr, &debugMessenger) == VK_SUCCESS)
			{
				LogNote("Vulkan: validation messages will be written to this log.");
			}
		}

		return PickPhysicalDevice();
	}

	bool VulkanBackend::CreateSurfaceAndDevice(void* newWindowHandle, uint32_t width, uint32_t height)
	{
		if (isDead || instance == VK_NULL_HANDLE)
		{
			return false;
		}

		// Destroy what a previous window had
		//
		// The driver destroys its old window before creating a new one, so a surface and
		// swapchain left from it would present into nothing.
		DestroyDevice();
		windowHandle = newWindowHandle;

		// Create the surface for the window
		//
		// The API types the window as an HWND, which the driver interface carries as a
		// plain pointer.
		VkWin32SurfaceCreateInfoKHR surfaceInformation{ VK_STRUCTURE_TYPE_WIN32_SURFACE_CREATE_INFO_KHR };
		surfaceInformation.hinstance = GetModuleHandleA(nullptr);
		surfaceInformation.hwnd      = static_cast<HWND>(newWindowHandle);

		VkResult const result = vkCreateWin32SurfaceKHR(instance, &surfaceInformation, nullptr, &surface);
		if (result != VK_SUCCESS)
		{
			Fail("vkCreateWin32SurfaceKHR", result);
			return false;
		}

		// Create the device and what every frame needs
		if (!CreateLogicalDevice() || !CreateFrameResources() || !CreateStagingBuffer(STAGING_BUFFER_SIZE))
		{
			return false;
		}

		// Create the draw resources
		//
		// Descriptors before the pipeline layout, which references the set layouts, and
		// the default texture needs the pool.
		if (!CreateGeometryBuffers() || !CreateShaderModules() || !CreateDescriptorResources() || !CreatePipelineLayout())
		{
			return false;
		}

		return CreateSwapchain(width, height);
	}

	void VulkanBackend::Destroy(void)
	{
		DestroyDevice();

		if (instance == VK_NULL_HANDLE)
		{
			return;
		}

		if (debugMessenger != VK_NULL_HANDLE && vkDestroyDebugUtilsMessengerEXT != nullptr)
		{
			vkDestroyDebugUtilsMessengerEXT(instance, debugMessenger, nullptr);
			debugMessenger = VK_NULL_HANDLE;
		}

		vkDestroyInstance(instance, nullptr);
		instance = VK_NULL_HANDLE;
	}

	void VulkanBackend::Clear(float red, float green, float blue, float alpha)
	{
		if (!EnsureFrame())
		{
			return;
		}

		// Remember the colour for frames that start without a clear
		lastClearColour[0] = red;
		lastClearColour[1] = green;
		lastClearColour[2] = blue;
		lastClearColour[3] = alpha;

		VkClearColorValue colour{};
		colour.float32[0] = red;
		colour.float32[1] = green;
		colour.float32[2] = blue;
		colour.float32[3] = alpha;

		// Clear only the scissor under a sub-viewport
		//
		// The game's OpenGL driver has its scissor test on there, and glClear honours it.
		// An image clear has no rectangle, so this goes through the render pass.
		VkRect2D scissor{};
		if (ViewportRectangle(scissor))
		{
			if (scissor.extent.width == 0 || scissor.extent.height == 0)
			{
				return;
			}

			BeginRenderPassIfNeeded();

			VkClearAttachment attachment{};
			attachment.aspectMask       = VK_IMAGE_ASPECT_COLOR_BIT;
			attachment.colorAttachment  = 0;
			attachment.clearValue.color = colour;

			VkClearRect clearRectangle{};
			clearRectangle.rect       = scissor;
			clearRectangle.layerCount = 1;

			vkCmdClearAttachments(commandBuffer, 1, &attachment, 1, &clearRectangle);
			return;
		}

		// Clear the whole image otherwise
		//
		// A clear is a transfer operation and cannot happen inside a render pass, so any
		// open pass has to end first.
		VkImageSubresourceRange range{};
		range.aspectMask     = VK_IMAGE_ASPECT_COLOR_BIT;
		range.baseMipLevel   = 0;
		range.levelCount     = 1;
		range.baseArrayLayer = 0;
		range.layerCount     = 1;

		EndRenderPassIfActive();
		TransitionTo(VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);

		vkCmdClearColorImage(commandBuffer, swapchainImages[imageIndex], VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, &colour, 1, &range);
	}

	void VulkanBackend::BlitPixels(int32_t destinationX, int32_t destinationY, uint32_t width, uint32_t height, uint32_t sourceWidth, void const* pixels)
	{
		if (pixels == nullptr || width == 0 || height == 0)
		{
			return;
		}

		if (!EnsureFrame())
		{
			return;
		}

		// Refuse an origin off the top or left
		//
		// That would need the source pointer advanced as well as the extent reduced. Not
		// seen from the game so far, so it is refused rather than half-implemented.
		if (destinationX < 0 || destinationY < 0)
		{
			LogNote("Vulkan: blit origin %d,%d is negative; skipping.", destinationX, destinationY);
			return;
		}

		// Both are zero or more, checked just above.
		uint32_t const left = static_cast<uint32_t>(destinationX);
		uint32_t const top  = static_cast<uint32_t>(destinationY);

		if (left >= swapchainExtent.width || top >= swapchainExtent.height)
		{
			return;
		}

		// Clip against the right and bottom edges
		//
		// Rows are still strided by the full source width, which is what bufferRowLength
		// expresses, so clipping the extent alone gives the correct result.
		uint32_t const copyWidth  = std::min(width, swapchainExtent.width - left);
		uint32_t const copyHeight = std::min(height, swapchainExtent.height - top);

		// Stage the pixels
		VkDeviceSize const bytes = VkDeviceSize{ sourceWidth } * height * 4u;

		if (stagingUsed + bytes > stagingSize)
		{
			LogNote("Vulkan: staging buffer exhausted (%llu bytes needed, %llu free); skipping a blit.", bytes, stagingSize - stagingUsed);
			return;
		}

		// The staging memory is plain bytes, and the copy fits the 16 MB buffer, so its
		// size fits a size_t.
		VkDeviceSize const offset = stagingUsed;
		memcpy(static_cast<uint8_t*>(stagingMapped) + offset, pixels, static_cast<size_t>(bytes));

		// Offsets into the buffer must satisfy the texel size alignment, which is 4 for a
		// 32-bit format. Rounding up keeps every later blit legal.
		stagingUsed = (offset + bytes + 3u) & ~VkDeviceSize{ 3 };

		// Copy them into the image
		//
		// Transfers cannot run inside a render pass, so a blit arriving after a draw has
		// to close it.
		VkBufferImageCopy copy{};
		copy.bufferOffset      = offset;
		copy.bufferRowLength   = sourceWidth;
		copy.bufferImageHeight = height;
		copy.imageSubresource.aspectMask     = VK_IMAGE_ASPECT_COLOR_BIT;
		copy.imageSubresource.mipLevel       = 0;
		copy.imageSubresource.baseArrayLayer = 0;
		copy.imageSubresource.layerCount     = 1;
		copy.imageOffset = { destinationX, destinationY, 0 };
		copy.imageExtent = { copyWidth, copyHeight, 1 };

		EndRenderPassIfActive();
		TransitionTo(VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);

		vkCmdCopyBufferToImage(commandBuffer, stagingBuffer, swapchainImages[imageIndex], VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &copy);
	}

	void VulkanBackend::Present(void)
	{
		if (isDead)
		{
			return;
		}

		// Make sure there is a frame to present
		//
		// The game means "swap buffers" by this, so a present has to happen even when
		// nothing asked us to start a frame. Otherwise the swapchain stops cycling, the
		// window keeps showing its last image, and every overlay that hooks
		// vkQueuePresentKHR, including Steam and any FPS counter, freezes with it, which
		// looks exactly like a hang without being one.
		//
		// The synthesised frame is cleared rather than left undefined, because presenting
		// an undefined image shows whatever happened to be in that memory.
		if (!isFrameActive)
		{
			if (!EnsureFrame())
			{
				return;
			}

			Clear(lastClearColour[0], lastClearColour[1], lastClearColour[2], lastClearColour[3]);
		}

		EndRenderPassIfActive();

		// Record a requested capture
		//
		// Recorded into this frame's command buffer, between the last draw and the
		// transition for presenting, so it sees exactly what the user sees.
		bool     isCapturingRegion = false;
		bool     isCapturingFrame  = false;
		uint32_t regionWidth       = 0;
		uint32_t regionHeight      = 0;

		if (isRegionCaptureRequested && !isCaptureRequested)
		{
			isRegionCaptureRequested = false;
			isCapturingRegion = RecordRegionCapture(regionWidth, regionHeight);
		}

		if (isCaptureRequested)
		{
			isCapturingFrame   = RecordFrameCapture();
			isCaptureRequested = false;
		}

		// Finish and submit the frame
		TransitionTo(VK_IMAGE_LAYOUT_PRESENT_SRC_KHR);

		VkResult result = vkEndCommandBuffer(commandBuffer);
		if (result != VK_SUCCESS)
		{
			Fail("vkEndCommandBuffer", result);
			isFrameActive = false;
			return;
		}

		VkPipelineStageFlags waitStages = ACQUIRE_STAGES;

		VkSubmitInfo submit{ VK_STRUCTURE_TYPE_SUBMIT_INFO };
		submit.waitSemaphoreCount   = 1;
		submit.pWaitSemaphores      = &imageAvailableSemaphore;
		submit.pWaitDstStageMask    = &waitStages;
		submit.commandBufferCount   = 1;
		submit.pCommandBuffers      = &commandBuffer;
		submit.signalSemaphoreCount = 1;
		submit.pSignalSemaphores    = &renderFinishedSemaphore;

		vkResetFences(device, 1, &frameFence);

		result = vkQueueSubmit(queue, 1, &submit, frameFence);
		if (result != VK_SUCCESS)
		{
			Fail("vkQueueSubmit", result);
			isFrameActive = false;
			return;
		}

		if (isCapturingRegion || isCapturingFrame)
		{
			WriteCapture(isCapturingRegion, regionWidth, regionHeight);
		}

		// Present it
		VkPresentInfoKHR present{ VK_STRUCTURE_TYPE_PRESENT_INFO_KHR };
		present.waitSemaphoreCount = 1;
		present.pWaitSemaphores    = &renderFinishedSemaphore;
		present.swapchainCount     = 1;
		present.pSwapchains        = &swapchain;
		present.pImageIndices      = &imageIndex;

		result = vkQueuePresentKHR(queue, &present);
		isFrameActive = false;

		// Rebuild the swapchain when it no longer fits the window
		if (result == VK_ERROR_OUT_OF_DATE_KHR || result == VK_SUBOPTIMAL_KHR)
		{
			LogNote("Vulkan: swapchain needs rebuilding after present (%s).", VkResultName(result));
			DestroySwapchain();

			// A failure here leaves no swapchain, which makes IsDeviceReady report false
			// and stops the game drawing. Silence would make that indistinguishable from
			// a hang, so it says so.
			if (!CreateSwapchain(swapchainExtent.width, swapchainExtent.height))
			{
				LogNote("Vulkan: could not rebuild the swapchain; presenting has stopped.");
			}

			return;
		}

		if (result != VK_SUCCESS)
		{
			Fail("vkQueuePresentKHR", result);
			return;
		}

		// Count the frame
		presentedFrames++;

		if ((presentedFrames % HEARTBEAT_FRAMES) == 0)
		{
			LogHeartbeat();
		}
	}

	void VulkanBackend::RequestCapture(char const* path)
	{
		if (isDead || path == nullptr)
		{
			return;
		}

		isCaptureRequested = true;
		capturePath        = path;
	}

	void VulkanBackend::RequestRegionCapture(char const* path, bool isDepth)
	{
		if (isDead || path == nullptr)
		{
			return;
		}

		isRegionCaptureRequested = true;
		isRegionCaptureDepth     = isDepth;
		regionCapturePath        = path;
	}
}
