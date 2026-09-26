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
 * The depth buffer and the buffer regions.
 *
 * The city view is drawn once into the framebuffer, saved into a region, and restored
 * from it every frame, with only what changed redrawn on top. Both copies go through
 * images the render pass does not track, so each one carries its own barriers.
 */

//// Dependencies

#include "VulkanBackend.h"
#include "Logger.h"

#include <algorithm>

namespace scvk
{
	//// Constants

	namespace
	{
		// Depth attachment access, both read and written by the depth test.
		constexpr VkAccessFlags DEPTH_ATTACHMENT_ACCESS = VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_READ_BIT | VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT;
	}

	//// Private Functions

	bool VulkanBackend::CreateDepthResources(void)
	{
		// Pick a depth format
		//
		// D32 first, falling back to the packed depth-stencil format. One or the other is
		// guaranteed present, and the game only needs depth: its stencil calls are
		// recorded but not yet honoured.
		VkFormat const candidates[] = { VK_FORMAT_D32_SFLOAT, VK_FORMAT_D24_UNORM_S8_UINT, VK_FORMAT_D16_UNORM };

		depthFormat = VK_FORMAT_UNDEFINED;
		for (VkFormat candidate : candidates)
		{
			VkFormatProperties properties{};
			vkGetPhysicalDeviceFormatProperties(physicalDevice, candidate, &properties);

			if ((properties.optimalTilingFeatures & VK_FORMAT_FEATURE_DEPTH_STENCIL_ATTACHMENT_BIT) != 0)
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

		// Create the image
		//
		// TRANSFER_DST for the game's depth clears, TRANSFER_SRC because it also saves
		// the depth buffer into a buffer region.
		VkImageCreateInfo imageInformation{ VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO };
		imageInformation.imageType     = VK_IMAGE_TYPE_2D;
		imageInformation.format        = depthFormat;
		imageInformation.extent        = { swapchainExtent.width, swapchainExtent.height, 1 };
		imageInformation.mipLevels     = 1;
		imageInformation.arrayLayers   = 1;
		imageInformation.samples       = VK_SAMPLE_COUNT_1_BIT;
		imageInformation.tiling        = VK_IMAGE_TILING_OPTIMAL;
		imageInformation.usage         = VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT;
		imageInformation.sharingMode   = VK_SHARING_MODE_EXCLUSIVE;
		imageInformation.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;

		VkResult result = vkCreateImage(device, &imageInformation, nullptr, &depthImage);
		if (result != VK_SUCCESS)
		{
			Fail("vkCreateImage (depth)", result);
			return false;
		}

		// Back it with device-local memory
		VkMemoryRequirements requirements{};
		vkGetImageMemoryRequirements(device, depthImage, &requirements);

		uint32_t typeIndex = 0;
		if (!FindMemoryType(requirements.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT, typeIndex))
		{
			LogNote("Vulkan: no device-local memory for the depth buffer.");
			return false;
		}

		VkMemoryAllocateInfo allocationInformation{ VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO };
		allocationInformation.allocationSize  = requirements.size;
		allocationInformation.memoryTypeIndex = typeIndex;

		result = vkAllocateMemory(device, &allocationInformation, nullptr, &depthMemory);
		if (result != VK_SUCCESS)
		{
			Fail("vkAllocateMemory (depth)", result);
			return false;
		}

		vkBindImageMemory(device, depthImage, depthMemory, 0);

		// Create its view
		VkImageViewCreateInfo viewInformation{ VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO };
		viewInformation.image    = depthImage;
		viewInformation.viewType = VK_IMAGE_VIEW_TYPE_2D;
		viewInformation.format   = depthFormat;
		viewInformation.subresourceRange.aspectMask = VK_IMAGE_ASPECT_DEPTH_BIT;
		viewInformation.subresourceRange.levelCount = 1;
		viewInformation.subresourceRange.layerCount = 1;

		result = vkCreateImageView(device, &viewInformation, nullptr, &depthView);
		if (result != VK_SUCCESS)
		{
			Fail("vkCreateImageView (depth)", result);
			return false;
		}

		// Record that its layout still has to move
		//
		// The image is still UNDEFINED, and the render pass declares the attachment
		// layout as its initial one. Whichever comes first, the next render pass or the
		// game's next depth clear, moves it there.
		isDepthLayoutPending = true;

		LogNote("Vulkan: depth buffer ready, %ux%u, format %d.", swapchainExtent.width, swapchainExtent.height, depthFormat);
		return true;
	}

	void VulkanBackend::DestroyDepthResources(void)
	{
		if (depthView != VK_NULL_HANDLE)   { vkDestroyImageView(device, depthView, nullptr); depthView = VK_NULL_HANDLE; }
		if (depthImage != VK_NULL_HANDLE)  { vkDestroyImage(device, depthImage, nullptr); depthImage = VK_NULL_HANDLE; }
		if (depthMemory != VK_NULL_HANDLE) { vkFreeMemory(device, depthMemory, nullptr); depthMemory = VK_NULL_HANDLE; }
	}

	void VulkanBackend::BarrierDepthImage(VkImageLayout oldLayout, VkImageLayout newLayout, VkAccessFlags sourceAccess, VkAccessFlags destinationAccess, VkPipelineStageFlags sourceStages, VkPipelineStageFlags destinationStages)
	{
		VkImageMemoryBarrier barrier{ VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER };
		barrier.oldLayout           = oldLayout;
		barrier.newLayout           = newLayout;
		barrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
		barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
		barrier.image               = depthImage;
		barrier.srcAccessMask       = sourceAccess;
		barrier.dstAccessMask       = destinationAccess;
		barrier.subresourceRange.aspectMask = VK_IMAGE_ASPECT_DEPTH_BIT;
		barrier.subresourceRange.levelCount = 1;
		barrier.subresourceRange.layerCount = 1;

		vkCmdPipelineBarrier(commandBuffer, sourceStages, destinationStages, 0, 0, nullptr, 0, nullptr, 1, &barrier);
	}

	VkImageLayout VulkanBackend::DepthRestingLayout(void) const
	{
		return isDepthLayoutPending ? VK_IMAGE_LAYOUT_UNDEFINED : VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL;
	}

	void VulkanBackend::TransitionRegion(BufferRegion const& region, VkImageLayout oldLayout, VkImageLayout newLayout)
	{
		// Work out the two sides of the dependency
		VkAccessFlags        sourceAccess      = 0;
		VkAccessFlags        destinationAccess = 0;
		VkPipelineStageFlags sourceStages      = 0;
		VkPipelineStageFlags destinationStages = 0;

		LayoutAccess(oldLayout, sourceAccess, sourceStages);
		LayoutAccess(newLayout, destinationAccess, destinationStages);

		// Record the barrier
		VkImageMemoryBarrier barrier{ VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER };
		barrier.srcAccessMask       = sourceAccess;
		barrier.dstAccessMask       = destinationAccess;
		barrier.oldLayout           = oldLayout;
		barrier.newLayout           = newLayout;
		barrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
		barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
		barrier.image               = region.image;
		barrier.subresourceRange.aspectMask = region.isDepth ? VK_IMAGE_ASPECT_DEPTH_BIT : VK_IMAGE_ASPECT_COLOR_BIT;
		barrier.subresourceRange.levelCount = 1;
		barrier.subresourceRange.layerCount = 1;

		vkCmdPipelineBarrier(commandBuffer, sourceStages, destinationStages, 0, 0, nullptr, 0, nullptr, 1, &barrier);
	}

	bool VulkanBackend::ClampRegionCopy(BufferRegion const& region, int32_t regionX, int32_t regionY, int32_t screenX, int32_t screenY, int32_t& width, int32_t& height) const
	{
		if (regionX < 0 || regionY < 0 || screenX < 0 || screenY < 0)
		{
			return false;
		}

		// Both rectangles have to stay inside their image, and they share one extent, so
		// the smaller of the two limits governs. Region sizes match the window, far below
		// INT32_MAX, so they convert without loss.
		int32_t const regionWidth  = static_cast<int32_t>(region.width);
		int32_t const regionHeight = static_cast<int32_t>(region.height);

		width  = std::min(width, std::min(SwapchainWidth() - screenX, regionWidth - regionX));
		height = std::min(height, std::min(SwapchainHeight() - screenY, regionHeight - regionY));
		return true;
	}

	//// Public API

	void VulkanBackend::ClearDepth(float depth)
	{
		if (!EnsureFrame() || depthImage == VK_NULL_HANDLE)
		{
			return;
		}

		// Clear only the scissor under a sub-viewport, the same way as the colour clear
		VkRect2D scissor{};
		if (ViewportRectangle(scissor))
		{
			if (scissor.extent.width == 0 || scissor.extent.height == 0)
			{
				return;
			}

			BeginRenderPassIfNeeded();

			VkClearAttachment attachment{};
			attachment.aspectMask = VK_IMAGE_ASPECT_DEPTH_BIT;
			attachment.clearValue.depthStencil.depth   = depth;
			attachment.clearValue.depthStencil.stencil = 0;

			VkClearRect clearRectangle{};
			clearRectangle.rect       = scissor;
			clearRectangle.layerCount = 1;

			vkCmdClearAttachments(commandBuffer, 1, &attachment, 1, &clearRectangle);
			return;
		}

		// Move the image into the transfer layout
		//
		// A whole-image depth clear is a transfer operation, so it cannot run inside a
		// render pass any more than a colour clear can. Discarding the old contents is
		// exactly what a clear does, so the still-UNDEFINED first clear needs no special
		// handling beyond naming the layout the image is actually in. The barrier starts
		// from the depth tests, not the top of the pipe: a clear later in a frame has to
		// wait for the draws that wrote depth before it.
		EndRenderPassIfActive();

		BarrierDepthImage(DepthRestingLayout(), VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT, VK_ACCESS_TRANSFER_WRITE_BIT, DEPTH_STAGES, VK_PIPELINE_STAGE_TRANSFER_BIT);
		isDepthLayoutPending = false;

		// Clear it
		VkClearDepthStencilValue value{};
		value.depth   = depth;
		value.stencil = 0;

		VkImageSubresourceRange range{};
		range.aspectMask = VK_IMAGE_ASPECT_DEPTH_BIT;
		range.levelCount = 1;
		range.layerCount = 1;

		vkCmdClearDepthStencilImage(commandBuffer, depthImage, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, &value, 1, &range);

		// Move it back for the depth tests
		BarrierDepthImage(VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL, VK_ACCESS_TRANSFER_WRITE_BIT, DEPTH_ATTACHMENT_ACCESS, VK_PIPELINE_STAGE_TRANSFER_BIT, DEPTH_STAGES);
	}

	uint32_t VulkanBackend::CreateBufferRegion(bool isDepth)
	{
		if (isDead || device == VK_NULL_HANDLE || swapchainExtent.width == 0)
		{
			return 0;
		}

		if (isDepth && depthFormat == VK_FORMAT_UNDEFINED)
		{
			return 0;
		}

		// Create an image the size of the window, in the format it copies
		BufferRegion region;
		region.isDepth = isDepth;
		region.format  = isDepth ? depthFormat : swapchainFormat;
		region.width   = swapchainExtent.width;
		region.height  = swapchainExtent.height;

		VkImageCreateInfo imageInformation{ VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO };
		imageInformation.imageType     = VK_IMAGE_TYPE_2D;
		imageInformation.format        = region.format;
		imageInformation.extent        = { region.width, region.height, 1 };
		imageInformation.mipLevels     = 1;
		imageInformation.arrayLayers   = 1;
		imageInformation.samples       = VK_SAMPLE_COUNT_1_BIT;
		imageInformation.tiling        = VK_IMAGE_TILING_OPTIMAL;
		imageInformation.usage         = VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT;
		imageInformation.sharingMode   = VK_SHARING_MODE_EXCLUSIVE;
		imageInformation.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;

		VkResult result = vkCreateImage(device, &imageInformation, nullptr, &region.image);
		if (result != VK_SUCCESS)
		{
			Fail("vkCreateImage (buffer region)", result);
			return 0;
		}

		// Back it with device-local memory
		VkMemoryRequirements requirements{};
		vkGetImageMemoryRequirements(device, region.image, &requirements);

		uint32_t typeIndex = 0;
		if (!FindMemoryType(requirements.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT, typeIndex))
		{
			vkDestroyImage(device, region.image, nullptr);
			return 0;
		}

		VkMemoryAllocateInfo allocationInformation{ VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO };
		allocationInformation.allocationSize  = requirements.size;
		allocationInformation.memoryTypeIndex = typeIndex;

		result = vkAllocateMemory(device, &allocationInformation, nullptr, &region.memory);
		if (result != VK_SUCCESS)
		{
			Fail("vkAllocateMemory (buffer region)", result);
			vkDestroyImage(device, region.image, nullptr);
			return 0;
		}

		vkBindImageMemory(device, region.image, region.memory, 0);
		region.isLive = true;

		// Reuse a dead slot before growing
		//
		// So a game that cycles regions does not walk the handle space upward forever.
		for (size_t i = 0; i < bufferRegions.size(); i++)
		{
			if (!bufferRegions[i].isLive)
			{
				bufferRegions[i] = region;
				return i + 1;
			}
		}

		bufferRegions.push_back(region);
		return bufferRegions.size();
	}

	bool VulkanBackend::IsBufferRegion(uint32_t handle) const
	{
		return handle != 0 && handle <= bufferRegions.size() && bufferRegions[handle - 1].isLive;
	}

	bool VulkanBackend::SaveBufferRegion(uint32_t handle, int32_t regionX, int32_t regionY, int32_t width, int32_t height, int32_t screenX, int32_t screenY)
	{
		if (!IsBufferRegion(handle) || width <= 0 || height <= 0 || !EnsureFrame())
		{
			return false;
		}

		BufferRegion& region = bufferRegions[handle - 1];

		// Fit the copy to both images and the scissor
		//
		// A copy is not a render pass operation, the same way a clear is not. The region
		// is the destination here, and it shares the window's coordinates, so the scissor
		// applies to it directly.
		EndRenderPassIfActive();

		if (!ClampRegionCopy(region, regionX, regionY, screenX, screenY, width, height))
		{
			return false;
		}

		if (!ClipToScissor(screenX, screenY, regionX, regionY, width, height))
		{
			return true;
		}

		// Move the source into the transfer source layout
		VkImage const       source       = region.isDepth ? depthImage : swapchainImages[imageIndex];
		VkImageLayout const sourceLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;

		if (region.isDepth)
		{
			if (depthImage == VK_NULL_HANDLE)
			{
				return false;
			}

			BarrierDepthImage(DepthRestingLayout(), sourceLayout, VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT, VK_ACCESS_TRANSFER_READ_BIT, DEPTH_STAGES, VK_PIPELINE_STAGE_TRANSFER_BIT);
			isDepthLayoutPending = false;
		}
		else
		{
			TransitionTo(sourceLayout);
		}

		// Copy into the region
		TransitionRegion(region, region.hasContent ? VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL : VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);

		// Both extents were clamped to more than zero above.
		VkImageCopy copy{};
		copy.srcSubresource.aspectMask = region.isDepth ? VK_IMAGE_ASPECT_DEPTH_BIT : VK_IMAGE_ASPECT_COLOR_BIT;
		copy.srcSubresource.layerCount = 1;
		copy.srcOffset = { screenX, screenY, 0 };
		copy.dstSubresource.aspectMask = copy.srcSubresource.aspectMask;
		copy.dstSubresource.layerCount = 1;
		copy.dstOffset = { regionX, regionY, 0 };
		copy.extent    = { static_cast<uint32_t>(width), static_cast<uint32_t>(height), 1 };

		vkCmdCopyImage(commandBuffer, source, sourceLayout, region.image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &copy);

		// Leave the region ready to be read
		//
		// Restoring is the only thing that happens to a region after it has been saved.
		TransitionRegion(region, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL);
		region.hasContent = true;

		// Return the depth image to the depth tests
		if (region.isDepth)
		{
			BarrierDepthImage(sourceLayout, VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL, VK_ACCESS_TRANSFER_READ_BIT, DEPTH_ATTACHMENT_ACCESS, VK_PIPELINE_STAGE_TRANSFER_BIT, DEPTH_STAGES);
		}

		return true;
	}

	bool VulkanBackend::RestoreBufferRegion(uint32_t handle, int32_t regionX, int32_t regionY, int32_t width, int32_t height, int32_t screenX, int32_t screenY)
	{
		if (!IsBufferRegion(handle) || width <= 0 || height <= 0 || !EnsureFrame())
		{
			return false;
		}

		BufferRegion& region = bufferRegions[handle - 1];

		// Nothing has been saved yet, so there is nothing to put back. Copying anyway
		// would paint uninitialised memory over the frame.
		if (!region.hasContent)
		{
			return false;
		}

		// Fit the copy to both images and the scissor
		EndRenderPassIfActive();

		if (!ClampRegionCopy(region, regionX, regionY, screenX, screenY, width, height))
		{
			return false;
		}

		if (!ClipToScissor(regionX, regionY, screenX, screenY, width, height))
		{
			return true;
		}

		// Move the destination into the transfer destination layout
		//
		// From the layout the depth image is really in. UNDEFINED would let the driver
		// discard the depth outside the rectangle being restored.
		VkImage const destination = region.isDepth ? depthImage : swapchainImages[imageIndex];

		if (region.isDepth)
		{
			if (depthImage == VK_NULL_HANDLE)
			{
				return false;
			}

			BarrierDepthImage(DepthRestingLayout(), VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT, VK_ACCESS_TRANSFER_WRITE_BIT, DEPTH_STAGES, VK_PIPELINE_STAGE_TRANSFER_BIT);
			isDepthLayoutPending = false;
		}
		else
		{
			TransitionTo(VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
		}

		// Copy out of the region
		//
		// Both extents were clamped to more than zero above.
		VkImageCopy copy{};
		copy.srcSubresource.aspectMask = region.isDepth ? VK_IMAGE_ASPECT_DEPTH_BIT : VK_IMAGE_ASPECT_COLOR_BIT;
		copy.srcSubresource.layerCount = 1;
		copy.srcOffset = { regionX, regionY, 0 };
		copy.dstSubresource.aspectMask = copy.srcSubresource.aspectMask;
		copy.dstSubresource.layerCount = 1;
		copy.dstOffset = { screenX, screenY, 0 };
		copy.extent    = { static_cast<uint32_t>(width), static_cast<uint32_t>(height), 1 };

		vkCmdCopyImage(commandBuffer, region.image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, destination, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &copy);

		// Return the depth image to the depth tests
		if (region.isDepth)
		{
			BarrierDepthImage(VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL, VK_ACCESS_TRANSFER_WRITE_BIT, DEPTH_ATTACHMENT_ACCESS, VK_PIPELINE_STAGE_TRANSFER_BIT, DEPTH_STAGES);
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
		for (uint32_t handle = 1; handle <= bufferRegions.size(); handle++)
		{
			DestroyBufferRegion(handle);
		}

		bufferRegions.clear();
	}
}
