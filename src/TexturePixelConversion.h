/*
 * scvk - a native Vulkan renderer for SimCity 4
 *
 * Pixel conversions carried over from SCD3D11, Copyright (C) 2025 Nelson Gomez
 * (nsgomez), under the same licence.
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

#pragma once

//// Dependencies

#include <stdint.h>

/**
 * Converts texels the game uploads in its less common formats and types to 8-bit RGBA.
 *
 * The numbers are the game's own, as SCGL's translation tables list them. Formats 0 to 6
 * are RGB, RGBA, BGR, BGRA, alpha, luminance and luminance with alpha. Types 0 to 7 are
 * one component each, from signed byte through double; 8, 9, 12 and 13 are packed
 * 4_4_4_4, 5_5_5_1, 8_8_8_8 and reversed 4_4_4_4. The common byte and 16-bit uploads
 * have faster paths of their own in VulkanBackend_Textures.cpp.
 */
namespace scvk
{
	//// Public API

	/** How many bytes one texel of the format and type takes, or 0 when it is not handled. */
	uint32_t TextureSourcePixelBytes(uint32_t format, uint32_t type);

	/** Reads one texel into RGBA, returning false for a format or type that is not handled. */
	bool ConvertTextureSourcePixel(uint32_t format, uint32_t type, void const* source, uint8_t rgba[4]);
}
