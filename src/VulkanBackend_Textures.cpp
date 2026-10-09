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
 * Textures and samplers.
 *
 * Each texture owns an image, a view and a descriptor set that never changes, and a range
 * of a memory block it shares with other textures. Uploads are gathered into batches that
 * run ahead of the frame drawing with them, staged straight into the batch's arena in the
 * image's own layout. Filter and wrap live on each stage, as they do on a Direct3D texture
 * stage, and pick a sampler from a small cache when a draw binds them.
 *
 * Uncompressed textures are B8G8R8A8, the order the game hands most of them over in, so
 * the common upload is a plain copy, as it is for SCD3D11's B8G8R8A8 textures.
 */

//// Dependencies

#include "VulkanBackend.h"
#include "Logger.h"
#include "TextureUploadUtils.h"

#include <windows.h>
#include <algorithm>
#include <stdio.h>
#include <vector>
#include <string.h>

namespace scvk
{
	//// Types

	namespace
	{
		/**
		 * Adds the time between its construction and its destruction to a running total,
		 * so a function with several early returns is timed on every path.
		 */
		class TickAccumulator
		{
		public:
			explicit TickAccumulator(int64_t& total) : total(total), startTicks(ReadTicks()) {}
			~TickAccumulator() { total += ReadTicks() - startTicks; }

			TickAccumulator(TickAccumulator const&) = delete;
			TickAccumulator& operator=(TickAccumulator const&) = delete;

		private:
			static int64_t ReadTicks(void)
			{
				LARGE_INTEGER now{};
				QueryPerformanceCounter(&now);
				return now.QuadPart;
			}

			int64_t& total;
			int64_t  startTicks;
		};
	}

	//// Constants

	namespace
	{
		// One descriptor set per texture, allocated when the texture is created and never
		// rewritten, so nothing can be updated while the GPU is reading it. A pool holds
		// this many, and another pool is added when they are all taken: an unmodded city
		// already reached 2848 live textures within minutes.
		constexpr uint32_t TEXTURE_SETS_PER_POOL = 4096;

		// Shrinks every texture pool, so an ordinary session fills many of them and the
		// chain gets exercised without needing thousands of custom buildings.
		constexpr char const* SMALL_TEXTURE_POOLS_MARKER = "scvk-small-texture-pools";
		constexpr uint32_t    SMALL_TEXTURE_SETS_PER_POOL = 64;

		// Textures share device-local blocks of this size. A modded city held 10195
		// textures in 142 MB, three blocks, where it had been 10195 allocations. The
		// memory is never mapped, so unlike the arenas it costs the 32-bit process no
		// address space.
		constexpr VkDeviceSize TEXTURE_BLOCK_SIZE = 64ull * 1024 * 1024;

		// Staging for a batch of texture uploads. A block holds a 2048x2048 RGBA level,
		// past anything the game was seen to upload; a larger one gets a buffer of its own.
		// A batch that fills both blocks is submitted and waited for early.
		constexpr VkDeviceSize TEXTURE_UPLOAD_BLOCK_SIZE     = 16ull * 1024 * 1024;
		constexpr size_t       TEXTURE_UPLOAD_MAXIMUM_BLOCKS = 2;

		// A copy's buffer offset must be a multiple of the texel block size, which is 16
		// bytes at most here (DXT3 and DXT5).
		constexpr VkDeviceSize TEXTURE_UPLOAD_ALIGNMENT = 16;

		// The fixed samplers of scvk's own passes come from the sampler pool as well.
		constexpr uint32_t FIXED_SAMPLERS = 3;

		// The game's upload enumerations, from SCGL's translation tables. Formats: 0 RGB,
		// 1 RGBA, 2 BGR, 3 BGRA. Types: 1 GL_UNSIGNED_BYTE, 8 GL_UNSIGNED_SHORT_4_4_4_4
		// and 13 GL_UNSIGNED_SHORT_4_4_4_4_REV.
		constexpr uint32_t GD_FORMAT_RGBA = 1;
		constexpr uint32_t GD_FORMAT_BGR  = 2;
		constexpr uint32_t GD_FORMAT_BGRA = 3;

		constexpr uint32_t GD_TYPE_UNSIGNED_BYTE     = 1;
		constexpr uint32_t GD_TYPE_4444              = 8;
		constexpr uint32_t GD_TYPE_4444_REVERSED     = 13;

		// The game's internal format for plain RGBA8, which the default texture uses.
		constexpr uint32_t GD_INTERNAL_FORMAT_RGBA8 = 4;

		// Texture dumps wait until the startup screen is over, since its splash tiles
		// were dumped once and turned out to be perfectly correct.
		constexpr uint64_t TEXTURE_DUMP_AFTER_FRAMES = 1000;

		// A texture this small keeps the start of its top level for the log.
		constexpr uint32_t TINY_TEXTURE_TEXELS = 16;

		// A live texture no draw has sampled for this many frames counts as idle. It
		// matches the heartbeat interval, so idle means unused since the last heartbeat.
		constexpr uint64_t IDLE_TEXTURE_FRAMES = 300;
	}

	//// Private Functions

	namespace
	{
		/** The game's internal texture format enumeration, mapped to Vulkan. */
		VkFormat MapInternalFormat(uint32_t gdInternalFormat, bool& outIsCompressed)
		{
			outIsCompressed = true;

			switch (gdInternalFormat)
			{
			case 5: return VK_FORMAT_BC1_RGBA_UNORM_BLOCK;   // DXT1
			case 6: return VK_FORMAT_BC2_UNORM_BLOCK;        // DXT3
			case 7: return VK_FORMAT_BC3_UNORM_BLOCK;        // DXT5

			default:
				// RGB5, RGB8, RGBA4, RGB5_A1 and RGBA8 all become BGRA8. The narrower
				// ones lose nothing that matters here, and the upload path only ever
				// hands over 8 bits per channel anyway. BGRA because that is the order
				// the game's interface textures arrive in, so they are copied unchanged.
				outIsCompressed = false;
				return VK_FORMAT_B8G8R8A8_UNORM;
			}
		}

		/** Compressed size for a DXT level, in bytes. */
		VkDeviceSize CompressedSize(VkFormat format, uint32_t width, uint32_t height)
		{
			VkDeviceSize const blocks = VkDeviceSize{ (width + 3u) / 4u } * ((height + 3u) / 4u);

			// DXT1 packs a 4x4 block into 8 bytes; DXT3 and DXT5 add 8 more for the alpha
			// block.
			return blocks * ((format == VK_FORMAT_BC1_RGBA_UNORM_BLOCK) ? 8u : 16u);
		}

		/**
		 * The game's filter values, from its own translation table: 0 nearest, 1 linear,
		 * then the four mipmapped minification filters in the order nearest/linear
		 * crossed with nearest/linear.
		 */
		VkFilter MapFilter(uint32_t gdFilter)
		{
			return (gdFilter == 0 || gdFilter == 4 || gdFilter == 6) ? VK_FILTER_NEAREST : VK_FILTER_LINEAR;
		}

		/**
		 * The game's wrap values: 2 clamp, 3 repeat.
		 *
		 * Clamp is to the edge texel, as the DirectX driver does with D3DTADDRESS_CLAMP.
		 * An earlier version clamped to a transparent black border like GL_CLAMP, and
		 * linear filtering then blended every clamped tile's outer half texel toward
		 * black, which drew dark seams along the terrain and lot tile edges. Textures
		 * that must leave nothing outside their footprint, like the building shadow mask,
		 * carry their own transparent edge texels instead.
		 */
		VkSamplerAddressMode MapAddressMode(uint32_t gdWrap)
		{
			return (gdWrap == 2) ? VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE : VK_SAMPLER_ADDRESS_MODE_REPEAT;
		}
	}

