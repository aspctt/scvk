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
 * The passes of scvk's own, beside the game's draws: the shadow map and its composite,
 * the scene depth handed to ReShade, and what ReShade and other plugins are given to draw
 * into the frame themselves.
 *
 * The shadow passes follow SCD3D11's live shadows: casters drawn into a 2048 square depth
 * map from the sun, with the occupant number of each texel's caster beside it, then a
 * full-screen pass that darkens what the scene depth puts in their shadow or below the
 * terrain's.
 */

//// Dependencies

#include "VulkanBackend.h"
#include "Logger.h"
#include "ShaderBinaries.h"

#include <algorithm>
#include <string.h>

namespace scvk
{
	//// Constants

	namespace
	{
		// The shadow map's size, SCD3D11's.
		constexpr uint32_t SHADOW_MAP_SIZE = 2048;

		// The occupant numbers run from 1 and fit 16 bits, 0 meaning none, as in SCD3D11.
		// Every device can draw into and sample this format.
		constexpr VkFormat SHADOW_OCCUPANT_FORMAT = VK_FORMAT_R16_UINT;

		// An occupant box as the composite reads it: the low corner, then the high, each
		// padded to four floats.
		constexpr VkDeviceSize OCCUPANT_BOX_BYTES = sizeof(float) * 8;

		// The descriptor set of the composite: its constants, six images, then the occupant
		// boxes, in the bindings composite.frag declares.
		constexpr uint32_t COMPOSITE_IMAGE_COUNT    = 6;
		constexpr uint32_t COMPOSITE_BINDING_COUNT  = COMPOSITE_IMAGE_COUNT + 2;
		constexpr uint32_t COMPOSITE_BOXES_BINDING  = COMPOSITE_IMAGE_COUNT + 1;

		// The depth buffer's steps, as DepthBufferSteps gives them.
		constexpr float SIXTEEN_BIT_DEPTH_STEPS      = 65536.0f;
		constexpr float TWENTY_FOUR_BIT_DEPTH_STEPS  = 16777216.0f;

		// The push constant block, as the shadow shaders declare it.
		struct ShadowPushConstants
		{
			float lightMatrix[16];
			float rowS[4];
			float rowT[4];
			float material[4];
			float uvBounds[4];
		};

		static_assert(sizeof(ShadowPushConstants) == sizeof(ShadowCasterDraw::constants), "the caster constants are the shadow shaders' push constant block");
		static_assert(sizeof(ShadowPushConstants) <= 128, "Vulkan only promises 128 bytes of push constants");
		static_assert(sizeof(ShadowCompositeConstants) == 288, "the composite's uniform block, laid out as std140 lays it out");

		// The descriptor set of the water reflections: their constants, then the scene depth
		// and the copy of the frame, in the bindings reflection.frag declares.
		constexpr uint32_t REFLECTION_BINDING_COUNT = 3;

		static_assert(sizeof(WaterReflectionConstants) == 112, "the reflections' uniform block, laid out as std140 lays it out");

	}

	//// Private Functions

	namespace
	{
		/** A shader module from SPIR-V words, or null. */
		VkShaderModule MakeModule(VkDevice device, uint32_t const* code, size_t bytes)
		{
			VkShaderModuleCreateInfo information{ VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO };
			information.codeSize = bytes;
			information.pCode    = code;

			VkShaderModule module = VK_NULL_HANDLE;
			if (vkCreateShaderModule(device, &information, nullptr, &module) != VK_SUCCESS)
			{
				return VK_NULL_HANDLE;
			}

			return module;
		}
	}

	VkPipeline VulkanBackend::CreateFullscreenPipeline(VkShaderModule vertexShader, VkShaderModule fragmentShader, VkPipelineLayout layout, VkRenderPass pass, VkPrimitiveTopology topology, EffectBlend blend)
	{
		VkPipelineShaderStageCreateInfo stages[2]{};
		stages[0].sType  = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
		stages[0].stage  = VK_SHADER_STAGE_VERTEX_BIT;
		stages[0].module = vertexShader;
		stages[0].pName  = "main";
		stages[1].sType  = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
		stages[1].stage  = VK_SHADER_STAGE_FRAGMENT_BIT;
		stages[1].module = fragmentShader;
		stages[1].pName  = "main";

		// No vertex input: the vertex stage makes its corners from the vertex index
		VkPipelineVertexInputStateCreateInfo vertexInput{ VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO };

		VkPipelineInputAssemblyStateCreateInfo inputAssembly{ VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO };
		inputAssembly.topology = topology;

		VkPipelineViewportStateCreateInfo viewportState{ VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO };
		viewportState.viewportCount = 1;
		viewportState.scissorCount  = 1;

		VkPipelineRasterizationStateCreateInfo rasterization{ VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO };
		rasterization.polygonMode = VK_POLYGON_MODE_FILL;
		rasterization.cullMode    = VK_CULL_MODE_NONE;
		rasterization.frontFace   = VK_FRONT_FACE_COUNTER_CLOCKWISE;
		rasterization.lineWidth   = 1.0f;

		VkPipelineMultisampleStateCreateInfo multisample{ VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO };
		multisample.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT;

		// Depth untouched, whether or not the pass has a depth attachment
		VkPipelineDepthStencilStateCreateInfo depthStencil{ VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO };
		depthStencil.depthCompareOp = VK_COMPARE_OP_ALWAYS;

		// Blend as the pass asks, always leaving the destination alpha alone when blending
		//
		// The shadow composite multiplies, as SCD3D11's composite blends: a shadow takes the
		// direct light away and keeps the ground's own colour, tinted by the game's shadow
		// colour. Blending towards that colour instead, as the composite once did, washed
		// large terrain shadows out in blue. The water reflections are laid over the water
		// by their alpha.
		VkPipelineColorBlendAttachmentState blendAttachment{};
		blendAttachment.colorWriteMask = VK_COLOR_COMPONENT_R_BIT | VK_COLOR_COMPONENT_G_BIT | VK_COLOR_COMPONENT_B_BIT | VK_COLOR_COMPONENT_A_BIT;

		if (blend != EffectBlend::Replace)
		{
			bool const isMultiply = blend == EffectBlend::Multiply;

			blendAttachment.blendEnable         = VK_TRUE;
			blendAttachment.srcColorBlendFactor = isMultiply ? VK_BLEND_FACTOR_ZERO : VK_BLEND_FACTOR_SRC_ALPHA;
			blendAttachment.dstColorBlendFactor = isMultiply ? VK_BLEND_FACTOR_SRC_COLOR : VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA;
			blendAttachment.colorBlendOp        = VK_BLEND_OP_ADD;
			blendAttachment.srcAlphaBlendFactor = VK_BLEND_FACTOR_ZERO;
			blendAttachment.dstAlphaBlendFactor = VK_BLEND_FACTOR_ONE;
			blendAttachment.alphaBlendOp        = VK_BLEND_OP_ADD;
		}

		VkPipelineColorBlendStateCreateInfo colourBlend{ VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO };
		colourBlend.attachmentCount = 1;
		colourBlend.pAttachments    = &blendAttachment;

		VkDynamicState const dynamicStates[] = { VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR };

		VkPipelineDynamicStateCreateInfo dynamicState{ VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO };
		dynamicState.dynamicStateCount = _countof(dynamicStates);
		dynamicState.pDynamicStates    = dynamicStates;

		VkGraphicsPipelineCreateInfo information{ VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO };
		information.stageCount          = _countof(stages);
		information.pStages             = stages;
		information.pVertexInputState   = &vertexInput;
		information.pInputAssemblyState = &inputAssembly;
		information.pViewportState      = &viewportState;
		information.pRasterizationState = &rasterization;
		information.pMultisampleState   = &multisample;
		information.pDepthStencilState  = &depthStencil;
		information.pColorBlendState    = &colourBlend;
		information.pDynamicState       = &dynamicState;
		information.layout              = layout;
		information.renderPass          = pass;

		VkPipeline pipeline = VK_NULL_HANDLE;
		VkResult const result = vkCreateGraphicsPipelines(device, pipelineCache, 1, &information, nullptr, &pipeline);
		if (result != VK_SUCCESS)
		{
			LogError("Vulkan: could not create an effect pipeline: %s", VkResultName(result));
			NoteDeviceLoss(result);
			return VK_NULL_HANDLE;
		}

		return pipeline;
	}

