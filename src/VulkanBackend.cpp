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

#include "VulkanBackend.h"
#include "Logger.h"
#include <Windows.h>
#include "ShaderBinaries.h"

#include <VertexFormatUtils.h>
#include "version.h"

#include <stdio.h>
#include <string.h>

namespace scvk
{
	namespace
	{
		// 16 MB. One full-screen 1920x1080 BGRA image is about 8 MB and the
		// startup screen is under 2 MB, so this covers several uploads in a
		// single frame without ever reallocating mid-flight.
		constexpr VkDeviceSize kStagingBufferSize = 16u * 1024u * 1024u;

		constexpr char const* kValidationLayer = "VK_LAYER_KHRONOS_validation";

		bool HasValidationLayer(void)
		{
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

			for (VkLayerProperties const& layer : layers)
			{
				if (strcmp(layer.layerName, kValidationLayer) == 0)
				{
					return true;
				}
			}

			return false;
		}

		bool HasInstanceExtension(char const* wanted)
		{
			uint32_t count = 0;
			if (vkEnumerateInstanceExtensionProperties(nullptr, &count, nullptr) != VK_SUCCESS || count == 0)
			{
				return false;
			}

			std::vector<VkExtensionProperties> extensions(count);
			if (vkEnumerateInstanceExtensionProperties(nullptr, &count, extensions.data()) != VK_SUCCESS)
			{
				return false;
			}

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
		 * Without this the layers write to stdout and the debugger, neither of
		 * which is visible when the driver is running inside SimCity 4. The log
		 * is the only channel that survives, so validation is close to useless
		 * in the real environment unless it ends up there.
		 */
		VKAPI_ATTR VkBool32 VKAPI_CALL DebugCallback(
			VkDebugUtilsMessageSeverityFlagBitsEXT severity,
			VkDebugUtilsMessageTypeFlagsEXT,
			VkDebugUtilsMessengerCallbackDataEXT const* data,
			void*)
		{
			char const* level = "info";
			if (severity & VK_DEBUG_UTILS_MESSAGE_SEVERITY_ERROR_BIT_EXT)        level = "ERROR";
			else if (severity & VK_DEBUG_UTILS_MESSAGE_SEVERITY_WARNING_BIT_EXT) level = "warning";

			LogNote("Vulkan %s: %s", level, (data != nullptr && data->pMessage != nullptr) ? data->pMessage : "(no message)");

			// False means "do not abort the offending call", which is what the
			// spec requires here.
			return VK_FALSE;
		}
	}

	VulkanBackend::VulkanBackend(void) { }

	VulkanBackend::~VulkanBackend(void) { Destroy(); }

	void VulkanBackend::Fail(char const* what, VkResult result)
	{
		if (!dead)
		{
			LogNote("Vulkan: %s failed with %s. Falling back to doing nothing; the game will keep "
				"running but nothing will be drawn.", what, VkResultName(result));
			dead = true;
		}
	}

	bool VulkanBackend::CreateInstance(void)
	{
		if (!LoadVulkanLoader())
		{
			dead = true;
			return false;
		}

		VkApplicationInfo app{ VK_STRUCTURE_TYPE_APPLICATION_INFO };
		app.pApplicationName   = "SimCity 4";
		app.applicationVersion = VK_MAKE_VERSION(1, 0, 0);
		app.pEngineName        = "scvk";
		app.engineVersion      = VK_MAKE_VERSION(SCVK_VERSION_MAJOR, SCVK_VERSION_MINOR, SCVK_VERSION_PATCH);

		// 1.0 deliberately. Nothing here needs a later core version, and asking
		// for more would exclude older drivers for no benefit.
		app.apiVersion = VK_API_VERSION_1_0;

		std::vector<char const*> extensions{
			VK_KHR_SURFACE_EXTENSION_NAME,
			VK_KHR_WIN32_SURFACE_EXTENSION_NAME,
		};

		bool wantDebugMessenger = false;
#ifndef NDEBUG
		if (HasInstanceExtension(VK_EXT_DEBUG_UTILS_EXTENSION_NAME))
		{
			extensions.push_back(VK_EXT_DEBUG_UTILS_EXTENSION_NAME);
			wantDebugMessenger = true;
		}
#endif

		VkInstanceCreateInfo info{ VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO };
		info.pApplicationInfo        = &app;
		info.enabledExtensionCount   = static_cast<uint32_t>(extensions.size());
		info.ppEnabledExtensionNames = extensions.data();

#ifndef NDEBUG
		// Only in Debug, and only if the layer is actually installed. SimCity 4
		// is a 32-bit process, so this needs the 32-bit validation layers; a
		// default SDK install only provides 64-bit ones and the layer will be
		// absent here.
		char const* layers[] = { kValidationLayer };
		if (HasValidationLayer())
		{
			info.enabledLayerCount   = 1;
			info.ppEnabledLayerNames = layers;
			LogNote("Vulkan: validation layers enabled.");
		}
		else
		{
			LogNote("Vulkan: validation layers not available to this 32-bit process. Install the "
				"32-bit components of the Vulkan SDK to get them.");
		}
#endif

		VkResult result = vkCreateInstance(&info, nullptr, &instance);
		if (result != VK_SUCCESS)
		{
			Fail("vkCreateInstance", result);
			return false;
		}

		if (!LoadVulkanInstanceFunctions(instance))
		{
			dead = true;
			return false;
		}

		if (wantDebugMessenger && vkCreateDebugUtilsMessengerEXT != nullptr)
		{
			VkDebugUtilsMessengerCreateInfoEXT messengerInfo{ VK_STRUCTURE_TYPE_DEBUG_UTILS_MESSENGER_CREATE_INFO_EXT };
			messengerInfo.messageSeverity =
				VK_DEBUG_UTILS_MESSAGE_SEVERITY_WARNING_BIT_EXT |
				VK_DEBUG_UTILS_MESSAGE_SEVERITY_ERROR_BIT_EXT;
			messengerInfo.messageType =
				VK_DEBUG_UTILS_MESSAGE_TYPE_GENERAL_BIT_EXT |
				VK_DEBUG_UTILS_MESSAGE_TYPE_VALIDATION_BIT_EXT |
				VK_DEBUG_UTILS_MESSAGE_TYPE_PERFORMANCE_BIT_EXT;
			messengerInfo.pfnUserCallback = &DebugCallback;

			if (vkCreateDebugUtilsMessengerEXT(instance, &messengerInfo, nullptr, &debugMessenger) == VK_SUCCESS)
			{
				LogNote("Vulkan: validation messages will be written to this log.");
			}
		}

		return PickPhysicalDevice();
	}

	bool VulkanBackend::PickPhysicalDevice(void)
	{
		uint32_t count = 0;
		VkResult result = vkEnumeratePhysicalDevices(instance, &count, nullptr);
		if (result != VK_SUCCESS || count == 0)
		{
			LogNote("Vulkan: no physical devices reported.");
			dead = true;
			return false;
		}

		std::vector<VkPhysicalDevice> devices(count);
		result = vkEnumeratePhysicalDevices(instance, &count, devices.data());
		if (result != VK_SUCCESS)
		{
			Fail("vkEnumeratePhysicalDevices", result);
			return false;
		}

		// Prefer a discrete GPU. This machine class often has an integrated
		// adapter listed first, and picking it would work but would be a poor
		// default for a game.
		VkPhysicalDevice best = VK_NULL_HANDLE;
		int bestScore = -1;

		for (VkPhysicalDevice candidate : devices)
		{
			VkPhysicalDeviceProperties props{};
			vkGetPhysicalDeviceProperties(candidate, &props);

			int score = 0;
			if (props.deviceType == VK_PHYSICAL_DEVICE_TYPE_DISCRETE_GPU) score = 3;
			else if (props.deviceType == VK_PHYSICAL_DEVICE_TYPE_INTEGRATED_GPU) score = 2;
			else if (props.deviceType == VK_PHYSICAL_DEVICE_TYPE_VIRTUAL_GPU) score = 1;

			LogNote("Vulkan: found device \"%s\" (type %d, API %u.%u.%u)",
				props.deviceName, static_cast<int>(props.deviceType),
				VK_VERSION_MAJOR(props.apiVersion), VK_VERSION_MINOR(props.apiVersion),
				VK_VERSION_PATCH(props.apiVersion));

			if (score > bestScore)
			{
				bestScore = score;
				best = candidate;
			}
		}

		physicalDevice = best;

		VkPhysicalDeviceProperties props{};
		vkGetPhysicalDeviceProperties(physicalDevice, &props);
		deviceName = props.deviceName;

		char buffer[32];
		sprintf_s(buffer, sizeof(buffer), "%u.%u.%u",
			VK_VERSION_MAJOR(props.apiVersion), VK_VERSION_MINOR(props.apiVersion),
			VK_VERSION_PATCH(props.apiVersion));
		apiVersion = buffer;

		LogNote("Vulkan: selected \"%s\".", deviceName.c_str());
		return true;
	}

	bool VulkanBackend::CreateSurfaceAndDevice(void* hwnd, uint32_t width, uint32_t height)
	{
		if (dead || instance == VK_NULL_HANDLE)
		{
			return false;
		}

		windowHandle = hwnd;

		VkWin32SurfaceCreateInfoKHR surfaceInfo{ VK_STRUCTURE_TYPE_WIN32_SURFACE_CREATE_INFO_KHR };
		surfaceInfo.hinstance = GetModuleHandleA(nullptr);
		surfaceInfo.hwnd      = static_cast<HWND>(hwnd);

		VkResult result = vkCreateWin32SurfaceKHR(instance, &surfaceInfo, nullptr, &surface);
		if (result != VK_SUCCESS)
		{
			Fail("vkCreateWin32SurfaceKHR", result);
			return false;
		}

		if (!CreateLogicalDevice())
		{
			return false;
		}

		if (!CreateFrameResources())
		{
			return false;
		}

		if (!CreateStagingBuffer(kStagingBufferSize))
		{
			return false;
		}

		// Descriptors before the pipeline layout, which references the set
		// layout, and the default texture needs the pool and sampler.
		if (!CreateGeometryBuffers() || !CreateShaderModules() ||
			!CreateDescriptorResources() || !CreatePipelineLayout())
		{
			return false;
		}

		return CreateSwapchain(width, height);
	}

	bool VulkanBackend::CreateLogicalDevice(void)
	{
		uint32_t familyCount = 0;
		vkGetPhysicalDeviceQueueFamilyProperties(physicalDevice, &familyCount, nullptr);

		std::vector<VkQueueFamilyProperties> families(familyCount);
		vkGetPhysicalDeviceQueueFamilyProperties(physicalDevice, &familyCount, families.data());

		// One queue that can both do transfers and present. Every real GPU has
		// such a family, and requiring it avoids all cross-queue ownership
		// transfer handling for no practical loss.
		queueFamily = UINT32_MAX;
		for (uint32_t i = 0; i < familyCount; i++)
		{
			if ((families[i].queueFlags & VK_QUEUE_GRAPHICS_BIT) == 0)
			{
				continue;
			}

			VkBool32 present = VK_FALSE;
			vkGetPhysicalDeviceSurfaceSupportKHR(physicalDevice, i, surface, &present);
			if (present == VK_TRUE)
			{
				queueFamily = i;
				break;
			}
		}

		if (queueFamily == UINT32_MAX)
		{
			LogNote("Vulkan: no queue family supports both graphics and presenting to this window.");
			dead = true;
			return false;
		}

		float priority = 1.0f;
		VkDeviceQueueCreateInfo queueInfo{ VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO };
		queueInfo.queueFamilyIndex = queueFamily;
		queueInfo.queueCount       = 1;
		queueInfo.pQueuePriorities = &priority;

		char const* extensions[] = { VK_KHR_SWAPCHAIN_EXTENSION_NAME };

		VkDeviceCreateInfo deviceInfo{ VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO };
		deviceInfo.queueCreateInfoCount    = 1;
		deviceInfo.pQueueCreateInfos       = &queueInfo;
		deviceInfo.enabledExtensionCount   = static_cast<uint32_t>(_countof(extensions));
		deviceInfo.ppEnabledExtensionNames = extensions;
		deviceInfo.pEnabledFeatures        = nullptr;

		VkResult result = vkCreateDevice(physicalDevice, &deviceInfo, nullptr, &device);
		if (result != VK_SUCCESS)
		{
			Fail("vkCreateDevice", result);
			return false;
		}

		if (!LoadVulkanDeviceFunctions(device))
		{
			dead = true;
			return false;
		}

		vkGetDeviceQueue(device, queueFamily, 0, &queue);
		return true;
	}

	bool VulkanBackend::CreateFrameResources(void)
	{
		VkCommandPoolCreateInfo poolInfo{ VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO };
		poolInfo.flags            = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
		poolInfo.queueFamilyIndex = queueFamily;

		VkResult result = vkCreateCommandPool(device, &poolInfo, nullptr, &commandPool);
		if (result != VK_SUCCESS)
		{
			Fail("vkCreateCommandPool", result);
			return false;
		}

		VkCommandBufferAllocateInfo allocInfo{ VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO };
		allocInfo.commandPool        = commandPool;
		allocInfo.level              = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
		allocInfo.commandBufferCount = 1;

		result = vkAllocateCommandBuffers(device, &allocInfo, &commandBuffer);
		if (result != VK_SUCCESS)
		{
			Fail("vkAllocateCommandBuffers", result);
			return false;
		}

		VkSemaphoreCreateInfo semaphoreInfo{ VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO };
		if (vkCreateSemaphore(device, &semaphoreInfo, nullptr, &imageAvailable) != VK_SUCCESS ||
			vkCreateSemaphore(device, &semaphoreInfo, nullptr, &renderFinished) != VK_SUCCESS)
		{
			Fail("vkCreateSemaphore", VK_ERROR_INITIALIZATION_FAILED);
			return false;
		}

		// Created signalled so the first frame does not wait on a submit that
		// never happened.
		VkFenceCreateInfo fenceInfo{ VK_STRUCTURE_TYPE_FENCE_CREATE_INFO };
		fenceInfo.flags = VK_FENCE_CREATE_SIGNALED_BIT;

		result = vkCreateFence(device, &fenceInfo, nullptr, &inFlight);
		if (result != VK_SUCCESS)
		{
			Fail("vkCreateFence", result);
			return false;
		}

		return true;
	}

	bool VulkanBackend::FindMemoryType(uint32_t typeBits, VkMemoryPropertyFlags properties, uint32_t& outIndex) const
	{
		VkPhysicalDeviceMemoryProperties memory{};
		vkGetPhysicalDeviceMemoryProperties(physicalDevice, &memory);

		for (uint32_t i = 0; i < memory.memoryTypeCount; i++)
		{
			bool typeAllowed = (typeBits & (1u << i)) != 0;
			bool hasProps    = (memory.memoryTypes[i].propertyFlags & properties) == properties;

			if (typeAllowed && hasProps)
			{
				outIndex = i;
				return true;
			}
		}

		return false;
	}

