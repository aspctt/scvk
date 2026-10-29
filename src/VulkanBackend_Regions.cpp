/*
 * The depth buffer and the buffer regions.
 *
 * The city view is drawn once into the back buffer, saved into a region, and restored
 * from it every frame, with only what changed redrawn on top. Regions are images the
 * render pass does not track, so each copy carries its own barriers; the back buffer and
 * the depth buffer track their own layouts.
 */

//// Dependencies

#include "VulkanBackend.h"
#include "Logger.h"

#include <algorithm>
#include <cmath>

namespace scvk
{
	//// Constants

	namespace
	{
		// Depth formats in order of preference, by default and with Use32BitDepth. The
		// ones with stencil come first, and every device has D24_UNORM_S8_UINT or
		// D32_SFLOAT_S8_UINT, so the formats without are there for a device that breaks
		// the rule. Every device has D16_UNORM, so both lists end up with a format.
		constexpr size_t   DEPTH_FORMAT_CANDIDATES = 5;
		constexpr VkFormat SIXTEEN_BIT_DEPTH_FIRST[DEPTH_FORMAT_CANDIDATES]    = { VK_FORMAT_D16_UNORM_S8_UINT, VK_FORMAT_D24_UNORM_S8_UINT, VK_FORMAT_D32_SFLOAT_S8_UINT, VK_FORMAT_D16_UNORM, VK_FORMAT_D32_SFLOAT };
		constexpr VkFormat THIRTY_TWO_BIT_DEPTH_FIRST[DEPTH_FORMAT_CANDIDATES] = { VK_FORMAT_D32_SFLOAT_S8_UINT, VK_FORMAT_D24_UNORM_S8_UINT, VK_FORMAT_D32_SFLOAT, VK_FORMAT_D16_UNORM_S8_UINT, VK_FORMAT_D16_UNORM };

		// Steps of the grid a quantised depth sits on, the same as the fragment stage's.
		// A power of two, so a value on the grid is exact as a float.
		constexpr float DEPTH_STEPS = 65536.0f;

		/** Whether a depth format has a stencil. */
		bool HasStencilComponent(VkFormat format)
		{
			return format == VK_FORMAT_D16_UNORM_S8_UINT || format == VK_FORMAT_D24_UNORM_S8_UINT || format == VK_FORMAT_D32_SFLOAT_S8_UINT;
		}

		/** Whether a depth format keeps 16 bits of depth, like the DirectX renderer's. */
		bool IsSixteenBitDepth(VkFormat format)
		{
			return format == VK_FORMAT_D16_UNORM || format == VK_FORMAT_D16_UNORM_S8_UINT;
		}
	}

	//// Private Functions

	bool VulkanBackend::ChooseDepthFormat(void)
	{
		// Pick a depth format
		//
		// 16-bit first, because that is what the game's DirectX renderer gets: it asks
		// Direct3D 7 for the depth formats of its device and keeps the first one listed
		// (the callback at 0x88bdc0, used at 0x88c2a8), and NVIDIA's driver lists 16-bit
		// first. Content is made against that precision. Ground lights sit on their models
		// and are drawn after them, testing less-or-equal without writing, and the same
		// surface redrawn into a scroll strip can come out a step or two of a 32-bit float
		// nearer, so a float buffer failed the lights along the strips and left lines. A
		// 16-bit buffer rounds both to the same value, as on DirectX.
		//
		// With a stencil as well, which the game never uses but SC4RenderServices does, to
		// draw each terrain decal back over the shadows once per pixel. Not every device
		// pairs 16-bit depth with a stencil: an AMD Radeon integrated GPU offers
		// D16_UNORM_S8_UINT, an RTX 3070 only the 24-bit and 32-bit pairs. On those the
		// fragment stage rounds every depth onto a 16-bit grid before it is tested and
		// written, which keeps the comparisons a 16-bit buffer makes. The clears are
		// rounded the same way.
		//
		// Use32BitDepth in scvk.ini puts the 32-bit float first instead, as up to 0.2.0,
		// and nothing is rounded. Surfaces that sit very close together then stop banding,
		// but the lights above can leave lines again.
		VkFormat const* const candidates = shouldUse32BitDepth ? THIRTY_TWO_BIT_DEPTH_FIRST : SIXTEEN_BIT_DEPTH_FIRST;

		depthBuffer.format = VK_FORMAT_UNDEFINED;
		hasStencil         = false;
		isDepthQuantised   = false;
		isDepthSampleable  = false;

		for (size_t index = 0; index < DEPTH_FORMAT_CANDIDATES; ++index)
		{
			VkFormat const candidate = candidates[index];

			VkFormatProperties properties{};
			vkGetPhysicalDeviceFormatProperties(physicalDevice, candidate, &properties);

			if ((properties.optimalTilingFeatures & VK_FORMAT_FEATURE_DEPTH_STENCIL_ATTACHMENT_BIT) == 0)
			{
				continue;
			}

			depthBuffer.format = candidate;
			hasStencil         = HasStencilComponent(candidate);
			isDepthQuantised   = !shouldUse32BitDepth && !IsSixteenBitDepth(candidate);

			// Sampling it is what the shadow composite and ReShade's depth need
			isDepthSampleable = (properties.optimalTilingFeatures & VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT) != 0;
			break;
		}

		if (depthBuffer.format == VK_FORMAT_UNDEFINED)
		{
			LogError("Vulkan: no usable depth format.");
			return false;
		}

		if (shouldUse32BitDepth && depthBuffer.format != VK_FORMAT_D32_SFLOAT && depthBuffer.format != VK_FORMAT_D32_SFLOAT_S8_UINT)
		{
			LogWarn("Vulkan: Use32BitDepth is set, but the device has no 32-bit depth format; using format %d.", depthBuffer.format);
		}

		// The video mode still says there is a stencil, since the game reads its modes
		// before there is a device. Without one the stencil calls do nothing, which is
		// what they did before scvk had a stencil.
		if (!hasStencil)
		{
			LogWarn("Vulkan: the device has no depth format with a stencil; stencil calls will do nothing.");
		}

		return true;
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
		barrier.subresourceRange.aspectMask = region.isDepth ? DepthAspects() : VkImageAspectFlags{ VK_IMAGE_ASPECT_COLOR_BIT };
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
		// the smaller of the two limits governs. Region sizes match the render size, far
		// below INT32_MAX, so they convert without loss.
		int32_t const regionWidth  = static_cast<int32_t>(region.width);
		int32_t const regionHeight = static_cast<int32_t>(region.height);

		width  = std::min(width, std::min(RenderWidth() - screenX, regionWidth - regionX));
		height = std::min(height, std::min(RenderHeight() - screenY, regionHeight - regionY));
		return width > 0 && height > 0;
	}