	bool VulkanBackend::CreateEffectResources(void)
	{
		// The shadow passes and the depth for ReShade are extras: without them the picture
		// is the game's own, so their failure is logged and the device carries on.
		hasShadowResources = CreateShadowResources();
		if (!hasShadowResources)
		{
			LogInfo("Vulkan: the shadow passes are unavailable on this device.");
		}

		if (!CreateSceneDepthResources())
		{
			LogInfo("Vulkan: the scene depth for ReShade is unavailable on this device.");
		}

		if (!CreateReflectionResources())
		{
			LogInfo("Vulkan: the water reflections are unavailable on this device.");
		}

		return true;
	}

	void VulkanBackend::DestroyEffectResources(void)
	{
		if (device == VK_NULL_HANDLE)
		{
			return;
		}

		// The shadows
		VkPipeline* const shadowPipelines[] = { &shadowCasterPipeline, &shadowRegistryPipeline, &shadowLinePipeline, &shadowPointPipeline, &compositePipeline, &sceneDepthPipeline, &reflectionPipeline };
		for (VkPipeline* pipeline : shadowPipelines)
		{
			if (*pipeline != VK_NULL_HANDLE) { vkDestroyPipeline(device, *pipeline, nullptr); *pipeline = VK_NULL_HANDLE; }
		}

		VkPipelineLayout* const layouts[] = { &shadowPipelineLayout, &compositePipelineLayout, &sceneDepthPipelineLayout, &reflectionPipelineLayout };
		for (VkPipelineLayout* layout : layouts)
		{
			if (*layout != VK_NULL_HANDLE) { vkDestroyPipelineLayout(device, *layout, nullptr); *layout = VK_NULL_HANDLE; }
		}

		VkDescriptorSetLayout* const setLayouts[] = { &compositeSetLayout, &sceneDepthSetLayout, &reflectionSetLayout };
		for (VkDescriptorSetLayout* layout : setLayouts)
		{
			if (*layout != VK_NULL_HANDLE) { vkDestroyDescriptorSetLayout(device, *layout, nullptr); *layout = VK_NULL_HANDLE; }
		}

		VkShaderModule* const modules[] = { &shadowVertexModule, &shadowFragmentModule, &fullscreenVertexModule, &compositeFragmentModule, &sceneDepthFragmentModule, &reflectionFragmentModule };
		for (VkShaderModule* module : modules)
		{
			if (*module != VK_NULL_HANDLE) { vkDestroyShaderModule(device, *module, nullptr); *module = VK_NULL_HANDLE; }
		}

		if (shadowFramebuffer != VK_NULL_HANDLE)     { vkDestroyFramebuffer(device, shadowFramebuffer, nullptr); shadowFramebuffer = VK_NULL_HANDLE; }
		if (sceneDepthFramebuffer != VK_NULL_HANDLE) { vkDestroyFramebuffer(device, sceneDepthFramebuffer, nullptr); sceneDepthFramebuffer = VK_NULL_HANDLE; }
		if (shadowPass != VK_NULL_HANDLE)            { vkDestroyRenderPass(device, shadowPass, nullptr); shadowPass = VK_NULL_HANDLE; }
		if (sceneDepthPass != VK_NULL_HANDLE)        { vkDestroyRenderPass(device, sceneDepthPass, nullptr); sceneDepthPass = VK_NULL_HANDLE; }

		DestroyRenderImage(shadowMap);
		DestroyRenderImage(shadowOccupantMap);
		DestroyRenderImage(terrainCeilingImage);
		DestroyRenderImage(terrainVertexImage);
		DestroyRenderImage(sceneDepthImage);
		DestroyRenderImage(reflectionSourceImage);

		hasShadowResources = false;
	}