	bool VulkanBackend::CreateStagingBuffer(VkDeviceSize size)
	{
		VkBufferCreateInfo bufferInfo{ VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO };
		bufferInfo.size        = size;
		bufferInfo.usage       = VK_BUFFER_USAGE_TRANSFER_SRC_BIT;
		bufferInfo.sharingMode = VK_SHARING_MODE_EXCLUSIVE;

		VkResult result = vkCreateBuffer(device, &bufferInfo, nullptr, &stagingBuffer);
		if (result != VK_SUCCESS)
		{
			Fail("vkCreateBuffer", result);
			return false;
		}

		VkMemoryRequirements requirements{};
		vkGetBufferMemoryRequirements(device, stagingBuffer, &requirements);

		uint32_t typeIndex = 0;
		if (!FindMemoryType(requirements.memoryTypeBits,
				VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT, typeIndex))
		{
			LogNote("Vulkan: no host-visible coherent memory type available for staging.");
			dead = true;
			return false;
		}

		VkMemoryAllocateInfo allocInfo{ VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO };
		allocInfo.allocationSize  = requirements.size;
		allocInfo.memoryTypeIndex = typeIndex;

		result = vkAllocateMemory(device, &allocInfo, nullptr, &stagingMemory);
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

		// Mapped once and left mapped. Coherent memory needs no flushing, and
		// mapping per upload would be pure overhead on a path that runs every
		// frame.
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
		VkSurfaceCapabilitiesKHR caps{};
		VkResult result = vkGetPhysicalDeviceSurfaceCapabilitiesKHR(physicalDevice, surface, &caps);
		if (result != VK_SUCCESS)
		{
			Fail("vkGetPhysicalDeviceSurfaceCapabilitiesKHR", result);
			return false;
		}

		// TRANSFER_DST is what this code actually uses, for the clear and the
		// pixel copies. COLOR_ATTACHMENT is not used by scvk yet, but it is
		// requested anyway for two reasons.
		//
		// Overlay layers (Steam, GPU vendor overlays, and in testing the
		// Rockstar Social Club layer) wrap the swapchain in their own render
		// pass and framebuffer. Those require COLOR_ATTACHMENT on the images,
		// and leaving it out makes every one of them generate validation
		// errors and potentially misbehave, for a flag that costs nothing.
		// The 3D path will need it regardless.
		//
		// The spec guarantees COLOR_ATTACHMENT is in supportedUsageFlags for
		// any surface, so only TRANSFER_DST is worth testing.
		if ((caps.supportedUsageFlags & VK_IMAGE_USAGE_TRANSFER_DST_BIT) == 0)
		{
			LogNote("Vulkan: the surface does not allow swapchain images to be transfer destinations.");
			dead = true;
			return false;
		}

		uint32_t formatCount = 0;
		vkGetPhysicalDeviceSurfaceFormatsKHR(physicalDevice, surface, &formatCount, nullptr);
		if (formatCount == 0)
		{
			LogNote("Vulkan: the surface reports no formats.");
			dead = true;
			return false;
		}

		std::vector<VkSurfaceFormatKHR> formats(formatCount);
		vkGetPhysicalDeviceSurfaceFormatsKHR(physicalDevice, surface, &formatCount, formats.data());

		// B8G8R8A8_UNORM specifically. The game hands us BGRA8 pixels, so
		// matching it means the blits are a straight copy with no conversion
		// and no shader.
		VkSurfaceFormatKHR chosen = formats[0];
		for (VkSurfaceFormatKHR const& candidate : formats)
		{
			if (candidate.format == VK_FORMAT_B8G8R8A8_UNORM &&
				candidate.colorSpace == VK_COLOR_SPACE_SRGB_NONLINEAR_KHR)
			{
				chosen = candidate;
				break;
			}
		}

		if (chosen.format != VK_FORMAT_B8G8R8A8_UNORM)
		{
			LogNote("Vulkan: B8G8R8A8_UNORM is unavailable; using format %d instead. Blits will have "
				"the wrong channel order until a conversion step exists.", static_cast<int>(chosen.format));
		}

		VkExtent2D extent = caps.currentExtent;
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

		uint32_t imageCount = caps.minImageCount + 1;
		if (caps.maxImageCount > 0 && imageCount > caps.maxImageCount)
		{
			imageCount = caps.maxImageCount;
		}

		VkSwapchainCreateInfoKHR info{ VK_STRUCTURE_TYPE_SWAPCHAIN_CREATE_INFO_KHR };
		info.surface          = surface;
		info.minImageCount    = imageCount;
		info.imageFormat      = chosen.format;
		info.imageColorSpace  = chosen.colorSpace;
		info.imageExtent      = extent;
		info.imageArrayLayers = 1;
		info.imageUsage       = VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT |
		                        VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT;
		info.imageSharingMode = VK_SHARING_MODE_EXCLUSIVE;
		info.preTransform     = caps.currentTransform;
		info.compositeAlpha   = VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR;

		// FIFO is the only mode the spec guarantees, and it is vsync, which is
		// what this game expects anyway.
		info.presentMode = VK_PRESENT_MODE_FIFO_KHR;
		info.clipped     = VK_TRUE;
		info.oldSwapchain = VK_NULL_HANDLE;

		result = vkCreateSwapchainKHR(device, &info, nullptr, &swapchain);
		if (result != VK_SUCCESS)
		{
			Fail("vkCreateSwapchainKHR", result);
			return false;
		}

		swapchainFormat = chosen.format;
		swapchainExtent = extent;

		uint32_t actualCount = 0;
		vkGetSwapchainImagesKHR(device, swapchain, &actualCount, nullptr);
		swapchainImages.resize(actualCount);
		vkGetSwapchainImagesKHR(device, swapchain, &actualCount, swapchainImages.data());

		LogNote("Vulkan: swapchain ready, %ux%u, %u images, format %d.",
			extent.width, extent.height, actualCount, static_cast<int>(chosen.format));

		// The render pass depends on the swapchain format, and the
		// framebuffers on the images and extent, so both belong here rather
		// than in one-time setup.
		return CreateDepthResources() && CreateRenderPass() && CreateFramebuffers();
	}

	bool VulkanBackend::EnsureFrame(void)
	{
		if (dead || swapchain == VK_NULL_HANDLE)
		{
			return false;
		}

		if (frameActive)
		{
			return true;
		}

		// One frame in flight. The CPU waits for the previous submit before
		// starting the next, which costs throughput but removes every
		// question about which resources are still being read by the GPU.
		// Worth revisiting once there is a real draw path; meaningless while
		// a frame is one clear and one copy.
		//
		// Both waits are bounded rather than UINT64_MAX. These run on the
		// game's main thread, which is also the thread that pumps its window
		// messages, so blocking here indefinitely makes the whole game
		// unresponsive and unkillable except from Task Manager. A minimised or
		// occluded window can legitimately stall an acquire under FIFO, so
		// this is a reachable state, not a theoretical one. Dropping a frame
		// is always better than wedging the process.
		constexpr uint64_t kWaitTimeoutNs = 1000ull * 1000ull * 1000ull;

		VkResult result = vkWaitForFences(device, 1, &inFlight, VK_TRUE, kWaitTimeoutNs);
		if (result == VK_TIMEOUT)
		{
			LogNote("Vulkan: timed out waiting for the previous frame; skipping this one.");
			return false;
		}

		result = vkAcquireNextImageKHR(device, swapchain, kWaitTimeoutNs,
			imageAvailable, VK_NULL_HANDLE, &imageIndex);

		if (result == VK_TIMEOUT || result == VK_NOT_READY)
		{
			// Normal when the window is minimised or hidden. Not an error, and
			// not worth a log line every frame.
			return false;
		}

		if (result == VK_ERROR_OUT_OF_DATE_KHR)
		{
			LogNote("Vulkan: swapchain out of date; rebuilding.");
			DestroySwapchain();
			if (!CreateSwapchain(swapchainExtent.width, swapchainExtent.height))
			{
				return false;
			}
			return false;
		}

		if (result != VK_SUCCESS && result != VK_SUBOPTIMAL_KHR)
		{
			Fail("vkAcquireNextImageKHR", result);
			return false;
		}

		// The fence is deliberately not reset here. It is reset immediately
		// before the submit that signals it, so that failing out of this
		// function cannot leave it unsignalled forever, which would make every
		// later frame time out.
		vkResetCommandBuffer(commandBuffer, 0);

		VkCommandBufferBeginInfo begin{ VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO };
		begin.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;

		result = vkBeginCommandBuffer(commandBuffer, &begin);
		if (result != VK_SUCCESS)
		{
			Fail("vkBeginCommandBuffer", result);
			return false;
		}

		frameActive = true;
		stagingUsed = 0;
		vertexUsed  = 0;
		indexUsed   = 0;

		// UNDEFINED as the starting point: we overwrite every pixel we care
		// about and discarding the previous contents is cheaper than
		// preserving them.
		currentLayout = VK_IMAGE_LAYOUT_UNDEFINED;
		TransitionTo(VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);

		return true;
	}

	namespace
	{
		/** What a layout implies about access and pipeline stage. */
		void LayoutAccess(VkImageLayout layout, VkAccessFlags& access, VkPipelineStageFlags& stage)
		{
			switch (layout)
			{
			case VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL:
				access = VK_ACCESS_TRANSFER_WRITE_BIT;
				stage  = VK_PIPELINE_STAGE_TRANSFER_BIT;
				break;

			case VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL:
				access = VK_ACCESS_TRANSFER_READ_BIT;
				stage  = VK_PIPELINE_STAGE_TRANSFER_BIT;
				break;

			case VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL:
				access = VK_ACCESS_COLOR_ATTACHMENT_READ_BIT | VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
				stage  = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
				break;

			case VK_IMAGE_LAYOUT_PRESENT_SRC_KHR:
				access = 0;
				stage  = VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT;
				break;

			case VK_IMAGE_LAYOUT_UNDEFINED:
			default:
				access = 0;
				stage  = VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT;
				break;
			}
		}
	}

	void VulkanBackend::TransitionTo(VkImageLayout newLayout)
	{
		if (currentLayout == newLayout)
		{
			return;
		}

		VkAccessFlags        srcAccess = 0;
		VkAccessFlags        dstAccess = 0;
		VkPipelineStageFlags srcStage  = 0;
		VkPipelineStageFlags dstStage  = 0;

		LayoutAccess(currentLayout, srcAccess, srcStage);
		LayoutAccess(newLayout, dstAccess, dstStage);

		VkImageMemoryBarrier barrier{ VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER };
		barrier.srcAccessMask       = srcAccess;
		barrier.dstAccessMask       = dstAccess;
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

		vkCmdPipelineBarrier(commandBuffer, srcStage, dstStage, 0, 0, nullptr, 0, nullptr, 1, &barrier);
		currentLayout = newLayout;
	}

	void VulkanBackend::BeginRenderPassIfNeeded(void)
	{
		if (renderPassActive)
		{
			return;
		}

		// Drawing needs the colour attachment layout; clears and blits need
		// the transfer layout. The game interleaves them freely, so the
		// transition is driven by what is about to happen rather than fixed
		// once per frame.
		TransitionTo(VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL);

		// A freshly created depth image is UNDEFINED, and the render pass
		// declares its attachment layout as the initial one. Moving it here
		// covers the case where the game opens a pass before it has asked for
		// any depth clear.
		if (depthLayoutPending && depthImage != VK_NULL_HANDLE)
		{
			VkImageMemoryBarrier barrier{ VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER };
			barrier.oldLayout           = VK_IMAGE_LAYOUT_UNDEFINED;
			barrier.newLayout           = VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL;
			barrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
			barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
			barrier.image               = depthImage;
			barrier.dstAccessMask       = VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_READ_BIT |
			                              VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT;
			barrier.subresourceRange.aspectMask = VK_IMAGE_ASPECT_DEPTH_BIT;
			barrier.subresourceRange.levelCount = 1;
			barrier.subresourceRange.layerCount = 1;

			vkCmdPipelineBarrier(commandBuffer,
				VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_EARLY_FRAGMENT_TESTS_BIT,
				0, 0, nullptr, 0, nullptr, 1, &barrier);

			depthLayoutPending = false;
		}

		VkRenderPassBeginInfo begin{ VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO };
		begin.renderPass  = renderPass;
		begin.framebuffer = framebuffers[imageIndex];
		begin.renderArea.offset = { 0, 0 };
		begin.renderArea.extent = swapchainExtent;

		// No clear values: the attachment loads what is already there, because
		// the game clears through its own Clear call, which may or may not
		// have happened this frame.
		begin.clearValueCount = 0;

		vkCmdBeginRenderPass(commandBuffer, &begin, VK_SUBPASS_CONTENTS_INLINE);
		renderPassActive = true;

		// The viewport itself is applied per draw, because the game changes it
		// between draws within a single pass.
		ApplyViewport();
	}

	void VulkanBackend::SetViewport(int32_t x, int32_t y, int32_t width, int32_t height)
	{
		viewportX      = x;
		viewportY      = y;
		viewportWidth  = width;
		viewportHeight = height;
	}

	void VulkanBackend::SetFullViewport(void)
	{
		viewportWidth  = -1;
		viewportHeight = -1;
	}

	void VulkanBackend::SetBlendState(bool enabled, uint32_t srcFactor, uint32_t dstFactor)
	{
		blendEnable = enabled;
		blendSrc    = static_cast<uint8_t>(srcFactor);
		blendDst    = static_cast<uint8_t>(dstFactor);
	}

	void VulkanBackend::SetAlphaTest(int comparison, float reference)
	{
		fragmentState[0] = static_cast<float>(comparison);
		fragmentState[1] = reference;
	}

	void VulkanBackend::SetTextureReplace(bool replace)
	{
		fragmentState[2] = replace ? 1.0f : 0.0f;
	}

	void VulkanBackend::ApplyViewport(void)
	{
		if (!renderPassActive)
		{
			return;
		}

		int32_t x = 0;
		int32_t y = 0;
		int32_t width  = static_cast<int32_t>(swapchainExtent.width);
		int32_t height = static_cast<int32_t>(swapchainExtent.height);

		if (viewportWidth > 0 && viewportHeight > 0)
		{
			x = viewportX;
			width  = viewportWidth;
			height = viewportHeight;

			// OpenGL measures the viewport from the bottom of the window and
			// Vulkan from the top, so the origin has to be reflected.
			y = static_cast<int32_t>(swapchainExtent.height) - viewportY - viewportHeight;
		}

		// Clamped because a viewport outside the framebuffer is invalid, and
		// the game can name one while the window is being resized.
		int32_t const maxWidth  = static_cast<int32_t>(swapchainExtent.width);
		int32_t const maxHeight = static_cast<int32_t>(swapchainExtent.height);

		if (x < 0) { width += x; x = 0; }
		if (y < 0) { height += y; y = 0; }
		if (x + width  > maxWidth)  { width  = maxWidth  - x; }
		if (y + height > maxHeight) { height = maxHeight - y; }
		if (width <= 0 || height <= 0) { return; }

		VkViewport viewport{};
		viewport.x        = static_cast<float>(x);
		viewport.y        = static_cast<float>(y);
		viewport.width    = static_cast<float>(width);
		viewport.height   = static_cast<float>(height);
		viewport.minDepth = 0.0f;
		viewport.maxDepth = 1.0f;
		vkCmdSetViewport(commandBuffer, 0, 1, &viewport);

		// Scissor follows the viewport. OpenGL treats them separately and the
		// game enables scissoring only for sub-rectangles, but Vulkan always
		// scissors, and matching the viewport gives the same result.
		VkRect2D scissor{};
		scissor.offset = { x, y };
		scissor.extent = { static_cast<uint32_t>(width), static_cast<uint32_t>(height) };
		vkCmdSetScissor(commandBuffer, 0, 1, &scissor);

		// What actually reached Vulkan, as opposed to what the driver believes
		// it asked for. The arithmetic in the frame dump says these rectangles
		// should already be producing a correct picture, so the discrepancy is
		// either here or downstream of here.
		if (viewportLogsRemaining > 0 &&
			(x != loggedViewport[0] || y != loggedViewport[1] ||
			 width != loggedViewport[2] || height != loggedViewport[3]))
		{
			viewportLogsRemaining--;
			loggedViewport[0] = x;
			loggedViewport[1] = y;
			loggedViewport[2] = width;
			loggedViewport[3] = height;

			LogNote("Vulkan: viewport %d,%d %dx%d (from game %d,%d %dx%d, swapchain %ux%u)",
				x, y, width, height,
				viewportX, viewportY, viewportWidth, viewportHeight,
				swapchainExtent.width, swapchainExtent.height);
		}
	}