	bool VulkanBackend::AllocateRegionImage(bool isDepth, BufferRegion& outRegion)
	{
		BufferRegion region;
		region.isDepth = isDepth;
		region.format  = isDepth ? depthBuffer.format : backBuffer.format;
		region.width   = renderWidth;
		region.height  = renderHeight;

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
			LogError("Vulkan: could not create a buffer region: %s", VkResultName(result));
			NoteDeviceLoss(result);
			return false;
		}

		// Back it with device-local memory
		VkMemoryRequirements requirements{};
		vkGetImageMemoryRequirements(device, region.image, &requirements);

		uint32_t typeIndex = 0;
		if (!FindMemoryType(requirements.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT, typeIndex))
		{
			vkDestroyImage(device, region.image, nullptr);
			return false;
		}

		VkMemoryAllocateInfo allocationInformation{ VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO };
		allocationInformation.allocationSize  = requirements.size;
		allocationInformation.memoryTypeIndex = typeIndex;

		result = AllocateDeviceMemory(allocationInformation, region.memory);
		if (result != VK_SUCCESS)
		{
			LogError("Vulkan: could not allocate a buffer region: %s", VkResultName(result));
			NoteDeviceLoss(result);
			vkDestroyImage(device, region.image, nullptr);
			return false;
		}

		vkBindImageMemory(device, region.image, region.memory, 0);
		region.isLive = true;

