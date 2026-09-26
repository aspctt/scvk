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
 * Each texture owns an image, a view and a descriptor set that never changes. Filter and
 * wrap live on the texture, as they do on an OpenGL texture object, and pick a sampler
 * from a small cache when a draw binds it.
 */

//// Dependencies

#include "VulkanBackend.h"
#include "Logger.h"

#include <Windows.h>
#include <stdio.h>
#include <string.h>

namespace scvk
{
	//// Constants

	namespace
	{
		// One descriptor set per texture, allocated when the texture is created and never
		// rewritten, so nothing can be updated while the GPU is reading it. A session
		// created 89 textures, so this has generous headroom.
		constexpr uint32_t MAXIMUM_TEXTURES = 4096;

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
				// RGB5, RGB8, RGBA4, RGB5_A1 and RGBA8 all become RGBA8. The narrower
				// ones lose nothing that matters here, and the upload path only ever
				// hands over 8 bits per channel anyway.
				outIsCompressed = false;
				return VK_FORMAT_R8G8B8A8_UNORM;
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
		 * GL_CLAMP, not CLAMP_TO_EDGE: it clamps to the border rather than smearing the
		 * edge texel outward. That distinction is the whole point here, since a projected
		 * cloud shadow needs nothing outside its own footprint, and a transparent border
		 * gives exactly that.
		 */
		VkSamplerAddressMode MapAddressMode(uint32_t gdWrap)
		{
			return (gdWrap == 2) ? VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_BORDER : VK_SAMPLER_ADDRESS_MODE_REPEAT;
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

		// Create the pool
		//
		// Samplers are their own descriptor type and their own sets, one per distinct
		// filter and wrap combination the game asks for.
		VkDescriptorPoolSize imagePoolSize{};
		imagePoolSize.type            = VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE;
		imagePoolSize.descriptorCount = MAXIMUM_TEXTURES;

		VkDescriptorPoolSize samplerPoolSize{};
		samplerPoolSize.type            = VK_DESCRIPTOR_TYPE_SAMPLER;
		samplerPoolSize.descriptorCount = MAXIMUM_SAMPLERS;

		VkDescriptorPoolSize const poolSizes[] = { imagePoolSize, samplerPoolSize };

		VkDescriptorPoolCreateInfo poolInformation{ VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO };
		poolInformation.flags         = VK_DESCRIPTOR_POOL_CREATE_FREE_DESCRIPTOR_SET_BIT;
		poolInformation.maxSets       = MAXIMUM_TEXTURES + MAXIMUM_SAMPLERS;
		poolInformation.poolSizeCount = _countof(poolSizes);
		poolInformation.pPoolSizes    = poolSizes;

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

		// Reserve a command buffer and fence for uploads
		//
		// Uploads happen outside the frame's own recording.
		VkCommandBufferAllocateInfo allocationInformation{ VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO };
		allocationInformation.commandPool        = commandPool;
		allocationInformation.level              = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
		allocationInformation.commandBufferCount = 1;

		result = vkAllocateCommandBuffers(device, &allocationInformation, &uploadCommandBuffer);
		if (result != VK_SUCCESS)
		{
			Fail("vkAllocateCommandBuffers (upload)", result);
			return false;
		}

		VkFenceCreateInfo fenceInformation{ VK_STRUCTURE_TYPE_FENCE_CREATE_INFO };
		result = vkCreateFence(device, &fenceInformation, nullptr, &uploadFence);
		if (result != VK_SUCCESS)
		{
			Fail("vkCreateFence (upload)", result);
			return false;
		}

		return CreateDefaultTexture();
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

		return true;
	}

	void VulkanBackend::DestroyTextures(void)
	{
		FlushRetiredTextures();

		for (Texture& texture : textures)
		{
			if (texture.view != VK_NULL_HANDLE)   { vkDestroyImageView(device, texture.view, nullptr); }
			if (texture.image != VK_NULL_HANDLE)  { vkDestroyImage(device, texture.image, nullptr); }
			if (texture.memory != VK_NULL_HANDLE) { vkFreeMemory(device, texture.memory, nullptr); }

			texture = Texture{};
		}

		textures.clear();
		currentTexture  = 0;
		currentTexture1 = 0;
	}

	void VulkanBackend::FlushRetiredTextures(void)
	{
		// Called once a frame's fence has been waited on, which means every command that
		// could still have been reading these has completed.
		for (RetiredTexture const& retired : retiredTextures)
		{
			if (retired.view != VK_NULL_HANDLE)   { vkDestroyImageView(device, retired.view, nullptr); }
			if (retired.image != VK_NULL_HANDLE)  { vkDestroyImage(device, retired.image, nullptr); }
			if (retired.memory != VK_NULL_HANDLE) { vkFreeMemory(device, retired.memory, nullptr); }

			// The descriptor set matters as much as the image. Its pool is capped, and
			// leaking sets exhausts it long before memory runs out.
			if (retired.descriptor != VK_NULL_HANDLE)
			{
				vkFreeDescriptorSets(device, descriptorPool, 1, &retired.descriptor);
			}
		}

		retiredTextures.clear();
	}