	void VulkanBackend::EndRenderPassIfActive(void)
	{
		if (!renderPassActive)
		{
			return;
		}

		vkCmdEndRenderPass(commandBuffer);
		renderPassActive = false;

		// The render pass declares this as its final layout, so record it
		// rather than emitting a redundant barrier.
		currentLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
	}

	void VulkanBackend::Clear(float r, float g, float b, float a)
	{
		if (!EnsureFrame())
		{
			return;
		}

		lastClearColour[0] = r;
		lastClearColour[1] = g;
		lastClearColour[2] = b;
		lastClearColour[3] = a;

		VkClearColorValue colour{};
		colour.float32[0] = r;
		colour.float32[1] = g;
		colour.float32[2] = b;
		colour.float32[3] = a;

		VkImageSubresourceRange range{};
		range.aspectMask     = VK_IMAGE_ASPECT_COLOR_BIT;
		range.baseMipLevel   = 0;
		range.levelCount     = 1;
		range.baseArrayLayer = 0;
		range.layerCount     = 1;

		// A clear is a transfer operation and cannot happen inside a render
		// pass, so any open pass has to end first.
		EndRenderPassIfActive();
		TransitionTo(VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);

		vkCmdClearColorImage(commandBuffer, swapchainImages[imageIndex],
			VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, &colour, 1, &range);
	}

	void VulkanBackend::BlitPixels(int32_t destX, int32_t destY, uint32_t width, uint32_t height,
		uint32_t srcWidth, void const* pixels)
	{
		if (pixels == nullptr || width == 0 || height == 0)
		{
			return;
		}

		if (!EnsureFrame())
		{
			return;
		}

		// A copy whose origin is off the top or left would need the source
		// pointer advanced as well as the extent reduced. Not seen from the
		// game so far, so it is refused rather than half-implemented.
		if (destX < 0 || destY < 0)
		{
			LogNote("Vulkan: blit origin %d,%d is negative; skipping.", destX, destY);
			return;
		}

		if (static_cast<uint32_t>(destX) >= swapchainExtent.width ||
			static_cast<uint32_t>(destY) >= swapchainExtent.height)
		{
			return;
		}

		// Clip against the right and bottom edges. Rows are still strided by
		// the full source width, which is what bufferRowLength expresses, so
		// clipping the extent alone gives the correct result.
		uint32_t copyWidth  = width;
		uint32_t copyHeight = height;

		if (destX + copyWidth > swapchainExtent.width)
		{
			copyWidth = swapchainExtent.width - destX;
		}
		if (destY + copyHeight > swapchainExtent.height)
		{
			copyHeight = swapchainExtent.height - destY;
		}

		VkDeviceSize bytes = static_cast<VkDeviceSize>(srcWidth) * height * 4u;

		if (stagingUsed + bytes > stagingSize)
		{
			LogNote("Vulkan: staging buffer exhausted (%llu bytes needed, %llu free); skipping a blit.",
				static_cast<unsigned long long>(bytes),
				static_cast<unsigned long long>(stagingSize - stagingUsed));
			return;
		}

		VkDeviceSize offset = stagingUsed;
		memcpy(static_cast<uint8_t*>(stagingMapped) + offset, pixels, static_cast<size_t>(bytes));

		// Offsets into the buffer must satisfy the texel size alignment, which
		// is 4 for a 32-bit format. Rounding up keeps every later blit legal.
		stagingUsed = (offset + bytes + 3u) & ~static_cast<VkDeviceSize>(3u);

		VkBufferImageCopy region{};
		region.bufferOffset      = offset;
		region.bufferRowLength   = srcWidth;
		region.bufferImageHeight = height;

		region.imageSubresource.aspectMask     = VK_IMAGE_ASPECT_COLOR_BIT;
		region.imageSubresource.mipLevel       = 0;
		region.imageSubresource.baseArrayLayer = 0;
		region.imageSubresource.layerCount     = 1;

		region.imageOffset = { destX, destY, 0 };
		region.imageExtent = { copyWidth, copyHeight, 1 };

		// Same constraint as the clear: transfers cannot run inside a render
		// pass, so a blit arriving after a draw has to close it.
		EndRenderPassIfActive();
		TransitionTo(VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);

		vkCmdCopyBufferToImage(commandBuffer, stagingBuffer, swapchainImages[imageIndex],
			VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &region);
	}

	void VulkanBackend::RequestCapture(char const* path)
	{
		if (dead || path == nullptr)
		{
			return;
		}

		captureRequested = true;
		capturePath      = path;
	}

	namespace
	{
		/**
		 * Writes BGRA pixels as a 32 bit BMP.
		 *
		 * Top-down, signalled by a negative height, so the rows can go
		 * straight out in the order the GPU produced them.
		 */
		bool WriteBmp(char const* path, uint8_t const* pixels, uint32_t width, uint32_t height, uint32_t rowPitch)
		{
			FILE* file = nullptr;
			if (fopen_s(&file, path, "wb") != 0 || file == nullptr)
			{
				return false;
			}

			uint32_t const imageBytes = width * height * 4u;
			uint32_t const fileBytes  = 14u + 40u + imageBytes;

			auto put16 = [file](uint16_t v) { fwrite(&v, 2, 1, file); };
			auto put32 = [file](uint32_t v) { fwrite(&v, 4, 1, file); };

			fwrite("BM", 1, 2, file);
			put32(fileBytes);
			put32(0);
			put32(14u + 40u);

			put32(40u);
			put32(width);
			put32(static_cast<uint32_t>(-static_cast<int32_t>(height)));
			put16(1);
			put16(32);
			put32(0);
			put32(imageBytes);
			put32(2835);
			put32(2835);
			put32(0);
			put32(0);

			for (uint32_t y = 0; y < height; y++)
			{
				fwrite(pixels + static_cast<size_t>(y) * rowPitch, 4, width, file);
			}

			fclose(file);
			return true;
		}
	}

	void VulkanBackend::Present(void)
	{
		if (dead)
		{
			return;
		}

		// The game means "swap buffers" by this, so a present has to happen
		// even when nothing asked us to start a frame.
		//
		// Only Clear and the blits currently begin a frame, because the draw
		// calls are still stubs. A frame made entirely of draws would leave
		// frameActive false, this would return early, and the swapchain would
		// stop cycling. The window then keeps showing its last image and every
		// overlay that hooks vkQueuePresentKHR, including Steam and any FPS
		// counter, freezes with it, which looks exactly like a hang without
		// being one.
		//
		// Synthesising the frame keeps the swapchain alive. It is cleared
		// rather than left undefined, because presenting an undefined image
		// shows whatever happened to be in that memory.
		if (!frameActive)
		{
			if (!EnsureFrame())
			{
				return;
			}

			Clear(lastClearColour[0], lastClearColour[1], lastClearColour[2], lastClearColour[3]);
		}

		EndRenderPassIfActive();

		// The capture is recorded into this frame's command buffer, between
		// the last draw and the transition for presenting, so it sees exactly
		// what the user sees.
		bool capturingThisFrame = false;

		if (captureRequested)
		{
			VkDeviceSize const needed =
				static_cast<VkDeviceSize>(swapchainExtent.width) * swapchainExtent.height * 4u;

			if (readbackSize < needed)
			{
				if (readbackMapped != nullptr) { vkUnmapMemory(device, readbackMemory); readbackMapped = nullptr; }
				if (readbackMemory != VK_NULL_HANDLE) { vkFreeMemory(device, readbackMemory, nullptr); readbackMemory = VK_NULL_HANDLE; }
				if (readbackBuffer != VK_NULL_HANDLE) { vkDestroyBuffer(device, readbackBuffer, nullptr); readbackBuffer = VK_NULL_HANDLE; }

				if (CreateHostBuffer(needed, VK_BUFFER_USAGE_TRANSFER_DST_BIT,
						readbackBuffer, readbackMemory, readbackMapped))
				{
					readbackSize = needed;
				}
			}

			if (readbackSize >= needed)
			{
				TransitionTo(VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL);

				VkBufferImageCopy region{};
				region.bufferOffset      = 0;
				region.bufferRowLength   = 0;
				region.bufferImageHeight = 0;
				region.imageSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
				region.imageSubresource.layerCount = 1;
				region.imageOffset = { 0, 0, 0 };
				region.imageExtent = { swapchainExtent.width, swapchainExtent.height, 1 };

				vkCmdCopyImageToBuffer(commandBuffer, swapchainImages[imageIndex],
					VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, readbackBuffer, 1, &region);

				capturingThisFrame = true;
			}

			captureRequested = false;
		}

		TransitionTo(VK_IMAGE_LAYOUT_PRESENT_SRC_KHR);

		VkResult result = vkEndCommandBuffer(commandBuffer);
		if (result != VK_SUCCESS)
		{
			Fail("vkEndCommandBuffer", result);
			frameActive = false;
			return;
		}

		VkPipelineStageFlags waitStage = VK_PIPELINE_STAGE_TRANSFER_BIT;

		VkSubmitInfo submit{ VK_STRUCTURE_TYPE_SUBMIT_INFO };
		submit.waitSemaphoreCount   = 1;
		submit.pWaitSemaphores      = &imageAvailable;
		submit.pWaitDstStageMask    = &waitStage;
		submit.commandBufferCount   = 1;
		submit.pCommandBuffers      = &commandBuffer;
		submit.signalSemaphoreCount = 1;
		submit.pSignalSemaphores    = &renderFinished;

		vkResetFences(device, 1, &inFlight);

		result = vkQueueSubmit(queue, 1, &submit, inFlight);
		if (result != VK_SUCCESS)
		{
			Fail("vkQueueSubmit", result);
			frameActive = false;
			return;
		}

		if (capturingThisFrame)
		{
			// Waiting here stalls the pipeline, which is fine: captures are
			// deliberate, rare, and the alternative is reading a buffer the
			// GPU is still writing.
			vkWaitForFences(device, 1, &inFlight, VK_TRUE, UINT64_MAX);

			uint32_t const rowPitch = swapchainExtent.width * 4u;

			if (WriteBmp(capturePath.c_str(), static_cast<uint8_t const*>(readbackMapped),
					swapchainExtent.width, swapchainExtent.height, rowPitch))
			{
				LogNote("Vulkan: captured frame %llu to %s (%ux%u)",
					static_cast<unsigned long long>(presentedFrames + 1),
					capturePath.c_str(), swapchainExtent.width, swapchainExtent.height);
			}
			else
			{
				LogNote("Vulkan: could not write the capture to %s", capturePath.c_str());
			}
		}

		VkPresentInfoKHR present{ VK_STRUCTURE_TYPE_PRESENT_INFO_KHR };
		present.waitSemaphoreCount = 1;
		present.pWaitSemaphores    = &renderFinished;
		present.swapchainCount     = 1;
		present.pSwapchains        = &swapchain;
		present.pImageIndices      = &imageIndex;

		result = vkQueuePresentKHR(queue, &present);
		frameActive = false;

		if (result == VK_ERROR_OUT_OF_DATE_KHR || result == VK_SUBOPTIMAL_KHR)
		{
			LogNote("Vulkan: swapchain needs rebuilding after present (%s).", VkResultName(result));
			DestroySwapchain();

			// A failure here leaves no swapchain, which makes IsDeviceReady
			// report false and stops the game drawing. Silence would make that
			// indistinguishable from a hang, so say so.
			if (!CreateSwapchain(swapchainExtent.width, swapchainExtent.height))
			{
				LogNote("Vulkan: could not rebuild the swapchain; presenting has stopped.");
			}
		}
		else if (result != VK_SUCCESS)
		{
			Fail("vkQueuePresentKHR", result);
		}
		else
		{
			presentedFrames++;

			// A heartbeat, so the log answers "is it still presenting?" without
			// needing the trace. Cheap at one line per few hundred frames.
			if ((presentedFrames % 300ull) == 0ull)
			{
				LogNote("Vulkan: %llu frames presented.", static_cast<unsigned long long>(presentedFrames));
			}
		}
	}

	namespace
	{
		/** The game's internal texture format enumeration, mapped to Vulkan. */
		VkFormat MapInternalFormat(uint32_t gdInternalFormat, bool& compressed)
		{
			compressed = true;

			switch (gdInternalFormat)
			{
			case 5: return VK_FORMAT_BC1_RGBA_UNORM_BLOCK;   // DXT1
			case 6: return VK_FORMAT_BC2_UNORM_BLOCK;        // DXT3
			case 7: return VK_FORMAT_BC3_UNORM_BLOCK;        // DXT5

			default:
				// RGB5, RGB8, RGBA4, RGB5_A1 and RGBA8 all become RGBA8. The
				// narrower ones lose nothing that matters here, and the upload
				// path only ever hands over 8 bits per channel anyway.
				compressed = false;
				return VK_FORMAT_R8G8B8A8_UNORM;
			}
		}

		/** Compressed size for a DXT level, in bytes. */
		VkDeviceSize CompressedSize(VkFormat format, uint32_t width, uint32_t height)
		{
			VkDeviceSize const blocks =
				static_cast<VkDeviceSize>((width + 3u) / 4u) * ((height + 3u) / 4u);

			// DXT1 packs a 4x4 block into 8 bytes; DXT3 and DXT5 add 8 more
			// for the alpha block.
			return blocks * ((format == VK_FORMAT_BC1_RGBA_UNORM_BLOCK) ? 8u : 16u);
		}
	}

	namespace
	{
		/** The game's comparison enumeration, which follows OpenGL's order. */
		VkCompareOp MapCompareOp(uint8_t gdFunc)
		{
			switch (gdFunc)
			{
			case 0:  return VK_COMPARE_OP_NEVER;
			case 1:  return VK_COMPARE_OP_LESS;
			case 2:  return VK_COMPARE_OP_EQUAL;
			case 3:  return VK_COMPARE_OP_LESS_OR_EQUAL;
			case 4:  return VK_COMPARE_OP_GREATER;
			case 5:  return VK_COMPARE_OP_NOT_EQUAL;
			case 6:  return VK_COMPARE_OP_GREATER_OR_EQUAL;
			default: return VK_COMPARE_OP_ALWAYS;
			}
		}

		/** The game's blend factor enumeration, which follows OpenGL's order. */
		VkBlendFactor MapBlendFactor(uint8_t gdFactor)
		{
			switch (gdFactor)
			{
			case 0:  return VK_BLEND_FACTOR_ZERO;
			case 1:  return VK_BLEND_FACTOR_ONE;
			case 2:  return VK_BLEND_FACTOR_SRC_COLOR;
			case 3:  return VK_BLEND_FACTOR_ONE_MINUS_SRC_COLOR;
			case 4:  return VK_BLEND_FACTOR_SRC_ALPHA;
			case 5:  return VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA;
			case 6:  return VK_BLEND_FACTOR_DST_ALPHA;
			case 7:  return VK_BLEND_FACTOR_ONE_MINUS_DST_ALPHA;
			case 8:  return VK_BLEND_FACTOR_DST_COLOR;
			case 9:  return VK_BLEND_FACTOR_ONE_MINUS_DST_COLOR;
			case 10: return VK_BLEND_FACTOR_SRC_ALPHA_SATURATE;
			default: return VK_BLEND_FACTOR_ONE;
			}
		}
	}