	bool VulkanBackend::CreateShadowResources(void)
	{
		if (!isDepthSampleable || depthBuffer.secondView == VK_NULL_HANDLE || stencilView == VK_NULL_HANDLE)
		{
			return false;
		}

		// Pick the shadow map's format: 32-bit depth, drawn into and sampled
		VkFormatProperties properties{};
		vkGetPhysicalDeviceFormatProperties(physicalDevice, VK_FORMAT_D32_SFLOAT, &properties);

		VkFormatFeatureFlags const needed = VK_FORMAT_FEATURE_DEPTH_STENCIL_ATTACHMENT_BIT | VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT;
		if ((properties.optimalTilingFeatures & needed) != needed)
		{
			return false;
		}

		if (!CreateRenderImage(VK_FORMAT_D32_SFLOAT, SHADOW_MAP_SIZE, SHADOW_MAP_SIZE, VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT, VK_IMAGE_ASPECT_DEPTH_BIT, VK_FORMAT_UNDEFINED, 0, shadowMap))
		{
			return false;
		}

		// The occupant map beside it, drawn into and sampled as well
		VkFormatProperties occupantProperties{};
		vkGetPhysicalDeviceFormatProperties(physicalDevice, SHADOW_OCCUPANT_FORMAT, &occupantProperties);

		VkFormatFeatureFlags const occupantNeeded = VK_FORMAT_FEATURE_COLOR_ATTACHMENT_BIT | VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT;
		if ((occupantProperties.optimalTilingFeatures & occupantNeeded) != occupantNeeded)
		{
			return false;
		}

		if (!CreateRenderImage(SHADOW_OCCUPANT_FORMAT, SHADOW_MAP_SIZE, SHADOW_MAP_SIZE, VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT, VK_IMAGE_ASPECT_COLOR_BIT, VK_FORMAT_UNDEFINED, 0, shadowOccupantMap))
		{
			return false;
		}

		// The caster pass: the depth and the occupants, both cleared, then left ready to
		// sample. The occupants clear to 0, no occupant.
		VkAttachmentDescription attachments[2]{};
		attachments[0].format         = VK_FORMAT_D32_SFLOAT;
		attachments[0].samples        = VK_SAMPLE_COUNT_1_BIT;
		attachments[0].loadOp         = VK_ATTACHMENT_LOAD_OP_CLEAR;
		attachments[0].storeOp        = VK_ATTACHMENT_STORE_OP_STORE;
		attachments[0].stencilLoadOp  = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
		attachments[0].stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
		attachments[0].initialLayout  = VK_IMAGE_LAYOUT_UNDEFINED;
		attachments[0].finalLayout    = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
		attachments[1]                = attachments[0];
		attachments[1].format         = SHADOW_OCCUPANT_FORMAT;

		VkAttachmentReference depthReference{ 0, VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL };
		VkAttachmentReference occupantReference{ 1, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL };

		VkSubpassDescription subpass{};
		subpass.pipelineBindPoint       = VK_PIPELINE_BIND_POINT_GRAPHICS;
		subpass.colorAttachmentCount    = 1;
		subpass.pColorAttachments       = &occupantReference;
		subpass.pDepthStencilAttachment = &depthReference;

		// After the last composite read the maps, and before the next one reads them
		VkSubpassDependency dependencies[2]{};
		dependencies[0].srcSubpass    = VK_SUBPASS_EXTERNAL;
		dependencies[0].dstSubpass    = 0;
		dependencies[0].srcStageMask  = VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT;
		dependencies[0].dstStageMask  = DEPTH_STAGES | VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
		dependencies[0].srcAccessMask = 0;
		dependencies[0].dstAccessMask = VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_READ_BIT | VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT | VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
		dependencies[1].srcSubpass    = 0;
		dependencies[1].dstSubpass    = VK_SUBPASS_EXTERNAL;
		dependencies[1].srcStageMask  = DEPTH_STAGES | VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
		dependencies[1].dstStageMask  = VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT;
		dependencies[1].srcAccessMask = VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT | VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
		dependencies[1].dstAccessMask = VK_ACCESS_SHADER_READ_BIT;

		VkRenderPassCreateInfo passInformation{ VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO };
		passInformation.attachmentCount = _countof(attachments);
		passInformation.pAttachments    = attachments;
		passInformation.subpassCount    = 1;
		passInformation.pSubpasses      = &subpass;
		passInformation.dependencyCount = _countof(dependencies);
		passInformation.pDependencies   = dependencies;

		if (vkCreateRenderPass(device, &passInformation, nullptr, &shadowPass) != VK_SUCCESS)
		{
			return false;
		}

		VkImageView const framebufferViews[] = { shadowMap.view, shadowOccupantMap.view };

		VkFramebufferCreateInfo framebufferInformation{ VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO };
		framebufferInformation.renderPass      = shadowPass;
		framebufferInformation.attachmentCount = _countof(framebufferViews);
		framebufferInformation.pAttachments    = framebufferViews;
		framebufferInformation.width           = SHADOW_MAP_SIZE;
		framebufferInformation.height          = SHADOW_MAP_SIZE;
		framebufferInformation.layers          = 1;

		if (vkCreateFramebuffer(device, &framebufferInformation, nullptr, &shadowFramebuffer) != VK_SUCCESS)
		{
			return false;
		}

		// The shaders
		shadowVertexModule      = MakeModule(device, SHADOW_VERTEX_SPIRV, sizeof(SHADOW_VERTEX_SPIRV));
		shadowFragmentModule    = MakeModule(device, SHADOW_FRAGMENT_SPIRV, sizeof(SHADOW_FRAGMENT_SPIRV));
		fullscreenVertexModule  = MakeModule(device, FULLSCREEN_VERTEX_SPIRV, sizeof(FULLSCREEN_VERTEX_SPIRV));
		compositeFragmentModule = MakeModule(device, COMPOSITE_FRAGMENT_SPIRV, sizeof(COMPOSITE_FRAGMENT_SPIRV));

		if (shadowVertexModule == VK_NULL_HANDLE || shadowFragmentModule == VK_NULL_HANDLE || fullscreenVertexModule == VK_NULL_HANDLE || compositeFragmentModule == VK_NULL_HANDLE)
		{
			return false;
		}

		// The caster pipelines: a caster's texture and sampler, and its constants
		VkDescriptorSetLayout const casterSetLayouts[] = { imageSetLayout, samplerSetLayout };

		VkPushConstantRange range{};
		range.stageFlags = VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT;
		range.size       = sizeof(ShadowPushConstants);

		VkPipelineLayoutCreateInfo layoutInformation{ VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO };
		layoutInformation.setLayoutCount         = _countof(casterSetLayouts);
		layoutInformation.pSetLayouts            = casterSetLayouts;
		layoutInformation.pushConstantRangeCount = 1;
		layoutInformation.pPushConstantRanges    = &range;

		if (vkCreatePipelineLayout(device, &layoutInformation, nullptr, &shadowPipelineLayout) != VK_SUCCESS)
		{
			return false;
		}

		// Four pipelines: the captured casters' triangles, the registry casters', and the
		// captured casters' lines and points, which SCD3D11 draws with the topology the
		// game gave them
		VkPipeline* const variantPipelines[]  = { &shadowCasterPipeline, &shadowRegistryPipeline, &shadowLinePipeline, &shadowPointPipeline };
		VkPrimitiveTopology const topologies[] = { VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST, VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST, VK_PRIMITIVE_TOPOLOGY_LINE_LIST, VK_PRIMITIVE_TOPOLOGY_POINT_LIST };

		for (uint32_t variant = 0; variant < _countof(variantPipelines); variant++)
		{
			VkBool32 const isRegistry = (variant == 1) ? VK_TRUE : VK_FALSE;

			VkSpecializationMapEntry const entry{ 0, 0, sizeof(VkBool32) };

			VkSpecializationInfo specialization{};
			specialization.mapEntryCount = 1;
			specialization.pMapEntries   = &entry;
			specialization.dataSize      = sizeof(isRegistry);
			specialization.pData         = &isRegistry;

			VkPipelineShaderStageCreateInfo stages[2]{};
			stages[0].sType               = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
			stages[0].stage               = VK_SHADER_STAGE_VERTEX_BIT;
			stages[0].module              = shadowVertexModule;
			stages[0].pName               = "main";
			stages[0].pSpecializationInfo = &specialization;
			stages[1].sType               = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
			stages[1].stage               = VK_SHADER_STAGE_FRAGMENT_BIT;
			stages[1].module              = shadowFragmentModule;
			stages[1].pName               = "main";
			stages[1].pSpecializationInfo = &specialization;

			VkVertexInputBindingDescription const binding{ 0, sizeof(ShadowVertex), VK_VERTEX_INPUT_RATE_VERTEX };
			VkVertexInputAttributeDescription const attributes[] = {
				{ 0, 0, VK_FORMAT_R32G32B32_SFLOAT, 0 },
				{ 1, 0, VK_FORMAT_R32G32_SFLOAT, sizeof(float) * 3 },
			};

			VkPipelineVertexInputStateCreateInfo vertexInput{ VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO };
			vertexInput.vertexBindingDescriptionCount   = 1;
			vertexInput.pVertexBindingDescriptions      = &binding;
			vertexInput.vertexAttributeDescriptionCount = _countof(attributes);
			vertexInput.pVertexAttributeDescriptions    = attributes;

			VkPipelineInputAssemblyStateCreateInfo inputAssembly{ VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO };
			inputAssembly.topology = topologies[variant];

			// Viewports are in floats, which hold the map's size exactly
			VkViewport const viewport{ 0.0f, 0.0f, static_cast<float>(SHADOW_MAP_SIZE), static_cast<float>(SHADOW_MAP_SIZE), 0.0f, 1.0f };
			VkRect2D const   scissor{ { 0, 0 }, { SHADOW_MAP_SIZE, SHADOW_MAP_SIZE } };

			VkPipelineViewportStateCreateInfo viewportState{ VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO };
			viewportState.viewportCount = 1;
			viewportState.pViewports    = &viewport;
			viewportState.scissorCount  = 1;
			viewportState.pScissors     = &scissor;

			VkPipelineRasterizationStateCreateInfo rasterization{ VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO };
			rasterization.polygonMode = VK_POLYGON_MODE_FILL;
			rasterization.cullMode    = VK_CULL_MODE_NONE;
			rasterization.frontFace   = VK_FRONT_FACE_COUNTER_CLOCKWISE;
			rasterization.lineWidth   = 1.0f;

			// No depth bias in the map
			//
			// A caster that is also lit, a road deck or a ramp at an angle to the sun, has
			// depths that change across each texel, and compared against its own rounded
			// depth it shadowed itself in bands. The composite moves each receiver off its
			// surface by as much as its slope to the light needs, as SCD3D11 does, which an
			// earlier slope-scaled bias here only doubled.

			VkPipelineMultisampleStateCreateInfo multisample{ VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO };
			multisample.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT;

			VkPipelineDepthStencilStateCreateInfo depthStencil{ VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO };
			depthStencil.depthTestEnable  = VK_TRUE;
			depthStencil.depthWriteEnable = VK_TRUE;
			depthStencil.depthCompareOp   = VK_COMPARE_OP_LESS;

			// The occupant number goes into the first channel as it is, never blended
			VkPipelineColorBlendAttachmentState occupantBlend{};
			occupantBlend.colorWriteMask = VK_COLOR_COMPONENT_R_BIT;

			VkPipelineColorBlendStateCreateInfo colourBlend{ VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO };
			colourBlend.attachmentCount = 1;
			colourBlend.pAttachments    = &occupantBlend;

			VkGraphicsPipelineCreateInfo information{ VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO };
			information.stageCount          = _countof(stages);
			information.pStages             = stages;
			information.pVertexInputState   = &vertexInput;
			information.pInputAssemblyState = &inputAssembly;
			information.pViewportState      = &viewportState;
			information.pRasterizationState = &rasterization;
			information.pMultisampleState   = &multisample;
			information.pDepthStencilState  = &depthStencil;
			information.pColorBlendState    = &colourBlend;
			information.layout              = shadowPipelineLayout;
			information.renderPass          = shadowPass;

			VkPipeline& pipeline = *variantPipelines[variant];
			if (vkCreateGraphicsPipelines(device, pipelineCache, 1, &information, nullptr, &pipeline) != VK_SUCCESS)
			{
				pipeline = VK_NULL_HANDLE;
				return false;
			}
		}

		// The composite: its constants, the scene depth, the shadow map, the terrain maps,
		// the stencil, the occupant map and the occupant boxes
		VkDescriptorSetLayoutBinding bindings[COMPOSITE_BINDING_COUNT]{};
		for (uint32_t index = 0; index < _countof(bindings); index++)
		{
			bindings[index].binding         = index;
			bindings[index].descriptorType  = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
			bindings[index].descriptorCount = 1;
			bindings[index].stageFlags      = VK_SHADER_STAGE_FRAGMENT_BIT;
		}

		bindings[0].descriptorType                       = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
		bindings[COMPOSITE_BOXES_BINDING].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;

		VkDescriptorSetLayoutCreateInfo setLayoutInformation{ VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO };
		setLayoutInformation.bindingCount = _countof(bindings);
		setLayoutInformation.pBindings    = bindings;

		if (vkCreateDescriptorSetLayout(device, &setLayoutInformation, nullptr, &compositeSetLayout) != VK_SUCCESS)
		{
			return false;
		}

		VkPipelineLayoutCreateInfo compositeLayoutInformation{ VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO };
		compositeLayoutInformation.setLayoutCount = 1;
		compositeLayoutInformation.pSetLayouts    = &compositeSetLayout;

		if (vkCreatePipelineLayout(device, &compositeLayoutInformation, nullptr, &compositePipelineLayout) != VK_SUCCESS)
		{
			return false;
		}

		compositePipeline = CreateFullscreenPipeline(fullscreenVertexModule, compositeFragmentModule, compositePipelineLayout, colourOnlyPass, VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST, EffectBlend::Multiply);
		if (compositePipeline == VK_NULL_HANDLE)
		{
			return false;
		}

		LogInfo("Vulkan: shadow passes ready, %ux%u shadow map.", SHADOW_MAP_SIZE, SHADOW_MAP_SIZE);
		return true;
	}

