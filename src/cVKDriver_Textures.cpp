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
 * Textures and texture stages.
 *
 * The texture stage combiners are the hardest part of this interface to carry
 * over. They describe a two-stage fixed function blending network - sources,
 * operands, combine modes and output scale - which Vulkan has no equivalent
 * for at all. It is reproduced by packing the network into push constants and
 * evaluating it in the fragment shader, rather than by baking it into the
 * pipeline: the network changes far too often for that, and none of it needs
 * to be known at pipeline creation.
 *
 * The second stage runs only for geometry that carries two texture coordinate
 * sets and has a texture bound to that stage, which in practice means the
 * terrain. Everything else stays on the texture environment path, which is
 * already correct and does not gain anything from being restated in terms of
 * combiners.
 *
 * Texture names are handed out for real here even though nothing backs them,
 * because the game stores what it is given and passes it back.
 */

#include "cVKDriver.h"
#include "Logger.h"
#include "VulkanBackend.h"

#include <cGDCombiner.h>

namespace scvk
{
	namespace
	{
		/**
		 * Packs one channel of a combiner into the word the shader reads.
		 *
		 * Layout, low bits first: the combine mode, then three source and
		 * operand pairs, then the output scale.
		 *
		 * The operand arrives as an eGDBlend, whose useful values here are
		 * SrcColor, OneMinusSrcColor, SrcAlpha and OneMinusSrcAlpha. Those are
		 * numbered 2 to 5 in that enumeration and 0 to 3 in the shader, and the
		 * game leaves the field at zero when it means the default, which is the
		 * plain colour. Both readings land on 0, so a subtraction that would
		 * underflow is simply clamped rather than special cased.
		 */
		uint32_t PackCombinerChannel(uint8_t mode, cGDCombiner::ParamOperandPair const* params, uint8_t scale)
		{
			uint32_t packed = static_cast<uint32_t>(mode) & 7u;

			for (uint32_t i = 0; i < 3; i++)
			{
				uint32_t const source = static_cast<uint32_t>(params[i].SourceType) & 3u;

				uint32_t operand = static_cast<uint32_t>(params[i].OperandType);
				operand = (operand >= 2u) ? (operand - 2u) : 0u;
				operand &= 7u;

				packed |= source  << (3u + i * 5u);
				packed |= operand << (5u + i * 5u);
			}

			packed |= (static_cast<uint32_t>(scale) & 3u) << 18u;
			return packed;
		}

		/**
		 * Expresses a texture environment mode as a combiner word.
		 *
		 * The combiner network is only consulted when the environment mode
		 * selects Combine, exactly as the fixed function pipeline defines it.
		 * SimCity 4 sets combiners on both stages but never asks for Combine,
		 * so the network it uploads is inert state, and taking it at face value
		 * paints the terrain in the environment colour it never set: a flat
		 * dark navy where the ground should be.
		 *
		 * Translating the mode into the same encoding keeps one path in the
		 * shader rather than two.
		 */
		uint32_t SynthesiseEnvCombiner(int32_t envMode)
		{
			// Source 0 is the texture and source 1 is what the previous stage
			// produced, which for the first stage is the primary colour.
			constexpr uint32_t kFromTexture  = 0u << 3;
			constexpr uint32_t kFromPrevious = 1u << 8;

			switch (envMode)
			{
			case kGDTextureEnvParam_Replace:
				return 0u | kFromTexture;

			// Decal and Blend need the environment colour and an interpolation
			// against the texture alpha. Neither has been seen from the game,
			// and guessing at them would repeat the mistake above, so they fall
			// through to the default rather than being invented.
			case kGDTextureEnvParam_Modulate:
			default:
				return 1u | kFromTexture | kFromPrevious;
			}
		}
	}

	void cVKDriver::BindTexture(uint32_t gdTextureTarget, uint32_t texture)
	{
		SCVK_CALL("%u, %u", gdTextureTarget, texture);
	}

	void cVKDriver::TexImage2D(uint32_t gdTextureTarget, int32_t level, int32_t gdInternalTexFormat, int32_t width, int32_t height, int32_t border, uint32_t gdTexFormat, uint32_t gdType, void const* pixels)
	{
		SCVK_CALL("%u, %d, %d, %dx%d, border %d, fmt %u, type %u, %p",
			gdTextureTarget, level, gdInternalTexFormat, width, height, border, gdTexFormat, gdType, pixels);
	}

