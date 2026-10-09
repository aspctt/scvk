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
 * The game packs its vertex formats into a bitfield and asks the driver to decode strides
 * and element offsets back out of it, then does its own pointer arithmetic with the
 * results. A wrong stride is not a visual artefact, it is an out-of-bounds walk, so these
 * have to be answered honestly whether or not anything is being drawn.
 *
 * The same decoding drives the pipelines: a format identifies which attributes exist and
 * where, and there is no shortcut. Keying on stride instead looks tempting and is wrong,
 * because V3F_C4UB_T2F and V3F_N3F are both 24 bytes and agree on nothing after the
 * position.
 *
 * The decoding is SCGL's, vendored under the LGPL. See vendor/README.md.
 */

//// Dependencies

#include "cVKDriver.h"
#include "Logger.h"
#include "VulkanBackend.h"

#include <VertexFormatUtils.h>

namespace scvk
{
	//// Constants

	namespace
	{
		// The coordinate source the game uses for generated coordinates. 0x10 is its
		// counterpart of D3DTSS_TCI_CAMERASPACEPOSITION: the coordinate comes from the
		// vertex position in eye space rather than from a coordinate set. The low three
		// bits name a coordinate set, which is what a source without the flag samples.
		// See SCGL's GLTextureUnit.cpp.
		constexpr uint32_t CAMERA_SPACE_POSITION_SOURCE = 0x10;
		constexpr uint32_t SOURCE_SET_BITS              = 7;

		// The game's type numbering, shared with its texture uploads.
		constexpr uint32_t GD_INDEX_TYPE_UNSIGNED_SHORT = 3;
		constexpr uint32_t GD_INDEX_TYPE_UNSIGNED_INT   = 5;
	}

	//// Private Functions

	void cVKDriver::UpdateTransform(void)
	{
		// Combine the projection and modelview
		//
		// Column-major, matching both the game and GLSL: result = P * M, so
		// result[column][row] = sum over k of P[k][row] * M[column][k].
		float modelViewProjection[16];

		for (int column = 0; column < 4; column++)
		{
			for (int row = 0; row < 4; row++)
			{
				float sum = 0.0f;
				for (int k = 0; k < 4; k++)
				{
					sum += projectionMatrix[k * 4 + row] * modelViewMatrix[column * 4 + k];
				}

				modelViewProjection[column * 4 + row] = sum;
			}
		}

		vulkan->SetTransform(modelViewProjection);

		// Hand over the fog's distance row
		//
		// The fog measures distance as the depth in front of the camera, minus the
		// eye-space z, as Direct3D does: the game's DirectX driver loads the modelview as
		// the world matrix under a view matrix that flips z. Eye z is the modelview's
		// third row, which column-major storage spreads across every fourth float.
		float const fogDistanceRow[4] = { -modelViewMatrix[2], -modelViewMatrix[6], -modelViewMatrix[10], -modelViewMatrix[14] };
		vulkan->SetFogDistanceRow(fogDistanceRow);
	}

	void cVKDriver::PushStageCoordinates(void)
	{
		for (uint32_t stage = 0; stage < 2; stage++)
		{
			float const* const stageMatrix = textureStageMatrices[stage];
			bool const isGenerated = IsGeneratingCoordinates(stage);

			// Take the matrix rows a 2D sample reads
			//
			// Column major throughout, matching the game and GLSL: M[column * 4 + row].
			float rowS[4] = { stageMatrix[0], stageMatrix[4], stageMatrix[8], stageMatrix[12] };
			float rowT[4] = { stageMatrix[1], stageMatrix[5], stageMatrix[9], stageMatrix[13] };

			// Fold the modelview in when the stage generates
			//
			// texcoord = textureMatrix * modelview * position, so the two are combined here
			// and what samples it is left with one dot product per component.
			if (isGenerated)
			{
				for (int column = 0; column < 4; column++)
				{
					float sumS = 0.0f;
					float sumT = 0.0f;

					for (int k = 0; k < 4; k++)
					{
						sumS += stageMatrix[k * 4 + 0] * modelViewMatrix[column * 4 + k];
						sumT += stageMatrix[k * 4 + 1] * modelViewMatrix[column * 4 + k];
					}

					rowS[column] = sumS;
					rowT[column] = sumT;
				}
			}

			vulkan->SetStageCoordinates(stage, isGenerated, textureCoordinateSource[stage] & SOURCE_SET_BITS, rowS, rowT);
		}
	}

	void cVKDriver::PushChangedTransforms(void)
	{
		if (isStageCoordinatesDirty)
		{
			PushStageCoordinates();
			isStageCoordinatesDirty = false;
		}

		if (isTransformDirty)
		{
			UpdateTransform();
			isTransformDirty = false;
		}
	}

	bool cVKDriver::IsGeneratingCoordinates(uint32_t stage) const
	{
		return (textureCoordinateSource[stage] & ~SOURCE_SET_BITS) == CAMERA_SPACE_POSITION_SOURCE;
	}

	bool cVKDriver::IsCloudShadowDraw(void) const
	{
		// The building shadows projected onto the terrain generate on the first stage
		// too, but always with the second stage on.
		return isTextureStageEnabled[0] && !isTextureStageEnabled[1] && IsGeneratingCoordinates(0);
	}

	bool cVKDriver::IsBuildingShadowDraw(void) const
	{
		// The first stage projects the building's own texture and the second samples the
		// mask that keeps each shadow to one copy.
		return isTextureStageEnabled[0] && isTextureStageEnabled[1] && stage1Texture != 0 && IsGeneratingCoordinates(0) && IsGeneratingCoordinates(1);
	}