	bool VulkanBackend::CreateDescriptorResources(void)
	{
		if (imageSetLayout != VK_NULL_HANDLE)
		{
			return true;
		}

		// Create the image set layout
		VkDescriptorSetLayoutBinding binding{};
		binding.binding         = 0;
		binding.descriptorType  = VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE;
		binding.descriptorCount = 1;
		binding.stageFlags      = VK_SHADER_STAGE_FRAGMENT_BIT;

		VkDescriptorSetLayoutCreateInfo layoutInformation{ VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO };
		layoutInformation.bindingCount = 1;
		layoutInformation.pBindings    = &binding;

		VkResult result = vkCreateDescriptorSetLayout(device, &layoutInformation, nullptr, &imageSetLayout);
		if (result != VK_SUCCESS)
		{
			Fail("vkCreateDescriptorSetLayout", result);
			return false;
		}

		// Create the sampler pool
		//
		// Samplers are their own descriptor type and their own sets, one per distinct
		// filter and wrap combination the game asks for, and the fixed ones of scvk's own
		// passes. Texture sets live in pools of their own, made by AllocateTextureSet as
		// they are needed.
		VkDescriptorPoolSize samplerPoolSize{};
		samplerPoolSize.type            = VK_DESCRIPTOR_TYPE_SAMPLER;
		samplerPoolSize.descriptorCount = MAXIMUM_SAMPLERS + FIXED_SAMPLERS;

		VkDescriptorPoolCreateInfo poolInformation{ VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO };
		poolInformation.flags         = VK_DESCRIPTOR_POOL_CREATE_FREE_DESCRIPTOR_SET_BIT;
		poolInformation.maxSets       = MAXIMUM_SAMPLERS + FIXED_SAMPLERS;
		poolInformation.poolSizeCount = 1;
		poolInformation.pPoolSizes    = &samplerPoolSize;

		textureSetsPerPool = TEXTURE_SETS_PER_POOL;
		if (HasMarkerFile(SMALL_TEXTURE_POOLS_MARKER))
		{
			textureSetsPerPool = SMALL_TEXTURE_SETS_PER_POOL;
			LogInfo("Vulkan: %s present, texture pools hold %u sets.", SMALL_TEXTURE_POOLS_MARKER, textureSetsPerPool);
		}

		result = vkCreateDescriptorPool(device, &poolInformation, nullptr, &descriptorPool);
		if (result != VK_SUCCESS)
		{
			Fail("vkCreateDescriptorPool", result);
			return false;
		}

		// Create the sampler set layout, whose sets carry nothing but a sampler
		VkDescriptorSetLayoutBinding samplerBinding{};
		samplerBinding.binding         = 0;
		samplerBinding.descriptorType  = VK_DESCRIPTOR_TYPE_SAMPLER;
		samplerBinding.descriptorCount = 1;
		samplerBinding.stageFlags      = VK_SHADER_STAGE_FRAGMENT_BIT;

		VkDescriptorSetLayoutCreateInfo samplerLayoutInformation{ VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO };
		samplerLayoutInformation.bindingCount = 1;
		samplerLayoutInformation.pBindings    = &samplerBinding;

		result = vkCreateDescriptorSetLayout(device, &samplerLayoutInformation, nullptr, &samplerSetLayout);
		if (result != VK_SUCCESS)
		{
			Fail("vkCreateDescriptorSetLayout (sampler)", result);
			return false;
		}

		// Reserve command buffers and fences for work outside the frame
		//
		// One runs the texture batches, the other anything waited for at once. They come
		// from the utility pool, since the frames' pools are reset as a whole.
		VkCommandBufferAllocateInfo allocationInformation{ VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO };
		allocationInformation.commandPool        = utilityCommandPool;
		allocationInformation.level              = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
		allocationInformation.commandBufferCount = 1;

		result = vkAllocateCommandBuffers(device, &allocationInformation, &uploadCommandBuffer);
		if (result != VK_SUCCESS)
		{
			Fail("vkAllocateCommandBuffers (upload)", result);
			return false;
		}

		result = vkAllocateCommandBuffers(device, &allocationInformation, &textureBatchCommandBuffer);
		if (result != VK_SUCCESS)
		{
			Fail("vkAllocateCommandBuffers (texture batch)", result);
			return false;
		}

		VkFenceCreateInfo fenceInformation{ VK_STRUCTURE_TYPE_FENCE_CREATE_INFO };
		result = vkCreateFence(device, &fenceInformation, nullptr, &uploadFence);
		if (result != VK_SUCCESS)
		{
			Fail("vkCreateFence (upload)", result);
			return false;
		}

		result = vkCreateFence(device, &fenceInformation, nullptr, &textureBatchFence);
		if (result != VK_SUCCESS)
		{
			Fail("vkCreateFence (texture batch)", result);
			return false;
		}

		isTextureBatchOpen     = false;
		isTextureBatchInFlight = false;

		// Create the staging arena for texture uploads
		//
		// Unlike the frame arenas it is rewound as a whole whenever a batch has finished,
		// so within a batch it only ever moves forward.
		textureUploadArena.name          = "texture upload";
		textureUploadArena.usage         = VK_BUFFER_USAGE_TRANSFER_SRC_BIT;
		textureUploadArena.blockSize     = TEXTURE_UPLOAD_BLOCK_SIZE;
		textureUploadArena.maximumBlocks = TEXTURE_UPLOAD_MAXIMUM_BLOCKS;

		if (!ArenaAddBlock(textureUploadArena))
		{
			return false;
		}

		return CreateFixedSamplers() && CreateDefaultTexture();
	}

	bool VulkanBackend::CreateFixedSamplers(void)
	{
		// Point and linear, clamped, for the blits and the full-screen passes, and linear
		// repeating for the shadow casters' alpha
		struct FixedSampler
		{
			VkFilter             filter;
			VkSamplerAddressMode address;
			VkSampler*           sampler;
			VkDescriptorSet*     set;
		};

		FixedSampler const fixedSamplers[FIXED_SAMPLERS] = {
			{ VK_FILTER_NEAREST, VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE, &pointClampSampler, &pointClampSet },
			{ VK_FILTER_LINEAR, VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE, &linearClampSampler, &linearClampSet },
			{ VK_FILTER_LINEAR, VK_SAMPLER_ADDRESS_MODE_REPEAT, &linearWrapSampler, &linearWrapSet },
		};

		for (FixedSampler const& fixed : fixedSamplers)
		{
			VkSamplerCreateInfo information{ VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO };
			information.magFilter    = fixed.filter;
			information.minFilter    = fixed.filter;
			information.mipmapMode   = VK_SAMPLER_MIPMAP_MODE_NEAREST;
			information.addressModeU = fixed.address;
			information.addressModeV = fixed.address;
			information.addressModeW = fixed.address;
			information.maxLod       = 0.25f;

			VkResult result = vkCreateSampler(device, &information, nullptr, fixed.sampler);
			if (result != VK_SUCCESS)
			{
				Fail("vkCreateSampler (fixed)", result);
				return false;
			}

			VkDescriptorSetAllocateInfo allocationInformation{ VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO };
			allocationInformation.descriptorPool     = descriptorPool;
			allocationInformation.descriptorSetCount = 1;
			allocationInformation.pSetLayouts        = &samplerSetLayout;

			result = vkAllocateDescriptorSets(device, &allocationInformation, fixed.set);
			if (result != VK_SUCCESS)
			{
				Fail("vkAllocateDescriptorSets (fixed sampler)", result);
				return false;
			}

			VkDescriptorImageInfo imageInformation{};
			imageInformation.sampler = *fixed.sampler;

			VkWriteDescriptorSet write{ VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET };
			write.dstSet          = *fixed.set;
			write.dstBinding      = 0;
			write.descriptorCount = 1;
			write.descriptorType  = VK_DESCRIPTOR_TYPE_SAMPLER;
			write.pImageInfo      = &imageInformation;

			vkUpdateDescriptorSets(device, 1, &write, 0, nullptr);
		}

		return true;
	}

	bool VulkanBackend::CreateDefaultTexture(void)
	{
		// Create a single white texel
		//
		// Draws with no texture bound sample this, so modulate becomes a multiply by one
		// and untextured geometry needs no separate shader or pipeline.
		textures.clear();
		textures.emplace_back();

		uint32_t const handle = CreateTexture(GD_INTERNAL_FORMAT_RGBA8, 1, 1, 1);
		if (handle == 0)
		{
			return false;
		}

		uint8_t const white[4] = { 255, 255, 255, 255 };
		UploadTextureLevel(handle, 0, 0, 0, 1, 1, GD_FORMAT_RGBA, GD_TYPE_UNSIGNED_BYTE, 0, white);

		// Move it into slot 0, so an unset texture resolves to it naturally
		//
		// The source slot must be blanked, not just marked dead: a copy leaves both
		// entries owning the same image, view and memory, and teardown walks every slot,
		// so leaving it would destroy each of them twice.
		textures[0] = textures[handle];
		textures[handle] = Texture{};
		textures.pop_back();

		return true;
	}

	void VulkanBackend::DestroyTextures(void)
	{
		// Drop a batch never submitted, and wait out one that was
		//
		// Nothing draws with these textures again, so the batch need not run.
		if (isTextureBatchOpen)
		{
			vkEndCommandBuffer(textureBatchCommandBuffer);
			isTextureBatchOpen = false;
		}

		WaitForTextureBatch();
		DestroyArena(textureUploadArena);

		for (Texture& texture : textures)
		{
			if (texture.view != VK_NULL_HANDLE)  { vkDestroyImageView(device, texture.view, nullptr); }
			if (texture.image != VK_NULL_HANDLE) { vkDestroyImage(device, texture.image, nullptr); }
			ReleaseImageMemory(texture.memory);

			texture = Texture{};
		}

		textures.clear();
		currentTexture  = 0;
		currentTexture1 = 0;

		// Free the blocks, now that no image is bound into them
		for (TextureBlock& block : textureBlocks)
		{
			FreeDeviceMemory(block.memory);
		}

		textureBlocks.clear();

		// Destroying a pool frees every set still allocated from it
		for (TexturePool const& texturePool : texturePools)
		{
			vkDestroyDescriptorPool(device, texturePool.pool, nullptr);
		}

		texturePools.clear();
		blitImageSet = VK_NULL_HANDLE;
	}

