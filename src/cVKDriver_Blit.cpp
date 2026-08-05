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
 * An earlier version of this comment guessed these were incidental. The first
 * real trace says otherwise: StretchBlt is how the game puts its startup and
 * loading screens on the display. Every call is identical,
 *
 *     StretchBlt(576,240 768x600 from 768x600, fmt 3, type 1)
 *
 * which is a 768x600 image centred in a 1920x1080 window, unscaled. It is
 * called continuously while the game sits on that screen, and refusing it
 * accounted for 899,697 log lines in a single session.
 *
 * So these are on the critical path for anything visible before a city loads,
 * and they need a real implementation earlier than the draw path does.
 *
 * They are now implemented as a staged upload followed by
 * vkCmdCopyBufferToImage straight into the swapchain image, which works
 * because the game's BGRA8 pixels match the swapchain format exactly. No
 * conversion, no shader, no render pass.
 *
 * One thing remains unresolved: the interface hands over two void pointers and
 * names neither. See UploadBlit.
 */

#include "cVKDriver.h"
#include "Logger.h"
#include "VulkanBackend.h"

#include <Windows.h>

namespace scvk
{
	namespace
	{
		// The game's own format and type enumerations, as decoded from SCGL's
		// translation tables. Index 3 of the format table is BGRA and index 1
		// of the type table is unsigned byte, which together mean plain BGRA8.
		constexpr uint32_t kFormatBGRA        = 3;
		constexpr uint32_t kTypeUnsignedByte  = 1;

		/** True if the range can be read without faulting. */
		bool IsReadable(void const* address, size_t bytes)
		{
			if (address == nullptr)
			{
				return false;
			}

			MEMORY_BASIC_INFORMATION info{};
			if (VirtualQuery(address, &info, sizeof(info)) == 0 || info.State != MEM_COMMIT)
			{
				return false;
			}

			DWORD const readable = PAGE_READONLY | PAGE_READWRITE | PAGE_WRITECOPY |
				PAGE_EXECUTE_READ | PAGE_EXECUTE_READWRITE | PAGE_EXECUTE_WRITECOPY;

			if ((info.Protect & readable) == 0 || (info.Protect & PAGE_GUARD) != 0)
			{
				return false;
			}

			// The region has to actually extend far enough to hold the data.
			uintptr_t regionEnd = reinterpret_cast<uintptr_t>(info.BaseAddress) + info.RegionSize;
			return reinterpret_cast<uintptr_t>(address) + bytes <= regionEnd;
		}

		/**
		 * Describes a candidate pixel buffer.
		 *
		 * The blit entry points take two void pointers and the interface names
		 * neither of them. SCGL calls them unknownBuffer1 and unknownBuffer2
		 * and never resolved which carries the image, because it implements
		 * none of these methods. Guessing produced a black screen, so this
		 * reports what is actually behind each pointer: whether it is readable,
		 * how much of it is non-zero, and the leading bytes.
		 */
		void DescribeBuffer(char const* label, void const* buffer, size_t expectedBytes)
		{
			if (buffer == nullptr)
			{
				LogNote("    %s: null", label);
				return;
			}

			if (!IsReadable(buffer, expectedBytes))
			{
				// Still worth probing a little: a smaller readable region means
				// this is a pointer to something other than the full image.
				bool small = IsReadable(buffer, 64);
				LogNote("    %s: %p, NOT readable for %zu bytes%s", label, buffer, expectedBytes,
					small ? " (but the first 64 bytes are readable)" : "");
				if (!small)
				{
					return;
				}
				expectedBytes = 64;
			}

			uint8_t const* bytes = static_cast<uint8_t const*>(buffer);

			// Sample rather than scan the whole thing; enough to tell an image
			// from a zero-filled block.
			size_t nonZero = 0;
			size_t samples = 0;
			for (size_t i = 0; i < expectedBytes; i += 997)
			{
				if (bytes[i] != 0) nonZero++;
				samples++;
			}

			char hex[64];
			int n = 0;
			for (int i = 0; i < 12 && n < static_cast<int>(sizeof(hex)) - 4; i++)
			{
				n += sprintf_s(hex + n, sizeof(hex) - n, "%02X ", bytes[i]);
			}

			LogNote("    %s: %p, readable, %zu/%zu sampled bytes non-zero, starts %s",
				label, buffer, nonZero, samples, hex);
		}
	}

