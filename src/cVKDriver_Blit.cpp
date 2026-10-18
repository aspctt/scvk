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
 * 2D blits.
 *
 * Six entry points covering the cross product of stretched/unstretched and
 * plain/alpha/alpha-modulated, all drawn the way SCD3D11 draws them: the source pixels go
 * into a texture of the driver's and a quad is drawn over the destination, scaled to it,
 * point sampled so a 1:1 blit stays exact. The plain blits are opaque unless the game has
 * asked through Punt for the source alpha; the Alpha blits take a constant alpha, and the
 * modulated ones multiply by a colour. A colour key turns matching texels transparent.
 *
 * The game draws its startup screen this way in some configurations, and its interface
 * through textured quads otherwise.
 */

//// Dependencies

#include "cVKDriver.h"
#include "Logger.h"
#include "TextureUploadUtils.h"
#include "VulkanBackend.h"

#include <windows.h>
#include <stdio.h>
#include <string.h>

namespace scvk
{
	//// Constants

	namespace
	{
		// The game's own format and type enumerations, as decoded from SCGL's translation
		// tables. Index 3 of the format table is BGRA and index 1 of the type table is
		// unsigned byte, which together mean plain BGRA8.
		constexpr uint32_t GD_FORMAT_BGR         = 2;
		constexpr uint32_t GD_FORMAT_BGRA        = 3;
		constexpr uint32_t GD_TYPE_UNSIGNED_BYTE = 1;

		// How a blit treats alpha: opaque, the source's, a constant, or the source's
		// multiplied by a colour.
		constexpr uint32_t BLIT_ALPHA_OPAQUE           = 0;
		constexpr uint32_t BLIT_ALPHA_SOURCE           = 1;
		constexpr uint32_t BLIT_ALPHA_CONSTANT         = 2;
		constexpr uint32_t BLIT_ALPHA_SOURCE_MODULATED = 3;

		// Page protections that allow reading.
		constexpr DWORD READABLE_PROTECTION = PAGE_READONLY | PAGE_READWRITE | PAGE_WRITECOPY | PAGE_EXECUTE_READ | PAGE_EXECUTE_READWRITE | PAGE_EXECUTE_WRITECOPY;

		// A candidate buffer is sampled at this stride rather than scanned whole; enough
		// to tell an image from a zero-filled block.
		constexpr size_t PROBE_SAMPLE_STRIDE = 997;

		// How much of a buffer counts as readable when the whole image is not.
		constexpr size_t PROBE_MINIMUM_BYTES = 64;
	}

	//// Private Functions

	namespace
	{
		/**
		 * A blit's colour or alpha, 0xAARRGGBB. A bare byte carries only alpha, so 0xFF
		 * and 0xFFFFFFFF both leave the colour alone, as SCD3D11 reads it.
		 */
		void UnpackColour(uint32_t value, float rgba[4])
		{
			if (value <= 0xff)
			{
				rgba[0] = rgba[1] = rgba[2] = 1.0f;
				rgba[3] = static_cast<float>(value) / 255.0f;
				return;
			}

			rgba[0] = static_cast<float>((value >> 16) & 0xff) / 255.0f;
			rgba[1] = static_cast<float>((value >> 8) & 0xff) / 255.0f;
			rgba[2] = static_cast<float>(value & 0xff) / 255.0f;
			rgba[3] = static_cast<float>(value >> 24) / 255.0f;
		}

		/** True if the range can be read without faulting. */
		bool IsReadable(void const* address, size_t bytes)
		{
			if (address == nullptr)
			{
				return false;
			}

			// Check the page is committed and readable
			MEMORY_BASIC_INFORMATION memoryInformation{};
			if (VirtualQuery(address, &memoryInformation, sizeof(memoryInformation)) == 0 || memoryInformation.State != MEM_COMMIT)
			{
				return false;
			}

			if ((memoryInformation.Protect & READABLE_PROTECTION) == 0 || (memoryInformation.Protect & PAGE_GUARD) != 0)
			{
				return false;
			}

			// Check the region extends far enough to hold the data
			//
			// The addresses are compared as numbers.
			uintptr_t const regionEnd = reinterpret_cast<uintptr_t>(memoryInformation.BaseAddress) + memoryInformation.RegionSize;
			return reinterpret_cast<uintptr_t>(address) + bytes <= regionEnd;
		}

