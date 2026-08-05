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
 * for at all. Reproducing it means encoding the combiner state into the same
 * key that selects the pipeline, and evaluating it in the fragment shader.
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
	}

	void cVKDriver::TexEnv(uint32_t gdTextureEnvTarget, uint32_t gdTextureEnvParamType, float const* params)
	{
		SCVK_CALL("%u, %u, %p", gdTextureEnvTarget, gdTextureEnvParamType, params);
	}

	void cVKDriver::TexParameter(uint32_t gdTextureTarget, uint32_t gdTextureParamType, int32_t gdTextureParam)
	{
		SCVK_CALL("%u, %u, %d", gdTextureTarget, gdTextureParamType, gdTextureParam);
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
	}

	void cVKDriver::TexStageCoord(uint32_t gdTexCoordSource)
	{
		SCVK_CALL("%u", gdTexCoordSource);
	}

	void cVKDriver::TexStageMatrix(float const* matrix, uint32_t unknown0, uint32_t unknown1, uint32_t gdTexMatFlags)
	{
		SCVK_CALL("%p, %u, %u, 0x%x", matrix, unknown0, unknown1, gdTexMatFlags);

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

		// Only the first stage is honoured. The game drives two, but the second
		// is part of the combiner network that has no equivalent yet.
		if (texUnit == 0)
		{
			boundTexture = texture;
			vulkan->SetTexture(texture);
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
	}
}