	bool VulkanBackend::CreateSceneDepthResources(void)
	{
		if (!isDepthSampleable || depthBuffer.secondView == VK_NULL_HANDLE)
		{
			return false;
		}

		if (fullscreenVertexModule == VK_NULL_HANDLE)
		{
			fullscreenVertexModule = MakeModule(device, FULLSCREEN_VERTEX_SPIRV, sizeof(FULLSCREEN_VERTEX_SPIRV));
		}

		sceneDepthFragmentModule = MakeModule(device, SCENE_DEPTH_FRAGMENT_SPIRV, sizeof(SCENE_DEPTH_FRAGMENT_SPIRV));
		if (fullscreenVertexModule == VK_NULL_HANDLE || sceneDepthFragmentModule == VK_NULL_HANDLE)
		{
			return false;
		}

		// A pass writing every pixel of it, then leaving it for ReShade to sample
		VkAttachmentDescription attachment{};
		attachment.format         = VK_FORMAT_R32_SFLOAT;
		attachment.samples        = VK_SAMPLE_COUNT_1_BIT;
		attachment.loadOp         = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
		attachment.storeOp        = VK_ATTACHMENT_STORE_OP_STORE;
		attachment.stencilLoadOp  = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
		attachment.stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
		attachment.initialLayout  = VK_IMAGE_LAYOUT_UNDEFINED;
		attachment.finalLayout    = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;

		VkAttachmentReference colourReference{ 0, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL };

		VkSubpassDescription subpass{};
		subpass.pipelineBindPoint    = VK_PIPELINE_BIND_POINT_GRAPHICS;
		subpass.colorAttachmentCount = 1;
		subpass.pColorAttachments    = &colourReference;

		// ReShade may read it from any shader stage, in work submitted after this
		VkSubpassDependency dependencies[2]{};
		dependencies[0].srcSubpass    = VK_SUBPASS_EXTERNAL;
		dependencies[0].dstSubpass    = 0;
		dependencies[0].srcStageMask  = VK_PIPELINE_STAGE_ALL_COMMANDS_BIT;
		dependencies[0].dstStageMask  = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
		dependencies[0].srcAccessMask = 0;
		dependencies[0].dstAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
		dependencies[1].srcSubpass    = 0;
		dependencies[1].dstSubpass    = VK_SUBPASS_EXTERNAL;
		dependencies[1].srcStageMask  = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
		dependencies[1].dstStageMask  = VK_PIPELINE_STAGE_ALL_COMMANDS_BIT;
		dependencies[1].srcAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
		dependencies[1].dstAccessMask = VK_ACCESS_SHADER_READ_BIT;

		VkRenderPassCreateInfo passInformation{ VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO };
		passInformation.attachmentCount = 1;
		passInformation.pAttachments    = &attachment;
		passInformation.subpassCount    = 1;
		passInformation.pSubpasses      = &subpass;
		passInformation.dependencyCount = _countof(dependencies);
		passInformation.pDependencies   = dependencies;

		if (vkCreateRenderPass(device, &passInformation, nullptr, &sceneDepthPass) != VK_SUCCESS || !CreateSceneDepthTarget())
		{
			return false;
		}

		// The depth buffer, sampled, and the far plane to encode for
		VkDescriptorSetLayoutBinding binding{};
		binding.binding         = 0;
		binding.descriptorType  = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
		binding.descriptorCount = 1;
		binding.stageFlags      = VK_SHADER_STAGE_FRAGMENT_BIT;

		VkDescriptorSetLayoutCreateInfo setLayoutInformation{ VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO };
		setLayoutInformation.bindingCount = 1;
		setLayoutInformation.pBindings    = &binding;

		if (vkCreateDescriptorSetLayout(device, &setLayoutInformation, nullptr, &sceneDepthSetLayout) != VK_SUCCESS)
		{
			return false;
		}

		VkPushConstantRange range{};
		range.stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT;
		range.size       = sizeof(float);

		VkPipelineLayoutCreateInfo layoutInformation{ VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO };
		layoutInformation.setLayoutCount         = 1;
		layoutInformation.pSetLayouts            = &sceneDepthSetLayout;
		layoutInformation.pushConstantRangeCount = 1;
		layoutInformation.pPushConstantRanges    = &range;

		if (vkCreatePipelineLayout(device, &layoutInformation, nullptr, &sceneDepthPipelineLayout) != VK_SUCCESS)
		{
			return false;
		}

		sceneDepthPipeline = CreateFullscreenPipeline(fullscreenVertexModule, sceneDepthFragmentModule, sceneDepthPipelineLayout, sceneDepthPass, VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST, EffectBlend::Replace);
		return sceneDepthPipeline != VK_NULL_HANDLE;
	}

