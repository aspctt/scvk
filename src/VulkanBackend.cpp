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
#include "ShaderBinaries.h"
#include "version.h"

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

		if (!CreateGeometryBuffers() || !CreateShaderModules() || !CreatePipelineLayout())
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
		info.imageUsage       = VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT;
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
		return CreateRenderPass() && CreateFramebuffers();
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

		VkViewport viewport{};
		viewport.x        = 0.0f;
		viewport.y        = 0.0f;
		viewport.width    = static_cast<float>(swapchainExtent.width);
		viewport.height   = static_cast<float>(swapchainExtent.height);
		viewport.minDepth = 0.0f;
		viewport.maxDepth = 1.0f;
		vkCmdSetViewport(commandBuffer, 0, 1, &viewport);

		VkRect2D scissor{};
		scissor.offset = { 0, 0 };
		scissor.extent = swapchainExtent;
		vkCmdSetScissor(commandBuffer, 0, 1, &scissor);
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

		constexpr VkDeviceSize kVertexBufferSize = 16u * 1024u * 1024u;

		if (!CreateHostBuffer(kVertexBufferSize, VK_BUFFER_USAGE_VERTEX_BUFFER_BIT,
				vertexBuffer, vertexMemory, vertexMapped))
		{
			return false;
		}

		vertexSize = kVertexBufferSize;
		vertexUsed = 0;

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

	void VulkanBackend::DrawVertices(uint32_t gdPrimType, uint32_t stride,
		void const* vertices, uint32_t firstVertex, uint32_t vertexCount)
	{
		if (vertices == nullptr || vertexCount == 0 || stride == 0)
		{
			return;
		}

		// The game's primitive numbering, from its own translation table:
		// 0 triangles, 1 triangle strip, 2 triangle fan, 3 points, 4 lines,
		// 5 line strip, 6 quads, 7 quad strip.
		VkPrimitiveTopology topology;
		bool asQuads = false;

		switch (gdPrimType)
		{
		case 0: topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;  break;
		case 1: topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_STRIP; break;
		case 2: topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_FAN;   break;
		case 3: topology = VK_PRIMITIVE_TOPOLOGY_POINT_LIST;     break;
		case 4: topology = VK_PRIMITIVE_TOPOLOGY_LINE_LIST;      break;
		case 5: topology = VK_PRIMITIVE_TOPOLOGY_LINE_STRIP;     break;
		case 6: topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST; asQuads = true; break;

		default:
			LogNote("Vulkan: primitive type %u is not handled; skipping the draw.", gdPrimType);
			return;
		}

		if (!EnsureFrame())
		{
			return;
		}

		VkDeviceSize bytes = static_cast<VkDeviceSize>(vertexCount) * stride;

		// Alignment so the binding offset stays legal for the attributes.
		VkDeviceSize offset = (vertexUsed + 15u) & ~static_cast<VkDeviceSize>(15u);

		if (offset + bytes > vertexSize)
		{
			LogNote("Vulkan: per-frame vertex buffer exhausted; dropping a draw of %u vertices.", vertexCount);
			return;
		}

		memcpy(static_cast<uint8_t*>(vertexMapped) + offset,
			static_cast<uint8_t const*>(vertices) + static_cast<size_t>(firstVertex) * stride,
			static_cast<size_t>(bytes));

		vertexUsed = offset + bytes;

		PipelineKey key{ stride, topology };
		VkPipeline pipeline = GetPipeline(key);
		if (pipeline == VK_NULL_HANDLE)
		{
			return;
		}

		BeginRenderPassIfNeeded();

		vkCmdBindPipeline(commandBuffer, VK_PIPELINE_BIND_POINT_GRAPHICS, pipeline);
		vkCmdPushConstants(commandBuffer, pipelineLayout, VK_SHADER_STAGE_VERTEX_BIT,
			0, sizeof(transform), transform);

		VkBuffer buffers[] = { vertexBuffer };
		VkDeviceSize offsets[] = { offset };
		vkCmdBindVertexBuffers(commandBuffer, 0, 1, buffers, offsets);

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

		VkSubpassDescription subpass{};
		subpass.pipelineBindPoint    = VK_PIPELINE_BIND_POINT_GRAPHICS;
		subpass.colorAttachmentCount = 1;
		subpass.pColorAttachments    = &colourRef;

		VkRenderPassCreateInfo info{ VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO };
		info.attachmentCount = 1;
		info.pAttachments    = &colour;
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

			VkFramebufferCreateInfo fbInfo{ VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO };
			fbInfo.renderPass      = renderPass;
			fbInfo.attachmentCount = 1;
			fbInfo.pAttachments    = &swapchainImageViews[i];
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
		if (vertModule != VK_NULL_HANDLE)
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

		return create(kGeometryVertSpv, _countof(kGeometryVertSpv), vertModule)
			&& create(kGeometryFragSpv, _countof(kGeometryFragSpv), fragModule);
	}

	bool VulkanBackend::CreatePipelineLayout(void)
	{
		if (pipelineLayout != VK_NULL_HANDLE)
		{
			return true;
		}

		// One mat4 by push constant. The guaranteed minimum push constant size
		// is 128 bytes, so 64 always fits and this needs no descriptor set,
		// no uniform buffer and no per-frame allocation.
		VkPushConstantRange range{};
		range.stageFlags = VK_SHADER_STAGE_VERTEX_BIT;
		range.offset     = 0;
		range.size       = sizeof(float) * 16;

		VkPipelineLayoutCreateInfo info{ VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO };
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

		VkPipelineShaderStageCreateInfo stages[2]{};
		stages[0].sType  = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
		stages[0].stage  = VK_SHADER_STAGE_VERTEX_BIT;
		stages[0].module = vertModule;
		stages[0].pName  = "main";
		stages[1].sType  = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
		stages[1].stage  = VK_SHADER_STAGE_FRAGMENT_BIT;
		stages[1].module = fragModule;
		stages[1].pName  = "main";

		VkVertexInputBindingDescription binding{};
		binding.binding   = 0;
		binding.stride    = key.stride;
		binding.inputRate = VK_VERTEX_INPUT_RATE_VERTEX;

		// Both of the game's formats put a float3 position at 0 and a packed
		// colour at 12; only the stride differs.
		VkVertexInputAttributeDescription attributes[2]{};
		attributes[0].location = 0;
		attributes[0].binding  = 0;
		attributes[0].format   = VK_FORMAT_R32G32B32_SFLOAT;
		attributes[0].offset   = 0;
		attributes[1].location = 1;
		attributes[1].binding  = 0;
		attributes[1].format   = VK_FORMAT_R8G8B8A8_UNORM;
		attributes[1].offset   = 12;

		VkPipelineVertexInputStateCreateInfo vertexInput{ VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO };
		vertexInput.vertexBindingDescriptionCount   = 1;
		vertexInput.pVertexBindingDescriptions      = &binding;
		vertexInput.vertexAttributeDescriptionCount = 2;
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

		VkPipelineColorBlendAttachmentState blendAttachment{};
		blendAttachment.blendEnable    = VK_FALSE;
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

		LogNote("Vulkan: created pipeline for stride %u, topology %d.",
			key.stride, static_cast<int>(key.topology));

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

			if (renderPass != VK_NULL_HANDLE) { vkDestroyRenderPass(device, renderPass, nullptr); renderPass = VK_NULL_HANDLE; }
			if (pipelineLayout != VK_NULL_HANDLE) { vkDestroyPipelineLayout(device, pipelineLayout, nullptr); pipelineLayout = VK_NULL_HANDLE; }
			if (vertModule != VK_NULL_HANDLE) { vkDestroyShaderModule(device, vertModule, nullptr); vertModule = VK_NULL_HANDLE; }
			if (fragModule != VK_NULL_HANDLE) { vkDestroyShaderModule(device, fragModule, nullptr); fragModule = VK_NULL_HANDLE; }

			if (vertexMapped != nullptr) { vkUnmapMemory(device, vertexMemory); vertexMapped = nullptr; }
			if (vertexMemory != VK_NULL_HANDLE) { vkFreeMemory(device, vertexMemory, nullptr); vertexMemory = VK_NULL_HANDLE; }
			if (vertexBuffer != VK_NULL_HANDLE) { vkDestroyBuffer(device, vertexBuffer, nullptr); vertexBuffer = VK_NULL_HANDLE; }

			if (quadIndexMemory != VK_NULL_HANDLE) { vkFreeMemory(device, quadIndexMemory, nullptr); quadIndexMemory = VK_NULL_HANDLE; }
			if (quadIndexBuffer != VK_NULL_HANDLE) { vkDestroyBuffer(device, quadIndexBuffer, nullptr); quadIndexBuffer = VK_NULL_HANDLE; }

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
