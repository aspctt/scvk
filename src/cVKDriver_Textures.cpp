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
 * The texture stage combiners are the hardest part of this interface to carry over. They
 * describe a two-stage fixed function blending network - sources, operands, combine modes
 * and output scale - which Vulkan has no equivalent for at all. It is reproduced by
 * packing the network into push constants and evaluating it in the fragment shader,
 * rather than by baking it into the pipeline: the network changes far too often for that,
 * and none of it needs to be known at pipeline creation.
 *
 * The second stage runs only for geometry that carries two texture coordinate sets and
 * has a texture bound to that stage, which in practice means the terrain. Everything else
 * stays on the texture environment path, which is already correct and does not gain
 * anything from being restated in terms of combiners.
 */

//// Dependencies

#include "cVKDriver.h"
#include "Logger.h"
#include "VulkanBackend.h"

#include <cGDCombiner.h>

#include <string.h>

namespace scvk
{
	//// Constants

	namespace
	{
		// The interface's two texture stages.
		constexpr uint32_t STAGE_COUNT = 2;

		// Combiner word fields, in the layout the fragment shader reads. Source 0 is the
		// texture and source 1 is what the previous stage produced, which for the first
		// stage is the primary colour.
		constexpr uint32_t COMBINE_REPLACE     = 0;
		constexpr uint32_t COMBINE_MODULATE    = 1;
		constexpr uint32_t COMBINE_INTERPOLATE = 4;

		constexpr uint32_t ARGUMENT0_FROM_TEXTURE  = 0u << 3;
		constexpr uint32_t ARGUMENT0_FROM_PREVIOUS = 1u << 3;
		constexpr uint32_t ARGUMENT1_FROM_PREVIOUS = 1u << 8;

		// Argument 2 is the texture, read through its alpha.
		constexpr uint32_t ARGUMENT2_TEXTURE_ALPHA = (0u << 13) | (2u << 15);

		constexpr float IDENTITY_MATRIX[16] = {
			1, 0, 0, 0,
			0, 1, 0, 0,
			0, 0, 1, 0,
			0, 0, 0, 1,
		};
	}

	//// Private Functions

	namespace
	{
		/**
		 * Packs one channel of a combiner into the word the shader reads.
		 *
		 * Layout, low bits first: the combine mode, then three source and operand pairs,
		 * then the output scale.
		 *
		 * The operand arrives as an eGDBlend, whose useful values here are SrcColor,
		 * OneMinusSrcColor, SrcAlpha and OneMinusSrcAlpha. Those are numbered 2 to 5 in
		 * that enumeration and 0 to 3 in the shader, and the game leaves the field at
		 * zero when it means the default, which is the plain colour. Both readings land
		 * on 0, so a subtraction that would underflow is simply clamped rather than
		 * special cased.
		 */
		uint32_t PackCombinerChannel(uint8_t mode, cGDCombiner::ParamOperandPair const* parameters, uint8_t scale)
		{
			uint32_t packed = mode & 7u;

			for (uint32_t i = 0; i < 3; i++)
			{
				uint32_t const source = parameters[i].SourceType & 3u;

				uint32_t operand = parameters[i].OperandType;
				operand = (operand >= 2u) ? (operand - 2u) : 0u;
				operand &= 7u;

				packed |= source << (3u + i * 5u);
				packed |= operand << (5u + i * 5u);
			}

			packed |= (scale & 3u) << 18u;
			return packed;
		}

		/**
		 * Expresses a texture environment mode as a combiner word.
		 *
		 * The combiner network is only consulted when the environment mode selects
		 * Combine, exactly as the fixed function pipeline defines it. SimCity 4 sets
		 * combiners on both stages but never asks for Combine, so the network it uploads
		 * is inert state, and taking it at face value paints the terrain in the
		 * environment colour it never set: a flat dark navy where the ground should be.
		 *
		 * Translating the mode into the same encoding keeps one path in the shader rather
		 * than two.
		 */
		uint32_t SynthesiseEnvironmentCombiner(int32_t environmentMode, bool isAlphaChannel)
		{
			switch (environmentMode)
			{
			case kGDTextureEnvParam_Replace:
				return COMBINE_REPLACE | ARGUMENT0_FROM_TEXTURE;

			case kGDTextureEnvParam_Decal:
				// Colour is interpolated between what came before and the texture, using
				// the texture's own alpha as the weight, and the alpha passes through
				// untouched. Replace reads argument 0, so passing the previous value
				// through needs it there rather than in argument 1.
				if (isAlphaChannel)
				{
					return COMBINE_REPLACE | ARGUMENT0_FROM_PREVIOUS;
				}

				return COMBINE_INTERPOLATE | ARGUMENT0_FROM_TEXTURE | ARGUMENT1_FROM_PREVIOUS | ARGUMENT2_TEXTURE_ALPHA;

			// Blend needs the environment colour, which the game has not been seen
			// setting, so it stays on the default rather than being invented.
			case kGDTextureEnvParam_Modulate:
			default:
				return COMBINE_MODULATE | ARGUMENT0_FROM_TEXTURE | ARGUMENT1_FROM_PREVIOUS;
			}
		}
	}

