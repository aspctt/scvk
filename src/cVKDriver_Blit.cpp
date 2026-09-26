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
 * plain/alpha/alpha-modulated.
 *
 * An earlier version of this comment guessed these were incidental. The first real trace
 * says otherwise: StretchBlt is how the game puts its startup and loading screens on the
 * display. Every call is identical,
 *
 *     StretchBlt(576,240 768x600 from 768x600, fmt 3, type 1)
 *
 * which is a 768x600 image centred in a 1920x1080 window, unscaled. It is called
 * continuously while the game sits on that screen, and refusing it accounted for 899,697
 * log lines in a single session.
 *
 * So these are on the critical path for anything visible before a city loads. They are
 * implemented as a staged upload followed by vkCmdCopyBufferToImage straight into the
 * swapchain image, which works because the game's BGRA8 pixels match the swapchain format
 * exactly. No conversion, no shader, no render pass.
 *
 * One thing remains unresolved: the interface hands over two void pointers and names
 * neither. See UploadBlit.
 */

//// Dependencies

#include "cVKDriver.h"
#include "Logger.h"
#include "VulkanBackend.h"

#include <Windows.h>

namespace scvk
{
	//// Constants

	namespace
	{
		// The game's own format and type enumerations, as decoded from SCGL's translation
		// tables. Index 3 of the format table is BGRA and index 1 of the type table is
		// unsigned byte, which together mean plain BGRA8.
		constexpr uint32_t GD_FORMAT_BGRA        = 3;
		constexpr uint32_t GD_TYPE_UNSIGNED_BYTE = 1;

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
				LogNote("    %s: null", label);
				return;
			}

