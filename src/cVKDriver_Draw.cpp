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
 * The game packs its vertex formats into a bitfield and asks the driver to
 * decode strides and element offsets back out of it, then does its own pointer
 * arithmetic with the results. A wrong stride is not a visual artefact, it is
 * an out-of-bounds walk, so these have to be answered honestly whether or not
 * anything is being drawn.
 *
 * The same decoding drives the pipelines: a format identifies which attributes
 * exist and where, and there is no shortcut. Keying on stride instead looks
 * tempting and is wrong, because V3F_C4UB_T2F and V3F_N3F are both 24 bytes
 * and agree on nothing after the position.
 *
 * The decoding is SCGL's, vendored under the LGPL. See vendor/README.md.
 */

#include "cVKDriver.h"
#include "Logger.h"
#include "VulkanBackend.h"

#include <VertexFormatUtils.h>

namespace scvk
{
	void cVKDriver::UpdateTransform(void)
	{
		// Column-major, matching both the game and GLSL: result = P * M, so
		// result[col][row] = sum over k of P[k][row] * M[col][k].
		float mvp[16];

		for (int col = 0; col < 4; col++)
		{
			for (int row = 0; row < 4; row++)
			{
				float sum = 0.0f;
				for (int k = 0; k < 4; k++)
				{
					sum += projectionMatrix[k * 4 + row] * modelViewMatrix[col * 4 + k];
				}
				mvp[col * 4 + row] = sum;
			}
		}

		vulkan->SetTransform(mvp);
	}

