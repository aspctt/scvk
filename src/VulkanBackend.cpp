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
 *
 * The game draws into a back buffer of scvk's own rather than into the swapchain. It
 * keeps its contents from one frame to the next, which the game relies on: it redraws
 * only what changed, and a swapchain image holds whatever frame last used it, two or
 * three frames ago. Presenting copies the back buffer into the image just acquired, so
 * the frame's drawing never waits for the swapchain at all, only that last copy does.
 */

//// Dependencies

#include "VulkanBackend.h"
#include "Logger.h"
#include "Settings.h"
#include "Watchdog.h"
#include "version.h"

#include <windows.h>
#include <algorithm>
#include <stddef.h>
#include <stdio.h>
#include <string.h>

namespace scvk
{
	//// Constants

	namespace
	{
		constexpr char const* VALIDATION_LAYER = "VK_LAYER_KHRONOS_validation";

		// Switches on synchronisation validation, which is slow.
		constexpr char const* SYNC_VALIDATION_MARKER = "scvk-validate-sync";

		// Where the compiled pipelines are kept between sessions.
		constexpr char const* PIPELINE_CACHE_FILE = "scvk-pipelines.bin";

		// The waits that run on the game's main thread, which also pumps its window
		// messages, are bounded rather than UINT64_MAX. Blocking there indefinitely makes
		// the whole game unresponsive and unkillable except from Task Manager, so a frame
		// is dropped instead.
		constexpr uint64_t WAIT_TIMEOUT_NANOSECONDS = 2000ull * 1000ull * 1000ull;

		// An acquire waits at most this long for a swapchain image before the frame is
		// drawn without being shown.
		constexpr uint64_t ACQUIRE_TIMEOUT_NANOSECONDS = 1000ull * 1000ull * 1000ull;

		// A heartbeat line this many frames apart.
		constexpr uint64_t HEARTBEAT_FRAMES = 300;

		// A gap between presents long enough to see as a hitch. The game's own frame caps
		// go no lower than 15 a second, about 67 ms, so a capped frame never counts.
		constexpr int64_t SLOW_FRAME_MILLISECONDS = 100;

		// The smallest limits every Vulkan implementation has to accept: a viewport at
		// least 4096 wide and high, and bounds of at least -8192 to 8191.
		constexpr int32_t MAXIMUM_VIEWPORT_DIMENSION = 4096;
		constexpr int32_t VIEWPORT_BOUNDS_MINIMUM    = -8192;
		constexpr int32_t VIEWPORT_BOUNDS_MAXIMUM    = 8191;

		// The same validation hazard repeats every frame once synchronisation validation
		// is on, so each message ID is written a few times and then only counted.
		constexpr int      DEBUG_MESSAGE_IDS     = 64;
		constexpr uint32_t DEBUG_MESSAGE_REPEATS = 5;

		// The format of the back buffer. The game hands over BGRA8 pixels and Windows
		// always offers this for a swapchain, so blits and presenting copy with no
		// conversion.
		constexpr VkFormat BACK_BUFFER_FORMAT      = VK_FORMAT_B8G8R8A8_UNORM;
		constexpr VkFormat BACK_BUFFER_SRGB_FORMAT = VK_FORMAT_B8G8R8A8_SRGB;

		// A device-local heap the CPU can write counts as resizable BAR when it is at least
		// this large. The 256 MB window older systems expose is too small to give the
		// arenas.
		constexpr VkDeviceSize RESIZABLE_BAR_MINIMUM = 1024ull * 1024ull * 1024ull;

		// Sets each frame's transient descriptor pool holds: the shadow composites, the
		// depth handed to ReShade and the like, a few a frame.
		constexpr uint32_t TRANSIENT_SETS_PER_FRAME = 256;

		// While nothing is presented, a frame is held this long so a minimised game does
		// not spin a core drawing frames nobody sees.
		constexpr DWORD PAUSED_FRAME_MILLISECONDS = 10;
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

		/** Whether a physical device offers an extension. */
		bool HasDeviceExtension(VkPhysicalDevice physicalDevice, char const* wanted)
		{
			// List the extensions
			uint32_t count = 0;
			if (vkEnumerateDeviceExtensionProperties(physicalDevice, nullptr, &count, nullptr) != VK_SUCCESS || count == 0)
			{
				return false;
			}

			std::vector<VkExtensionProperties> extensions(count);
			if (vkEnumerateDeviceExtensionProperties(physicalDevice, nullptr, &count, extensions.data()) != VK_SUCCESS)
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
						LogWarn("Vulkan: message %s has now been reported %u times.", (data != nullptr && data->pMessageIdName != nullptr) ? data->pMessageIdName : "?", seen);
					}