		/**
		 * Describes a candidate pixel buffer.
		 *
		 * The blit entry points take two void pointers and the interface names neither of
		 * them. SCGL calls them unknownBuffer1 and unknownBuffer2 and never resolved
		 * which carries the image, because it implements none of these methods. Guessing
		 * produced a black screen, so this reports what is actually behind each pointer:
		 * whether it is readable, how much of it is non-zero, and the leading bytes.
		 */
		void DescribeBuffer(char const* label, void const* buffer, size_t expectedBytes)
		{
			if (buffer == nullptr)
			{
				LogDebug("    %s: null", label);
				return;
			}

			// Fall back to a smaller probe when the whole image is not readable
			//
			// A smaller readable region means this is a pointer to something other than
			// the full image.
			if (!IsReadable(buffer, expectedBytes))
			{
				bool const isStartReadable = IsReadable(buffer, PROBE_MINIMUM_BYTES);
				LogDebug("    %s: %p, NOT readable for %zu bytes%s", label, buffer, expectedBytes, isStartReadable ? " (but the first 64 bytes are readable)" : "");

				if (!isStartReadable)
				{
					return;
				}

				expectedBytes = PROBE_MINIMUM_BYTES;
			}

			// Sample how much of it is non-zero
			//
			// The buffer is untyped bytes.
			uint8_t const* const bytes = static_cast<uint8_t const*>(buffer);

			size_t nonZero = 0;
			size_t samples = 0;
			for (size_t i = 0; i < expectedBytes; i += PROBE_SAMPLE_STRIDE)
			{
				if (bytes[i] != 0)
				{
					nonZero++;
				}

				samples++;
			}

			// Write it with its leading bytes
			char leadingBytes[64];
			sprintf_s(leadingBytes, sizeof(leadingBytes), "%02X %02X %02X %02X %02X %02X %02X %02X %02X %02X %02X %02X ", bytes[0], bytes[1], bytes[2], bytes[3], bytes[4], bytes[5], bytes[6], bytes[7], bytes[8], bytes[9], bytes[10], bytes[11]);

			LogDebug("    %s: %p, readable, %zu/%zu sampled bytes non-zero, starts %s", label, buffer, nonZero, samples, leadingBytes);
		}
	}

