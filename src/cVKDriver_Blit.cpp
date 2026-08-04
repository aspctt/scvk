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
 * and they need a real implementation earlier than the draw path does. In
 * Vulkan they become a staged upload into a texture plus a full-screen quad,
 * or a straight vkCmdCopyBufferToImage into the swapchain image where the
 * formats allow it.
 *
 * Until then they keep reporting not-supported, which is honest and which the
 * game demonstrably survives.
 */

#include "cVKDriver.h"
#include "Logger.h"

namespace scvk
{
	void cVKDriver::BitBlt(int32_t destLeft, int32_t destTop, int32_t width, int32_t height, uint32_t gdTexFormat, uint32_t gdType, void const* buffer, bool unknown, void const* buffer2)
	{
		SCVK_CALL("%d,%d %dx%d, fmt %u, type %u", destLeft, destTop, width, height, gdTexFormat, gdType);
		SetLastError(DriverError::NotSupported);
	}

	void cVKDriver::StretchBlt(int32_t destLeft, int32_t destTop, int32_t destWidth, int32_t destHeight, int32_t srcWidth, int32_t srcHeight, uint32_t gdTexFormat, uint32_t gdType, void const* buffer, bool unknown, void const* buffer2)
	{
		SCVK_CALL("%d,%d %dx%d from %dx%d, fmt %u, type %u",
			destLeft, destTop, destWidth, destHeight, srcWidth, srcHeight, gdTexFormat, gdType);
		SetLastError(DriverError::NotSupported);
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