	void VulkanBackend::FlushRetired(FrameSlot& slot)
	{
		if (device == VK_NULL_HANDLE)
		{
			slot.retiredImages.clear();
			slot.retiredBuffers.clear();
			slot.retiredMemory.clear();
			return;
		}

		PhaseScope const releasing(*this, FRAME_PHASE_TEXTURES);

		// Called once the slot's fence, or the whole device, has been waited on, which
		// means every command that could still have been reading these has completed.
		// Texture batches go to the queue ahead of the frame that retired anything they
		// name, so they have finished too.
		for (RetiredImage& retired : slot.retiredImages)
		{
			if (retired.secondView != VK_NULL_HANDLE) { vkDestroyImageView(device, retired.secondView, nullptr); }
			if (retired.view != VK_NULL_HANDLE)       { vkDestroyImageView(device, retired.view, nullptr); }
			if (retired.image != VK_NULL_HANDLE)      { vkDestroyImage(device, retired.image, nullptr); }
			ReleaseImageMemory(retired.memory);

			// The descriptor set matters as much as the image. Each pool is capped, and
			// leaking sets would add a pool every few thousand deletions.
			if (retired.descriptor != VK_NULL_HANDLE && retired.descriptorPoolIndex < texturePools.size())
			{
				TexturePool& texturePool = texturePools[retired.descriptorPoolIndex];
				vkFreeDescriptorSets(device, texturePool.pool, 1, &retired.descriptor);
				texturePool.usedSets--;
			}
		}

		for (VkBuffer buffer : slot.retiredBuffers)
		{
			vkDestroyBuffer(device, buffer, nullptr);
		}

		for (VkDeviceMemory& memory : slot.retiredMemory)
		{
			FreeDeviceMemory(memory);
		}

		slot.retiredImages.clear();
		slot.retiredBuffers.clear();
		slot.retiredMemory.clear();
	}

	bool VulkanBackend::AllocateTextureMemory(VkMemoryRequirements const& requirements, ImageMemory& outMemory)
	{
		TickAccumulator const timer(textureMemoryTicks);

		uint32_t typeIndex = 0;
		if (!FindMemoryType(requirements.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT, typeIndex))
		{
			LogError("Vulkan: no device-local memory type for textures.");
			return false;
		}

		VkMemoryAllocateInfo allocationInformation{ VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO };
		allocationInformation.memoryTypeIndex = typeIndex;

		// Give a texture larger than a block an allocation of its own
		if (requirements.size > TEXTURE_BLOCK_SIZE)
		{
			allocationInformation.allocationSize = requirements.size;

			ImageMemory dedicated;
			VkResult const result = AllocateDeviceMemory(allocationInformation, dedicated.memory);
			if (result != VK_SUCCESS)
			{
				LogError("Vulkan: could not allocate %llu bytes for a texture: %s", requirements.size, VkResultName(result));
				NoteDeviceLoss(result);
				return false;
			}

			dedicated.size = requirements.size;
			outMemory = dedicated;
			return true;
		}

		// Take the first block of the same memory type with room
		//
		// Every image here has optimal tiling, so the spacing bufferImageGranularity
		// demands between linear and optimal resources never comes into it.
		for (uint32_t i = 0; i < textureBlocks.size(); i++)
		{
			TextureBlock& block = textureBlocks[i];

			MemoryRange  range;
			VkDeviceSize offset = 0;
			if (block.memoryTypeIndex != typeIndex || !TakeBlockRange(block, requirements.size, requirements.alignment, range, offset))
			{
				continue;
			}

			outMemory.memory      = block.memory;
			outMemory.offset      = offset;
			outMemory.size        = requirements.size;
			outMemory.blockIndex  = i;
			outMemory.isDedicated = false;
			outMemory.range       = range;
			return true;
		}

		// Add a block when none has room
		TextureBlock block;
		block.memoryTypeIndex  = typeIndex;
		block.largestFreeBound = TEXTURE_BLOCK_SIZE;
		block.freeRanges.push_back(MemoryRange{ 0, TEXTURE_BLOCK_SIZE });

		allocationInformation.allocationSize = TEXTURE_BLOCK_SIZE;

		VkResult const result = AllocateDeviceMemory(allocationInformation, block.memory);
		if (result != VK_SUCCESS)
		{
			// Out of video memory is not the end of the backend: this texture is left
			// out, and the game carries on with the white one in its place
			LogError("Vulkan: could not allocate another texture block: %s", VkResultName(result));
			NoteDeviceLoss(result);
			return false;
		}

		// A fresh block starts at offset 0, which suits any alignment.
		MemoryRange  range;
		VkDeviceSize offset = 0;
		TakeBlockRange(block, requirements.size, requirements.alignment, range, offset);

		textureBlocks.push_back(block);
		LogDebug("Vulkan: textures now take %zu blocks of %llu MB; %u memory allocations in all.", textureBlocks.size(), TEXTURE_BLOCK_SIZE >> 20, liveMemoryAllocations);

		outMemory.memory      = block.memory;
		outMemory.offset      = offset;
		outMemory.size        = requirements.size;
		outMemory.blockIndex  = static_cast<uint32_t>(textureBlocks.size() - 1);
		outMemory.isDedicated = false;
		outMemory.range       = range;
		return true;
	}

	bool VulkanBackend::TakeBlockRange(TextureBlock& block, VkDeviceSize size, VkDeviceSize alignment, MemoryRange& outRange, VkDeviceSize& outOffset)
	{
		// Pass over a block with no range large enough
		if (size > block.largestFreeBound)
		{
			return false;
		}

		VkDeviceSize largestSeen = 0;

		for (std::vector<MemoryRange>::iterator range = block.freeRanges.begin(); range != block.freeRanges.end(); ++range)
		{
			if (range->size > largestSeen)
			{
				largestSeen = range->size;
			}

			// Skip a range the aligned start leaves too little of
			//
			// Vulkan guarantees the alignment is a power of two.
			VkDeviceSize const aligned = (range->offset + alignment - 1u) & ~(alignment - 1u);
			VkDeviceSize const end     = range->offset + range->size;

			if (aligned >= end || end - aligned < size)
			{
				continue;
			}

			// Take the alignment gap along with the image
			//
			// Left free, every gap became a range of its own, too small for most
			// textures, and every later search walked all of them: a city with 14000
			// textures took 1.8 ms to create each one. The gaps came to 2 KB a texture at
			// most, and the whole stretch goes back when the texture is deleted.
			outRange  = MemoryRange{ range->offset, aligned + size - range->offset };
			outOffset = aligned;

			if (end > aligned + size)
			{
				*range = MemoryRange{ aligned + size, end - (aligned + size) };
			}
			else
			{
				block.freeRanges.erase(range);
			}

			block.usedBytes += outRange.size;
			return true;
		}

		// The whole list was walked, so its largest range is now known exactly
		block.largestFreeBound = largestSeen;
		return false;
	}

	void VulkanBackend::ReturnBlockRange(TextureBlock& block, MemoryRange range)
	{
		// Find the first free range after it
		std::vector<MemoryRange>::iterator next = block.freeRanges.begin();
		while (next != block.freeRanges.end() && next->offset < range.offset)
		{
			++next;
		}

		block.usedBytes -= range.size;

		// Merge it with the range before, the range after, or both
		bool const touchesBefore = next != block.freeRanges.begin() && (next - 1)->offset + (next - 1)->size == range.offset;
		bool const touchesAfter  = next != block.freeRanges.end() && range.offset + range.size == next->offset;

		VkDeviceSize mergedSize = range.size;

		if (touchesBefore && touchesAfter)
		{
			(next - 1)->size += range.size + next->size;
			mergedSize = (next - 1)->size;
			block.freeRanges.erase(next);
		}
		else if (touchesBefore)
		{
			(next - 1)->size += range.size;
			mergedSize = (next - 1)->size;
		}
		else if (touchesAfter)
		{
			next->offset = range.offset;
			next->size  += range.size;
			mergedSize = next->size;
		}
		else
		{
			block.freeRanges.insert(next, range);
		}

		// Keep the bound above every free range
		if (mergedSize > block.largestFreeBound)
		{
			block.largestFreeBound = mergedSize;
		}
	}