	void cVKDriver::UploadBlit(char const* caller, int32_t destinationLeft, int32_t destinationTop, int32_t destinationWidth, int32_t destinationHeight, int32_t sourceWidth, int32_t sourceHeight, uint32_t gdTextureFormat, uint32_t gdType, void const* pixels, bool isColourKeyed, void const* colourKey, uint32_t alphaMode, uint32_t alphaValue)
	{
		if (destinationWidth <= 0 || destinationHeight <= 0 || sourceWidth <= 0 || sourceHeight <= 0 || pixels == nullptr)
		{
			SetLastError(DriverError::INVALID_VALUE);
			return;
		}

		// Both are positive, checked just above.
		uint32_t const width  = static_cast<uint32_t>(sourceWidth);
		uint32_t const height = static_cast<uint32_t>(sourceHeight);

		// Describe the pixels, a few times a session
		if (blitProbesRemaining > 0)
		{
			blitProbesRemaining--;
			LogDebug("  %s source buffers, %ux%u format %u type %u:", caller, width, height, gdTextureFormat, gdType);
			DescribeBuffer("pixels", pixels, size_t{ width } * height * 4u);
		}

		// Read the format the way SCD3D11 does: up to four bytes a pixel
		uint32_t const pixelBytes = TextureSourcePixelBytes(gdTextureFormat, gdType);
		if (pixelBytes == 0 || pixelBytes > 4)
		{
			static bool hasLogged = false;
			if (!hasLogged)
			{
				hasLogged = true;
				LogWarn("%s: source format %u type %u is not implemented.", caller, gdTextureFormat, gdType);
			}

			SetLastError(DriverError::NOT_SUPPORTED);
			return;
		}

		isColourKeyed = isColourKeyed && colourKey != nullptr;

		uint32_t key = 0;
		if (isColourKeyed)
		{
			memcpy(&key, colourKey, pixelBytes);
		}

		bool isSourceAlphaUsed = alphaMode == BLIT_ALPHA_SOURCE || alphaMode == BLIT_ALPHA_SOURCE_MODULATED;

		// The constant alpha, or the modulating colour, unpacked from 0xAARRGGBB
		float modulate[4] = { 1.0f, 1.0f, 1.0f, 1.0f };

		if (alphaMode == BLIT_ALPHA_CONSTANT)
		{
			float colour[4];
			UnpackColour(alphaValue, colour);
			modulate[3] = colour[3];
		}
		else if (alphaMode == BLIT_ALPHA_SOURCE_MODULATED)
		{
			UnpackColour(alphaValue, modulate);
		}

		// Bring the pixels to tight BGRA8 rows
		//
		// BGRA8 is the back buffer's own order and goes as it is; every other format is
		// read pixel by pixel. Packed formats cannot be compared in the shader, so their
		// colour key is applied here.
		uint32_t const   sourcePitch = ((pixelStoreRowLength != 0) ? pixelStoreRowLength : width) * pixelBytes;
		uint8_t const*   bgra        = static_cast<uint8_t const*>(pixels);
		bool const       isBgra      = gdType == GD_TYPE_UNSIGNED_BYTE && gdTextureFormat == GD_FORMAT_BGRA;
		bool const       isBgr       = gdType == GD_TYPE_UNSIGNED_BYTE && gdTextureFormat == GD_FORMAT_BGR;

		if (!isBgra || sourcePitch != width * 4u)
		{
			blitScratch.resize(size_t{ width } * height * 4u);

			for (uint32_t y = 0; y < height; y++)
			{
				uint8_t const* source      = static_cast<uint8_t const*>(pixels) + size_t{ y } * sourcePitch;
				uint8_t*       destination = blitScratch.data() + size_t{ y } * width * 4u;

				if (isBgra)
				{
					memcpy(destination, source, size_t{ width } * 4u);
					continue;
				}

				for (uint32_t x = 0; x < width; x++, source += pixelBytes, destination += 4)
				{
					if (isBgr)
					{
						destination[0] = source[0];
						destination[1] = source[1];
						destination[2] = source[2];
						destination[3] = 0xff;
						continue;
					}

					uint8_t rgba[4];
					if (!ConvertTextureSourcePixel(gdTextureFormat, gdType, source, rgba))
					{
						SetLastError(DriverError::NOT_SUPPORTED);
						return;
					}

					destination[0] = rgba[2];
					destination[1] = rgba[1];
					destination[2] = rgba[0];
					// Both arms a byte, so the choice stays one rather than becoming an int
					destination[3] = isSourceAlphaUsed ? rgba[3] : uint8_t{ 0xff };

					if (isColourKeyed)
					{
						uint32_t raw = 0;
						memcpy(&raw, source, pixelBytes);

						if (raw == key)
						{
							destination[3] = 0;
						}
					}
				}
			}

			// The converted alpha already holds the key and the source alpha
			if (!isBgra && !isBgr)
			{
				isSourceAlphaUsed = true;
				isColourKeyed     = false;
			}

			bgra = blitScratch.data();
		}

		// A 24 or 32-bit key is 0x00RRGGBB, matching the order of BGR(A) pixels
		float keyColour[4] = { 0.0f, 0.0f, 0.0f, 0.0f };

		if (isColourKeyed)
		{
			keyColour[0] = static_cast<float>((key >> 16) & 0xff) / 255.0f;
			keyColour[1] = static_cast<float>((key >> 8) & 0xff) / 255.0f;
			keyColour[2] = static_cast<float>(key & 0xff) / 255.0f;
			keyColour[3] = 1.0f;
		}

		bool const isBlended = alphaMode != BLIT_ALPHA_OPAQUE || isColourKeyed || isSourceAlphaUsed;
		vulkan->DrawPixels(destinationLeft, destinationTop, destinationWidth, destinationHeight, width, height, bgra, modulate, keyColour, isSourceAlphaUsed, isBlended);
	}

	//// Public API

	void cVKDriver::BitBlt(int32_t destinationLeft, int32_t destinationTop, int32_t width, int32_t height, uint32_t gdTextureFormat, uint32_t gdType, void const* buffer, bool isColourKeyed, void const* colourKey)
	{
		SCVK_CALL("%d,%d %dx%d, fmt %u, type %u, key %d", destinationLeft, destinationTop, width, height, gdTextureFormat, gdType, isColourKeyed);
		UploadBlit("BitBlt", destinationLeft, destinationTop, width, height, width, height, gdTextureFormat, gdType, buffer, isColourKeyed, colourKey, isBlitSourceAlphaUsed ? BLIT_ALPHA_SOURCE : BLIT_ALPHA_OPAQUE, 0xffffffffu);
	}

