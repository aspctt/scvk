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

#include "VulkanApi.h"
#include "Logger.h"

PFN_vkGetInstanceProcAddr vkGetInstanceProcAddr = nullptr;

#define SCVK_VK_DEFINE(name) PFN_##name name = nullptr;
SCVK_VK_GLOBAL_FUNCTIONS(SCVK_VK_DEFINE)
SCVK_VK_INSTANCE_FUNCTIONS(SCVK_VK_DEFINE)
SCVK_VK_DEVICE_FUNCTIONS(SCVK_VK_DEFINE)
#undef SCVK_VK_DEFINE

PFN_vkCreateDebugUtilsMessengerEXT  vkCreateDebugUtilsMessengerEXT  = nullptr;
PFN_vkDestroyDebugUtilsMessengerEXT vkDestroyDebugUtilsMessengerEXT = nullptr;

namespace scvk
{
	namespace
	{
		HMODULE gVulkanLibrary = nullptr;
	}

	bool LoadVulkanLoader(void)
	{
		if (gVulkanLibrary != nullptr)
		{
			return true;
		}

		gVulkanLibrary = LoadLibraryA("vulkan-1.dll");
		if (gVulkanLibrary == nullptr)
		{
			LogNote("Vulkan: vulkan-1.dll could not be loaded (error %lu). No Vulkan driver is installed, "
				"or it is not registered for 32-bit processes.", GetLastError());
			return false;
		}

		vkGetInstanceProcAddr = reinterpret_cast<PFN_vkGetInstanceProcAddr>(
			GetProcAddress(gVulkanLibrary, "vkGetInstanceProcAddr"));

		if (vkGetInstanceProcAddr == nullptr)
		{
			LogNote("Vulkan: vulkan-1.dll has no vkGetInstanceProcAddr; the file is not a Vulkan loader.");
			return false;
		}

		bool ok = true;

#define SCVK_VK_LOAD_GLOBAL(name)                                                            \
		name = reinterpret_cast<PFN_##name>(vkGetInstanceProcAddr(VK_NULL_HANDLE, #name));   \
		if (name == nullptr) { LogNote("Vulkan: missing global entry point %s", #name); ok = false; }

		SCVK_VK_GLOBAL_FUNCTIONS(SCVK_VK_LOAD_GLOBAL)
#undef SCVK_VK_LOAD_GLOBAL

		return ok;
	}

	bool LoadVulkanInstanceFunctions(VkInstance instance)
	{
		bool ok = true;

#define SCVK_VK_LOAD_INSTANCE(name)                                                     \
		name = reinterpret_cast<PFN_##name>(vkGetInstanceProcAddr(instance, #name));    \
		if (name == nullptr) { LogNote("Vulkan: missing instance entry point %s", #name); ok = false; }

		SCVK_VK_INSTANCE_FUNCTIONS(SCVK_VK_LOAD_INSTANCE)
#undef SCVK_VK_LOAD_INSTANCE

		// Optional. Absent whenever VK_EXT_debug_utils was not enabled, which
		// is the normal case in Release, so a null result is not a failure.
		vkCreateDebugUtilsMessengerEXT = reinterpret_cast<PFN_vkCreateDebugUtilsMessengerEXT>(
			vkGetInstanceProcAddr(instance, "vkCreateDebugUtilsMessengerEXT"));
		vkDestroyDebugUtilsMessengerEXT = reinterpret_cast<PFN_vkDestroyDebugUtilsMessengerEXT>(
			vkGetInstanceProcAddr(instance, "vkDestroyDebugUtilsMessengerEXT"));

		return ok;
	}

	bool LoadVulkanDeviceFunctions(VkDevice device)
	{
		bool ok = true;

#define SCVK_VK_LOAD_DEVICE(name)                                                   \
		name = reinterpret_cast<PFN_##name>(vkGetDeviceProcAddr(device, #name));    \
		if (name == nullptr) { LogNote("Vulkan: missing device entry point %s", #name); ok = false; }

		SCVK_VK_DEVICE_FUNCTIONS(SCVK_VK_LOAD_DEVICE)
#undef SCVK_VK_LOAD_DEVICE

		return ok;
	}

	void UnloadVulkan(void)
	{
		if (gVulkanLibrary != nullptr)
		{
			FreeLibrary(gVulkanLibrary);
			gVulkanLibrary = nullptr;
		}

		vkGetInstanceProcAddr = nullptr;
	}

	char const* VkResultName(VkResult result)
	{
		switch (result)
		{
		case VK_SUCCESS:                        return "VK_SUCCESS";
		case VK_NOT_READY:                      return "VK_NOT_READY";
		case VK_TIMEOUT:                        return "VK_TIMEOUT";
		case VK_EVENT_SET:                      return "VK_EVENT_SET";
		case VK_EVENT_RESET:                    return "VK_EVENT_RESET";
		case VK_INCOMPLETE:                     return "VK_INCOMPLETE";
		case VK_ERROR_OUT_OF_HOST_MEMORY:       return "VK_ERROR_OUT_OF_HOST_MEMORY";
		case VK_ERROR_OUT_OF_DEVICE_MEMORY:     return "VK_ERROR_OUT_OF_DEVICE_MEMORY";
		case VK_ERROR_INITIALIZATION_FAILED:    return "VK_ERROR_INITIALIZATION_FAILED";
		case VK_ERROR_DEVICE_LOST:              return "VK_ERROR_DEVICE_LOST";
		case VK_ERROR_MEMORY_MAP_FAILED:        return "VK_ERROR_MEMORY_MAP_FAILED";
		case VK_ERROR_LAYER_NOT_PRESENT:        return "VK_ERROR_LAYER_NOT_PRESENT";
		case VK_ERROR_EXTENSION_NOT_PRESENT:    return "VK_ERROR_EXTENSION_NOT_PRESENT";
		case VK_ERROR_FEATURE_NOT_PRESENT:      return "VK_ERROR_FEATURE_NOT_PRESENT";
		case VK_ERROR_INCOMPATIBLE_DRIVER:      return "VK_ERROR_INCOMPATIBLE_DRIVER";
		case VK_ERROR_TOO_MANY_OBJECTS:         return "VK_ERROR_TOO_MANY_OBJECTS";
		case VK_ERROR_FORMAT_NOT_SUPPORTED:     return "VK_ERROR_FORMAT_NOT_SUPPORTED";
		case VK_ERROR_SURFACE_LOST_KHR:         return "VK_ERROR_SURFACE_LOST_KHR";
		case VK_ERROR_NATIVE_WINDOW_IN_USE_KHR: return "VK_ERROR_NATIVE_WINDOW_IN_USE_KHR";
		case VK_SUBOPTIMAL_KHR:                 return "VK_SUBOPTIMAL_KHR";
		case VK_ERROR_OUT_OF_DATE_KHR:          return "VK_ERROR_OUT_OF_DATE_KHR";
		default:                                return "VK_ERROR (unmapped)";
		}
	}
}