	void VulkanBackend::ReleaseImageMemory(ImageMemory& memory)
	{
		if (memory.memory == VK_NULL_HANDLE)
		{
			return;
		}

		if (memory.isDedicated)
		{
			FreeDeviceMemory(memory.memory);
		}
		else if (memory.blockIndex < textureBlocks.size())
		{
			ReturnBlockRange(textureBlocks[memory.blockIndex], memory.range);
		}

		memory = ImageMemory{};
	}

	VkDescriptorSet VulkanBackend::GetSamplerSet(uint32_t const parameters[4])
	{
		// Reuse the sampler made for the same parameters
		uint32_t const key = (parameters[0] & 0xff) | ((parameters[1] & 0xff) << 8) | ((parameters[2] & 0xff) << 16) | ((parameters[3] & 0xff) << 24);

		for (SamplerEntry const& entry : samplers)
		{
			if (entry.key == key)
			{
				return entry.set;
			}
		}

		VkDescriptorSet const fallback = samplers.empty() ? linearWrapSet : samplers[0].set;

		if (samplers.size() >= MAXIMUM_SAMPLERS)
		{
			return fallback;
		}

		// Create the sampler
		//
		// Only values 4 to 7 select a mipmapped minification filter. The unmipmapped ones
		// must not reach past the base level, which also keeps a texture whose upper
		// levels were never uploaded from being sampled.
		bool const isMipmapped = parameters[1] >= 4;

		VkSamplerCreateInfo information{ VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO };
		information.magFilter    = MapFilter(parameters[0]);
		information.minFilter    = MapFilter(parameters[1]);
		information.mipmapMode   = (parameters[1] == 6 || parameters[1] == 7) ? VK_SAMPLER_MIPMAP_MODE_LINEAR : VK_SAMPLER_MIPMAP_MODE_NEAREST;
		information.addressModeU = MapAddressMode(parameters[2]);
		information.addressModeV = MapAddressMode(parameters[3]);
		information.addressModeW = VK_SAMPLER_ADDRESS_MODE_REPEAT;
		information.borderColor  = VK_BORDER_COLOR_FLOAT_TRANSPARENT_BLACK;
		information.maxLod       = isMipmapped ? VK_LOD_CLAMP_NONE : 0.25f;

		SamplerEntry entry;
		entry.key = key;

		VkResult result = vkCreateSampler(device, &information, nullptr, &entry.sampler);
		if (result != VK_SUCCESS)
		{
			Fail("vkCreateSampler", result);
			return fallback;
		}

		// Give it a set of its own
		VkDescriptorSetAllocateInfo allocationInformation{ VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO };
		allocationInformation.descriptorPool     = descriptorPool;
		allocationInformation.descriptorSetCount = 1;
		allocationInformation.pSetLayouts        = &samplerSetLayout;

		result = vkAllocateDescriptorSets(device, &allocationInformation, &entry.set);
		if (result != VK_SUCCESS)
		{
			Fail("vkAllocateDescriptorSets (sampler)", result);
			vkDestroySampler(device, entry.sampler, nullptr);
			return fallback;
		}

		VkDescriptorImageInfo imageInformation{};
		imageInformation.sampler = entry.sampler;

		VkWriteDescriptorSet write{ VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET };
		write.dstSet          = entry.set;
		write.dstBinding      = 0;
		write.descriptorCount = 1;
		write.descriptorType  = VK_DESCRIPTOR_TYPE_SAMPLER;
		write.pImageInfo      = &imageInformation;

		vkUpdateDescriptorSets(device, 1, &write, 0, nullptr);

		LogDebug("Vulkan: created sampler for mag %u, min %u, wrap %u/%u.", parameters[0], parameters[1], parameters[2], parameters[3]);

		samplers.push_back(entry);
		return entry.set;
	}

	bool VulkanBackend::AllocateTextureSet(VkDescriptorSet& outSet, uint32_t& outPoolIndex)
	{
		// Find the first pool with room
		//
		// Counting is enough to know there is room. Vulkan 1.0 leaves going past a pool's
		// limits undefined rather than promising an error, and fragmentation never fails
		// an allocation when every set in the pool has the same descriptor counts, which
		// a pool of nothing but texture sets guarantees.
		uint32_t poolIndex = 0;
		while (poolIndex < texturePools.size() && texturePools[poolIndex].usedSets >= textureSetsPerPool)
		{
			poolIndex++;
		}

		// Add a pool when they are all full
		if (poolIndex == texturePools.size())
		{
			VkDescriptorPoolSize poolSize{};
			poolSize.type            = VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE;
			poolSize.descriptorCount = textureSetsPerPool;

			VkDescriptorPoolCreateInfo poolInformation{ VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO };
			poolInformation.flags         = VK_DESCRIPTOR_POOL_CREATE_FREE_DESCRIPTOR_SET_BIT;
			poolInformation.maxSets       = textureSetsPerPool;
			poolInformation.poolSizeCount = 1;
			poolInformation.pPoolSizes    = &poolSize;

			TexturePool texturePool;
			VkResult const result = vkCreateDescriptorPool(device, &poolInformation, nullptr, &texturePool.pool);
			if (result != VK_SUCCESS)
			{
				Fail("vkCreateDescriptorPool (textures)", result);
				return false;
			}

			texturePools.push_back(texturePool);
			LogDebug("Vulkan: texture descriptor pool %u added, room for %u textures in all.", poolIndex + 1, (poolIndex + 1) * textureSetsPerPool);
		}

		// Take a set from it
		VkDescriptorSetAllocateInfo setInformation{ VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO };
		setInformation.descriptorPool     = texturePools[poolIndex].pool;
		setInformation.descriptorSetCount = 1;
		setInformation.pSetLayouts        = &imageSetLayout;

		VkResult const result = vkAllocateDescriptorSets(device, &setInformation, &outSet);
		if (result != VK_SUCCESS)
		{
			Fail("vkAllocateDescriptorSets", result);
			return false;
		}

		texturePools[poolIndex].usedSets++;
		outPoolIndex = poolIndex;
		return true;
	}

	bool VulkanBackend::AllocateTransientSet(VkDescriptorSetLayout layout, VkDescriptorSet& outSet)
	{
		if (!isFrameActive)
		{
			return false;
		}

		FrameSlot& slot = frameSlots[currentSlot];

		VkDescriptorSetAllocateInfo setInformation{ VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO };
		setInformation.descriptorPool     = slot.transientPool;
		setInformation.descriptorSetCount = 1;
		setInformation.pSetLayouts        = &layout;

		// A full pool means the frame asked for more passes than it should; the pass is
		// skipped rather than the frame
		VkResult const result = vkAllocateDescriptorSets(device, &setInformation, &outSet);
		if (result != VK_SUCCESS)
		{
			if (slot.transientSets != UINT32_MAX)
			{
				LogWarn("Vulkan: no transient descriptor set left this frame (%u used): %s", slot.transientSets, VkResultName(result));
				slot.transientSets = UINT32_MAX;
			}

			return false;
		}

		if (slot.transientSets != UINT32_MAX)
		{
			slot.transientSets++;
		}

		return true;
	}

	void VulkanBackend::NoteTextureUse(uint32_t handle)
	{
		// Zero is the default white texture, which is never uploaded to.
		if (handle == 0 || handle >= textures.size())
		{
			return;
		}

		Texture& texture = textures[handle];
		texture.lastDrawnFrame = presentedFrames;

		if (texture.uploadedLevels != 0)
		{
			return;
		}

		// Count a draw that samples a texture nothing was uploaded to
		drawsBeforeUpload++;

		if (hazardNotesRemaining > 0)
		{
			hazardNotesRemaining--;
			LogDebug("  HAZARD: texture %u (%ux%u) sampled before anything was uploaded to it", handle, texture.width, texture.height);
		}
	}

	VkDeviceSize VulkanBackend::StagedBytes(Texture const& texture, uint32_t width, uint32_t height)
	{
		return texture.isCompressed ? CompressedSize(texture.format, width, height) : VkDeviceSize{ width } * height * 4u;
	}