	void cVKDriver::PixelStore(uint32_t gdParameter, int32_t param)
	{
		SCVK_CALL("%u, %d", gdParameter, param);
	}

	void cVKDriver::TexEnv(uint32_t gdTextureEnvTarget, uint32_t gdTextureEnvParamType, int32_t gdTextureEnvModeParam)
	{
		SCVK_CALL("%u, %u, %d", gdTextureEnvTarget, gdTextureEnvParamType, gdTextureEnvModeParam);

		if (NoteOnce(3, gdTextureEnvTarget | (gdTextureEnvParamType << 8) |
				(static_cast<uint32_t>(gdTextureEnvModeParam & 0xff) << 16)))
		{
			LogNote("  TEXENV target %u, param type %u, mode %d",
				gdTextureEnvTarget, gdTextureEnvParamType, gdTextureEnvModeParam);
		}

		// Parameter type 0 is the mode, whose values start Replace, Modulate.
		if (gdTextureEnvParamType == kGDTextureEnvParamType_Mode && gdTextureEnvTarget < 2)
		{
			texEnvMode[gdTextureEnvTarget] = gdTextureEnvModeParam;
			PushCombinerState();

			if (gdTextureEnvTarget == 0)
			{
				vulkan->SetTextureReplace(gdTextureEnvModeParam == kGDTextureEnvParam_Replace);
			}
		}
	}

	void cVKDriver::PushCombinerState(void)
	{
		for (uint32_t stage = 0; stage < 2; stage++)
		{
			bool const combining = texEnvMode[stage] == kGDTextureEnvParam_Combine
				|| texEnvMode[stage] == kGDTextureEnvParam_Combine4;

			uint32_t const rgb = combining
				? rawCombiner[stage * 2 + 0]
				: SynthesiseEnvCombiner(texEnvMode[stage]);
			uint32_t const alpha = combining
				? rawCombiner[stage * 2 + 1]
				: SynthesiseEnvCombiner(texEnvMode[stage]);

			packedCombiner[stage * 2 + 0] = rgb;
			packedCombiner[stage * 2 + 1] = alpha;

			vulkan->SetCombinerState(stage, rgb, alpha);
		}
	}

	void cVKDriver::TexEnv(uint32_t gdTextureEnvTarget, uint32_t gdTextureEnvParamType, float const* params)
	{
		SCVK_CALL("%u, %u, %p", gdTextureEnvTarget, gdTextureEnvParamType, params);

		// The environment colour, which a combiner may name as a source. One
		// value is kept rather than one per stage, because the game has not
		// been seen setting it per stage and the shader has room for one.
		if (gdTextureEnvParamType == kGDTextureEnvParamType_Color && params != nullptr)
		{
			vulkan->SetConstantColour(params[0], params[1], params[2], params[3]);
		}
	}

	void cVKDriver::TexParameter(uint32_t gdTextureTarget, uint32_t gdTextureParamType, int32_t gdTextureParam)
	{
		SCVK_CALL("%u, %u, %d", gdTextureTarget, gdTextureParamType, gdTextureParam);

		// Filters, wrap modes and the level clamp all arrive here, and all of
		// them are currently ignored. Which of them the game actually sets
		// decides how much that costs.
		if (NoteOnce(6, gdTextureTarget | (gdTextureParamType << 8) |
				(static_cast<uint32_t>(gdTextureParam & 0xffff) << 16)))
		{
			LogNote("  TEXPARAM target %u, param type %u, value %d",
				gdTextureTarget, gdTextureParamType, gdTextureParam);
		}
	}

	void cVKDriver::GenTextures(int32_t count, uint32_t* textures)
	{
		SCVK_CALL("%d, %p", count, textures);

		if (textures == nullptr || count <= 0)
		{
			SetLastError(DriverError::InvalidValue);
			return;
		}

		for (int32_t i = 0; i < count; i++)
		{
			textures[i] = nextTextureName++;
		}
	}

	void cVKDriver::DeleteTextures(int32_t count, uint32_t const* textures)
	{
		SCVK_CALL("%d, %p", count, textures);
	}

	bool cVKDriver::IsTexture(uint32_t texture)
	{
		SCVK_CALL("%u", texture);

		// Any name we have issued and not reused. Good enough while nothing is
		// actually backing them.
		return texture != 0 && texture < nextTextureName;
	}

	void cVKDriver::PrioritizeTextures(int32_t count, uint32_t const* textures, float const* priorities)
	{
		SCVK_CALL("%d, %p, %p", count, textures, priorities);
	}

