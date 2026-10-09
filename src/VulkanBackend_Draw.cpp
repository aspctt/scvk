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
 * push constants or dynamic state. The game's geometry lives in client memory, so each
 * draw copies its vertices and indices into per-frame arenas first.
 *
 * A city redraw at the widest zoom records over a hundred thousand draws, so each one
 * binds only what differs from the draw before it: the pipeline comes from a hash table
 * with the last one checked first, and the pipeline, push constants, descriptor sets,
 * vertex buffers, viewport and dynamic state are each skipped when unchanged.
 */

//// Dependencies

#include "VulkanBackend.h"
#include "Logger.h"
#include "ShaderBinaries.h"

#include <VertexFormatUtils.h>

#include <algorithm>
#include <stddef.h>
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
		// The cap keeps the arena from eating the address space of what is a 32-bit
		// process; a frame that needs more still is submitted part way instead.
		constexpr VkDeviceSize VERTEX_BLOCK_SIZE     = 32u * 1024u * 1024u;
		constexpr int          VERTEX_INITIAL_BLOCKS = 2;
		constexpr VkDeviceSize INDEX_BLOCK_SIZE      = 8u * 1024u * 1024u;
		constexpr size_t       ARENA_MAXIMUM_BLOCKS  = 8;

		// Staging for blits: a full-screen 1920x1080 BGRA image is about 8 MB.
		constexpr VkDeviceSize STAGING_BLOCK_SIZE     = 16u * 1024u * 1024u;
		constexpr size_t       STAGING_MAXIMUM_BLOCKS = 4;

		// Uniforms of the passes of scvk's own, a few hundred bytes each.
		constexpr VkDeviceSize UNIFORM_BLOCK_SIZE     = 1024u * 1024u;
		constexpr size_t       UNIFORM_MAXIMUM_BLOCKS = 4;

		// Vertex data is aligned so the binding offset stays legal for the attributes.
		constexpr VkDeviceSize VERTEX_ALIGNMENT = 16;

		// A mat4, the fragment state, the combiner network, the environment colour and the
		// scene tint: 128 bytes, the guaranteed minimum.
		constexpr uint32_t PUSH_CONSTANT_BYTES = sizeof(float) * 32;
		constexpr VkShaderStageFlags PUSH_CONSTANT_STAGES = VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT;

		// A combiner word that modulates the texture with the previous stage, in the
		// layout the fragment shader reads.
		constexpr uint32_t MODULATE_WITH_PREVIOUS = 1u | (0u << 3) | (1u << 8);

		// A combiner word that hands the previous stage's result on unchanged.
		constexpr uint32_t PASS_PREVIOUS = 0u | (1u << 3);

		// The texture environment modes, in the game's own order, and the lighting flags
		// packed above the mode for the vertex stage.
		constexpr uint32_t ENVIRONMENT_MODULATE    = 1;
		constexpr uint32_t ENVIRONMENT_BLEND       = 3;
		constexpr uint32_t ENVIRONMENT_COMBINE     = 4;
		constexpr uint32_t ENVIRONMENT_COMBINE4    = 5;
		constexpr uint32_t ALPHA_FROM_VERTEX_FLAG  = 8;
		constexpr uint32_t COLOUR_FROM_VERTEX_FLAG = 16;

		// The last of the game's fog modes, which run exponential, squared exponential and
		// linear, and the mode the vertex stage reads as no fog.
		constexpr uint32_t GD_FOG_MODE_LINEAR = 2;
		constexpr float    FOG_MODE_OFF       = 0.0f;

		// Where a stage's coordinates come from, in the draw record.
		constexpr float STAGE_SOURCE_FIRST_SET  = 0.0f;
		constexpr float STAGE_SOURCE_SECOND_SET = 1.0f;
		constexpr float STAGE_SOURCE_POSITION   = 2.0f;

		// Floats per stage in the draw record's rows: s, then t.
		constexpr uint32_t STAGE_ROW_FLOATS = 8;

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

		/** The game's stencil operations, the five its DirectX driver maps: keep, replace, increment and decrement clamped, invert. */
		VkStencilOp MapStencilOperation(uint32_t gdOperation)
		{
			switch (gdOperation)
			{
			case 1:  return VK_STENCIL_OP_REPLACE;
			case 2:  return VK_STENCIL_OP_INCREMENT_AND_CLAMP;
			case 3:  return VK_STENCIL_OP_DECREMENT_AND_CLAMP;
			case 4:  return VK_STENCIL_OP_INVERT;
			default: return VK_STENCIL_OP_KEEP;
			}
		}

		/** Mixes a value into a hash, FNV-1a style. */
		size_t MixHash(size_t hash, uint32_t value)
		{
			return (hash ^ value) * 16777619u;
		}
	}

	size_t VulkanBackend::PipelineKeyHash::operator()(PipelineKey const& key) const
	{
		size_t hash = 2166136261u;
		hash = MixHash(hash, key.format);
		hash = MixHash(hash, static_cast<uint32_t>(key.topology));
		hash = MixHash(hash, (key.isBlendEnabled ? 1u : 0u) | (key.sourceFactor << 1) | (key.destinationFactor << 6));
		hash = MixHash(hash, (key.isDepthTestEnabled ? 1u : 0u) | (key.isDepthWriteEnabled ? 2u : 0u) | (key.depthComparison << 2) | (key.isColourWriteEnabled ? 64u : 0u) | (key.isFaceCullingEnabled ? 128u : 0u) | (key.isFlatShaded ? 256u : 0u));
		hash = MixHash(hash, (key.isStencilTestEnabled ? 1u : 0u) | (key.stencilComparison << 1) | (key.stencilFailOperation << 5) | (key.stencilDepthFailOperation << 9) | (key.stencilPassOperation << 13));
		return hash;
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

		// Indexed by flat * 6 + hasColour * 3 + textureCoordinateSets, matching the order
		// the generated header declares them in. The flat variants hand the colour over
		// without interpolating it, for ShadeModel(flat).
		return CreateShaderModule(GEOMETRY_VERTEX_SPIRV_NONE, _countof(GEOMETRY_VERTEX_SPIRV_NONE), vertexModules[0])
			&& CreateShaderModule(GEOMETRY_VERTEX_SPIRV_TEXTURE, _countof(GEOMETRY_VERTEX_SPIRV_TEXTURE), vertexModules[1])
			&& CreateShaderModule(GEOMETRY_VERTEX_SPIRV_TEXTURE2, _countof(GEOMETRY_VERTEX_SPIRV_TEXTURE2), vertexModules[2])
			&& CreateShaderModule(GEOMETRY_VERTEX_SPIRV_COLOUR, _countof(GEOMETRY_VERTEX_SPIRV_COLOUR), vertexModules[3])
			&& CreateShaderModule(GEOMETRY_VERTEX_SPIRV_COLOUR_TEXTURE, _countof(GEOMETRY_VERTEX_SPIRV_COLOUR_TEXTURE), vertexModules[4])
			&& CreateShaderModule(GEOMETRY_VERTEX_SPIRV_COLOUR_TEXTURE2, _countof(GEOMETRY_VERTEX_SPIRV_COLOUR_TEXTURE2), vertexModules[5])
			&& CreateShaderModule(GEOMETRY_VERTEX_FLAT_SPIRV_NONE, _countof(GEOMETRY_VERTEX_FLAT_SPIRV_NONE), vertexModules[6])
			&& CreateShaderModule(GEOMETRY_VERTEX_FLAT_SPIRV_TEXTURE, _countof(GEOMETRY_VERTEX_FLAT_SPIRV_TEXTURE), vertexModules[7])
			&& CreateShaderModule(GEOMETRY_VERTEX_FLAT_SPIRV_TEXTURE2, _countof(GEOMETRY_VERTEX_FLAT_SPIRV_TEXTURE2), vertexModules[8])
			&& CreateShaderModule(GEOMETRY_VERTEX_FLAT_SPIRV_COLOUR, _countof(GEOMETRY_VERTEX_FLAT_SPIRV_COLOUR), vertexModules[9])
			&& CreateShaderModule(GEOMETRY_VERTEX_FLAT_SPIRV_COLOUR_TEXTURE, _countof(GEOMETRY_VERTEX_FLAT_SPIRV_COLOUR_TEXTURE), vertexModules[10])
			&& CreateShaderModule(GEOMETRY_VERTEX_FLAT_SPIRV_COLOUR_TEXTURE2, _countof(GEOMETRY_VERTEX_FLAT_SPIRV_COLOUR_TEXTURE2), vertexModules[11])
			&& CreateShaderModule(GEOMETRY_FRAGMENT_SPIRV, _countof(GEOMETRY_FRAGMENT_SPIRV), fragmentModules[0])
			&& CreateShaderModule(GEOMETRY_FRAGMENT_FLAT_SPIRV, _countof(GEOMETRY_FRAGMENT_FLAT_SPIRV), fragmentModules[1])
			&& CreateShaderModule(GEOMETRY_VERTEX_LIT_SPIRV, _countof(GEOMETRY_VERTEX_LIT_SPIRV), vertexModules[12])
			&& CreateShaderModule(GEOMETRY_FRAGMENT_LIT_SPIRV, _countof(GEOMETRY_FRAGMENT_LIT_SPIRV), fragmentModules[2]);
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

		// Bind one image set per texture stage, then one sampler set per stage
		//
		// Keeping them separate is what lets a descriptor set stay a property of one
		// texture: a single set with two bindings would need a set per pair of textures
		// instead, and the pairs multiply. Four sets is the minimum every Vulkan device
		// supports. The blits and the shadow casters use the same layout.
		VkDescriptorSetLayout const setLayouts[] = { imageSetLayout, imageSetLayout, samplerSetLayout, samplerSetLayout };

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

		// The staging the blits copy from, and the uniforms of scvk's own passes
		stagingArena.name          = "staging";
		stagingArena.usage         = VK_BUFFER_USAGE_TRANSFER_SRC_BIT;
		stagingArena.blockSize     = STAGING_BLOCK_SIZE;
		stagingArena.maximumBlocks = STAGING_MAXIMUM_BLOCKS;

		uniformArena.name          = "uniform";
		uniformArena.usage         = VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT;
		uniformArena.blockSize     = UNIFORM_BLOCK_SIZE;
		uniformArena.maximumBlocks = UNIFORM_MAXIMUM_BLOCKS;

		if (!ArenaAddBlock(indexArena) || !ArenaAddBlock(stagingArena) || !ArenaAddBlock(uniformArena))
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
		// Reuse the pipeline of the last draw, which most draws share
		if (lastPipeline != VK_NULL_HANDLE && key == lastPipelineKey)
		{
			return lastPipeline;
		}

		// Or one made earlier for the same key
		VkPipeline pipeline = VK_NULL_HANDLE;
		auto const found = pipelines.find(key);

		if (found != pipelines.end())
		{
			pipeline = found->second;
		}
		else
		{
			pipeline = CreatePipeline(key);
			if (pipeline == VK_NULL_HANDLE)
			{
				return VK_NULL_HANDLE;
			}

			pipelines.emplace(key, pipeline);
		}

		lastPipelineKey = key;
		lastPipeline    = pipeline;
		return pipeline;
	}

	VkPipeline VulkanBackend::CreatePipeline(PipelineKey const& key)
	{
		PhaseScope const creating(*this, FRAME_PHASE_PIPELINES);

		// Pick the shader variants for the format's attributes
		//
		// A shader may not declare an input the pipeline does not supply, so the variant
		// has to match which attributes this format actually has.
		VertexLayout const layout  = DecodeVertexLayout(key.format);
		bool const         isLit   = key.format == LIT_VERTEX_FORMAT;
		uint32_t const     variant = isLit ? 12u : (key.isFlatShaded ? 6u : 0u) + (layout.hasColour ? 3u : 0u) + layout.textureCoordinateSets;

		VkPipelineShaderStageCreateInfo stages[2]{};
		stages[0].sType  = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
		stages[0].stage  = VK_SHADER_STAGE_VERTEX_BIT;
		stages[0].module = vertexModules[variant];
		stages[0].pName  = "main";
		stages[1].sType  = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
		stages[1].stage  = VK_SHADER_STAGE_FRAGMENT_BIT;
		stages[1].module = fragmentModules[isLit ? 2 : (key.isFlatShaded ? 1 : 0)];
		stages[1].pName  = "main";

		// Describe the vertex attributes
		//
		// Position is the only attribute every format has. Colour and texture coordinate
		// are added at the offsets the format itself declares, which is not the same
		// across formats: V3F_C4UB_T2F puts the colour at 12 and V3F_N3F_C4UB puts it at
		// 24.
		VkVertexInputBindingDescription bindings[2]{};
		bindings[0].binding   = 0;
		bindings[0].stride    = layout.stride;
		bindings[0].inputRate = VK_VERTEX_INPUT_RATE_VERTEX;

		// The draw record is read per instance, and every draw is one instance, so all of a
		// draw's vertices read the same record.
		bindings[1].binding   = 1;
		bindings[1].stride    = sizeof(DrawRecord);
		bindings[1].inputRate = VK_VERTEX_INPUT_RATE_INSTANCE;

		VkVertexInputAttributeDescription attributes[12 + 1 + 2 * LIT_LIGHT_COUNT]{};
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

		// The draw record's parts, a vec4 each: the fog's row, parameters and colour, each
		// stage's two rows, and the stages' sources.
		uint32_t const rowsOffset    = offsetof(DrawRecord, stageRows);
		uint32_t const recordParts[] = {
			offsetof(DrawRecord, fogDistanceRow),
			offsetof(DrawRecord, fogParameters),
			offsetof(DrawRecord, fogColour),
			rowsOffset,
			rowsOffset + sizeof(float) * 4,
			rowsOffset + sizeof(float) * 8,
			rowsOffset + sizeof(float) * 12,
			offsetof(DrawRecord, stageSources),
		};

		for (uint32_t part = 0; part < _countof(recordParts); part++)
		{
			attributes[attributeCount].location = 4 + part;
			attributes[attributeCount].binding  = 1;
			attributes[attributeCount].format   = VK_FORMAT_R32G32B32A32_SFLOAT;
			attributes[attributeCount].offset   = recordParts[part];
			attributeCount++;
		}

		// The lit variant's normal, vectors to the lights and light colours
		if (isLit)
		{
			attributes[attributeCount].location = 12;
			attributes[attributeCount].binding  = 0;
			attributes[attributeCount].format   = VK_FORMAT_R32G32B32_SFLOAT;
			attributes[attributeCount].offset   = offsetof(LitVertex, normal);
			attributeCount++;

			for (uint32_t light = 0; light < LIT_LIGHT_COUNT; light++)
			{
				attributes[attributeCount].location = 13 + light;
				attributes[attributeCount].binding  = 0;
				attributes[attributeCount].format   = VK_FORMAT_R32G32B32_SFLOAT;
				attributes[attributeCount].offset   = static_cast<uint32_t>(offsetof(LitVertex, toLight) + light * sizeof(LitVertex::toLight[0]));
				attributeCount++;

				attributes[attributeCount].location = 13 + LIT_LIGHT_COUNT + light;
				attributes[attributeCount].binding  = 0;
				attributes[attributeCount].format   = VK_FORMAT_R8G8B8A8_UNORM;
				attributes[attributeCount].offset   = static_cast<uint32_t>(offsetof(LitVertex, lightColour) + light * sizeof(LitVertex::lightColour[0]));
				attributeCount++;
			}
		}

		VkPipelineVertexInputStateCreateInfo vertexInput{ VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO };
		vertexInput.vertexBindingDescriptionCount   = _countof(bindings);
		vertexInput.pVertexBindingDescriptions      = bindings;
		vertexInput.vertexAttributeDescriptionCount = attributeCount;
		vertexInput.pVertexAttributeDescriptions    = attributes;

		// Describe assembly, viewport and rasterisation
		//
		// Back faces are culled when the game asks, the way OpenGL's defaults cull them:
		// the game's own OpenGL driver never calls glCullFace or glFrontFace, and its
		// Direct3D driver culls faces that wind clockwise on screen. The clip space
		// correction flips y and Vulkan's framebuffer y points down, so a face winding
		// counter-clockwise on screen is front-facing here as it is under OpenGL.
		//
		// The depth bias is always on and set per draw from the polygon offset, which is
		// zero for nearly every draw.
		VkPipelineInputAssemblyStateCreateInfo assembly{ VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO };
		assembly.topology = key.topology;

		VkPipelineViewportStateCreateInfo viewport{ VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO };
		viewport.viewportCount = 1;
		viewport.scissorCount  = 1;

		VkPipelineRasterizationStateCreateInfo rasterisation{ VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO };
		rasterisation.polygonMode     = VK_POLYGON_MODE_FILL;
		rasterisation.cullMode        = key.isFaceCullingEnabled ? VK_CULL_MODE_BACK_BIT : VK_CULL_MODE_NONE;
		rasterisation.frontFace       = VK_FRONT_FACE_COUNTER_CLOCKWISE;
		rasterisation.depthBiasEnable = VK_TRUE;
		rasterisation.lineWidth       = 1.0f;

		VkPipelineMultisampleStateCreateInfo multisample{ VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO };
		multisample.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT;

		// Describe depth, stencil and blending from the key
		VkPipelineDepthStencilStateCreateInfo depthStencil{ VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO };
		depthStencil.depthTestEnable  = key.isDepthTestEnabled ? VK_TRUE : VK_FALSE;
		depthStencil.depthWriteEnable = key.isDepthWriteEnabled ? VK_TRUE : VK_FALSE;
		depthStencil.depthCompareOp   = MapCompareOperation(key.depthComparison);
		depthStencil.minDepthBounds   = 0.0f;
		depthStencil.maxDepthBounds   = 1.0f;

		depthStencil.stencilTestEnable = (key.isStencilTestEnabled && hasStencil) ? VK_TRUE : VK_FALSE;
		depthStencil.front.failOp      = MapStencilOperation(key.stencilFailOperation);
		depthStencil.front.depthFailOp = MapStencilOperation(key.stencilDepthFailOperation);
		depthStencil.front.passOp      = MapStencilOperation(key.stencilPassOperation);
		depthStencil.front.compareOp   = MapCompareOperation(key.stencilComparison);
		depthStencil.back              = depthStencil.front;

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

		// Leave the viewport, scissor, depth bias and stencil values to each draw
		VkDynamicState dynamicStates[] = {
			VK_DYNAMIC_STATE_VIEWPORT,
			VK_DYNAMIC_STATE_SCISSOR,
			VK_DYNAMIC_STATE_DEPTH_BIAS,
			VK_DYNAMIC_STATE_STENCIL_COMPARE_MASK,
			VK_DYNAMIC_STATE_STENCIL_WRITE_MASK,
			VK_DYNAMIC_STATE_STENCIL_REFERENCE,
		};

		VkPipelineDynamicStateCreateInfo dynamic{ VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO };
		dynamic.dynamicStateCount = _countof(dynamicStates);
		dynamic.pDynamicStates    = dynamicStates;

		// Create the pipeline, through the cache that outlives the session
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
		VkResult const result = vkCreateGraphicsPipelines(device, pipelineCache, 1, &information, nullptr, &pipeline);
		if (result != VK_SUCCESS)
		{
			Fail("vkCreateGraphicsPipelines", result);
			return VK_NULL_HANDLE;
		}

		unsavedPipelines++;
		LogDebug("Vulkan: created pipeline %zu for format 0x%x (stride %u, colour %d, texcoord sets %u), topology %d, blend %d (%u,%u), stencil %d, flat %d.", pipelines.size() + 1, key.format, layout.stride, layout.hasColour ? 1 : 0, layout.textureCoordinateSets, key.topology, key.isBlendEnabled ? 1 : 0, key.sourceFactor, key.destinationFactor, key.isStencilTestEnabled ? 1 : 0, key.isFlatShaded ? 1 : 0);
		return pipeline;
	}

	void VulkanBackend::DestroyPipelines(void)
	{
		for (auto const& entry : pipelines)
		{
			if (entry.second != VK_NULL_HANDLE)
			{
				vkDestroyPipeline(device, entry.second, nullptr);
			}
		}

		pipelines.clear();
		lastPipeline    = VK_NULL_HANDLE;
		lastPipelineKey = PipelineKey{};
	}

	VulkanBackend::VertexLayout VulkanBackend::DecodeVertexLayout(uint32_t gdVertexFormat)
	{
		// Decoded with the game's own packed-format helpers rather than a hand-written
		// table. The formats disagree about which attributes are present and where, and
		// the packed encoding is the authority.
		VertexLayout layout;

		// The lit vertex, which is scvk's own
		if (gdVertexFormat == LIT_VERTEX_FORMAT)
		{
			layout.stride                     = sizeof(LitVertex);
			layout.hasColour                  = true;
			layout.colourOffset               = offsetof(LitVertex, colour);
			layout.textureCoordinateSets      = 2;
			layout.textureCoordinateOffset[0] = offsetof(LitVertex, coordinates[0]);
			layout.textureCoordinateOffset[1] = offsetof(LitVertex, coordinates[1]);
			return layout;
		}

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
			LogWarn("Vulkan: primitive type %u is not handled; skipping the draw.", gdPrimitiveType);
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

		// Round up to the alignment
		//
		// A power of two, except for vertices, which are aligned to their stride. Blocks
		// are far smaller than four gigabytes, so the division works in 32 bits.
		VkDeviceSize aligned;

		if ((alignment & (alignment - 1u)) == 0)
		{
			aligned = (arena.usedBytes + alignment - 1u) & ~(alignment - 1u);
		}
		else
		{
			uint32_t const used = static_cast<uint32_t>(arena.usedBytes);
			uint32_t const step = static_cast<uint32_t>(alignment);
			aligned = VkDeviceSize{ (used + step - 1u) / step * step };
		}

		// Move on through the ring when this block is full
		if (aligned + bytes > arena.blockSize)
		{
			size_t const next = (arena.currentBlock + 1) % arena.blocks.size();
			uint64_t const nextFrame = arena.blocks[next].lastFrame;
			bool const isNextThisFrame = arena.blocks.size() == 1 || nextFrame == frameSerial;
			bool const isNextInFlight  = nextFrame > completedSerial;

			if (isNextThisFrame || isNextInFlight)
			{
				// Add a block after this one while the arena may grow, which costs less than
				// waiting for the GPU. Blocks stay once added, so a heavy frame pays for the
				// allocation once rather than every time it recurs.
				if (arena.blocks.size() < arena.maximumBlocks)
				{
					ArenaBlock block;
					if (!CreateHostBuffer(arena.blockSize, arena.usage, block.buffer, block.memory, block.mapped))
					{
						return false;
					}

					arena.blocks.insert(arena.blocks.begin() + static_cast<ptrdiff_t>(arena.currentBlock + 1), block);
					arena.currentBlock++;
					LogDebug("Vulkan: the %s arena grew to %zu blocks of %llu MB.", arena.name, arena.blocks.size(), arena.blockSize >> 20);
				}
				else if (isNextThisFrame)
				{
					// The frame alone fills the ring; the caller submits it part way
					return false;
				}
				else
				{
					// Wait for the frame still reading the next block
					if (!WaitForFrame(nextFrame))
					{
						return false;
					}

					arenaWaits++;
					arena.currentBlock = next;
				}
			}
			else
			{
				arena.currentBlock = next;
			}

			aligned = 0;
		}

		// Hand out the space
		//
		// The mapped memory is untyped bytes.
		ArenaBlock& block = arena.blocks[arena.currentBlock];
		block.lastFrame = frameSerial;

		outBuffer       = block.buffer;
		outOffset       = aligned;
		outAddress      = static_cast<uint8_t*>(block.mapped) + aligned;
		arena.usedBytes = aligned + bytes;
		return true;
	}

	bool VulkanBackend::ArenaHasRoom(Arena const& arena, VkDeviceSize bytes, VkDeviceSize alignment) const
	{
		if (arena.blocks.empty() || bytes > arena.blockSize)
		{
			return false;
		}

		// The rest of this block, a block the arena may still add, or the next one, which
		// is either free or waited for, unless this frame wrote it
		VkDeviceSize const aligned = (arena.usedBytes + alignment - 1u) & ~(alignment - 1u);

		if (aligned + bytes <= arena.blockSize || arena.blocks.size() < arena.maximumBlocks)
		{
			return true;
		}

		size_t const next = (arena.currentBlock + 1) % arena.blocks.size();
		return arena.blocks.size() > 1 && arena.blocks[next].lastFrame != frameSerial;
	}

	void VulkanBackend::ArenaRewind(Arena& arena)
	{
		arena.currentBlock = 0;
		arena.usedBytes    = 0;

		for (ArenaBlock& block : arena.blocks)
		{
			block.lastFrame = 0;
		}
	}

	void VulkanBackend::DestroyArena(Arena& arena)
	{
		for (ArenaBlock& block : arena.blocks)
		{
			if (block.mapped != nullptr)        { vkUnmapMemory(device, block.memory); }
			FreeDeviceMemory(block.memory);
			if (block.buffer != VK_NULL_HANDLE) { vkDestroyBuffer(device, block.buffer, nullptr); }
		}

		arena.blocks.clear();
		ArenaRewind(arena);
	}

	bool VulkanBackend::ReserveDrawSpace(VkDeviceSize vertexBytes, VkDeviceSize indexBytes, uint32_t vertexStride)
	{
		// Count the draw record in with the vertices
		//
		// It comes from the same arena when the draw has to write a fresh copy, with
		// alignment padding before it, as the vertices have before them.
		VkDeviceSize const vertexArenaBytes = vertexBytes + vertexStride + VERTEX_ALIGNMENT + sizeof(DrawRecord);

		bool const hasVertexRoom = ArenaHasRoom(vertexArena, vertexArenaBytes, VERTEX_ALIGNMENT);
		bool const hasIndexRoom  = indexBytes == 0 || ArenaHasRoom(indexArena, indexBytes, sizeof(uint32_t));

		if (hasVertexRoom && hasIndexRoom)
		{
			return true;
		}

		// Submit the frame so far and reuse the arenas
		//
		// Done before the draw takes any space, so nothing it writes is rewound under it.
		// A full redraw of a large city at the widest zoom can fill every block. Dropping
		// the rest of the frame lost whatever the game draws last, the sea above all, and
		// the game then saved that scene and kept restoring it without the water.
		if (!SubmitFrameSoFar())
		{
			return false;
		}

		LogDebug("Vulkan: the per-frame geometry filled every block; submitted frame %llu part way to reuse it.", frameSerial);
		return true;
	}

	bool VulkanBackend::UploadVertices(void const* vertices, uint32_t firstVertex, uint32_t vertexCount, VertexLayout const& layout, VkBuffer& outBuffer, uint32_t& outBaseVertex)
	{
		// Reserve the space
		//
		// Measured in 64 bits, so a range the game's indices make absurdly large is
		// refused by the arena rather than wrapping.
		//
		// Aligned to the stride, so the copy starts on a whole vertex of the block. The
		// block is then bound once and each draw names the vertex it starts at, where
		// binding it again at every draw's own offset cost a call per draw, and a city
		// redraw at the widest zoom makes over a hundred thousand. Strides are whole
		// numbers of floats, so every attribute stays aligned to its components.
		VkDeviceSize const bytes = VkDeviceSize{ vertexCount } * layout.stride;
		VkDeviceSize offset      = 0;
		uint8_t* destination = nullptr;

		if (!ArenaAllocate(vertexArena, bytes, layout.stride, outBuffer, offset, destination))
		{
			LogWarn("Vulkan: no room for per-frame vertex data; dropping a draw of %u vertices.", vertexCount);
			return false;
		}

		// A block is far smaller than four gigabytes, so the offset fits 32 bits.
		outBaseVertex = static_cast<uint32_t>(offset) / layout.stride;

		// Copy the vertices
		//
		// The game's vertices are untyped bytes, and the arena accepted the size, so it
		// fits within one block and within a size_t. They go in as they are: the vertex
		// stage works out each stage's coordinates from the draw record.
		uint8_t const* const source = static_cast<uint8_t const*>(vertices) + size_t{ firstVertex } * layout.stride;

		PhaseScope const copying(*this, FRAME_PHASE_VERTEX_COPIES);
		memcpy(destination, source, static_cast<size_t>(bytes));

		// Count the copy for the heartbeat
		vertexUploads++;
		vertexUploadBytes      += bytes;
		frameVertexBytes       += bytes;
		largestFrameVertexBytes = std::max(largestFrameVertexBytes, frameVertexBytes);

		return true;
	}

	void VulkanBackend::LogVertexTraffic(void)
	{
		// The time they took is on the heartbeat's phase line.
		//
		// Byte totals stay far below the range where a double loses whole megabytes.
		double const megabyte = 1024.0 * 1024.0;
		LogDebug("Vulkan: vertices since the last heartbeat %u copies (%.1f MB), largest frame %.1f MB; %zu vertex blocks, %u waits for the GPU, %u partial submits; %zu pipelines.", vertexUploads, static_cast<double>(vertexUploadBytes) / megabyte, static_cast<double>(largestFrameVertexBytes) / megabyte, vertexArena.blocks.size(), arenaWaits, partialSubmits, pipelines.size());

		vertexUploads           = 0;
		vertexUploadBytes       = 0;
		largestFrameVertexBytes = 0;
		arenaWaits              = 0;
		partialSubmits          = 0;
	}

	bool VulkanBackend::IsTwoStageDraw(uint32_t textureCoordinateSets) const
	{
		// The second stage runs only when it is switched on, has a texture, and has a
		// coordinate to sample with: a set of its own in the geometry, one generated from
		// the position, or the first set when the stage asks for it. All three matter: the
		// game leaves a 4x4 placeholder bound to the stage for the whole session and turns
		// the stage itself off, so taking the binding as the signal modulates the city
		// terrain down to black.
		//
		// Buildings carry one set and light their windows at night by laying a second
		// texture over the first through that same set, as a decal. Requiring a set of its
		// own skipped the stage, and no window ever lit.
		bool const hasSharedSet  = stageCoordinates[1].isGenerated || stageCoordinates[1].sourceSet == 0;
		bool const hasCoordinate = textureCoordinateSets >= 2 || (textureCoordinateSets == 1 && hasSharedSet);

		return hasCoordinate && currentTexture1 != 0 && isStageEnabled[1];
	}

	bool VulkanBackend::IsCombinerDraw(uint32_t textureCoordinateSets) const
	{
		// The first stage runs its network alone when the second stage is off
		//
		// As Direct3D's first stage applies its operation whatever the second does. The
		// game draws its building shadows that way when the graphics rules turn the second
		// stage off: the network takes the shadow colour from the environment colour and
		// only the alpha from the mask, so treating it as modulate drew them white.
		bool const isFirstStageAlone = isFirstStageCombining && isStageEnabled[0];

		return IsTwoStageDraw(textureCoordinateSets) || isFirstStageAlone;
	}

	bool VulkanBackend::BindDrawState(uint32_t gdVertexFormat, VkPrimitiveTopology topology, VkBuffer vertexBuffer, VkDeviceSize vertexOffset, VertexLayout const& layout)
	{
		// Find the pipeline for the current state
		PipelineKey const key{ gdVertexFormat, topology, isBlendEnabled, blendSourceFactor, blendDestinationFactor, isDepthTestEnabled, isDepthWriteEnabled, depthComparison, isColourWriteEnabled, isFaceCullingEnabled, isStencilTestEnabled && hasStencil, stencilComparison, stencilFailOperation, stencilDepthFailOperation, stencilPassOperation, isFlatShaded };
		VkPipeline const pipeline = GetPipeline(key);
		if (pipeline == VK_NULL_HANDLE)
		{
			return false;
		}

		// Find the draw record's copy
		VkBuffer     recordBuffer = VK_NULL_HANDLE;
		VkDeviceSize recordOffset = 0;
		if (!GetDrawRecordCopy(recordBuffer, recordOffset))
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

		// Decide which of the fragment stage's paths the draw takes
		bool const isTwoStage  = IsTwoStageDraw(layout.textureCoordinateSets);
		bool const isCombining = IsCombinerDraw(layout.textureCoordinateSets);

		fragmentState[3] = isCombining ? 2.0f : 1.0f;

		// Bind what changed
		if (bound.pipeline != pipeline)
		{
			vkCmdBindPipeline(commandBuffer, VK_PIPELINE_BIND_POINT_GRAPHICS, pipeline);
			bound.pipeline = pipeline;
		}

		ApplyDynamicState();
		PushDrawConstants(isTwoStage, isCombining);
		BindTextures(isTwoStage);

		if (bound.vertexBuffer != vertexBuffer || bound.vertexOffset != vertexOffset)
		{
			vkCmdBindVertexBuffers(commandBuffer, 0, 1, &vertexBuffer, &vertexOffset);
			bound.vertexBuffer = vertexBuffer;
			bound.vertexOffset = vertexOffset;
		}

		if (bound.recordBuffer != recordBuffer || bound.recordOffset != recordOffset)
		{
			vkCmdBindVertexBuffers(commandBuffer, 1, 1, &recordBuffer, &recordOffset);
			bound.recordBuffer = recordBuffer;
			bound.recordOffset = recordOffset;
		}

		return true;
	}

	void VulkanBackend::BindDescriptorSets(VkPipelineLayout layout, VkDescriptorSet const sets[4])
	{
		// A change of layout disturbs every set, so all four go again
		if (bound.layout != layout)
		{
			vkCmdBindDescriptorSets(commandBuffer, VK_PIPELINE_BIND_POINT_GRAPHICS, layout, 0, 4, sets, 0, nullptr);
			memcpy(bound.sets, sets, sizeof(bound.sets));
			bound.layout = layout;
			return;
		}

		// Otherwise bind the run from the first set that changed to the last
		uint32_t first = 4;
		uint32_t last  = 0;

		for (uint32_t index = 0; index < 4; index++)
		{
			if (bound.sets[index] != sets[index])
			{
				first = std::min(first, index);
				last  = index;
			}
		}

		if (first > last)
		{
			return;
		}

		vkCmdBindDescriptorSets(commandBuffer, VK_PIPELINE_BIND_POINT_GRAPHICS, layout, first, last - first + 1, sets + first, 0, nullptr);
		memcpy(bound.sets, sets, sizeof(bound.sets));
	}

	void VulkanBackend::ApplyDynamicState(void)
	{
		// The polygon offset, in units of the depth buffer's resolution, as Direct3D's
		// depth bias takes it
		if (bound.depthBias != polygonOffset)
		{
			vkCmdSetDepthBias(commandBuffer, static_cast<float>(polygonOffset), 0.0f, 0.0f);
			bound.depthBias = polygonOffset;
		}

		// The stencil values, kept even while the test is off since every pipeline names
		// them as dynamic state
		if (!bound.hasStencilState || bound.stencilReference != stencilReference || bound.stencilCompareMask != stencilReadMask || bound.stencilWriteMask != stencilWriteMask)
		{
			vkCmdSetStencilReference(commandBuffer, VK_STENCIL_FACE_FRONT_AND_BACK, stencilReference);
			vkCmdSetStencilCompareMask(commandBuffer, VK_STENCIL_FACE_FRONT_AND_BACK, stencilReadMask);
			vkCmdSetStencilWriteMask(commandBuffer, VK_STENCIL_FACE_FRONT_AND_BACK, stencilWriteMask);

			bound.stencilReference   = stencilReference;
			bound.stencilCompareMask = stencilReadMask;
			bound.stencilWriteMask   = stencilWriteMask;
			bound.hasStencilState    = true;
		}
	}

	void VulkanBackend::PushDrawConstants(bool isTwoStage, bool isCombining)
	{
		// Copy the state this draw sends
		//
		// Copies, because the game's own settings must survive for the next draw that has
		// the first stage on.
		PushConstantBlock block;
		memcpy(block.transform, transform, sizeof(block.transform));
		memcpy(block.fragmentState, fragmentState, sizeof(block.fragmentState));
		memcpy(block.combinerState, combinerState, sizeof(block.combinerState));
		memcpy(block.constantColour, constantColour, sizeof(block.constantColour));
		memcpy(block.sceneTint, sceneTint, sizeof(block.sceneTint));

		// Pack the environment mode with the lighting sources in higher bits
		//
		// The push constant block is full at the guaranteed 128 bytes and all three are
		// small enough to share one slot. The shader reads its state as floats, and small
		// integers convert exactly.
		uint32_t const lightingSources = (isAlphaFromVertex ? ALPHA_FROM_VERTEX_FLAG : 0u) | (isColourFromVertex ? COLOUR_FROM_VERTEX_FLAG : 0u);
		block.fragmentState[2] = static_cast<float>(textureEnvironmentMode | lightingSources);

		// Pass the primary colour through a disabled first stage
		//
		// A stage with texturing off passes the primary colour through untouched,
		// whatever its environment mode or combiner says. The white texture bound in its
		// place does that under modulate, so the stage is pushed as modulate rather than
		// as the game left it: replace or decal would otherwise turn it white, and a
		// combiner could turn it anything.
		if (!isStageEnabled[0])
		{
			block.fragmentState[2] = static_cast<float>(ENVIRONMENT_MODULATE | lightingSources);
			block.combinerState[0] = MODULATE_WITH_PREVIOUS;
			block.combinerState[1] = MODULATE_WITH_PREVIOUS;
		}

		// Hand the first stage's result through an idle second stage
		//
		// The shader always runs both stages of the network, and the second still holds
		// whatever the game last gave it.
		if (isCombining && !isTwoStage)
		{
			block.combinerState[2] = PASS_PREVIOUS;
			block.combinerState[3] = PASS_PREVIOUS;
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

			block.fragmentState[3] = 10.0f + static_cast<float>(pass);
		}

		if (debugChannel >= 0)
		{
			block.fragmentState[3] = 20.0f + static_cast<float>(debugChannel);
		}

		// Push the whole block, unless the last draw pushed the same
		//
		// In one call rather than one per part. A redraw of the city at the widest zoom
		// records over a hundred thousand draws, so every call a draw makes counts, and
		// consecutive draws of one mesh repeat their constants.
		if (bound.hasPushConstants && memcmp(&bound.pushConstants, &block, sizeof(block)) == 0)
		{
			return;
		}

		vkCmdPushConstants(commandBuffer, pipelineLayout, PUSH_CONSTANT_STAGES, 0, sizeof(block), &block);
		bound.pushConstants    = block;
		bound.hasPushConstants = true;
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
		uint32_t const bound0 = (isStageEnabled[0] && currentTexture < textures.size() && textures[currentTexture].isLive) ? currentTexture : 0;
		uint32_t const bound1 = (isTwoStage && currentTexture1 < textures.size() && textures[currentTexture1].isLive) ? currentTexture1 : 0;

		NoteTextureUse(bound0);
		NoteTextureUse(bound1);

		// Bind their sets
		//
		// Each stage has its own sampler, made from that stage's parameters. Sharing the
		// first stage's sampler gave the building shadows' small clamped mask the repeat
		// of the texture they project, so the mask tiled and every shadow repeated along
		// the streets.
		VkDescriptorSet const sets[] = {
			textures[bound0].descriptor,
			textures[bound1].descriptor,
			GetSamplerSet(stageParameters[0]),
			GetSamplerSet(stageParameters[1]),
		};

		BindDescriptorSets(pipelineLayout, sets);
	}

	void VulkanBackend::BindIndexBuffer(VkBuffer buffer)
	{
		if (bound.indexBuffer == buffer)
		{
			return;
		}

		vkCmdBindIndexBuffer(commandBuffer, buffer, 0, VK_INDEX_TYPE_UINT32);
		bound.indexBuffer = buffer;
	}

	void VulkanBackend::UpdateDrawRecord(DrawRecord const& record)
	{
		if (memcmp(&record, &drawRecord, sizeof(DrawRecord)) == 0)
		{
			return;
		}

		drawRecord       = record;
		drawRecordBuffer = VK_NULL_HANDLE;
	}

	bool VulkanBackend::GetDrawRecordCopy(VkBuffer& outBuffer, VkDeviceSize& outOffset)
	{
		// Write a copy when there is none
		//
		// Once per change rather than once per draw. The fog changes with the view and
		// never while it is off, and the stages' coordinates change between passes, not
		// within one.
		if (drawRecordBuffer == VK_NULL_HANDLE)
		{
			uint8_t* destination = nullptr;

			if (!ArenaAllocate(vertexArena, sizeof(DrawRecord), VERTEX_ALIGNMENT, drawRecordBuffer, drawRecordOffset, destination))
			{
				drawRecordBuffer = VK_NULL_HANDLE;
				LogWarn("Vulkan: no room for per-frame vertex data; dropping a draw for want of its draw record.");
				return false;
			}

			memcpy(destination, &drawRecord, sizeof(DrawRecord));
		}

		outBuffer = drawRecordBuffer;
		outOffset = drawRecordOffset;
		return true;
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
		// The game's own order: 0 replace, 1 modulate, 2 decal, 3 blend. Anything else
		// falls back to modulate, which is the fixed function default, except that the
		// combine modes are noted so the draw can run the network instead.
		textureEnvironmentMode = (mode <= ENVIRONMENT_BLEND) ? mode : ENVIRONMENT_MODULATE;
		isFirstStageCombining  = (mode == ENVIRONMENT_COMBINE || mode == ENVIRONMENT_COMBINE4);
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

	void VulkanBackend::SetStencilState(bool isTestEnabled, uint32_t comparison, uint32_t reference, uint32_t readMask, uint32_t writeMask, uint32_t failOperation, uint32_t depthFailOperation, uint32_t passOperation)
	{
		// The masks and reference are eight bits deep, as the stencil buffer is
		isStencilTestEnabled      = isTestEnabled;
		stencilComparison         = comparison;
		stencilReference          = reference & 0xffu;
		stencilReadMask           = readMask & 0xffu;
		stencilWriteMask          = writeMask & 0xffu;
		stencilFailOperation      = failOperation;
		stencilDepthFailOperation = depthFailOperation;
		stencilPassOperation      = passOperation;
	}

	void VulkanBackend::SetPolygonOffset(int32_t offset)
	{
		polygonOffset = offset;
	}

	void VulkanBackend::SetFlatShading(bool isEnabled)
	{
		isFlatShaded = isEnabled;
	}

	void VulkanBackend::SetFaceCulling(bool isEnabled)
	{
		isFaceCullingEnabled = isEnabled;
	}

	void VulkanBackend::SetSceneTint(float red, float green, float blue, float alpha, bool isColourFromVertexColour, bool isAlphaFromVertexColour)
	{
		sceneTint[0] = red;
		sceneTint[1] = green;
		sceneTint[2] = blue;
		sceneTint[3] = alpha;

		isColourFromVertex = isColourFromVertexColour;
		isAlphaFromVertex  = isAlphaFromVertexColour;
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

	void VulkanBackend::SetStageCoordinates(uint32_t stage, bool isGenerated, uint32_t sourceSet, float const* rowS, float const* rowT)
	{
		if (stage > 1 || rowS == nullptr || rowT == nullptr)
		{
			return;
		}

		StageCoordinates& coordinates = stageCoordinates[stage];
		coordinates.isGenerated = isGenerated;
		coordinates.sourceSet   = sourceSet;
		memcpy(coordinates.rows, rowS, sizeof(float) * 4);
		memcpy(coordinates.rows + 4, rowT, sizeof(float) * 4);

		// Hand them to the vertex stage
		//
		// A set past the second falls back to the first, as it does for a format that
		// lacks the set named.
		DrawRecord record = drawRecord;
		memcpy(record.stageRows + stage * STAGE_ROW_FLOATS, coordinates.rows, sizeof(coordinates.rows));
		record.stageSources[stage] = isGenerated ? STAGE_SOURCE_POSITION : ((sourceSet == 1) ? STAGE_SOURCE_SECOND_SET : STAGE_SOURCE_FIRST_SET);
		UpdateDrawRecord(record);
	}

	void VulkanBackend::SetFog(bool isEnabled, uint32_t gdMode, float density, float start, float end, float const* colour)
	{
		if (colour == nullptr)
		{
			return;
		}

		DrawRecord record = drawRecord;

		// Number the mode the way the vertex stage reads it
		//
		// One higher than the game's, leaving zero for off. The game's modes run 0 to 2,
		// and the small values convert to floats exactly.
		isFogEnabled         = isEnabled && gdMode <= GD_FOG_MODE_LINEAR;
		record.fogParameters[0] = isFogEnabled ? static_cast<float>(gdMode + 1u) : FOG_MODE_OFF;
		record.fogParameters[1] = density;

		// Reduce the linear equation to a scale and an offset
		//
		// f = (end - distance) / (end - start). OpenGL leaves equal start and end
		// undefined, and they are taken as no fog here rather than as a division by zero.
		float const range = end - start;
		record.fogParameters[2] = (range != 0.0f) ? -1.0f / range : 0.0f;
		record.fogParameters[3] = (range != 0.0f) ? end / range : 1.0f;

		memcpy(record.fogColour, colour, sizeof(record.fogColour));
		UpdateDrawRecord(record);
	}

	void VulkanBackend::SetFogDistanceRow(float const* row)
	{
		// Leave the record alone while the fog is off
		//
		// The row changes with nearly every draw of a moving scene, and a record nothing
		// reads would only be copied again each time.
		if (!isFogEnabled || row == nullptr)
		{
			return;
		}

		DrawRecord record = drawRecord;
		memcpy(record.fogDistanceRow, row, sizeof(record.fogDistanceRow));
		UpdateDrawRecord(record);
	}

	void VulkanBackend::SetDebugPassColours(bool isEnabled)
	{
		shouldShowPassColours = isEnabled;
		LogInfo("Vulkan: pass identification colours are %s.", isEnabled ? "on" : "off");
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

		PhaseScope const recording(*this, FRAME_PHASE_RECORDING);

		// Translate the primitive and start the frame
		VkPrimitiveTopology topology;
		bool isQuadList = false;

		if (!MapTopology(gdPrimitiveType, topology, isQuadList) || !EnsureFrame())
		{
			return;
		}

		if (!ReserveDrawSpace(VkDeviceSize{ vertexCount } * layout.stride, 0, layout.stride))
		{
			return;
		}

		// Copy the vertices and bind the state
		VkBuffer vertexBuffer = VK_NULL_HANDLE;
		uint32_t baseVertex   = 0;
		if (!UploadVertices(vertices, firstVertex, vertexCount, layout, vertexBuffer, baseVertex))
		{
			return;
		}

		if (!BindDrawState(gdVertexFormat, topology, vertexBuffer, 0, layout))
		{
			return;
		}

		// Draw plain primitives directly
		if (!isQuadList)
		{
			vkCmdDraw(commandBuffer, vertexCount, 1, baseVertex, 0);
			return;
		}

		// Draw quads through the shared quad indices
		uint32_t quads = vertexCount / 4u;
		if (quads > quadCapacity)
		{
			LogWarn("Vulkan: %u quads exceeds the index buffer capacity of %u; clamping.", quads, quadCapacity);
			quads = quadCapacity;
		}

		if (quads == 0)
		{
			return;
		}

		// The base vertex is below the block's size in vertices, far from INT32_MAX.
		BindIndexBuffer(quadIndexBuffer);
		vkCmdDrawIndexed(commandBuffer, quads * 6u, 1, 0, static_cast<int32_t>(baseVertex), 0);
	}

	void VulkanBackend::DrawIndexedVertices(uint32_t gdPrimitiveType, uint32_t gdVertexFormat, void const* vertices, void const* indices, uint32_t indexCount, bool isIndex32Bit)
	{
		VertexLayout const layout = DecodeVertexLayout(gdVertexFormat);

		if (vertices == nullptr || indices == nullptr || indexCount == 0 || layout.stride == 0)
		{
			return;
		}

		PhaseScope const recording(*this, FRAME_PHASE_RECORDING);

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

		if (isIndex32Bit)
		{
			for (uint32_t i = 0; i < indexCount; i++)
			{
				lowest  = std::min(lowest, wideIndices[i]);
				highest = std::max(highest, wideIndices[i]);
			}
		}
		else
		{
			for (uint32_t i = 0; i < indexCount; i++)
			{
				lowest  = std::min(lowest, uint32_t{ narrowIndices[i] });
				highest = std::max(highest, uint32_t{ narrowIndices[i] });
			}
		}

		// Count the indices, expanded when they describe quads
		//
		// Quads are expanded here rather than being drawn through the shared quad index
		// buffer, because that buffer describes consecutive vertices and these do not
		// have to be consecutive.
		uint32_t const vertexCount = highest - lowest + 1u;
		uint32_t const emitted     = isQuadList ? (indexCount / 4u) * 6u : indexCount;

		if (emitted == 0)
		{
			return;
		}

		size_t const indexBytes = size_t{ emitted } * sizeof(uint32_t);

		if (!ReserveDrawSpace(VkDeviceSize{ vertexCount } * layout.stride, indexBytes, layout.stride))
		{
			return;
		}

		// Copy that range of vertices
		VkBuffer vertexBuffer = VK_NULL_HANDLE;
		uint32_t baseVertex   = 0;
		if (!UploadVertices(vertices, lowest, vertexCount, layout, vertexBuffer, baseVertex))
		{
			return;
		}

		// Reserve space for the indices
		VkBuffer     indexBuffer = VK_NULL_HANDLE;
		VkDeviceSize indexOffset = 0;
		uint8_t*     indexDestination = nullptr;

		if (!ArenaAllocate(indexArena, indexBytes, sizeof(uint32_t), indexBuffer, indexOffset, indexDestination))
		{
			LogWarn("Vulkan: no room for per-frame index data; dropping a draw of %u indices.", indexCount);
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
		if (!BindDrawState(gdVertexFormat, topology, vertexBuffer, 0, layout))
		{
			return;
		}

		// Bind the block once and address the indices through the first index
		//
		// The offset is a multiple of four, which the arena's alignment guarantees, and a
		// block is far smaller than four billion indices.
		BindIndexBuffer(indexBuffer);

		// Offset the indices to where the copied slice sits in the block
		//
		// The indices went in unchanged, so they still count from the start of the game's
		// array, while the block holds the slice from the lowest one onward, starting at
		// the base vertex. The vertex offset closes that gap, negative when the slice sits
		// nearer the start than its lowest index, which is what the parameter is signed
		// for. Real indices and base vertices are far below INT32_MAX, so the conversions
		// lose nothing.
		int32_t const vertexOffset = static_cast<int32_t>(baseVertex) - static_cast<int32_t>(lowest);
		vkCmdDrawIndexed(commandBuffer, emitted, 1, static_cast<uint32_t>(indexOffset / sizeof(uint32_t)), vertexOffset, 0);
	}
}
