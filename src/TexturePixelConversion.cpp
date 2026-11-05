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

//// Dependencies

#include "TexturePixelConversion.h"

#include <string.h>

#include <algorithm>
#include <cmath>

namespace scvk
{
	//// Constants

	namespace
	{
		// The game's formats, in SCGL's order
		constexpr uint32_t GD_FORMAT_RGB             = 0;
		constexpr uint32_t GD_FORMAT_RGBA            = 1;
		constexpr uint32_t GD_FORMAT_BGR             = 2;
		constexpr uint32_t GD_FORMAT_BGRA            = 3;
		constexpr uint32_t GD_FORMAT_ALPHA           = 4;
		constexpr uint32_t GD_FORMAT_LUMINANCE       = 5;
		constexpr uint32_t GD_FORMAT_LUMINANCE_ALPHA = 6;

		// The game's types, in SCGL's order. 10, 11, 14 and 15 map to nothing in SCGL's
		// table, and scvk handles the 16-bit ones it does meet on its own.
		constexpr uint32_t GD_TYPE_BYTE           = 0;
		constexpr uint32_t GD_TYPE_UNSIGNED_BYTE  = 1;
		constexpr uint32_t GD_TYPE_SHORT          = 2;
		constexpr uint32_t GD_TYPE_UNSIGNED_SHORT = 3;
		constexpr uint32_t GD_TYPE_INT            = 4;
		constexpr uint32_t GD_TYPE_UNSIGNED_INT   = 5;
		constexpr uint32_t GD_TYPE_FLOAT          = 6;
		constexpr uint32_t GD_TYPE_DOUBLE         = 7;
		constexpr uint32_t GD_TYPE_4444           = 8;
		constexpr uint32_t GD_TYPE_5551           = 9;
		constexpr uint32_t GD_TYPE_8888           = 12;
		constexpr uint32_t GD_TYPE_4444_REVERSED  = 13;
	}

	//// Private Functions

	namespace
	{
		/** Reads a value of any type from a byte pointer, which need not be aligned for it. */
		template <typename Value>
		Value ReadUnaligned(void const* source)
		{
			Value value;
			memcpy(&value, source, sizeof(value));
			return value;
		}

		/** Turns a component between 0 and 1 into a byte, clamping anything outside. */
		uint8_t ToByte(double value)
		{
			// Clamped to 0..1 first, so the rounded result is 0..255 and fits.
			return static_cast<uint8_t>(std::lround(std::clamp(value, 0.0, 1.0) * 255.0));
		}

		/**
		 * Reads one component of a single-component type as 0 to 1.
		 *
		 * Signed types are normalised as OpenGL does and their negative half clamps to 0, since
		 * a colour has none.
		 */
		double ReadNormalizedComponent(uint32_t type, uint8_t const* source)
		{
			switch (type)
			{
			case GD_TYPE_BYTE:           return std::max(0.0, ReadUnaligned<int8_t>(source) / 127.0);
			case GD_TYPE_UNSIGNED_BYTE:  return ReadUnaligned<uint8_t>(source) / 255.0;
			case GD_TYPE_SHORT:          return std::max(0.0, ReadUnaligned<int16_t>(source) / 32767.0);
			case GD_TYPE_UNSIGNED_SHORT: return ReadUnaligned<uint16_t>(source) / 65535.0;
			case GD_TYPE_INT:            return std::max(0.0, ReadUnaligned<int32_t>(source) / 2147483647.0);
			case GD_TYPE_UNSIGNED_INT:   return ReadUnaligned<uint32_t>(source) / 4294967295.0;
			case GD_TYPE_FLOAT:          return double{ ReadUnaligned<float>(source) };
			case GD_TYPE_DOUBLE:         return ReadUnaligned<double>(source);
			default:                     return 0.0;
			}
		}

		/** How many bytes one component of a single-component type takes, 0 for packed types. */
		uint32_t ComponentBytes(uint32_t type)
		{
			switch (type)
			{
			case GD_TYPE_BYTE:
			case GD_TYPE_UNSIGNED_BYTE:
				return 1;

			case GD_TYPE_SHORT:
			case GD_TYPE_UNSIGNED_SHORT:
				return 2;

			case GD_TYPE_INT:
			case GD_TYPE_UNSIGNED_INT:
			case GD_TYPE_FLOAT:
				return 4;

			case GD_TYPE_DOUBLE:
				return 8;

			default:
				return 0;
			}
		}

		/** How many components a texel of the format has, 0 for a format that is not handled. */
		uint32_t FormatComponents(uint32_t format)
		{
			switch (format)
			{
			case GD_FORMAT_RGB:
			case GD_FORMAT_BGR:
				return 3;

			case GD_FORMAT_RGBA:
			case GD_FORMAT_BGRA:
				return 4;

			case GD_FORMAT_ALPHA:
			case GD_FORMAT_LUMINANCE:
				return 1;

			case GD_FORMAT_LUMINANCE_ALPHA:
				return 2;

			default:
				return 0;
			}
		}