	//// Public API

	void cVKDriver::DrawArrays(uint32_t gdPrimitiveType, int32_t first, int32_t count)
	{
		SCVK_CALL("%u, %d, %d", gdPrimitiveType, first, count);

		if (count <= 0 || first < 0 || vertexPointer == nullptr || vertexStride == 0)
		{
			return;
		}

		if (shouldSkipCloudShadows && IsCloudShadowDraw())
		{
			return;
		}

		// Describe the draw for the diagnostics
		ProbeDrawArrays(gdPrimitiveType, first, count);
		MaybeArmDump();
		DumpDraw(gdPrimitiveType, count, first, nullptr, false);
		ReportLargeDraw(gdPrimitiveType, first, count);
		ReportProjectionMismatch(gdPrimitiveType, count);
		NoteRegionDraw(gdPrimitiveType, count, first, nullptr, false);
		NoteMultitexturedDraw(vertexFormat);
		NoteDarkTintedDraw(gdPrimitiveType, count);
		ReportShadowMaskDraw(gdPrimitiveType, count, first, nullptr, false);
		NoteShadowStrength();
		NoteSharedSetDraw(gdPrimitiveType, count);

		// Draw it
		//
		// Both numbers were checked to be positive above.
		PushChangedTransforms();
		vulkan->DrawVertices(gdPrimitiveType, vertexFormat, vertexPointer, static_cast<uint32_t>(first), static_cast<uint32_t>(count));
	}

	void cVKDriver::DrawElements(uint32_t gdPrimitiveType, int32_t count, uint32_t gdType, void const* indices)
	{
		SCVK_CALL("%u, %d, %u, %p", gdPrimitiveType, count, gdType, indices);

		if (count <= 0 || indices == nullptr || vertexPointer == nullptr || vertexStride == 0)
		{
			return;
		}

		if (shouldSkipCloudShadows && IsCloudShadowDraw())
		{
			return;
		}

		// Read the index width
		//
		// Unsigned byte indices are expressible in the enumeration but Vulkan has no core
		// equivalent, and the game has not been seen using them.
		bool isIndex32Bit;

		switch (gdType)
		{
		case GD_INDEX_TYPE_UNSIGNED_SHORT: isIndex32Bit = false; break;
		case GD_INDEX_TYPE_UNSIGNED_INT:   isIndex32Bit = true;  break;

		default:
			if (indexTypeWarningsRemaining > 0)
			{
				indexTypeWarningsRemaining--;
				LogWarn("  DrawElements: index type %u is not handled; skipping the draw.", gdType);
			}

			return;
		}

		// Describe the draw for the diagnostics
		NoteRegionDraw(gdPrimitiveType, count, 0, indices, isIndex32Bit);
		NoteMultitexturedDraw(vertexFormat);
		NoteDarkTintedDraw(gdPrimitiveType, count);
		ReportShadowMaskDraw(gdPrimitiveType, count, 0, indices, isIndex32Bit);
		NoteShadowStrength();
		NoteSharedSetDraw(gdPrimitiveType, count);
		MaybeArmDump();
		DumpDraw(gdPrimitiveType, count, 0, indices, isIndex32Bit);

		// Draw it
		//
		// The count was checked to be positive above.
		PushChangedTransforms();
		vulkan->DrawIndexedVertices(gdPrimitiveType, vertexFormat, vertexPointer, indices, static_cast<uint32_t>(count), isIndex32Bit);
	}

	void cVKDriver::InterleavedArrays(uint32_t gdVertexFormat, int32_t stride, void const* pointer)
	{
		// A stride of zero means "tightly packed", which the game leaves for the driver
		// to work out from the format. Strides are a few dozen bytes, so they fit either
		// signedness.
		if (stride == 0)
		{
			stride = static_cast<int32_t>(RZVertexFormatStride(gdVertexFormat));
		}

		uint32_t const unsignedStride = static_cast<uint32_t>(stride);

		SCVK_CALL("0x%x, %d, %p", gdVertexFormat, stride, pointer);

		// Report each format once
		//
		// How many texture coordinate sets a format carries is what decides whether a
		// second texture stage has anything to sample with.
		if (NoteOnce(NOTE_VERTEX_FORMAT, gdVertexFormat))
		{
			LogDebug("  FORMAT 0x%x: stride %u, %u texcoord set(s), %u colour, %u normal", gdVertexFormat, unsignedStride, RZVertexFormatNumElements(gdVertexFormat, kGDElementType_TexCoord), RZVertexFormatNumElements(gdVertexFormat, kGDElementType_Color), RZVertexFormatNumElements(gdVertexFormat, kGDElementType_Normal));
		}

		// Record the format and pointer rather than uploading
		//
		// The game names a client pointer here and draws from it later, possibly several
		// times, so the copy happens at draw time when the vertex range is actually
		// known. The format is kept as well as the stride, because the stride alone does
		// not identify the layout.
		vertexFormat  = gdVertexFormat;
		vertexStride  = unsignedStride;
		vertexPointer = pointer;
	}

	uint32_t cVKDriver::MakeVertexFormat(uint32_t count, intptr_t gdElementTypeList)
	{
		// The game builds a format from an element type list here rather than from a
		// standard format id. Not observed in practice; the trace says whether it is ever
		// reached. The list arrives as an integer, printed as the pointer it is.
		SCVK_CALL("%u, 0x%p  [UNIMPLEMENTED]", count, reinterpret_cast<void*>(gdElementTypeList));

		SetLastError(DriverError::NOT_SUPPORTED);
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
