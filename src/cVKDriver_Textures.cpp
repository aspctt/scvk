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
 * has a texture bound to that stage, which in practice means the terrain. A first stage
 * given a network runs it alone when the second is off, which is how the shadows draw
 * when the graphics rules turn the second stage off. Everything else stays on the texture
 * environment path, which is already correct and does not gain anything from being
 * restated in terms of combiners.
 */

//// Dependencies

#include "cVKDriver.h"
#include "Logger.h"
#include "VulkanBackend.h"

#include <cGDCombiner.h>

#include <algorithm>
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

		// Argument 0 the environment colour, argument 2 the texture's own colour.
		constexpr uint32_t ARGUMENT0_FROM_CONSTANT  = 2u << 3;
		constexpr uint32_t ARGUMENT2_TEXTURE_COLOUR = (0u << 13) | (0u << 15);

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
		 * The operand is an index, in the order SCGL maps it: the colour, one minus the
		 * colour, the alpha, one minus the alpha. The shader numbers them the same way.
		 * An earlier version read it as an eGDBlend and subtracted 2, which turned the
		 * alpha operand the game does send into the colour.
		 */
		uint32_t PackCombinerChannel(uint8_t mode, cGDCombiner::ParamOperandPair const* parameters, uint8_t scale)
		{
			uint32_t packed = mode & 7u;

			for (uint32_t i = 0; i < 3; i++)
			{
				uint32_t const source  = parameters[i].SourceType & 3u;
				uint32_t const operand = parameters[i].OperandType & 3u;

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
		 * Combine, exactly as the fixed function pipeline defines it. Applying the last
		 * network regardless of the mode painted the terrain a flat dark navy where the
		 * ground should be.
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

			case kGDTextureEnvParam_Blend:
				// The texture's colour weighs the environment colour against what came
				// before, and the alpha is the texture's times the previous one, as OpenGL
				// defines GL_BLEND and SCD3D11 draws it, rather than as modulate.
				if (isAlphaChannel)
				{
					return COMBINE_MODULATE | ARGUMENT0_FROM_TEXTURE | ARGUMENT1_FROM_PREVIOUS;
				}

				return COMBINE_INTERPOLATE | ARGUMENT0_FROM_CONSTANT | ARGUMENT1_FROM_PREVIOUS | ARGUMENT2_TEXTURE_COLOUR;

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

		// Send the environment colour
		vulkan->SetConstantColour(environmentColour[0], environmentColour[1], environmentColour[2], environmentColour[3]);
	}

	//// Public API

	void cVKDriver::BindTexture(uint32_t gdTextureTarget, uint32_t texture)
	{
		SCVK_CALL("%u, %u", gdTextureTarget, texture);

		// Binds a name GenTextures handed out, or CreateTexture made, to the active stage,
		// as SCD3D11 does
		if (gdTextureTarget != 0 || (texture != 0 && !vulkan->IsTextureName(texture)))
		{
			SetLastError(DriverError::INVALID_VALUE);
			return;
		}

		BindStageTexture(activeTextureStage, texture);
	}

	void cVKDriver::TexImage2D(uint32_t gdTextureTarget, int32_t level, int32_t gdInternalTextureFormat, int32_t width, int32_t height, int32_t border, uint32_t gdTextureFormat, uint32_t gdType, void const* pixels)
	{
		SCVK_CALL("%u, %d, %d, %dx%d, border %d, fmt %u, type %u, %p", gdTextureTarget, level, gdInternalTextureFormat, width, height, border, gdTextureFormat, gdType, pixels);

		// Defines a level of the texture bound to the active stage, OpenGL's way of making
		// a texture, which SCD3D11 implements alongside CreateTexture
		uint32_t const texture = (activeTextureStage == 0) ? boundTexture : stage1Texture;

		if (gdTextureTarget != 0 || level < 0 || border != 0 || width <= 0 || height <= 0 || gdInternalTextureFormat < 0 || !vulkan->IsTextureName(texture))
		{
			SetLastError(DriverError::INVALID_VALUE);
			return;
		}

		// All of them are positive, checked just above.
		uint32_t const levelIndex     = static_cast<uint32_t>(level);
		uint32_t const levelWidth     = static_cast<uint32_t>(width);
		uint32_t const levelHeight    = static_cast<uint32_t>(height);
		uint32_t const internalFormat = static_cast<uint32_t>(gdInternalTextureFormat);

		if (levelIndex == 0)
		{
			// The top level gives the texture its size and format, keeping an image that
			// already has them
			if (!vulkan->DefineTexture(texture, internalFormat, levelWidth, levelHeight, 1))
			{
				SetLastError(DriverError::CREATE_CONTEXT_FAILED);
				return;
			}
		}
		else
		{
			// A smaller level has to match the top level's size halved that many times,
			// and adds every level to the image the first time one is named
			uint32_t topWidth = 0, topHeight = 0, levels = 0, uploadedLevels = 0, uploads = 0;
			if (!vulkan->DescribeTexture(texture, topWidth, topHeight, levels, uploadedLevels, uploads))
			{
				SetLastError(DriverError::INVALID_VALUE);
				return;
			}

			uint32_t maximumLevels = 1;
			for (uint32_t dimension = std::max(topWidth, topHeight); dimension > 1; dimension >>= 1)
			{
				maximumLevels++;
			}

			if (levelIndex >= maximumLevels || levelWidth != std::max(topWidth >> levelIndex, 1u) || levelHeight != std::max(topHeight >> levelIndex, 1u))
			{
				SetLastError(DriverError::INVALID_VALUE);
				return;
			}

			if (levelIndex >= levels && !vulkan->DefineTexture(texture, internalFormat, topWidth, topHeight, maximumLevels))
			{
				SetLastError(DriverError::CREATE_CONTEXT_FAILED);
				return;
			}
		}

		// Bind the image the name now has, which a reserved name did not have before
		BindStageTexture(activeTextureStage, texture);

		if (pixels != nullptr)
		{
			vulkan->UploadTextureLevel(texture, levelIndex, 0, 0, levelWidth, levelHeight, gdTextureFormat, gdType, pixelStoreRowLength, pixels);
		}
	}

	void cVKDriver::PixelStore(uint32_t gdParameter, int32_t parameter)
	{
		SCVK_CALL("%u, %d", gdParameter, parameter);

		// The one parameter is the row length, in pixels, of what TexImage2D and the
		// blits read
		if (gdParameter != 0 || parameter < 0)
		{
			SetLastError(DriverError::INVALID_VALUE);
			return;
		}

		pixelStoreRowLength = static_cast<uint32_t>(parameter);
	}

	void cVKDriver::TexEnv(uint32_t gdTextureEnvironmentTarget, uint32_t gdTextureEnvironmentParameterType, int32_t gdTextureEnvironmentMode)
	{
		SCVK_CALL("%u, %u, %d", gdTextureEnvironmentTarget, gdTextureEnvironmentParameterType, gdTextureEnvironmentMode);

		// Report each distinct call once. The mode is masked to a byte, so never
		// negative.
		uint32_t const modeByte = static_cast<uint32_t>(gdTextureEnvironmentMode & 0xff);

		if (NoteOnce(NOTE_TEXTURE_ENVIRONMENT, gdTextureEnvironmentTarget | (gdTextureEnvironmentParameterType << 8) | (modeByte << 16)))
		{
			LogDebug("  TEXENV target %u, param type %u, mode %d", gdTextureEnvironmentTarget, gdTextureEnvironmentParameterType, gdTextureEnvironmentMode);
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

		// The backend treats anything outside its four modes as modulate, so a value
		// that wraps around when made unsigned is harmless.
		if (activeTextureStage == 0)
		{
			vulkan->SetTextureEnvironmentMode(static_cast<uint32_t>(gdTextureEnvironmentMode));
		}
	}

	void cVKDriver::TexEnv(uint32_t gdTextureEnvironmentTarget, uint32_t gdTextureEnvironmentParameterType, float const* parameters)
	{
		SCVK_CALL("%u, %u, %p", gdTextureEnvironmentTarget, gdTextureEnvironmentParameterType, parameters);

		if (gdTextureEnvironmentParameterType != kGDTextureEnvParamType_Color || parameters == nullptr)
		{
			return;
		}

		// Set the environment colour every stage shares
		//
		// The DirectX driver (0x8830a0) writes it to the one texture factor whatever stage
		// is selected. The building shadows rely on that: the game fades each patch's
		// shadow by setting the colour again while the second stage is selected, and the
		// first stage reads it. Keeping a colour per stage, as OpenGL does, left the first
		// stage at full strength and drew every shadow too dark and too blue.
		memcpy(environmentColour, parameters, sizeof(environmentColour));
		PushCombinerState();
	}

	void cVKDriver::TexParameter(uint32_t gdTextureTarget, uint32_t gdTextureParameterType, int32_t gdTextureParameter)
	{
		SCVK_CALL("%u, %u, %d", gdTextureTarget, gdTextureParameterType, gdTextureParameter);

		// Report each distinct call once. The value is masked to 16 bits, so never
		// negative.
		uint32_t const valueBits = static_cast<uint32_t>(gdTextureParameter & 0xffff);

		if (NoteOnce(NOTE_TEXTURE_PARAMETER, gdTextureTarget | (gdTextureParameterType << 8) | (valueBits << 16)))
		{
			LogDebug("  TEXPARAM target %u, param type %u, value %d", gdTextureTarget, gdTextureParameterType, gdTextureParameter);
		}

		// Set it on the selected stage
		//
		// Parameter type 0 is the magnification filter, 1 the minification filter, 2 the
		// wrap in S and 3 the wrap in T. The game sets both clamp and repeat. The value
		// is not negative, checked here.
		//
		// The stage keeps it, whatever is bound there, as a Direct3D texture stage does
		// and as the game's own DirectX driver does. The game sets most parameters
		// through a cache of its own that holds filter and wrap per stage, and skips the
		// call when the stage already has the value.
		//
		// The first stage used to keep it on the bound texture instead, as an OpenGL
		// texture object does. The interface's 2D path sets clamp before binding its
		// texture, so that clamp landed on whatever was bound before, and the cache later
		// skipped setting repeat because the stage already held it. The walls of the
		// ground cut away beside sloped lots were left clamped that way, with coordinates
		// far outside 0 to 1, and sampled only the transparent border, which was black.
		// The building shadows' small mask on the second stage repeated for the same
		// reason before it moved to the stage.
		//
		// That earlier model answered black patches on the terrain, seen while every
		// parameter applied to both stages at once, so the second stage's clamps reached
		// the first. Here only the selected stage takes the value, as on DirectX.
		if (gdTextureParameterType >= 4 || gdTextureParameter < 0)
		{
			return;
		}

		vulkan->SetStageParameter(activeTextureStage, gdTextureParameterType, static_cast<uint32_t>(gdTextureParameter));
	}

	void cVKDriver::GenTextures(int32_t count, uint32_t* textures)
	{
		SCVK_CALL("%d, %p", count, textures);

		if (count < 0 || (count > 0 && textures == nullptr))
		{
			SetLastError(DriverError::INVALID_VALUE);
			return;
		}

		// Names come from the same space as CreateTexture's handles, so either kind can be
		// bound, deleted and asked about the same way
		for (int32_t i = 0; i < count; i++)
		{
			textures[i] = vulkan->ReserveTexture();
		}
	}

	void cVKDriver::DeleteTextures(int32_t count, uint32_t const* textures)
	{
		SCVK_CALL("%d, %p", count, textures);

		// The handles CreateTexture returned and the names GenTextures handed out share
		// one space
		if (count < 0 || (count > 0 && textures == nullptr))
		{
			SetLastError(DriverError::INVALID_VALUE);
			return;
		}

		for (int32_t i = 0; i < count; i++)
		{
			if (boundTexture == textures[i])
			{
				boundTexture = 0;
			}

			if (stage1Texture == textures[i])
			{
				stage1Texture = 0;
			}

			vulkan->DestroyTexture(textures[i]);
		}
	}

	bool cVKDriver::IsTexture(uint32_t texture)
	{
		SCVK_CALL("%u", texture);

		// A name with an image behind it, as SCD3D11 answers.
		return vulkan->TextureSerial(texture) != 0;
	}

	void cVKDriver::PrioritizeTextures(int32_t count, uint32_t const* textures, float const* priorities)
	{
		SCVK_CALL("%d, %p, %p", count, textures, priorities);
	}

	bool cVKDriver::AreTexturesResident(int32_t count, uint32_t const* textures, bool* residences)
	{
		SCVK_CALL("%d, %p, %p", count, textures, residences);

		if (count < 0 || (count > 0 && (textures == nullptr || residences == nullptr)))
		{
			SetLastError(DriverError::INVALID_VALUE);
			return false;
		}

		// Every texture with an image is resident; Vulkan keeps no other kind
		bool areAllResident = true;

		for (int32_t i = 0; i < count; i++)
		{
			residences[i]   = vulkan->TextureSerial(textures[i]) != 0;
			areAllResident = areAllResident && residences[i];
		}

		return areAllResident;
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
		areStageCoordinatesDirty = true;

		if (NoteOnce(NOTE_COORDINATE_SOURCE, activeTextureStage | (gdTextureCoordinateSource << 8)))
		{
			LogDebug("  TEXCOORDSRC stage %u source %u", activeTextureStage, gdTextureCoordinateSource);
		}
	}

	void cVKDriver::TexStageMatrix(float const* matrix, uint32_t unknown0, uint32_t unknown1, uint32_t gdTextureMatrixFlags)
	{
		SCVK_CALL("%p, %u, %u, 0x%x", matrix, unknown0, unknown1, gdTextureMatrixFlags);

		lastTextureMatrixFlags = gdTextureMatrixFlags;

		textureStageMatrixArguments[activeTextureStage][0] = unknown0;
		textureStageMatrixArguments[activeTextureStage][1] = unknown1;
		textureStageMatrixArguments[activeTextureStage][2] = gdTextureMatrixFlags;

		// Keep the matrix for the active stage
		//
		// It transforms that stage's coordinates whether they are generated or come from
		// the vertex, as OpenGL's texture matrix does. Dropping it for vertex coordinates
		// left the foundations sampling far outside their clamped textures, which came
		// out black. A null matrix means identity. The flag cases the OpenGL driver
		// distinguishes all rewrite rows 2 and 3 of the matrix and leave rows 0 and 1
		// alone. A 2D sample uses only those first two rows, so none of that distinction
		// reaches here, and the fourth row is taken to leave q at one.
		memcpy(textureStageMatrices[activeTextureStage], (matrix != nullptr) ? matrix : IDENTITY_MATRIX, sizeof(textureStageMatrices[activeTextureStage]));
		areStageCoordinatesDirty = true;

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
		LogDebug("    texture matrix flags 0x%x: [%.3f %.3f %.3f %.3f] [%.3f %.3f %.3f %.3f] [%.3f %.3f %.3f %.3f] [%.3f %.3f %.3f %.3f]", gdTextureMatrixFlags, matrix[0], matrix[1], matrix[2], matrix[3], matrix[4], matrix[5], matrix[6], matrix[7], matrix[8], matrix[9], matrix[10], matrix[11], matrix[12], matrix[13], matrix[14], matrix[15]);
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
			LogDebug("  STAGE unit %u %s (texture %u)", textureUnit, texture != 0 ? "bound" : "cleared", texture);
		}

		if (textureUnit == 1 && texture != 0 && NoteOnce(NOTE_STAGE1_TEXTURE, texture))
		{
			vulkan->LogTextureInformation(texture, "stage 1");
		}

		// Bind it to its stage
		if (textureUnit >= STAGE_COUNT || (texture != 0 && !vulkan->IsTextureName(texture)))
		{
			SetLastError(DriverError::INVALID_VALUE);
			return;
		}

		BindStageTexture(textureUnit, texture);
	}

	void cVKDriver::BindStageTexture(uint32_t stage, uint32_t texture)
	{
		if (stage == 0)
		{
			boundTexture = texture;
			vulkan->SetTexture(texture);
		}
		else
		{
			stage1Texture = texture;
			vulkan->SetTexture1(texture);
		}
	}

	intptr_t cVKDriver::GetTexture(uint32_t textureUnit)
	{
		SCVK_CALL("%u", textureUnit);

		if (textureUnit >= STAGE_COUNT)
		{
			SetLastError(DriverError::INVALID_VALUE);
			return 0;
		}

		// Handles are small positive numbers, so they fit the signed type the interface
		// returns.
		return static_cast<intptr_t>((textureUnit == 0) ? boundTexture : stage1Texture);
	}

	intptr_t cVKDriver::CreateTexture(uint32_t gdInternalTextureFormat, uint32_t width, uint32_t height, uint32_t levels, uint32_t gdTextureHintFlags)
	{
		SCVK_CALL("fmt %u, %ux%u, %u levels, hints 0x%x", gdInternalTextureFormat, width, height, levels, gdTextureHintFlags);

		// Must be non-zero: the game tests the result before using it. Handles are small
		// positive numbers, so they fit the signed type the interface returns.
		uint32_t const texture = vulkan->CreateTexture(gdInternalTextureFormat, width, height, levels);
		if (texture == 0)
		{
			SetLastError(DriverError::CREATE_CONTEXT_FAILED);
			return 0;
		}

		// The new texture is bound to the active stage, as SCD3D11 binds it
		BindStageTexture(activeTextureStage, texture);
		return static_cast<intptr_t>(texture);
	}

	void cVKDriver::LoadTextureLevel(uint32_t texture, int32_t level, int32_t offsetX, int32_t offsetY, int32_t width, int32_t height, uint32_t gdTextureFormat, uint32_t gdType, uint32_t rowLength, void const* pixels)
	{
		SCVK_CALL("%u, level %d, +%d+%d, %dx%d, fmt %u, type %u, row %u, %p", texture, level, offsetX, offsetY, width, height, gdTextureFormat, gdType, rowLength, pixels);

		if (level < 0 || width <= 0 || height <= 0 || offsetX < 0 || offsetY < 0 || pixels == nullptr)
		{
			SetLastError(DriverError::INVALID_VALUE);
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

		// Pack it and switch the stage to it
		//
		// Setting a network also selects Combine for the stage, until the next mode
		// replaces it, as SCGL does. The game never asks for Combine through the mode
		// itself, so without this its networks never applied, and the building shadows
		// took the texture colours rather than their own.
		if (textureUnit < STAGE_COUNT)
		{
			rawCombiner[textureUnit * 2 + 0] = PackCombinerChannel(combiner.RGBCombineMode, combiner.RGBParams, combiner.RGBScale);
			rawCombiner[textureUnit * 2 + 1] = PackCombinerChannel(combiner.AlphaCombineMode, combiner.AlphaParams, combiner.AlphaScale);

			textureEnvironmentMode[textureUnit] = kGDTextureEnvParam_Combine;
			PushCombinerState();

			if (textureUnit == 0)
			{
				vulkan->SetTextureEnvironmentMode(kGDTextureEnvParam_Combine);
			}
		}

		// Describe it the first time it is seen
		if (NoteOnce(NOTE_COMBINER, key))
		{
			LogDebug("  COMBINER unit %u: rgb mode %u scale %u  src/op (%u,%u) (%u,%u) (%u,%u) | alpha mode %u scale %u  src/op (%u,%u) (%u,%u) (%u,%u)", textureUnit, combiner.RGBCombineMode, combiner.RGBScale, combiner.RGBParams[0].SourceType, combiner.RGBParams[0].OperandType, combiner.RGBParams[1].SourceType, combiner.RGBParams[1].OperandType, combiner.RGBParams[2].SourceType, combiner.RGBParams[2].OperandType, combiner.AlphaCombineMode, combiner.AlphaScale, combiner.AlphaParams[0].SourceType, combiner.AlphaParams[0].OperandType, combiner.AlphaParams[1].SourceType, combiner.AlphaParams[1].OperandType, combiner.AlphaParams[2].SourceType, combiner.AlphaParams[2].OperandType);
		}
	}
}
