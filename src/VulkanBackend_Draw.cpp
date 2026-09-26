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
 * Pipelines and draws.
 *
 * Everything the fixed function interface treats as state that Vulkan bakes into a
 * pipeline becomes part of a pipeline key, and everything that changes per draw goes in
 * push constants. The game's geometry lives in client memory, so each draw copies its
 * vertices and indices into per-frame arenas first.
 */

//// Dependencies

#include "VulkanBackend.h"
#include "Logger.h"
#include "ShaderBinaries.h"

#include <VertexFormatUtils.h>

#include <algorithm>
#include <string.h>

namespace scvk
{
	//// Constants

	namespace
	{
		// Vulkan has no quad topology, so quads are drawn as indexed triangle pairs. The
		// index pattern depends only on the vertex count, never on the data, so it is
		// built once, for this many quads, and reused.
		constexpr uint32_t MAXIMUM_QUADS = 16384;

		// Two vertex blocks up front, the 64 MB a city frame usually fits in. A frame of
		// the whole city at the widest zoom needs more, and gets it one block at a time.
		// The cap keeps a runaway from eating the address space of what is a 32-bit
		// process.
		constexpr VkDeviceSize VERTEX_BLOCK_SIZE     = 32u * 1024u * 1024u;
		constexpr int          VERTEX_INITIAL_BLOCKS = 2;
		constexpr VkDeviceSize INDEX_BLOCK_SIZE      = 8u * 1024u * 1024u;
		constexpr size_t       ARENA_MAXIMUM_BLOCKS  = 8;

		// Vertex data is aligned so the binding offset stays legal for the attributes.
		constexpr VkDeviceSize VERTEX_ALIGNMENT = 16;

		// A mat4, the fragment state, the two aliased slots and the scene tint: 128
		// bytes, the guaranteed minimum.
		constexpr uint32_t PUSH_CONSTANT_BYTES = sizeof(float) * 32;
		constexpr VkShaderStageFlags PUSH_CONSTANT_STAGES = VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT;

		// A combiner word that modulates the texture with the previous stage, in the
		// layout the fragment shader reads.
		constexpr uint32_t MODULATE_WITH_PREVIOUS = 1u | (0u << 3) | (1u << 8);

		// OpenGL clip space and Vulkan clip space differ in two ways: Y points the other
		// way, and depth runs 0..1 rather than -1..1. Column-major, like everything the
		// game hands over.
		constexpr float CLIP_SPACE_CORRECTION[16] = {
			1.0f,  0.0f, 0.0f, 0.0f,
			0.0f, -1.0f, 0.0f, 0.0f,
			0.0f,  0.0f, 0.5f, 0.0f,
			0.0f,  0.0f, 0.5f, 1.0f,
		};
	}

	//// Private Functions