	void cVKDriver::StretchBlt(int32_t destinationLeft, int32_t destinationTop, int32_t destinationWidth, int32_t destinationHeight, int32_t sourceWidth, int32_t sourceHeight, uint32_t gdTextureFormat, uint32_t gdType, void const* buffer, bool isColourKeyed, void const* colourKey)
	{
		SCVK_CALL("%d,%d %dx%d from %dx%d, fmt %u, type %u, key %d", destinationLeft, destinationTop, destinationWidth, destinationHeight, sourceWidth, sourceHeight, gdTextureFormat, gdType, isColourKeyed);
		UploadBlit("StretchBlt", destinationLeft, destinationTop, destinationWidth, destinationHeight, sourceWidth, sourceHeight, gdTextureFormat, gdType, buffer, isColourKeyed, colourKey, isBlitSourceAlphaUsed ? BLIT_ALPHA_SOURCE : BLIT_ALPHA_OPAQUE, 0xffffffffu);
	}

	void cVKDriver::BitBltAlpha(int32_t destinationLeft, int32_t destinationTop, int32_t width, int32_t height, uint32_t gdTextureFormat, uint32_t gdType, void const* buffer, bool isColourKeyed, void const* colourKey, uint32_t alpha)
	{
		SCVK_CALL("%d,%d %dx%d, fmt %u, type %u, key %d, alpha 0x%x", destinationLeft, destinationTop, width, height, gdTextureFormat, gdType, isColourKeyed, alpha);
		UploadBlit("BitBltAlpha", destinationLeft, destinationTop, width, height, width, height, gdTextureFormat, gdType, buffer, isColourKeyed, colourKey, BLIT_ALPHA_CONSTANT, alpha);
	}

	void cVKDriver::StretchBltAlpha(int32_t destinationLeft, int32_t destinationTop, int32_t destinationWidth, int32_t destinationHeight, int32_t sourceWidth, int32_t sourceHeight, uint32_t gdTextureFormat, uint32_t gdType, void const* buffer, bool isColourKeyed, void const* colourKey, uint32_t alpha)
	{
		SCVK_CALL("%d,%d %dx%d from %dx%d, fmt %u, type %u, key %d, alpha 0x%x", destinationLeft, destinationTop, destinationWidth, destinationHeight, sourceWidth, sourceHeight, gdTextureFormat, gdType, isColourKeyed, alpha);
		UploadBlit("StretchBltAlpha", destinationLeft, destinationTop, destinationWidth, destinationHeight, sourceWidth, sourceHeight, gdTextureFormat, gdType, buffer, isColourKeyed, colourKey, BLIT_ALPHA_CONSTANT, alpha);
	}

	void cVKDriver::BitBltAlphaModulate(int32_t destinationLeft, int32_t destinationTop, int32_t width, int32_t height, uint32_t gdTextureFormat, uint32_t gdType, void const* buffer, bool isColourKeyed, void const* colourKey, uint32_t alpha)
	{
		SCVK_CALL("%d,%d %dx%d, fmt %u, type %u, key %d, colour 0x%x", destinationLeft, destinationTop, width, height, gdTextureFormat, gdType, isColourKeyed, alpha);
		UploadBlit("BitBltAlphaModulate", destinationLeft, destinationTop, width, height, width, height, gdTextureFormat, gdType, buffer, isColourKeyed, colourKey, BLIT_ALPHA_SOURCE_MODULATED, alpha);
	}

	void cVKDriver::StretchBltAlphaModulate(int32_t destinationLeft, int32_t destinationTop, int32_t destinationWidth, int32_t destinationHeight, int32_t sourceWidth, int32_t sourceHeight, uint32_t gdTextureFormat, uint32_t gdType, void const* buffer, bool isColourKeyed, void const* colourKey, uint32_t alpha)
	{
		SCVK_CALL("%d,%d %dx%d from %dx%d, fmt %u, type %u, key %d, colour 0x%x", destinationLeft, destinationTop, destinationWidth, destinationHeight, sourceWidth, sourceHeight, gdTextureFormat, gdType, isColourKeyed, alpha);
		UploadBlit("StretchBltAlphaModulate", destinationLeft, destinationTop, destinationWidth, destinationHeight, sourceWidth, sourceHeight, gdTextureFormat, gdType, buffer, isColourKeyed, colourKey, BLIT_ALPHA_SOURCE_MODULATED, alpha);
	}
}