	void cVKDriver::SetTextureStageEnabled(bool isEnabled)
	{
		isTextureStageEnabled[activeTextureStage] = isEnabled;
		vulkan->SetTextureStageEnabled(activeTextureStage, isEnabled);
	}

	void cVKDriver::PushCombinerState(void)
	{
		for (uint32_t stage = 0; stage < STAGE_COUNT; stage++)
		{
			// Take the network only when the mode asks for it
			bool const isCombining = textureEnvironmentMode[stage] == kGDTextureEnvParam_Combine || textureEnvironmentMode[stage] == kGDTextureEnvParam_Combine4;

			uint32_t const rgb   = isCombining ? rawCombiner[stage * 2 + 0] : SynthesiseEnvironmentCombiner(textureEnvironmentMode[stage], false);
			uint32_t const alpha = isCombining ? rawCombiner[stage * 2 + 1] : SynthesiseEnvironmentCombiner(textureEnvironmentMode[stage], true);

			// Keep it for the diagnostics and send it
			packedCombiner[stage * 2 + 0] = rgb;
			packedCombiner[stage * 2 + 1] = alpha;

			vulkan->SetCombinerState(stage, rgb, alpha);
		}
	}

	//// Public API

	void cVKDriver::BindTexture(uint32_t gdTextureTarget, uint32_t texture)
	{
		SCVK_CALL("%u, %u", gdTextureTarget, texture);
	}

	void cVKDriver::TexImage2D(uint32_t gdTextureTarget, int32_t level, int32_t gdInternalTextureFormat, int32_t width, int32_t height, int32_t border, uint32_t gdTextureFormat, uint32_t gdType, void const* pixels)
	{
		SCVK_CALL("%u, %d, %d, %dx%d, border %d, fmt %u, type %u, %p", gdTextureTarget, level, gdInternalTextureFormat, width, height, border, gdTextureFormat, gdType, pixels);
	}

	void cVKDriver::PixelStore(uint32_t gdParameter, int32_t parameter)
	{
		SCVK_CALL("%u, %d", gdParameter, parameter);
	}

	void cVKDriver::TexEnv(uint32_t gdTextureEnvironmentTarget, uint32_t gdTextureEnvironmentParameterType, int32_t gdTextureEnvironmentMode)
	{
		SCVK_CALL("%u, %u, %d", gdTextureEnvironmentTarget, gdTextureEnvironmentParameterType, gdTextureEnvironmentMode);

		// Report each distinct call once. The mode is masked to a byte, so never
		// negative.
		uint32_t const modeByte = static_cast<uint32_t>(gdTextureEnvironmentMode & 0xff);

		if (NoteOnce(NOTE_TEXTURE_ENVIRONMENT, gdTextureEnvironmentTarget | (gdTextureEnvironmentParameterType << 8) | (modeByte << 16)))
		{
			LogNote("  TEXENV target %u, param type %u, mode %d", gdTextureEnvironmentTarget, gdTextureEnvironmentParameterType, gdTextureEnvironmentMode);
		}

		// Only the mode matters, and it applies to the active stage
		//
		// Parameter type 0 is the mode, whose values start Replace, Modulate. The target
		// is not a stage index. It is the equivalent of OpenGL's GL_TEXTURE_ENV and is
		// always zero; the stage is whichever one TexStage last selected. Indexing by the
		// target instead put every mode on stage 0, including the ones meant for stage 1,
		// which turned the whole city white once decal was implemented.
		if (gdTextureEnvironmentParameterType != kGDTextureEnvParamType_Mode)
		{
			return;
		}

		textureEnvironmentMode[activeTextureStage] = gdTextureEnvironmentMode;
		PushCombinerState();

		// The backend treats anything outside its three modes as modulate, so a value
		// that wraps around when made unsigned is harmless.
		if (activeTextureStage == 0)
		{
			vulkan->SetTextureEnvironmentMode(static_cast<uint32_t>(gdTextureEnvironmentMode));
		}
	}

