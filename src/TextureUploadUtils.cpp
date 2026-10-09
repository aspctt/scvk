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

#include "TextureUploadUtils.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <limits>

namespace scvk {
	namespace {
		constexpr bool RangeFits(uint64_t offset, uint64_t size, uint64_t limit) {
			return offset <= limit && size <= limit - offset;
		}

		template <typename T>
		T Read(void const *source) {
			T value;
			memcpy(&value, source, sizeof(value));
			return value;
		}

		uint8_t ToByte(double value) {
			value = (std::max)(0.0, (std::min)(1.0, value));
			return static_cast<uint8_t>(std::lround(value * 255.0));
		}

		double ReadNormalizedComponent(uint32_t type, uint8_t const *source) {
			switch (type) {
				case 0: return (std::max)(0.0, static_cast<double>(Read<int8_t>(source)) / 127.0);
				case 1: return static_cast<double>(Read<uint8_t>(source)) / 255.0;
				case 2: return (std::max)(0.0, static_cast<double>(Read<int16_t>(source)) / 32767.0);
				case 3: return static_cast<double>(Read<uint16_t>(source)) / 65535.0;
				case 4: return (std::max)(0.0, static_cast<double>(Read<int32_t>(source)) / 2147483647.0);
				case 5: return static_cast<double>(Read<uint32_t>(source)) / 4294967295.0;
				case 6: return static_cast<double>(Read<float>(source));
				case 7: return Read<double>(source);
				default: return 0.0;
			}
		}

		uint32_t ScalarBytes(uint32_t type) {
			switch (type) {
				case 0:
				case 1: return 1;
				case 2:
				case 3: return 2;
				case 4:
				case 5:
				case 6: return 4;
				case 7: return 8;
				default: return 0;
			}
		}

		void AssignComponents(uint32_t format, uint8_t const components[4], uint8_t rgba[4]) {
			switch (format) {
				case 0: rgba[0] = components[0]; rgba[1] = components[1]; rgba[2] = components[2]; rgba[3] = 0xff; break;
				case 1: memcpy(rgba, components, 4); break;
				case 2: rgba[0] = components[2]; rgba[1] = components[1]; rgba[2] = components[0]; rgba[3] = 0xff; break;
				case 3: rgba[0] = components[2]; rgba[1] = components[1]; rgba[2] = components[0]; rgba[3] = components[3]; break;
				case 4: rgba[0] = rgba[1] = rgba[2] = 0xff; rgba[3] = components[0]; break;
				case 5: rgba[0] = rgba[1] = rgba[2] = components[0]; rgba[3] = 0xff; break;
				case 6: rgba[0] = rgba[1] = rgba[2] = components[0]; rgba[3] = components[1]; break;
				default: memset(rgba, 0, 4); break;
			}
		}
	}

	uint32_t TextureSourceComponents(uint32_t format) {
		switch (format) {
			case 0:
			case 2: return 3;
			case 1:
			case 3: return 4;
			case 4:
			case 5: return 1;
			case 6: return 2;
			default: return 0;
		}
	}

	uint32_t TextureSourcePixelBytes(uint32_t format, uint32_t type) {
		uint32_t const components = TextureSourceComponents(format);
		if (components == 0) return 0;
		uint32_t const scalarBytes = ScalarBytes(type);
		if (scalarBytes != 0) return components * scalarBytes;
		if (components != 4) return 0;
		if (type == 8 || type == 9 || type == 13) return 2;
		if (type == 12) return 4;
		return 0;
	}

	bool ConvertTextureSourcePixel(uint32_t format, uint32_t type, void const *source, uint8_t rgba[4]) {
		uint32_t const components = TextureSourceComponents(format);
		uint32_t const pixelBytes = TextureSourcePixelBytes(format, type);
		if (components == 0 || pixelBytes == 0 || source == nullptr || rgba == nullptr) return false;

		uint8_t converted[4]{0, 0, 0, 0xff};
		uint32_t const scalarBytes = ScalarBytes(type);
		if (scalarBytes != 0) {
			uint8_t const *bytes = static_cast<uint8_t const *>(source);
			for (uint32_t component = 0; component < components; ++component) {
				converted[component] = ToByte(ReadNormalizedComponent(type, bytes + component * scalarBytes));
			}
		} else if (type == 8 || type == 13) {
			uint16_t const value = Read<uint16_t>(source);
			for (uint32_t component = 0; component < 4; ++component) {
				uint32_t const shift = type == 8 ? 12 - component * 4 : component * 4;
				converted[component] = static_cast<uint8_t>(((value >> shift) & 0xf) * 17);
			}
		} else if (type == 9) {
			uint16_t const value = Read<uint16_t>(source);
			converted[0] = static_cast<uint8_t>(((value >> 11) & 0x1f) * 255 / 31);
			converted[1] = static_cast<uint8_t>(((value >> 6) & 0x1f) * 255 / 31);
			converted[2] = static_cast<uint8_t>(((value >> 1) & 0x1f) * 255 / 31);
			converted[3] = (value & 1) != 0 ? 0xff : 0;
		} else if (type == 12) {
			uint32_t const value = Read<uint32_t>(source);
			for (uint32_t component = 0; component < 4; ++component) {
				converted[component] = static_cast<uint8_t>(value >> (24 - component * 8));
			}
		} else {
			return false;
		}
		AssignComponents(format, converted, rgba);
		return true;
	}

	bool IsValidBlockCompressedUpdate(
		uint32_t mipWidth, uint32_t mipHeight,
		uint32_t x, uint32_t y, uint32_t width, uint32_t height) {
		if (width == 0 || height == 0 || !RangeFits(x, width, mipWidth) || !RangeFits(y, height, mipHeight)) return false;
		return x % 4 == 0 && y % 4 == 0 &&
		       ((x + width) % 4 == 0 || x + width == mipWidth) &&
		       ((y + height) % 4 == 0 || y + height == mipHeight);
	}
}