	void cVKDriver::UploadBlit(char const* caller,
		int32_t destLeft, int32_t destTop,
		int32_t destWidth, int32_t destHeight,
		int32_t srcWidth, int32_t srcHeight,
		uint32_t gdTexFormat, uint32_t gdType,
		void const* buffer1, void const* buffer2)
	{
		if (srcWidth <= 0 || srcHeight <= 0)
		{
			return;
		}

		size_t expectedBytes = static_cast<size_t>(srcWidth) * srcHeight * 4u;

		if (blitProbesRemaining > 0)
		{
			blitProbesRemaining--;
			LogNote("  %s source buffers, expecting %zu bytes of BGRA8:", caller, expectedBytes);
			DescribeBuffer("buffer1", buffer1, expectedBytes);
			DescribeBuffer("buffer2", buffer2, expectedBytes);
		}

		// buffer1 is the working assumption. If the probe above shows it is
		// zero-filled and buffer2 is not, this is the line to change.
		void const* pixels = buffer1;

		if (pixels == nullptr)
		{
			return;
		}

		// BGRA8 is the only combination seen from the game, and it happens to
		// match the swapchain exactly, so it copies with no conversion. Any
		// other format would need converting before this could work, so say so
		// rather than uploading nonsense.
		if (gdTexFormat != kFormatBGRA || gdType != kTypeUnsignedByte)
		{
			LogNote("%s: unsupported pixel format %u type %u; skipping.", caller, gdTexFormat, gdType);
			SetLastError(DriverError::NotSupported);
			return;
		}

		// Scaling needs an intermediate image and vkCmdBlitImage. Every call
		// observed so far is 1:1, so the copy path covers it and the scaling
		// path can wait until something actually needs it.
		if (destWidth != srcWidth || destHeight != srcHeight)
		{
			LogNote("%s: scaled blit %dx%d from %dx%d is not implemented yet; copying unscaled.",
				caller, destWidth, destHeight, srcWidth, srcHeight);
		}

		vulkan->BlitPixels(destLeft, destTop,
			static_cast<uint32_t>(srcWidth), static_cast<uint32_t>(srcHeight),
			static_cast<uint32_t>(srcWidth), pixels);
	}

	void cVKDriver::BitBlt(int32_t destLeft, int32_t destTop, int32_t width, int32_t height, uint32_t gdTexFormat, uint32_t gdType, void const* buffer, bool unknown, void const* buffer2)
	{
		SCVK_CALL("%d,%d %dx%d, fmt %u, type %u", destLeft, destTop, width, height, gdTexFormat, gdType);
		UploadBlit("BitBlt", destLeft, destTop, width, height, width, height, gdTexFormat, gdType, buffer, buffer2);
	}

	void cVKDriver::StretchBlt(int32_t destLeft, int32_t destTop, int32_t destWidth, int32_t destHeight, int32_t srcWidth, int32_t srcHeight, uint32_t gdTexFormat, uint32_t gdType, void const* buffer, bool unknown, void const* buffer2)
	{
		SCVK_CALL("%d,%d %dx%d from %dx%d, fmt %u, type %u",
			destLeft, destTop, destWidth, destHeight, srcWidth, srcHeight, gdTexFormat, gdType);
		UploadBlit("StretchBlt", destLeft, destTop, destWidth, destHeight, srcWidth, srcHeight, gdTexFormat, gdType, buffer, buffer2);
	}

	void cVKDriver::BitBltAlpha(int32_t destLeft, int32_t destTop, int32_t width, int32_t height, uint32_t gdTexFormat, uint32_t gdType, void const* buffer, bool unknown, void const* buffer2, uint32_t alpha)
	{
		SCVK_CALL("%d,%d %dx%d, fmt %u, type %u, alpha %u",
			destLeft, destTop, width, height, gdTexFormat, gdType, alpha);
		SetLastError(DriverError::NotSupported);
	}

	void cVKDriver::StretchBltAlpha(int32_t destLeft, int32_t destTop, int32_t destWidth, int32_t destHeight, int32_t srcWidth, int32_t srcHeight, uint32_t gdTexFormat, uint32_t gdType, void const* buffer, bool unknown, void const* buffer2, uint32_t alpha)
	{
		SCVK_CALL("%d,%d %dx%d from %dx%d, fmt %u, type %u, alpha %u",
			destLeft, destTop, destWidth, destHeight, srcWidth, srcHeight, gdTexFormat, gdType, alpha);
		SetLastError(DriverError::NotSupported);
	}

	void cVKDriver::BitBltAlphaModulate(int32_t destLeft, int32_t destTop, int32_t width, uint32_t gdTexFormat, uint32_t gdType, void const* buffer, bool unknown, void const* buffer2, uint32_t alpha)
	{
		SCVK_CALL("%d,%d w%d, fmt %u, type %u, alpha %u", destLeft, destTop, width, gdTexFormat, gdType, alpha);
		SetLastError(DriverError::NotSupported);
	}

	void cVKDriver::StretchBltAlphaModulate(int32_t destLeft, int32_t destTop, int32_t destWidth, int32_t destHeight, int32_t srcWidth, int32_t srcHeight, uint32_t gdTexFormat, uint32_t gdType, void const* buffer, bool unknown, void const* buffer2, uint32_t alpha)
	{
		SCVK_CALL("%d,%d %dx%d from %dx%d, fmt %u, type %u, alpha %u",
			destLeft, destTop, destWidth, destHeight, srcWidth, srcHeight, gdTexFormat, gdType, alpha);
		SetLastError(DriverError::NotSupported);
	}
}