	bool VulkanBackend::CreateReflectionResources(void)
	{
		// The march reads the depth
		if (!isDepthSampleable || depthBuffer.secondView == VK_NULL_HANDLE)
		{
			return false;
		}

		if (fullscreenVertexModule == VK_NULL_HANDLE)
		{
			fullscreenVertexModule = MakeModule(device, FULLSCREEN_VERTEX_SPIRV, sizeof(FULLSCREEN_VERTEX_SPIRV));
		}

		reflectionFragmentModule = MakeModule(device, REFLECTION_FRAGMENT_SPIRV, sizeof(REFLECTION_FRAGMENT_SPIRV));
		if (fullscreenVertexModule == VK_NULL_HANDLE || reflectionFragmentModule == VK_NULL_HANDLE)
		{
			return false;
		}

		if (!CreateReflectionSource())
		{
			return false;
		}

		// Its constants, the scene depth and the copy
		VkDescriptorSetLayoutBinding bindings[REFLECTION_BINDING_COUNT]{};
		for (uint32_t index = 0; index < _countof(bindings); index++)
		{
			bindings[index].binding         = index;
			bindings[index].descriptorType  = (index == 0) ? VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER : VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
			bindings[index].descriptorCount = 1;
			bindings[index].stageFlags      = VK_SHADER_STAGE_FRAGMENT_BIT;
		}

		VkDescriptorSetLayoutCreateInfo setLayoutInformation{ VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO };
		setLayoutInformation.bindingCount = _countof(bindings);
		setLayoutInformation.pBindings    = bindings;

		if (vkCreateDescriptorSetLayout(device, &setLayoutInformation, nullptr, &reflectionSetLayout) != VK_SUCCESS)
		{
			return false;
		}

		VkPipelineLayoutCreateInfo layoutInformation{ VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO };
		layoutInformation.setLayoutCount = 1;
		layoutInformation.pSetLayouts    = &reflectionSetLayout;

		if (vkCreatePipelineLayout(device, &layoutInformation, nullptr, &reflectionPipelineLayout) != VK_SUCCESS)
		{
			return false;
		}

		reflectionPipeline = CreateFullscreenPipeline(fullscreenVertexModule, reflectionFragmentModule, reflectionPipelineLayout, colourOnlyPass, VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST, EffectBlend::Over);
		return reflectionPipeline != VK_NULL_HANDLE;
	}

	bool VulkanBackend::CreateSceneDepthTarget(void)
	{
		// An R32 float image the size of the back buffer, drawn into and sampled
		if (!CreateRenderImage(VK_FORMAT_R32_SFLOAT, renderWidth, renderHeight, VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT, VK_IMAGE_ASPECT_COLOR_BIT, VK_FORMAT_UNDEFINED, 0, sceneDepthImage))
		{
			return false;
		}

		VkFramebufferCreateInfo framebufferInformation{ VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO };
		framebufferInformation.renderPass      = sceneDepthPass;
		framebufferInformation.attachmentCount = 1;
		framebufferInformation.pAttachments    = &sceneDepthImage.view;
		framebufferInformation.width           = renderWidth;
		framebufferInformation.height          = renderHeight;
		framebufferInformation.layers          = 1;

		return vkCreateFramebuffer(device, &framebufferInformation, nullptr, &sceneDepthFramebuffer) == VK_SUCCESS;
	}

	bool VulkanBackend::CreateReflectionSource(void)
	{
		// A copy of the back buffer for the pass to read, since it draws into the back
		// buffer itself
		return CreateRenderImage(backBuffer.format, renderWidth, renderHeight, VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_SAMPLED_BIT, VK_IMAGE_ASPECT_COLOR_BIT, VK_FORMAT_UNDEFINED, 0, reflectionSourceImage);
	}

	bool VulkanBackend::ResizeEffectImages(void)
	{
		// Take the old ones down
		//
		// The caller has the device idle, so nothing is still reading them.
		if (sceneDepthFramebuffer != VK_NULL_HANDLE)
		{
			vkDestroyFramebuffer(device, sceneDepthFramebuffer, nullptr);
			sceneDepthFramebuffer = VK_NULL_HANDLE;
		}

		DestroyRenderImage(sceneDepthImage);
		DestroyRenderImage(reflectionSourceImage);

		// Make again only what a device that has the effects had. The shadow maps keep
		// their own size, and the composite reads the depth through each pass's own set.
		bool const hasSceneDepth  = sceneDepthPass == VK_NULL_HANDLE || CreateSceneDepthTarget();
		bool const hasReflections = reflectionPipeline == VK_NULL_HANDLE || CreateReflectionSource();

		if (!hasSceneDepth || !hasReflections)
		{
			LogWarn("Vulkan: could not make the %s at %ux%u.", hasSceneDepth ? "water reflections' copy of the frame" : "scene depth for ReShade", renderWidth, renderHeight);
			return false;
		}

		return true;
	}

	bool VulkanBackend::AllocateFrameStaging(VkDeviceSize bytes, VkBuffer& outBuffer, VkDeviceSize& outOffset, uint8_t*& outAddress)
	{
		if (!isFrameActive)
		{
			return false;
		}

		// Take it from the arena, submitting the frame so far to make room
		if (bytes <= stagingArena.blockSize)
		{
			if (!ArenaHasRoom(stagingArena, bytes, 16) && !SubmitFrameSoFar())
			{
				return false;
			}

			return ArenaAllocate(stagingArena, bytes, 16, outBuffer, outOffset, outAddress);
		}

		// Give anything larger a buffer of its own, destroyed once the frame is done
		VkBuffer       buffer = VK_NULL_HANDLE;
		VkDeviceMemory memory = VK_NULL_HANDLE;
		void*          mapped = nullptr;

		if (!CreateHostBuffer(bytes, VK_BUFFER_USAGE_TRANSFER_SRC_BIT, buffer, memory, mapped))
		{
			return false;
		}

		FrameSlot& slot = frameSlots[currentSlot];
		slot.retiredBuffers.push_back(buffer);
		slot.retiredMemory.push_back(memory);

		// Mapped memory comes back untyped, and the caller writes bytes to it
		outBuffer  = buffer;
		outOffset  = 0;
		outAddress = static_cast<uint8_t*>(mapped);
		return true;
	}

	bool VulkanBackend::UploadFloatTexture(RenderImage& image, VkFormat format, uint32_t width, uint32_t height, void const* texels, VkDeviceSize bytes)
	{
		if (width == 0 || height == 0 || texels == nullptr || !EnsureFrame())
		{
			return false;
		}

		// Replace an image of another size
		if (image.image != VK_NULL_HANDLE && (image.width != width || image.height != height || image.format != format))
		{
			RetireRenderImage(image);
		}

		if (image.image == VK_NULL_HANDLE && !CreateRenderImage(format, width, height, VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT, VK_IMAGE_ASPECT_COLOR_BIT, VK_FORMAT_UNDEFINED, 0, image))
		{
			return false;
		}

		// Stage and copy it
		VkBuffer     buffer  = VK_NULL_HANDLE;
		VkDeviceSize offset  = 0;
		uint8_t*     address = nullptr;

		if (!AllocateFrameStaging(bytes, buffer, offset, address))
		{
			return false;
		}

		// A staging block or a buffer of its own fits a 32-bit size.
		memcpy(address, texels, static_cast<size_t>(bytes));

		EndRenderPassIfActive();
		TransitionImage(image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_ASPECT_COLOR_BIT);

		VkBufferImageCopy copy{};
		copy.bufferOffset     = offset;
		copy.imageSubresource = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1 };
		copy.imageExtent      = { width, height, 1 };