					return VK_FALSE;
				}
			}

			// Write the message at the level its severity matches
			char const* const message = (data != nullptr && data->pMessage != nullptr) ? data->pMessage : "(no message)";

			if ((severity & VK_DEBUG_UTILS_MESSAGE_SEVERITY_ERROR_BIT_EXT) != 0)
			{
				LogError("Vulkan ERROR: %s", message);
			}
			else if ((severity & VK_DEBUG_UTILS_MESSAGE_SEVERITY_WARNING_BIT_EXT) != 0)
			{
				LogWarn("Vulkan warning: %s", message);
			}
			else
			{
				LogDebug("Vulkan info: %s", message);
			}

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

		/** The name of a present mode, for the log. */
		char const* PresentModeName(VkPresentModeKHR mode)
		{
			switch (mode)
			{
			case VK_PRESENT_MODE_IMMEDIATE_KHR:    return "immediate";
			case VK_PRESENT_MODE_MAILBOX_KHR:      return "mailbox";
			case VK_PRESENT_MODE_FIFO_KHR:         return "FIFO";
			case VK_PRESENT_MODE_FIFO_RELAXED_KHR: return "FIFO relaxed";
			default:                               return "other";
			}
		}

		/** Milliseconds since the system started, the clock the device recovery backs off by. */
		uint64_t TickMilliseconds(void)
		{
			return GetTickCount64();
		}
	}

	bool VulkanBackend::NoteDeviceLoss(VkResult result)
	{
		if (result != VK_ERROR_DEVICE_LOST)
		{
			return false;
		}

		// Stop recording at once and rebuild at the next frame boundary
		//
		// Nothing recorded into a lost device's command buffers will ever run, and every
		// object of it has to be destroyed and made again. The game carries on meanwhile,
		// as it would on a DirectX device it has to reset.
		if (!isDeviceLost)
		{
			LogError("Vulkan: the device was lost. scvk will rebuild it at the next frame; textures come back empty until the game uploads them again.");
			isDeviceLost = true;
		}

		isFrameActive = false;
		activePass    = ActivePass::None;
		commandBuffer = VK_NULL_HANDLE;
		return true;
	}

	void VulkanBackend::Fail(char const* what, VkResult result)
	{
		if (NoteDeviceLoss(result))
		{
			LogError("Vulkan: %s reported the device lost.", what);
			return;
		}

		if (isDead)
		{
			return;
		}

		LogError("Vulkan: %s failed with %s. Falling back to doing nothing; the game will keep running but nothing will be drawn.", what, VkResultName(result));
		isDead = true;
	}

	bool VulkanBackend::PickPhysicalDevice(void)
	{
		// List the devices
		uint32_t count = 0;
		VkResult result = vkEnumeratePhysicalDevices(instance, &count, nullptr);
		if (result != VK_SUCCESS || count == 0)
		{
			LogError("Vulkan: no physical devices reported.");
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

		// Prefer a discrete GPU, unless asked for the first one listed
		//
		// Laptops with two GPUs often list the integrated one first, and picking it would
		// work but would be a poor default for a game. -GPU:default on the command
		// line takes the first one instead, which is usually the one Windows picks.
		bool const shouldPreferDiscrete = GetSettings().shouldPreferHighPerformanceGpu;

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

			LogInfo("Vulkan: found device \"%s\" (type %d, vendor 0x%04X, device 0x%04X, API %u.%u.%u, %u memory allocations)", properties.deviceName, properties.deviceType, properties.vendorID, properties.deviceID, VK_VERSION_MAJOR(properties.apiVersion), VK_VERSION_MINOR(properties.apiVersion), VK_VERSION_PATCH(properties.apiVersion), properties.limits.maxMemoryAllocationCount);

			if (!shouldPreferDiscrete)
			{
				if (best == VK_NULL_HANDLE)
				{
					best = candidate;
				}

				continue;
			}

			if (score > bestScore)
			{
				bestScore = score;
				best = candidate;
			}
		}

		// Record what was chosen
		physicalDevice = best;
		vkGetPhysicalDeviceProperties(physicalDevice, &physicalDeviceProperties);
		deviceName = physicalDeviceProperties.deviceName;

		char version[32];
		sprintf_s(version, sizeof(version), "%u.%u.%u", VK_VERSION_MAJOR(physicalDeviceProperties.apiVersion), VK_VERSION_MINOR(physicalDeviceProperties.apiVersion), VK_VERSION_PATCH(physicalDeviceProperties.apiVersion));
		apiVersion = version;

		LogInfo("Vulkan: selected \"%s\" (%s).", deviceName.c_str(), shouldPreferDiscrete ? "high-performance preference" : "first device listed");

		// Record how far textures can go on it
		maximumMemoryAllocations = physicalDeviceProperties.limits.maxMemoryAllocationCount;

		// See whether the lit variant fits it
		//
		// Its vertex has 29 attributes and its vertex stage 22 outputs, above the 16 Vulkan
		// guarantees and below what every desktop driver offers. Without them the lighting
		// extension's lights stay per vertex.
		VkPhysicalDeviceLimits const& limits = physicalDeviceProperties.limits;
		isPerPixelLightingSupported = limits.maxVertexInputAttributes >= 29 && limits.maxVertexOutputComponents >= 88 && limits.maxFragmentInputComponents >= 88 && limits.maxVertexInputBindingStride >= sizeof(LitVertex) && limits.maxVertexInputAttributeOffset >= offsetof(LitVertex, lightColour[LIT_LIGHT_COUNT - 1]);
		if (!isPerPixelLightingSupported)
		{
			LogInfo("Vulkan: the device takes %u vertex attributes and %u vertex outputs; the lighting extension's lights are worked out per vertex.", limits.maxVertexInputAttributes, limits.maxVertexOutputComponents);
		}

		VkPhysicalDeviceMemoryProperties memory{};
		vkGetPhysicalDeviceMemoryProperties(physicalDevice, &memory);

		VkDeviceSize largestLocalHeap = 0;
		for (uint32_t i = 0; i < memory.memoryHeapCount; i++)
		{
			bool const isLocal = (memory.memoryHeaps[i].flags & VK_MEMORY_HEAP_DEVICE_LOCAL_BIT) != 0;
			if (isLocal && memory.memoryHeaps[i].size > largestLocalHeap)
			{
				largestLocalHeap = memory.memoryHeaps[i].size;
			}
		}

		// Choose where the per-frame arenas live
		//
		// Memory the CPU writes and the GPU reads in place. On the device, through a
		// resizable BAR or on an integrated GPU, the GPU reads every vertex at its own
		// speed; otherwise it reads them across the bus, which is what the plain
		// host-visible type gives.
		VkMemoryPropertyFlags const hostFlags   = VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT;
		VkMemoryPropertyFlags const deviceFlags = hostFlags | VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT;

		arenaMemoryType          = UINT32_MAX;
		isArenaMemoryDeviceLocal = false;

		for (uint32_t i = 0; i < memory.memoryTypeCount; i++)
		{
			VkMemoryType const& type = memory.memoryTypes[i];
			if ((type.propertyFlags & deviceFlags) == deviceFlags && memory.memoryHeaps[type.heapIndex].size >= RESIZABLE_BAR_MINIMUM)
			{
				arenaMemoryType          = i;
				isArenaMemoryDeviceLocal = true;
				break;
			}
		}

		if (arenaMemoryType == UINT32_MAX)
		{
			FindMemoryType(UINT32_MAX, hostFlags, arenaMemoryType);
		}

		LogInfo("Vulkan: %llu MB of device-local memory, at most %u memory allocations; per-frame geometry in %s memory.", largestLocalHeap / (1024u * 1024u), maximumMemoryAllocations, isArenaMemoryDeviceLocal ? "device-local" : "system");
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
			LogError("Vulkan: no queue family supports both graphics and presenting to this window.");
			isDead = true;
			return false;
		}

		// Create the device with the swapchain extension, and the fullscreen policy one
		// when the instance and the device both have what it needs
		float priority = 1.0f;
		VkDeviceQueueCreateInfo queueInformation{ VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO };
		queueInformation.queueFamilyIndex = queueFamily;
		queueInformation.queueCount       = 1;
		queueInformation.pQueuePriorities = &priority;

		std::vector<char const*> extensions{ VK_KHR_SWAPCHAIN_EXTENSION_NAME };

		hasFullscreenPolicyControl = canAskFullscreenPolicy && HasDeviceExtension(physicalDevice, VK_EXT_FULL_SCREEN_EXCLUSIVE_EXTENSION_NAME);
		if (hasFullscreenPolicyControl)
		{
			extensions.push_back(VK_EXT_FULL_SCREEN_EXCLUSIVE_EXTENSION_NAME);
		}

		// Wine and Proton do not offer the extension; there the window goes through the
		// Linux compositor, or straight to the display, as the Vulkan driver decides.
		LogInfo("Vulkan: %s", hasFullscreenPolicyControl ? "the driver will not take exclusive control of a fullscreen window." : "VK_EXT_full_screen_exclusive is not offered; the driver presents a fullscreen window as it sees fit.");

		VkDeviceCreateInfo deviceInformation{ VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO };
		deviceInformation.queueCreateInfoCount    = 1;
		deviceInformation.pQueueCreateInfos       = &queueInformation;
		deviceInformation.enabledExtensionCount   = static_cast<uint32_t>(extensions.size());
		deviceInformation.ppEnabledExtensionNames = extensions.data();
		deviceInformation.pEnabledFeatures        = nullptr;

		VkResult const result = vkCreateDevice(physicalDevice, &deviceInformation, nullptr, &device);
		if (result != VK_SUCCESS)
		{
			// A device that cannot be made is fatal even when it reports itself lost.
			LogError("Vulkan: vkCreateDevice failed with %s.", VkResultName(result));
			device = VK_NULL_HANDLE;
			if (!isRecoveringDevice)
			{
				isDead = true;
			}

			return false;
		}

		// Load its entry points and fetch the queue
		if (!LoadVulkanDeviceFunctions(device))
		{
			isDead = true;
			return false;
		}

		vkGetDeviceQueue(device, queueFamily, 0, &queue);
		LoadPipelineCache();
		return true;
	}

	void VulkanBackend::LoadPipelineCache(void)
	{
		if (pipelineCache != VK_NULL_HANDLE)
		{
			return;
		}

		// Read the file, when it was written for this very device and driver
		//
		// Vulkan checks the header itself and ignores data it cannot use, but some
		// drivers have crashed on caches from another version, so the header is compared
		// here first and a mismatch starts empty.
		std::vector<uint8_t> data;
		char path[MAX_PATH];

		if (LogFilePath(PIPELINE_CACHE_FILE, path, sizeof(path)))
		{
			FILE* file = nullptr;
			if (fopen_s(&file, path, "rb") == 0 && file != nullptr)
			{
				fseek(file, 0, SEEK_END);
				long const size = ftell(file);
				fseek(file, 0, SEEK_SET);

				if (size > 0 && size < 64L * 1024L * 1024L)
				{
					data.resize(static_cast<size_t>(size));
					if (fread(data.data(), 1, data.size(), file) != data.size())
					{
						data.clear();
					}
				}

				fclose(file);
			}
		}

		uint32_t const headerBytes = 16u + VK_UUID_SIZE;
		bool isUsable = data.size() >= headerBytes;

		if (isUsable)
		{
			uint32_t header[4];
			memcpy(header, data.data(), sizeof(header));

			isUsable = header[0] >= headerBytes && header[1] == VK_PIPELINE_CACHE_HEADER_VERSION_ONE && header[2] == physicalDeviceProperties.vendorID && header[3] == physicalDeviceProperties.deviceID && memcmp(data.data() + 16, physicalDeviceProperties.pipelineCacheUUID, VK_UUID_SIZE) == 0;
		}

		VkPipelineCacheCreateInfo information{ VK_STRUCTURE_TYPE_PIPELINE_CACHE_CREATE_INFO };
		information.initialDataSize = isUsable ? data.size() : 0;
		information.pInitialData    = isUsable ? data.data() : nullptr;

		if (vkCreatePipelineCache(device, &information, nullptr, &pipelineCache) != VK_SUCCESS)
		{
			pipelineCache = VK_NULL_HANDLE;
			return;
		}

		LogInfo("Vulkan: pipeline cache %s.", isUsable ? "loaded from the last session" : "started empty");
	}

	void VulkanBackend::SavePipelineCache(void)
	{
		if (pipelineCache == VK_NULL_HANDLE || device == VK_NULL_HANDLE || unsavedPipelines == 0)
		{
			return;
		}

		size_t size = 0;
		if (vkGetPipelineCacheData(device, pipelineCache, &size, nullptr) != VK_SUCCESS || size == 0)
		{
			return;
		}

		std::vector<uint8_t> data(size);
		if (vkGetPipelineCacheData(device, pipelineCache, &size, data.data()) != VK_SUCCESS)
		{
			return;
		}

		char path[MAX_PATH];
		if (!LogFilePath(PIPELINE_CACHE_FILE, path, sizeof(path)))
		{
			return;
		}

		FILE* file = nullptr;
		if (fopen_s(&file, path, "wb") != 0 || file == nullptr)
		{
			return;
		}

		fwrite(data.data(), 1, size, file);
		fclose(file);
		unsavedPipelines = 0;
	}

	bool VulkanBackend::CreateFrameResources(void)
	{
		// Each frame in flight gets its own command pool, reset as a whole once the GPU has
		// finished the frame, its fence, the semaphore its swapchain image arrives on, and
		// a pool for descriptor sets it uses once
		VkDescriptorPoolSize const transientSizes[] = {
			{ VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, TRANSIENT_SETS_PER_FRAME },
			{ VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, TRANSIENT_SETS_PER_FRAME * 4u },
		};

		for (FrameSlot& slot : frameSlots)
		{
			VkCommandPoolCreateInfo poolInformation{ VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO };
			poolInformation.flags            = VK_COMMAND_POOL_CREATE_TRANSIENT_BIT;
			poolInformation.queueFamilyIndex = queueFamily;

			VkResult result = vkCreateCommandPool(device, &poolInformation, nullptr, &slot.commandPool);
			if (result != VK_SUCCESS)
			{
				Fail("vkCreateCommandPool", result);
				return false;
			}

			VkFenceCreateInfo fenceInformation{ VK_STRUCTURE_TYPE_FENCE_CREATE_INFO };
			VkSemaphoreCreateInfo semaphoreInformation{ VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO };

			if (vkCreateFence(device, &fenceInformation, nullptr, &slot.fence) != VK_SUCCESS || vkCreateSemaphore(device, &semaphoreInformation, nullptr, &slot.imageAvailable) != VK_SUCCESS)
			{
				Fail("vkCreateFence", VK_ERROR_INITIALIZATION_FAILED);
				return false;
			}

			VkDescriptorPoolCreateInfo descriptorInformation{ VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO };
			descriptorInformation.maxSets       = TRANSIENT_SETS_PER_FRAME;
			descriptorInformation.poolSizeCount = _countof(transientSizes);
			descriptorInformation.pPoolSizes    = transientSizes;

			result = vkCreateDescriptorPool(device, &descriptorInformation, nullptr, &slot.transientPool);
			if (result != VK_SUCCESS)
			{
				Fail("vkCreateDescriptorPool (transient)", result);
				return false;
			}

			slot.commandBuffers.clear();
			slot.usedCommandBuffers = 0;
			slot.serial             = 0;
			slot.isFencePending     = false;
		}

		// And one pool for the commands run outside the frames
		VkCommandPoolCreateInfo utilityInformation{ VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO };
		utilityInformation.flags            = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
		utilityInformation.queueFamilyIndex = queueFamily;

		VkResult const result = vkCreateCommandPool(device, &utilityInformation, nullptr, &utilityCommandPool);
		if (result != VK_SUCCESS)
		{
			Fail("vkCreateCommandPool (utility)", result);
			return false;
		}

		currentSlot     = 0;
		completedSerial = frameSerial - 1;
		return true;
	}

	void VulkanBackend::DestroyFrameResources(void)
	{
		for (FrameSlot& slot : frameSlots)
		{
			FlushRetired(slot);

			if (slot.transientPool != VK_NULL_HANDLE)  { vkDestroyDescriptorPool(device, slot.transientPool, nullptr); slot.transientPool = VK_NULL_HANDLE; }
			if (slot.fence != VK_NULL_HANDLE)          { vkDestroyFence(device, slot.fence, nullptr); slot.fence = VK_NULL_HANDLE; }
			if (slot.imageAvailable != VK_NULL_HANDLE) { vkDestroySemaphore(device, slot.imageAvailable, nullptr); slot.imageAvailable = VK_NULL_HANDLE; }
			if (slot.commandPool != VK_NULL_HANDLE)    { vkDestroyCommandPool(device, slot.commandPool, nullptr); slot.commandPool = VK_NULL_HANDLE; }

			slot.commandBuffers.clear();
			slot.usedCommandBuffers = 0;
			slot.isFencePending     = false;
			slot.serial             = 0;
		}

		if (utilityCommandPool != VK_NULL_HANDLE)
		{
			vkDestroyCommandPool(device, utilityCommandPool, nullptr);
			utilityCommandPool = VK_NULL_HANDLE;
		}

		commandBuffer             = VK_NULL_HANDLE;
		uploadCommandBuffer       = VK_NULL_HANDLE;
		textureBatchCommandBuffer = VK_NULL_HANDLE;
	}

	void VulkanBackend::ChoosePresentMode(void)
	{
		presentMode = VK_PRESENT_MODE_FIFO_KHR;

		// FIFO is vsync, the only mode the spec guarantees, and what the game expects
		if (GetSettings().isVSyncEnabled)
		{
			return;
		}

		// Without vsync, mailbox shows the newest frame without tearing; immediate is the
		// fallback where that is all the driver offers
		uint32_t count = 0;
		vkGetPhysicalDeviceSurfacePresentModesKHR(physicalDevice, surface, &count, nullptr);

		std::vector<VkPresentModeKHR> modes(count);
		if (count > 0)
		{
			vkGetPhysicalDeviceSurfacePresentModesKHR(physicalDevice, surface, &count, modes.data());
		}

		bool const hasMailbox   = std::find(modes.begin(), modes.end(), VK_PRESENT_MODE_MAILBOX_KHR) != modes.end();
		bool const hasImmediate = std::find(modes.begin(), modes.end(), VK_PRESENT_MODE_IMMEDIATE_KHR) != modes.end();

		presentMode = hasMailbox ? VK_PRESENT_MODE_MAILBOX_KHR : (hasImmediate ? VK_PRESENT_MODE_IMMEDIATE_KHR : VK_PRESENT_MODE_FIFO_KHR);
	}

	bool VulkanBackend::CreateSwapchain(void)
	{
		// Check the surface allows transfers into its images
		//
		// TRANSFER_DST is what presenting needs, since it copies the back buffer in.
		// COLOR_ATTACHMENT is guaranteed and overlay layers (Steam, GPU vendor overlays,
		// ReShade) draw into the images with it.
		VkSurfaceCapabilitiesKHR capabilities{};
		VkResult result = vkGetPhysicalDeviceSurfaceCapabilitiesKHR(physicalDevice, surface, &capabilities);
		if (result != VK_SUCCESS)
		{
			if (!NoteDeviceLoss(result))
			{
				LogWarn("Vulkan: could not read the surface's capabilities (%s).", VkResultName(result));
			}

			return false;
		}

		if ((capabilities.supportedUsageFlags & VK_IMAGE_USAGE_TRANSFER_DST_BIT) == 0)
		{
			LogError("Vulkan: the surface does not allow swapchain images to be transfer destinations.");
			isDead = true;
			return false;
		}

		// Choose B8G8R8A8_UNORM, which the back buffer copies into unchanged
		uint32_t formatCount = 0;
		vkGetPhysicalDeviceSurfaceFormatsKHR(physicalDevice, surface, &formatCount, nullptr);
		if (formatCount == 0)
		{
			LogError("Vulkan: the surface reports no formats.");
			isDead = true;
			return false;
		}

		std::vector<VkSurfaceFormatKHR> formats(formatCount);
		vkGetPhysicalDeviceSurfaceFormatsKHR(physicalDevice, surface, &formatCount, formats.data());

		VkSurfaceFormatKHR chosen = formats[0];
		for (VkSurfaceFormatKHR const& candidate : formats)
		{
			if (candidate.format == BACK_BUFFER_FORMAT && candidate.colorSpace == VK_COLOR_SPACE_SRGB_NONLINEAR_KHR)
			{
				chosen = candidate;
				break;
			}
		}

		if (chosen.format != BACK_BUFFER_FORMAT)
		{
			LogWarn("Vulkan: B8G8R8A8_UNORM is unavailable for the window; presenting through format %d, converted as it is copied.", chosen.format);
		}

		// Size it to the window, which may differ from the render size
		VkExtent2D extent = capabilities.currentExtent;
		if (extent.width == UINT32_MAX)
		{
			extent.width  = renderWidth;
			extent.height = renderHeight;
		}

		if (extent.width == 0 || extent.height == 0)
		{
			LogDebug("Vulkan: the window has no area; presenting waits until it has.");
			return false;
		}

		// Take one image more than the minimum for the flip model, or the minimum for the
		// legacy one
		//
		// As SCD3D11 chooses between DXGI's swap effects: a window or borderless
		// fullscreen, which the desktop compositor shows, flips between its images, with
		// one more than the minimum so the next frame need not wait for the compositor to
		// release one. Exclusive fullscreen keeps the legacy model, with as few images as
		// the surface allows, as does any window under -FlipModel:off. The game draws into
		// the back buffer either way, so no swapchain image ever has to keep its contents.
		isFlipModel = GetSettings().isFlipModelPreferred && !isExclusiveFullscreen;

		uint32_t imageCount = capabilities.minImageCount + (isFlipModel ? 1u : 0u);
		if (capabilities.maxImageCount > 0 && imageCount > capabilities.maxImageCount)
		{
			imageCount = capabilities.maxImageCount;
		}

		ChoosePresentMode();

		// Create it
		VkSwapchainCreateInfoKHR information{ VK_STRUCTURE_TYPE_SWAPCHAIN_CREATE_INFO_KHR };
		information.surface          = surface;
		information.minImageCount    = imageCount;
		information.imageFormat      = chosen.format;
		information.imageColorSpace  = chosen.colorSpace;
		information.imageExtent      = extent;
		information.imageArrayLayers = 1;
		information.imageUsage       = VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT;
		information.imageSharingMode = VK_SHARING_MODE_EXCLUSIVE;
		information.preTransform     = capabilities.currentTransform;
		information.compositeAlpha   = VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR;
		information.presentMode      = presentMode;
		information.clipped          = VK_TRUE;
		information.oldSwapchain     = VK_NULL_HANDLE;

		// Keep the driver from taking exclusive control of the screen
		//
		// Left to decide, a driver may present a window that covers the monitor by taking
		// the display for itself and bypassing the desktop compositor. For an AMD player in
		// exclusive fullscreen, PrintScreen copied the desktop instead of the game, which is
		// what bypassing the compositor looks like. DXVK disallows it by default too, since
		// it blocks Alt+Tab and windows drawn over the game.
		VkSurfaceFullScreenExclusiveInfoEXT fullscreenPolicy{ VK_STRUCTURE_TYPE_SURFACE_FULL_SCREEN_EXCLUSIVE_INFO_EXT };
		fullscreenPolicy.fullScreenExclusive = VK_FULL_SCREEN_EXCLUSIVE_DISALLOWED_EXT;

		if (hasFullscreenPolicyControl)
		{
			information.pNext = &fullscreenPolicy;
		}

		result = vkCreateSwapchainKHR(device, &information, nullptr, &swapchain);
		if (result != VK_SUCCESS)
		{
			swapchain = VK_NULL_HANDLE;
			if (!NoteDeviceLoss(result))
			{
				LogWarn("Vulkan: vkCreateSwapchainKHR failed with %s; trying again next frame.", VkResultName(result));
			}

			return false;
		}

		swapchainFormat = chosen.format;
		swapchainExtent = extent;

		// Fetch its images, with a semaphore each for presenting them
		uint32_t actualCount = 0;
		vkGetSwapchainImagesKHR(device, swapchain, &actualCount, nullptr);
		swapchainImages.resize(actualCount);
		vkGetSwapchainImagesKHR(device, swapchain, &actualCount, swapchainImages.data());

		renderFinishedSemaphores.assign(actualCount, VK_NULL_HANDLE);
		for (VkSemaphore& semaphore : renderFinishedSemaphores)
		{
			VkSemaphoreCreateInfo semaphoreInformation{ VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO };
			if (vkCreateSemaphore(device, &semaphoreInformation, nullptr, &semaphore) != VK_SUCCESS)
			{
				semaphore = VK_NULL_HANDLE;
				DestroySwapchain();
				LogWarn("Vulkan: could not create the swapchain's semaphores.");
				return false;
			}
		}

		LogInfo("Vulkan: swapchain ready, %ux%u, %u images (%s), format %d, %s presentation; drawing at %ux%u.", extent.width, extent.height, actualCount, isFlipModel ? "flip model" : "legacy", chosen.format, PresentModeName(presentMode), renderWidth, renderHeight);
		return true;
	}

	bool VulkanBackend::CreateRenderPasses(void)
	{
		if (renderPass != VK_NULL_HANDLE)
		{
			return true;
		}

		// Order each pass after the passes and copies before it, and what follows after it
		//
		// The game opens and closes passes many times a frame as draws, clears and copies
		// interleave, and a pass that loads an attachment has to see what the last one
		// stored.
		VkSubpassDependency dependencies[2]{};
		dependencies[0].srcSubpass    = VK_SUBPASS_EXTERNAL;
		dependencies[0].dstSubpass    = 0;
		dependencies[0].srcStageMask  = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT | DEPTH_STAGES | VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT;
		dependencies[0].dstStageMask  = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT | DEPTH_STAGES | VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT;
		dependencies[0].srcAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT | VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT;
		dependencies[0].dstAccessMask = VK_ACCESS_COLOR_ATTACHMENT_READ_BIT | VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT | VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_READ_BIT | VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT | VK_ACCESS_SHADER_READ_BIT;

		dependencies[1]               = dependencies[0];
		dependencies[1].srcSubpass    = 0;
		dependencies[1].dstSubpass    = VK_SUBPASS_EXTERNAL;

		// Describe the colour attachment
		//
		// LOAD rather than CLEAR. The game issues its own Clear, and a pass may be opened
		// and closed several times in one frame as draws and copies interleave. Clearing
		// on load would erase earlier work each time.
		VkAttachmentDescription colour{};
		colour.format         = backBuffer.format;
		colour.samples        = VK_SAMPLE_COUNT_1_BIT;
		colour.loadOp         = VK_ATTACHMENT_LOAD_OP_LOAD;
		colour.storeOp        = VK_ATTACHMENT_STORE_OP_STORE;
		colour.stencilLoadOp  = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
		colour.stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
		colour.initialLayout  = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
		colour.finalLayout    = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;

		VkAttachmentReference colourReference{ 0, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL };

		// Describe the depth attachment, stencil included
		VkAttachmentDescription depth{};
		depth.format         = depthBuffer.format;
		depth.samples        = VK_SAMPLE_COUNT_1_BIT;
		depth.loadOp         = VK_ATTACHMENT_LOAD_OP_LOAD;
		depth.storeOp        = VK_ATTACHMENT_STORE_OP_STORE;
		depth.stencilLoadOp  = hasStencil ? VK_ATTACHMENT_LOAD_OP_LOAD : VK_ATTACHMENT_LOAD_OP_DONT_CARE;
		depth.stencilStoreOp = hasStencil ? VK_ATTACHMENT_STORE_OP_STORE : VK_ATTACHMENT_STORE_OP_DONT_CARE;
		depth.initialLayout  = VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL;
		depth.finalLayout    = VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL;

		VkAttachmentReference depthReference{ 1, VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL };

		// Create the main pass, colour and depth
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
		information.dependencyCount = _countof(dependencies);
		information.pDependencies   = dependencies;

		VkResult result = vkCreateRenderPass(device, &information, nullptr, &renderPass);
		if (result != VK_SUCCESS)
		{
			Fail("vkCreateRenderPass", result);
			return false;
		}

		// Create the colour-only pass, for the passes that sample the depth buffer
		VkSubpassDescription colourOnlySubpass{};
		colourOnlySubpass.pipelineBindPoint    = VK_PIPELINE_BIND_POINT_GRAPHICS;
		colourOnlySubpass.colorAttachmentCount = 1;
		colourOnlySubpass.pColorAttachments    = &colourReference;

		information.attachmentCount = 1;
		information.pAttachments    = &colour;
		information.pSubpasses      = &colourOnlySubpass;

		result = vkCreateRenderPass(device, &information, nullptr, &colourOnlyPass);
		if (result != VK_SUCCESS)
		{
			Fail("vkCreateRenderPass (colour only)", result);
			return false;
		}

		return true;
	}

	bool VulkanBackend::CreateRenderTargets(void)
	{
		if (backBuffer.image != VK_NULL_HANDLE)
		{
			return true;
		}

		// Create the back buffer
		//
		// Mutable, so ReShade can have an sRGB view of it, and sampled and copied for the
		// passes and the captures that read it.
		VkImageUsageFlags const colourUsage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_SAMPLED_BIT;
		if (!CreateRenderImage(BACK_BUFFER_FORMAT, renderWidth, renderHeight, colourUsage, VK_IMAGE_ASPECT_COLOR_BIT, BACK_BUFFER_SRGB_FORMAT, VK_IMAGE_CREATE_MUTABLE_FORMAT_BIT, backBuffer))
		{
			LogError("Vulkan: could not create the %ux%u back buffer.", renderWidth, renderHeight);
			return false;
		}

		// Create the depth buffer, sampled where the format allows
		if (!ChooseDepthFormat())
		{
			return false;
		}

		VkImageUsageFlags depthUsage = VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT;
		if (isDepthSampleable)
		{
			depthUsage |= VK_IMAGE_USAGE_SAMPLED_BIT;
		}

		if (!CreateRenderImage(depthBuffer.format, renderWidth, renderHeight, depthUsage, DepthAspects(), isDepthSampleable ? depthBuffer.format : VK_FORMAT_UNDEFINED, 0, depthBuffer))
		{
			LogError("Vulkan: could not create the depth buffer.");
			return false;
		}

		LogInfo("Vulkan: back buffer and depth buffer ready, %ux%u, depth format %d%s%s.", renderWidth, renderHeight, depthBuffer.format, hasStencil ? " with stencil" : "", isDepthSampleable ? ", sampleable" : "");

		// Create the passes and their framebuffers
		if (!CreateRenderPasses())
		{
			return false;
		}

		VkImageView const attachments[] = { backBuffer.view, depthBuffer.view };

		VkFramebufferCreateInfo framebufferInformation{ VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO };
		framebufferInformation.renderPass      = renderPass;
		framebufferInformation.attachmentCount = _countof(attachments);
		framebufferInformation.pAttachments    = attachments;
		framebufferInformation.width           = renderWidth;
		framebufferInformation.height          = renderHeight;
		framebufferInformation.layers          = 1;

		VkResult result = vkCreateFramebuffer(device, &framebufferInformation, nullptr, &framebuffer);
		if (result != VK_SUCCESS)
		{
			Fail("vkCreateFramebuffer", result);
			return false;
		}

		framebufferInformation.renderPass      = colourOnlyPass;
		framebufferInformation.attachmentCount = 1;

		result = vkCreateFramebuffer(device, &framebufferInformation, nullptr, &colourOnlyFramebuffer);
		if (result != VK_SUCCESS)
		{
			Fail("vkCreateFramebuffer (colour only)", result);
			return false;
		}

		return true;
	}

	void VulkanBackend::DestroyRenderTargets(void)
	{
		if (framebuffer != VK_NULL_HANDLE)           { vkDestroyFramebuffer(device, framebuffer, nullptr); framebuffer = VK_NULL_HANDLE; }
		if (colourOnlyFramebuffer != VK_NULL_HANDLE) { vkDestroyFramebuffer(device, colourOnlyFramebuffer, nullptr); colourOnlyFramebuffer = VK_NULL_HANDLE; }
		if (renderPass != VK_NULL_HANDLE)            { vkDestroyRenderPass(device, renderPass, nullptr); renderPass = VK_NULL_HANDLE; }
		if (colourOnlyPass != VK_NULL_HANDLE)        { vkDestroyRenderPass(device, colourOnlyPass, nullptr); colourOnlyPass = VK_NULL_HANDLE; }

		DestroyRenderImage(backBuffer);
		DestroyRenderImage(depthBuffer);
		activePass = ActivePass::None;
	}

	void VulkanBackend::DestroySwapchain(void)
	{
		if (device == VK_NULL_HANDLE)
		{
			return;
		}

		// Wait for the GPU, then destroy the swapchain alone
		//
		// The back buffer, the depth buffer and the buffer regions belong to the render
		// size, not to the window, so they all stay. The game never makes new regions
		// when its old handles stop working, and it relies on the back buffer keeping
		// what it drew.
		vkDeviceWaitIdle(device);

		if (swapchain != VK_NULL_HANDLE)
		{
			vkDestroySwapchainKHR(device, swapchain, nullptr);
			swapchain = VK_NULL_HANDLE;
		}

		// The semaphores presenting waited on go with it, so a new swapchain starts with
		// none still pending
		for (VkSemaphore semaphore : renderFinishedSemaphores)
		{
			if (semaphore != VK_NULL_HANDLE)
			{
				vkDestroySemaphore(device, semaphore, nullptr);
			}
		}

		renderFinishedSemaphores.clear();
		swapchainImages.clear();
	}

	bool VulkanBackend::RestoreSwapchain(void)
	{
		if (isDead || isDeviceLost || device == VK_NULL_HANDLE || surface == VK_NULL_HANDLE)
		{
			return false;
		}

		// Wait while the window has no area
		//
		// A minimised window reports a zero extent, and no swapchain can be made for it.
		// This runs every frame until the window is back, so it checks quietly.
		VkSurfaceCapabilitiesKHR capabilities{};
		VkResult const result = vkGetPhysicalDeviceSurfaceCapabilitiesKHR(physicalDevice, surface, &capabilities);

		if (result != VK_SUCCESS || capabilities.currentExtent.width == 0 || capabilities.currentExtent.height == 0)
		{
			NoteDeviceLoss(result);
			return false;
		}

		LogDebug("Vulkan: the window has area again; rebuilding the swapchain.");
		return CreateSwapchain();
	}

	void VulkanBackend::DestroyDevice(void)
	{
		if (device != VK_NULL_HANDLE)
		{
			// Let the GPU finish, then tell whoever holds objects of this device to let
			// them go while it still exists
			vkDeviceWaitIdle(device);

			if (beforeDeviceDestroyHook != nullptr)
			{
				beforeDeviceDestroyHook(beforeDeviceDestroyHookContext);
			}

			// Destroy what renders, then everything retired, then the textures and their
			// descriptors
			SavePipelineCache();

			isFrameActive = false;
			activePass    = ActivePass::None;
			commandBuffer = VK_NULL_HANDLE;

			DestroySwapchain();

			DestroyAllBufferRegions();
			DestroyEffectResources();
			DestroyRenderTargets();
			DestroyPipelines();

			frameSlots[0].retiredImages.insert(frameSlots[0].retiredImages.end(), pendingRetiredImages.begin(), pendingRetiredImages.end());
			pendingRetiredImages.clear();

			for (FrameSlot& slot : frameSlots)
			{
				FlushRetired(slot);
			}

			DestroyTextures();

			if (uploadFence != VK_NULL_HANDLE)       { vkDestroyFence(device, uploadFence, nullptr); uploadFence = VK_NULL_HANDLE; }
			if (textureBatchFence != VK_NULL_HANDLE) { vkDestroyFence(device, textureBatchFence, nullptr); textureBatchFence = VK_NULL_HANDLE; }

			for (SamplerEntry const& entry : samplers)
			{
				if (entry.sampler != VK_NULL_HANDLE) { vkDestroySampler(device, entry.sampler, nullptr); }
			}

			samplers.clear();

			if (pointClampSampler != VK_NULL_HANDLE)  { vkDestroySampler(device, pointClampSampler, nullptr); pointClampSampler = VK_NULL_HANDLE; }
			if (linearClampSampler != VK_NULL_HANDLE) { vkDestroySampler(device, linearClampSampler, nullptr); linearClampSampler = VK_NULL_HANDLE; }
			if (linearWrapSampler != VK_NULL_HANDLE)  { vkDestroySampler(device, linearWrapSampler, nullptr); linearWrapSampler = VK_NULL_HANDLE; }
			pointClampSet  = VK_NULL_HANDLE;
			linearClampSet = VK_NULL_HANDLE;
			linearWrapSet  = VK_NULL_HANDLE;

			if (samplerSetLayout != VK_NULL_HANDLE) { vkDestroyDescriptorSetLayout(device, samplerSetLayout, nullptr); samplerSetLayout = VK_NULL_HANDLE; }
			if (descriptorPool != VK_NULL_HANDLE)   { vkDestroyDescriptorPool(device, descriptorPool, nullptr); descriptorPool = VK_NULL_HANDLE; }
			if (imageSetLayout != VK_NULL_HANDLE)   { vkDestroyDescriptorSetLayout(device, imageSetLayout, nullptr); imageSetLayout = VK_NULL_HANDLE; }

			// Destroy the shaders and the layout
			for (VkShaderModule& module : vertexModules)
			{
				if (module != VK_NULL_HANDLE) { vkDestroyShaderModule(device, module, nullptr); module = VK_NULL_HANDLE; }
			}

			for (VkShaderModule& module : fragmentModules)
			{
				if (module != VK_NULL_HANDLE) { vkDestroyShaderModule(device, module, nullptr); module = VK_NULL_HANDLE; }
			}

			if (pipelineLayout != VK_NULL_HANDLE) { vkDestroyPipelineLayout(device, pipelineLayout, nullptr); pipelineLayout = VK_NULL_HANDLE; }

			// Destroy the buffers
			DestroyArena(vertexArena);
			DestroyArena(indexArena);
			DestroyArena(stagingArena);
			DestroyArena(uniformArena);
			drawRecordBuffer = VK_NULL_HANDLE;

			FreeDeviceMemory(quadIndexMemory);
			if (quadIndexBuffer != VK_NULL_HANDLE) { vkDestroyBuffer(device, quadIndexBuffer, nullptr); quadIndexBuffer = VK_NULL_HANDLE; }

			DestroyReadbackBuffer();

			// Destroy the frames, the cache, then the device
			DestroyFrameResources();

			if (pipelineCache != VK_NULL_HANDLE) { vkDestroyPipelineCache(device, pipelineCache, nullptr); pipelineCache = VK_NULL_HANDLE; }

			vkDestroyDevice(device, nullptr);
			device = VK_NULL_HANDLE;
			queue  = VK_NULL_HANDLE;
			liveMemoryAllocations = 0;
		}

		// Destroy the window's surface
		if (surface != VK_NULL_HANDLE && instance != VK_NULL_HANDLE)
		{
			vkDestroySurfaceKHR(instance, surface, nullptr);
			surface = VK_NULL_HANDLE;
		}

		bound = BindingCache{};
	}

	bool VulkanBackend::RecoverDevice(void)
	{
		LogInfo("Vulkan: rebuilding the device (attempt %u).", deviceRecoveryFailures + 1);

		// Keep what the textures and regions were, by handle
		//
		// The game holds on to its texture names and region handles, and never asks for
		// new ones after a reset. They come back with their sizes and formats but empty,
		// until the game uploads them again, as SCD3D11 does after losing its device.
		std::vector<Texture> previousTextures = textures;
		std::vector<BufferRegion> previousRegions = bufferRegions;
		uint32_t const previousTexture  = currentTexture;
		uint32_t const previousTexture1 = currentTexture1;

		// Destroy everything, then make it again
		DestroyDevice();

		if (!CreateSurfaceAndDevice(windowHandle, renderWidth, renderHeight))
		{
			// Keep the descriptions for the next attempt
			textures      = previousTextures;
			bufferRegions = previousRegions;
			for (Texture& texture : textures)
			{
				texture.image      = VK_NULL_HANDLE;
				texture.view       = VK_NULL_HANDLE;
				texture.descriptor = VK_NULL_HANDLE;
				texture.memory     = ImageMemory{};
			}

			for (BufferRegion& region : bufferRegions)
			{
				region.image  = VK_NULL_HANDLE;
				region.memory = VK_NULL_HANDLE;
			}

			return false;
		}

		// Rebuild the textures under their old handles
		uint32_t rebuilt = 0;
		textures.resize(std::max(textures.size(), previousTextures.size()));

		for (size_t handle = 1; handle < previousTextures.size(); handle++)
		{
			Texture& texture = textures[handle];
			texture = Texture{};

			Texture const& previous = previousTextures[handle];
			if (previous.isReserved)
			{
				texture.isReserved = true;
				continue;
			}

			if (!previous.isLive)
			{
				continue;
			}

			texture.format         = previous.format;
			texture.width          = previous.width;
			texture.height         = previous.height;
			texture.levels         = previous.levels;
			texture.isCompressed   = previous.isCompressed;
			texture.internalFormat = previous.internalFormat;

			if (CreateTextureObjects(texture))
			{
				rebuilt++;
			}
		}

		// Rebuild the regions under their old handles
		bufferRegions.resize(previousRegions.size());
		for (size_t index = 0; index < previousRegions.size(); index++)
		{
			bufferRegions[index] = BufferRegion{};
			if (previousRegions[index].isLive)
			{
				AllocateRegionImage(previousRegions[index].isDepth, bufferRegions[index]);
			}
		}

		SetTexture(previousTexture);
		SetTexture1(previousTexture1);
		LogInfo("Vulkan: the device is back; %u textures and %zu regions recreated empty.", rebuilt, bufferRegions.size());
		return true;
	}

	bool VulkanBackend::TryRecoverDevice(void)
	{
		if (!isDeviceLost)
		{
			return !isDead;
		}

		if (isDead || isRecoveringDevice)
		{
			return false;
		}

		// Back off between attempts, from half a second to eight
		uint64_t const now = TickMilliseconds();
		if (now < nextDeviceRecoveryTick)
		{
			return false;
		}

		isRecoveringDevice = true;
		bool const isRecovered = RecoverDevice();
		isRecoveringDevice = false;

		if (isRecovered)
		{
			isDeviceLost           = false;
			deviceRecoveryFailures = 0;
			nextDeviceRecoveryTick = 0;
			return true;
		}

		if (deviceRecoveryFailures < 5)
		{
			deviceRecoveryFailures++;
		}

		uint64_t const delay = 250ull << deviceRecoveryFailures;
		nextDeviceRecoveryTick = now + delay;
		LogWarn("Vulkan: rebuilding the device failed; trying again in %llu ms.", delay);
		return false;
	}

	bool VulkanBackend::FindMemoryType(uint32_t typeBits, VkMemoryPropertyFlags properties, uint32_t& outIndex) const
	{
		VkPhysicalDeviceMemoryProperties memory{};
		vkGetPhysicalDeviceMemoryProperties(physicalDevice, &memory);

		for (uint32_t i = 0; i < memory.memoryTypeCount; i++)
		{
			bool const isTypeAllowed = (typeBits & (1u << i)) != 0;
			bool const hasProperties = (memory.memoryTypes[i].propertyFlags & properties) == properties;

			if (isTypeAllowed && hasProperties)
			{
				outIndex = i;
				return true;
			}
		}

		return false;
	}

	VkResult VulkanBackend::AllocateDeviceMemory(VkMemoryAllocateInfo const& information, VkDeviceMemory& outMemory)
	{
		VkResult const result = vkAllocateMemory(device, &information, nullptr, &outMemory);
		if (result == VK_SUCCESS)
		{
			liveMemoryAllocations++;
		}
		else
		{
			outMemory = VK_NULL_HANDLE;
			NoteDeviceLoss(result);
		}

		return result;
	}

	void VulkanBackend::FreeDeviceMemory(VkDeviceMemory& memory)
	{
		if (memory == VK_NULL_HANDLE)
		{
			return;
		}

		vkFreeMemory(device, memory, nullptr);
		memory = VK_NULL_HANDLE;
		liveMemoryAllocations--;
	}

	bool VulkanBackend::CreateHostBuffer(VkDeviceSize size, VkBufferUsageFlags usage, VkBuffer& outBuffer, VkDeviceMemory& outMemory, void*& outMapped)
	{
		outBuffer = VK_NULL_HANDLE;
		outMemory = VK_NULL_HANDLE;
		outMapped = nullptr;

		// Create the buffer
		VkBufferCreateInfo bufferInformation{ VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO };
		bufferInformation.size        = size;
		bufferInformation.usage       = usage;
		bufferInformation.sharingMode = VK_SHARING_MODE_EXCLUSIVE;

		VkResult result = vkCreateBuffer(device, &bufferInformation, nullptr, &outBuffer);
		if (result != VK_SUCCESS)
		{
			outBuffer = VK_NULL_HANDLE;
			NoteDeviceLoss(result);
			LogWarn("Vulkan: could not create a %llu byte buffer (%s).", size, VkResultName(result));
			return false;
		}

		// Back it with memory the CPU writes, on the device when that is fast
		//
		// Running short is not fatal: the caller drops what it was going to put there, and
		// the frame carries on.
		VkMemoryRequirements requirements{};
		vkGetBufferMemoryRequirements(device, outBuffer, &requirements);

		uint32_t typeIndex = arenaMemoryType;
		if (typeIndex == UINT32_MAX || (requirements.memoryTypeBits & (1u << typeIndex)) == 0)
		{
			if (!FindMemoryType(requirements.memoryTypeBits, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT, typeIndex))
			{
				LogError("Vulkan: no host-visible coherent memory type available.");
				vkDestroyBuffer(device, outBuffer, nullptr);
				outBuffer = VK_NULL_HANDLE;
				isDead = true;
				return false;
			}
		}

		VkMemoryAllocateInfo allocationInformation{ VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO };
		allocationInformation.allocationSize  = requirements.size;
		allocationInformation.memoryTypeIndex = typeIndex;

		result = AllocateDeviceMemory(allocationInformation, outMemory);

		// A device-local type can run out where system memory does not
		if (result != VK_SUCCESS && typeIndex == arenaMemoryType && isArenaMemoryDeviceLocal && !isDeviceLost)
		{
			uint32_t systemType = 0;
			if (FindMemoryType(requirements.memoryTypeBits & ~(1u << typeIndex), VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT, systemType))
			{
				allocationInformation.memoryTypeIndex = systemType;
				result = AllocateDeviceMemory(allocationInformation, outMemory);
			}
		}

		if (result != VK_SUCCESS)
		{
			LogWarn("Vulkan: could not allocate %llu bytes of host-visible memory (%s).", requirements.size, VkResultName(result));
			vkDestroyBuffer(device, outBuffer, nullptr);
			outBuffer = VK_NULL_HANDLE;
			return false;
		}

		result = vkBindBufferMemory(device, outBuffer, outMemory, 0);

		// Map it for its whole life
		if (result == VK_SUCCESS)
		{
			result = vkMapMemory(device, outMemory, 0, requirements.size, 0, &outMapped);
		}

		if (result != VK_SUCCESS)
		{
			LogWarn("Vulkan: could not map a %llu byte buffer (%s).", size, VkResultName(result));
			NoteDeviceLoss(result);
			FreeDeviceMemory(outMemory);
			vkDestroyBuffer(device, outBuffer, nullptr);
			outBuffer = VK_NULL_HANDLE;
			outMapped = nullptr;
			return false;
		}

		return true;
	}

	bool VulkanBackend::CreateRenderImage(VkFormat format, uint32_t width, uint32_t height, VkImageUsageFlags usage, VkImageAspectFlags viewAspect, VkFormat secondViewFormat, VkImageCreateFlags flags, RenderImage& outImage)
	{
		RenderImage image;
		image.format = format;
		image.width  = width;
		image.height = height;

		// Create the image
		VkImageCreateInfo imageInformation{ VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO };
		imageInformation.flags         = flags;
		imageInformation.imageType     = VK_IMAGE_TYPE_2D;
		imageInformation.format        = format;
		imageInformation.extent        = { width, height, 1 };
		imageInformation.mipLevels     = 1;
		imageInformation.arrayLayers   = 1;
		imageInformation.samples       = VK_SAMPLE_COUNT_1_BIT;
		imageInformation.tiling        = VK_IMAGE_TILING_OPTIMAL;
		imageInformation.usage         = usage;
		imageInformation.sharingMode   = VK_SHARING_MODE_EXCLUSIVE;
		imageInformation.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;

		VkResult result = vkCreateImage(device, &imageInformation, nullptr, &image.image);
		if (result != VK_SUCCESS)
		{
			NoteDeviceLoss(result);
			return false;
		}

		// Back it with device-local memory of its own
		VkMemoryRequirements requirements{};
		vkGetImageMemoryRequirements(device, image.image, &requirements);

		uint32_t typeIndex = 0;
		if (!FindMemoryType(requirements.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT, typeIndex))
		{
			vkDestroyImage(device, image.image, nullptr);
			return false;
		}

		VkMemoryAllocateInfo allocationInformation{ VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO };
		allocationInformation.allocationSize  = requirements.size;
		allocationInformation.memoryTypeIndex = typeIndex;

		result = AllocateDeviceMemory(allocationInformation, image.memory);
		if (result == VK_SUCCESS)
		{
			result = vkBindImageMemory(device, image.image, image.memory, 0);
		}

		// Create its views: the one rendering uses, and the second one when asked for,
		// another format of a colour image or the depth alone of a depth image
		VkImageViewCreateInfo viewInformation{ VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO };
		viewInformation.image    = image.image;
		viewInformation.viewType = VK_IMAGE_VIEW_TYPE_2D;
		viewInformation.format   = format;
		viewInformation.subresourceRange.aspectMask = viewAspect;
		viewInformation.subresourceRange.levelCount = 1;
		viewInformation.subresourceRange.layerCount = 1;

		if (result == VK_SUCCESS)
		{
			result = vkCreateImageView(device, &viewInformation, nullptr, &image.view);
		}

		if (result == VK_SUCCESS && secondViewFormat != VK_FORMAT_UNDEFINED)
		{
			bool const isDepthImage = (viewAspect & VK_IMAGE_ASPECT_DEPTH_BIT) != 0;
			viewInformation.format = secondViewFormat;
			viewInformation.subresourceRange.aspectMask = isDepthImage ? VkImageAspectFlags{ VK_IMAGE_ASPECT_DEPTH_BIT } : viewAspect;

			// The second view is a convenience; the image works without it
			if (vkCreateImageView(device, &viewInformation, nullptr, &image.secondView) != VK_SUCCESS)
			{
				image.secondView = VK_NULL_HANDLE;
			}
		}

		if (result != VK_SUCCESS)
		{
			NoteDeviceLoss(result);
			DestroyRenderImage(image);
			return false;
		}

		outImage = image;
		return true;
	}

	void VulkanBackend::DestroyRenderImage(RenderImage& image)
	{
		if (device != VK_NULL_HANDLE)
		{
			if (image.secondView != VK_NULL_HANDLE) { vkDestroyImageView(device, image.secondView, nullptr); }
			if (image.view != VK_NULL_HANDLE)       { vkDestroyImageView(device, image.view, nullptr); }
			if (image.image != VK_NULL_HANDLE)      { vkDestroyImage(device, image.image, nullptr); }
			FreeDeviceMemory(image.memory);
		}

		image = RenderImage{};
	}

	void VulkanBackend::RetireRenderImage(RenderImage& image)
	{
		if (image.image == VK_NULL_HANDLE)
		{
			image = RenderImage{};
			return;
		}

		// Destroyed once the GPU has finished the frame being recorded, and with it
		// everything before it
		RetiredImage retired;
		retired.image         = image.image;
		retired.memory.memory = image.memory;
		retired.view          = image.view;
		retired.secondView    = image.secondView;

		Retire(retired);
		image = RenderImage{};
	}

	void VulkanBackend::Retire(RetiredImage const& retired)
	{
		if (isFrameActive)
		{
			frameSlots[currentSlot].retiredImages.push_back(retired);
		}
		else
		{
			pendingRetiredImages.push_back(retired);
		}
	}

	int32_t VulkanBackend::RenderWidth(void) const
	{
		// Render sizes are far below INT32_MAX, so the conversion loses nothing.
		return static_cast<int32_t>(renderWidth);
	}

	int32_t VulkanBackend::RenderHeight(void) const
	{
		// Render sizes are far below INT32_MAX, so the conversion loses nothing.
		return static_cast<int32_t>(renderHeight);
	}

	bool VulkanBackend::WaitForFrame(uint64_t serial)
	{
		if (serial == 0 || serial <= completedSerial)
		{
			return true;
		}

		// Find the slot that holds that frame's fence
		//
		// A slot reused since then means the frame was waited for before the reuse.
		FrameSlot* found = nullptr;
		for (FrameSlot& candidate : frameSlots)
		{
			if (candidate.serial == serial && candidate.isFencePending)
			{
				found = &candidate;
			}
		}

		if (found == nullptr)
		{
			completedSerial = std::max(completedSerial, serial);
			return true;
		}

		FrameSlot& slot = *found;

		VkResult const result = WaitForFence(slot.fence, WAIT_TIMEOUT_NANOSECONDS);
		if (result == VK_TIMEOUT)
		{
			LogWarn("Vulkan: timed out waiting for frame %llu; skipping work that needed it.", serial);
			return false;
		}

		if (result != VK_SUCCESS)
		{
			Fail("vkWaitForFences", result);
			return false;
		}

		// A fence signals once everything submitted before it has finished too
		slot.isFencePending = false;
		completedSerial     = std::max(completedSerial, serial);
		return true;
	}

	bool VulkanBackend::BeginCommandBuffer(void)
	{
		FrameSlot& slot = frameSlots[currentSlot];

		// Take the next of the slot's command buffers, allocating one when they are all used
		if (slot.usedCommandBuffers == slot.commandBuffers.size())
		{
			VkCommandBufferAllocateInfo allocationInformation{ VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO };
			allocationInformation.commandPool        = slot.commandPool;
			allocationInformation.level              = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
			allocationInformation.commandBufferCount = 1;

			VkCommandBuffer allocated = VK_NULL_HANDLE;
			VkResult const result = vkAllocateCommandBuffers(device, &allocationInformation, &allocated);
			if (result != VK_SUCCESS)
			{
				Fail("vkAllocateCommandBuffers", result);
				return false;
			}

			slot.commandBuffers.push_back(allocated);
		}

		commandBuffer = slot.commandBuffers[slot.usedCommandBuffers++];

		VkCommandBufferBeginInfo beginInformation{ VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO };
		beginInformation.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;

		VkResult const result = vkBeginCommandBuffer(commandBuffer, &beginInformation);
		if (result != VK_SUCCESS)
		{
			Fail("vkBeginCommandBuffer", result);
			commandBuffer = VK_NULL_HANDLE;
			return false;
		}

		// A new command buffer has nothing bound
		InvalidateBindings();
		activePass = ActivePass::None;

		// Wait for what someone else drew into the back buffer meanwhile
		WaitForExternalWork();
		return true;
	}

	bool VulkanBackend::SubmitCommandBuffer(VkSemaphore waitSemaphore, VkPipelineStageFlags waitStages, VkSemaphore signalSemaphore, bool shouldSignalFence)
	{
		if (commandBuffer == VK_NULL_HANDLE)
		{
			return false;
		}

		EndRenderPassIfActive();

		VkResult result = vkEndCommandBuffer(commandBuffer);
		if (result != VK_SUCCESS)
		{
			Fail("vkEndCommandBuffer", result);
			commandBuffer = VK_NULL_HANDLE;
			return false;
		}

		// The submit points at a copy of the handle: the member is cleared below, before
		// the submit reads it
		VkCommandBuffer const submitted = commandBuffer;

		VkSubmitInfo submit{ VK_STRUCTURE_TYPE_SUBMIT_INFO };
		submit.waitSemaphoreCount   = (waitSemaphore != VK_NULL_HANDLE) ? 1u : 0u;
		submit.pWaitSemaphores      = &waitSemaphore;
		submit.pWaitDstStageMask    = &waitStages;
		submit.commandBufferCount   = 1;
		submit.pCommandBuffers      = &submitted;
		submit.signalSemaphoreCount = (signalSemaphore != VK_NULL_HANDLE) ? 1u : 0u;
		submit.pSignalSemaphores    = &signalSemaphore;

		// Send the texture uploads ahead of the draws that sample them
		SubmitTextureBatch();

		FrameSlot& slot = frameSlots[currentSlot];
		VkFence fence = VK_NULL_HANDLE;

		if (shouldSignalFence)
		{
			vkResetFences(device, 1, &slot.fence);
			fence = slot.fence;
		}

		commandBuffer = VK_NULL_HANDLE;

		result = SubmitToQueue(submit, fence);
		if (result != VK_SUCCESS)
		{
			Fail("vkQueueSubmit", result);
			return false;
		}

		if (shouldSignalFence)
		{
			slot.isFencePending = true;
			slot.serial         = frameSerial;
		}

		return true;
	}

	bool VulkanBackend::EnsureFrame(void)
	{
		if (isDead || isDeviceLost || !IsReady())
		{
			return false;
		}

		if (isFrameActive)
		{
			return true;
		}

		PhaseScope const recording(*this, FRAME_PHASE_RECORDING);

		// Wait for the frame that last used this slot
		//
		// Two frames in flight: this one is recorded while the GPU still draws the one
		// before it, and only the one before that has to be finished.
		FrameSlot& slot = frameSlots[currentSlot];
		if (!WaitForFrame(slot.serial))
		{
			return false;
		}

		// Release what it retired and reset what it recorded
		FlushRetired(slot);
		slot.retiredImages.insert(slot.retiredImages.end(), pendingRetiredImages.begin(), pendingRetiredImages.end());
		pendingRetiredImages.clear();
		vkResetCommandPool(device, slot.commandPool, 0);
		vkResetDescriptorPool(device, slot.transientPool, 0);
		slot.usedCommandBuffers = 0;
		slot.transientSets      = 0;
		slot.serial             = frameSerial;
		slot.isFencePending     = false;

		isFrameActive = true;
		if (!BeginCommandBuffer())
		{
			isFrameActive = false;
			return false;
		}

		frameVertexBytes = 0;

		// The draw record's copy belongs to an earlier frame's ring position
		drawRecordBuffer = VK_NULL_HANDLE;

		// Give a fresh back buffer and depth buffer defined contents
		//
		// Black and far, which is what the game sees before it draws anything. From then
		// on the back buffer keeps whatever the last frame left in it.
		if (backBuffer.layout == VK_IMAGE_LAYOUT_UNDEFINED)
		{
			TransitionTo(VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);

			VkClearColorValue const black{ { 0.0f, 0.0f, 0.0f, 1.0f } };
			VkImageSubresourceRange range{ VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 };
			vkCmdClearColorImage(commandBuffer, backBuffer.image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, &black, 1, &range);
		}

		if (depthBuffer.layout == VK_IMAGE_LAYOUT_UNDEFINED)
		{
			TransitionDepth(VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);

			VkClearDepthStencilValue const farthest{ 1.0f, 0 };
			VkImageSubresourceRange range{ DepthAspects(), 0, 1, 0, 1 };
			vkCmdClearDepthStencilImage(commandBuffer, depthBuffer.image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, &farthest, 1, &range);
		}

		return true;
	}

	bool VulkanBackend::SubmitFrameSoFar(void)
	{
		if (!isFrameActive || commandBuffer == VK_NULL_HANDLE)
		{
			return false;
		}

		// Submit what has been recorded and wait for it
		//
		// The frame's fence covers this part; the rest of the frame resets and signals it
		// again when it ends. Nothing waits for a swapchain image, since only presenting
		// touches one.
		if (!SubmitCommandBuffer(VK_NULL_HANDLE, 0, VK_NULL_HANDLE, true))
		{
			isFrameActive = false;
			return false;
		}

		partialSubmits++;

		FrameSlot& slot = frameSlots[currentSlot];
		VkResult const result = WaitForFence(slot.fence, UINT64_MAX);
		if (result != VK_SUCCESS)
		{
			Fail("vkWaitForFences", result);
			isFrameActive = false;
			return false;
		}

		slot.isFencePending = false;

		// Everything submitted so far has finished, so every arena block is free again
		completedSerial = std::max(completedSerial, frameSerial - 1);
		ArenaRewind(vertexArena);
		ArenaRewind(indexArena);
		ArenaRewind(stagingArena);
		ArenaRewind(uniformArena);
		drawRecordBuffer = VK_NULL_HANDLE;

		// Carry on recording the same frame in the next command buffer
		if (!BeginCommandBuffer())
		{
			isFrameActive = false;
			return false;
		}

		return true;
	}

	VkResult VulkanBackend::SubmitToQueue(VkSubmitInfo const& submit, VkFence fence)
	{
		PhaseScope const submitting(*this, FRAME_PHASE_SUBMITS);
		return vkQueueSubmit(queue, 1, &submit, fence);
	}

	VkResult VulkanBackend::WaitForFence(VkFence fence, uint64_t timeoutNanoseconds)
	{
		PhaseScope const waiting(*this, FRAME_PHASE_GPU_WAITS);
		return vkWaitForFences(device, 1, &fence, VK_TRUE, timeoutNanoseconds);
	}

	VulkanBackend::FramePhase VulkanBackend::EnterPhase(FramePhase phase)
	{
		// Only follow the phases, without the clock, when nothing reports their times
		if (!isPhaseTimingEnabled)
		{
			FramePhase const interruptedPhase = activePhase;
			activePhase = phase;
			return interruptedPhase;
		}

		LARGE_INTEGER now{};
		QueryPerformanceCounter(&now);

		// Charge the phase being left, once the clock has started
		if (phaseStartTicks != 0)
		{
			framePhaseTicks[activePhase] += now.QuadPart - phaseStartTicks;
		}

		// Make the new one active
		FramePhase const interruptedPhase = activePhase;
		activePhase     = phase;
		phaseStartTicks = now.QuadPart;
		return interruptedPhase;
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

		case VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL:
			outAccess = VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_READ_BIT | VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT;
			outStages = DEPTH_STAGES;
			break;

		case VK_IMAGE_LAYOUT_DEPTH_STENCIL_READ_ONLY_OPTIMAL:
			outAccess = VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_READ_BIT;
			outStages = VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT | DEPTH_STAGES;
			break;

		case VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL:
			outAccess = VK_ACCESS_SHADER_READ_BIT;
			outStages = VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT;
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

	void VulkanBackend::TransitionImage(RenderImage& image, VkImageLayout newLayout, VkImageAspectFlags aspect)
	{
		// Staying in a layout that writes still needs a barrier: two transfers writing the
		// same image are not ordered against each other without one, and the game copies
		// then copies again into the same place.
		bool const isWriteAfterWrite = newLayout == VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
		if (image.layout == newLayout && !isWriteAfterWrite)
		{
			return;
		}

		if (image.image == VK_NULL_HANDLE || commandBuffer == VK_NULL_HANDLE)
		{
			return;
		}

		VkAccessFlags        sourceAccess      = 0;
		VkAccessFlags        destinationAccess = 0;
		VkPipelineStageFlags sourceStages      = 0;
		VkPipelineStageFlags destinationStages = 0;

		LayoutAccess(image.layout, sourceAccess, sourceStages);
		LayoutAccess(newLayout, destinationAccess, destinationStages);

		VkImageMemoryBarrier barrier{ VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER };
		barrier.srcAccessMask       = sourceAccess;
		barrier.dstAccessMask       = destinationAccess;
		barrier.oldLayout           = image.layout;
		barrier.newLayout           = newLayout;
		barrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
		barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
		barrier.image               = image.image;
		barrier.subresourceRange    = { aspect, 0, 1, 0, 1 };

		vkCmdPipelineBarrier(commandBuffer, sourceStages, destinationStages, 0, 0, nullptr, 0, nullptr, 1, &barrier);
		image.layout = newLayout;
	}

	void VulkanBackend::TransitionTo(VkImageLayout newLayout)
	{
		// The layout of the back buffer changes only outside a render pass
		if (backBuffer.layout != newLayout && activePass != ActivePass::None)
		{
			EndRenderPassIfActive();
		}

		TransitionImage(backBuffer, newLayout, VK_IMAGE_ASPECT_COLOR_BIT);
	}

	void VulkanBackend::TransitionDepth(VkImageLayout newLayout)
	{
		if (depthBuffer.layout != newLayout && activePass != ActivePass::None)
		{
			EndRenderPassIfActive();
		}

		TransitionImage(depthBuffer, newLayout, DepthAspects());
	}

	VkImageAspectFlags VulkanBackend::DepthAspects(void) const
	{
		return VK_IMAGE_ASPECT_DEPTH_BIT | (hasStencil ? VkImageAspectFlags{ VK_IMAGE_ASPECT_STENCIL_BIT } : VkImageAspectFlags{ 0 });
	}

	void VulkanBackend::WaitForExternalWork(void)
	{
		if (!isExternalWorkPending || commandBuffer == VK_NULL_HANDLE)
		{
			return;
		}

		// Order everything after what was submitted from outside
		//
		// ReShade's effects and a frame callback draw into the back buffer with commands
		// of their own, and the next pass only loads what they stored once it waits.
		VkMemoryBarrier barrier{ VK_STRUCTURE_TYPE_MEMORY_BARRIER };
		barrier.srcAccessMask = VK_ACCESS_MEMORY_WRITE_BIT;
		barrier.dstAccessMask = VK_ACCESS_MEMORY_READ_BIT | VK_ACCESS_MEMORY_WRITE_BIT;

		vkCmdPipelineBarrier(commandBuffer, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, 0, 1, &barrier, 0, nullptr, 0, nullptr);
		isExternalWorkPending = false;
	}

	void VulkanBackend::InvalidateBindings(void)
	{
		bound = BindingCache{};
	}

	void VulkanBackend::BeginRenderPassIfNeeded(void)
	{
		if (activePass == ActivePass::Main)
		{
			return;
		}

		EndRenderPassIfActive();

		// Move both attachments into their layouts
		//
		// Drawing needs the attachment layouts; clears outside a pass and copies need the
		// transfer ones. The game interleaves them freely, so the transition is driven by
		// what is about to happen rather than fixed once per frame.
		TransitionTo(VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL);
		TransitionDepth(VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL);

		// Begin the pass
		//
		// No clear values: the attachments load what is already there, because the game
		// clears through its own Clear call, which may or may not have happened this
		// frame.
		VkRenderPassBeginInfo beginInformation{ VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO };
		beginInformation.renderPass        = renderPass;
		beginInformation.framebuffer       = framebuffer;
		beginInformation.renderArea.offset = { 0, 0 };
		beginInformation.renderArea.extent = { renderWidth, renderHeight };
		beginInformation.clearValueCount   = 0;

		vkCmdBeginRenderPass(commandBuffer, &beginInformation, VK_SUBPASS_CONTENTS_INLINE);
		activePass = ActivePass::Main;

		// Apply the viewport
		//
		// Draws apply it again, because the game changes it between draws within a
		// single pass.
		ApplyViewport();
	}

	void VulkanBackend::BeginColourOnlyPass(void)
	{
		if (activePass == ActivePass::ColourOnly)
		{
			return;
		}

		EndRenderPassIfActive();
		TransitionTo(VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL);

		VkRenderPassBeginInfo beginInformation{ VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO };
		beginInformation.renderPass        = colourOnlyPass;
		beginInformation.framebuffer       = colourOnlyFramebuffer;
		beginInformation.renderArea.offset = { 0, 0 };
		beginInformation.renderArea.extent = { renderWidth, renderHeight };

		vkCmdBeginRenderPass(commandBuffer, &beginInformation, VK_SUBPASS_CONTENTS_INLINE);
		activePass = ActivePass::ColourOnly;
	}

	void VulkanBackend::EndRenderPassIfActive(void)
	{
		if (activePass == ActivePass::None)
		{
			return;
		}

		vkCmdEndRenderPass(commandBuffer);
		activePass = ActivePass::None;

		// The passes declare these as their final layouts, so they are recorded rather
		// than emitting redundant barriers.
		backBuffer.layout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
	}

	void VulkanBackend::ApplyViewport(void)
	{
		if (activePass == ActivePass::None)
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
		int32_t width  = RenderWidth();
		int32_t height = RenderHeight();

		if (viewportWidth > 0 && viewportHeight > 0)
		{
			x      = viewportX;
			width  = std::min(viewportWidth, MAXIMUM_VIEWPORT_DIMENSION);
			height = std::min(viewportHeight, MAXIMUM_VIEWPORT_DIMENSION);
			y      = RenderHeight() - viewportY - viewportHeight;
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

		// Set only what changed since the command buffer last set it
		bool const isViewportSame = bound.hasViewport && memcmp(&viewport, &bound.viewport, sizeof(viewport)) == 0;
		bool const isScissorSame  = bound.hasViewport && memcmp(&rectangle, &bound.scissor, sizeof(rectangle)) == 0;

		if (!isViewportSame)
		{
			vkCmdSetViewport(commandBuffer, 0, 1, &viewport);
		}

		if (!isScissorSame)
		{
			vkCmdSetScissor(commandBuffer, 0, 1, &rectangle);
		}

		bound.viewport    = viewport;
		bound.scissor     = rectangle;
		bound.hasViewport = true;

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

			LogDebug("Vulkan: viewport %d,%d %dx%d (from game %d,%d %dx%d, render %ux%u)", x, y, width, height, viewportX, viewportY, viewportWidth, viewportHeight, renderWidth, renderHeight);
		}
	}

	bool VulkanBackend::ViewportRectangle(VkRect2D& outRectangle) const
	{
		// Start from the game's rectangle, or the whole window
		int32_t x      = 0;
		int32_t y      = 0;
		int32_t width  = RenderWidth();
		int32_t height = RenderHeight();

		bool const isSubViewport = viewportWidth > 0 && viewportHeight > 0;

		if (isSubViewport)
		{
			x      = viewportX;
			width  = viewportWidth;
			height = viewportHeight;

			// OpenGL measures the viewport from the bottom of the window and Vulkan from
			// the top, so the origin has to be reflected.
			y = RenderHeight() - viewportY - viewportHeight;
		}

		// Clamp it to the window
		//
		// A viewport outside the framebuffer is invalid, and the game can name one while
		// the window is being resized.
		if (x < 0) { width += x; x = 0; }
		if (y < 0) { height += y; y = 0; }
		if (x + width > RenderWidth())   { width  = RenderWidth() - x; }
		if (y + height > RenderHeight()) { height = RenderHeight() - y; }
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

		// Read back through system memory: the CPU reads it, which device memory makes slow
		VkBufferCreateInfo bufferInformation{ VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO };
		bufferInformation.size        = size;
		bufferInformation.usage       = VK_BUFFER_USAGE_TRANSFER_DST_BIT;
		bufferInformation.sharingMode = VK_SHARING_MODE_EXCLUSIVE;

		if (vkCreateBuffer(device, &bufferInformation, nullptr, &readbackBuffer) != VK_SUCCESS)
		{
			readbackBuffer = VK_NULL_HANDLE;
			return false;
		}

		VkMemoryRequirements requirements{};
		vkGetBufferMemoryRequirements(device, readbackBuffer, &requirements);

		uint32_t typeIndex = 0;
		bool const hasCached = FindMemoryType(requirements.memoryTypeBits, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT | VK_MEMORY_PROPERTY_HOST_CACHED_BIT, typeIndex);
		if (!hasCached && !FindMemoryType(requirements.memoryTypeBits, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT, typeIndex))
		{
			DestroyReadbackBuffer();
			return false;
		}

		VkMemoryAllocateInfo allocationInformation{ VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO };
		allocationInformation.allocationSize  = requirements.size;
		allocationInformation.memoryTypeIndex = typeIndex;

		if (AllocateDeviceMemory(allocationInformation, readbackMemory) != VK_SUCCESS || vkBindBufferMemory(device, readbackBuffer, readbackMemory, 0) != VK_SUCCESS || vkMapMemory(device, readbackMemory, 0, requirements.size, 0, &readbackMapped) != VK_SUCCESS)
		{
			readbackMapped = nullptr;
			DestroyReadbackBuffer();
			return false;
		}

		readbackSize = size;
		return true;
	}

	void VulkanBackend::DestroyReadbackBuffer(void)
	{
		if (device == VK_NULL_HANDLE)
		{
			return;
		}

		if (readbackMapped != nullptr)        { vkUnmapMemory(device, readbackMemory); readbackMapped = nullptr; }
		FreeDeviceMemory(readbackMemory);
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

			// Four bytes a texel either way: a 32-bit depth, or a 24-bit one packed with
			// the unused byte, which reads the same once masked.
			if (region.isDepth && region.format != VK_FORMAT_D32_SFLOAT && region.format != VK_FORMAT_D24_UNORM_S8_UINT && region.format != VK_FORMAT_D32_SFLOAT_S8_UINT)
			{
				LogWarn("Vulkan: the depth region is format %d; not capturing it.", region.format);
				return false;
			}

			if (!EnsureReadbackBuffer(VkDeviceSize{ region.width } * region.height * 4u))
			{
				return false;
			}

			// Copy it out
			//
			// Saved regions are left in the transfer source layout, ready to be restored,
			// which is also what a read needs. Depth alone, never the stencil.
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
		if (!EnsureReadbackBuffer(VkDeviceSize{ renderWidth } * renderHeight * 4u))
		{
			return false;
		}

		TransitionTo(VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL);

		VkBufferImageCopy copy{};
		copy.imageSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
		copy.imageSubresource.layerCount = 1;
		copy.imageExtent = { renderWidth, renderHeight, 1 };

		vkCmdCopyImageToBuffer(commandBuffer, backBuffer.image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, readbackBuffer, 1, &copy);
		return true;
	}

	void VulkanBackend::WriteCapture(bool isRegion, uint32_t regionWidth, uint32_t regionHeight)
	{
		// Wait for the GPU to finish the copy
		//
		// This stalls the pipeline, which is fine: captures are deliberate, rare, and the
		// alternative is reading a buffer the GPU is still writing.
		if (!WaitForFrame(frameSerial))
		{
			return;
		}

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
				LogInfo("Vulkan: wrote the saved region to %s (%ux%u)", regionCapturePath.c_str(), regionWidth, regionHeight);
			}
			else
			{
				LogWarn("Vulkan: could not write the region capture to %s", regionCapturePath.c_str());
			}

			return;
		}

		// Write a frame capture
		if (WriteBmp(capturePath.c_str(), pixels, renderWidth, renderHeight, renderWidth * 4u))
		{
			LogInfo("Vulkan: captured frame %llu to %s (%ux%u)", presentedFrames + 1, capturePath.c_str(), renderWidth, renderHeight);
		}
		else
		{
			LogWarn("Vulkan: could not write the capture to %s", capturePath.c_str());
		}
	}

	void VulkanBackend::RecordPresentCopy(uint32_t swapchainImageIndex)
	{
		VkImage const target = swapchainImages[swapchainImageIndex];

		// Move the back buffer to be read and the swapchain image to be written
		//
		// The image's old contents are discarded, so it starts UNDEFINED, in the stage
		// the submit waits for it in.
		TransitionTo(VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL);

		VkImageMemoryBarrier barrier{ VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER };
		barrier.srcAccessMask       = 0;
		barrier.dstAccessMask       = VK_ACCESS_TRANSFER_WRITE_BIT;
		barrier.oldLayout           = VK_IMAGE_LAYOUT_UNDEFINED;
		barrier.newLayout           = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
		barrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
		barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
		barrier.image               = target;
		barrier.subresourceRange    = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 };

		vkCmdPipelineBarrier(commandBuffer, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, nullptr, 0, nullptr, 1, &barrier);

		// Copy it straight across when the two match, or scale it when they do not
		//
		// The window is another size than the picture in borderless fullscreen, when the
		// video mode is smaller than the monitor, and whenever another plugin reshapes the
		// window. The picture is stretched to fill it, as DXGI does for SCD3D11.
		bool const isSameSize   = swapchainExtent.width == renderWidth && swapchainExtent.height == renderHeight;
		bool const isSameFormat = swapchainFormat == backBuffer.format;

		if (isSameSize && isSameFormat)
		{
			VkImageCopy copy{};
			copy.srcSubresource = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1 };
			copy.dstSubresource = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1 };
			copy.extent         = { renderWidth, renderHeight, 1 };

			vkCmdCopyImage(commandBuffer, backBuffer.image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, target, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &copy);
		}
		else
		{
			VkImageBlit blit{};
			blit.srcSubresource = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1 };
			blit.dstSubresource = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1 };
			blit.srcOffsets[1]  = { RenderWidth(), RenderHeight(), 1 };
			blit.dstOffsets[1]  = { static_cast<int32_t>(swapchainExtent.width), static_cast<int32_t>(swapchainExtent.height), 1 };

			vkCmdBlitImage(commandBuffer, backBuffer.image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, target, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &blit, isSameSize ? VK_FILTER_NEAREST : VK_FILTER_LINEAR);
		}

		// Hand the image over for presenting
		barrier.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
		barrier.dstAccessMask = 0;
		barrier.oldLayout     = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
		barrier.newLayout     = VK_IMAGE_LAYOUT_PRESENT_SRC_KHR;

		vkCmdPipelineBarrier(commandBuffer, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT, 0, 0, nullptr, 0, nullptr, 1, &barrier);
	}

	void VulkanBackend::LogHeartbeat(void)
	{
		// A heartbeat, so the log answers "is it still presenting?" without needing the
		// trace. Cheap at one line per few hundred frames.
		//
		// The rate since the last one says whether presenting waits for the display. A
		// rate far above the refresh rate means it does not, which the game does not
		// expect unless vsync was switched off.
		LARGE_INTEGER now{};
		LARGE_INTEGER frequency{};
		QueryPerformanceCounter(&now);
		QueryPerformanceFrequency(&frequency);

		if (lastHeartbeatTicks != 0 && now.QuadPart > lastHeartbeatTicks && frequency.QuadPart > 0)
		{
			// The tick counts are far below the range where a double loses whole ticks.
			double const seconds = static_cast<double>(now.QuadPart - lastHeartbeatTicks) / static_cast<double>(frequency.QuadPart);
			LogDebug("Vulkan: %llu frames presented, %.0f a second.", presentedFrames, static_cast<double>(HEARTBEAT_FRAMES) / seconds);
		}
		else
		{
			LogDebug("Vulkan: %llu frames presented.", presentedFrames);
		}

		lastHeartbeatTicks = now.QuadPart;

		// Report the worst gap between presents and where the time went
		//
		// Split into the game's time and scvk's, so a hitch says whose it is.
		if (ticksPerSecond > 0)
		{
			// The tick counts are far below the range where a double loses whole ticks.
			double const slowestMilliseconds = static_cast<double>(slowestFrameTicks) * 1000.0 / static_cast<double>(ticksPerSecond);
			LogDebug("Vulkan: slowest frame %.0f ms, %u over %lld ms.", slowestMilliseconds, slowFrames, SLOW_FRAME_MILLISECONDS);
			LogPhaseTicks("all frames", allFramesPhaseTicks);

			if (slowFrames > 0)
			{
				LogPhaseTicks("slow frames", slowFramesPhaseTicks);
				LogPhaseTicks("the slowest frame", slowestFramePhaseTicks);
			}
		}

		// Start the next interval
		slowestFrameTicks = 0;
		slowFrames        = 0;
		std::fill(allFramesPhaseTicks, allFramesPhaseTicks + FRAME_PHASE_COUNT, int64_t{ 0 });
		std::fill(slowFramesPhaseTicks, slowFramesPhaseTicks + FRAME_PHASE_COUNT, int64_t{ 0 });
		std::fill(slowestFramePhaseTicks, slowestFramePhaseTicks + FRAME_PHASE_COUNT, int64_t{ 0 });

		LogTextureTraffic();
		LogVertexTraffic();

		if (drawsBeforeUpload != 0 || uploadsAfterDraw != 0)
		{
			LogDebug("Vulkan: texture hazards so far: %llu draws before an upload, %llu uploads after a draw in the same frame.", drawsBeforeUpload, uploadsAfterDraw);
		}

		// Keep the compiled pipelines, in case the session ends without a shutdown
		SavePipelineCache();
	}

	void VulkanBackend::TimeFrame(int64_t nowTicks)
	{
		// Learn the counter's rate
		if (ticksPerSecond == 0)
		{
			LARGE_INTEGER frequency{};
			QueryPerformanceFrequency(&frequency);
			ticksPerSecond = frequency.QuadPart;
		}

		// Close the frame's last phase
		//
		// At the tick the gap is measured to, so the frame's phases add up to the gap.
		if (phaseStartTicks != 0)
		{
			framePhaseTicks[activePhase] += nowTicks - phaseStartTicks;
		}

		phaseStartTicks = nowTicks;

		// Count the gap since the last present, and where it went
		if (lastPresentTicks != 0 && nowTicks > lastPresentTicks)
		{
			int64_t const frameTicks = nowTicks - lastPresentTicks;
			bool const    isSlow     = frameTicks * 1000 > SLOW_FRAME_MILLISECONDS * ticksPerSecond;

			for (uint32_t phase = 0; phase < FRAME_PHASE_COUNT; phase++)
			{
				allFramesPhaseTicks[phase] += framePhaseTicks[phase];

				if (isSlow)
				{
					slowFramesPhaseTicks[phase] += framePhaseTicks[phase];
				}
			}

			if (frameTicks > slowestFrameTicks)
			{
				slowestFrameTicks = frameTicks;
				std::copy(framePhaseTicks, framePhaseTicks + FRAME_PHASE_COUNT, slowestFramePhaseTicks);
			}

			if (isSlow)
			{
				slowFrames++;
			}
		}

		// Start the next frame
		lastPresentTicks = nowTicks;
		std::fill(framePhaseTicks, framePhaseTicks + FRAME_PHASE_COUNT, int64_t{ 0 });
	}

	void VulkanBackend::LogPhaseTicks(char const* heading, int64_t const phaseTicks[FRAME_PHASE_COUNT]) const
	{
		if (ticksPerSecond <= 0)
		{
			return;
		}

		// Convert each phase to milliseconds
		//
		// The tick counts are far below the range where a double loses whole ticks.
		double milliseconds[FRAME_PHASE_COUNT] = {};
		for (uint32_t phase = 0; phase < FRAME_PHASE_COUNT; phase++)
		{
			milliseconds[phase] = static_cast<double>(phaseTicks[phase]) * 1000.0 / static_cast<double>(ticksPerSecond);
		}

		LogDebug("Vulkan: %s in ms: game %.0f, recording %.0f, vertex copies %.0f, pipelines %.0f, textures %.0f, submits %.0f, GPU waits %.0f, swapchain %.0f.", heading, milliseconds[FRAME_PHASE_GAME], milliseconds[FRAME_PHASE_RECORDING], milliseconds[FRAME_PHASE_VERTEX_COPIES], milliseconds[FRAME_PHASE_PIPELINES], milliseconds[FRAME_PHASE_TEXTURES], milliseconds[FRAME_PHASE_SUBMITS], milliseconds[FRAME_PHASE_GPU_WAITS], milliseconds[FRAME_PHASE_SWAPCHAIN]);
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

		// Ask for what the fullscreen policy needs when the loader has it
		//
		// VK_EXT_full_screen_exclusive builds on these two, and on Vulkan 1.0 the first
		// has to be asked for by name. Without them the swapchain is created without a
		// policy, as before.
		canAskFullscreenPolicy = HasInstanceExtension(VK_KHR_GET_PHYSICAL_DEVICE_PROPERTIES_2_EXTENSION_NAME) && HasInstanceExtension(VK_KHR_GET_SURFACE_CAPABILITIES_2_EXTENSION_NAME);
		if (canAskFullscreenPolicy)
		{
			extensions.push_back(VK_KHR_GET_PHYSICAL_DEVICE_PROPERTIES_2_EXTENSION_NAME);
			extensions.push_back(VK_KHR_GET_SURFACE_CAPABILITIES_2_EXTENSION_NAME);
		}

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
			LogInfo("Vulkan: validation layers enabled.");

			if (HasMarkerFile(SYNC_VALIDATION_MARKER) && HasInstanceExtension(VK_EXT_VALIDATION_FEATURES_EXTENSION_NAME, VALIDATION_LAYER))
			{
				extensions.push_back(VK_EXT_VALIDATION_FEATURES_EXTENSION_NAME);
				information.enabledExtensionCount   = extensions.size();
				information.ppEnabledExtensionNames = extensions.data();
				information.pNext                   = &features;
				LogInfo("Vulkan: synchronisation validation enabled.");
			}
		}
		else
		{
			LogWarn("Vulkan: validation layers not available to this 32-bit process. Install the 32-bit components of the Vulkan SDK to get them.");
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
				LogInfo("Vulkan: validation messages will be written to this log.");
			}
		}

		return PickPhysicalDevice();
	}

	bool VulkanBackend::CreateSurfaceAndDevice(void* newWindowHandle, uint32_t width, uint32_t height)
	{
		if (isDead || instance == VK_NULL_HANDLE || width == 0 || height == 0)
		{
			return false;
		}

		// Destroy what a previous window had
		//
		// The driver destroys its old window before creating a new one, so a surface and
		// swapchain left from it would present into nothing.
		DestroyDevice();
		windowHandle = newWindowHandle;

		// Time the frame's phases only when the log reports them
		isPhaseTimingEnabled = IsLogged(LOG_LEVEL_DEBUG);
		renderWidth  = width;
		renderHeight = height;

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
		if (!CreateLogicalDevice() || !CreateFrameResources())
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

		// Create what the game draws into, then the swapchain it is shown through
		//
		// A window with no area yet leaves the swapchain for later; the frames are drawn
		// all the same.
		if (!CreateRenderTargets())
		{
			isDead = true;
			return false;
		}

		CreateEffectResources();
		CreateSwapchain();

		deviceGeneration++;
		isPresentationPaused = false;
		lastPresentResult    = VK_SUCCESS;
		return !isDead;
	}

	bool VulkanBackend::IsReady(void)
	{
		return !isDead && !isDeviceLost && device != VK_NULL_HANDLE && backBuffer.image != VK_NULL_HANDLE;
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

		PhaseScope const recording(*this, FRAME_PHASE_RECORDING);

		// Clear inside the pass, only the scissor under a sub-viewport
		//
		// The game's OpenGL driver has its scissor test on there, and glClear honours it.
		// Clearing as an attachment keeps the pass open and the back buffer in its
		// attachment layout, where an image clear would close both.
		VkRect2D scissor{};
		ViewportRectangle(scissor);

		if (scissor.extent.width == 0 || scissor.extent.height == 0)
		{
			return;
		}

		BeginRenderPassIfNeeded();

		VkClearAttachment attachment{};
		attachment.aspectMask       = VK_IMAGE_ASPECT_COLOR_BIT;
		attachment.colorAttachment  = 0;
		attachment.clearValue.color = { { red, green, blue, alpha } };

		VkClearRect clearRectangle{};
		clearRectangle.rect       = scissor;
		clearRectangle.layerCount = 1;

		vkCmdClearAttachments(commandBuffer, 1, &attachment, 1, &clearRectangle);
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

		PhaseScope const recording(*this, FRAME_PHASE_RECORDING);

		// Refuse an origin off the top or left
		//
		// That would need the source pointer advanced as well as the extent reduced. Not
		// seen from the game so far, so it is refused rather than half-implemented.
		if (destinationX < 0 || destinationY < 0)
		{
			LogWarn("Vulkan: blit origin %d,%d is negative; skipping.", destinationX, destinationY);
			return;
		}

		// Both are zero or more, checked just above.
		uint32_t const left = static_cast<uint32_t>(destinationX);
		uint32_t const top  = static_cast<uint32_t>(destinationY);

		if (left >= renderWidth || top >= renderHeight)
		{
			return;
		}

		// Clip against the right and bottom edges
		//
		// Rows are still strided by the full source width, which is what bufferRowLength
		// expresses, so clipping the extent alone gives the correct result.
		uint32_t const copyWidth  = std::min(width, renderWidth - left);
		uint32_t const copyHeight = std::min(height, renderHeight - top);

		// Stage the pixels
		VkDeviceSize const bytes = VkDeviceSize{ sourceWidth } * height * 4u;

		VkBuffer     stagingBuffer = VK_NULL_HANDLE;
		VkDeviceSize offset        = 0;
		uint8_t*     destination   = nullptr;

		if (!AllocateFrameStaging(bytes, stagingBuffer, offset, destination))
		{
			LogWarn("Vulkan: no staging for a blit of %llu bytes; skipping it.", bytes);
			return;
		}

		// The copy fits the arena block, so its size fits a size_t.
		memcpy(destination, pixels, static_cast<size_t>(bytes));

		// Copy them into the image
		//
		// Transfers cannot run inside a render pass, so a blit arriving after a draw has
		// to close it.
		VkBufferImageCopy copy{};
		copy.bufferOffset      = offset;
		copy.bufferRowLength   = sourceWidth;
		copy.bufferImageHeight = height;
		copy.imageSubresource  = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1 };
		copy.imageOffset       = { destinationX, destinationY, 0 };
		copy.imageExtent       = { copyWidth, copyHeight, 1 };

		EndRenderPassIfActive();
		TransitionTo(VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);

		vkCmdCopyBufferToImage(commandBuffer, stagingBuffer, backBuffer.image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &copy);
	}

	bool VulkanBackend::ReadFramePixels(uint32_t x, uint32_t y, uint32_t width, uint32_t height, uint8_t* outPixels)
	{
		if (!IsReady() || outPixels == nullptr || width == 0 || height == 0)
		{
			return false;
		}

		PhaseScope const recording(*this, FRAME_PHASE_RECORDING);

		if (x >= renderWidth || y >= renderHeight || width > renderWidth - x || height > renderHeight - y)
		{
			return false;
		}

		VkDeviceSize const bytes = VkDeviceSize{ width } * height * 4u;

		if (!EnsureReadbackBuffer(bytes))
		{
			return false;
		}

		// Describe the copy, and the barrier that makes it visible to the host
		//
		// Both coordinates were checked against the image above, which is far below
		// INT32_MAX, so they convert to offsets unchanged. The fence wait only says the
		// copy has finished; reading its result on the host also needs the write made
		// visible there.
		VkBufferImageCopy copy{};
		copy.imageSubresource = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1 };
		copy.imageOffset      = { static_cast<int32_t>(x), static_cast<int32_t>(y), 0 };
		copy.imageExtent      = { width, height, 1 };

		VkMemoryBarrier barrier{ VK_STRUCTURE_TYPE_MEMORY_BARRIER };
		barrier.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
		barrier.dstAccessMask = VK_ACCESS_HOST_READ_BIT;

		if (!isFrameActive)
		{
			// Between frames, read the back buffer on commands of their own
			//
			// It still holds the frame last presented, which is what the game expects for
			// a photo. The barrier reaches back across the frames already submitted, so
			// the read waits for the one that drew it.
			BeginUploadCommands();
			commandBuffer = uploadCommandBuffer;
			TransitionTo(VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL);
			vkCmdCopyImageToBuffer(uploadCommandBuffer, backBuffer.image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, readbackBuffer, 1, &copy);
			vkCmdPipelineBarrier(uploadCommandBuffer, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_HOST_BIT, 0, 1, &barrier, 0, nullptr, 0, nullptr);
			commandBuffer = VK_NULL_HANDLE;
			SubmitUploadCommands();
		}
		else
		{
			// Or read the frame drawn so far, submitting it early to wait for it
			EndRenderPassIfActive();
			TransitionTo(VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL);

			vkCmdCopyImageToBuffer(commandBuffer, backBuffer.image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, readbackBuffer, 1, &copy);
			vkCmdPipelineBarrier(commandBuffer, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_HOST_BIT, 0, 1, &barrier, 0, nullptr, 0, nullptr);

			if (!SubmitFrameSoFar())
			{
				return false;
			}
		}

		if (isDeviceLost)
		{
			return false;
		}

		// Hand the pixels over with every alpha opaque
		//
		// The back buffer's alpha holds whatever blending left there, which is not part of
		// the picture, so it is replaced the way SCGL does. The readback memory holds
		// bytes.
		uint8_t const* const source = static_cast<uint8_t const*>(readbackMapped);
		size_t const pixelCount = size_t{ width } * height;

		for (size_t i = 0; i < pixelCount; i++)
		{
			outPixels[i * 4u + 0u] = source[i * 4u + 0u];
			outPixels[i * 4u + 1u] = source[i * 4u + 1u];
			outPixels[i * 4u + 2u] = source[i * 4u + 2u];
			outPixels[i * 4u + 3u] = 0xff;
		}

		return true;
	}

	void VulkanBackend::Present(void)
	{
		if (isDead || isDeviceLost)
		{
			return;
		}

		PhaseScope const recording(*this, FRAME_PHASE_RECORDING);

		// Make sure there is a frame to present
		//
		// The game means "swap buffers" by this, so a present has to happen even when
		// nothing asked us to start a frame. Otherwise the swapchain stops cycling, the
		// window keeps showing its last image, and every overlay that hooks
		// vkQueuePresentKHR, including Steam and any FPS counter, freezes with it, which
		// looks exactly like a hang without being one. The back buffer still holds the
		// last frame, so presenting it again shows what the game expects.
		if (!isFrameActive && !EnsureFrame())
		{
			return;
		}

		NoteRenderPhase("Present: recording the end of the frame");
		EndRenderPassIfActive();

		// Record a requested capture
		//
		// Recorded between the last draw and the copy for presenting, so it sees exactly
		// what the user sees.
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

		// Acquire an image to show the frame in, rebuilding the swapchain if it is gone
		//
		// The frame's drawing does not depend on it: a minimised window, which has no
		// swapchain, still gets its frames drawn into the back buffer, as a DirectX device
		// does with presentation paused.
		FrameSlot& slot = frameSlots[currentSlot];
		uint32_t   imageIndex  = 0;
		bool       isAcquired  = false;

		NoteRenderPhase("Present: acquiring a swapchain image");

		if (swapchain != VK_NULL_HANDLE || RestoreSwapchain())
		{
			VkResult result;
			{
				PhaseScope const acquiring(*this, FRAME_PHASE_SWAPCHAIN);
				result = vkAcquireNextImageKHR(device, swapchain, ACQUIRE_TIMEOUT_NANOSECONDS, slot.imageAvailable, VK_NULL_HANDLE, &imageIndex);
			}

			if (result == VK_SUCCESS || result == VK_SUBOPTIMAL_KHR)
			{
				isAcquired = true;
			}
			else if (result == VK_ERROR_OUT_OF_DATE_KHR || result == VK_ERROR_SURFACE_LOST_KHR)
			{
				LogDebug("Vulkan: the swapchain no longer fits the window (%s); rebuilding it.", VkResultName(result));
				DestroySwapchain();
				CreateSwapchain();
			}
			else if (NoteDeviceLoss(result))
			{
				return;
			}
			else if (result != VK_TIMEOUT && result != VK_NOT_READY && result != lastPresentResult)
			{
				LogWarn("Vulkan: vkAcquireNextImageKHR returned %s; this frame is not shown.", VkResultName(result));
			}
		}

		// Say when presenting stops and starts again
		if (isAcquired == isPresentationPaused)
		{
			HWND const window = static_cast<HWND>(windowHandle);
			LogInfo("Vulkan: presentation %s (iconic %d, visible %d, foreground %d).", isAcquired ? "resumed" : "paused", IsIconic(window) ? 1 : 0, IsWindowVisible(window) ? 1 : 0, (GetForegroundWindow() == window) ? 1 : 0);
			isPresentationPaused = !isAcquired;
		}

		// Copy the back buffer into it and submit the frame
		if (isAcquired)
		{
			RecordPresentCopy(imageIndex);
		}

		NoteRenderPhase("Present: submitting the frame");

		bool const isSubmitted = SubmitCommandBuffer(isAcquired ? slot.imageAvailable : VK_NULL_HANDLE, VK_PIPELINE_STAGE_TRANSFER_BIT, isAcquired ? renderFinishedSemaphores[imageIndex] : VK_NULL_HANDLE, true);
		isFrameActive = false;

		if (!isSubmitted)
		{
			return;
		}

		if (isCapturingRegion || isCapturingFrame)
		{
			WriteCapture(isCapturingRegion, regionWidth, regionHeight);
		}

		// Present it
		VkResult result = VK_SUCCESS;

		if (isAcquired)
		{
			NoteRenderPhase("Present: vkQueuePresentKHR");

			VkPresentInfoKHR present{ VK_STRUCTURE_TYPE_PRESENT_INFO_KHR };
			present.waitSemaphoreCount = 1;
			present.pWaitSemaphores    = &renderFinishedSemaphores[imageIndex];
			present.swapchainCount     = 1;
			present.pSwapchains        = &swapchain;
			present.pImageIndices      = &imageIndex;

			PhaseScope const presenting(*this, FRAME_PHASE_SWAPCHAIN);
			result = vkQueuePresentKHR(queue, &present);
		}

		// Move on to the next frame slot
		currentSlot = (currentSlot + 1) % FRAMES_IN_FLIGHT;
		frameSerial++;

		// Log a change in what presenting returns, success codes included
		if (result != lastPresentResult)
		{
			HWND const window = static_cast<HWND>(windowHandle);
			LogInfo("Vulkan: present returned %s (was %s; iconic %d, visible %d, foreground %d).", VkResultName(result), VkResultName(lastPresentResult), IsIconic(window) ? 1 : 0, IsWindowVisible(window) ? 1 : 0, (GetForegroundWindow() == window) ? 1 : 0);
			lastPresentResult = result;
		}

		// Rebuild the swapchain when it no longer fits the window
		if (result == VK_ERROR_OUT_OF_DATE_KHR || result == VK_SUBOPTIMAL_KHR || result == VK_ERROR_SURFACE_LOST_KHR)
		{
			DestroySwapchain();

			// A failure here leaves no swapchain, and frames are drawn without being shown
			// until RestoreSwapchain makes one again. Silence would make that
			// indistinguishable from a hang, so it says so.
			if (!CreateSwapchain())
			{
				LogWarn("Vulkan: could not rebuild the swapchain; frames are drawn but not shown until the window has area again.");
			}
		}
		else if (NoteDeviceLoss(result))
		{
			return;
		}
		else if (result != VK_SUCCESS)
		{
			LogWarn("Vulkan: vkQueuePresentKHR failed with %s.", VkResultName(result));
		}

		// Time the gap since the last present
		LARGE_INTEGER now{};
		QueryPerformanceCounter(&now);
		TimeFrame(now.QuadPart);

		// Count the frame
		presentedFrames++;

		// Confirm the first one
		//
		// At the default log level nothing follows the depth buffer line, so a log that
		// stops there looked the same whether the game ran or not. The first present is
		// the earliest point where every part of the renderer has worked once.
		if (presentedFrames == 1)
		{
			LogInfo("scvk %s is running; the first frame is on screen.", SCVK_VERSION_STRING);
		}

		if ((presentedFrames % HEARTBEAT_FRAMES) == 0)
		{
			LogHeartbeat();
		}

		// Hold a frame nobody sees, so a minimised game does not spin a core
		if (!isAcquired)
		{
			Sleep(PAUSED_FRAME_MILLISECONDS);
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
