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
 * Draw submission and vertex formats.
 *
 * The draw calls themselves are stubs, but the vertex format queries are not,
 * and deliberately so. The game packs its vertex formats into a bitfield and
 * asks the driver to decode strides and element offsets back out of it, then
 * does its own pointer arithmetic with the results. A wrong stride is not a
 * visual artefact, it is an out-of-bounds walk, so answering these honestly is
 * what lets the game reach a first frame at all.
 *
 * The decoding is SCGL's, vendored under the LGPL. See vendor/README.md.
 */

#include "cVKDriver.h"
#include "Logger.h"

#include <VertexFormatUtils.h>

namespace scvk
{
	void cVKDriver::DrawArrays(uint32_t gdPrimType, int32_t first, int32_t count)
	{
		SCVK_CALL("%u, %d, %d", gdPrimType, first, count);
	}

	void cVKDriver::DrawElements(uint32_t gdPrimType, int32_t count, uint32_t gdType, void const* indices)
	{
		SCVK_CALL("%u, %d, %u, %p", gdPrimType, count, gdType, indices);
	}

	void cVKDriver::InterleavedArrays(uint32_t gdVertexFormat, int32_t stride, void const* pointer)
	{
		// A stride of zero means "tightly packed", which the game leaves for
		// the driver to work out from the format.
		if (stride == 0)
		{
			stride = static_cast<int32_t>(RZVertexFormatStride(gdVertexFormat));
		}

		SCVK_CALL("0x%x, %d, %p", gdVertexFormat, stride, pointer);
	}

	uint32_t cVKDriver::MakeVertexFormat(uint32_t count, intptr_t gdElementTypePtr)
	{
		// The game builds a format from an element type list here rather than
		// from a standard format id. Not yet observed in practice; the trace
		// will say whether it is ever reached.
		SCVK_CALL("%u, 0x%p  [UNIMPLEMENTED]", count, reinterpret_cast<void*>(gdElementTypePtr));

		SetLastError(DriverError::NotSupported);
		return UINT32_MAX;
	}

	uint32_t cVKDriver::MakeVertexFormat(uint32_t gdVertexFormat)
	{
		SCVK_CALL("%u", gdVertexFormat);
		return RZMakeVertexFormat(gdVertexFormat);
	}

	uint32_t cVKDriver::VertexFormatStride(uint32_t gdVertexFormat)
	{
		SCVK_CALL("0x%x", gdVertexFormat);
		return RZVertexFormatStride(gdVertexFormat);
	}

	uint32_t cVKDriver::VertexFormatElementOffset(uint32_t gdVertexFormat, uint32_t gdElementType, uint32_t index)
	{
		SCVK_CALL("0x%x, %u, %u", gdVertexFormat, gdElementType, index);
		return RZVertexFormatElementOffset(gdVertexFormat, gdElementType, index);
	}

	uint32_t cVKDriver::VertexFormatNumElements(uint32_t gdVertexFormat, uint32_t gdElementType)
	{
		SCVK_CALL("0x%x, %u", gdVertexFormat, gdElementType);
		return RZVertexFormatNumElements(gdVertexFormat, gdElementType);
	}
}