	bool VulkanBackend::CreateDescriptorResources(void)
	{
		if (descriptorLayout != VK_NULL_HANDLE)
		{
			return true;
		}

		VkDescriptorSetLayoutBinding binding{};
		binding.binding         = 0;
		binding.descriptorType  = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
		binding.descriptorCount = 1;
		binding.stageFlags      = VK_SHADER_STAGE_FRAGMENT_BIT;

		VkDescriptorSetLayoutCreateInfo layoutInfo{ VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO };
		layoutInfo.bindingCount = 1;
		layoutInfo.pBindings    = &binding;

		VkResult result = vkCreateDescriptorSetLayout(device, &layoutInfo, nullptr, &descriptorLayout);
		if (result != VK_SUCCESS)
		{
			Fail("vkCreateDescriptorSetLayout", result);
			return false;
		}

		// One set per texture, allocated when the texture is created and never
		// rewritten, so nothing can be updated while the GPU is reading it.
		// A session created 89 textures, so this has generous headroom.
		constexpr uint32_t kMaxTextures = 4096;

		VkDescriptorPoolSize poolSize{};
		poolSize.type            = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
		poolSize.descriptorCount = kMaxTextures;

		VkDescriptorPoolCreateInfo poolInfo{ VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO };
		poolInfo.flags         = VK_DESCRIPTOR_POOL_CREATE_FREE_DESCRIPTOR_SET_BIT;
		poolInfo.maxSets       = kMaxTextures;
		poolInfo.poolSizeCount = 1;
		poolInfo.pPoolSizes    = &poolSize;

		result = vkCreateDescriptorPool(device, &poolInfo, nullptr, &descriptorPool);
		if (result != VK_SUCCESS)
		{
			Fail("vkCreateDescriptorPool", result);
			return false;
		}

		// A single sampler for now. TexParameter carries per-texture filter and
		// wrap settings which are not honoured yet; repeat plus linear is the
		// common case and wrong only at the edges of clamped textures.
		VkSamplerCreateInfo samplerInfo{ VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO };
		samplerInfo.magFilter    = VK_FILTER_LINEAR;
		samplerInfo.minFilter    = VK_FILTER_LINEAR;
		samplerInfo.mipmapMode   = VK_SAMPLER_MIPMAP_MODE_LINEAR;
		samplerInfo.addressModeU = VK_SAMPLER_ADDRESS_MODE_REPEAT;
		samplerInfo.addressModeV = VK_SAMPLER_ADDRESS_MODE_REPEAT;
		samplerInfo.addressModeW = VK_SAMPLER_ADDRESS_MODE_REPEAT;
		samplerInfo.maxLod       = VK_LOD_CLAMP_NONE;

		result = vkCreateSampler(device, &samplerInfo, nullptr, &sampler);
		if (result != VK_SUCCESS)
		{
			Fail("vkCreateSampler", result);
			return false;
		}

		// A command buffer and fence reserved for uploads, which happen
		// outside the frame's own recording.
		VkCommandBufferAllocateInfo allocInfo{ VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO };
		allocInfo.commandPool        = commandPool;
		allocInfo.level              = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
		allocInfo.commandBufferCount = 1;

		result = vkAllocateCommandBuffers(device, &allocInfo, &uploadCommandBuffer);
		if (result != VK_SUCCESS)
		{
			Fail("vkAllocateCommandBuffers (upload)", result);
			return false;
		}

		VkFenceCreateInfo fenceInfo{ VK_STRUCTURE_TYPE_FENCE_CREATE_INFO };
		result = vkCreateFence(device, &fenceInfo, nullptr, &uploadFence);
		if (result != VK_SUCCESS)
		{
			Fail("vkCreateFence (upload)", result);
			return false;
		}

		return CreateDefaultTexture();
	}

	bool VulkanBackend::CreateDefaultTexture(void)
	{
		// Handle 0: a single white texel. Draws with no texture bound sample
		// this, so modulate becomes a multiply by one and untextured geometry
		// needs no separate shader or pipeline.
		textures.clear();
		textures.emplace_back();

		uint32_t handle = CreateTexture(4, 1, 1, 1);
		if (handle == 0)
		{
			return false;
		}

		uint8_t const white[4] = { 255, 255, 255, 255 };
		UploadTextureLevel(handle, 0, 0, 0, 1, 1, 1 /* RGBA */, 1 /* unsigned byte */, 0, white);

		// Move it into slot 0 so an unset texture resolves to it naturally.
		//
		// The source slot must be blanked, not just marked dead: a copy leaves
		// both entries owning the same image, view and memory, and teardown
		// walks every slot, so leaving it would destroy each of them twice.
		textures[0] = textures[handle];
		textures[handle] = Texture{};

		return true;
	}

	uint32_t VulkanBackend::CreateTexture(uint32_t gdInternalFormat, uint32_t width, uint32_t height, uint32_t levels)
	{
		if (dead || device == VK_NULL_HANDLE || width == 0 || height == 0)
		{
			return 0;
		}

		Texture texture;
		texture.format = MapInternalFormat(gdInternalFormat, texture.compressed);
		texture.width  = width;
		texture.height = height;
		texture.levels = (levels == 0) ? 1 : levels;

		VkImageCreateInfo imageInfo{ VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO };
		imageInfo.imageType     = VK_IMAGE_TYPE_2D;
		imageInfo.format        = texture.format;
		imageInfo.extent        = { width, height, 1 };
		imageInfo.mipLevels     = texture.levels;
		imageInfo.arrayLayers   = 1;
		imageInfo.samples       = VK_SAMPLE_COUNT_1_BIT;
		imageInfo.tiling        = VK_IMAGE_TILING_OPTIMAL;
		imageInfo.usage         = VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_SAMPLED_BIT;
		imageInfo.sharingMode   = VK_SHARING_MODE_EXCLUSIVE;
		imageInfo.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;

		VkResult result = vkCreateImage(device, &imageInfo, nullptr, &texture.image);
		if (result != VK_SUCCESS)
		{
			LogNote("Vulkan: could not create a %ux%u texture (format %d): %s",
				width, height, static_cast<int>(texture.format), VkResultName(result));
			return 0;
		}

		VkMemoryRequirements requirements{};
		vkGetImageMemoryRequirements(device, texture.image, &requirements);

		uint32_t typeIndex = 0;
		if (!FindMemoryType(requirements.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT, typeIndex))
		{
			vkDestroyImage(device, texture.image, nullptr);
			LogNote("Vulkan: no device-local memory type for textures.");
			return 0;
		}

		VkMemoryAllocateInfo allocInfo{ VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO };
		allocInfo.allocationSize  = requirements.size;
		allocInfo.memoryTypeIndex = typeIndex;

		result = vkAllocateMemory(device, &allocInfo, nullptr, &texture.memory);
		if (result != VK_SUCCESS)
		{
			vkDestroyImage(device, texture.image, nullptr);
			Fail("vkAllocateMemory (texture)", result);
			return 0;
		}

		vkBindImageMemory(device, texture.image, texture.memory, 0);

		VkImageViewCreateInfo viewInfo{ VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO };
		viewInfo.image    = texture.image;
		viewInfo.viewType = VK_IMAGE_VIEW_TYPE_2D;
		viewInfo.format   = texture.format;
		viewInfo.subresourceRange.aspectMask   = VK_IMAGE_ASPECT_COLOR_BIT;
		viewInfo.subresourceRange.levelCount   = texture.levels;
		viewInfo.subresourceRange.layerCount   = 1;

		result = vkCreateImageView(device, &viewInfo, nullptr, &texture.view);
		if (result != VK_SUCCESS)
		{
			Fail("vkCreateImageView (texture)", result);
			return 0;
		}

		VkDescriptorSetAllocateInfo setInfo{ VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO };
		setInfo.descriptorPool     = descriptorPool;
		setInfo.descriptorSetCount = 1;
		setInfo.pSetLayouts        = &descriptorLayout;

		result = vkAllocateDescriptorSets(device, &setInfo, &texture.descriptor);
		if (result != VK_SUCCESS)
		{
			Fail("vkAllocateDescriptorSets", result);
			return 0;
		}

		VkDescriptorImageInfo imageBinding{};
		imageBinding.sampler     = sampler;
		imageBinding.imageView   = texture.view;
		imageBinding.imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;

		VkWriteDescriptorSet write{ VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET };
		write.dstSet          = texture.descriptor;
		write.dstBinding      = 0;
		write.descriptorCount = 1;
		write.descriptorType  = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
		write.pImageInfo      = &imageBinding;

		vkUpdateDescriptorSets(device, 1, &write, 0, nullptr);

		texture.live = true;

		// Put the image into its sampled layout straight away, so a draw that
		// binds it before anything has been uploaded is still valid.
		VkCommandBufferBeginInfo begin{ VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO };
		begin.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;

		vkResetCommandBuffer(uploadCommandBuffer, 0);
		vkBeginCommandBuffer(uploadCommandBuffer, &begin);

		VkImageMemoryBarrier barrier{ VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER };
		barrier.oldLayout           = VK_IMAGE_LAYOUT_UNDEFINED;
		barrier.newLayout           = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
		barrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
		barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
		barrier.image               = texture.image;
		barrier.dstAccessMask       = VK_ACCESS_SHADER_READ_BIT;
		barrier.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
		barrier.subresourceRange.levelCount = texture.levels;
		barrier.subresourceRange.layerCount = 1;

		vkCmdPipelineBarrier(uploadCommandBuffer,
			VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT,
			0, 0, nullptr, 0, nullptr, 1, &barrier);

		vkEndCommandBuffer(uploadCommandBuffer);

		VkSubmitInfo submit{ VK_STRUCTURE_TYPE_SUBMIT_INFO };
		submit.commandBufferCount = 1;
		submit.pCommandBuffers    = &uploadCommandBuffer;

		vkResetFences(device, 1, &uploadFence);
		vkQueueSubmit(queue, 1, &submit, uploadFence);
		vkWaitForFences(device, 1, &uploadFence, VK_TRUE, UINT64_MAX);

		textures.push_back(texture);
		return static_cast<uint32_t>(textures.size() - 1);
	}

	void VulkanBackend::UploadTextureLevel(uint32_t handle, uint32_t level,
		int32_t xoffset, int32_t yoffset, uint32_t width, uint32_t height,
		uint32_t gdFormat, uint32_t gdType, uint32_t rowLength, void const* pixels)
	{
		if (dead || pixels == nullptr || handle == 0 || handle >= textures.size())
		{
			return;
		}

		Texture& texture = textures[handle];
		if (!texture.live || width == 0 || height == 0)
		{
			return;
		}

		if (level + 1 > texture.uploadedLevels)
		{
			texture.uploadedLevels = level + 1;
		}

		// Build a tightly packed copy in the destination's own format.
		std::vector<uint8_t> staged;

		if (texture.compressed)
		{
			VkDeviceSize const size = CompressedSize(texture.format, width, height);
			staged.resize(static_cast<size_t>(size));
			memcpy(staged.data(), pixels, staged.size());
		}
		else
		{
			// The game's format enumeration: 1 is RGBA, 3 is BGRA. Only
			// 8-bit-per-channel input has ever been observed.
			bool const isBgra = (gdFormat == 3);
			bool const isRgba = (gdFormat == 1);

			if ((!isBgra && !isRgba) || gdType != 1)
			{
				LogNote("Vulkan: texture upload format %u type %u is not handled; skipping.", gdFormat, gdType);
				return;
			}

			uint32_t const srcStride = (rowLength != 0) ? rowLength : width;

			staged.resize(static_cast<size_t>(width) * height * 4u);

			uint8_t const* src = static_cast<uint8_t const*>(pixels);
			for (uint32_t y = 0; y < height; y++)
			{
				uint8_t const* srcRow = src + static_cast<size_t>(y) * srcStride * 4u;
				uint8_t*       dstRow = staged.data() + static_cast<size_t>(y) * width * 4u;

				if (isRgba)
				{
					memcpy(dstRow, srcRow, static_cast<size_t>(width) * 4u);
				}
				else
				{
					// BGRA to RGBA. Done here rather than by choosing a BGRA
					// image format, so every uncompressed texture ends up in
					// one predictable layout.
					for (uint32_t x = 0; x < width; x++)
					{
						dstRow[x * 4 + 0] = srcRow[x * 4 + 2];
						dstRow[x * 4 + 1] = srcRow[x * 4 + 1];
						dstRow[x * 4 + 2] = srcRow[x * 4 + 0];
						dstRow[x * 4 + 3] = srcRow[x * 4 + 3];
					}
				}
			}
		}

		// Write out what the game actually handed over.
		//
		// The interface geometry, viewports, projections and uploads are all
		// 1:1, yet the finished picture is magnified, so the question is
		// whether the content arriving here is already wrong. A texture that
		// looks like a normal interface element means we mangle it later; one
		// that is already blown up means the game drew it that way, and the
		// cause is something we report back to the game.
		// Only once past the startup screen. The first uploads are all splash
		// tiles, which were dumped and turned out to be perfectly correct, so
		// they answer nothing. The interesting textures are the interface ones
		// uploaded later.
		if (textureDumpsRemaining > 0 && !texture.compressed && presentedFrames > 1000)
		{
			textureDumpsRemaining--;

			char path[MAX_PATH];
			if (LogDirectory(path, sizeof(path)))
			{
				char name[64];
				sprintf_s(name, sizeof(name), "scvk-tex-%u-%ux%u.bmp", handle, width, height);

				if (strlen(path) + strlen(name) < sizeof(path))
				{
					strcat_s(path, sizeof(path), name);

					// The source is BGRA, which is already BMP's byte order,
					// so it goes out untouched rather than via the swizzled
					// copy above.
					uint32_t const srcStride = (rowLength != 0) ? rowLength : width;
					WriteBmp(path, static_cast<uint8_t const*>(pixels), width, height, srcStride * 4u);

					LogNote("Vulkan: wrote texture %u (%ux%u) to %s", handle, width, height, path);
				}
			}
		}

		VkBuffer       upload       = VK_NULL_HANDLE;
		VkDeviceMemory uploadMemory = VK_NULL_HANDLE;
		void*          uploadMapped = nullptr;

		if (!CreateHostBuffer(staged.size(), VK_BUFFER_USAGE_TRANSFER_SRC_BIT, upload, uploadMemory, uploadMapped))
		{
			return;
		}

		memcpy(uploadMapped, staged.data(), staged.size());

		VkCommandBufferBeginInfo begin{ VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO };
		begin.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;

		vkResetCommandBuffer(uploadCommandBuffer, 0);
		vkBeginCommandBuffer(uploadCommandBuffer, &begin);

		VkImageMemoryBarrier barrier{ VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER };
		barrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
		barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
		barrier.image               = texture.image;
		barrier.subresourceRange.aspectMask     = VK_IMAGE_ASPECT_COLOR_BIT;
		barrier.subresourceRange.baseMipLevel   = level;
		barrier.subresourceRange.levelCount     = 1;
		barrier.subresourceRange.layerCount     = 1;

		barrier.oldLayout     = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
		barrier.newLayout     = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
		barrier.srcAccessMask = VK_ACCESS_SHADER_READ_BIT;
		barrier.dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;

		vkCmdPipelineBarrier(uploadCommandBuffer,
			VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT,
			0, 0, nullptr, 0, nullptr, 1, &barrier);

		VkBufferImageCopy region{};
		region.bufferOffset      = 0;
		region.bufferRowLength   = 0;
		region.bufferImageHeight = 0;
		region.imageSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
		region.imageSubresource.mipLevel   = level;
		region.imageSubresource.layerCount = 1;
		region.imageOffset = { xoffset, yoffset, 0 };
		region.imageExtent = { width, height, 1 };

		vkCmdCopyBufferToImage(uploadCommandBuffer, upload, texture.image,
			VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &region);

		barrier.oldLayout     = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
		barrier.newLayout     = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
		barrier.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
		barrier.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;

		vkCmdPipelineBarrier(uploadCommandBuffer,
			VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT,
			0, 0, nullptr, 0, nullptr, 1, &barrier);

		vkEndCommandBuffer(uploadCommandBuffer);

		VkSubmitInfo submit{ VK_STRUCTURE_TYPE_SUBMIT_INFO };
		submit.commandBufferCount = 1;
		submit.pCommandBuffers    = &uploadCommandBuffer;

		vkResetFences(device, 1, &uploadFence);
		vkQueueSubmit(queue, 1, &submit, uploadFence);

		// Waited on rather than pipelined. Uploads are rare (a few hundred in
		// a session) and always happen outside the frame, so the simplicity is
		// worth more than the throughput.
		vkWaitForFences(device, 1, &uploadFence, VK_TRUE, UINT64_MAX);

		vkUnmapMemory(device, uploadMemory);
		vkFreeMemory(device, uploadMemory, nullptr);
		vkDestroyBuffer(device, upload, nullptr);
	}