	void cVKDriver::DrawArrays(uint32_t gdPrimType, int32_t first, int32_t count)
	{
		SCVK_CALL("%u, %d, %d", gdPrimType, first, count);

		if (count <= 0 || first < 0 || vertexPointer == nullptr || vertexStride == 0)
		{
			return;
		}

		// One sample per distinct combination of format, primitive type and
		// projection.
		//
		// Sampling the first few draws only reported tiny sub-pixel quads.
		// Sampling per format and primitive was better but still only covered
		// the startup screen, because later screens reuse the same formats and
		// nothing new was ever recorded. The projection is what actually
		// distinguishes one rendering context from another here, so it belongs
		// in the key.
		uint32_t projectionHash = 2166136261u;
		for (int i = 0; i < 16; i++)
		{
			// Quantised, so floating point noise does not make every frame
			// look like a new context.
			int32_t const quantised = static_cast<int32_t>(projectionMatrix[i] * 1000.0f);
			projectionHash = (projectionHash ^ static_cast<uint32_t>(quantised)) * 16777619u;
		}

		uint32_t const probeKey =
			(vertexFormat << 8) ^ (gdPrimType & 0xFF) ^ (projectionHash & 0xFFFF0000u);
		bool alreadyProbed = false;
		for (int i = 0; i < probedCombinations; i++)
		{
			if (probedKeys[i] == probeKey) { alreadyProbed = true; break; }
		}

		if (!alreadyProbed && probedCombinations < static_cast<int>(_countof(probedKeys)))
		{
			probedKeys[probedCombinations++] = probeKey;

			LogNote("  DrawArrays prim %u, %d vertices, format 0x%x stride %u, viewport %d,%d %dx%d:",
				gdPrimType, count, vertexFormat, vertexStride,
				viewportX, viewportY, viewportWidth, viewportHeight);

			// Both matrices, column by column. If geometry appears at the
			// wrong scale this is where the answer is: the positions the game
			// submits are small world-space values and mean nothing without
			// the projection that maps them.
			LogNote("    modelview  [%.3f %.3f %.3f %.3f] [%.3f %.3f %.3f %.3f] [%.3f %.3f %.3f %.3f] [%.3f %.3f %.3f %.3f]",
				modelViewMatrix[0], modelViewMatrix[1], modelViewMatrix[2], modelViewMatrix[3],
				modelViewMatrix[4], modelViewMatrix[5], modelViewMatrix[6], modelViewMatrix[7],
				modelViewMatrix[8], modelViewMatrix[9], modelViewMatrix[10], modelViewMatrix[11],
				modelViewMatrix[12], modelViewMatrix[13], modelViewMatrix[14], modelViewMatrix[15]);
			LogNote("    projection [%.3f %.3f %.3f %.3f] [%.3f %.3f %.3f %.3f] [%.3f %.3f %.3f %.3f] [%.3f %.3f %.3f %.3f]",
				projectionMatrix[0], projectionMatrix[1], projectionMatrix[2], projectionMatrix[3],
				projectionMatrix[4], projectionMatrix[5], projectionMatrix[6], projectionMatrix[7],
				projectionMatrix[8], projectionMatrix[9], projectionMatrix[10], projectionMatrix[11],
				projectionMatrix[12], projectionMatrix[13], projectionMatrix[14], projectionMatrix[15]);

			int const shown = (count < 3) ? count : 3;
			for (int i = 0; i < shown; i++)
			{
				uint8_t const* v = static_cast<uint8_t const*>(vertexPointer) +
					static_cast<size_t>(first + i) * vertexStride;

				float const* position = reinterpret_cast<float const*>(v);

				// Texture coordinates matter as much as positions here: if the
				// content looks magnified, either the quad is too big or the
				// coordinates cover too little of the texture, and these two
				// numbers tell those apart.
				if (RZVertexFormatNumElements(vertexFormat, kGDElementType_TexCoord) != 0)
				{
					uint32_t const uvOffset = RZVertexFormatElementOffset(vertexFormat, kGDElementType_TexCoord, 0);
					float const* uv = reinterpret_cast<float const*>(v + uvOffset);

					LogNote("    v%d pos %.3f %.3f %.3f  uv %.4f %.4f", i, position[0], position[1], position[2], uv[0], uv[1]);
				}
				else
				{
					uint8_t const* colour = v + 12;
					LogNote("    v%d pos %.3f %.3f %.3f  colour %3u %3u %3u %3u",
						i, position[0], position[1], position[2],
						colour[0], colour[1], colour[2], colour[3]);
				}
			}
		}

		// Detect a projection that disagrees with the viewport it is drawn
		// into.
		//
		// An orthographic projection's x scale is 2 divided by the width of
		// the region it maps onto the whole of clip space. If that width is
		// not the viewport's width, the draw is stretched by exactly their
		// ratio, which is the magnification being chased. Every context
		// sampled so far agreed, and the splash screen renders perfectly, so
		// the culprit is a pairing the sampling has not caught.
		if (mismatchReportsRemaining > 0 && viewportWidth > 0 && viewportHeight > 0)
		{
			float const xScale = projectionMatrix[0] < 0.0f ? -projectionMatrix[0] : projectionMatrix[0];
			float const yScale = projectionMatrix[5] < 0.0f ? -projectionMatrix[5] : projectionMatrix[5];

			// A near-zero scale means a perspective or degenerate projection,
			// where this reasoning does not apply.
			if (xScale > 1e-6f && yScale > 1e-6f)
			{
				float const impliedWidth  = 2.0f / xScale;
				float const impliedHeight = 2.0f / yScale;

				float const widthRatio  = impliedWidth  / static_cast<float>(viewportWidth);
				float const heightRatio = impliedHeight / static_cast<float>(viewportHeight);

				bool const stretched =
					widthRatio  < 0.9f || widthRatio  > 1.1f ||
					heightRatio < 0.9f || heightRatio > 1.1f;

				if (stretched)
				{
					mismatchReportsRemaining--;
					LogNote("  MISMATCH: projection covers %.1fx%.1f but viewport is %dx%d at %d,%d "
						"(stretched %.2fx by %.2fx), format 0x%x prim %u, %d vertices",
						impliedWidth, impliedHeight, viewportWidth, viewportHeight, viewportX, viewportY,
						widthRatio, heightRatio, vertexFormat, gdPrimType, count);
				}
			}
		}

		UpdateTransform();
		vulkan->DrawVertices(gdPrimType, vertexFormat, vertexPointer,
			static_cast<uint32_t>(first), static_cast<uint32_t>(count));
	}

	void cVKDriver::DrawElements(uint32_t gdPrimType, int32_t count, uint32_t gdType, void const* indices)
	{
		// Never observed from the game, which only ever uses DrawArrays.
		SCVK_CALL("%u, %d, %u, %p  [UNIMPLEMENTED]", gdPrimType, count, gdType, indices);
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

		// Recorded rather than uploaded. The game names a client pointer here
		// and draws from it later, possibly several times, so the copy happens
		// at draw time when the vertex range is actually known.
		//
		// The format is kept as well as the stride, because the stride alone
		// does not identify the layout: V3F_C4UB_T2F and V3F_N3F are both 24
		// bytes and share nothing past the position.
		vertexFormat  = gdVertexFormat;
		vertexStride  = static_cast<uint32_t>(stride);
		vertexPointer = pointer;
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