	bool cVKDriver::AreTexturesResident(int32_t count, uint32_t const* textures, bool* residences)
	{
		SCVK_CALL("%d, %p, %p", count, textures, residences);

		if (residences != nullptr)
		{
			for (int32_t i = 0; i < count; i++)
			{
				residences[i] = true;
			}
		}

		return true;
	}

	void cVKDriver::TexStage(uint32_t texUnit)
	{
		SCVK_CALL("%u", texUnit);

		// Selects which stage the texture enable, the combiner and the stage
		// matrix calls that follow are talking about.
		activeTexStage = (texUnit < 2) ? texUnit : 1;
	}

	void cVKDriver::SetTextureStageEnabled(bool enabled)
	{
		texStageEnabled[activeTexStage] = enabled;
		vulkan->SetTextureStageEnabled(activeTexStage, enabled);
	}

	void cVKDriver::TexStageCoord(uint32_t gdTexCoordSource)
	{
		SCVK_CALL("%u", gdTexCoordSource);
	}

	void cVKDriver::TexStageMatrix(float const* matrix, uint32_t unknown0, uint32_t unknown1, uint32_t gdTexMatFlags)
	{
		SCVK_CALL("%p, %u, %u, 0x%x", matrix, unknown0, unknown1, gdTexMatFlags);

		lastTexMatrixFlags = gdTexMatFlags;

		// Logged once per distinct flag value. The game drives this over a
		// million times a session, and a texture matrix that scales texture
		// coordinates would magnify whatever it samples, which is one of the
		// candidate explanations for the picture being too large.
		if (texMatrixProbesRemaining > 0 && matrix != nullptr)
		{
			texMatrixProbesRemaining--;
			LogNote("    texture matrix flags 0x%x: [%.3f %.3f %.3f %.3f] [%.3f %.3f %.3f %.3f] [%.3f %.3f %.3f %.3f] [%.3f %.3f %.3f %.3f]",
				gdTexMatFlags,
				matrix[0], matrix[1], matrix[2], matrix[3],
				matrix[4], matrix[5], matrix[6], matrix[7],
				matrix[8], matrix[9], matrix[10], matrix[11],
				matrix[12], matrix[13], matrix[14], matrix[15]);
		}
	}

	void cVKDriver::TexStageCombine(eGDTextureStageCombineParamType gdParamType, eGDTextureStageCombineModeParam gdParam)
	{
		SCVK_CALL("mode: %d, %d", static_cast<int>(gdParamType), static_cast<int>(gdParam));
	}

	void cVKDriver::TexStageCombine(eGDTextureStageCombineSourceParamType gdParamType, eGDTextureStageCombineSourceParam gdParam)
	{
		SCVK_CALL("source: %d, %d", static_cast<int>(gdParamType), static_cast<int>(gdParam));
	}

	void cVKDriver::TexStageCombine(eGDTextureStageCombineOperandType gdParamType, eGDBlend gdBlend)
	{
		SCVK_CALL("operand: %d, %d", static_cast<int>(gdParamType), static_cast<int>(gdBlend));
	}

	void cVKDriver::TexStageCombine(eGDTextureStageCombineScaleParamType gdParamType, eGDTextureStageCombineScaleParam gdParam)
	{
		SCVK_CALL("scale: %d, %d", static_cast<int>(gdParamType), static_cast<int>(gdParam));
	}

	void cVKDriver::SetTexture(uint32_t texture, uint32_t texUnit)
	{
		SCVK_CALL("%u, %u", texture, texUnit);

		// Whether the second unit is ever given a texture, and what it is
		// paired with, decides how much of the combiner network matters.
		if (NoteOnce(2, texUnit | (texture != 0 ? 0x100u : 0u)))
		{
			LogNote("  STAGE unit %u %s (texture %u)",
				texUnit, texture != 0 ? "bound" : "cleared", texture);
		}

		// A handful of the textures the second stage is given, described in
		// full. The terrain samples black there, and whether the level the
		// sampler reaches was ever filled is the first thing to rule out.
		if (texUnit == 1 && texture != 0 && NoteOnce(7, texture))
		{
			vulkan->LogTextureInfo(texture, "stage 1");
		}

		if (texUnit == 0)
		{
			boundTexture = texture;
			vulkan->SetTexture(texture);
		}
		else if (texUnit == 1)
		{
			stage1Texture = texture;
			vulkan->SetTexture1(texture);
		}
	}

