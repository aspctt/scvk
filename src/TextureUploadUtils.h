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

#include <cstdint>

// The game's pixel formats and types, as SCGL's translation tables number them, read
// into 8-bit RGBA. Used by the blits and by texture uploads in formats other than the
// common byte ones.

namespace scvk {
	uint32_t TextureSourceComponents(uint32_t format);
	uint32_t TextureSourcePixelBytes(uint32_t format, uint32_t type);
	bool ConvertTextureSourcePixel(uint32_t format, uint32_t type, void const *source, uint8_t rgba[4]);
	bool IsValidBlockCompressedUpdate(
		uint32_t mipWidth, uint32_t mipHeight,
		uint32_t x, uint32_t y, uint32_t width, uint32_t height);
}