	void cVKDriver::TexEnv(uint32_t gdTextureEnvironmentTarget, uint32_t gdTextureEnvironmentParameterType, float const* parameters)
	{
		SCVK_CALL("%u, %u, %p", gdTextureEnvironmentTarget, gdTextureEnvironmentParameterType, parameters);

		// The environment colour, which a combiner may name as a source. One value is
		// kept rather than one per stage, because the game has not been seen setting it
		// per stage and the shader has room for one.
		if (gdTextureEnvironmentParameterType != kGDTextureEnvParamType_Color || parameters == nullptr)
		{
			return;
		}

		vulkan->SetConstantColour(parameters[0], parameters[1], parameters[2], parameters[3]);
	}

	void cVKDriver::TexParameter(uint32_t gdTextureTarget, uint32_t gdTextureParameterType, int32_t gdTextureParameter)
	{
		SCVK_CALL("%u, %u, %d", gdTextureTarget, gdTextureParameterType, gdTextureParameter);

		// Report each distinct call once. The value is masked to 16 bits, so never
		// negative.
		uint32_t const valueBits = static_cast<uint32_t>(gdTextureParameter & 0xffff);

		if (NoteOnce(NOTE_TEXTURE_PARAMETER, gdTextureTarget | (gdTextureParameterType << 8) | (valueBits << 16)))
		{
			LogNote("  TEXPARAM target %u, param type %u, value %d", gdTextureTarget, gdTextureParameterType, gdTextureParameter);
		}

		// Set it on the texture bound to the selected stage
		//
		// Parameter type 0 is the magnification filter, 1 the minification filter, 2 the
		// wrap in S and 3 the wrap in T. The game sets both clamp and repeat. The value
		// is not negative, checked here.
		//
		// The texture keeps it, as an OpenGL texture object does. The game relies on
		// that: it sets clamp after binding an interface texture but never sets repeat
		// back on the terrain textures. Applying the latest values to whatever was drawn
		// next instead left the terrain clamped whenever it was redrawn straight after the
		// interface, and a clamped grass layer samples the transparent border, which was
		// the black patches.
		if (gdTextureParameterType >= 4 || gdTextureParameter < 0)
		{
			return;
		}

		uint32_t const texture = (activeTextureStage == 1) ? stage1Texture : boundTexture;
		vulkan->SetTextureParameter(texture, gdTextureParameterType, static_cast<uint32_t>(gdTextureParameter));
	}