	bool VulkanBackend::StageTexels(Texture const& texture, uint32_t width, uint32_t height, uint32_t gdFormat, uint32_t gdType, uint32_t rowLength, void const* pixels, uint8_t* outStaged)
	{
		// Copy compressed blocks as they are
		//
		// A level of a texture in a 32-bit process fits a size_t.
		if (texture.isCompressed)
		{
			memcpy(outStaged, pixels, static_cast<size_t>(CompressedSize(texture.format, width, height)));
			return true;
		}

		// Work out the component order and packing
		//
		// Texels arrive either as one byte per component, or as one native-order 16-bit
		// word with four bits per component. The packed types only exist for
		// four-component formats.
		bool const isReversed    = (gdFormat == GD_FORMAT_BGR || gdFormat == GD_FORMAT_BGRA);
		bool const hasAlpha      = (gdFormat == GD_FORMAT_RGBA || gdFormat == GD_FORMAT_BGRA);
		bool const isKnownOrder  = (gdFormat <= GD_FORMAT_BGRA);
		bool const isPacked      = (gdType == GD_TYPE_4444);
		bool const isPackedRev   = (gdType == GD_TYPE_4444_REVERSED);

		if (!isKnownOrder || (gdType != GD_TYPE_UNSIGNED_BYTE && !((isPacked || isPackedRev) && hasAlpha)))
		{
			// Any other format and type the game's tables name, read one texel at a time
			// the way SCD3D11 reads them: alpha, luminance, 16-bit and float components
			uint32_t const pixelBytes = TextureSourcePixelBytes(gdFormat, gdType);
			if (pixelBytes == 0)
			{
				LogWarn("Vulkan: texture upload format %u type %u (%ux%u) is not handled; skipping.", gdFormat, gdType, width, height);
				return false;
			}

			uint32_t const stride   = (rowLength != 0) ? rowLength : width;
			size_t const   rowBytes = (size_t{ stride } * pixelBytes + 3u) & ~size_t{ 3 };

			for (uint32_t y = 0; y < height; y++)
			{
				uint8_t const* sourceTexel    = static_cast<uint8_t const*>(pixels) + size_t{ y } * rowBytes;
				uint8_t*       destinationRow = outStaged + size_t{ y } * width * 4u;

				for (uint32_t x = 0; x < width; x++, sourceTexel += pixelBytes)
				{
					uint8_t rgba[4];
					if (!ConvertTextureSourcePixel(gdFormat, gdType, sourceTexel, rgba))
					{
						return false;
					}

					destinationRow[x * 4 + 0] = rgba[2];
					destinationRow[x * 4 + 1] = rgba[1];
					destinationRow[x * 4 + 2] = rgba[0];
					destinationRow[x * 4 + 3] = rgba[3];
				}
			}

			return true;
		}

		// Work out the source rows
		//
		// Rows are padded to the unpack alignment, which the game leaves at OpenGL's
		// default of 4 bytes. That only changes anything for 16-bit or 24-bit texels,
		// such as on an odd width.
		uint32_t const sourceStride   = (rowLength != 0) ? rowLength : width;
		uint32_t const bytesPerTexel  = (gdType != GD_TYPE_UNSIGNED_BYTE) ? 2u : (hasAlpha ? 4u : 3u);
		size_t const   sourceRowBytes = (size_t{ sourceStride } * bytesPerTexel + 3u) & ~size_t{ 3 };
		size_t const   rowBytes       = size_t{ width } * 4u;

		// The game's pixels are untyped bytes.
		uint8_t const* const source = static_cast<uint8_t const*>(pixels);

		// Copy BGRA8 as it is, the whole level at once when the rows are tight
		if (gdType == GD_TYPE_UNSIGNED_BYTE && gdFormat == GD_FORMAT_BGRA)
		{
			if (sourceRowBytes == rowBytes)
			{
				memcpy(outStaged, source, rowBytes * height);
				return true;
			}

			for (uint32_t y = 0; y < height; y++)
			{
				memcpy(outStaged + size_t{ y } * rowBytes, source + size_t{ y } * sourceRowBytes, rowBytes);
			}

			return true;
		}

		// Convert each row into BGRA8
		for (uint32_t y = 0; y < height; y++)
		{
			uint8_t const* const sourceRow      = source + size_t{ y } * sourceRowBytes;
			uint8_t* const       destinationRow = outStaged + size_t{ y } * rowBytes;

			for (uint32_t x = 0; x < width; x++)
			{
				// Read the components in the order the format names them
				uint8_t components[4];

				if (gdType == GD_TYPE_UNSIGNED_BYTE)
				{
					// A format without alpha reads as fully opaque.
					components[3] = 255u;
					memcpy(components, sourceRow + x * bytesPerTexel, bytesPerTexel);
				}
				else
				{
					uint16_t word;
					memcpy(&word, sourceRow + x * 2u, 2u);

					// The plain type packs the first component into the most significant
					// bits, the reversed one into the least. A 4-bit value times 17 is
					// its exact 8-bit equivalent, at most 255, so it fits a byte.
					for (uint32_t i = 0; i < 4; i++)
					{
						uint32_t const shift = isPackedRev ? (i * 4u) : (12u - i * 4u);
						components[i] = static_cast<uint8_t>(((word >> shift) & 0xFu) * 17u);
					}
				}

				// Write them blue first
				destinationRow[x * 4 + 0] = isReversed ? components[0] : components[2];
				destinationRow[x * 4 + 1] = components[1];
				destinationRow[x * 4 + 2] = isReversed ? components[2] : components[0];
				destinationRow[x * 4 + 3] = components[3];
			}
		}

		return true;
	}

	void VulkanBackend::DumpUploadedTexture(Texture const& texture, uint32_t handle, uint32_t width, uint32_t height, uint32_t gdFormat, uint32_t gdType, uint32_t rowLength, void const* pixels)
	{
		// Write out what the game actually handed over
		//
		// The interface geometry, viewports, projections and uploads were all 1:1, yet
		// the finished picture was magnified, so the question was whether the content
		// arriving here was already wrong. Only once past the startup screen, and only
		// for BGRA8, which is already BMP's byte order.
		bool const isDumpable = !texture.isCompressed && gdType == GD_TYPE_UNSIGNED_BYTE && gdFormat == GD_FORMAT_BGRA;

		if (textureDumpsRemaining <= 0 || !isDumpable || presentedFrames <= TEXTURE_DUMP_AFTER_FRAMES || !IsLogged(LOG_LEVEL_DEBUG))
		{
			return;
		}

		textureDumpsRemaining--;

		// Name the file after the texture
		char name[64];
		sprintf_s(name, sizeof(name), "scvk-tex-%u-%ux%u.bmp", handle, width, height);

		char path[MAX_PATH];
		if (!LogFilePath(name, path, sizeof(path)))
		{
			return;
		}

		// Write the source untouched
		//
		// The game's pixels are untyped bytes.
		uint32_t const sourceStride = (rowLength != 0) ? rowLength : width;
		WriteBmp(path, static_cast<uint8_t const*>(pixels), width, height, sourceStride * 4u);

		LogDebug("Vulkan: wrote texture %u (%ux%u) to %s", handle, width, height, path);
	}

	void VulkanBackend::LogTextureTraffic(void)
	{
		// Total the live textures, and the idle ones among them
		//
		// Idle means no draw has sampled it lately. That covers textures the game released
		// into its texture cache, but mostly ones it still holds for things out of view: an
		// unmodded city kept 43 MB idle with an 8 MB cache.
		uint32_t     liveCount = 0;
		VkDeviceSize liveBytes = 0;
		uint32_t     idleCount = 0;
		VkDeviceSize idleBytes = 0;

		for (Texture const& texture : textures)
		{
			if (!texture.isLive)
			{
				continue;
			}

			liveCount++;
			liveBytes += texture.memory.size;

			bool const isIdle = (texture.lastDrawnFrame == UINT64_MAX) || (texture.lastDrawnFrame + IDLE_TEXTURE_FRAMES <= presentedFrames);
			if (isIdle)
			{
				idleCount++;
				idleBytes += texture.memory.size;
			}
		}

		// The tick counts are far below the range where a double loses whole ticks.
		double const workMilliseconds = (ticksPerSecond > 0) ? static_cast<double>(textureWorkTicks) * 1000.0 / static_cast<double>(ticksPerSecond) : 0.0;

		// Byte totals stay far below the range where a double loses whole megabytes.
		double const megabyte = 1024.0 * 1024.0;
		LogDebug("Vulkan: textures %u live (%.0f MB), %u idle (%.0f MB); since the last heartbeat %u created, %u deleted, %u uploads (%.1f MB) in %u batches taking %.0f ms.", liveCount, static_cast<double>(liveBytes) / megabyte, idleCount, static_cast<double>(idleBytes) / megabyte, texturesCreated, texturesDestroyed, textureUploads, static_cast<double>(textureUploadBytes) / megabyte, textureBatches, workMilliseconds);

		// Say how the texture memory is laid out
		VkDeviceSize blockUsedBytes = 0;
		size_t       freeRanges     = 0;
		for (TextureBlock const& block : textureBlocks)
		{
			blockUsedBytes += block.usedBytes;
			freeRanges     += block.freeRanges.size();
		}

		// The tick counts are far below the range where a double loses whole ticks.
		double const memoryMilliseconds = (ticksPerSecond > 0) ? static_cast<double>(textureMemoryTicks) * 1000.0 / static_cast<double>(ticksPerSecond) : 0.0;

		LogDebug("Vulkan: texture blocks %zu (%.0f MB in use, %zu free ranges, %.0f ms finding room); %u memory allocations of %u allowed.", textureBlocks.size(), static_cast<double>(blockUsedBytes) / megabyte, freeRanges, memoryMilliseconds, liveMemoryAllocations, maximumMemoryAllocations);

		// Warn when the allocations near the device's limit
		//
		// Going past it is undefined. Some drivers allow billions; the spec only promises
		// 4096. Widened so the products cannot overflow.
		if (maximumMemoryAllocations != 0 && uint64_t{ liveMemoryAllocations } * 10u >= uint64_t{ maximumMemoryAllocations } * 9u)
		{
			LogWarn("Vulkan: WARNING: %u memory allocations against a limit of %u.", liveMemoryAllocations, maximumMemoryAllocations);
		}

		texturesCreated    = 0;
		texturesDestroyed  = 0;
		textureUploads     = 0;
		textureUploadBytes = 0;
		textureBatches     = 0;
		textureWorkTicks   = 0;
		textureMemoryTicks = 0;
	}