	void VulkanBackend::SetTexture(uint32_t handle)
	{
		currentTexture = (handle < textures.size() && textures[handle].live) ? handle : 0;
	}

	void VulkanBackend::LogTextureInfo(uint32_t handle, char const* why)
	{
		if (handle == 0 || handle >= textures.size() || !textures[handle].live)
		{
			LogNote("  TEXINFO %s: handle %u is not a live texture", why, handle);
			return;
		}

		Texture const& texture = textures[handle];
		LogNote("  TEXINFO %s: handle %u, %ux%u, format %d, %s, %u level(s) declared, %u uploaded",
			why, handle, texture.width, texture.height, static_cast<int>(texture.format),
			texture.compressed ? "compressed" : "plain",
			texture.levels, texture.uploadedLevels);
	}

	void VulkanBackend::SetTexture1(uint32_t handle)
	{
		currentTexture1 = (handle < textures.size() && textures[handle].live) ? handle : 0;
	}

	void VulkanBackend::SetTextureStageEnabled(uint32_t stage, bool enabled)
	{
		if (stage < 2)
		{
			stageEnabled[stage] = enabled;
		}
	}

	void VulkanBackend::SetCombinerState(uint32_t stage, uint32_t packedRGB, uint32_t packedAlpha)
	{
		if (stage > 1)
		{
			return;
		}

		combinerState[stage * 2 + 0] = packedRGB;
		combinerState[stage * 2 + 1] = packedAlpha;
	}

	void VulkanBackend::SetConstantColour(float r, float g, float b, float a)
	{
		constantColour[0] = r;
		constantColour[1] = g;
		constantColour[2] = b;
		constantColour[3] = a;
	}

	void VulkanBackend::DestroyTexture(uint32_t handle)
	{
		if (handle == 0 || handle >= textures.size() || !textures[handle].live)
		{
			return;
		}

		vkDeviceWaitIdle(device);

		Texture& texture = textures[handle];

		if (texture.view != VK_NULL_HANDLE)   vkDestroyImageView(device, texture.view, nullptr);
		if (texture.image != VK_NULL_HANDLE)  vkDestroyImage(device, texture.image, nullptr);
		if (texture.memory != VK_NULL_HANDLE) vkFreeMemory(device, texture.memory, nullptr);

		texture = Texture{};

		if (currentTexture == handle)
		{
			currentTexture = 0;
		}
	}

	void VulkanBackend::DestroyTextures(void)
	{
		for (Texture& texture : textures)
		{
			if (texture.view != VK_NULL_HANDLE)   vkDestroyImageView(device, texture.view, nullptr);
			if (texture.image != VK_NULL_HANDLE)  vkDestroyImage(device, texture.image, nullptr);
			if (texture.memory != VK_NULL_HANDLE) vkFreeMemory(device, texture.memory, nullptr);
			texture = Texture{};
		}

		textures.clear();
		currentTexture = 0;
	}

	bool VulkanBackend::CreateHostBuffer(VkDeviceSize size, VkBufferUsageFlags usage,
		VkBuffer& outBuffer, VkDeviceMemory& outMemory, void*& outMapped)
	{
		VkBufferCreateInfo bufferInfo{ VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO };
		bufferInfo.size        = size;
		bufferInfo.usage       = usage;
		bufferInfo.sharingMode = VK_SHARING_MODE_EXCLUSIVE;

		VkResult result = vkCreateBuffer(device, &bufferInfo, nullptr, &outBuffer);
		if (result != VK_SUCCESS)
		{
			Fail("vkCreateBuffer", result);
			return false;
		}

		VkMemoryRequirements requirements{};
		vkGetBufferMemoryRequirements(device, outBuffer, &requirements);

		uint32_t typeIndex = 0;
		if (!FindMemoryType(requirements.memoryTypeBits,
				VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT, typeIndex))
		{
			LogNote("Vulkan: no host-visible coherent memory type available.");
			dead = true;
			return false;
		}

		VkMemoryAllocateInfo allocInfo{ VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO };
		allocInfo.allocationSize  = requirements.size;
		allocInfo.memoryTypeIndex = typeIndex;

		result = vkAllocateMemory(device, &allocInfo, nullptr, &outMemory);
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

		result = vkMapMemory(device, outMemory, 0, requirements.size, 0, &outMapped);
		if (result != VK_SUCCESS)
		{
			Fail("vkMapMemory", result);
			return false;
		}

		return true;
	}

	bool VulkanBackend::CreateGeometryBuffers(void)
	{
		if (vertexBuffer != VK_NULL_HANDLE)
		{
			return true;
		}

		// Sized from measurement rather than guesswork: a city frame overflowed
		// 16MB even after each draw was trimmed to the vertices it references.
		constexpr VkDeviceSize kVertexBufferSize = 64u * 1024u * 1024u;

		if (!CreateHostBuffer(kVertexBufferSize, VK_BUFFER_USAGE_VERTEX_BUFFER_BIT,
				vertexBuffer, vertexMemory, vertexMapped))
		{
			return false;
		}

		vertexSize = kVertexBufferSize;
		vertexUsed = 0;

		// Indices arriving with a draw are client memory too, so they get the
		// same treatment as the vertices: copied into a per-frame arena that is
		// rewound when the frame begins.
		constexpr VkDeviceSize kIndexBufferSize = 8u * 1024u * 1024u;

		if (!CreateHostBuffer(kIndexBufferSize, VK_BUFFER_USAGE_INDEX_BUFFER_BIT,
				indexBuffer, indexMemory, indexMapped))
		{
			return false;
		}

		indexBufferSize = kIndexBufferSize;
		indexUsed       = 0;

		// Vulkan has no quad topology, so quads are drawn as indexed triangle
		// pairs. The index pattern depends only on the vertex count, never on
		// the data, so it is built once and reused.
		constexpr uint32_t kMaxQuads = 16384;

		void* indexMapped = nullptr;
		if (!CreateHostBuffer(static_cast<VkDeviceSize>(kMaxQuads) * 6u * sizeof(uint32_t),
				VK_BUFFER_USAGE_INDEX_BUFFER_BIT, quadIndexBuffer, quadIndexMemory, indexMapped))
		{
			return false;
		}

		uint32_t* indices = static_cast<uint32_t*>(indexMapped);
		for (uint32_t quad = 0; quad < kMaxQuads; quad++)
		{
			uint32_t const base = quad * 4u;
			uint32_t*      out  = indices + quad * 6u;

			out[0] = base + 0; out[1] = base + 1; out[2] = base + 2;
			out[3] = base + 0; out[4] = base + 2; out[5] = base + 3;
		}

		vkUnmapMemory(device, quadIndexMemory);
		quadCapacity = kMaxQuads;

		return true;
	}

	void VulkanBackend::SetTransform(float const* glMvp)
	{
		// OpenGL clip space and Vulkan clip space differ in two ways: Y points
		// the other way, and depth runs 0..1 rather than -1..1. Correcting
		// here keeps the game's matrices untouched and confines the difference
		// to the one place that knows it is Vulkan.
		//
		// Column-major throughout, matching both the game and GLSL, so the
		// result can be pushed straight into the shader.
		static float const correction[16] = {
			1.0f,  0.0f, 0.0f, 0.0f,
			0.0f, -1.0f, 0.0f, 0.0f,
			0.0f,  0.0f, 0.5f, 0.0f,
			0.0f,  0.0f, 0.5f, 1.0f,
		};

		for (int col = 0; col < 4; col++)
		{
			for (int row = 0; row < 4; row++)
			{
				float sum = 0.0f;
				for (int k = 0; k < 4; k++)
				{
					sum += correction[k * 4 + row] * glMvp[col * 4 + k];
				}
				transform[col * 4 + row] = sum;
			}
		}
	}

	VulkanBackend::VertexLayout VulkanBackend::DecodeVertexLayout(uint32_t gdVertexFormat)
	{
		// Decoded with the game's own packed-format helpers rather than a
		// hand-written table. The formats disagree about which attributes are
		// present and where, and the packed encoding is the authority.
		VertexLayout layout;
		layout.stride = RZVertexFormatStride(gdVertexFormat);

		layout.hasColour = RZVertexFormatNumElements(gdVertexFormat, kGDElementType_Color) != 0;
		if (layout.hasColour)
		{
			layout.colourOffset = RZVertexFormatElementOffset(gdVertexFormat, kGDElementType_Color, 0);
		}

		// This counts coordinate sets, not components. The terrain carries two,
		// one per texture stage, which is what the second stage samples with.
		layout.texCoordSets = RZVertexFormatNumElements(gdVertexFormat, kGDElementType_TexCoord);
		if (layout.texCoordSets > 2)
		{
			layout.texCoordSets = 2;
		}

		for (uint32_t set = 0; set < layout.texCoordSets; set++)
		{
			layout.texCoordOffset[set] = RZVertexFormatElementOffset(gdVertexFormat, kGDElementType_TexCoord, set);
		}

		return layout;
	}

	bool VulkanBackend::MapTopology(uint32_t gdPrimType, VkPrimitiveTopology& topology, bool& asQuads)
	{
		// The game's primitive numbering, from its own translation table:
		// 0 triangles, 1 triangle strip, 2 triangle fan, 3 points, 4 lines,
		// 5 line strip, 6 quads, 7 quad strip.
		asQuads = false;

		switch (gdPrimType)
		{
		case 0: topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;  return true;
		case 1: topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_STRIP; return true;
		case 2: topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_FAN;   return true;
		case 3: topology = VK_PRIMITIVE_TOPOLOGY_POINT_LIST;     return true;
		case 4: topology = VK_PRIMITIVE_TOPOLOGY_LINE_LIST;      return true;
		case 5: topology = VK_PRIMITIVE_TOPOLOGY_LINE_STRIP;     return true;
		case 6: topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST; asQuads = true; return true;

		// A quad strip covers the same surface as a triangle strip over the
		// same vertices, so it needs no index expansion at all.
		case 7: topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_STRIP; return true;

		default:
			LogNote("Vulkan: primitive type %u is not handled; skipping the draw.", gdPrimType);
			return false;
		}
	}

	bool VulkanBackend::BindDrawState(uint32_t gdVertexFormat, VkPrimitiveTopology topology,
		VkDeviceSize vertexOffset, uint32_t texCoordSets)
	{
		PipelineKey key{ gdVertexFormat, topology, blendEnable, blendSrc, blendDst, depthTest, depthWrite, depthCompare };
		VkPipeline pipeline = GetPipeline(key);
		if (pipeline == VK_NULL_HANDLE)
		{
			return false;
		}

		BeginRenderPassIfNeeded();

		// Per draw, not once per render pass.
		//
		// The game changes the viewport between draws inside a single pass: it
		// tiles the interface by drawing full-sized quads and letting a much
		// smaller viewport clip each one. Applying the viewport only when the
		// pass opens leaves every draw in that pass using whichever viewport
		// happened to be current at the time, which stretched the interface
		// across the window.
		ApplyViewport();

		// The second stage runs only when it is switched on, has a texture, and
		// the geometry carries a coordinate set to sample it with. All three
		// matter: the game leaves a 4x4 placeholder bound to the stage for the
		// whole session and turns the stage itself off, so taking the binding
		// as the signal modulates the city terrain down to black.
		//
		// Everything else keeps the texture environment path, which is already
		// correct and has no business being rewritten in terms of combiners.
		bool const twoStages = texCoordSets >= 2 && currentTexture1 != 0 && stageEnabled[1];
		fragmentState[3] = twoStages ? 2.0f : 1.0f;

		vkCmdBindPipeline(commandBuffer, VK_PIPELINE_BIND_POINT_GRAPHICS, pipeline);
		vkCmdPushConstants(commandBuffer, pipelineLayout,
			VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT,
			0, sizeof(transform), transform);
		vkCmdPushConstants(commandBuffer, pipelineLayout,
			VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT,
			sizeof(transform), sizeof(fragmentState), fragmentState);
		vkCmdPushConstants(commandBuffer, pipelineLayout,
			VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT,
			sizeof(transform) + sizeof(fragmentState), sizeof(combinerState), combinerState);
		vkCmdPushConstants(commandBuffer, pipelineLayout,
			VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT,
			sizeof(transform) + sizeof(fragmentState) + sizeof(combinerState),
			sizeof(constantColour), constantColour);

		// Slot 0 holds the 1x1 white texture, so an unset or deleted texture
		// still binds something valid and multiplies by one. That is also what
		// the second stage gets when it is idle, since the fragment shader
		// samples both unconditionally.
		uint32_t const bound = (currentTexture < textures.size() && textures[currentTexture].live)
			? currentTexture : 0;
		uint32_t const bound1 = (twoStages && currentTexture1 < textures.size() && textures[currentTexture1].live)
			? currentTexture1 : 0;

		VkDescriptorSet const sets[] = { textures[bound].descriptor, textures[bound1].descriptor };

		vkCmdBindDescriptorSets(commandBuffer, VK_PIPELINE_BIND_POINT_GRAPHICS, pipelineLayout,
			0, _countof(sets), sets, 0, nullptr);

		VkBuffer buffers[] = { vertexBuffer };
		VkDeviceSize offsets[] = { vertexOffset };
		vkCmdBindVertexBuffers(commandBuffer, 0, 1, buffers, offsets);

		return true;
	}