			// Fall back to a smaller probe when the whole image is not readable
			//
			// A smaller readable region means this is a pointer to something other than
			// the full image.
			if (!IsReadable(buffer, expectedBytes))
			{
				bool const isStartReadable = IsReadable(buffer, PROBE_MINIMUM_BYTES);
				LogNote("    %s: %p, NOT readable for %zu bytes%s", label, buffer, expectedBytes, isStartReadable ? " (but the first 64 bytes are readable)" : "");

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

			LogNote("    %s: %p, readable, %zu/%zu sampled bytes non-zero, starts %s", label, buffer, nonZero, samples, leadingBytes);
		}
	}

	void cVKDriver::UploadBlit(char const* caller, int32_t destinationLeft, int32_t destinationTop, int32_t destinationWidth, int32_t destinationHeight, int32_t sourceWidth, int32_t sourceHeight, uint32_t gdTextureFormat, uint32_t gdType, void const* buffer1, void const* buffer2)
	{
		if (sourceWidth <= 0 || sourceHeight <= 0)
		{
			return;
		}

		// Both are positive, checked just above.
		uint32_t const width  = static_cast<uint32_t>(sourceWidth);
		uint32_t const height = static_cast<uint32_t>(sourceHeight);

		// Describe the two source pointers, a few times a session
		size_t const expectedBytes = size_t{ width } * height * 4u;

		if (blitProbesRemaining > 0)
		{
			blitProbesRemaining--;
			LogNote("  %s source buffers, expecting %zu bytes of BGRA8:", caller, expectedBytes);
			DescribeBuffer("buffer1", buffer1, expectedBytes);
			DescribeBuffer("buffer2", buffer2, expectedBytes);
		}

		// Take the first pointer as the pixels
		//
		// That is the working assumption. If the probe above shows it is zero-filled and
		// the second is not, this is the line to change.
		void const* const pixels = buffer1;

		if (pixels == nullptr)
		{
			return;
		}

		// Refuse anything but BGRA8
		//
		// It is the only combination seen from the game, and it happens to match the
		// swapchain exactly, so it copies with no conversion. Any other format would need
		// converting before this could work, so it says so rather than uploading
		// nonsense.
		if (gdTextureFormat != GD_FORMAT_BGRA || gdType != GD_TYPE_UNSIGNED_BYTE)
		{
			LogNote("%s: unsupported pixel format %u type %u; skipping.", caller, gdTextureFormat, gdType);
			SetLastError(DriverError::NOT_SUPPORTED);
			return;
		}

		// Copy it unscaled
		//
		// Scaling needs an intermediate image and vkCmdBlitImage. Every call observed so
		// far is 1:1, so the copy path covers it and the scaling path can wait until
		// something actually needs it.
		if (destinationWidth != sourceWidth || destinationHeight != sourceHeight)
		{
			LogNote("%s: scaled blit %dx%d from %dx%d is not implemented yet; copying unscaled.", caller, destinationWidth, destinationHeight, sourceWidth, sourceHeight);
		}

		vulkan->BlitPixels(destinationLeft, destinationTop, width, height, width, pixels);
	}

	//// Public API

	void cVKDriver::BitBlt(int32_t destinationLeft, int32_t destinationTop, int32_t width, int32_t height, uint32_t gdTextureFormat, uint32_t gdType, void const* buffer, [[maybe_unused]] bool isUnknownFlagSet, void const* buffer2)
	{
		SCVK_CALL("%d,%d %dx%d, fmt %u, type %u", destinationLeft, destinationTop, width, height, gdTextureFormat, gdType);
		UploadBlit("BitBlt", destinationLeft, destinationTop, width, height, width, height, gdTextureFormat, gdType, buffer, buffer2);
	}

	void cVKDriver::StretchBlt(int32_t destinationLeft, int32_t destinationTop, int32_t destinationWidth, int32_t destinationHeight, int32_t sourceWidth, int32_t sourceHeight, uint32_t gdTextureFormat, uint32_t gdType, void const* buffer, [[maybe_unused]] bool isUnknownFlagSet, void const* buffer2)
	{
		SCVK_CALL("%d,%d %dx%d from %dx%d, fmt %u, type %u", destinationLeft, destinationTop, destinationWidth, destinationHeight, sourceWidth, sourceHeight, gdTextureFormat, gdType);
		UploadBlit("StretchBlt", destinationLeft, destinationTop, destinationWidth, destinationHeight, sourceWidth, sourceHeight, gdTextureFormat, gdType, buffer, buffer2);
	}

	void cVKDriver::BitBltAlpha(int32_t destinationLeft, int32_t destinationTop, int32_t width, int32_t height, uint32_t gdTextureFormat, uint32_t gdType, [[maybe_unused]] void const* buffer, [[maybe_unused]] bool isUnknownFlagSet, [[maybe_unused]] void const* buffer2, uint32_t alpha)
	{
		SCVK_CALL("%d,%d %dx%d, fmt %u, type %u, alpha %u", destinationLeft, destinationTop, width, height, gdTextureFormat, gdType, alpha);
		SetLastError(DriverError::NOT_SUPPORTED);
	}

	void cVKDriver::StretchBltAlpha(int32_t destinationLeft, int32_t destinationTop, int32_t destinationWidth, int32_t destinationHeight, int32_t sourceWidth, int32_t sourceHeight, uint32_t gdTextureFormat, uint32_t gdType, [[maybe_unused]] void const* buffer, [[maybe_unused]] bool isUnknownFlagSet, [[maybe_unused]] void const* buffer2, uint32_t alpha)
	{
		SCVK_CALL("%d,%d %dx%d from %dx%d, fmt %u, type %u, alpha %u", destinationLeft, destinationTop, destinationWidth, destinationHeight, sourceWidth, sourceHeight, gdTextureFormat, gdType, alpha);
		SetLastError(DriverError::NOT_SUPPORTED);
	}

	void cVKDriver::BitBltAlphaModulate(int32_t destinationLeft, int32_t destinationTop, int32_t width, uint32_t gdTextureFormat, uint32_t gdType, [[maybe_unused]] void const* buffer, [[maybe_unused]] bool isUnknownFlagSet, [[maybe_unused]] void const* buffer2, uint32_t alpha)
	{
		SCVK_CALL("%d,%d w%d, fmt %u, type %u, alpha %u", destinationLeft, destinationTop, width, gdTextureFormat, gdType, alpha);
		SetLastError(DriverError::NOT_SUPPORTED);
	}

	void cVKDriver::StretchBltAlphaModulate(int32_t destinationLeft, int32_t destinationTop, int32_t destinationWidth, int32_t destinationHeight, int32_t sourceWidth, int32_t sourceHeight, uint32_t gdTextureFormat, uint32_t gdType, [[maybe_unused]] void const* buffer, [[maybe_unused]] bool isUnknownFlagSet, [[maybe_unused]] void const* buffer2, uint32_t alpha)
	{
		SCVK_CALL("%d,%d %dx%d from %dx%d, fmt %u, type %u, alpha %u", destinationLeft, destinationTop, destinationWidth, destinationHeight, sourceWidth, sourceHeight, gdTextureFormat, gdType, alpha);
		SetLastError(DriverError::NOT_SUPPORTED);
	}
}