	void VulkanBackend::BeginUploadCommands(void)
	{
		VkCommandBufferBeginInfo beginInformation{ VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO };
		beginInformation.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;

		vkResetCommandBuffer(uploadCommandBuffer, 0);
		vkBeginCommandBuffer(uploadCommandBuffer, &beginInformation);
	}

	void VulkanBackend::SubmitUploadCommands(void)
	{
		vkEndCommandBuffer(uploadCommandBuffer);

		// Anything staged for textures goes first, as it would ahead of a frame
		SubmitTextureBatch();

		VkSubmitInfo submit{ VK_STRUCTURE_TYPE_SUBMIT_INFO };
		submit.commandBufferCount = 1;
		submit.pCommandBuffers    = &uploadCommandBuffer;

		vkResetFences(device, 1, &uploadFence);

		VkResult result = SubmitToQueue(submit, uploadFence);
		if (result != VK_SUCCESS)
		{
			Fail("vkQueueSubmit (upload)", result);
			return;
		}

		// Waited on rather than pipelined. Only reading the last frame back uses this,
		// and it needs the pixels at once.
		result = WaitForFence(uploadFence, UINT64_MAX);
		if (result != VK_SUCCESS)
		{
			Fail("vkWaitForFences (upload)", result);
		}
	}

	bool VulkanBackend::BeginTextureBatch(void)
	{
		if (isTextureBatchOpen)
		{
			return true;
		}

		if (textureBatchCommandBuffer == VK_NULL_HANDLE)
		{
			return false;
		}

		// Let the previous batch finish, so its command buffer and staging can be reused
		WaitForTextureBatch();

		VkCommandBufferBeginInfo beginInformation{ VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO };
		beginInformation.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;

		vkResetCommandBuffer(textureBatchCommandBuffer, 0);

		VkResult const result = vkBeginCommandBuffer(textureBatchCommandBuffer, &beginInformation);
		if (result != VK_SUCCESS)
		{
			Fail("vkBeginCommandBuffer (texture batch)", result);
			return false;
		}

		isTextureBatchOpen = true;
		return true;
	}

	void VulkanBackend::SubmitTextureBatch(void)
	{
		if (!isTextureBatchOpen)
		{
			return;
		}

		isTextureBatchOpen = false;

		VkResult result = vkEndCommandBuffer(textureBatchCommandBuffer);
		if (result != VK_SUCCESS)
		{
			Fail("vkEndCommandBuffer (texture batch)", result);
			return;
		}

		VkSubmitInfo submit{ VK_STRUCTURE_TYPE_SUBMIT_INFO };
		submit.commandBufferCount = 1;
		submit.pCommandBuffers    = &textureBatchCommandBuffer;

		vkResetFences(device, 1, &textureBatchFence);

		result = SubmitToQueue(submit, textureBatchFence);
		if (result != VK_SUCCESS)
		{
			Fail("vkQueueSubmit (texture batch)", result);
			return;
		}

		isTextureBatchInFlight = true;
		textureBatches++;
	}

	void VulkanBackend::FinishTextureBatch(void)
	{
		SubmitTextureBatch();
		WaitForTextureBatch();
	}

	void VulkanBackend::WaitForTextureBatch(void)
	{
		if (isTextureBatchInFlight)
		{
			// A lost device never signals; whatever the batch staged is let go regardless
			VkResult const result = WaitForFence(textureBatchFence, UINT64_MAX);
			if (result != VK_SUCCESS)
			{
				NoteDeviceLoss(result);
			}

			isTextureBatchInFlight = false;
		}

		// An open batch still reads its staging
		if (isTextureBatchOpen)
		{
			return;
		}

		// Free the staging for the next batch
		ArenaRewind(textureUploadArena);

		for (ArenaBlock& block : oversizedUploadBuffers)
		{
			if (block.mapped != nullptr) { vkUnmapMemory(device, block.memory); }
			FreeDeviceMemory(block.memory);
			if (block.buffer != VK_NULL_HANDLE) { vkDestroyBuffer(device, block.buffer, nullptr); }
		}

		oversizedUploadBuffers.clear();
	}

	bool VulkanBackend::AllocateTextureStaging(VkDeviceSize bytes, VkBuffer& outBuffer, VkDeviceSize& outOffset, uint8_t*& outAddress)
	{
		// Give an upload too large for the arena a buffer of its own, freed with the batch
		if (bytes > textureUploadArena.blockSize)
		{
			if (!BeginTextureBatch())
			{
				return false;
			}

			ArenaBlock block;
			if (!CreateHostBuffer(bytes, VK_BUFFER_USAGE_TRANSFER_SRC_BIT, block.buffer, block.memory, block.mapped))
			{
				return false;
			}

			oversizedUploadBuffers.push_back(block);

			outBuffer  = block.buffer;
			outOffset  = 0;
			outAddress = static_cast<uint8_t*>(block.mapped);
			return true;
		}

		// Finish the batch when the arena is full, so its staging can be reused
		//
		// Only an open batch can have filled it: the arena is rewound whenever a batch
		// finishes, and opening one waits for the last.
		if (isTextureBatchOpen && !ArenaHasRoom(textureUploadArena, bytes, TEXTURE_UPLOAD_ALIGNMENT))
		{
			FinishTextureBatch();
		}

		if (!BeginTextureBatch())
		{
			return false;
		}

		if (!ArenaAllocate(textureUploadArena, bytes, TEXTURE_UPLOAD_ALIGNMENT, outBuffer, outOffset, outAddress))
		{
			// The batch alone fills every block; send it and start over in an empty arena
			FinishTextureBatch();

			if (!BeginTextureBatch() || !ArenaAllocate(textureUploadArena, bytes, TEXTURE_UPLOAD_ALIGNMENT, outBuffer, outOffset, outAddress))
			{
				LogWarn("Vulkan: no staging for a texture upload of %llu bytes; skipping it.", bytes);
				return false;
			}
		}

		return true;
	}

	bool VulkanBackend::CreateTextureObjects(Texture& texture)
	{
		// Create the image
		VkImageCreateInfo imageInformation{ VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO };
		imageInformation.imageType     = VK_IMAGE_TYPE_2D;
		imageInformation.format        = texture.format;
		imageInformation.extent        = { texture.width, texture.height, 1 };
		imageInformation.mipLevels     = texture.levels;
		imageInformation.arrayLayers   = 1;
		imageInformation.samples       = VK_SAMPLE_COUNT_1_BIT;
		imageInformation.tiling        = VK_IMAGE_TILING_OPTIMAL;
		imageInformation.usage         = VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_SAMPLED_BIT;
		imageInformation.sharingMode   = VK_SHARING_MODE_EXCLUSIVE;
		imageInformation.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;

		VkResult result = vkCreateImage(device, &imageInformation, nullptr, &texture.image);
		if (result != VK_SUCCESS)
		{
			LogError("Vulkan: could not create a %ux%u texture (format %d): %s", texture.width, texture.height, texture.format, VkResultName(result));
			texture.image = VK_NULL_HANDLE;
			return false;
		}

		// Back it with device-local memory, shared with other textures
		VkMemoryRequirements requirements{};
		vkGetImageMemoryRequirements(device, texture.image, &requirements);

		if (!AllocateTextureMemory(requirements, texture.memory))
		{
			vkDestroyImage(device, texture.image, nullptr);
			texture.image = VK_NULL_HANDLE;
			return false;
		}

		vkBindImageMemory(device, texture.image, texture.memory.memory, texture.memory.offset);

		// Create its view
		VkImageViewCreateInfo viewInformation{ VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO };
		viewInformation.image    = texture.image;
		viewInformation.viewType = VK_IMAGE_VIEW_TYPE_2D;
		viewInformation.format   = texture.format;
		viewInformation.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
		viewInformation.subresourceRange.levelCount = texture.levels;
		viewInformation.subresourceRange.layerCount = 1;

		result = vkCreateImageView(device, &viewInformation, nullptr, &texture.view);
		if (result != VK_SUCCESS)
		{
			LogError("Vulkan: could not create a texture view: %s", VkResultName(result));
			vkDestroyImage(device, texture.image, nullptr);
			ReleaseImageMemory(texture.memory);
			texture.image = VK_NULL_HANDLE;
			texture.view  = VK_NULL_HANDLE;
			return false;
		}

		// Give it a descriptor set of its own
		if (!AllocateTextureSet(texture.descriptor, texture.descriptorPoolIndex))
		{
			vkDestroyImageView(device, texture.view, nullptr);
			vkDestroyImage(device, texture.image, nullptr);
			ReleaseImageMemory(texture.memory);
			texture.image      = VK_NULL_HANDLE;
			texture.view       = VK_NULL_HANDLE;
			texture.descriptor = VK_NULL_HANDLE;
			return false;
		}

		VkDescriptorImageInfo imageBinding{};
		imageBinding.sampler     = VK_NULL_HANDLE;
		imageBinding.imageView   = texture.view;
		imageBinding.imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;

		VkWriteDescriptorSet write{ VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET };
		write.dstSet          = texture.descriptor;
		write.dstBinding      = 0;
		write.descriptorCount = 1;
		write.descriptorType  = VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE;
		write.pImageInfo      = &imageBinding;

		vkUpdateDescriptorSets(device, 1, &write, 0, nullptr);

		texture.isLive         = true;
		texture.isReserved     = false;
		texture.serial         = nextTextureSerial++;
		texture.uploadedLevels = 0;
		texture.uploadCount    = 0;
		texture.firstByteCount = 0;
		texture.lastDrawnFrame = UINT64_MAX;

		// Put the image into its sampled layout ahead of the frame's draws
		//
		// So a draw that binds it before anything has been uploaded is still valid.
		if (!BeginTextureBatch())
		{
			return false;
		}

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

		vkCmdPipelineBarrier(textureBatchCommandBuffer, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT, 0, 0, nullptr, 0, nullptr, 1, &barrier);

		texturesCreated++;
		return true;
	}