	bool VulkanBackend::UploadVertices(void const* vertices, uint32_t firstVertex,
		uint32_t vertexCount, uint32_t stride, VkDeviceSize& outOffset)
	{
		VkDeviceSize const bytes = static_cast<VkDeviceSize>(vertexCount) * stride;

		// Alignment so the binding offset stays legal for the attributes.
		VkDeviceSize const offset = (vertexUsed + 15u) & ~static_cast<VkDeviceSize>(15u);

		if (offset + bytes > vertexSize)
		{
			LogNote("Vulkan: per-frame vertex buffer exhausted; dropping a draw of %u vertices.", vertexCount);
			return false;
		}

		memcpy(static_cast<uint8_t*>(vertexMapped) + offset,
			static_cast<uint8_t const*>(vertices) + static_cast<size_t>(firstVertex) * stride,
			static_cast<size_t>(bytes));

		vertexUsed = offset + bytes;
		outOffset  = offset;
		return true;
	}

	void VulkanBackend::DrawVertices(uint32_t gdPrimType, uint32_t gdVertexFormat,
		void const* vertices, uint32_t firstVertex, uint32_t vertexCount)
	{
		VertexLayout const layout = DecodeVertexLayout(gdVertexFormat);
		uint32_t const stride = layout.stride;

		if (vertices == nullptr || vertexCount == 0 || stride == 0)
		{
			return;
		}

		VkPrimitiveTopology topology;
		bool asQuads = false;

		if (!MapTopology(gdPrimType, topology, asQuads) || !EnsureFrame())
		{
			return;
		}

		VkDeviceSize offset = 0;
		if (!UploadVertices(vertices, firstVertex, vertexCount, stride, offset))
		{
			return;
		}

		if (!BindDrawState(gdVertexFormat, topology, offset, layout.texCoordSets))
		{
			return;
		}

		if (asQuads)
		{
			uint32_t quads = vertexCount / 4u;
			if (quads > quadCapacity)
			{
				LogNote("Vulkan: %u quads exceeds the index buffer capacity of %u; clamping.",
					quads, quadCapacity);
				quads = quadCapacity;
			}

			if (quads == 0)
			{
				return;
			}

			vkCmdBindIndexBuffer(commandBuffer, quadIndexBuffer, 0, VK_INDEX_TYPE_UINT32);
			vkCmdDrawIndexed(commandBuffer, quads * 6u, 1, 0, 0, 0);
		}
		else
		{
			vkCmdDraw(commandBuffer, vertexCount, 1, 0, 0);
		}
	}

	void VulkanBackend::DrawIndexedVertices(uint32_t gdPrimType, uint32_t gdVertexFormat,
		void const* vertices, void const* indices, uint32_t indexCount, bool indicesAre32Bit)
	{
		VertexLayout const layout = DecodeVertexLayout(gdVertexFormat);
		uint32_t const stride = layout.stride;

		if (vertices == nullptr || indices == nullptr || indexCount == 0 || stride == 0)
		{
			return;
		}

		VkPrimitiveTopology topology;
		bool asQuads = false;

		if (!MapTopology(gdPrimType, topology, asQuads) || !EnsureFrame())
		{
			return;
		}

		// The indices are client memory and say nothing about how many vertices
		// back them, so the range has to be measured before anything can be
		// copied.
		//
		// Only the span actually referenced is taken, not everything from zero.
		// The game indexes small windows of large shared arrays, and uploading
		// each window's whole prefix exhausted the per-frame arena partway
		// through a city frame, which dropped several hundred draws.
		uint32_t lowest  = UINT32_MAX;
		uint32_t highest = 0;

		if (indicesAre32Bit)
		{
			uint32_t const* source = static_cast<uint32_t const*>(indices);
			for (uint32_t i = 0; i < indexCount; i++)
			{
				if (source[i] < lowest)  { lowest  = source[i]; }
				if (source[i] > highest) { highest = source[i]; }
			}
		}
		else
		{
			uint16_t const* source = static_cast<uint16_t const*>(indices);
			for (uint32_t i = 0; i < indexCount; i++)
			{
				if (source[i] < lowest)  { lowest  = source[i]; }
				if (source[i] > highest) { highest = source[i]; }
			}
		}

		uint32_t const vertexCount = highest - lowest + 1u;

		VkDeviceSize vertexOffset = 0;
		if (!UploadVertices(vertices, lowest, vertexCount, stride, vertexOffset))
		{
			return;
		}

		// Quads are expanded here rather than being drawn through the shared
		// quad index buffer, because that buffer describes consecutive vertices
		// and these do not have to be consecutive.
		uint32_t const emitted = asQuads ? (indexCount / 4u) * 6u : indexCount;
		if (emitted == 0)
		{
			return;
		}

		VkDeviceSize const indexBytes = static_cast<VkDeviceSize>(emitted) * sizeof(uint32_t);
		VkDeviceSize const indexOffset = (indexUsed + 3u) & ~static_cast<VkDeviceSize>(3u);

		if (indexOffset + indexBytes > indexBufferSize)
		{
			LogNote("Vulkan: per-frame index buffer exhausted; dropping a draw of %u indices.", indexCount);
			return;
		}

		uint32_t* out = reinterpret_cast<uint32_t*>(static_cast<uint8_t*>(indexMapped) + indexOffset);

		if (asQuads)
		{
			uint32_t const quads = indexCount / 4u;
			for (uint32_t quad = 0; quad < quads; quad++)
			{
				uint32_t corner[4];
				for (uint32_t c = 0; c < 4; c++)
				{
					uint32_t const at = quad * 4u + c;
					corner[c] = indicesAre32Bit
						? static_cast<uint32_t const*>(indices)[at]
						: static_cast<uint32_t>(static_cast<uint16_t const*>(indices)[at]);
				}

				out[quad * 6u + 0] = corner[0];
				out[quad * 6u + 1] = corner[1];
				out[quad * 6u + 2] = corner[2];
				out[quad * 6u + 3] = corner[0];
				out[quad * 6u + 4] = corner[2];
				out[quad * 6u + 5] = corner[3];
			}
		}
		else if (indicesAre32Bit)
		{
			memcpy(out, indices, static_cast<size_t>(indexBytes));
		}
		else
		{
			// Widened rather than kept narrow, so one buffer and one index type
			// serve every draw regardless of what the game sent.
			uint16_t const* source = static_cast<uint16_t const*>(indices);
			for (uint32_t i = 0; i < indexCount; i++)
			{
				out[i] = source[i];
			}
		}

		indexUsed = indexOffset + indexBytes;

		if (!BindDrawState(gdVertexFormat, topology, vertexOffset, layout.texCoordSets))
		{
			return;
		}

		vkCmdBindIndexBuffer(commandBuffer, indexBuffer, indexOffset, VK_INDEX_TYPE_UINT32);

		// The indices went in unchanged, so they still count from the start of
		// the game's array while the buffer holds only the slice from the
		// lowest one onward. A negative vertex offset closes that gap, which is
		// what the parameter is signed for.
		vkCmdDrawIndexed(commandBuffer, emitted, 1, 0, -static_cast<int32_t>(lowest), 0);
	}

	bool VulkanBackend::CreateRenderPass(void)
	{
		if (renderPass != VK_NULL_HANDLE)
		{
			return true;
		}

		VkAttachmentDescription colour{};
		colour.format         = swapchainFormat;
		colour.samples        = VK_SAMPLE_COUNT_1_BIT;

		// LOAD rather than CLEAR. The game issues its own Clear, which is a
		// transfer operation outside the pass, and a pass may be opened and
		// closed several times in one frame as draws and blits interleave.
		// Clearing on load would erase earlier work each time.
		colour.loadOp         = VK_ATTACHMENT_LOAD_OP_LOAD;
		colour.storeOp        = VK_ATTACHMENT_STORE_OP_STORE;
		colour.stencilLoadOp  = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
		colour.stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
		colour.initialLayout  = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
		colour.finalLayout    = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;

		VkAttachmentReference colourRef{};
		colourRef.attachment = 0;
		colourRef.layout     = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;

		// Depth loads and stores like the colour attachment, because the game
		// clears it explicitly and may open and close several passes per frame.
		VkAttachmentDescription depth{};
		depth.format         = depthFormat;
		depth.samples        = VK_SAMPLE_COUNT_1_BIT;
		depth.loadOp         = VK_ATTACHMENT_LOAD_OP_LOAD;
		depth.storeOp        = VK_ATTACHMENT_STORE_OP_STORE;
		depth.stencilLoadOp  = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
		depth.stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
		depth.initialLayout  = VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL;
		depth.finalLayout    = VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL;

		VkAttachmentReference depthRef{};
		depthRef.attachment = 1;
		depthRef.layout     = VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL;

		VkSubpassDescription subpass{};
		subpass.pipelineBindPoint       = VK_PIPELINE_BIND_POINT_GRAPHICS;
		subpass.colorAttachmentCount    = 1;
		subpass.pColorAttachments       = &colourRef;
		subpass.pDepthStencilAttachment = &depthRef;

		VkAttachmentDescription attachments[] = { colour, depth };

		VkRenderPassCreateInfo info{ VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO };
		info.attachmentCount = 2;
		info.pAttachments    = attachments;
		info.subpassCount    = 1;
		info.pSubpasses      = &subpass;

		VkResult result = vkCreateRenderPass(device, &info, nullptr, &renderPass);
		if (result != VK_SUCCESS)
		{
			Fail("vkCreateRenderPass", result);
			return false;
		}

		return true;
	}

	bool VulkanBackend::CreateDepthResources(void)
	{
		// D32 first, falling back to the packed depth-stencil format. One or
		// the other is guaranteed present, and the game only needs depth: its
		// stencil calls are recorded but not yet honoured.
		VkFormat const candidates[] = { VK_FORMAT_D32_SFLOAT, VK_FORMAT_D24_UNORM_S8_UINT, VK_FORMAT_D16_UNORM };

		depthFormat = VK_FORMAT_UNDEFINED;
		for (VkFormat candidate : candidates)
		{
			VkFormatProperties properties{};
			vkGetPhysicalDeviceFormatProperties(physicalDevice, candidate, &properties);

			if (properties.optimalTilingFeatures & VK_FORMAT_FEATURE_DEPTH_STENCIL_ATTACHMENT_BIT)
			{
				depthFormat = candidate;
				break;
			}
		}

		if (depthFormat == VK_FORMAT_UNDEFINED)
		{
			LogNote("Vulkan: no usable depth format; depth testing will be unavailable.");
			return false;
		}

		VkImageCreateInfo imageInfo{ VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO };
		imageInfo.imageType     = VK_IMAGE_TYPE_2D;
		imageInfo.format        = depthFormat;
		imageInfo.extent        = { swapchainExtent.width, swapchainExtent.height, 1 };
		imageInfo.mipLevels     = 1;
		imageInfo.arrayLayers   = 1;
		imageInfo.samples       = VK_SAMPLE_COUNT_1_BIT;
		imageInfo.tiling        = VK_IMAGE_TILING_OPTIMAL;
		// TRANSFER_DST for the game's depth clears, TRANSFER_SRC because it
		// also saves the depth buffer into a buffer region.
		imageInfo.usage         = VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT |
		                          VK_IMAGE_USAGE_TRANSFER_DST_BIT |
		                          VK_IMAGE_USAGE_TRANSFER_SRC_BIT;
		imageInfo.sharingMode   = VK_SHARING_MODE_EXCLUSIVE;
		imageInfo.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;

		VkResult result = vkCreateImage(device, &imageInfo, nullptr, &depthImage);
		if (result != VK_SUCCESS)
		{
			Fail("vkCreateImage (depth)", result);
			return false;
		}

		VkMemoryRequirements requirements{};
		vkGetImageMemoryRequirements(device, depthImage, &requirements);

		uint32_t typeIndex = 0;
		if (!FindMemoryType(requirements.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT, typeIndex))
		{
			LogNote("Vulkan: no device-local memory for the depth buffer.");
			return false;
		}

		VkMemoryAllocateInfo allocInfo{ VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO };
		allocInfo.allocationSize  = requirements.size;
		allocInfo.memoryTypeIndex = typeIndex;

		result = vkAllocateMemory(device, &allocInfo, nullptr, &depthMemory);
		if (result != VK_SUCCESS)
		{
			Fail("vkAllocateMemory (depth)", result);
			return false;
		}

		vkBindImageMemory(device, depthImage, depthMemory, 0);

		VkImageViewCreateInfo viewInfo{ VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO };
		viewInfo.image    = depthImage;
		viewInfo.viewType = VK_IMAGE_VIEW_TYPE_2D;
		viewInfo.format   = depthFormat;
		viewInfo.subresourceRange.aspectMask = VK_IMAGE_ASPECT_DEPTH_BIT;
		viewInfo.subresourceRange.levelCount = 1;
		viewInfo.subresourceRange.layerCount = 1;

		result = vkCreateImageView(device, &viewInfo, nullptr, &depthView);
		if (result != VK_SUCCESS)
		{
			Fail("vkCreateImageView (depth)", result);
			return false;
		}

		// The image is still UNDEFINED, and the render pass declares the
		// attachment layout as its initial one. Whichever comes first, the next
		// render pass or the game's next depth clear, moves it there; this only
		// records that the move is still owed.
		depthLayoutPending = true;

		LogNote("Vulkan: depth buffer ready, %ux%u, format %d.",
			swapchainExtent.width, swapchainExtent.height, static_cast<int>(depthFormat));
		return true;
	}

	void VulkanBackend::DestroyDepthResources(void)
	{
		if (depthView != VK_NULL_HANDLE)   { vkDestroyImageView(device, depthView, nullptr);   depthView = VK_NULL_HANDLE; }
		if (depthImage != VK_NULL_HANDLE)  { vkDestroyImage(device, depthImage, nullptr);      depthImage = VK_NULL_HANDLE; }
		if (depthMemory != VK_NULL_HANDLE) { vkFreeMemory(device, depthMemory, nullptr);       depthMemory = VK_NULL_HANDLE; }
	}

	void VulkanBackend::SetDepthState(bool test, bool write, uint32_t comparison)
	{
		depthTest    = test;
		depthWrite   = write;
		depthCompare = static_cast<uint8_t>(comparison);
	}

	void VulkanBackend::ClearDepth(float depth)
	{
		if (!EnsureFrame() || depthImage == VK_NULL_HANDLE)
		{
			return;
		}

		// A depth clear is a transfer operation, so it cannot run inside a
		// render pass any more than a colour clear can.
		EndRenderPassIfActive();

		VkImageMemoryBarrier barrier{ VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER };
		// Discarding the old contents is exactly what a clear does, so the
		// still-UNDEFINED first clear needs no special handling beyond
		// naming the layout the image is actually in.
		barrier.oldLayout           = depthLayoutPending
			? VK_IMAGE_LAYOUT_UNDEFINED
			: VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL;
		barrier.newLayout           = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
		depthLayoutPending          = false;
		barrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
		barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
		barrier.image               = depthImage;
		barrier.dstAccessMask       = VK_ACCESS_TRANSFER_WRITE_BIT;
		barrier.subresourceRange.aspectMask = VK_IMAGE_ASPECT_DEPTH_BIT;
		barrier.subresourceRange.levelCount = 1;
		barrier.subresourceRange.layerCount = 1;

		vkCmdPipelineBarrier(commandBuffer,
			VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT,
			0, 0, nullptr, 0, nullptr, 1, &barrier);

		VkClearDepthStencilValue value{};
		value.depth   = depth;
		value.stencil = 0;

		VkImageSubresourceRange range{};
		range.aspectMask = VK_IMAGE_ASPECT_DEPTH_BIT;
		range.levelCount = 1;
		range.layerCount = 1;

		vkCmdClearDepthStencilImage(commandBuffer, depthImage,
			VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, &value, 1, &range);

		barrier.oldLayout     = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
		barrier.newLayout     = VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL;
		barrier.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
		barrier.dstAccessMask = VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_READ_BIT | VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT;

		vkCmdPipelineBarrier(commandBuffer,
			VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_EARLY_FRAGMENT_TESTS_BIT,
			0, 0, nullptr, 0, nullptr, 1, &barrier);
	}