	namespace
	{
		/** The game's comparison enumeration, which follows OpenGL's order. */
		VkCompareOp MapCompareOperation(uint32_t gdComparison)
		{
			switch (gdComparison)
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
		VkBlendFactor MapBlendFactor(uint32_t gdFactor)
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

	bool VulkanBackend::CreateShaderModule(uint32_t const* code, size_t wordCount, VkShaderModule& outModule)
	{
		VkShaderModuleCreateInfo information{ VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO };
		information.codeSize = wordCount * sizeof(uint32_t);
		information.pCode    = code;

		VkResult const result = vkCreateShaderModule(device, &information, nullptr, &outModule);
		if (result != VK_SUCCESS)
		{
			Fail("vkCreateShaderModule", result);
			return false;
		}

		return true;
	}

	bool VulkanBackend::CreateShaderModules(void)
	{
		if (vertexModules[0] != VK_NULL_HANDLE)
		{
			return true;
		}

		// Indexed by hasColour * 3 + textureCoordinateSets, matching the order the
		// generated header declares them in.
		return CreateShaderModule(GEOMETRY_VERTEX_SPIRV_NONE, _countof(GEOMETRY_VERTEX_SPIRV_NONE), vertexModules[0])
			&& CreateShaderModule(GEOMETRY_VERTEX_SPIRV_TEXTURE, _countof(GEOMETRY_VERTEX_SPIRV_TEXTURE), vertexModules[1])
			&& CreateShaderModule(GEOMETRY_VERTEX_SPIRV_TEXTURE2, _countof(GEOMETRY_VERTEX_SPIRV_TEXTURE2), vertexModules[2])
			&& CreateShaderModule(GEOMETRY_VERTEX_SPIRV_COLOUR, _countof(GEOMETRY_VERTEX_SPIRV_COLOUR), vertexModules[3])
			&& CreateShaderModule(GEOMETRY_VERTEX_SPIRV_COLOUR_TEXTURE, _countof(GEOMETRY_VERTEX_SPIRV_COLOUR_TEXTURE), vertexModules[4])
			&& CreateShaderModule(GEOMETRY_VERTEX_SPIRV_COLOUR_TEXTURE2, _countof(GEOMETRY_VERTEX_SPIRV_COLOUR_TEXTURE2), vertexModules[5])
			&& CreateShaderModule(GEOMETRY_FRAGMENT_SPIRV, _countof(GEOMETRY_FRAGMENT_SPIRV), fragmentModule);
	}

	bool VulkanBackend::CreatePipelineLayout(void)
	{
		if (pipelineLayout != VK_NULL_HANDLE)
		{
			return true;
		}

		// Carry all per-draw state in push constants
		//
		// No uniform buffer and no per-frame allocation.
		VkPushConstantRange range{};
		range.stageFlags = PUSH_CONSTANT_STAGES;
		range.offset     = 0;
		range.size       = PUSH_CONSTANT_BYTES;

		// Bind one set per texture stage, plus a third for the sampler
		//
		// Keeping them separate is what lets a descriptor set stay a property of one
		// texture: a single set with two bindings would need a set per pair of textures
		// instead, and the pairs multiply.
		VkDescriptorSetLayout const setLayouts[] = { imageSetLayout, imageSetLayout, samplerSetLayout };

		VkPipelineLayoutCreateInfo information{ VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO };
		information.setLayoutCount         = _countof(setLayouts);
		information.pSetLayouts            = setLayouts;
		information.pushConstantRangeCount = 1;
		information.pPushConstantRanges    = &range;

		VkResult const result = vkCreatePipelineLayout(device, &information, nullptr, &pipelineLayout);
		if (result != VK_SUCCESS)
		{
			Fail("vkCreatePipelineLayout", result);
			return false;
		}

		return true;
	}

	bool VulkanBackend::CreateGeometryBuffers(void)
	{
		if (!vertexArena.blocks.empty())
		{
			return true;
		}

		// Create the vertex arena
		vertexArena.name          = "vertex";
		vertexArena.usage         = VK_BUFFER_USAGE_VERTEX_BUFFER_BIT;
		vertexArena.blockSize     = VERTEX_BLOCK_SIZE;
		vertexArena.maximumBlocks = ARENA_MAXIMUM_BLOCKS;

		for (int i = 0; i < VERTEX_INITIAL_BLOCKS; i++)
		{
			if (!ArenaAddBlock(vertexArena))
			{
				return false;
			}
		}

		// Create the index arena
		//
		// Indices arriving with a draw are client memory too, so they get the same
		// treatment as the vertices.
		indexArena.name          = "index";
		indexArena.usage         = VK_BUFFER_USAGE_INDEX_BUFFER_BIT;
		indexArena.blockSize     = INDEX_BLOCK_SIZE;
		indexArena.maximumBlocks = ARENA_MAXIMUM_BLOCKS;

		if (!ArenaAddBlock(indexArena))
		{
			return false;
		}

		// Build the quad index pattern
		void* indexMapped = nullptr;
		if (!CreateHostBuffer(VkDeviceSize{ MAXIMUM_QUADS } * 6u * sizeof(uint32_t), VK_BUFFER_USAGE_INDEX_BUFFER_BIT, quadIndexBuffer, quadIndexMemory, indexMapped))
		{
			return false;
		}

		// The mapped memory is untyped; it holds 32-bit indices.
		uint32_t* const indices = static_cast<uint32_t*>(indexMapped);

		for (uint32_t quad = 0; quad < MAXIMUM_QUADS; quad++)
		{
			uint32_t const base   = quad * 4u;
			uint32_t* const quadIndices = indices + quad * 6u;

			quadIndices[0] = base + 0; quadIndices[1] = base + 1; quadIndices[2] = base + 2;
			quadIndices[3] = base + 0; quadIndices[4] = base + 2; quadIndices[5] = base + 3;
		}

		vkUnmapMemory(device, quadIndexMemory);
		quadCapacity = MAXIMUM_QUADS;

		return true;
	}

	VkPipeline VulkanBackend::GetPipeline(PipelineKey const& key)
	{
		// Reuse a pipeline made for the same key
		for (PipelineEntry const& entry : pipelines)
		{
			if (entry.key == key)
			{
				return entry.pipeline;
			}
		}

		// Pick the vertex shader variant for the format's attributes
		//
		// A shader may not declare an input the pipeline does not supply, so the variant
		// has to match which attributes this format actually has.
		VertexLayout const layout = DecodeVertexLayout(key.format);
		uint32_t const variant = (layout.hasColour ? 3u : 0u) + layout.textureCoordinateSets;

		VkPipelineShaderStageCreateInfo stages[2]{};
		stages[0].sType  = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
		stages[0].stage  = VK_SHADER_STAGE_VERTEX_BIT;
		stages[0].module = vertexModules[variant];
		stages[0].pName  = "main";
		stages[1].sType  = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
		stages[1].stage  = VK_SHADER_STAGE_FRAGMENT_BIT;
		stages[1].module = fragmentModule;
		stages[1].pName  = "main";

		// Describe the vertex attributes
		//
		// Position is the only attribute every format has. Colour and texture coordinate
		// are added at the offsets the format itself declares, which is not the same
		// across formats: V3F_C4UB_T2F puts the colour at 12 and V3F_N3F_C4UB puts it at
		// 24.
		VkVertexInputBindingDescription binding{};
		binding.binding   = 0;
		binding.stride    = layout.stride;
		binding.inputRate = VK_VERTEX_INPUT_RATE_VERTEX;

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

		for (uint32_t set = 0; set < layout.textureCoordinateSets; set++)
		{
			attributes[attributeCount].location = 2 + set;
			attributes[attributeCount].binding  = 0;
			attributes[attributeCount].format   = VK_FORMAT_R32G32_SFLOAT;
			attributes[attributeCount].offset   = layout.textureCoordinateOffset[set];
			attributeCount++;
		}

		VkPipelineVertexInputStateCreateInfo vertexInput{ VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO };
		vertexInput.vertexBindingDescriptionCount   = 1;
		vertexInput.pVertexBindingDescriptions      = &binding;
		vertexInput.vertexAttributeDescriptionCount = attributeCount;
		vertexInput.pVertexAttributeDescriptions    = attributes;

		// Describe assembly, viewport and rasterisation
		//
		// Culling is off deliberately. The game's winding convention is not known, and
		// culling the wrong way round would hide geometry.
		VkPipelineInputAssemblyStateCreateInfo assembly{ VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO };
		assembly.topology = key.topology;

		VkPipelineViewportStateCreateInfo viewport{ VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO };
		viewport.viewportCount = 1;
		viewport.scissorCount  = 1;

		VkPipelineRasterizationStateCreateInfo rasterisation{ VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO };
		rasterisation.polygonMode = VK_POLYGON_MODE_FILL;
		rasterisation.cullMode    = VK_CULL_MODE_NONE;
		rasterisation.frontFace   = VK_FRONT_FACE_COUNTER_CLOCKWISE;
		rasterisation.lineWidth   = 1.0f;

		VkPipelineMultisampleStateCreateInfo multisample{ VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO };
		multisample.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT;

		// Describe depth and blending from the key
		VkPipelineDepthStencilStateCreateInfo depthStencil{ VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO };
		depthStencil.depthTestEnable  = key.isDepthTestEnabled ? VK_TRUE : VK_FALSE;
		depthStencil.depthWriteEnable = key.isDepthWriteEnabled ? VK_TRUE : VK_FALSE;
		depthStencil.depthCompareOp   = MapCompareOperation(key.depthComparison);
		depthStencil.minDepthBounds   = 0.0f;
		depthStencil.maxDepthBounds   = 1.0f;

		VkPipelineColorBlendAttachmentState blendAttachment{};
		blendAttachment.blendEnable         = key.isBlendEnabled ? VK_TRUE : VK_FALSE;
		blendAttachment.srcColorBlendFactor = MapBlendFactor(key.sourceFactor);
		blendAttachment.dstColorBlendFactor = MapBlendFactor(key.destinationFactor);
		blendAttachment.colorBlendOp        = VK_BLEND_OP_ADD;
		blendAttachment.srcAlphaBlendFactor = MapBlendFactor(key.sourceFactor);
		blendAttachment.dstAlphaBlendFactor = MapBlendFactor(key.destinationFactor);
		blendAttachment.alphaBlendOp        = VK_BLEND_OP_ADD;
		blendAttachment.colorWriteMask      = key.isColourWriteEnabled ? (VK_COLOR_COMPONENT_R_BIT | VK_COLOR_COMPONENT_G_BIT | VK_COLOR_COMPONENT_B_BIT | VK_COLOR_COMPONENT_A_BIT) : 0;

		VkPipelineColorBlendStateCreateInfo blend{ VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO };
		blend.attachmentCount = 1;
		blend.pAttachments    = &blendAttachment;

		// Leave the viewport and scissor to each draw
		VkDynamicState dynamicStates[] = { VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR };
		VkPipelineDynamicStateCreateInfo dynamic{ VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO };
		dynamic.dynamicStateCount = _countof(dynamicStates);
		dynamic.pDynamicStates    = dynamicStates;

		// Create and remember the pipeline
		VkGraphicsPipelineCreateInfo information{ VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO };
		information.stageCount          = _countof(stages);
		information.pStages             = stages;
		information.pVertexInputState   = &vertexInput;
		information.pInputAssemblyState = &assembly;
		information.pViewportState      = &viewport;
		information.pRasterizationState = &rasterisation;
		information.pMultisampleState   = &multisample;
		information.pDepthStencilState  = &depthStencil;
		information.pColorBlendState    = &blend;
		information.pDynamicState       = &dynamic;
		information.layout              = pipelineLayout;
		information.renderPass          = renderPass;
		information.subpass             = 0;

		VkPipeline pipeline = VK_NULL_HANDLE;
		VkResult const result = vkCreateGraphicsPipelines(device, VK_NULL_HANDLE, 1, &information, nullptr, &pipeline);
		if (result != VK_SUCCESS)
		{
			Fail("vkCreateGraphicsPipelines", result);
			return VK_NULL_HANDLE;
		}

		LogNote("Vulkan: created pipeline for format 0x%x (stride %u, colour %d, texcoord sets %u), topology %d, blend %d (%u,%u).", key.format, layout.stride, layout.hasColour ? 1 : 0, layout.textureCoordinateSets, key.topology, key.isBlendEnabled ? 1 : 0, key.sourceFactor, key.destinationFactor);

		pipelines.push_back({ key, pipeline });
		return pipeline;
	}

	void VulkanBackend::DestroyPipelines(void)
	{
		for (PipelineEntry const& entry : pipelines)
		{
			if (entry.pipeline != VK_NULL_HANDLE)
			{
				vkDestroyPipeline(device, entry.pipeline, nullptr);
			}
		}

		pipelines.clear();
	}

	VulkanBackend::VertexLayout VulkanBackend::DecodeVertexLayout(uint32_t gdVertexFormat)
	{
		// Decoded with the game's own packed-format helpers rather than a hand-written
		// table. The formats disagree about which attributes are present and where, and
		// the packed encoding is the authority.
		VertexLayout layout;
		layout.stride = RZVertexFormatStride(gdVertexFormat);

		// Find the colour
		layout.hasColour = RZVertexFormatNumElements(gdVertexFormat, kGDElementType_Color) != 0;
		if (layout.hasColour)
		{
			layout.colourOffset = RZVertexFormatElementOffset(gdVertexFormat, kGDElementType_Color, 0);
		}

		// Find the coordinate sets
		//
		// This counts coordinate sets, not components. The terrain carries two, one per
		// texture stage, which is what the second stage samples with.
		layout.textureCoordinateSets = RZVertexFormatNumElements(gdVertexFormat, kGDElementType_TexCoord);
		if (layout.textureCoordinateSets > 2)
		{
			layout.textureCoordinateSets = 2;
		}

		for (uint32_t set = 0; set < layout.textureCoordinateSets; set++)
		{
			layout.textureCoordinateOffset[set] = RZVertexFormatElementOffset(gdVertexFormat, kGDElementType_TexCoord, set);
		}

		return layout;
	}

	bool VulkanBackend::MapTopology(uint32_t gdPrimitiveType, VkPrimitiveTopology& outTopology, bool& outIsQuadList)
	{
		// The game's primitive numbering, from its own translation table: 0 triangles, 1
		// triangle strip, 2 triangle fan, 3 points, 4 lines, 5 line strip, 6 quads, 7
		// quad strip.
		outIsQuadList = false;

		switch (gdPrimitiveType)
		{
		case 0: outTopology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;  return true;
		case 1: outTopology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_STRIP; return true;
		case 2: outTopology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_FAN;   return true;
		case 3: outTopology = VK_PRIMITIVE_TOPOLOGY_POINT_LIST;     return true;
		case 4: outTopology = VK_PRIMITIVE_TOPOLOGY_LINE_LIST;      return true;
		case 5: outTopology = VK_PRIMITIVE_TOPOLOGY_LINE_STRIP;     return true;
		case 6: outTopology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST; outIsQuadList = true; return true;

		// A quad strip covers the same surface as a triangle strip over the same
		// vertices, so it needs no index expansion at all.
		case 7: outTopology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_STRIP; return true;

		default:
			LogNote("Vulkan: primitive type %u is not handled; skipping the draw.", gdPrimitiveType);
			return false;
		}
	}

	bool VulkanBackend::ArenaAddBlock(Arena& arena)
	{
		ArenaBlock block;

		if (!CreateHostBuffer(arena.blockSize, arena.usage, block.buffer, block.memory, block.mapped))
		{
			return false;
		}

		arena.blocks.push_back(block);
		return true;
	}

	bool VulkanBackend::ArenaAllocate(Arena& arena, VkDeviceSize bytes, VkDeviceSize alignment, VkBuffer& outBuffer, VkDeviceSize& outOffset, uint8_t*& outAddress)
	{
		if (arena.blocks.empty() || bytes > arena.blockSize)
		{
			return false;
		}

		// Move to the next block when this one is full
		//
		// Blocks stay allocated once added, so a heavy frame pays for the allocation once
		// rather than every time it recurs.
		VkDeviceSize aligned = (arena.usedBytes + alignment - 1u) & ~(alignment - 1u);

		if (aligned + bytes > arena.blockSize)
		{
			if (arena.currentBlock + 1 >= arena.blocks.size())
			{
				if (arena.blocks.size() >= arena.maximumBlocks || !ArenaAddBlock(arena))
				{
					return false;
				}

				LogNote("Vulkan: the per-frame %s data grew to %u blocks of %llu MB.", arena.name, arena.blocks.size(), arena.blockSize >> 20);
			}

			arena.currentBlock++;
			aligned = 0;
		}

		// Hand out the space
		//
		// The mapped memory is untyped bytes.
		ArenaBlock const& block = arena.blocks[arena.currentBlock];

		outBuffer       = block.buffer;
		outOffset       = aligned;
		outAddress      = static_cast<uint8_t*>(block.mapped) + aligned;
		arena.usedBytes = aligned + bytes;
		return true;
	}

	void VulkanBackend::ArenaRewind(Arena& arena)
	{
		arena.currentBlock = 0;
		arena.usedBytes    = 0;
	}

	void VulkanBackend::DestroyArena(Arena& arena)
	{
		for (ArenaBlock& block : arena.blocks)
		{
			if (block.mapped != nullptr)        { vkUnmapMemory(device, block.memory); }
			if (block.memory != VK_NULL_HANDLE) { vkFreeMemory(device, block.memory, nullptr); }
			if (block.buffer != VK_NULL_HANDLE) { vkDestroyBuffer(device, block.buffer, nullptr); }
		}

		arena.blocks.clear();
		ArenaRewind(arena);
	}

	bool VulkanBackend::UploadVertices(void const* vertices, uint32_t firstVertex, uint32_t vertexCount, uint32_t stride, VkBuffer& outBuffer, VkDeviceSize& outOffset)
	{
		// Reserve the space
		//
		// Measured in 64 bits, so a range the game's indices make absurdly large is
		// refused by the arena rather than wrapping.
		VkDeviceSize const bytes = VkDeviceSize{ vertexCount } * stride;
		uint8_t* destination = nullptr;

		if (!ArenaAllocate(vertexArena, bytes, VERTEX_ALIGNMENT, outBuffer, outOffset, destination))
		{
			LogNote("Vulkan: no room for per-frame vertex data; dropping a draw of %u vertices.", vertexCount);
			return false;
		}

		// Copy the vertices
		//
		// The game's vertices are untyped bytes, and the arena accepted the size, so it
		// fits within one block and within a size_t.
		uint8_t const* const source = static_cast<uint8_t const*>(vertices) + size_t{ firstVertex } * stride;
		memcpy(destination, source, static_cast<size_t>(bytes));

		return true;
	}

	bool VulkanBackend::BindDrawState(uint32_t gdVertexFormat, VkPrimitiveTopology topology, VkBuffer vertexBuffer, VkDeviceSize vertexOffset, uint32_t textureCoordinateSets)
	{
		// Find the pipeline for the current state
		PipelineKey const key{ gdVertexFormat, topology, isBlendEnabled, blendSourceFactor, blendDestinationFactor, isDepthTestEnabled, isDepthWriteEnabled, depthComparison, isColourWriteEnabled };
		VkPipeline const pipeline = GetPipeline(key);
		if (pipeline == VK_NULL_HANDLE)
		{
			return false;
		}

		// Open the pass and apply the viewport
		//
		// Per draw, not once per render pass. The game changes the viewport between draws
		// inside a single pass: it tiles the interface by drawing full-sized quads and
		// letting a much smaller viewport clip each one. Applying the viewport only when
		// the pass opens left every draw in that pass using whichever viewport happened
		// to be current at the time, which stretched the interface across the window.
		BeginRenderPassIfNeeded();
		ApplyViewport();

		// Decide which of the shader's paths the draw takes
		//
		// The second stage runs only when it is switched on, has a texture, and the
		// geometry carries a coordinate set to sample it with. All three matter: the game
		// leaves a 4x4 placeholder bound to the stage for the whole session and turns the
		// stage itself off, so taking the binding as the signal modulates the city
		// terrain down to black. Everything else keeps the texture environment path.
		//
		// Generated coordinates take the aliased slots when the second stage is not using
		// them. The interface keeps these apart on its own: the cloud shadow pass that
		// generates coordinates runs on a single stage.
		bool const isTwoStage   = textureCoordinateSets >= 2 && currentTexture1 != 0 && isStageEnabled[1];
		bool const isGenerating = isTextureGenerationActive && !isTwoStage;

		fragmentState[3] = isGenerating ? 3.0f : (isTwoStage ? 2.0f : 1.0f);

		// Bind everything
		vkCmdBindPipeline(commandBuffer, VK_PIPELINE_BIND_POINT_GRAPHICS, pipeline);
		PushDrawConstants(isGenerating);
		BindTextures(isTwoStage);

		VkBuffer buffers[] = { vertexBuffer };
		VkDeviceSize offsets[] = { vertexOffset };
		vkCmdBindVertexBuffers(commandBuffer, 0, 1, buffers, offsets);

		return true;
	}

	void VulkanBackend::PushDrawConstants(bool isGenerating)
	{
		// Copy the state this draw sends
		//
		// Copies, because the game's own settings must survive for the next draw that has
		// the first stage on.
		float    drawFragmentState[4];
		uint32_t drawCombinerState[4];
		memcpy(drawFragmentState, fragmentState, sizeof(drawFragmentState));
		memcpy(drawCombinerState, combinerState, sizeof(drawCombinerState));

		// Pack the environment mode with the alpha source in a higher digit
		//
		// The push constant block is full at the guaranteed 128 bytes and both are small
		// enough to share one slot. The shader reads its state as floats, and small
		// integers convert exactly.
		float const alphaSourceFlag = isAlphaFromVertex ? 8.0f : 0.0f;
		drawFragmentState[2] = static_cast<float>(textureEnvironmentMode) + alphaSourceFlag;

		// Pass the primary colour through a disabled first stage
		//
		// A stage with texturing off passes the primary colour through untouched,
		// whatever its environment mode or combiner says. The white texture bound in its
		// place does that under modulate, so the stage is pushed as modulate rather than
		// as the game left it: replace or decal would otherwise turn it white, and a
		// combiner could turn it anything.
		if (!isStageEnabled[0])
		{
			drawFragmentState[2] = 1.0f + alphaSourceFlag;
			drawCombinerState[0] = MODULATE_WITH_PREVIOUS;
			drawCombinerState[1] = MODULATE_WITH_PREVIOUS;
		}

		// Apply the diagnostics
		//
		// Pass identification replaces the draw's colour with a flat one chosen by its
		// blend configuration, so a capture says directly which pass owns a given region
		// of the picture. The channel view shows one shader input on its own.
		if (shouldShowPassColours)
		{
			int pass;

			if (!isBlendEnabled)
			{
				pass = (blendSourceFactor == 1 && blendDestinationFactor == 0) ? 0 : ((blendSourceFactor == 1 && blendDestinationFactor == 1) ? 1 : 4);
			}
			else
			{
				pass = (blendSourceFactor == 4 && blendDestinationFactor == 1) ? 2 : ((blendSourceFactor == 4 && blendDestinationFactor == 5) ? 3 : 5);
			}

			drawFragmentState[3] = 10.0f + static_cast<float>(pass);
		}

		if (debugChannel >= 0)
		{
			drawFragmentState[3] = 20.0f + static_cast<float>(debugChannel);
		}

		// Push the transform and the fragment state
		vkCmdPushConstants(commandBuffer, pipelineLayout, PUSH_CONSTANT_STAGES, 0, sizeof(transform), transform);
		vkCmdPushConstants(commandBuffer, pipelineLayout, PUSH_CONSTANT_STAGES, sizeof(transform), sizeof(drawFragmentState), drawFragmentState);

		// Push the two aliased slots, 32 bytes, filled according to the path
		uint32_t const aliasOffset = sizeof(transform) + sizeof(fragmentState);

		if (isGenerating)
		{
			vkCmdPushConstants(commandBuffer, pipelineLayout, PUSH_CONSTANT_STAGES, aliasOffset, sizeof(textureGenerationRows), textureGenerationRows);
		}
		else
		{
			vkCmdPushConstants(commandBuffer, pipelineLayout, PUSH_CONSTANT_STAGES, aliasOffset, sizeof(drawCombinerState), drawCombinerState);
			vkCmdPushConstants(commandBuffer, pipelineLayout, PUSH_CONSTANT_STAGES, aliasOffset + sizeof(combinerState), sizeof(constantColour), constantColour);
		}

		// Push the scene tint
		vkCmdPushConstants(commandBuffer, pipelineLayout, PUSH_CONSTANT_STAGES, aliasOffset + sizeof(combinerState) + sizeof(constantColour), sizeof(sceneTint), sceneTint);
	}

	void VulkanBackend::BindTextures(bool isTwoStage)
	{
		// Pick each stage's texture
		//
		// Slot 0 holds the 1x1 white texture, so an unset or deleted texture still binds
		// something valid and multiplies by one. That is also what the second stage gets
		// when it is idle, since the fragment shader samples both unconditionally.
		//
		// A stage with texturing switched off gets it too. Geometry that carries no
		// texture coordinates still leaves a texture bound, and sampling it at coordinate
		// zero multiplies the vertex colour by whatever happens to sit in that texel. The
		// water side faces are drawn exactly that way, eight untextured colour-only
		// draws, and the texel they landed on took them to black.
		uint32_t const bound  = (isStageEnabled[0] && currentTexture < textures.size() && textures[currentTexture].isLive) ? currentTexture : 0;
		uint32_t const bound1 = (isTwoStage && currentTexture1 < textures.size() && textures[currentTexture1].isLive) ? currentTexture1 : 0;

		NoteTextureUse(bound);
		NoteTextureUse(bound1);

		// Bind their sets
		//
		// One sampler serves both stages, taken from the first stage's texture. The two
		// stages only ever run together on the terrain, which uses the same parameters
		// for both.
		VkDescriptorSet const sets[] = {
			textures[bound].descriptor,
			textures[bound1].descriptor,
			GetSamplerSet(bound),
		};

		vkCmdBindDescriptorSets(commandBuffer, VK_PIPELINE_BIND_POINT_GRAPHICS, pipelineLayout, 0, _countof(sets), sets, 0, nullptr);
	}

	//// Public API

	void VulkanBackend::SetTransform(float const* openGlModelViewProjection)
	{
		// Correct the game's matrix into Vulkan clip space
		//
		// Doing it here keeps the game's matrices untouched and confines the difference
		// to the one place that knows it is Vulkan. Column-major throughout, matching
		// both the game and GLSL, so the result can be pushed straight into the shader.
		for (int column = 0; column < 4; column++)
		{
			for (int row = 0; row < 4; row++)
			{
				float sum = 0.0f;
				for (int k = 0; k < 4; k++)
				{
					sum += CLIP_SPACE_CORRECTION[k * 4 + row] * openGlModelViewProjection[column * 4 + k];
				}

				transform[column * 4 + row] = sum;
			}
		}
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

	void VulkanBackend::SetBlendState(bool isEnabled, uint32_t sourceFactor, uint32_t destinationFactor)
	{
		isBlendEnabled         = isEnabled;
		blendSourceFactor      = sourceFactor;
		blendDestinationFactor = destinationFactor;
	}

	void VulkanBackend::SetAlphaTest(int comparison, float reference)
	{
		// The shader reads its state as floats, and small integers convert exactly.
		fragmentState[0] = static_cast<float>(comparison);
		fragmentState[1] = reference;
	}

	void VulkanBackend::SetTextureEnvironmentMode(uint32_t mode)
	{
		// The game's own order: 0 replace, 1 modulate, 2 decal. Anything else falls back
		// to modulate, which is the fixed function default.
		textureEnvironmentMode = (mode <= 2u) ? mode : 1u;
	}

	void VulkanBackend::SetColourWrite(bool isEnabled)
	{
		isColourWriteEnabled = isEnabled;
	}

	void VulkanBackend::SetDepthState(bool isTestEnabled, bool isWriteEnabled, uint32_t comparison)
	{
		isDepthTestEnabled  = isTestEnabled;
		isDepthWriteEnabled = isWriteEnabled;
		depthComparison     = comparison;
	}

	void VulkanBackend::SetSceneTint(float red, float green, float blue, float alpha, bool isAlphaFromVertexColour)
	{
		sceneTint[0] = red;
		sceneTint[1] = green;
		sceneTint[2] = blue;
		sceneTint[3] = alpha;

		isAlphaFromVertex = isAlphaFromVertexColour;
	}

	void VulkanBackend::SetConstantColour(float red, float green, float blue, float alpha)
	{
		constantColour[0] = red;
		constantColour[1] = green;
		constantColour[2] = blue;
		constantColour[3] = alpha;
	}

	void VulkanBackend::SetCombinerState(uint32_t stage, uint32_t packedRgb, uint32_t packedAlpha)
	{
		if (stage > 1)
		{
			return;
		}

		combinerState[stage * 2 + 0] = packedRgb;
		combinerState[stage * 2 + 1] = packedAlpha;
	}

	void VulkanBackend::SetTextureGeneration(bool isActive, float const* rowS, float const* rowT)
	{
		isTextureGenerationActive = isActive;

		if (!isActive || rowS == nullptr || rowT == nullptr)
		{
			return;
		}

		memcpy(textureGenerationRows, rowS, sizeof(float) * 4);
		memcpy(textureGenerationRows + 4, rowT, sizeof(float) * 4);
	}

	void VulkanBackend::SetDebugPassColours(bool isEnabled)
	{
		shouldShowPassColours = isEnabled;
		LogNote("Vulkan: pass identification colours are %s.", isEnabled ? "on" : "off");
	}

	void VulkanBackend::SetDebugChannel(int channel)
	{
		debugChannel = channel;
	}

	void VulkanBackend::DrawVertices(uint32_t gdPrimitiveType, uint32_t gdVertexFormat, void const* vertices, uint32_t firstVertex, uint32_t vertexCount)
	{
		VertexLayout const layout = DecodeVertexLayout(gdVertexFormat);

		if (vertices == nullptr || vertexCount == 0 || layout.stride == 0)
		{
			return;
		}

		// Translate the primitive and start the frame
		VkPrimitiveTopology topology;
		bool isQuadList = false;

		if (!MapTopology(gdPrimitiveType, topology, isQuadList) || !EnsureFrame())
		{
			return;
		}

		// Copy the vertices and bind the state
		VkBuffer     vertexBuffer = VK_NULL_HANDLE;
		VkDeviceSize vertexOffset = 0;
		if (!UploadVertices(vertices, firstVertex, vertexCount, layout.stride, vertexBuffer, vertexOffset))
		{
			return;
		}

		if (!BindDrawState(gdVertexFormat, topology, vertexBuffer, vertexOffset, layout.textureCoordinateSets))
		{
			return;
		}

		// Draw plain primitives directly
		if (!isQuadList)
		{
			vkCmdDraw(commandBuffer, vertexCount, 1, 0, 0);
			return;
		}

		// Draw quads through the shared quad indices
		uint32_t quads = vertexCount / 4u;
		if (quads > quadCapacity)
		{
			LogNote("Vulkan: %u quads exceeds the index buffer capacity of %u; clamping.", quads, quadCapacity);
			quads = quadCapacity;
		}

		if (quads == 0)
		{
			return;
		}

		vkCmdBindIndexBuffer(commandBuffer, quadIndexBuffer, 0, VK_INDEX_TYPE_UINT32);
		vkCmdDrawIndexed(commandBuffer, quads * 6u, 1, 0, 0, 0);
	}

	void VulkanBackend::DrawIndexedVertices(uint32_t gdPrimitiveType, uint32_t gdVertexFormat, void const* vertices, void const* indices, uint32_t indexCount, bool isIndex32Bit)
	{
		VertexLayout const layout = DecodeVertexLayout(gdVertexFormat);

		if (vertices == nullptr || indices == nullptr || indexCount == 0 || layout.stride == 0)
		{
			return;
		}

		// Translate the primitive and start the frame
		VkPrimitiveTopology topology;
		bool isQuadList = false;

		if (!MapTopology(gdPrimitiveType, topology, isQuadList) || !EnsureFrame())
		{
			return;
		}

		// Measure the vertex range the indices reference
		//
		// The indices are client memory and say nothing about how many vertices back
		// them, so the range has to be measured before anything can be copied.
		//
		// Only the span actually referenced is taken, not everything from zero. The game
		// indexes small windows of large shared arrays, and uploading each window's whole
		// prefix exhausted the per-frame arena partway through a city frame, which
		// dropped several hundred draws. The index array is untyped; its width says how
		// to read it.
		uint32_t const* const wideIndices   = static_cast<uint32_t const*>(indices);
		uint16_t const* const narrowIndices = static_cast<uint16_t const*>(indices);

		uint32_t lowest  = UINT32_MAX;
		uint32_t highest = 0;

		for (uint32_t i = 0; i < indexCount; i++)
		{
			uint32_t const index = isIndex32Bit ? wideIndices[i] : narrowIndices[i];

			lowest  = std::min(lowest, index);
			highest = std::max(highest, index);
		}

		// Copy that range of vertices
		uint32_t const vertexCount = highest - lowest + 1u;

		VkBuffer     vertexBuffer = VK_NULL_HANDLE;
		VkDeviceSize vertexOffset = 0;
		if (!UploadVertices(vertices, lowest, vertexCount, layout.stride, vertexBuffer, vertexOffset))
		{
			return;
		}

		// Reserve space for the indices, expanded when they describe quads
		//
		// Quads are expanded here rather than being drawn through the shared quad index
		// buffer, because that buffer describes consecutive vertices and these do not
		// have to be consecutive.
		uint32_t const emitted = isQuadList ? (indexCount / 4u) * 6u : indexCount;
		if (emitted == 0)
		{
			return;
		}

		size_t const indexBytes = size_t{ emitted } * sizeof(uint32_t);
		VkBuffer     indexBuffer = VK_NULL_HANDLE;
		VkDeviceSize indexOffset = 0;
		uint8_t*     indexDestination = nullptr;

		if (!ArenaAllocate(indexArena, indexBytes, sizeof(uint32_t), indexBuffer, indexOffset, indexDestination))
		{
			LogNote("Vulkan: no room for per-frame index data; dropping a draw of %u indices.", indexCount);
			return;
		}

		// Write them as 32-bit indices
		//
		// The arena hands out bytes, aligned for 32-bit indices. Narrow indices are
		// widened rather than kept narrow, so one buffer and one index type serve every
		// draw regardless of what the game sent.
		uint32_t* const output = reinterpret_cast<uint32_t*>(indexDestination);

		if (isQuadList)
		{
			uint32_t const quads = indexCount / 4u;
			for (uint32_t quad = 0; quad < quads; quad++)
			{
				uint32_t corner[4];
				for (uint32_t c = 0; c < 4; c++)
				{
					uint32_t const at = quad * 4u + c;
					corner[c] = isIndex32Bit ? wideIndices[at] : narrowIndices[at];
				}

				output[quad * 6u + 0] = corner[0];
				output[quad * 6u + 1] = corner[1];
				output[quad * 6u + 2] = corner[2];
				output[quad * 6u + 3] = corner[0];
				output[quad * 6u + 4] = corner[2];
				output[quad * 6u + 5] = corner[3];
			}
		}
		else if (isIndex32Bit)
		{
			memcpy(output, indices, indexBytes);
		}
		else
		{
			for (uint32_t i = 0; i < indexCount; i++)
			{
				output[i] = narrowIndices[i];
			}
		}

		// Bind the state and draw
		if (!BindDrawState(gdVertexFormat, topology, vertexBuffer, vertexOffset, layout.textureCoordinateSets))
		{
			return;
		}

		vkCmdBindIndexBuffer(commandBuffer, indexBuffer, indexOffset, VK_INDEX_TYPE_UINT32);

		// Offset the indices back to the start of the copied slice
		//
		// The indices went in unchanged, so they still count from the start of the game's
		// array while the buffer holds only the slice from the lowest one onward. A
		// negative vertex offset closes that gap, which is what the parameter is signed
		// for. Real indices are far below INT32_MAX, so the conversion loses nothing.
		vkCmdDrawIndexed(commandBuffer, emitted, 1, 0, -static_cast<int32_t>(lowest), 0);
	}
}