	void VulkanBackend::RefreshTextureParameters(uint32_t handle, bool shouldForce)
	{
		if (handle == 0 || handle >= textures.size() || !textures[handle].isLive)
		{
			return;
		}

		Texture& texture = textures[handle];

		if (!texture.hasStaleParameters && !shouldForce)
		{
			return;
		}

		// Report a forced refresh that changed what the texture samples with
		if (!texture.hasStaleParameters && memcmp(texture.parameters, textureParameters, sizeof(textureParameters)) != 0)
		{
			parameterRefreshChanges++;

			if (parameterNotesRemaining > 0)
			{
				parameterNotesRemaining--;
				LogNote("  PARAMS: texture %u (%ux%u) had %u,%u,%u,%u and now samples with %u,%u,%u,%u", handle, texture.width, texture.height, texture.parameters[0], texture.parameters[1], texture.parameters[2], texture.parameters[3], textureParameters[0], textureParameters[1], textureParameters[2], textureParameters[3]);
			}
		}

		// Copy the current values onto the texture
		for (int i = 0; i < 4; i++)
		{
			texture.parameters[i] = textureParameters[i];
		}

		texture.hasStaleParameters = false;
	}

	VkDescriptorSet VulkanBackend::GetSamplerSet(uint32_t handle)
	{
		// Reuse the sampler made for the same parameters
		uint32_t const* const parameters = (handle < textures.size() && textures[handle].isLive) ? textures[handle].parameters : textureParameters;

		uint32_t const key = (parameters[0] & 0xff) | ((parameters[1] & 0xff) << 8) | ((parameters[2] & 0xff) << 16) | ((parameters[3] & 0xff) << 24);

		for (SamplerEntry const& entry : samplers)
		{
			if (entry.key == key)
			{
				return entry.set;
			}
		}

		VkDescriptorSet const fallback = samplers.empty() ? VK_NULL_HANDLE : samplers[0].set;

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

		LogNote("Vulkan: created sampler for mag %u, min %u, wrap %u/%u.", textureParameters[0], textureParameters[1], textureParameters[2], textureParameters[3]);

		samplers.push_back(entry);
		return entry.set;
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
			LogNote("  HAZARD: texture %u (%ux%u) sampled before anything was uploaded to it", handle, texture.width, texture.height);
		}
	}