	void VulkanBackend::RetireTextureObjects(Texture& texture)
	{
		// Retire the objects rather than destroying them here
		//
		// The frame in progress may already have recorded commands that sample this
		// texture, and those have not been submitted yet, so the objects have to outlive
		// it. Waiting for the device instead would be correct but costs a full stall, and
		// the game deletes textures hundreds of times a session.
		if (texture.image != VK_NULL_HANDLE || texture.descriptor != VK_NULL_HANDLE)
		{
			RetiredImage retired;
			retired.image               = texture.image;
			retired.memory              = texture.memory;
			retired.view                = texture.view;
			retired.descriptor          = texture.descriptor;
			retired.descriptorPoolIndex = texture.descriptorPoolIndex;

			Retire(retired);
		}

		texture.image      = VK_NULL_HANDLE;
		texture.memory     = ImageMemory{};
		texture.view       = VK_NULL_HANDLE;
		texture.descriptor = VK_NULL_HANDLE;
		texture.isLive     = false;
		texture.serial     = 0;
	}

	//// Public API

	uint32_t VulkanBackend::CreateTexture(uint32_t gdInternalFormat, uint32_t width, uint32_t height, uint32_t levels)
	{
		if (isDead || device == VK_NULL_HANDLE || width == 0 || height == 0)
		{
			return 0;
		}

		TickAccumulator const timer(textureWorkTicks);
		PhaseScope const      creating(*this, FRAME_PHASE_TEXTURES);

		Texture texture;
		texture.format         = MapInternalFormat(gdInternalFormat, texture.isCompressed);
		texture.internalFormat = gdInternalFormat;
		texture.width          = width;
		texture.height         = height;
		texture.levels         = (levels == 0) ? 1 : levels;

		if (!CreateTextureObjects(texture))
		{
			RetireTextureObjects(texture);
			return 0;
		}

		// Hand out the next slot
		textures.push_back(texture);
		return static_cast<uint32_t>(textures.size() - 1);
	}

	uint32_t VulkanBackend::ReserveTexture(void)
	{
		if (isDead || device == VK_NULL_HANDLE)
		{
			return 0;
		}

		Texture texture;
		texture.isReserved = true;

		textures.push_back(texture);
		return static_cast<uint32_t>(textures.size() - 1);
	}

	bool VulkanBackend::DefineTexture(uint32_t handle, uint32_t gdInternalFormat, uint32_t width, uint32_t height, uint32_t levels)
	{
		if (isDead || device == VK_NULL_HANDLE || handle == 0 || handle >= textures.size() || width == 0 || height == 0)
		{
			return false;
		}

		Texture& texture = textures[handle];
		if (!texture.isLive && !texture.isReserved)
		{
			return false;
		}

		bool isCompressed = false;
		VkFormat const format = MapInternalFormat(gdInternalFormat, isCompressed);
		uint32_t const levelCount = (levels == 0) ? 1 : levels;

		// Keep an image that already matches, contents and all
		if (texture.isLive && texture.format == format && texture.width == width && texture.height == height && texture.levels >= levelCount)
		{
			return true;
		}

		TickAccumulator const timer(textureWorkTicks);
		PhaseScope const      defining(*this, FRAME_PHASE_TEXTURES);

		// Add levels to an image of the same format and size, keeping the ones it has
		//
		// TexImage2D names a texture's top level first and its smaller levels after, so
		// the image has to grow under it. The levels already uploaded are copied across
		// in the texture batch, ahead of any frame that samples the new image.
		if (texture.isLive && texture.format == format && texture.width == width && texture.height == height)
		{
			Texture grown = texture;
			grown.levels     = levelCount;
			grown.image      = VK_NULL_HANDLE;
			grown.memory     = ImageMemory{};
			grown.view       = VK_NULL_HANDLE;
			grown.descriptor = VK_NULL_HANDLE;

			if (!CreateTextureObjects(grown) || !BeginTextureBatch())
			{
				RetireTextureObjects(grown);
				return false;
			}

			VkImageMemoryBarrier barriers[2]{};
			for (VkImageMemoryBarrier& barrier : barriers)
			{
				barrier.sType               = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
				barrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
				barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
				barrier.subresourceRange    = { VK_IMAGE_ASPECT_COLOR_BIT, 0, texture.levels, 0, 1 };
			}

			barriers[0].image         = texture.image;
			barriers[0].oldLayout     = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
			barriers[0].newLayout     = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
			barriers[0].srcAccessMask = VK_ACCESS_SHADER_READ_BIT;
			barriers[0].dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
			barriers[1].image         = grown.image;
			barriers[1].oldLayout     = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
			barriers[1].newLayout     = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
			barriers[1].srcAccessMask = VK_ACCESS_SHADER_READ_BIT;
			barriers[1].dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;

			vkCmdPipelineBarrier(textureBatchCommandBuffer, VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, nullptr, 0, nullptr, 2, barriers);

			std::vector<VkImageCopy> copies(texture.levels);
			for (uint32_t level = 0; level < texture.levels; level++)
			{
				VkImageCopy& copy = copies[level];
				copy.srcSubresource = { VK_IMAGE_ASPECT_COLOR_BIT, level, 0, 1 };
				copy.dstSubresource = { VK_IMAGE_ASPECT_COLOR_BIT, level, 0, 1 };
				copy.extent         = { std::max(width >> level, 1u), std::max(height >> level, 1u), 1 };
			}

			vkCmdCopyImage(textureBatchCommandBuffer, texture.image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, grown.image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, static_cast<uint32_t>(copies.size()), copies.data());

			barriers[0].oldLayout     = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
			barriers[0].newLayout     = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
			barriers[0].srcAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
			barriers[0].dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
			barriers[1].oldLayout     = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
			barriers[1].newLayout     = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
			barriers[1].srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
			barriers[1].dstAccessMask = VK_ACCESS_SHADER_READ_BIT;

			vkCmdPipelineBarrier(textureBatchCommandBuffer, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT, 0, 0, nullptr, 0, nullptr, 2, barriers);

			// The new image takes the name; the old one goes once nothing reads it
			grown.uploadedLevels = texture.uploadedLevels;
			grown.uploadCount    = texture.uploadCount;
			grown.firstByteCount = texture.firstByteCount;
			memcpy(grown.firstBytes, texture.firstBytes, sizeof(grown.firstBytes));

			RetireTextureObjects(texture);
			texture = grown;
			return true;
		}

		// Replace it otherwise, under the same name
		if (texture.isLive)
		{
			RetireTextureObjects(texture);
			texturesDestroyed++;
		}

		texture.format         = format;
		texture.isCompressed   = isCompressed;
		texture.internalFormat = gdInternalFormat;
		texture.width          = width;
		texture.height         = height;
		texture.levels         = levelCount;

		if (!CreateTextureObjects(texture))
		{
			RetireTextureObjects(texture);
			texture.isReserved = true;
			return false;
		}

		return true;
	}