		/**
		 * Places components, in the order the format names them, into RGBA.
		 *
		 * Alpha alone is white with that alpha, and luminance fills all three colours, as
		 * OpenGL expands them.
		 */
		void AssignComponents(uint32_t format, uint8_t const components[4], uint8_t rgba[4])
		{
			switch (format)
			{
			case GD_FORMAT_RGB:
				rgba[0] = components[0];
				rgba[1] = components[1];
				rgba[2] = components[2];
				rgba[3] = 0xFF;
				break;

			case GD_FORMAT_RGBA:
				memcpy(rgba, components, 4);
				break;

			case GD_FORMAT_BGR:
				rgba[0] = components[2];
				rgba[1] = components[1];
				rgba[2] = components[0];
				rgba[3] = 0xFF;
				break;

			case GD_FORMAT_BGRA:
				rgba[0] = components[2];
				rgba[1] = components[1];
				rgba[2] = components[0];
				rgba[3] = components[3];
				break;

			case GD_FORMAT_ALPHA:
				rgba[0] = 0xFF;
				rgba[1] = 0xFF;
				rgba[2] = 0xFF;
				rgba[3] = components[0];
				break;

			case GD_FORMAT_LUMINANCE:
				rgba[0] = components[0];
				rgba[1] = components[0];
				rgba[2] = components[0];
				rgba[3] = 0xFF;
				break;

			case GD_FORMAT_LUMINANCE_ALPHA:
				rgba[0] = components[0];
				rgba[1] = components[0];
				rgba[2] = components[0];
				rgba[3] = components[1];
				break;

			default:
				memset(rgba, 0, 4);
				break;
			}
		}

		/** Widens a component of a few bits to a byte, so its largest value becomes 255. */
		uint8_t WidenComponent(uint32_t value, uint32_t largest)
		{
			// value is at most largest, so the result is at most 255.
			return static_cast<uint8_t>(value * 255u / largest);
		}
	}

	//// Public API

	uint32_t TextureSourcePixelBytes(uint32_t format, uint32_t type)
	{
		uint32_t const components = FormatComponents(format);
		if (components == 0)
		{
			return 0;
		}

		uint32_t const componentBytes = ComponentBytes(type);
		if (componentBytes != 0)
		{
			return components * componentBytes;
		}

		// The packed types hold all four components, so they only go with four-component
		// formats
		if (components != 4)
		{
			return 0;
		}

		if (type == GD_TYPE_4444 || type == GD_TYPE_5551 || type == GD_TYPE_4444_REVERSED)
		{
			return 2;
		}

		if (type == GD_TYPE_8888)
		{
			return 4;
		}

		return 0;
	}

	bool ConvertTextureSourcePixel(uint32_t format, uint32_t type, void const* source, uint8_t rgba[4])
	{
		uint32_t const components = FormatComponents(format);
		if (components == 0 || TextureSourcePixelBytes(format, type) == 0 || source == nullptr || rgba == nullptr)
		{
			return false;
		}

		// Read the components in the order the format names them
		//
		// The packed types put the first component in the highest bits, except reversed
		// 4_4_4_4, which puts it in the lowest. A missing alpha stays opaque.
		uint8_t converted[4] = { 0, 0, 0, 0xFF };
		uint32_t const componentBytes = ComponentBytes(type);

		if (componentBytes != 0)
		{
			uint8_t const* const bytes = static_cast<uint8_t const*>(source);
			for (uint32_t component = 0; component < components; component++)
			{
				converted[component] = ToByte(ReadNormalizedComponent(type, bytes + component * componentBytes));
			}
		}
		else if (type == GD_TYPE_4444 || type == GD_TYPE_4444_REVERSED)
		{
			uint32_t const value = ReadUnaligned<uint16_t>(source);
			for (uint32_t component = 0; component < 4; component++)
			{
				uint32_t const shift = (type == GD_TYPE_4444) ? 12 - component * 4 : component * 4;
				converted[component] = WidenComponent((value >> shift) & 0xF, 0xF);
			}
		}
		else if (type == GD_TYPE_5551)
		{
			uint32_t const value = ReadUnaligned<uint16_t>(source);
			converted[0] = WidenComponent((value >> 11) & 0x1F, 0x1F);
			converted[1] = WidenComponent((value >> 6) & 0x1F, 0x1F);
			converted[2] = WidenComponent((value >> 1) & 0x1F, 0x1F);
			converted[3] = WidenComponent(value & 1, 1);
		}
		else
		{
			// 8_8_8_8 is the only type left, since TextureSourcePixelBytes accepted it
			uint32_t const value = ReadUnaligned<uint32_t>(source);
			for (uint32_t component = 0; component < 4; component++)
			{
				converted[component] = WidenComponent((value >> (24 - component * 8)) & 0xFF, 0xFF);
			}
		}

		AssignComponents(format, converted, rgba);
		return true;
	}
}