	bool VulkanBackend::StageTexels(Texture const& texture, uint32_t width, uint32_t height, uint32_t gdFormat, uint32_t gdType, uint32_t rowLength, void const* pixels, std::vector<uint8_t>& outStaged)
	{
		// Copy compressed blocks as they are
		//
		// A level of a texture in a 32-bit process fits a size_t.
		if (texture.isCompressed)
		{
			outStaged.resize(static_cast<size_t>(CompressedSize(texture.format, width, height)));
			memcpy(outStaged.data(), pixels, outStaged.size());
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
			LogNote("Vulkan: texture upload format %u type %u (%ux%u) is not handled; skipping.", gdFormat, gdType, width, height);
			return false;
		}

		// Work out the source rows
		//
		// Rows are padded to the unpack alignment, which the game leaves at OpenGL's
		// default of 4 bytes. That only changes anything for 16-bit or 24-bit texels,
		// such as on an odd width.
		uint32_t const sourceStride  = (rowLength != 0) ? rowLength : width;
		uint32_t const bytesPerTexel = (gdType != GD_TYPE_UNSIGNED_BYTE) ? 2u : (hasAlpha ? 4u : 3u);
		size_t const   sourceRowBytes = (size_t{ sourceStride } * bytesPerTexel + 3u) & ~size_t{ 3 };

		outStaged.resize(size_t{ width } * height * 4u);

		// Convert each row into RGBA8
		//
		// The game's pixels are untyped bytes.
		uint8_t const* const source = static_cast<uint8_t const*>(pixels);

		for (uint32_t y = 0; y < height; y++)
		{
			uint8_t const* const sourceRow      = source + size_t{ y } * sourceRowBytes;
			uint8_t* const       destinationRow = outStaged.data() + size_t{ y } * width * 4u;

			if (gdType == GD_TYPE_UNSIGNED_BYTE && gdFormat == GD_FORMAT_RGBA)
			{
				memcpy(destinationRow, sourceRow, size_t{ width } * 4u);
				continue;
			}

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

				// Write them red first
				//
				// Done here rather than by choosing a BGRA image format, so every
				// uncompressed texture ends up in one predictable layout.
				destinationRow[x * 4 + 0] = isReversed ? components[2] : components[0];
				destinationRow[x * 4 + 1] = components[1];
				destinationRow[x * 4 + 2] = isReversed ? components[0] : components[2];
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

		if (textureDumpsRemaining <= 0 || !isDumpable || presentedFrames <= TEXTURE_DUMP_AFTER_FRAMES)
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

		LogNote("Vulkan: wrote texture %u (%ux%u) to %s", handle, width, height, path);
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

		VkSubmitInfo submit{ VK_STRUCTURE_TYPE_SUBMIT_INFO };
		submit.commandBufferCount = 1;
		submit.pCommandBuffers    = &uploadCommandBuffer;

		vkResetFences(device, 1, &uploadFence);
		vkQueueSubmit(queue, 1, &submit, uploadFence);

		// Waited on rather than pipelined. Uploads are rare (a few hundred in a session)
		// and always happen outside the frame, so the simplicity is worth more than the
		// throughput.
		vkWaitForFences(device, 1, &uploadFence, VK_TRUE, UINT64_MAX);
	}

	//// Public API

	uint32_t VulkanBackend::CreateTexture(uint32_t gdInternalFormat, uint32_t width, uint32_t height, uint32_t levels)
	{
		if (isDead || device == VK_NULL_HANDLE || width == 0 || height == 0)
		{
			return 0;
		}

		// Create the image
		Texture texture;
		texture.format = MapInternalFormat(gdInternalFormat, texture.isCompressed);
		texture.width  = width;
		texture.height = height;
		texture.levels = (levels == 0) ? 1 : levels;

		VkImageCreateInfo imageInformation{ VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO };
		imageInformation.imageType     = VK_IMAGE_TYPE_2D;
		imageInformation.format        = texture.format;
		imageInformation.extent        = { width, height, 1 };
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
			LogNote("Vulkan: could not create a %ux%u texture (format %d): %s", width, height, texture.format, VkResultName(result));
			return 0;
		}

		// Back it with device-local memory
		VkMemoryRequirements requirements{};
		vkGetImageMemoryRequirements(device, texture.image, &requirements);

		uint32_t typeIndex = 0;
		if (!FindMemoryType(requirements.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT, typeIndex))
		{
			vkDestroyImage(device, texture.image, nullptr);
			LogNote("Vulkan: no device-local memory type for textures.");
			return 0;
		}

		VkMemoryAllocateInfo allocationInformation{ VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO };
		allocationInformation.allocationSize  = requirements.size;
		allocationInformation.memoryTypeIndex = typeIndex;

		result = vkAllocateMemory(device, &allocationInformation, nullptr, &texture.memory);
		if (result != VK_SUCCESS)
		{
			vkDestroyImage(device, texture.image, nullptr);
			Fail("vkAllocateMemory (texture)", result);
			return 0;
		}

		vkBindImageMemory(device, texture.image, texture.memory, 0);

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
			Fail("vkCreateImageView (texture)", result);
			return 0;
		}

		// Give it a descriptor set of its own
		VkDescriptorSetAllocateInfo setInformation{ VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO };
		setInformation.descriptorPool     = descriptorPool;
		setInformation.descriptorSetCount = 1;
		setInformation.pSetLayouts        = &imageSetLayout;

		result = vkAllocateDescriptorSets(device, &setInformation, &texture.descriptor);
		if (result != VK_SUCCESS)
		{
			Fail("vkAllocateDescriptorSets", result);
			return 0;
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

		texture.isLive = true;

		// Put the image into its sampled layout straight away
		//
		// So a draw that binds it before anything has been uploaded is still valid.
		BeginUploadCommands();

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

		vkCmdPipelineBarrier(uploadCommandBuffer, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT, 0, 0, nullptr, 0, nullptr, 1, &barrier);

		SubmitUploadCommands();

		// Hand out the next slot
		textures.push_back(texture);
		return textures.size() - 1;
	}

