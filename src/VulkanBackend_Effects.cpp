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
 * The passes of scvk's own, beside the game's draws: the blits, the shadow map and its
 * composite, the scene depth handed to ReShade, and what ReShade and other plugins are
 * given to draw into the frame themselves.
 *
 * The blits are drawn as SCD3D11 draws them, a textured quad over the back buffer that
 * can scale, blend and key out a colour. The shadow passes follow SCD3D11's live shadows:
 * casters drawn into a 2048 square depth map from the sun, then a full-screen pass that
 * darkens what the scene depth puts in their shadow or below the terrain's.
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

		// The casters' depth bias in the shadow map: twice each face's depth slope, and
		// a floor of a few units of the 32-bit float depth's precision.
		constexpr float SHADOW_DEPTH_BIAS_SLOPE    = 2.0f;
		constexpr float SHADOW_DEPTH_BIAS_CONSTANT = 4.0f;

		// The push constant blocks, as the shaders declare them.
		struct BlitConstants
		{
			float rectangle[4];
			float sourceExtent[4];
			float modulate[4];
			float colourKey[4];
			float options[4];
		};

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

		// How the full-screen and blit pipelines blend.
		enum class EffectBlend
		{
			// Written over what is there.
			None,

			// Over by the source alpha, the alpha accumulating as Direct3D's blits do.
			Alpha,

			// Over by the source alpha, leaving the destination alpha alone, as SCD3D11's
			// shadow composite blends.
			Shadow,
		};
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

		/** The blend state of one colour attachment. */
		VkPipelineColorBlendAttachmentState MakeBlend(EffectBlend blend)
		{
			VkPipelineColorBlendAttachmentState attachment{};
			attachment.colorWriteMask = VK_COLOR_COMPONENT_R_BIT | VK_COLOR_COMPONENT_G_BIT | VK_COLOR_COMPONENT_B_BIT | VK_COLOR_COMPONENT_A_BIT;

			if (blend == EffectBlend::None)
			{
				return attachment;
			}

			attachment.blendEnable         = VK_TRUE;
			attachment.srcColorBlendFactor = VK_BLEND_FACTOR_SRC_ALPHA;
			attachment.dstColorBlendFactor = VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA;
			attachment.colorBlendOp        = VK_BLEND_OP_ADD;
			attachment.alphaBlendOp        = VK_BLEND_OP_ADD;

			if (blend == EffectBlend::Alpha)
			{
				attachment.srcAlphaBlendFactor = VK_BLEND_FACTOR_ONE;
				attachment.dstAlphaBlendFactor = VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA;
			}
			else
			{
				attachment.srcAlphaBlendFactor = VK_BLEND_FACTOR_ZERO;
				attachment.dstAlphaBlendFactor = VK_BLEND_FACTOR_ONE;
			}

			return attachment;
		}
	}

	VkPipeline VulkanBackend::CreateFullscreenPipeline(VkShaderModule vertexModule, VkShaderModule fragmentModule, VkPipelineLayout layout, VkRenderPass pass, VkPrimitiveTopology topology, bool isAlphaBlended, bool isShadowBlend)
	{
		VkPipelineShaderStageCreateInfo stages[2]{};
		stages[0].sType  = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
		stages[0].stage  = VK_SHADER_STAGE_VERTEX_BIT;
		stages[0].module = vertexModule;
		stages[0].pName  = "main";
		stages[1].sType  = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
		stages[1].stage  = VK_SHADER_STAGE_FRAGMENT_BIT;
		stages[1].module = fragmentModule;
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

		EffectBlend const blend = isShadowBlend ? EffectBlend::Shadow : (isAlphaBlended ? EffectBlend::Alpha : EffectBlend::None);
		VkPipelineColorBlendAttachmentState const blendAttachment = MakeBlend(blend);

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
		// The blits are part of what the game draws, so they have to work. The shadow
		// passes and the depth for ReShade are extras: without them the picture is the
		// game's own, so their failure is logged and the device carries on.
		if (!CreateBlitPipelines())
		{
			return false;
		}

		hasShadowResources = CreateShadowResources();
		if (!hasShadowResources)
		{
			LogInfo("Vulkan: the shadow passes are unavailable on this device.");
		}

		if (!CreateSceneDepthResources())
		{
			LogInfo("Vulkan: the scene depth for ReShade is unavailable on this device.");
		}

		return true;
	}

	void VulkanBackend::DestroyEffectResources(void)
	{
		if (device == VK_NULL_HANDLE)
		{
			return;
		}

		// The blits
		for (VkPipeline& pipeline : blitPipelines)
		{
			if (pipeline != VK_NULL_HANDLE) { vkDestroyPipeline(device, pipeline, nullptr); pipeline = VK_NULL_HANDLE; }
		}

		if (blitPipelineLayout != VK_NULL_HANDLE) { vkDestroyPipelineLayout(device, blitPipelineLayout, nullptr); blitPipelineLayout = VK_NULL_HANDLE; }
		if (blitVertexModule != VK_NULL_HANDLE)   { vkDestroyShaderModule(device, blitVertexModule, nullptr); blitVertexModule = VK_NULL_HANDLE; }
		if (blitFragmentModule != VK_NULL_HANDLE) { vkDestroyShaderModule(device, blitFragmentModule, nullptr); blitFragmentModule = VK_NULL_HANDLE; }

		if (blitImageSet != VK_NULL_HANDLE && blitImagePoolIndex < texturePools.size())
		{
			TexturePool& texturePool = texturePools[blitImagePoolIndex];
			vkFreeDescriptorSets(device, texturePool.pool, 1, &blitImageSet);
			texturePool.usedSets--;
		}

		blitImageSet = VK_NULL_HANDLE;
		DestroyRenderImage(blitImage);

		// The shadows
		VkPipeline* const shadowPipelines[] = { &shadowCasterPipeline, &shadowRegistryPipeline, &shadowLinePipeline, &shadowPointPipeline, &compositePipeline, &sceneDepthPipeline };
		for (VkPipeline* pipeline : shadowPipelines)
		{
			if (*pipeline != VK_NULL_HANDLE) { vkDestroyPipeline(device, *pipeline, nullptr); *pipeline = VK_NULL_HANDLE; }
		}

		VkPipelineLayout* const layouts[] = { &shadowPipelineLayout, &compositePipelineLayout, &sceneDepthPipelineLayout };
		for (VkPipelineLayout* layout : layouts)
		{
			if (*layout != VK_NULL_HANDLE) { vkDestroyPipelineLayout(device, *layout, nullptr); *layout = VK_NULL_HANDLE; }
		}

		VkDescriptorSetLayout* const setLayouts[] = { &compositeSetLayout, &sceneDepthSetLayout };
		for (VkDescriptorSetLayout* layout : setLayouts)
		{
			if (*layout != VK_NULL_HANDLE) { vkDestroyDescriptorSetLayout(device, *layout, nullptr); *layout = VK_NULL_HANDLE; }
		}

		VkShaderModule* const modules[] = { &shadowVertexModule, &shadowFragmentModule, &fullscreenVertexModule, &compositeFragmentModule, &sceneDepthFragmentModule };
		for (VkShaderModule* module : modules)
		{
			if (*module != VK_NULL_HANDLE) { vkDestroyShaderModule(device, *module, nullptr); *module = VK_NULL_HANDLE; }
		}

		if (shadowFramebuffer != VK_NULL_HANDLE)     { vkDestroyFramebuffer(device, shadowFramebuffer, nullptr); shadowFramebuffer = VK_NULL_HANDLE; }
		if (sceneDepthFramebuffer != VK_NULL_HANDLE) { vkDestroyFramebuffer(device, sceneDepthFramebuffer, nullptr); sceneDepthFramebuffer = VK_NULL_HANDLE; }
		if (shadowPass != VK_NULL_HANDLE)            { vkDestroyRenderPass(device, shadowPass, nullptr); shadowPass = VK_NULL_HANDLE; }
		if (sceneDepthPass != VK_NULL_HANDLE)        { vkDestroyRenderPass(device, sceneDepthPass, nullptr); sceneDepthPass = VK_NULL_HANDLE; }

		DestroyRenderImage(shadowMap);
		DestroyRenderImage(terrainCeilingImage);
		DestroyRenderImage(terrainVertexImage);
		DestroyRenderImage(sceneDepthImage);

		hasShadowResources = false;
	}

	bool VulkanBackend::CreateBlitPipelines(void)
	{
		blitVertexModule   = MakeModule(device, BLIT_VERTEX_SPIRV, sizeof(BLIT_VERTEX_SPIRV));
		blitFragmentModule = MakeModule(device, BLIT_FRAGMENT_SPIRV, sizeof(BLIT_FRAGMENT_SPIRV));

		if (blitVertexModule == VK_NULL_HANDLE || blitFragmentModule == VK_NULL_HANDLE)
		{
			LogError("Vulkan: could not create the blit shaders.");
			return false;
		}

		// The source image and its sampler, as the geometry binds a texture
		VkDescriptorSetLayout const setLayouts[] = { imageSetLayout, samplerSetLayout };

		VkPushConstantRange range{};
		range.stageFlags = VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT;
		range.size       = sizeof(BlitConstants);

		VkPipelineLayoutCreateInfo layoutInformation{ VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO };
		layoutInformation.setLayoutCount         = _countof(setLayouts);
		layoutInformation.pSetLayouts            = setLayouts;
		layoutInformation.pushConstantRangeCount = 1;
		layoutInformation.pPushConstantRanges    = &range;

		VkResult const result = vkCreatePipelineLayout(device, &layoutInformation, nullptr, &blitPipelineLayout);
		if (result != VK_SUCCESS)
		{
			Fail("vkCreatePipelineLayout (blit)", result);
			return false;
		}

		// Opaque and blended, drawn in the main pass with the depth left alone
		blitPipelines[0] = CreateFullscreenPipeline(blitVertexModule, blitFragmentModule, blitPipelineLayout, renderPass, VK_PRIMITIVE_TOPOLOGY_TRIANGLE_STRIP, false, false);
		blitPipelines[1] = CreateFullscreenPipeline(blitVertexModule, blitFragmentModule, blitPipelineLayout, renderPass, VK_PRIMITIVE_TOPOLOGY_TRIANGLE_STRIP, true, false);

		return blitPipelines[0] != VK_NULL_HANDLE && blitPipelines[1] != VK_NULL_HANDLE;
	}

	bool VulkanBackend::CreateShadowResources(void)
	{
		if (!isDepthSampleable || depthBuffer.secondView == VK_NULL_HANDLE)
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

		// The caster pass: depth alone, cleared, then left ready to sample
		VkAttachmentDescription attachment{};
		attachment.format         = VK_FORMAT_D32_SFLOAT;
		attachment.samples        = VK_SAMPLE_COUNT_1_BIT;
		attachment.loadOp         = VK_ATTACHMENT_LOAD_OP_CLEAR;
		attachment.storeOp        = VK_ATTACHMENT_STORE_OP_STORE;
		attachment.stencilLoadOp  = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
		attachment.stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
		attachment.initialLayout  = VK_IMAGE_LAYOUT_UNDEFINED;
		attachment.finalLayout    = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;

		VkAttachmentReference depthReference{ 0, VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL };

		VkSubpassDescription subpass{};
		subpass.pipelineBindPoint       = VK_PIPELINE_BIND_POINT_GRAPHICS;
		subpass.pDepthStencilAttachment = &depthReference;

		// After the last composite read the map, and before the next one reads it
		VkSubpassDependency dependencies[2]{};
		dependencies[0].srcSubpass    = VK_SUBPASS_EXTERNAL;
		dependencies[0].dstSubpass    = 0;
		dependencies[0].srcStageMask  = VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT;
		dependencies[0].dstStageMask  = DEPTH_STAGES;
		dependencies[0].srcAccessMask = 0;
		dependencies[0].dstAccessMask = VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_READ_BIT | VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT;
		dependencies[1].srcSubpass    = 0;
		dependencies[1].dstSubpass    = VK_SUBPASS_EXTERNAL;
		dependencies[1].srcStageMask  = DEPTH_STAGES;
		dependencies[1].dstStageMask  = VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT;
		dependencies[1].srcAccessMask = VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT;
		dependencies[1].dstAccessMask = VK_ACCESS_SHADER_READ_BIT;

		VkRenderPassCreateInfo passInformation{ VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO };
		passInformation.attachmentCount = 1;
		passInformation.pAttachments    = &attachment;
		passInformation.subpassCount    = 1;
		passInformation.pSubpasses      = &subpass;
		passInformation.dependencyCount = _countof(dependencies);
		passInformation.pDependencies   = dependencies;

		if (vkCreateRenderPass(device, &passInformation, nullptr, &shadowPass) != VK_SUCCESS)
		{
			return false;
		}

		VkFramebufferCreateInfo framebufferInformation{ VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO };
		framebufferInformation.renderPass      = shadowPass;
		framebufferInformation.attachmentCount = 1;
		framebufferInformation.pAttachments    = &shadowMap.view;
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

			// Slope-scaled depth bias, as shadow maps need and SCD3D11's caster pass lacked
			//
			// A caster that is also lit, a road deck or a ramp at an angle to the sun, has
			// depths that change across each texel, and compared against its own rounded
			// depth it shadowed itself in bands. Pushing each caster back by twice its depth
			// slope covers a texel's change and the composite's 3x3 filter. The constant
			// part is in units of the depth format's precision, a small floor for faces
			// turned straight at the sun.
			rasterization.depthBiasEnable         = VK_TRUE;
			rasterization.depthBiasConstantFactor = SHADOW_DEPTH_BIAS_CONSTANT;
			rasterization.depthBiasSlopeFactor    = SHADOW_DEPTH_BIAS_SLOPE;

			VkPipelineMultisampleStateCreateInfo multisample{ VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO };
			multisample.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT;

			VkPipelineDepthStencilStateCreateInfo depthStencil{ VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO };
			depthStencil.depthTestEnable  = VK_TRUE;
			depthStencil.depthWriteEnable = VK_TRUE;
			depthStencil.depthCompareOp   = VK_COMPARE_OP_LESS;

			VkPipelineColorBlendStateCreateInfo colourBlend{ VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO };

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

		// The composite: its constants, the scene depth, the shadow map and the terrain maps
		VkDescriptorSetLayoutBinding bindings[5]{};
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

		compositePipeline = CreateFullscreenPipeline(fullscreenVertexModule, compositeFragmentModule, compositePipelineLayout, colourOnlyPass, VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST, false, true);
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

		// An R32 float image the size of the back buffer, drawn into and sampled
		if (!CreateRenderImage(VK_FORMAT_R32_SFLOAT, renderWidth, renderHeight, VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT, VK_IMAGE_ASPECT_COLOR_BIT, VK_FORMAT_UNDEFINED, 0, sceneDepthImage))
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

		if (vkCreateRenderPass(device, &passInformation, nullptr, &sceneDepthPass) != VK_SUCCESS)
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

		if (vkCreateFramebuffer(device, &framebufferInformation, nullptr, &sceneDepthFramebuffer) != VK_SUCCESS)
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

		sceneDepthPipeline = CreateFullscreenPipeline(fullscreenVertexModule, sceneDepthFragmentModule, sceneDepthPipelineLayout, sceneDepthPass, VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST, false, false);
		return sceneDepthPipeline != VK_NULL_HANDLE;
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

	void VulkanBackend::DrawPixels(int32_t destinationX, int32_t destinationY, int32_t destinationWidth, int32_t destinationHeight, uint32_t sourceWidth, uint32_t sourceHeight, uint8_t const* bgraPixels, float const modulate[4], float const colourKey[4], bool isSourceAlphaUsed, bool isBlended)
	{
		if (bgraPixels == nullptr || sourceWidth == 0 || sourceHeight == 0 || destinationWidth <= 0 || destinationHeight <= 0 || renderWidth == 0 || renderHeight == 0)
		{
			return;
		}

		if (!EnsureFrame() || blitPipelines[0] == VK_NULL_HANDLE)
		{
			return;
		}

		PhaseScope const recording(*this, FRAME_PHASE_RECORDING);

		// Grow the blit image to hold the source
		//
		// It only grows, as SCD3D11's does. The old image and its set may still be in use
		// by draws recorded earlier, so both are retired rather than destroyed.
		if (blitImage.image == VK_NULL_HANDLE || blitImage.width < sourceWidth || blitImage.height < sourceHeight)
		{
			uint32_t const width  = std::max(sourceWidth, blitImage.width);
			uint32_t const height = std::max(sourceHeight, blitImage.height);

			if (blitImage.image != VK_NULL_HANDLE)
			{
				RetiredImage retired;
				retired.image               = blitImage.image;
				retired.memory.memory       = blitImage.memory;
				retired.view                = blitImage.view;
				retired.descriptor          = blitImageSet;
				retired.descriptorPoolIndex = blitImagePoolIndex;

				Retire(retired);
				blitImage    = RenderImage{};
				blitImageSet = VK_NULL_HANDLE;
			}

			if (!CreateRenderImage(VK_FORMAT_B8G8R8A8_UNORM, width, height, VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT, VK_IMAGE_ASPECT_COLOR_BIT, VK_FORMAT_UNDEFINED, 0, blitImage))
			{
				LogWarn("Vulkan: could not create a %ux%u blit image; skipping the blit.", width, height);
				return;
			}

			if (!AllocateTextureSet(blitImageSet, blitImagePoolIndex))
			{
				DestroyRenderImage(blitImage);
				blitImageSet = VK_NULL_HANDLE;
				return;
			}

			VkDescriptorImageInfo imageInformation{};
			imageInformation.imageView   = blitImage.view;
			imageInformation.imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;

			VkWriteDescriptorSet write{ VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET };
			write.dstSet          = blitImageSet;
			write.descriptorCount = 1;
			write.descriptorType  = VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE;
			write.pImageInfo      = &imageInformation;

			vkUpdateDescriptorSets(device, 1, &write, 0, nullptr);
		}

		// Stage the pixels and copy them in
		//
		// The image is used again by every blit, so each copy waits for the draws of the
		// blit before it, which the layout transitions order.
		VkDeviceSize const bytes = VkDeviceSize{ sourceWidth } * sourceHeight * 4u;

		VkBuffer     buffer  = VK_NULL_HANDLE;
		VkDeviceSize offset  = 0;
		uint8_t*     address = nullptr;

		if (!AllocateFrameStaging(bytes, buffer, offset, address))
		{
			LogWarn("Vulkan: no staging for a blit of %llu bytes; skipping it.", bytes);
			return;
		}

		// A staging block or a buffer of its own fits a 32-bit size.
		memcpy(address, bgraPixels, static_cast<size_t>(bytes));

		EndRenderPassIfActive();
		TransitionImage(blitImage, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_ASPECT_COLOR_BIT);

		VkBufferImageCopy copy{};
		copy.bufferOffset     = offset;
		copy.imageSubresource = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1 };
		copy.imageExtent      = { sourceWidth, sourceHeight, 1 };

		vkCmdCopyBufferToImage(commandBuffer, buffer, blitImage.image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &copy);
		TransitionImage(blitImage, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, VK_IMAGE_ASPECT_COLOR_BIT);

		// Draw the quad over the destination, within the scissor of a sub-viewport
		VkRect2D scissor{};
		ViewportRectangle(scissor);

		if (scissor.extent.width == 0 || scissor.extent.height == 0)
		{
			return;
		}

		BeginRenderPassIfNeeded();

		// The render size is far below the range where a float loses whole pixels.
		float const width  = static_cast<float>(renderWidth);
		float const height = static_cast<float>(renderHeight);

		BlitConstants constants{};
		constants.rectangle[0]    = 2.0f * static_cast<float>(destinationX) / width - 1.0f;
		constants.rectangle[1]    = 2.0f * static_cast<float>(destinationY) / height - 1.0f;
		constants.rectangle[2]    = 2.0f * static_cast<float>(destinationX + destinationWidth) / width - 1.0f;
		constants.rectangle[3]    = 2.0f * static_cast<float>(destinationY + destinationHeight) / height - 1.0f;
		constants.sourceExtent[0] = static_cast<float>(sourceWidth) / static_cast<float>(blitImage.width);
		constants.sourceExtent[1] = static_cast<float>(sourceHeight) / static_cast<float>(blitImage.height);
		memcpy(constants.modulate, modulate, sizeof(constants.modulate));
		memcpy(constants.colourKey, colourKey, sizeof(constants.colourKey));
		constants.options[0]      = isSourceAlphaUsed ? 1.0f : 0.0f;

		VkViewport const viewport{ 0.0f, 0.0f, width, height, 0.0f, 1.0f };
		VkDescriptorSet const sets[] = { blitImageSet, pointClampSet };

		vkCmdBindPipeline(commandBuffer, VK_PIPELINE_BIND_POINT_GRAPHICS, blitPipelines[isBlended ? 1 : 0]);
		vkCmdBindDescriptorSets(commandBuffer, VK_PIPELINE_BIND_POINT_GRAPHICS, blitPipelineLayout, 0, _countof(sets), sets, 0, nullptr);
		vkCmdSetViewport(commandBuffer, 0, 1, &viewport);
		vkCmdSetScissor(commandBuffer, 0, 1, &scissor);
		vkCmdPushConstants(commandBuffer, blitPipelineLayout, VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT, 0, sizeof(constants), &constants);
		vkCmdDraw(commandBuffer, 4, 1, 0, 0);

		// The geometry's bindings are not what is bound any more
		InvalidateBindings();
	}

	bool VulkanBackend::CanDrawShadows(void)
	{
		return !isDead && !isDeviceLost && hasShadowResources && isDepthSampleable && depthBuffer.secondView != VK_NULL_HANDLE && IsReady();
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

		// Draw them into the shadow map, cleared to the far plane
		EndRenderPassIfActive();

		VkClearValue clear{};
		clear.depthStencil.depth = 1.0f;

		VkRenderPassBeginInfo beginInformation{ VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO };
		beginInformation.renderPass        = shadowPass;
		beginInformation.framebuffer       = shadowFramebuffer;
		beginInformation.renderArea.extent = { SHADOW_MAP_SIZE, SHADOW_MAP_SIZE };
		beginInformation.clearValueCount   = 1;
		beginInformation.pClearValues      = &clear;

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
		shadowMap.layout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;

		InvalidateBindings();
		return true;
	}

	bool VulkanBackend::UploadTerrainShadowMaps(float const* ceiling, uint32_t ceilingWidth, uint32_t ceilingHeight, float const* vertices, uint32_t verticesX, uint32_t verticesZ)
	{
		if (!CanDrawShadows())
		{
			return false;
		}

		PhaseScope const recording(*this, FRAME_PHASE_RECORDING);

		VkDeviceSize const ceilingBytes = VkDeviceSize{ ceilingWidth } * ceilingHeight * sizeof(float);
		VkDeviceSize const vertexBytes  = VkDeviceSize{ verticesX } * verticesZ * sizeof(float) * 4u;

		return UploadFloatTexture(terrainCeilingImage, VK_FORMAT_R32_SFLOAT, ceilingWidth, ceilingHeight, ceiling, ceilingBytes) && UploadFloatTexture(terrainVertexImage, VK_FORMAT_R32G32B32A32_SFLOAT, verticesX, verticesZ, vertices, vertexBytes);
	}

	void VulkanBackend::CompositeShadows(ShadowCompositeConstants const& constants, int32_t const rectangle[4], bool isTerrainUsed)
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

		// Give a shadow map no caster has drawn into a defined layout, though nothing reads it
		if (shadowMap.layout == VK_IMAGE_LAYOUT_UNDEFINED)
		{
			EndRenderPassIfActive();
			TransitionImage(shadowMap, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, VK_IMAGE_ASPECT_DEPTH_BIT);
		}

		// Bind the scene depth, the shadow map and the terrain maps
		//
		// Without the terrain maps the shadow map stands in for them: every binding has to
		// name an image, and the shader leaves them unread.
		bool const hasTerrain = isTerrainUsed && terrainCeilingImage.layout == VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL && terrainVertexImage.layout == VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;

		VkDescriptorSet set = VK_NULL_HANDLE;
		if (!AllocateTransientSet(compositeSetLayout, set))
		{
			return;
		}

		VkDescriptorBufferInfo bufferInformation{ uniformBuffer, uniformOffset, sizeof(constants) };

		VkDescriptorImageInfo images[4]{};
		images[0] = { pointClampSampler, depthBuffer.secondView, VK_IMAGE_LAYOUT_DEPTH_STENCIL_READ_ONLY_OPTIMAL };
		images[1] = { pointClampSampler, shadowMap.view, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL };
		images[2] = { pointClampSampler, hasTerrain ? terrainCeilingImage.view : shadowMap.view, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL };
		images[3] = { pointClampSampler, hasTerrain ? terrainVertexImage.view : shadowMap.view, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL };

		VkWriteDescriptorSet writes[5]{};
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