	bool VulkanBackend::IsTextureName(uint32_t handle) const
	{
		return handle != 0 && handle < textures.size() && (textures[handle].isLive || textures[handle].isReserved);
	}

	uint32_t VulkanBackend::TextureSerial(uint32_t handle) const
	{
		if (handle == 0 || handle >= textures.size() || !textures[handle].isLive)
		{
			return 0;
		}

		return textures[handle].serial;
	}

	void VulkanBackend::UploadTextureLevel(uint32_t handle, uint32_t level, int32_t offsetX, int32_t offsetY, uint32_t width, uint32_t height, uint32_t gdFormat, uint32_t gdType, uint32_t rowLength, void const* pixels)
	{
		if (isDead || pixels == nullptr || handle >= textures.size())
		{
			return;
		}

		Texture& texture = textures[handle];
		if (!texture.isLive || width == 0 || height == 0 || level >= texture.levels)
		{
			return;
		}

		// Keep the copy inside the level
		uint32_t const levelWidth  = std::max(texture.width >> level, 1u);
		uint32_t const levelHeight = std::max(texture.height >> level, 1u);

		if (offsetX < 0 || offsetY < 0 || static_cast<uint32_t>(offsetX) + width > levelWidth || static_cast<uint32_t>(offsetY) + height > levelHeight)
		{
			LogWarn("Vulkan: texture %u level %u upload %d,%d %ux%u lies outside its %ux%u; skipping.", handle, level, offsetX, offsetY, width, height, levelWidth, levelHeight);
			return;
		}

		TickAccumulator const timer(textureWorkTicks);
		PhaseScope const      uploading(*this, FRAME_PHASE_TEXTURES);

		// Count an upload that lands after a draw this frame already recorded
		if (texture.lastDrawnFrame == presentedFrames)
		{
			uploadsAfterDraw++;

			if (hazardNotesRemaining > 0)
			{
				hazardNotesRemaining--;
				LogDebug("  HAZARD: texture %u (%ux%u) level %u, %d,%d %ux%u, uploaded after a draw this frame sampled it", handle, texture.width, texture.height, level, offsetX, offsetY, width, height);
			}
		}

		// Stage it straight into the batch's memory, in the image's own layout
		VkDeviceSize const bytes = StagedBytes(texture, width, height);

		VkBuffer     uploadBuffer  = VK_NULL_HANDLE;
		VkDeviceSize uploadOffset  = 0;
		uint8_t*     uploadAddress = nullptr;

		if (!AllocateTextureStaging(bytes, uploadBuffer, uploadOffset, uploadAddress))
		{
			return;
		}

		if (!StageTexels(texture, width, height, gdFormat, gdType, rowLength, pixels, uploadAddress))
		{
			return;
		}

		if (level + 1 > texture.uploadedLevels)
		{
			texture.uploadedLevels = level + 1;
		}

		texture.uploadCount++;

		DumpUploadedTexture(texture, handle, width, height, gdFormat, gdType, rowLength, pixels);

		// Keep the start of a tiny texture's top level for the log
		//
		// The building shadows mask with a 4x4 texture, and what it holds decides where
		// they show.
		if (level == 0 && offsetX == 0 && offsetY == 0 && texture.width * texture.height <= TINY_TEXTURE_TEXELS)
		{
			size_t const keptBytes = (bytes < sizeof(texture.firstBytes)) ? static_cast<size_t>(bytes) : sizeof(texture.firstBytes);
			memcpy(texture.firstBytes, uploadAddress, keptBytes);

			// At most 16, so it fits.
			texture.firstByteCount = static_cast<uint32_t>(keptBytes);
		}

		textureUploads++;
		textureUploadBytes += bytes;

		// Copy it into the level, out of and back into the sampled layout
		//
		// The barriers reach across submissions to the same queue: the first waits for
		// earlier frames still sampling the image, the second makes the copy visible to
		// the frames after.
		VkImageMemoryBarrier barrier{ VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER };
		barrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
		barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
		barrier.image               = texture.image;
		barrier.subresourceRange.aspectMask   = VK_IMAGE_ASPECT_COLOR_BIT;
		barrier.subresourceRange.baseMipLevel = level;
		barrier.subresourceRange.levelCount   = 1;
		barrier.subresourceRange.layerCount   = 1;

		barrier.oldLayout     = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
		barrier.newLayout     = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
		barrier.srcAccessMask = VK_ACCESS_SHADER_READ_BIT;
		barrier.dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;

		vkCmdPipelineBarrier(textureBatchCommandBuffer, VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, nullptr, 0, nullptr, 1, &barrier);

		VkBufferImageCopy copy{};
		copy.bufferOffset      = uploadOffset;
		copy.bufferRowLength   = 0;
		copy.bufferImageHeight = 0;
		copy.imageSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
		copy.imageSubresource.mipLevel   = level;
		copy.imageSubresource.layerCount = 1;
		copy.imageOffset = { offsetX, offsetY, 0 };
		copy.imageExtent = { width, height, 1 };

		vkCmdCopyBufferToImage(textureBatchCommandBuffer, uploadBuffer, texture.image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &copy);

		barrier.oldLayout     = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
		barrier.newLayout     = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
		barrier.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
		barrier.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;

		vkCmdPipelineBarrier(textureBatchCommandBuffer, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT, 0, 0, nullptr, 0, nullptr, 1, &barrier);
	}

	void VulkanBackend::SetStageParameter(uint32_t stage, uint32_t parameterType, uint32_t value)
	{
		if (stage >= _countof(stageParameters) || parameterType >= 4)
		{
			return;
		}

		stageParameters[stage][parameterType] = value;
	}

	void VulkanBackend::GetStageParameters(uint32_t stage, uint32_t outParameters[4]) const
	{
		uint32_t const clampedStage = (stage < _countof(stageParameters)) ? stage : 0;
		memcpy(outParameters, stageParameters[clampedStage], sizeof(stageParameters[clampedStage]));
	}

	void VulkanBackend::SetTexture(uint32_t handle)
	{
		currentTexture = (handle < textures.size() && textures[handle].isLive) ? handle : 0;
	}

	void VulkanBackend::SetTexture1(uint32_t handle)
	{
		currentTexture1 = (handle < textures.size() && textures[handle].isLive) ? handle : 0;
	}

	void VulkanBackend::SetTextureStageEnabled(uint32_t stage, bool isEnabled)
	{
		if (stage > 1)
		{
			return;
		}

		isStageEnabled[stage] = isEnabled;
	}

	void VulkanBackend::DestroyTexture(uint32_t handle)
	{
		if (handle == 0 || handle >= textures.size())
		{
			return;
		}

		Texture& texture = textures[handle];
		if (!texture.isLive && !texture.isReserved)
		{
			return;
		}

		if (texture.isLive)
		{
			RetireTextureObjects(texture);
			texturesDestroyed++;
		}

		// Clear the slot at once, so the handle no longer resolves to anything
		texture = Texture{};

		if (currentTexture == handle)
		{
			currentTexture = 0;
		}

		if (currentTexture1 == handle)
		{
			currentTexture1 = 0;
		}
	}

	void VulkanBackend::LogTextureInformation(uint32_t handle, char const* reason)
	{
		if (handle == 0 || handle >= textures.size() || !textures[handle].isLive)
		{
			LogDebug("  TEXINFO %s: handle %u is not a live texture", reason, handle);
			return;
		}

		Texture const& texture = textures[handle];

		// Spell out any kept bytes, two hex digits each
		char bytes[sizeof(texture.firstBytes) * 3 + 1] = {};

		for (uint32_t i = 0; i < texture.firstByteCount; i++)
		{
			sprintf_s(bytes + i * 3, sizeof(bytes) - i * 3, "%02x ", texture.firstBytes[i]);
		}

		LogDebug("  TEXINFO %s: handle %u, %ux%u, format %d, %s, %u level(s) declared, %u uploaded%s%s", reason, handle, texture.width, texture.height, texture.format, texture.isCompressed ? "compressed" : "plain", texture.levels, texture.uploadedLevels, (texture.firstByteCount > 0) ? ", bytes " : "", bytes);
	}

	bool VulkanBackend::DescribeTexture(uint32_t handle, uint32_t& outWidth, uint32_t& outHeight, uint32_t& outLevels, uint32_t& outUploadedLevels, uint32_t& outUploadCount) const
	{
		if (handle == 0 || handle >= textures.size() || !textures[handle].isLive)
		{
			return false;
		}

		Texture const& texture = textures[handle];
		outWidth          = texture.width;
		outHeight         = texture.height;
		outLevels         = texture.levels;
		outUploadedLevels = texture.uploadedLevels;
		outUploadCount    = texture.uploadCount;
		return true;
	}
}