	void VulkanBackend::UploadTextureLevel(uint32_t handle, uint32_t level, int32_t offsetX, int32_t offsetY, uint32_t width, uint32_t height, uint32_t gdFormat, uint32_t gdType, uint32_t rowLength, void const* pixels)
	{
		if (isDead || pixels == nullptr || handle == 0 || handle >= textures.size())
		{
			return;
		}

		Texture& texture = textures[handle];
		if (!texture.isLive || width == 0 || height == 0)
		{
			return;
		}

		// Count an upload that lands after a draw this frame already recorded
		if (texture.lastDrawnFrame == presentedFrames)
		{
			uploadsAfterDraw++;

			if (hazardNotesRemaining > 0)
			{
				hazardNotesRemaining--;
				LogNote("  HAZARD: texture %u (%ux%u) level %u, %d,%d %ux%u, uploaded after a draw this frame sampled it", handle, texture.width, texture.height, level, offsetX, offsetY, width, height);
			}
		}

		if (level + 1 > texture.uploadedLevels)
		{
			texture.uploadedLevels = level + 1;
		}

		// Build a tightly packed copy in the image's own format
		std::vector<uint8_t> staged;
		if (!StageTexels(texture, width, height, gdFormat, gdType, rowLength, pixels, staged))
		{
			return;
		}

		DumpUploadedTexture(texture, handle, width, height, gdFormat, gdType, rowLength, pixels);

		// Put it in a host buffer
		VkBuffer       uploadBuffer = VK_NULL_HANDLE;
		VkDeviceMemory uploadMemory = VK_NULL_HANDLE;
		void*          uploadMapped = nullptr;

		if (!CreateHostBuffer(staged.size(), VK_BUFFER_USAGE_TRANSFER_SRC_BIT, uploadBuffer, uploadMemory, uploadMapped))
		{
			return;
		}

		memcpy(uploadMapped, staged.data(), staged.size());

		// Copy it into the level, out of and back into the sampled layout
		BeginUploadCommands();

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

		vkCmdPipelineBarrier(uploadCommandBuffer, VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, nullptr, 0, nullptr, 1, &barrier);

		VkBufferImageCopy copy{};
		copy.bufferOffset      = 0;
		copy.bufferRowLength   = 0;
		copy.bufferImageHeight = 0;
		copy.imageSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
		copy.imageSubresource.mipLevel   = level;
		copy.imageSubresource.layerCount = 1;
		copy.imageOffset = { offsetX, offsetY, 0 };
		copy.imageExtent = { width, height, 1 };

		vkCmdCopyBufferToImage(uploadCommandBuffer, uploadBuffer, texture.image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &copy);

		barrier.oldLayout     = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
		barrier.newLayout     = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
		barrier.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
		barrier.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;

		vkCmdPipelineBarrier(uploadCommandBuffer, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT, 0, 0, nullptr, 0, nullptr, 1, &barrier);

		SubmitUploadCommands();

		// Release the host buffer
		vkUnmapMemory(device, uploadMemory);
		vkFreeMemory(device, uploadMemory, nullptr);
		vkDestroyBuffer(device, uploadBuffer, nullptr);
	}

	void VulkanBackend::SetTextureParameters(uint32_t magnificationFilter, uint32_t minificationFilter, uint32_t wrapS, uint32_t wrapT)
	{
		textureParameters[0] = magnificationFilter;
		textureParameters[1] = minificationFilter;
		textureParameters[2] = wrapS;
		textureParameters[3] = wrapT;

		shouldRefreshStage0Parameters = true;
	}

	void VulkanBackend::SetTexture(uint32_t handle)
	{
		currentTexture = (handle < textures.size() && textures[handle].isLive) ? handle : 0;

		// The bind only marks the texture as needing the parameters. They are read at the
		// next draw, which is when the game's driver applies them.
		if (currentTexture != 0 && currentTexture < textures.size())
		{
			textures[currentTexture].hasStaleParameters = true;
		}
	}

	void VulkanBackend::SetTexture1(uint32_t handle)
	{
		currentTexture1 = (handle < textures.size() && textures[handle].isLive) ? handle : 0;

		if (currentTexture1 != 0 && currentTexture1 < textures.size())
		{
			textures[currentTexture1].hasStaleParameters = true;
		}
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
		if (handle == 0 || handle >= textures.size() || !textures[handle].isLive)
		{
			return;
		}

		// Retire the objects rather than destroying them here
		//
		// The frame in progress may already have recorded commands that sample this
		// texture, and those have not been submitted yet, so the objects have to outlive
		// it. Waiting for the device instead would be correct but costs a full stall, and
		// the game deletes textures hundreds of times a session.
		Texture& texture = textures[handle];

		RetiredTexture retired;
		retired.image      = texture.image;
		retired.memory     = texture.memory;
		retired.view       = texture.view;
		retired.descriptor = texture.descriptor;

		retiredTextures.push_back(retired);

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
			LogNote("  TEXINFO %s: handle %u is not a live texture", reason, handle);
			return;
		}

		Texture const& texture = textures[handle];
		LogNote("  TEXINFO %s: handle %u, %ux%u, format %d, %s, %u level(s) declared, %u uploaded", reason, handle, texture.width, texture.height, texture.format, texture.isCompressed ? "compressed" : "plain", texture.levels, texture.uploadedLevels);
	}
}