	void VulkanBackend::TransitionRegion(BufferRegion const& region, VkImageLayout from, VkImageLayout to)
	{
		VkAccessFlags        srcAccess = 0;
		VkAccessFlags        dstAccess = 0;
		VkPipelineStageFlags srcStage  = 0;
		VkPipelineStageFlags dstStage  = 0;

		LayoutAccess(from, srcAccess, srcStage);
		LayoutAccess(to, dstAccess, dstStage);

		VkImageMemoryBarrier barrier{ VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER };
		barrier.srcAccessMask       = srcAccess;
		barrier.dstAccessMask       = dstAccess;
		barrier.oldLayout           = from;
		barrier.newLayout           = to;
		barrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
		barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
		barrier.image               = region.image;
		barrier.subresourceRange.aspectMask = region.depth
			? VK_IMAGE_ASPECT_DEPTH_BIT
			: VK_IMAGE_ASPECT_COLOR_BIT;
		barrier.subresourceRange.levelCount = 1;
		barrier.subresourceRange.layerCount = 1;

		vkCmdPipelineBarrier(commandBuffer, srcStage, dstStage, 0, 0, nullptr, 0, nullptr, 1, &barrier);
	}

	uint32_t VulkanBackend::CreateBufferRegion(bool depth)
	{
		if (dead || device == VK_NULL_HANDLE || swapchainExtent.width == 0)
		{
			return 0;
		}

		if (depth && depthFormat == VK_FORMAT_UNDEFINED)
		{
			return 0;
		}

		BufferRegion region;
		region.depth  = depth;
		region.format = depth ? depthFormat : swapchainFormat;
		region.width  = swapchainExtent.width;
		region.height = swapchainExtent.height;

		VkImageCreateInfo imageInfo{ VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO };
		imageInfo.imageType     = VK_IMAGE_TYPE_2D;
		imageInfo.format        = region.format;
		imageInfo.extent        = { region.width, region.height, 1 };
		imageInfo.mipLevels     = 1;
		imageInfo.arrayLayers   = 1;
		imageInfo.samples       = VK_SAMPLE_COUNT_1_BIT;
		imageInfo.tiling        = VK_IMAGE_TILING_OPTIMAL;
		imageInfo.usage         = VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT;
		imageInfo.sharingMode   = VK_SHARING_MODE_EXCLUSIVE;
		imageInfo.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;

		VkResult result = vkCreateImage(device, &imageInfo, nullptr, &region.image);
		if (result != VK_SUCCESS)
		{
			Fail("vkCreateImage (buffer region)", result);
			return 0;
		}

		VkMemoryRequirements requirements{};
		vkGetImageMemoryRequirements(device, region.image, &requirements);

		uint32_t typeIndex = 0;
		if (!FindMemoryType(requirements.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT, typeIndex))
		{
			vkDestroyImage(device, region.image, nullptr);
			return 0;
		}

		VkMemoryAllocateInfo allocInfo{ VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO };
		allocInfo.allocationSize  = requirements.size;
		allocInfo.memoryTypeIndex = typeIndex;

		result = vkAllocateMemory(device, &allocInfo, nullptr, &region.memory);
		if (result != VK_SUCCESS)
		{
			// Not fatal, and not necessarily a bug: the game asks for regions
			// speculatively and copes with being refused.
			Fail("vkAllocateMemory (buffer region)", result);
			vkDestroyImage(device, region.image, nullptr);
			return 0;
		}

		vkBindImageMemory(device, region.image, region.memory, 0);
		region.live = true;

		// Reuse a dead slot before growing, so a game that cycles regions does
		// not walk the handle space upward forever.
		for (size_t i = 0; i < bufferRegions.size(); i++)
		{
			if (!bufferRegions[i].live)
			{
				bufferRegions[i] = region;
				return static_cast<uint32_t>(i + 1);
			}
		}

		bufferRegions.push_back(region);
		return static_cast<uint32_t>(bufferRegions.size());
	}

	bool VulkanBackend::IsBufferRegion(uint32_t handle) const
	{
		return handle != 0 && handle <= bufferRegions.size() && bufferRegions[handle - 1].live;
	}

	bool VulkanBackend::SaveBufferRegion(uint32_t handle, int32_t regionX, int32_t regionY,
		int32_t width, int32_t height, int32_t screenX, int32_t screenY)
	{
		if (!IsBufferRegion(handle) || width <= 0 || height <= 0 || !EnsureFrame())
		{
			return false;
		}

		BufferRegion& region = bufferRegions[handle - 1];

		// A copy is not a render pass operation, the same way a clear is not.
		EndRenderPassIfActive();

		// Both rectangles have to stay inside their image, and they share one
		// extent, so the smaller of the two limits governs.
		int32_t const maxWidth  = static_cast<int32_t>(swapchainExtent.width);
		int32_t const maxHeight = static_cast<int32_t>(swapchainExtent.height);

		if (regionX < 0 || regionY < 0 || screenX < 0 || screenY < 0)
		{
			return false;
		}

		width  = std::min(width,  std::min(maxWidth  - screenX, static_cast<int32_t>(region.width)  - regionX));
		height = std::min(height, std::min(maxHeight - screenY, static_cast<int32_t>(region.height) - regionY));

		if (width <= 0 || height <= 0)
		{
			return false;
		}

		VkImage       source      = region.depth ? depthImage : swapchainImages[imageIndex];
		VkImageLayout sourceLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;

		if (region.depth)
		{
			if (depthImage == VK_NULL_HANDLE)
			{
				return false;
			}

			VkImageMemoryBarrier toSource{ VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER };
			toSource.oldLayout           = depthLayoutPending
				? VK_IMAGE_LAYOUT_UNDEFINED
				: VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL;
			toSource.newLayout           = sourceLayout;
			toSource.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
			toSource.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
			toSource.image               = depthImage;
			toSource.srcAccessMask       = VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT;
			toSource.dstAccessMask       = VK_ACCESS_TRANSFER_READ_BIT;
			toSource.subresourceRange.aspectMask = VK_IMAGE_ASPECT_DEPTH_BIT;
			toSource.subresourceRange.levelCount = 1;
			toSource.subresourceRange.layerCount = 1;

			vkCmdPipelineBarrier(commandBuffer,
				VK_PIPELINE_STAGE_LATE_FRAGMENT_TESTS_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT,
				0, 0, nullptr, 0, nullptr, 1, &toSource);

			depthLayoutPending = false;
		}
		else
		{
			TransitionTo(sourceLayout);
		}

		TransitionRegion(region,
			region.written ? VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL : VK_IMAGE_LAYOUT_UNDEFINED,
			VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);

		VkImageCopy copy{};
		copy.srcSubresource.aspectMask = region.depth ? VK_IMAGE_ASPECT_DEPTH_BIT : VK_IMAGE_ASPECT_COLOR_BIT;
		copy.srcSubresource.layerCount = 1;
		copy.srcOffset = { screenX, screenY, 0 };
		copy.dstSubresource.aspectMask = copy.srcSubresource.aspectMask;
		copy.dstSubresource.layerCount = 1;
		copy.dstOffset = { regionX, regionY, 0 };
		copy.extent    = { static_cast<uint32_t>(width), static_cast<uint32_t>(height), 1 };

		vkCmdCopyImage(commandBuffer,
			source, sourceLayout,
			region.image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
			1, &copy);

		// Left ready to be read, since restoring is the only thing that
		// happens to a region after it has been saved.
		TransitionRegion(region, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL);
		region.written = true;

		if (region.depth)
		{
			VkImageMemoryBarrier back{ VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER };
			back.oldLayout           = sourceLayout;
			back.newLayout           = VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL;
			back.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
			back.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
			back.image               = depthImage;
			back.srcAccessMask       = VK_ACCESS_TRANSFER_READ_BIT;
			back.dstAccessMask       = VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_READ_BIT |
			                           VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT;
			back.subresourceRange.aspectMask = VK_IMAGE_ASPECT_DEPTH_BIT;
			back.subresourceRange.levelCount = 1;
			back.subresourceRange.layerCount = 1;

			vkCmdPipelineBarrier(commandBuffer,
				VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_EARLY_FRAGMENT_TESTS_BIT,
				0, 0, nullptr, 0, nullptr, 1, &back);
		}

		return true;
	}