		outRegion = region;
		return true;
	}

	void VulkanBackend::ClearDepthAttachment(VkImageAspectFlags aspects, VkClearDepthStencilValue const& value)
	{
		if (!EnsureFrame() || depthBuffer.image == VK_NULL_HANDLE)
		{
			return;
		}

		PhaseScope const recording(*this, FRAME_PHASE_RECORDING);

		// Clear inside the pass, only the scissor under a sub-viewport
		//
		// The same way as the colour clear: the pass stays open and the depth buffer in
		// its attachment layout, and the clear is ordered against the draws around it by
		// the pass itself. Only the aspects asked for, so a depth clear keeps the stencil
		// and a stencil clear keeps the depth.
		VkRect2D scissor{};
		ViewportRectangle(scissor);

		if (scissor.extent.width == 0 || scissor.extent.height == 0)
		{
			return;
		}

		BeginRenderPassIfNeeded();

		VkClearAttachment attachment{};
		attachment.aspectMask              = aspects;
		attachment.clearValue.depthStencil = value;

		VkClearRect clearRectangle{};
		clearRectangle.rect       = scissor;
		clearRectangle.layerCount = 1;

		vkCmdClearAttachments(commandBuffer, 1, &attachment, 1, &clearRectangle);
	}

	//// Public API

	void VulkanBackend::ClearDepth(float depth)
	{
		// Round the value up onto the grid the fragment stage rounds to, as a 16-bit
		// buffer would store it. Left as it is, a clear could sit between two steps, and
		// a surface drawn at the cleared depth would then test nearer or farther than it
		// does on DirectX.
		float const clamped = std::clamp(depth, 0.0f, 1.0f);
		float const value   = isDepthQuantised ? std::ceil(clamped * DEPTH_STEPS) / DEPTH_STEPS : clamped;

		ClearDepthAttachment(VK_IMAGE_ASPECT_DEPTH_BIT, VkClearDepthStencilValue{ value, 0 });
	}

	void VulkanBackend::ClearStencil(uint32_t value)
	{
		if (!hasStencil)
		{
			return;
		}

		ClearDepthAttachment(VK_IMAGE_ASPECT_STENCIL_BIT, VkClearDepthStencilValue{ 0.0f, value });
	}

	uint32_t VulkanBackend::CreateBufferRegion(bool isDepth)
	{
		if (isDead || device == VK_NULL_HANDLE || renderWidth == 0 || backBuffer.image == VK_NULL_HANDLE)
		{
			return 0;
		}

		if (isDepth && depthBuffer.image == VK_NULL_HANDLE)
		{
			return 0;
		}

		// Create an image the size of the back buffer, in the format it copies
		BufferRegion region;
		if (!AllocateRegionImage(isDepth, region))
		{
			return 0;
		}

		// Reuse a dead slot before growing
		//
		// So a game that cycles regions does not walk the handle space upward forever.
		for (size_t i = 0; i < bufferRegions.size(); i++)
		{
			if (!bufferRegions[i].isLive)
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
		return handle != 0 && handle <= bufferRegions.size() && bufferRegions[handle - 1].isLive;
	}

	bool VulkanBackend::SaveBufferRegion(uint32_t handle, int32_t regionX, int32_t regionY, int32_t width, int32_t height, int32_t screenX, int32_t screenY)
	{
		if (!IsBufferRegion(handle) || width <= 0 || height <= 0 || !EnsureFrame())
		{
			return false;
		}

		PhaseScope const recording(*this, FRAME_PHASE_RECORDING);
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
		//
		// The depth buffer and the back buffer track their own layouts, and the next pass
		// moves them back into the attachment ones.
		if (region.isDepth)
		{
			TransitionDepth(VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL);
		}
		else
		{
			TransitionTo(VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL);
		}

		VkImage const source = region.isDepth ? depthBuffer.image : backBuffer.image;

		// Copy into the region
		TransitionRegion(region, region.hasContent ? VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL : VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);

		// Both extents were clamped to more than zero above.
		VkImageCopy copy{};
		copy.srcSubresource.aspectMask = region.isDepth ? DepthAspects() : VkImageAspectFlags{ VK_IMAGE_ASPECT_COLOR_BIT };
		copy.srcSubresource.layerCount = 1;
		copy.srcOffset = { screenX, screenY, 0 };
		copy.dstSubresource.aspectMask = copy.srcSubresource.aspectMask;
		copy.dstSubresource.layerCount = 1;
		copy.dstOffset = { regionX, regionY, 0 };
		copy.extent    = { static_cast<uint32_t>(width), static_cast<uint32_t>(height), 1 };

		vkCmdCopyImage(commandBuffer, source, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, region.image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &copy);

		// Leave the region ready to be read
		//
		// Restoring is the only thing that happens to a region after it has been saved.
		TransitionRegion(region, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL);
		region.hasContent = true;
		return true;
	}

	bool VulkanBackend::RestoreBufferRegion(uint32_t handle, int32_t regionX, int32_t regionY, int32_t width, int32_t height, int32_t screenX, int32_t screenY)
	{
		if (!IsBufferRegion(handle) || width <= 0 || height <= 0 || !EnsureFrame())
		{
			return false;
		}

		PhaseScope const recording(*this, FRAME_PHASE_RECORDING);
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
		// From the layout it is really in, which the tracking keeps. UNDEFINED would let
		// the driver discard the depth outside the rectangle being restored.
		if (region.isDepth)
		{
			TransitionDepth(VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
		}
		else
		{
			TransitionTo(VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
		}

		VkImage const destination = region.isDepth ? depthBuffer.image : backBuffer.image;

		// Copy out of the region
		//
		// Both extents were clamped to more than zero above.
		VkImageCopy copy{};
		copy.srcSubresource.aspectMask = region.isDepth ? DepthAspects() : VkImageAspectFlags{ VK_IMAGE_ASPECT_COLOR_BIT };
		copy.srcSubresource.layerCount = 1;
		copy.srcOffset = { regionX, regionY, 0 };
		copy.dstSubresource.aspectMask = copy.srcSubresource.aspectMask;
		copy.dstSubresource.layerCount = 1;
		copy.dstOffset = { screenX, screenY, 0 };
		copy.extent    = { static_cast<uint32_t>(width), static_cast<uint32_t>(height), 1 };

		vkCmdCopyImage(commandBuffer, region.image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, destination, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &copy);
		return true;
	}

	void VulkanBackend::DestroyBufferRegion(uint32_t handle)
	{
		if (!IsBufferRegion(handle))
		{
			return;
		}

		// Retire the image rather than destroying it here
		//
		// The game deletes its regions as it shuts down, while the frame it last submitted
		// may still be copying from them, which the validation layers reported. A frame
		// in progress may also have recorded copies that are not submitted yet, so the
		// image has to outlive that frame too, the same as a deleted texture.
		BufferRegion& region = bufferRegions[handle - 1];

		RetiredImage retired;
		retired.image  = region.image;
		retired.memory.memory = region.memory;

		Retire(retired);

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