		vkCmdCopyBufferToImage(commandBuffer, buffer, image.image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &copy);
		TransitionImage(image, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, VK_IMAGE_ASPECT_COLOR_BIT);
		return true;
	}

	//// Public API

	bool VulkanBackend::CanDrawShadows(void)
	{
		return !isDead && !isDeviceLost && hasShadowResources && isDepthSampleable && depthBuffer.secondView != VK_NULL_HANDLE && stencilView != VK_NULL_HANDLE && IsReady();
	}

	float VulkanBackend::DepthBufferSteps(void) const
	{
		// Without the 32-bit choice the buffer is 16-bit, or wider with every depth rounded
		// to 16 bits. With it, the 32-bit float's steps are never coarser than 2^-24 below 1,
		// and a device without one falls back to 24 bits.
		return shouldUse32BitDepth ? TWENTY_FOUR_BIT_DEPTH_STEPS : SIXTEEN_BIT_DEPTH_STEPS;
	}

	bool VulkanBackend::DrawShadowCasters(ShadowVertex const* vertices, uint32_t vertexCount, uint32_t const* indices, uint32_t indexCount, ShadowCasterDraw const* draws, uint32_t drawCount)
	{
		if (vertices == nullptr || indices == nullptr || draws == nullptr || vertexCount == 0 || indexCount == 0 || drawCount == 0)
		{
			return false;
		}

		if (!CanDrawShadows() || !EnsureFrame())
		{
			return false;
		}

		PhaseScope const recording(*this, FRAME_PHASE_RECORDING);

		// Copy the geometry into the frame's arenas
		VkDeviceSize const vertexBytes = VkDeviceSize{ vertexCount } * sizeof(ShadowVertex);
		VkDeviceSize const indexBytes  = VkDeviceSize{ indexCount } * sizeof(uint32_t);

		if (vertexBytes > vertexArena.blockSize || indexBytes > indexArena.blockSize)
		{
			LogWarn("Vulkan: %u shadow caster vertices and %u indices do not fit the arenas; skipping the shadow map.", vertexCount, indexCount);
			return false;
		}

		if (!ReserveDrawSpace(vertexBytes, indexBytes))
		{
			return false;
		}

		VkBuffer     vertexBuffer  = VK_NULL_HANDLE;
		VkDeviceSize vertexOffset  = 0;
		uint8_t*     vertexAddress = nullptr;
		VkBuffer     indexBuffer   = VK_NULL_HANDLE;
		VkDeviceSize indexOffset   = 0;
		uint8_t*     indexAddress  = nullptr;

		if (!ArenaAllocate(vertexArena, vertexBytes, 16, vertexBuffer, vertexOffset, vertexAddress) || !ArenaAllocate(indexArena, indexBytes, sizeof(uint32_t), indexBuffer, indexOffset, indexAddress))
		{
			return false;
		}

		// The sizes fit their arena blocks, so they fit a size_t.
		memcpy(vertexAddress, vertices, static_cast<size_t>(vertexBytes));
		memcpy(indexAddress, indices, static_cast<size_t>(indexBytes));

		// Draw them into the shadow map, cleared to the far plane, and the occupant map,
		// cleared to no occupant
		EndRenderPassIfActive();

		VkClearValue clears[2]{};
		clears[0].depthStencil.depth = 1.0f;
		clears[1].color.uint32[0]    = 0;

		VkRenderPassBeginInfo beginInformation{ VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO };
		beginInformation.renderPass        = shadowPass;
		beginInformation.framebuffer       = shadowFramebuffer;
		beginInformation.renderArea.extent = { SHADOW_MAP_SIZE, SHADOW_MAP_SIZE };
		beginInformation.clearValueCount   = _countof(clears);
		beginInformation.pClearValues      = clears;

		vkCmdBeginRenderPass(commandBuffer, &beginInformation, VK_SUBPASS_CONTENTS_INLINE);
		vkCmdBindVertexBuffers(commandBuffer, 0, 1, &vertexBuffer, &vertexOffset);
		vkCmdBindIndexBuffer(commandBuffer, indexBuffer, indexOffset, VK_INDEX_TYPE_UINT32);

		VkPipeline      boundPipeline = VK_NULL_HANDLE;
		VkDescriptorSet boundSets[2]  = {};

		for (uint32_t index = 0; index < drawCount; index++)
		{
			ShadowCasterDraw const& draw = draws[index];
			if (draw.indexCount == 0 || uint64_t{ draw.firstIndex } + draw.indexCount > indexCount)
			{
				continue;
			}

			VkPipeline const pipeline = draw.isRegistry ? shadowRegistryPipeline : ((draw.topology == SHADOW_CASTER_LINES) ? shadowLinePipeline : ((draw.topology == SHADOW_CASTER_POINTS) ? shadowPointPipeline : shadowCasterPipeline));
			if (pipeline != boundPipeline)
			{
				vkCmdBindPipeline(commandBuffer, VK_PIPELINE_BIND_POINT_GRAPHICS, pipeline);
				boundPipeline = pipeline;
			}

			// The white texture stands in for an untextured caster, whose alpha is not read
			uint32_t const texture = (draw.texture < textures.size() && textures[draw.texture].isLive) ? draw.texture : 0;
			VkDescriptorSet const sets[2] = { textures[texture].descriptor, GetSamplerSet(draw.samplerParameters) };

			if (sets[0] != boundSets[0] || sets[1] != boundSets[1])
			{
				vkCmdBindDescriptorSets(commandBuffer, VK_PIPELINE_BIND_POINT_GRAPHICS, shadowPipelineLayout, 0, 2, sets, 0, nullptr);
				boundSets[0] = sets[0];
				boundSets[1] = sets[1];
			}

			vkCmdPushConstants(commandBuffer, shadowPipelineLayout, VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT, 0, sizeof(draw.constants), draw.constants);
			vkCmdDrawIndexed(commandBuffer, draw.indexCount, 1, draw.firstIndex, draw.vertexOffset, 0);
		}

		vkCmdEndRenderPass(commandBuffer);
		shadowMap.layout         = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
		shadowOccupantMap.layout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;

		InvalidateBindings();
		return true;
	}

	bool VulkanBackend::UploadTerrainShadowMaps(float const* ceilings, uint32_t ceilingWidth, uint32_t ceilingHeight, float const* vertices, uint32_t verticesX, uint32_t verticesZ)
	{
		if (!CanDrawShadows())
		{
			return false;
		}

		PhaseScope const recording(*this, FRAME_PHASE_RECORDING);

		VkDeviceSize const ceilingBytes = VkDeviceSize{ ceilingWidth } * ceilingHeight * sizeof(float) * 2u;
		VkDeviceSize const vertexBytes  = VkDeviceSize{ verticesX } * verticesZ * sizeof(float) * 4u;

		return UploadFloatTexture(terrainCeilingImage, VK_FORMAT_R32G32_SFLOAT, ceilingWidth, ceilingHeight, ceilings, ceilingBytes) && UploadFloatTexture(terrainVertexImage, VK_FORMAT_R32G32B32A32_SFLOAT, verticesX, verticesZ, vertices, vertexBytes);
	}

	void VulkanBackend::CompositeShadows(ShadowCompositeConstants const& constants, int32_t const rectangle[4], bool isTerrainUsed, float const* occupantBoxes, uint32_t occupantCount)
	{
		if (!CanDrawShadows() || !EnsureFrame())
		{
			return;
		}

		PhaseScope const recording(*this, FRAME_PHASE_RECORDING);

		// Clip the rectangle to the back buffer
		int32_t const left   = std::max(rectangle[0], 0);
		int32_t const top    = std::max(rectangle[1], 0);
		int32_t const right  = std::min(rectangle[0] + rectangle[2], RenderWidth());
		int32_t const bottom = std::min(rectangle[1] + rectangle[3], RenderHeight());

		if (right <= left || bottom <= top)
		{
			return;
		}

		// Size the occupant boxes
		//
		// At least one, since the binding has to name a buffer even when the composite reads
		// none. More than half a block of the arena, which leaves the constants room beside
		// them, are left out, and the composite then skips its self test. A pass has a few
		// thousand occupants at most, and half a block holds sixteen thousand.
		VkDeviceSize const alignment    = std::max<VkDeviceSize>(physicalDeviceProperties.limits.minUniformBufferOffsetAlignment, 16);
		VkDeviceSize const boxAlignment = std::max<VkDeviceSize>(physicalDeviceProperties.limits.minStorageBufferOffsetAlignment, 16);

		ShadowCompositeConstants uploaded = constants;
		uint32_t boxCount = (occupantBoxes != nullptr) ? occupantCount : 0u;

		if (VkDeviceSize{ boxCount } * OCCUPANT_BOX_BYTES > uniformArena.blockSize / 2)
		{
			static bool hasLoggedBoxes = false;
			if (!hasLoggedBoxes)
			{
				hasLoggedBoxes = true;
				LogInfo("Vulkan: %u shadow occupants do not fit the uniform arena; their self test is skipped.", boxCount);
			}

			boxCount = 0;
			uploaded.material[3] = 0.0f;
		}

		VkDeviceSize const boxBytes = VkDeviceSize{ std::max(boxCount, 1u) } * OCCUPANT_BOX_BYTES;

		// Put the constants and the boxes in the uniform arena, making room for both at once
		// so that neither submits the frame under the other
		if (!ArenaHasRoom(uniformArena, sizeof(uploaded) + boxAlignment + boxBytes, alignment) && !SubmitFrameSoFar())
		{
			return;
		}

		VkBuffer     uniformBuffer  = VK_NULL_HANDLE;
		VkDeviceSize uniformOffset  = 0;
		uint8_t*     uniformAddress = nullptr;
		VkBuffer     boxBuffer      = VK_NULL_HANDLE;
		VkDeviceSize boxOffset      = 0;
		uint8_t*     boxAddress     = nullptr;

		if (!ArenaAllocate(uniformArena, sizeof(uploaded), alignment, uniformBuffer, uniformOffset, uniformAddress) || !ArenaAllocate(uniformArena, boxBytes, boxAlignment, boxBuffer, boxOffset, boxAddress))
		{
			return;
		}

		memcpy(uniformAddress, &uploaded, sizeof(uploaded));

		// The box bytes fit one block of the arena, so they fit a size_t.
		if (boxCount != 0)
		{
			memcpy(boxAddress, occupantBoxes, static_cast<size_t>(boxBytes));
		}
		else
		{
			memset(boxAddress, 0, static_cast<size_t>(boxBytes));
		}

		// Give maps no caster has drawn into a defined layout, though nothing reads them
		if (shadowMap.layout == VK_IMAGE_LAYOUT_UNDEFINED)
		{
			EndRenderPassIfActive();
			TransitionImage(shadowMap, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, VK_IMAGE_ASPECT_DEPTH_BIT);
		}

		if (shadowOccupantMap.layout == VK_IMAGE_LAYOUT_UNDEFINED)
		{
			EndRenderPassIfActive();
			TransitionImage(shadowOccupantMap, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, VK_IMAGE_ASPECT_COLOR_BIT);
		}

		// Bind the scene depth, the shadow map, the terrain maps, the stencil, the occupant
		// map and the boxes
		//
		// Without the terrain maps the shadow map stands in for them: every binding has to
		// name an image, and the shader leaves them unread.
		bool const hasTerrain = isTerrainUsed && terrainCeilingImage.layout == VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL && terrainVertexImage.layout == VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;

		VkDescriptorSet set = VK_NULL_HANDLE;
		if (!AllocateTransientSet(compositeSetLayout, set))
		{
			return;
		}

		VkDescriptorBufferInfo bufferInformation{ uniformBuffer, uniformOffset, sizeof(uploaded) };
		VkDescriptorBufferInfo boxInformation{ boxBuffer, boxOffset, boxBytes };

		// The point sampler reads the integer images too, which take no filtering
		VkDescriptorImageInfo images[COMPOSITE_IMAGE_COUNT]{};
		images[0] = { pointClampSampler, depthBuffer.secondView, VK_IMAGE_LAYOUT_DEPTH_STENCIL_READ_ONLY_OPTIMAL };
		images[1] = { pointClampSampler, shadowMap.view, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL };
		images[2] = { pointClampSampler, hasTerrain ? terrainCeilingImage.view : shadowMap.view, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL };
		images[3] = { pointClampSampler, hasTerrain ? terrainVertexImage.view : shadowMap.view, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL };
		images[4] = { pointClampSampler, stencilView, VK_IMAGE_LAYOUT_DEPTH_STENCIL_READ_ONLY_OPTIMAL };
		images[5] = { pointClampSampler, shadowOccupantMap.view, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL };

		VkWriteDescriptorSet writes[COMPOSITE_BINDING_COUNT]{};
		for (uint32_t index = 0; index < _countof(writes); index++)
		{
			writes[index].sType           = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
			writes[index].dstSet          = set;
			writes[index].dstBinding      = index;
			writes[index].descriptorCount = 1;

			if (index == 0)
			{
				writes[index].descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
				writes[index].pBufferInfo    = &bufferInformation;
			}
			else if (index == COMPOSITE_BOXES_BINDING)
			{
				writes[index].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
				writes[index].pBufferInfo    = &boxInformation;
			}
			else
			{
				writes[index].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
				writes[index].pImageInfo     = &images[index - 1];
			}
		}

		vkUpdateDescriptorSets(device, _countof(writes), writes, 0, nullptr);

		// Read the depth while drawing over the colour
		TransitionDepth(VK_IMAGE_LAYOUT_DEPTH_STENCIL_READ_ONLY_OPTIMAL);
		BeginColourOnlyPass();

		// The rectangle's sides are inside the back buffer, so they are not negative.
		VkViewport const viewport{ static_cast<float>(left), static_cast<float>(top), static_cast<float>(right - left), static_cast<float>(bottom - top), 0.0f, 1.0f };
		VkRect2D const   scissor{ { left, top }, { static_cast<uint32_t>(right - left), static_cast<uint32_t>(bottom - top) } };

		vkCmdBindPipeline(commandBuffer, VK_PIPELINE_BIND_POINT_GRAPHICS, compositePipeline);
		vkCmdBindDescriptorSets(commandBuffer, VK_PIPELINE_BIND_POINT_GRAPHICS, compositePipelineLayout, 0, 1, &set, 0, nullptr);
		vkCmdSetViewport(commandBuffer, 0, 1, &viewport);
		vkCmdSetScissor(commandBuffer, 0, 1, &scissor);
		vkCmdDraw(commandBuffer, 3, 1, 0, 0);

		InvalidateBindings();
	}

	VkImageView VulkanBackend::BackBufferView(bool isSrgb) const
	{
		return (isSrgb && backBuffer.secondView != VK_NULL_HANDLE) ? backBuffer.secondView : backBuffer.view;
	}

	VkImageView VulkanBackend::PrepareSceneDepth(float farPlane)
	{
		if (sceneDepthPipeline == VK_NULL_HANDLE || !EnsureFrame())
		{
			return VK_NULL_HANDLE;
		}

		PhaseScope const recording(*this, FRAME_PHASE_RECORDING);

		VkDescriptorSet set = VK_NULL_HANDLE;
		if (!AllocateTransientSet(sceneDepthSetLayout, set))
		{
			return VK_NULL_HANDLE;
		}

		VkDescriptorImageInfo image{ pointClampSampler, depthBuffer.secondView, VK_IMAGE_LAYOUT_DEPTH_STENCIL_READ_ONLY_OPTIMAL };

		VkWriteDescriptorSet write{ VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET };
		write.dstSet          = set;
		write.descriptorCount = 1;
		write.descriptorType  = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
		write.pImageInfo      = &image;

		vkUpdateDescriptorSets(device, 1, &write, 0, nullptr);

		// Read the depth buffer while writing every pixel of the encoded image
		EndRenderPassIfActive();
		TransitionDepth(VK_IMAGE_LAYOUT_DEPTH_STENCIL_READ_ONLY_OPTIMAL);

		VkRenderPassBeginInfo beginInformation{ VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO };
		beginInformation.renderPass        = sceneDepthPass;
		beginInformation.framebuffer       = sceneDepthFramebuffer;
		beginInformation.renderArea.extent = { renderWidth, renderHeight };

		// Viewports are in floats, which hold any window size exactly
		VkViewport const viewport{ 0.0f, 0.0f, static_cast<float>(renderWidth), static_cast<float>(renderHeight), 0.0f, 1.0f };
		VkRect2D const   scissor{ { 0, 0 }, { renderWidth, renderHeight } };

		vkCmdBeginRenderPass(commandBuffer, &beginInformation, VK_SUBPASS_CONTENTS_INLINE);
		vkCmdBindPipeline(commandBuffer, VK_PIPELINE_BIND_POINT_GRAPHICS, sceneDepthPipeline);
		vkCmdBindDescriptorSets(commandBuffer, VK_PIPELINE_BIND_POINT_GRAPHICS, sceneDepthPipelineLayout, 0, 1, &set, 0, nullptr);
		vkCmdSetViewport(commandBuffer, 0, 1, &viewport);
		vkCmdSetScissor(commandBuffer, 0, 1, &scissor);
		vkCmdPushConstants(commandBuffer, sceneDepthPipelineLayout, VK_SHADER_STAGE_FRAGMENT_BIT, 0, sizeof(farPlane), &farPlane);
		vkCmdDraw(commandBuffer, 3, 1, 0, 0);
		vkCmdEndRenderPass(commandBuffer);

		sceneDepthImage.layout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
		InvalidateBindings();
		return sceneDepthImage.view;
	}

	bool VulkanBackend::CanDrawReflections(void)
	{
		return !isDead && !isDeviceLost && reflectionPipeline != VK_NULL_HANDLE && isDepthSampleable && depthBuffer.secondView != VK_NULL_HANDLE && IsReady();
	}

	void VulkanBackend::DrawWaterReflections(WaterReflectionConstants const& constants)
	{
		if (!CanDrawReflections() || !EnsureFrame())
		{
			return;
		}

		PhaseScope const recording(*this, FRAME_PHASE_RECORDING);

		// Put the constants in the uniform arena
		VkDeviceSize const alignment = std::max<VkDeviceSize>(physicalDeviceProperties.limits.minUniformBufferOffsetAlignment, 16);

		if (!ArenaHasRoom(uniformArena, sizeof(constants), alignment) && !SubmitFrameSoFar())
		{
			return;
		}

		VkBuffer     uniformBuffer  = VK_NULL_HANDLE;
		VkDeviceSize uniformOffset  = 0;
		uint8_t*     uniformAddress = nullptr;

		if (!ArenaAllocate(uniformArena, sizeof(constants), alignment, uniformBuffer, uniformOffset, uniformAddress))
		{
			return;
		}

		memcpy(uniformAddress, &constants, sizeof(constants));

		VkDescriptorSet set = VK_NULL_HANDLE;
		if (!AllocateTransientSet(reflectionSetLayout, set))
		{
			return;
		}

		// Copy the frame as it stands, which the reflections read while drawing over it
		//
		// A copy is not a render pass operation. Every pixel of the copy is written again,
		// so whatever the last frame left in it goes.
		EndRenderPassIfActive();
		TransitionTo(VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL);
		TransitionImage(reflectionSourceImage, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_ASPECT_COLOR_BIT);

		VkImageCopy copy{};
		copy.srcSubresource = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1 };
		copy.dstSubresource = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1 };
		copy.extent         = { renderWidth, renderHeight, 1 };

		vkCmdCopyImage(commandBuffer, backBuffer.image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, reflectionSourceImage.image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &copy);
		TransitionImage(reflectionSourceImage, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, VK_IMAGE_ASPECT_COLOR_BIT);

		// Bind the constants, the scene depth and the copy, which is filtered since a
		// reflection lands between pixels
		VkDescriptorBufferInfo bufferInformation{ uniformBuffer, uniformOffset, sizeof(constants) };

		VkDescriptorImageInfo images[REFLECTION_BINDING_COUNT - 1]{};
		images[0] = { pointClampSampler, depthBuffer.secondView, VK_IMAGE_LAYOUT_DEPTH_STENCIL_READ_ONLY_OPTIMAL };
		images[1] = { linearClampSampler, reflectionSourceImage.view, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL };

		VkWriteDescriptorSet writes[REFLECTION_BINDING_COUNT]{};
		for (uint32_t index = 0; index < _countof(writes); index++)
		{
			writes[index].sType           = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
			writes[index].dstSet          = set;
			writes[index].dstBinding      = index;
			writes[index].descriptorCount = 1;
			writes[index].descriptorType  = (index == 0) ? VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER : VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;

			if (index == 0)
			{
				writes[index].pBufferInfo = &bufferInformation;
			}
			else
			{
				writes[index].pImageInfo = &images[index - 1];
			}
		}

		vkUpdateDescriptorSets(device, _countof(writes), writes, 0, nullptr);

		// Read the depth while drawing over the colour, across the whole back buffer
		TransitionDepth(VK_IMAGE_LAYOUT_DEPTH_STENCIL_READ_ONLY_OPTIMAL);
		BeginColourOnlyPass();

		// Viewports are in floats, which hold any window size exactly
		VkViewport const viewport{ 0.0f, 0.0f, static_cast<float>(renderWidth), static_cast<float>(renderHeight), 0.0f, 1.0f };
		VkRect2D const   scissor{ { 0, 0 }, { renderWidth, renderHeight } };

		vkCmdBindPipeline(commandBuffer, VK_PIPELINE_BIND_POINT_GRAPHICS, reflectionPipeline);
		vkCmdBindDescriptorSets(commandBuffer, VK_PIPELINE_BIND_POINT_GRAPHICS, reflectionPipelineLayout, 0, 1, &set, 0, nullptr);
		vkCmdSetViewport(commandBuffer, 0, 1, &viewport);
		vkCmdSetScissor(commandBuffer, 0, 1, &scissor);
		vkCmdDraw(commandBuffer, 3, 1, 0, 0);

		InvalidateBindings();
	}

	bool VulkanBackend::SubmitForExternalWork(void)
	{
		if (!EnsureFrame())
		{
			return false;
		}

		PhaseScope const submitting(*this, FRAME_PHASE_SUBMITS);

		// Leave the back buffer as a render target, as ReShade expects it
		EndRenderPassIfActive();
		TransitionTo(VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL);

		// Submit without waiting, then carry on in a command buffer that starts by
		// waiting for whatever is submitted in between
		if (!SubmitCommandBuffer(VK_NULL_HANDLE, 0, VK_NULL_HANDLE, false))
		{
			isFrameActive = false;
			return false;
		}

		isExternalWorkPending = true;

		if (!BeginCommandBuffer())
		{
			isFrameActive = false;
			return false;
		}

		return true;
	}

	bool VulkanBackend::BorrowExternalFrame(ExternalFrame& outFrame)
	{
		if (!EnsureFrame())
		{
			return false;
		}

		EndRenderPassIfActive();
		TransitionTo(VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL);

		outFrame.instance       = instance;
		outFrame.physicalDevice = physicalDevice;
		outFrame.device         = device;
		outFrame.queue          = queue;
		outFrame.queueFamily    = queueFamily;
		outFrame.commandBuffer  = commandBuffer;
		outFrame.backBuffer     = backBuffer.image;
		outFrame.backBufferView = backBuffer.view;
		outFrame.format         = backBuffer.format;
		outFrame.width          = renderWidth;
		outFrame.height         = renderHeight;
		return true;
	}

	void VulkanBackend::ReturnExternalFrame(void)
	{
		// Whatever the plugin bound is not what scvk thinks is bound
		activePass = ActivePass::None;
		InvalidateBindings();
	}

	void VulkanBackend::DescribeDevice(ExternalFrame& outFrame) const
	{
		outFrame = ExternalFrame{};
		outFrame.instance       = instance;
		outFrame.physicalDevice = physicalDevice;
		outFrame.device         = device;
		outFrame.queue          = queue;
		outFrame.queueFamily    = queueFamily;
	}
}