	bool VulkanBackend::RestoreBufferRegion(uint32_t handle, int32_t regionX, int32_t regionY,
		int32_t width, int32_t height, int32_t screenX, int32_t screenY)
	{
		if (!IsBufferRegion(handle) || width <= 0 || height <= 0 || !EnsureFrame())
		{
			return false;
		}

		BufferRegion& region = bufferRegions[handle - 1];

		// Nothing has been saved yet, so there is nothing to put back. Copying
		// anyway would paint uninitialised memory over the frame.
		if (!region.written)
		{
			return false;
		}

		EndRenderPassIfActive();

		int32_t const maxWidth  = static_cast<int32_t>(swapchainExtent.width);
		int32_t const maxHeight = static_cast<int32_t>(swapchainExtent.height);

		if (regionX < 0 || regionY < 0 || screenX < 0 || screenY < 0)
		{
			return false;
		}

		width  = std::min(width,  std::min(maxWidth  - screenX, static_cast<int32_t>(region.width)  - regionX));
		height = std::min(height, std::min(maxHeight - screenY, static_cast<int32_t>(region.height) - regionY));

		if (width <= 0 || height <= 0)
		{
			return false;
		}

		VkImage destination = region.depth ? depthImage : swapchainImages[imageIndex];

		if (region.depth)
		{
			if (depthImage == VK_NULL_HANDLE)
			{
				return false;
			}

			VkImageMemoryBarrier toDest{ VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER };
			toDest.oldLayout           = VK_IMAGE_LAYOUT_UNDEFINED;
			toDest.newLayout           = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
			toDest.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
			toDest.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
			toDest.image               = depthImage;
			toDest.dstAccessMask       = VK_ACCESS_TRANSFER_WRITE_BIT;
			toDest.subresourceRange.aspectMask = VK_IMAGE_ASPECT_DEPTH_BIT;
			toDest.subresourceRange.levelCount = 1;
			toDest.subresourceRange.layerCount = 1;

			vkCmdPipelineBarrier(commandBuffer,
				VK_PIPELINE_STAGE_LATE_FRAGMENT_TESTS_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT,
				0, 0, nullptr, 0, nullptr, 1, &toDest);

			depthLayoutPending = false;
		}
		else
		{
			TransitionTo(VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
		}

		VkImageCopy copy{};
		copy.srcSubresource.aspectMask = region.depth ? VK_IMAGE_ASPECT_DEPTH_BIT : VK_IMAGE_ASPECT_COLOR_BIT;
		copy.srcSubresource.layerCount = 1;
		copy.srcOffset = { regionX, regionY, 0 };
		copy.dstSubresource.aspectMask = copy.srcSubresource.aspectMask;
		copy.dstSubresource.layerCount = 1;
		copy.dstOffset = { screenX, screenY, 0 };
		copy.extent    = { static_cast<uint32_t>(width), static_cast<uint32_t>(height), 1 };

		vkCmdCopyImage(commandBuffer,
			region.image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
			destination, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
			1, &copy);

		if (region.depth)
		{
			VkImageMemoryBarrier back{ VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER };
			back.oldLayout           = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
			back.newLayout           = VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL;
			back.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
			back.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
			back.image               = depthImage;
			back.srcAccessMask       = VK_ACCESS_TRANSFER_WRITE_BIT;
			back.dstAccessMask       = VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_READ_BIT |
			                           VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT;
			back.subresourceRange.aspectMask = VK_IMAGE_ASPECT_DEPTH_BIT;
			back.subresourceRange.levelCount = 1;
			back.subresourceRange.layerCount = 1;

			vkCmdPipelineBarrier(commandBuffer,
				VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_EARLY_FRAGMENT_TESTS_BIT,
				0, 0, nullptr, 0, nullptr, 1, &back);
		}

		return true;
	}

	void VulkanBackend::DestroyBufferRegion(uint32_t handle)
	{
		if (!IsBufferRegion(handle))
		{
			return;
		}

		BufferRegion& region = bufferRegions[handle - 1];

		if (region.image != VK_NULL_HANDLE)  { vkDestroyImage(device, region.image, nullptr); }
		if (region.memory != VK_NULL_HANDLE) { vkFreeMemory(device, region.memory, nullptr); }

		region = BufferRegion{};
	}

	void VulkanBackend::DestroyAllBufferRegions(void)
	{
		for (uint32_t i = 1; i <= bufferRegions.size(); i++)
		{
			DestroyBufferRegion(i);
		}

		bufferRegions.clear();
	}

	bool VulkanBackend::CreateFramebuffers(void)
	{
		swapchainImageViews.resize(swapchainImages.size());
		framebuffers.resize(swapchainImages.size());

		for (size_t i = 0; i < swapchainImages.size(); i++)
		{
			VkImageViewCreateInfo viewInfo{ VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO };
			viewInfo.image    = swapchainImages[i];
			viewInfo.viewType = VK_IMAGE_VIEW_TYPE_2D;
			viewInfo.format   = swapchainFormat;
			viewInfo.subresourceRange.aspectMask     = VK_IMAGE_ASPECT_COLOR_BIT;
			viewInfo.subresourceRange.baseMipLevel   = 0;
			viewInfo.subresourceRange.levelCount     = 1;
			viewInfo.subresourceRange.baseArrayLayer = 0;
			viewInfo.subresourceRange.layerCount     = 1;

			VkResult result = vkCreateImageView(device, &viewInfo, nullptr, &swapchainImageViews[i]);
			if (result != VK_SUCCESS)
			{
				Fail("vkCreateImageView", result);
				return false;
			}

			VkImageView attachments[] = { swapchainImageViews[i], depthView };

			VkFramebufferCreateInfo fbInfo{ VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO };
			fbInfo.renderPass      = renderPass;
			fbInfo.attachmentCount = 2;
			fbInfo.pAttachments    = attachments;
			fbInfo.width           = swapchainExtent.width;
			fbInfo.height          = swapchainExtent.height;
			fbInfo.layers          = 1;

			result = vkCreateFramebuffer(device, &fbInfo, nullptr, &framebuffers[i]);
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
		for (VkFramebuffer fb : framebuffers)
		{
			if (fb != VK_NULL_HANDLE) vkDestroyFramebuffer(device, fb, nullptr);
		}
		framebuffers.clear();

		for (VkImageView view : swapchainImageViews)
		{
			if (view != VK_NULL_HANDLE) vkDestroyImageView(device, view, nullptr);
		}
		swapchainImageViews.clear();
	}

	bool VulkanBackend::CreateShaderModules(void)
	{
		if (vertModules[0] != VK_NULL_HANDLE)
		{
			return true;
		}

		auto create = [this](uint32_t const* code, size_t words, VkShaderModule& out) -> bool
		{
			VkShaderModuleCreateInfo info{ VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO };
			info.codeSize = words * sizeof(uint32_t);
			info.pCode    = code;

			VkResult result = vkCreateShaderModule(device, &info, nullptr, &out);
			if (result != VK_SUCCESS)
			{
				Fail("vkCreateShaderModule", result);
				return false;
			}
			return true;
		};

		// Indexed by hasColour * 3 + texCoordSets, matching the order the
		// generated header declares them in.
		return create(kGeometryVertSpv_None,    _countof(kGeometryVertSpv_None),    vertModules[0])
			&& create(kGeometryVertSpv_Tex,     _countof(kGeometryVertSpv_Tex),     vertModules[1])
			&& create(kGeometryVertSpv_Tex2,    _countof(kGeometryVertSpv_Tex2),    vertModules[2])
			&& create(kGeometryVertSpv_Col,     _countof(kGeometryVertSpv_Col),     vertModules[3])
			&& create(kGeometryVertSpv_ColTex,  _countof(kGeometryVertSpv_ColTex),  vertModules[4])
			&& create(kGeometryVertSpv_ColTex2, _countof(kGeometryVertSpv_ColTex2), vertModules[5])
			&& create(kGeometryFragSpv,         _countof(kGeometryFragSpv),         fragModule);
	}

	bool VulkanBackend::CreatePipelineLayout(void)
	{
		if (pipelineLayout != VK_NULL_HANDLE)
		{
			return true;
		}

		// A mat4, the fragment state, the combiner network and the environment
		// colour, all by push constant: 112 bytes against a guaranteed minimum
		// of 128. No descriptor set, no uniform buffer, no per-frame
		// allocation.
		VkPushConstantRange range{};
		range.stageFlags = VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT;
		range.offset     = 0;
		range.size       = sizeof(float) * 28;

		// One set per texture stage, both with the same single-sampler layout.
		// Keeping them separate is what lets a descriptor set stay a property
		// of one texture: a single set with two bindings would need a set per
		// pair of textures instead, and the pairs multiply.
		VkDescriptorSetLayout const setLayouts[] = { descriptorLayout, descriptorLayout };

		VkPipelineLayoutCreateInfo info{ VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO };
		info.setLayoutCount         = _countof(setLayouts);
		info.pSetLayouts            = setLayouts;
		info.pushConstantRangeCount = 1;
		info.pPushConstantRanges    = &range;

		VkResult result = vkCreatePipelineLayout(device, &info, nullptr, &pipelineLayout);
		if (result != VK_SUCCESS)
		{
			Fail("vkCreatePipelineLayout", result);
			return false;
		}

		return true;
	}

	VkPipeline VulkanBackend::GetPipeline(PipelineKey const& key)
	{
		for (PipelineEntry const& entry : pipelines)
		{
			if (entry.key == key)
			{
				return entry.pipeline;
			}
		}

		VertexLayout const layout = DecodeVertexLayout(key.format);

		// A shader may not declare an input the pipeline does not supply, so
		// the variant has to match which attributes this format actually has.
		uint32_t const variant = (layout.hasColour ? 3u : 0u) + layout.texCoordSets;

		VkPipelineShaderStageCreateInfo stages[2]{};
		stages[0].sType  = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
		stages[0].stage  = VK_SHADER_STAGE_VERTEX_BIT;
		stages[0].module = vertModules[variant];
		stages[0].pName  = "main";
		stages[1].sType  = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
		stages[1].stage  = VK_SHADER_STAGE_FRAGMENT_BIT;
		stages[1].module = fragModule;
		stages[1].pName  = "main";

		VkVertexInputBindingDescription binding{};
		binding.binding   = 0;
		binding.stride    = layout.stride;
		binding.inputRate = VK_VERTEX_INPUT_RATE_VERTEX;

		// Position is the only attribute every format has. Colour and texture
		// coordinate are added at the offsets the format itself declares,
		// which is not the same across formats: V3F_C4UB_T2F puts the colour
		// at 12 and V3F_N3F_C4UB puts it at 24.
		VkVertexInputAttributeDescription attributes[4]{};
		uint32_t attributeCount = 0;

		attributes[attributeCount].location = 0;
		attributes[attributeCount].binding  = 0;
		attributes[attributeCount].format   = VK_FORMAT_R32G32B32_SFLOAT;
		attributes[attributeCount].offset   = 0;
		attributeCount++;

		if (layout.hasColour)
		{
			attributes[attributeCount].location = 1;
			attributes[attributeCount].binding  = 0;
			attributes[attributeCount].format   = VK_FORMAT_R8G8B8A8_UNORM;
			attributes[attributeCount].offset   = layout.colourOffset;
			attributeCount++;
		}

		for (uint32_t set = 0; set < layout.texCoordSets; set++)
		{
			attributes[attributeCount].location = 2 + set;
			attributes[attributeCount].binding  = 0;
			attributes[attributeCount].format   = VK_FORMAT_R32G32_SFLOAT;
			attributes[attributeCount].offset   = layout.texCoordOffset[set];
			attributeCount++;
		}

		VkPipelineVertexInputStateCreateInfo vertexInput{ VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO };
		vertexInput.vertexBindingDescriptionCount   = 1;
		vertexInput.pVertexBindingDescriptions      = &binding;
		vertexInput.vertexAttributeDescriptionCount = attributeCount;
		vertexInput.pVertexAttributeDescriptions    = attributes;

		VkPipelineInputAssemblyStateCreateInfo assembly{ VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO };
		assembly.topology = key.topology;

		VkPipelineViewportStateCreateInfo viewport{ VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO };
		viewport.viewportCount = 1;
		viewport.scissorCount  = 1;

		VkPipelineRasterizationStateCreateInfo raster{ VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO };
		raster.polygonMode = VK_POLYGON_MODE_FILL;

		// Culling off deliberately. The game's winding convention is not known
		// yet, and culling the wrong way round would hide the geometry we are
		// trying to see for the first time.
		raster.cullMode    = VK_CULL_MODE_NONE;
		raster.frontFace   = VK_FRONT_FACE_COUNTER_CLOCKWISE;
		raster.lineWidth   = 1.0f;

		VkPipelineMultisampleStateCreateInfo multisample{ VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO };
		multisample.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT;

		VkPipelineDepthStencilStateCreateInfo depthStencil{ VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO };
		depthStencil.depthTestEnable  = key.depthTest ? VK_TRUE : VK_FALSE;
		depthStencil.depthWriteEnable = key.depthWrite ? VK_TRUE : VK_FALSE;
		depthStencil.depthCompareOp   = MapCompareOp(key.depthCompare);
		depthStencil.minDepthBounds   = 0.0f;
		depthStencil.maxDepthBounds   = 1.0f;

		VkPipelineColorBlendAttachmentState blendAttachment{};
		blendAttachment.blendEnable         = key.blendEnable ? VK_TRUE : VK_FALSE;
		blendAttachment.srcColorBlendFactor = MapBlendFactor(key.srcFactor);
		blendAttachment.dstColorBlendFactor = MapBlendFactor(key.dstFactor);
		blendAttachment.colorBlendOp        = VK_BLEND_OP_ADD;
		blendAttachment.srcAlphaBlendFactor = MapBlendFactor(key.srcFactor);
		blendAttachment.dstAlphaBlendFactor = MapBlendFactor(key.dstFactor);
		blendAttachment.alphaBlendOp        = VK_BLEND_OP_ADD;
		blendAttachment.colorWriteMask =
			VK_COLOR_COMPONENT_R_BIT | VK_COLOR_COMPONENT_G_BIT |
			VK_COLOR_COMPONENT_B_BIT | VK_COLOR_COMPONENT_A_BIT;

		VkPipelineColorBlendStateCreateInfo blend{ VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO };
		blend.attachmentCount = 1;
		blend.pAttachments    = &blendAttachment;

		VkDynamicState dynamicStates[] = { VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR };
		VkPipelineDynamicStateCreateInfo dynamic{ VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO };
		dynamic.dynamicStateCount = static_cast<uint32_t>(_countof(dynamicStates));
		dynamic.pDynamicStates    = dynamicStates;

		VkGraphicsPipelineCreateInfo info{ VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO };
		info.stageCount          = 2;
		info.pStages             = stages;
		info.pVertexInputState   = &vertexInput;
		info.pInputAssemblyState = &assembly;
		info.pViewportState      = &viewport;
		info.pRasterizationState = &raster;
		info.pMultisampleState   = &multisample;
		info.pDepthStencilState  = &depthStencil;
		info.pColorBlendState    = &blend;
		info.pDynamicState       = &dynamic;
		info.layout              = pipelineLayout;
		info.renderPass          = renderPass;
		info.subpass             = 0;

		VkPipeline pipeline = VK_NULL_HANDLE;
		VkResult result = vkCreateGraphicsPipelines(device, VK_NULL_HANDLE, 1, &info, nullptr, &pipeline);
		if (result != VK_SUCCESS)
		{
			Fail("vkCreateGraphicsPipelines", result);
			return VK_NULL_HANDLE;
		}

		LogNote("Vulkan: created pipeline for format 0x%x (stride %u, colour %d, texcoord sets %u), topology %d, blend %d (%u,%u).",
			key.format, layout.stride, layout.hasColour ? 1 : 0, layout.texCoordSets, static_cast<int>(key.topology), key.blendEnable ? 1 : 0, key.srcFactor, key.dstFactor);

		pipelines.push_back({ key, pipeline });
		return pipeline;
	}

	void VulkanBackend::DestroyPipelines(void)
	{
		for (PipelineEntry const& entry : pipelines)
		{
			if (entry.pipeline != VK_NULL_HANDLE) vkDestroyPipeline(device, entry.pipeline, nullptr);
		}
		pipelines.clear();
	}

	void VulkanBackend::DestroySwapchain(void)
	{
		if (device == VK_NULL_HANDLE)
		{
			return;
		}

		vkDeviceWaitIdle(device);

		DestroyFramebuffers();
		DestroyDepthResources();

		// Regions are sized to the swapchain, so a resize invalidates them.
		// The game reallocates on its own once its old handles stop working.
		DestroyAllBufferRegions();

		if (swapchain != VK_NULL_HANDLE)
		{
			vkDestroySwapchainKHR(device, swapchain, nullptr);
			swapchain = VK_NULL_HANDLE;
		}

		swapchainImages.clear();
		frameActive      = false;
		renderPassActive = false;
		currentLayout    = VK_IMAGE_LAYOUT_UNDEFINED;
	}

	void VulkanBackend::Destroy(void)
	{
		if (device != VK_NULL_HANDLE)
		{
			vkDeviceWaitIdle(device);

			DestroySwapchain();
			DestroyPipelines();
			DestroyTextures();

			if (uploadFence != VK_NULL_HANDLE) { vkDestroyFence(device, uploadFence, nullptr); uploadFence = VK_NULL_HANDLE; }
			if (sampler != VK_NULL_HANDLE) { vkDestroySampler(device, sampler, nullptr); sampler = VK_NULL_HANDLE; }
			if (descriptorPool != VK_NULL_HANDLE) { vkDestroyDescriptorPool(device, descriptorPool, nullptr); descriptorPool = VK_NULL_HANDLE; }
			if (descriptorLayout != VK_NULL_HANDLE) { vkDestroyDescriptorSetLayout(device, descriptorLayout, nullptr); descriptorLayout = VK_NULL_HANDLE; }
			for (VkShaderModule& m : vertModules) { if (m != VK_NULL_HANDLE) { vkDestroyShaderModule(device, m, nullptr); m = VK_NULL_HANDLE; } }

			if (renderPass != VK_NULL_HANDLE) { vkDestroyRenderPass(device, renderPass, nullptr); renderPass = VK_NULL_HANDLE; }
			if (pipelineLayout != VK_NULL_HANDLE) { vkDestroyPipelineLayout(device, pipelineLayout, nullptr); pipelineLayout = VK_NULL_HANDLE; }
			
			if (fragModule != VK_NULL_HANDLE) { vkDestroyShaderModule(device, fragModule, nullptr); fragModule = VK_NULL_HANDLE; }

			if (vertexMapped != nullptr) { vkUnmapMemory(device, vertexMemory); vertexMapped = nullptr; }
			if (vertexMemory != VK_NULL_HANDLE) { vkFreeMemory(device, vertexMemory, nullptr); vertexMemory = VK_NULL_HANDLE; }
			if (vertexBuffer != VK_NULL_HANDLE) { vkDestroyBuffer(device, vertexBuffer, nullptr); vertexBuffer = VK_NULL_HANDLE; }

			if (indexMapped != nullptr) { vkUnmapMemory(device, indexMemory); indexMapped = nullptr; }
			if (indexMemory != VK_NULL_HANDLE) { vkFreeMemory(device, indexMemory, nullptr); indexMemory = VK_NULL_HANDLE; }
			if (indexBuffer != VK_NULL_HANDLE) { vkDestroyBuffer(device, indexBuffer, nullptr); indexBuffer = VK_NULL_HANDLE; }

			if (quadIndexMemory != VK_NULL_HANDLE) { vkFreeMemory(device, quadIndexMemory, nullptr); quadIndexMemory = VK_NULL_HANDLE; }
			if (quadIndexBuffer != VK_NULL_HANDLE) { vkDestroyBuffer(device, quadIndexBuffer, nullptr); quadIndexBuffer = VK_NULL_HANDLE; }

			if (readbackMapped != nullptr) { vkUnmapMemory(device, readbackMemory); readbackMapped = nullptr; }
			if (readbackMemory != VK_NULL_HANDLE) { vkFreeMemory(device, readbackMemory, nullptr); readbackMemory = VK_NULL_HANDLE; }
			if (readbackBuffer != VK_NULL_HANDLE) { vkDestroyBuffer(device, readbackBuffer, nullptr); readbackBuffer = VK_NULL_HANDLE; }

			if (stagingMapped != nullptr) { vkUnmapMemory(device, stagingMemory); stagingMapped = nullptr; }
			if (stagingMemory != VK_NULL_HANDLE) { vkFreeMemory(device, stagingMemory, nullptr); stagingMemory = VK_NULL_HANDLE; }
			if (stagingBuffer != VK_NULL_HANDLE) { vkDestroyBuffer(device, stagingBuffer, nullptr); stagingBuffer = VK_NULL_HANDLE; }

			if (inFlight != VK_NULL_HANDLE) { vkDestroyFence(device, inFlight, nullptr); inFlight = VK_NULL_HANDLE; }
			if (imageAvailable != VK_NULL_HANDLE) { vkDestroySemaphore(device, imageAvailable, nullptr); imageAvailable = VK_NULL_HANDLE; }
			if (renderFinished != VK_NULL_HANDLE) { vkDestroySemaphore(device, renderFinished, nullptr); renderFinished = VK_NULL_HANDLE; }
			if (commandPool != VK_NULL_HANDLE) { vkDestroyCommandPool(device, commandPool, nullptr); commandPool = VK_NULL_HANDLE; }

			vkDestroyDevice(device, nullptr);
			device = VK_NULL_HANDLE;
		}

		if (instance != VK_NULL_HANDLE)
		{
			if (surface != VK_NULL_HANDLE)
			{
				vkDestroySurfaceKHR(instance, surface, nullptr);
				surface = VK_NULL_HANDLE;
			}

			if (debugMessenger != VK_NULL_HANDLE && vkDestroyDebugUtilsMessengerEXT != nullptr)
			{
				vkDestroyDebugUtilsMessengerEXT(instance, debugMessenger, nullptr);
				debugMessenger = VK_NULL_HANDLE;
			}

			vkDestroyInstance(instance, nullptr);
			instance = VK_NULL_HANDLE;
		}
	}
}