	intptr_t cVKDriver::GetTexture(uint32_t texUnit)
	{
		SCVK_CALL("%u", texUnit);
		return (texUnit == 0) ? static_cast<intptr_t>(boundTexture) : 0;
	}

	intptr_t cVKDriver::CreateTexture(uint32_t gdInternalTexFormat, uint32_t width, uint32_t height, uint32_t levels, uint32_t gdTexHintFlags)
	{
		SCVK_CALL("fmt %u, %ux%u, %u levels, hints 0x%x", gdInternalTexFormat, width, height, levels, gdTexHintFlags);

		// Must be non-zero: the game tests the result before using it.
		return static_cast<intptr_t>(vulkan->CreateTexture(gdInternalTexFormat, width, height, levels));
	}

	void cVKDriver::LoadTextureLevel(uint32_t texture, int32_t level, int32_t xoffset, int32_t yoffset, int32_t width, int32_t height, uint32_t gdTexFormat, uint32_t gdType, uint32_t rowLength, void const* pixels)
	{
		SCVK_CALL("%u, level %d, +%d+%d, %dx%d, fmt %u, type %u, row %u, %p",
			texture, level, xoffset, yoffset, width, height, gdTexFormat, gdType, rowLength, pixels);

		if (level < 0 || width <= 0 || height <= 0)
		{
			return;
		}

		vulkan->UploadTextureLevel(texture, static_cast<uint32_t>(level), xoffset, yoffset,
			static_cast<uint32_t>(width), static_cast<uint32_t>(height),
			gdTexFormat, gdType, rowLength, pixels);
	}

	void cVKDriver::SetCombiner(cGDCombiner const& combiner, uint32_t texUnit)
	{
		// Logged in full because this state is the main input to the fragment
		// shader that will eventually replace the fixed function combiners.
		SCVK_CALL("unit %u, rgbMode %u scale %u, alphaMode %u scale %u",
			texUnit,
			combiner.RGBCombineMode,
			combiner.RGBScale,
			combiner.AlphaCombineMode,
			combiner.AlphaScale);

		// The whole configuration, keyed by value, so each distinct one is
		// described exactly once however late in the session it first appears.
		uint32_t key = texUnit
			| (static_cast<uint32_t>(combiner.RGBCombineMode)   << 4)
			| (static_cast<uint32_t>(combiner.AlphaCombineMode) << 8)
			| (static_cast<uint32_t>(combiner.RGBScale)         << 12)
			| (static_cast<uint32_t>(combiner.AlphaScale)       << 14);

		for (int i = 0; i < 3; i++)
		{
			key ^= (static_cast<uint32_t>(combiner.RGBParams[i].SourceType)    << (16 + i * 2))
			     ^ (static_cast<uint32_t>(combiner.RGBParams[i].OperandType)   << (22 + i * 2))
			     ^ (static_cast<uint32_t>(combiner.AlphaParams[i].SourceType)  << (26 + i))
			     ^ (static_cast<uint32_t>(combiner.AlphaParams[i].OperandType) << (29 + i));
		}

		if (texUnit < 2)
		{
			rawCombiner[texUnit * 2 + 0] =
				PackCombinerChannel(combiner.RGBCombineMode, combiner.RGBParams, combiner.RGBScale);
			rawCombiner[texUnit * 2 + 1] =
				PackCombinerChannel(combiner.AlphaCombineMode, combiner.AlphaParams, combiner.AlphaScale);

			PushCombinerState();
		}

		if (NoteOnce(1, key))
		{
			LogNote("  COMBINER unit %u: rgb mode %u scale %u  src/op (%u,%u) (%u,%u) (%u,%u) | "
				"alpha mode %u scale %u  src/op (%u,%u) (%u,%u) (%u,%u)",
				texUnit,
				combiner.RGBCombineMode, combiner.RGBScale,
				combiner.RGBParams[0].SourceType, combiner.RGBParams[0].OperandType,
				combiner.RGBParams[1].SourceType, combiner.RGBParams[1].OperandType,
				combiner.RGBParams[2].SourceType, combiner.RGBParams[2].OperandType,
				combiner.AlphaCombineMode, combiner.AlphaScale,
				combiner.AlphaParams[0].SourceType, combiner.AlphaParams[0].OperandType,
				combiner.AlphaParams[1].SourceType, combiner.AlphaParams[1].OperandType,
				combiner.AlphaParams[2].SourceType, combiner.AlphaParams[2].OperandType);
		}
	}
}