	void cVKDriver::GenTextures(int32_t count, uint32_t* textures)
	{
		SCVK_CALL("%d, %p", count, textures);

		if (textures == nullptr || count <= 0)
		{
			SetLastError(DriverError::INVALID_VALUE);
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

		// These are the handles CreateTexture returned. The interface also has
		// GenTextures, which hands out names from a separate space, but the game never
		// calls it, so there is only one kind of value arriving here.
		if (textures == nullptr || count <= 0)
		{
			return;
		}

		for (int32_t i = 0; i < count; i++)
		{
			vulkan->DestroyTexture(textures[i]);
		}
	}

	bool cVKDriver::IsTexture(uint32_t texture)
	{
		SCVK_CALL("%u", texture);

		// Any name GenTextures has issued.
		return texture != 0 && texture < nextTextureName;
	}

	void cVKDriver::PrioritizeTextures(int32_t count, uint32_t const* textures, float const* priorities)
	{
		SCVK_CALL("%d, %p, %p", count, textures, priorities);
	}

	bool cVKDriver::AreTexturesResident(int32_t count, uint32_t const* textures, bool* residences)
	{
		SCVK_CALL("%d, %p, %p", count, textures, residences);

		if (residences == nullptr)
		{
			return true;
		}

		for (int32_t i = 0; i < count; i++)
		{
			residences[i] = true;
		}

		return true;
	}

	void cVKDriver::TexStage(uint32_t textureUnit)
	{
		SCVK_CALL("%u", textureUnit);

		// Selects which stage the texture enable, the combiner and the stage matrix calls
		// that follow are talking about.
		activeTextureStage = (textureUnit < STAGE_COUNT) ? textureUnit : STAGE_COUNT - 1;
	}

	void cVKDriver::TexStageCoord(uint32_t gdTextureCoordinateSource)
	{
		SCVK_CALL("%u", gdTextureCoordinateSource);

		// Which coordinate set, or generated source, feeds the active stage. The first
		// stage's source decides whether the draw generates its coordinates, which is
		// what the cloud shadows do.
		textureCoordinateSource[activeTextureStage] = gdTextureCoordinateSource;

		if (NoteOnce(NOTE_COORDINATE_SOURCE, activeTextureStage | (gdTextureCoordinateSource << 8)))
		{
			LogNote("  TEXCOORDSRC stage %u source %u", activeTextureStage, gdTextureCoordinateSource);
		}
	}

	void cVKDriver::TexStageMatrix(float const* matrix, uint32_t unknown0, uint32_t unknown1, uint32_t gdTextureMatrixFlags)
	{
		SCVK_CALL("%p, %u, %u, 0x%x", matrix, unknown0, unknown1, gdTextureMatrixFlags);

		lastTextureMatrixFlags = gdTextureMatrixFlags;

		// Keep the first stage's matrix
		//
		// That is the one that generates coordinates. A null matrix means identity. The
		// flag cases the OpenGL driver distinguishes all rewrite rows 2 and 3 of the
		// matrix and leave rows 0 and 1 alone. A 2D sample uses only those first two
		// rows, so none of that distinction reaches here.
		if (activeTextureStage == 0)
		{
			memcpy(textureStageMatrix, (matrix != nullptr) ? matrix : IDENTITY_MATRIX, sizeof(textureStageMatrix));
		}

		// Report the first few matrices
		//
		// The game drives this over a million times a session, and a texture matrix that
		// scales texture coordinates would magnify whatever it samples, which was one of
		// the candidate explanations for the picture being too large.
		if (textureMatrixProbesRemaining <= 0 || matrix == nullptr)
		{
			return;
		}

		textureMatrixProbesRemaining--;
		LogNote("    texture matrix flags 0x%x: [%.3f %.3f %.3f %.3f] [%.3f %.3f %.3f %.3f] [%.3f %.3f %.3f %.3f] [%.3f %.3f %.3f %.3f]", gdTextureMatrixFlags, matrix[0], matrix[1], matrix[2], matrix[3], matrix[4], matrix[5], matrix[6], matrix[7], matrix[8], matrix[9], matrix[10], matrix[11], matrix[12], matrix[13], matrix[14], matrix[15]);
	}

	/*
	 * The TexStageCombine overloads only trace. Their parameters are scoped enumerations,
	 * which are not promoted when passed to a variadic function, so each is converted to
	 * the number it stands for.
	 */

	void cVKDriver::TexStageCombine(eGDTextureStageCombineParamType gdParameterType, eGDTextureStageCombineModeParam gdParameter)
	{
		SCVK_CALL("mode: %d, %d", static_cast<int>(gdParameterType), static_cast<int>(gdParameter));
	}

	void cVKDriver::TexStageCombine(eGDTextureStageCombineSourceParamType gdParameterType, eGDTextureStageCombineSourceParam gdParameter)
	{
		SCVK_CALL("source: %d, %d", static_cast<int>(gdParameterType), static_cast<int>(gdParameter));
	}

	void cVKDriver::TexStageCombine(eGDTextureStageCombineOperandType gdParameterType, eGDBlend gdBlend)
	{
		SCVK_CALL("operand: %d, %d", static_cast<int>(gdParameterType), static_cast<int>(gdBlend));
	}

	void cVKDriver::TexStageCombine(eGDTextureStageCombineScaleParamType gdParameterType, eGDTextureStageCombineScaleParam gdParameter)
	{
		SCVK_CALL("scale: %d, %d", static_cast<int>(gdParameterType), static_cast<int>(gdParameter));
	}

	void cVKDriver::SetTexture(uint32_t texture, uint32_t textureUnit)
	{
		SCVK_CALL("%u, %u", texture, textureUnit);

		// Report the stage bindings
		//
		// Whether the second unit is ever given a texture, and what it is paired with,
		// decides how much of the combiner network matters. A handful of the textures the
		// second stage is given are described in full: the terrain once sampled black
		// there, and whether the level the sampler reaches was ever filled was the first
		// thing to rule out.
		if (NoteOnce(NOTE_STAGE_BINDING, textureUnit | (texture != 0 ? 0x100u : 0u)))
		{
			LogNote("  STAGE unit %u %s (texture %u)", textureUnit, texture != 0 ? "bound" : "cleared", texture);
		}

		if (textureUnit == 1 && texture != 0 && NoteOnce(NOTE_STAGE1_TEXTURE, texture))
		{
			vulkan->LogTextureInformation(texture, "stage 1");
		}

		// Bind it to its stage
		if (textureUnit == 0)
		{
			boundTexture = texture;
			vulkan->SetTexture(texture);
		}
		else if (textureUnit == 1)
		{
			stage1Texture = texture;
			vulkan->SetTexture1(texture);
		}
	}

	intptr_t cVKDriver::GetTexture(uint32_t textureUnit)
	{
		SCVK_CALL("%u", textureUnit);

		// Handles are small positive numbers, so they fit the signed type the interface
		// returns.
		return (textureUnit == 0) ? static_cast<intptr_t>(boundTexture) : 0;
	}

	intptr_t cVKDriver::CreateTexture(uint32_t gdInternalTextureFormat, uint32_t width, uint32_t height, uint32_t levels, uint32_t gdTextureHintFlags)
	{
		SCVK_CALL("fmt %u, %ux%u, %u levels, hints 0x%x", gdInternalTextureFormat, width, height, levels, gdTextureHintFlags);

		// Must be non-zero: the game tests the result before using it. Handles are small
		// positive numbers, so they fit the signed type the interface returns.
		return static_cast<intptr_t>(vulkan->CreateTexture(gdInternalTextureFormat, width, height, levels));
	}

	void cVKDriver::LoadTextureLevel(uint32_t texture, int32_t level, int32_t offsetX, int32_t offsetY, int32_t width, int32_t height, uint32_t gdTextureFormat, uint32_t gdType, uint32_t rowLength, void const* pixels)
	{
		SCVK_CALL("%u, level %d, +%d+%d, %dx%d, fmt %u, type %u, row %u, %p", texture, level, offsetX, offsetY, width, height, gdTextureFormat, gdType, rowLength, pixels);

		if (level < 0 || width <= 0 || height <= 0)
		{
			return;
		}

		// None of the three is negative, checked just above.
		vulkan->UploadTextureLevel(texture, static_cast<uint32_t>(level), offsetX, offsetY, static_cast<uint32_t>(width), static_cast<uint32_t>(height), gdTextureFormat, gdType, rowLength, pixels);
	}

	void cVKDriver::SetCombiner(cGDCombiner const& combiner, uint32_t textureUnit)
	{
		// Logged in full because this state is the main input to the fragment shader's
		// combiner path.
		SCVK_CALL("unit %u, rgbMode %u scale %u, alphaMode %u scale %u", textureUnit, combiner.RGBCombineMode, combiner.RGBScale, combiner.AlphaCombineMode, combiner.AlphaScale);

		// Key the whole configuration by value
		//
		// So each distinct one is described exactly once however late in the session it
		// first appears.
		uint32_t key = textureUnit | (uint32_t{ combiner.RGBCombineMode } << 4) | (uint32_t{ combiner.AlphaCombineMode } << 8) | (uint32_t{ combiner.RGBScale } << 12) | (uint32_t{ combiner.AlphaScale } << 14);

		for (uint32_t i = 0; i < 3; i++)
		{
			key ^= (uint32_t{ combiner.RGBParams[i].SourceType } << (16 + i * 2)) ^ (uint32_t{ combiner.RGBParams[i].OperandType } << (22 + i * 2)) ^ (uint32_t{ combiner.AlphaParams[i].SourceType } << (26 + i)) ^ (uint32_t{ combiner.AlphaParams[i].OperandType } << (29 + i));
		}

		// Pack and push it for the stage
		if (textureUnit < STAGE_COUNT)
		{
			rawCombiner[textureUnit * 2 + 0] = PackCombinerChannel(combiner.RGBCombineMode, combiner.RGBParams, combiner.RGBScale);
			rawCombiner[textureUnit * 2 + 1] = PackCombinerChannel(combiner.AlphaCombineMode, combiner.AlphaParams, combiner.AlphaScale);

			PushCombinerState();
		}

		// Describe it the first time it is seen
		if (NoteOnce(NOTE_COMBINER, key))
		{
			LogNote("  COMBINER unit %u: rgb mode %u scale %u  src/op (%u,%u) (%u,%u) (%u,%u) | alpha mode %u scale %u  src/op (%u,%u) (%u,%u) (%u,%u)", textureUnit, combiner.RGBCombineMode, combiner.RGBScale, combiner.RGBParams[0].SourceType, combiner.RGBParams[0].OperandType, combiner.RGBParams[1].SourceType, combiner.RGBParams[1].OperandType, combiner.RGBParams[2].SourceType, combiner.RGBParams[2].OperandType, combiner.AlphaCombineMode, combiner.AlphaScale, combiner.AlphaParams[0].SourceType, combiner.AlphaParams[0].OperandType, combiner.AlphaParams[1].SourceType, combiner.AlphaParams[1].OperandType, combiner.AlphaParams[2].SourceType, combiner.AlphaParams[2].OperandType);
		}
	}
}
