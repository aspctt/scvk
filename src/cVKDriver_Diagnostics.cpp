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
 * Diagnostics.
 *
 * Everything here only observes: it writes to scvk.log, or asks the backend for a
 * capture, and never changes what is drawn. Most of it exists because a bug could not be
 * found by reasoning and had to be measured instead, and the comments say which.
 *
 * The frame dump describes every draw of a scene rebuild. The partial update trace
 * records the buffer region copies and the draws between them. The tile ring summarises
 * each part of the scene the game saves, and Scroll Lock writes it out with captures of
 * the screen, the saved scene and the saved depth.
 */

//// Dependencies

#include "cVKDriver.h"
#include "Logger.h"
#include "VulkanBackend.h"

#include <VertexFormatUtils.h>

#include <Windows.h>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>

namespace scvk
{
	//// Constants

	namespace
	{
		// How many of a draw's vertices are transformed to measure it.
		constexpr int SAMPLED_VERTICES = 8;

		// How many of a probed draw's vertices are written out.
		constexpr int PROBED_VERTICES = 3;

		// A clip-space w this close to zero has no usable projection.
		constexpr float CLIP_W_EPSILON = 1e-6f;

		// Shadows are the only thing drawn with an ambient colour this dark.
		constexpr float DARK_TINT_THRESHOLD = 0.35f;

		// Clip space runs -1 to 1, so an extent of 2 is the whole viewport. Anything past
		// this is covering well over half of it.
		constexpr float LARGE_DRAW_EXTENT = 1.2f;

		// A projection whose implied size is off the viewport's by more than this ratio
		// is stretching what it draws.
		constexpr float STRETCH_MINIMUM = 0.9f;
		constexpr float STRETCH_MAXIMUM = 1.1f;

		// A projection scale this close to zero is perspective or degenerate.
		constexpr float SCALE_EPSILON = 1e-6f;

		// Periodic captures: a frame every this many frames, and the saved scene halfway
		// between.
		constexpr uint32_t CAPTURE_INTERVAL_FRAMES = 2000;
		constexpr uint32_t REGION_CAPTURE_OFFSET   = 1000;

		// The FNV-1a constants, for hashing the projection.
		constexpr uint32_t FNV_OFFSET_BASIS = 2166136261u;
		constexpr uint32_t FNV_PRIME        = 16777619u;

		// The Scroll Lock capture takes one kind a frame, counting down from the last.
		constexpr int KEY_CAPTURE_STEPS = 3;
		constexpr char const* KEY_CAPTURE_KINDS[KEY_CAPTURE_STEPS] = { "depth.raw", "region.bmp", "frame.bmp" };
	}

	//// Private Functions

	bool cVKDriver::NoteOnce(uint32_t bucket, uint32_t key)
	{
		uint64_t const entry = (uint64_t{ bucket } << 32) | key;

		for (uint32_t i = 0; i < notedCount; i++)
		{
			if (notedKeys[i] == entry)
			{
				return false;
			}
		}

		if (notedCount >= _countof(notedKeys))
		{
			return false;
		}

		notedKeys[notedCount++] = entry;
		return true;
	}

	bool cVKDriver::IsSubViewport(void) const
	{
		return viewportX != 0 || viewportY != 0 || viewportWidth != windowWidth || viewportHeight != windowHeight;
	}

	uint8_t const* cVKDriver::VertexAt(int32_t first, void const* indices, bool isIndex32Bit, int i) const
	{
		// Find the vertex number
		//
		// The index array is untyped; its width says how to read it. Without one the
		// vertices run on from the first, which the draw checked is not negative.
		size_t index;

		if (indices == nullptr)
		{
			index = static_cast<size_t>(first + i);
		}
		else if (isIndex32Bit)
		{
			index = static_cast<uint32_t const*>(indices)[i];
		}
		else
		{
			index = static_cast<uint16_t const*>(indices)[i];
		}

		// The game's vertices are untyped bytes.
		return static_cast<uint8_t const*>(vertexPointer) + index * vertexStride;
	}

	void cVKDriver::ProjectVertex(uint8_t const* vertex, float outClip[4]) const
	{
		// Every vertex format starts with three floats of position.
		float const* const position = reinterpret_cast<float const*>(vertex);

		// Transform into eye space
		float eye[4] = { 0.0f, 0.0f, 0.0f, 0.0f };
		for (int row = 0; row < 4; row++)
		{
			float sum = 0.0f;
			for (int k = 0; k < 4; k++)
			{
				sum += modelViewMatrix[k * 4 + row] * ((k < 3) ? position[k] : 1.0f);
			}

			eye[row] = sum;
		}

		// Then into clip space
		for (int row = 0; row < 4; row++)
		{
			float sum = 0.0f;
			for (int k = 0; k < 4; k++)
			{
				sum += projectionMatrix[k * 4 + row] * eye[k];
			}

			outClip[row] = sum;
		}
	}

	void cVKDriver::ProbeDrawArrays(uint32_t gdPrimitiveType, int32_t first, int32_t count)
	{
		// Key the draw on its format, primitive type and projection
		//
		// Sampling the first few draws only reported tiny sub-pixel quads. Sampling per
		// format and primitive was better but still only covered the startup screen,
		// because later screens reuse the same formats and nothing new was ever recorded.
		// The projection is what actually distinguishes one rendering context from
		// another here, so it belongs in the key. It is quantised, so floating point
		// noise does not make every frame look like a new context, and hashed as raw
		// bits.
		uint32_t projectionHash = FNV_OFFSET_BASIS;
		for (int i = 0; i < 16; i++)
		{
			int32_t const quantised = static_cast<int32_t>(projectionMatrix[i] * 1000.0f);
			projectionHash = (projectionHash ^ static_cast<uint32_t>(quantised)) * FNV_PRIME;
		}

		uint32_t const probeKey = (vertexFormat << 8) ^ (gdPrimitiveType & 0xFF) ^ (projectionHash & 0xFFFF0000u);

		for (uint32_t i = 0; i < probedCombinations; i++)
		{
			if (probedKeys[i] == probeKey)
			{
				return;
			}
		}

		if (probedCombinations >= _countof(probedKeys))
		{
			return;
		}

		probedKeys[probedCombinations++] = probeKey;

		// Write both matrices, column by column
		//
		// If geometry appears at the wrong scale this is where the answer is: the
		// positions the game submits are small world-space values and mean nothing
		// without the projection that maps them.
		LogNote("  DrawArrays prim %u, %d vertices, format 0x%x stride %u, viewport %d,%d %dx%d:", gdPrimitiveType, count, vertexFormat, vertexStride, viewportX, viewportY, viewportWidth, viewportHeight);

		float const* const modelView = modelViewMatrix;
		float const* const projection = projectionMatrix;

		LogNote("    modelview  [%.3f %.3f %.3f %.3f] [%.3f %.3f %.3f %.3f] [%.3f %.3f %.3f %.3f] [%.3f %.3f %.3f %.3f]", modelView[0], modelView[1], modelView[2], modelView[3], modelView[4], modelView[5], modelView[6], modelView[7], modelView[8], modelView[9], modelView[10], modelView[11], modelView[12], modelView[13], modelView[14], modelView[15]);
		LogNote("    projection [%.3f %.3f %.3f %.3f] [%.3f %.3f %.3f %.3f] [%.3f %.3f %.3f %.3f] [%.3f %.3f %.3f %.3f]", projection[0], projection[1], projection[2], projection[3], projection[4], projection[5], projection[6], projection[7], projection[8], projection[9], projection[10], projection[11], projection[12], projection[13], projection[14], projection[15]);

		// Write the first few vertices
		//
		// Texture coordinates matter as much as positions here: if the content looks
		// magnified, either the quad is too big or the coordinates cover too little of
		// the texture, and these two numbers tell those apart. Positions and coordinates
		// are floats inside the untyped vertex.
		int const shown = (count < PROBED_VERTICES) ? count : PROBED_VERTICES;
		bool const hasTextureCoordinates = RZVertexFormatNumElements(vertexFormat, kGDElementType_TexCoord) != 0;

		for (int i = 0; i < shown; i++)
		{
			uint8_t const* const vertex = VertexAt(first, nullptr, false, i);
			float const* const position = reinterpret_cast<float const*>(vertex);

			if (hasTextureCoordinates)
			{
				uint32_t const coordinateOffset = RZVertexFormatElementOffset(vertexFormat, kGDElementType_TexCoord, 0);
				float const* const coordinates = reinterpret_cast<float const*>(vertex + coordinateOffset);

				LogNote("    v%d pos %.3f %.3f %.3f  uv %.4f %.4f", i, position[0], position[1], position[2], coordinates[0], coordinates[1]);
			}
			else
			{
				uint8_t const* const colour = vertex + 12;
				LogNote("    v%d pos %.3f %.3f %.3f  colour %3u %3u %3u %3u", i, position[0], position[1], position[2], colour[0], colour[1], colour[2], colour[3]);
			}
		}
	}

	void cVKDriver::ReportLargeDraw(uint32_t gdPrimitiveType, int32_t first, int32_t count)
	{
		// Measure how much of its viewport a draw actually covers
		//
		// Reasoning about matrices repeatedly failed to find why the interface rendered
		// too large, and the projections all turned out to agree with their viewports. So
		// this stops inferring and measures: transform the vertices the way the shader
		// will, and report any draw that ends up covering most of the screen. Whatever is
		// painting over everything will name itself.
		if (coverageReportsRemaining <= 0 || count < 3)
		{
			return;
		}

		float minimumX = 1e30f, maximumX = -1e30f;
		float minimumY = 1e30f, maximumY = -1e30f;

		int const sampled = (count < SAMPLED_VERTICES) ? count : SAMPLED_VERTICES;
		for (int i = 0; i < sampled; i++)
		{
			float clip[4];
			ProjectVertex(VertexAt(first, nullptr, false, i), clip);

			if (clip[3] > -CLIP_W_EPSILON && clip[3] < CLIP_W_EPSILON)
			{
				return;
			}

			float const x = clip[0] / clip[3];
			float const y = clip[1] / clip[3];

			minimumX = (x < minimumX) ? x : minimumX;
			maximumX = (x > maximumX) ? x : maximumX;
			minimumY = (y < minimumY) ? y : minimumY;
			maximumY = (y > maximumY) ? y : maximumY;
		}

		// Report it if it covers more than about half the viewport
		if ((maximumX - minimumX) <= LARGE_DRAW_EXTENT && (maximumY - minimumY) <= LARGE_DRAW_EXTENT)
		{
			return;
		}

		coverageReportsRemaining--;
		LogNote("  LARGE DRAW: covers ndc x %.2f..%.2f y %.2f..%.2f of viewport %d,%d %dx%d, texture %u, format 0x%x prim %u, %d vertices", minimumX, maximumX, minimumY, maximumY, viewportX, viewportY, viewportWidth, viewportHeight, boundTexture, vertexFormat, gdPrimitiveType, count);
	}

	void cVKDriver::ReportProjectionMismatch(uint32_t gdPrimitiveType, int32_t count)
	{
		// Detect a projection that disagrees with the viewport it is drawn into
		//
		// An orthographic projection's x scale is 2 divided by the width of the region it
		// maps onto the whole of clip space. If that width is not the viewport's width,
		// the draw is stretched by exactly their ratio, which was the magnification being
		// chased.
		if (mismatchReportsRemaining <= 0 || viewportWidth <= 0 || viewportHeight <= 0)
		{
			return;
		}

		float const scaleX = projectionMatrix[0] < 0.0f ? -projectionMatrix[0] : projectionMatrix[0];
		float const scaleY = projectionMatrix[5] < 0.0f ? -projectionMatrix[5] : projectionMatrix[5];

		// A near-zero scale means a perspective or degenerate projection, where this
		// reasoning does not apply.
		if (scaleX <= SCALE_EPSILON || scaleY <= SCALE_EPSILON)
		{
			return;
		}

		// Compare the size it implies with the viewport's
		//
		// Viewport sizes are pixel counts, which convert to float exactly.
		float const impliedWidth  = 2.0f / scaleX;
		float const impliedHeight = 2.0f / scaleY;

		float const widthRatio  = impliedWidth / static_cast<float>(viewportWidth);
		float const heightRatio = impliedHeight / static_cast<float>(viewportHeight);

		bool const isStretched = widthRatio < STRETCH_MINIMUM || widthRatio > STRETCH_MAXIMUM || heightRatio < STRETCH_MINIMUM || heightRatio > STRETCH_MAXIMUM;

		if (!isStretched)
		{
			return;
		}

		mismatchReportsRemaining--;
		LogNote("  MISMATCH: projection covers %.1fx%.1f but viewport is %dx%d at %d,%d (stretched %.2fx by %.2fx), format 0x%x prim %u, %d vertices", impliedWidth, impliedHeight, viewportWidth, viewportHeight, viewportX, viewportY, widthRatio, heightRatio, vertexFormat, gdPrimitiveType, count);
	}

	void cVKDriver::NoteDarkTintedDraw(uint32_t gdPrimitiveType, int32_t count)
	{
		// The cloud shadow pass names itself by its tint.
		//
		// Shadows are the only thing drawn with a near black ambient colour, so this
		// catches them wherever in the frame they happen to be, which the frame dump
		// cannot: the shadows move every frame while the terrain under them is restored
		// from a buffer region, so the two are almost never in the same dumped frame.
		if (colourMultiplier[0] > DARK_TINT_THRESHOLD || colourMultiplier[1] > DARK_TINT_THRESHOLD || colourMultiplier[2] > DARK_TINT_THRESHOLD)
		{
			return;
		}

		bool const isBlending     = isCapabilityEnabled[kGDCapability_Blend];
		bool const isAlphaTesting = isCapabilityEnabled[kGDCapability_AlphaTest];

		uint32_t const key = (vertexFormat << 12) ^ (gdPrimitiveType << 8) ^ (isBlending ? 0x80u : 0u) ^ (blendSourceFactor << 4) ^ blendDestinationFactor ^ (isAlphaTesting ? 0x40000u : 0u) ^ (alphaComparison << 20);

		if (!NoteOnce(NOTE_CLOUD_SHADOW, key))
		{
			return;
		}

		LogNote("  SHADOW fmt 0x%x prim %u n=%d  tex %u/%u  blend %d(%u,%u)  alphatest %d func %u@%.2f  tint %.3f %.3f %.3f a %.3f  env %d  depth test %d write %d  stage1 on %d  coordsrc %u/%u", vertexFormat, gdPrimitiveType, count, boundTexture, stage1Texture, isBlending ? 1 : 0, blendSourceFactor, blendDestinationFactor, isAlphaTesting ? 1 : 0, alphaComparison, alphaReference, colourMultiplier[0], colourMultiplier[1], colourMultiplier[2], colourMultiplier[3], textureEnvironmentMode[0], isCapabilityEnabled[kGDCapability_DepthTest] ? 1 : 0, isDepthWriteEnabled ? 1 : 0, isTextureStageEnabled[1] ? 1 : 0, textureCoordinateSource[0], textureCoordinateSource[1]);

		vulkan->LogTextureInformation(boundTexture, "shadow stage 0");
	}

	void cVKDriver::NoteMultitexturedDraw(uint32_t gdVertexFormat)
	{
		if (RZVertexFormatNumElements(gdVertexFormat, kGDElementType_TexCoord) < 2)
		{
			return;
		}

		// Key on the combiner rather than the format
		//
		// The format is already reported on its own, and what matters here is which of
		// the configurations is the one the terrain actually draws with. This runs per
		// draw, and the configuration almost never changes between two of them, so the
		// repeat is caught before the search.
		uint32_t const key = packedCombiner[0] ^ (packedCombiner[1] << 1) ^ (packedCombiner[2] << 2) ^ (packedCombiner[3] << 3);

		if (key == lastMultitextureKey)
		{
			return;
		}

		lastMultitextureKey = key;

		if (NoteOnce(NOTE_MULTITEXTURE, key))
		{
			LogNote("  MULTITEX format 0x%x: stage 0 rgb 0x%05x alpha 0x%05x, stage 1 rgb 0x%05x alpha 0x%05x", gdVertexFormat, packedCombiner[0], packedCombiner[1], packedCombiner[2], packedCombiner[3]);
		}
	}

	void cVKDriver::MaybeArmDump(void)
	{
		// Wait for the base terrain pass
		//
		// It arms the dump, which then runs for the rest of that frame so the whole scene
		// build is captured together. Two coordinate sets alone is not enough to identify
		// it: the cloud shadows carry two as well and redraw every frame over the
		// restored buffer region, so arming on those caught a frame holding nothing but
		// shadows and interface. The shadows are the pass that generates its coordinates,
		// so requiring vertex coordinates separates the two. The second texture stage
		// does not: the game leaves it bound but disabled, and the terrain draws single
		// stage.
		if (!isDumpArmed || isDumpingFrame || IsGeneratingCoordinates(0) || RZVertexFormatNumElements(vertexFormat, kGDElementType_TexCoord) < 2)
		{
			return;
		}

		// Wait for a partial update
		//
		// It is recognised by drawing under a sub-viewport in a frame that already
		// restored the whole scene. Waiting for one costs nothing: the dump stays armed.
		if (!hasRegionFrameRestored || !IsSubViewport())
		{
			return;
		}

		isDumpArmed    = false;
		isDumpingFrame = true;
		dumpedDraws    = 0;
		LogNote("=== dumping the partial update of frame %u, sub-viewport %d,%d %dx%d ===", frameCounter, viewportX, viewportY, viewportWidth, viewportHeight);
	}

	void cVKDriver::DumpDraw(uint32_t gdPrimitiveType, int32_t count, int32_t first, void const* indices, bool isIndex32Bit)
	{
		// Records every draw of one frame with the pixel rectangle it lands on, so the
		// frame can be reconstructed from the log and compared against the capture of
		// that same frame.
		//
		// Reached from both draw paths. It used to live in DrawArrays alone, which made
		// it structurally blind to the terrain: that goes through DrawElements, so no
		// dump ever contained a single terrain draw however long the window was left
		// open.
		if (!isDumpingFrame || count <= 0 || vertexPointer == nullptr || vertexStride == 0)
		{
			return;
		}

		if (dumpedDraws >= MAXIMUM_DUMPED_DRAWS)
		{
			return;
		}

		if (NoteOnce(NOTE_TERRAIN_TEXTURE, boundTexture))
		{
			vulkan->LogTextureInformation(boundTexture, "terrain pass");
		}

		// Measure where the first few vertices land in normalised device coordinates
		int const sampled = (count < SAMPLED_VERTICES) ? count : SAMPLED_VERTICES;

		float minimumX = 1e30f, maximumX = -1e30f;
		float minimumY = 1e30f, maximumY = -1e30f;

		for (int i = 0; i < sampled; i++)
		{
			float clip[4];
			ProjectVertex(VertexAt(first, indices, isIndex32Bit, i), clip);

			if (clip[3] > -CLIP_W_EPSILON && clip[3] < CLIP_W_EPSILON)
			{
				LogNote("  draw %3d: degenerate transform  tex %u fmt 0x%x prim %u n=%d", dumpedDraws++, boundTexture, vertexFormat, gdPrimitiveType, count);
				return;
			}

			float const x = clip[0] / clip[3];
			float const y = clip[1] / clip[3];

			minimumX = (x < minimumX) ? x : minimumX;
			maximumX = (x > maximumX) ? x : maximumX;
			minimumY = (y < minimumY) ? y : minimumY;
			maximumY = (y > maximumY) ? y : maximumY;
		}

		// Map them to pixels
		//
		// The stored viewport y is bottom-origin, as OpenGL has it, and clip space y runs
		// the other way from screen y. Pixel counts convert to float exactly.
		int const viewportDrawWidth  = (viewportWidth > 0) ? viewportWidth : windowWidth;
		int const viewportDrawHeight = (viewportHeight > 0) ? viewportHeight : windowHeight;
		int const viewportLeft       = (viewportWidth > 0) ? viewportX : 0;
		int const viewportTop        = (viewportHeight > 0) ? (windowHeight - viewportY - viewportHeight) : 0;

		float const left   = static_cast<float>(viewportLeft) + (minimumX + 1.0f) * 0.5f * static_cast<float>(viewportDrawWidth);
		float const right  = static_cast<float>(viewportLeft) + (maximumX + 1.0f) * 0.5f * static_cast<float>(viewportDrawWidth);
		float const top    = static_cast<float>(viewportTop) + (1.0f - maximumY) * 0.5f * static_cast<float>(viewportDrawHeight);
		float const bottom = static_cast<float>(viewportTop) + (1.0f - minimumY) * 0.5f * static_cast<float>(viewportDrawHeight);

		// Measure the texture coordinates they cover
		float minimumU = 0.0f, maximumU = 0.0f, minimumV = 0.0f, maximumV = 0.0f;

		if (RZVertexFormatNumElements(vertexFormat, kGDElementType_TexCoord) != 0)
		{
			uint32_t const coordinateOffset = RZVertexFormatElementOffset(vertexFormat, kGDElementType_TexCoord, 0);
			minimumU = minimumV = 1e30f;
			maximumU = maximumV = -1e30f;

			for (int i = 0; i < sampled; i++)
			{
				// Coordinates are floats inside the untyped vertex.
				float const* const coordinates = reinterpret_cast<float const*>(VertexAt(first, indices, isIndex32Bit, i) + coordinateOffset);

				minimumU = (coordinates[0] < minimumU) ? coordinates[0] : minimumU;
				maximumU = (coordinates[0] > maximumU) ? coordinates[0] : maximumU;
				minimumV = (coordinates[1] < minimumV) ? coordinates[1] : minimumV;
				maximumV = (coordinates[1] > maximumV) ? coordinates[1] : maximumV;
			}
		}

		// Read the first vertex's colour
		//
		// That is the primary colour the texture environment starts from. A draw that
		// comes out black has either a black input or a state that discards the input,
		// and the two are told apart here.
		uint32_t colourBytes = 0xffffffffu;

		if (RZVertexFormatNumElements(vertexFormat, kGDElementType_Color) != 0)
		{
			uint32_t const offset = RZVertexFormatElementOffset(vertexFormat, kGDElementType_Color, 0);
			memcpy(&colourBytes, VertexAt(first, indices, isIndex32Bit, 0) + offset, sizeof(colourBytes));
		}

		// Write the draw
		//
		// Blend, alpha test and the second stage are all reported, because a draw that
		// comes out a flat block and a draw that comes out black are both questions about
		// state rather than geometry, and the rectangle alone cannot tell them apart.
		float const diffuseWeight = isVertexColourDiffuse ? diffuseLightFactor : 0.0f;

		LogNote("  draw %3d: screen %.0f,%.0f to %.0f,%.0f (%.0fx%.0f)  tex %u/%u fmt 0x%x prim %u n=%d  vp %d,%d %dx%d  uv %.3f..%.3f,%.3f..%.3f  vcol %02x%02x%02x a%02x  weight %.2f %.2f %.2f a%.2f (vc %d%d)  blend %d(%u,%u) atest %d %u@%.2f  depth %d/%d  env %d  stage1 %d texmat 0x%x", dumpedDraws++, left, top, right, bottom, right - left, bottom - top, boundTexture, stage1Texture, vertexFormat, gdPrimitiveType, count, viewportX, viewportY, viewportWidth, viewportHeight, minimumU, maximumU, minimumV, maximumV, (colourBytes >> 16) & 0xffu, (colourBytes >> 8) & 0xffu, colourBytes & 0xffu, (colourBytes >> 24) & 0xffu, (isVertexColourAmbient ? colourMultiplier[0] : 0.0f) + diffuseWeight, (isVertexColourAmbient ? colourMultiplier[1] : 0.0f) + diffuseWeight, (isVertexColourAmbient ? colourMultiplier[2] : 0.0f) + diffuseWeight, colourMultiplier[3], isVertexColourAmbient ? 1 : 0, isVertexColourDiffuse ? 1 : 0, isCapabilityEnabled[kGDCapability_Blend] ? 1 : 0, blendSourceFactor, blendDestinationFactor, isCapabilityEnabled[kGDCapability_AlphaTest] ? 1 : 0, alphaComparison, alphaReference, isCapabilityEnabled[kGDCapability_DepthTest] ? 1 : 0, isDepthWriteEnabled ? 1 : 0, textureEnvironmentMode[0], isTextureStageEnabled[1] ? 1 : 0, lastTextureMatrixFlags);
	}

	void cVKDriver::SampleWindowDepth(int32_t count, int32_t first, void const* indices, bool isIndex32Bit, float& outMinimumDepth, float& outMaximumDepth) const
	{
		// Start outside the default depth range of 0 to 1
		outMinimumDepth = 2.0f;
		outMaximumDepth = -2.0f;

		int const sampled = (count < SAMPLED_VERTICES) ? count : SAMPLED_VERTICES;

		for (int i = 0; i < sampled; i++)
		{
			// Transform the vertex and skip it if it has no projection
			float clip[4];
			ProjectVertex(VertexAt(first, indices, isIndex32Bit, i), clip);

			if (clip[3] > -CLIP_W_EPSILON && clip[3] < CLIP_W_EPSILON)
			{
				continue;
			}

			// Map its depth into the window range
			float const depth = (clip[2] / clip[3] + 1.0f) * 0.5f;

			outMinimumDepth = (depth < outMinimumDepth) ? depth : outMinimumDepth;
			outMaximumDepth = (depth > outMaximumDepth) ? depth : outMaximumDepth;
		}
	}

	void cVKDriver::NoteRegionStep(char const* format, ...)
	{
		if (regionTraceFrames <= 0)
		{
			return;
		}

		AppendRegionDrawCount();

		if (regionLineCount + 1 > REGION_LINES_PER_FRAME)
		{
			regionLinesDropped++;
			return;
		}

		// Format the step, then buffer it with the viewport in force
		char step[REGION_LINE_LENGTH];

		va_list arguments;
		va_start(arguments, format);
		vsnprintf(step, sizeof(step), format, arguments);
		va_end(arguments);

		sprintf_s(regionLines[regionLineCount++], REGION_LINE_LENGTH, "  %s  (viewport %d,%d %dx%d)", step, viewportX, viewportY, viewportWidth, viewportHeight);
	}

	void cVKDriver::AppendRegionDrawCount(void)
	{
		if (regionDrawsSinceStep == 0)
		{
			return;
		}

		// Buffer the count, with the sub-viewport the draws were under
		if (regionLineCount + 1 > REGION_LINES_PER_FRAME)
		{
			regionLinesDropped++;
		}
		else if (regionSubViewport[2] > 0)
		{
			sprintf_s(regionLines[regionLineCount++], REGION_LINE_LENGTH, "    %u draws, last sub-viewport %d,%d %dx%d", regionDrawsSinceStep, regionSubViewport[0], regionSubViewport[1], regionSubViewport[2], regionSubViewport[3]);
		}
		else
		{
			sprintf_s(regionLines[regionLineCount++], REGION_LINE_LENGTH, "    %u draws, no sub-viewport", regionDrawsSinceStep);
		}

		// Start counting again
		regionDrawsSinceStep = 0;
		regionSubViewport[2] = 0;
	}

	void cVKDriver::AttachRegionPending(void)
	{
		if (regionTraceFrames <= 0 || regionPendingCount == 0)
		{
			return;
		}

		AppendRegionDrawCount();

		// Buffer a heading, then the pending draws as far as there is room
		if (regionLineCount + 1 <= REGION_LINES_PER_FRAME)
		{
			sprintf_s(regionLines[regionLineCount++], REGION_LINE_LENGTH, "      first %d of %d draws under that sub-viewport:", regionPendingCount, regionPendingTotal);
		}

		for (int i = 0; i < regionPendingCount; i++)
		{
			if (regionLineCount + 1 > REGION_LINES_PER_FRAME)
			{
				regionLinesDropped++;
				break;
			}

			memcpy(regionLines[regionLineCount++], regionPending[i], REGION_LINE_LENGTH);
		}

		regionPendingCount = 0;
		regionPendingTotal = 0;
	}

	void cVKDriver::NoteRegionDraw(uint32_t gdPrimitiveType, int32_t count, int32_t first, void const* indices, bool isIndex32Bit)
	{
		NoteTileDraw(count, first, indices, isIndex32Bit);

		if (regionTraceFrames <= 0)
		{
			return;
		}

		// Count the draw, and track the sub-viewport it is under
		regionDrawsSinceStep++;

		if (!IsSubViewport())
		{
			return;
		}

		regionSubViewport[0] = viewportX;
		regionSubViewport[1] = viewportY;
		regionSubViewport[2] = viewportWidth;
		regionSubViewport[3] = viewportHeight;

		regionPendingTotal++;

		if (!hasRegionFrameRestored || regionPendingCount >= REGION_PENDING_DRAWS)
		{
			return;
		}

		// Describe it for the next save
		float minimumDepth;
		float maximumDepth;
		SampleWindowDepth(count, first, indices, isIndex32Bit, minimumDepth, maximumDepth);

		sprintf_s(regionPending[regionPendingCount++], REGION_LINE_LENGTH, "        fmt 0x%x prim %u n=%d  tex %u%s / %u%s  blend %d(%u,%u)  atest %d %u@%.2f  depth %d/%d func %u  cw %d  env %d/%d  tint %.2f %.2f %.2f a %.2f  z %.6f..%.6f", vertexFormat, gdPrimitiveType, count, boundTexture, isTextureStageEnabled[0] ? "" : " (off)", stage1Texture, isTextureStageEnabled[1] ? "" : " (off)", isCapabilityEnabled[kGDCapability_Blend] ? 1 : 0, blendSourceFactor, blendDestinationFactor, isCapabilityEnabled[kGDCapability_AlphaTest] ? 1 : 0, alphaComparison, alphaReference, isCapabilityEnabled[kGDCapability_DepthTest] ? 1 : 0, isDepthWriteEnabled ? 1 : 0, depthComparison, isColourWriteEnabled ? 1 : 0, textureEnvironmentMode[0], textureEnvironmentMode[1], colourMultiplier[0], colourMultiplier[1], colourMultiplier[2], colourMultiplier[3], minimumDepth, maximumDepth);
	}

	void cVKDriver::NoteRegionSubViewport(void)
	{
		regionPendingCount = 0;
		regionPendingTotal = 0;
	}

	void cVKDriver::FlushRegionTrace(void)
	{
		// Write out the frame's steps if it saved part of the scene
		if (regionTraceFrames > 0 && isRegionFrameInteresting)
		{
			regionTraceFrames--;

			LogNote("  REGION frame %u, %d steps%s:", frameCounter, regionLineCount, regionLinesDropped > 0 ? " (some dropped)" : "");

			for (int i = 0; i < regionLineCount; i++)
			{
				LogNote("  REGION %s", regionLines[i]);
			}

			if (regionDrawsSinceStep > 0)
			{
				LogNote("  REGION     %u draws before the frame ended", regionDrawsSinceStep);
			}
		}

		// Start the next frame's afresh
		ResetTile();

		regionLineCount          = 0;
		regionLinesDropped       = 0;
		isRegionFrameInteresting = false;
		hasRegionFrameRestored   = false;
		regionDrawsSinceStep     = 0;
		regionSubViewport[2]     = 0;
		regionPendingCount       = 0;
		regionPendingTotal       = 0;
	}

	bool cVKDriver::IsFullWindowCopy(int32_t x, int32_t y, int32_t width, int32_t height, int32_t screenX, int32_t screenY) const
	{
		return x == 0 && y == 0 && screenX == 0 && screenY == 0 && width == windowWidth && height == windowHeight;
	}

	void cVKDriver::NoteTileDraw(int32_t count, int32_t first, void const* indices, bool isIndex32Bit)
	{
		// Count a whole-window draw and stop there
		if (!IsSubViewport())
		{
			currentTile.fullViewportDraws++;
			return;
		}

		currentTile.subViewportDraws++;
		currentTile.subViewport[0] = viewportX;
		currentTile.subViewport[1] = viewportY;
		currentTile.subViewport[2] = viewportWidth;
		currentTile.subViewport[3] = viewportHeight;

		// Key the draw on everything that decides whether it can land on the screen
		uint32_t const key = (vertexFormat & 0xffu) | ((isCapabilityEnabled[kGDCapability_Blend] ? 1u : 0u) << 8) | ((blendSourceFactor & 0xfu) << 9) | ((blendDestinationFactor & 0xfu) << 13) | ((isCapabilityEnabled[kGDCapability_DepthTest] ? 1u : 0u) << 17) | ((isDepthWriteEnabled ? 1u : 0u) << 18) | ((depthComparison & 7u) << 19) | ((isColourWriteEnabled ? 1u : 0u) << 22) | ((isTextureStageEnabled[0] ? 1u : 0u) << 23) | ((isTextureStageEnabled[1] ? 1u : 0u) << 24) | ((IsCloudShadowDraw() ? 1u : 0u) << 25) | ((isCapabilityEnabled[kGDCapability_AlphaTest] ? 1u : 0u) << 26);

		float minimumDepth;
		float maximumDepth;
		SampleWindowDepth(count, first, indices, isIndex32Bit, minimumDepth, maximumDepth);

		// Add it to its class, or start a class for it
		for (int i = 0; i < currentTile.classCount; i++)
		{
			TileClass& entry = currentTile.classes[i];

			if (entry.key == key)
			{
				entry.count++;
				entry.minimumDepth = (minimumDepth < entry.minimumDepth) ? minimumDepth : entry.minimumDepth;
				entry.maximumDepth = (maximumDepth > entry.maximumDepth) ? maximumDepth : entry.maximumDepth;
				return;
			}
		}

		if (currentTile.classCount >= TileRecord::CLASS_LIMIT)
		{
			currentTile.unclassifiedDraws++;
			return;
		}

		TileClass& entry   = currentTile.classes[currentTile.classCount++];
		entry.key          = key;
		entry.count        = 1;
		entry.firstTexture = boundTexture;
		entry.minimumDepth = minimumDepth;
		entry.maximumDepth = maximumDepth;
	}

	void cVKDriver::NoteTileSave(int32_t x, int32_t y, int32_t width, int32_t height)
	{
		// The colour and depth saves come as a pair with nothing drawn between them, so
		// only the first one closes a tile.
		if (currentTile.subViewportDraws == 0)
		{
			return;
		}

		// Close the tile and keep it in the ring
		currentTile.frame            = frameCounter;
		currentTile.saveRectangle[0] = x;
		currentTile.saveRectangle[1] = y;
		currentTile.saveRectangle[2] = width;
		currentTile.saveRectangle[3] = height;
		currentTile.hazards          = vulkan->TextureHazardCount() - tileHazardsAtStart;

		tileRing[tileRingNext] = currentTile;
		tileRingNext = (tileRingNext + 1) % TILE_RING_SIZE;

		if (tileRingCount < TILE_RING_SIZE)
		{
			tileRingCount++;
		}

		ResetTile();
	}

	void cVKDriver::ResetTile(void)
	{
		currentTile.subViewportDraws  = 0;
		currentTile.fullViewportDraws = 0;
		currentTile.unclassifiedDraws = 0;
		currentTile.classCount        = 0;
		currentTile.subViewport[2]    = 0;
		tileHazardsAtStart            = vulkan->TextureHazardCount();
	}

	void cVKDriver::DumpTileRing(void)
	{
		LogNote("=== last %u saved tiles, oldest first ===", tileRingCount);

		uint32_t const start = (tileRingNext + TILE_RING_SIZE - tileRingCount) % TILE_RING_SIZE;

		for (uint32_t n = 0; n < tileRingCount; n++)
		{
			// Write the tile
			TileRecord const& tile = tileRing[(start + n) % TILE_RING_SIZE];

			LogNote("  TILE frame %u  save %d,%d %dx%d  sub %d,%d %dx%d  draws %u sub, %u full  hazards %llu%s", tile.frame, tile.saveRectangle[0], tile.saveRectangle[1], tile.saveRectangle[2], tile.saveRectangle[3], tile.subViewport[0], tile.subViewport[1], tile.subViewport[2], tile.subViewport[3], tile.subViewportDraws, tile.fullViewportDraws, tile.hazards, tile.unclassifiedDraws > 0 ? "  (classes overflowed)" : "");

			// Write its classes, unpacking each key
			for (int i = 0; i < tile.classCount; i++)
			{
				TileClass const& entry = tile.classes[i];
				uint32_t const key = entry.key;

				LogNote("    n=%-5u fmt 0x%-2x blend %u(%u,%u) depth %u/%u func %u cw %u tex %u/%u gen %u atest %u  first tex %u  z %.5f..%.5f", entry.count, key & 0xffu, (key >> 8) & 1u, (key >> 9) & 0xfu, (key >> 13) & 0xfu, (key >> 17) & 1u, (key >> 18) & 1u, (key >> 19) & 7u, (key >> 22) & 1u, (key >> 23) & 1u, (key >> 24) & 1u, (key >> 25) & 1u, (key >> 26) & 1u, entry.firstTexture, entry.minimumDepth, entry.maximumDepth);
			}
		}

		LogNote("=== end of saved tiles ===");
	}

	void cVKDriver::EndFrameDiagnostics(void)
	{
		// Close a dump that ran this frame
		//
		// A dump is held open across a window of frames rather than one. A single frame
		// is almost never the one worth seeing: the city draws its terrain once into a
		// buffer region and restores it every frame after, so an arbitrary frame holds
		// nothing but interface, and the first attempt at this caught 38 draws, all of
		// them toolbar. The redraws that actually build the scene are sparse, so the
		// window stays open until one of them turns up.
		if (isDumpingFrame)
		{
			LogNote("=== end of frame dump, %d draws ===", dumpedDraws);
			isDumpingFrame = false;
		}

		if (isDumpArmed && --dumpWindowRemaining <= 0)
		{
			isDumpArmed = false;
		}

		// Close the trace and move to the next frame
		FlushRegionTrace();
		frameCounter++;

		RequestPeriodicCaptures();
		PollKeyCapture();
	}

	void cVKDriver::RequestPeriodicCaptures(void)
	{
		// Spread across the session rather than at fixed early points
		//
		// The first attempt dumped frames 600 and 3000, and both landed while the startup
		// screen was still up: it runs at well over a thousand frames a second, so those
		// were the same two seconds of a session lasting tens of thousands of frames.
		// Sampling periodically covers whatever the game is actually showing later.
		if (frameDumpsRemaining <= 0)
		{
			return;
		}

		char name[64];
		char path[MAX_PATH];

		// Arm a dump and capture the same frame
		//
		// The picture lets the rectangles in the log be checked against actual pixels
		// rather than against a screenshot taken at some other moment.
		if (frameCounter % CAPTURE_INTERVAL_FRAMES == 0)
		{
			frameDumpsRemaining--;
			isDumpArmed         = true;
			dumpWindowRemaining = DUMP_WINDOW_FRAMES;

			sprintf_s(name, sizeof(name), "scvk-frame-%u.bmp", frameCounter);
			if (LogFilePath(name, path, sizeof(path)))
			{
				vulkan->RequestCapture(path);
			}
		}

		// Capture the saved scene halfway between
		//
		// So a patch that is baked into it can be told from one drawn over it each frame.
		if (frameCounter % CAPTURE_INTERVAL_FRAMES == REGION_CAPTURE_OFFSET)
		{
			sprintf_s(name, sizeof(name), "scvk-region-%u.bmp", frameCounter);
			if (LogFilePath(name, path, sizeof(path)))
			{
				vulkan->RequestRegionCapture(path);
			}
		}
	}

	void cVKDriver::PollKeyCapture(void)
	{
		// Start a capture on a fresh press
		//
		// The periodic captures rarely land on a black patch, so this takes one when the
		// user can see it. Only while the game has the focus, so the key does nothing
		// when pressed in another window.
		DWORD foregroundProcess = 0;
		GetWindowThreadProcessId(GetForegroundWindow(), &foregroundProcess);

		bool const isHeld = foregroundProcess == GetCurrentProcessId() && (GetAsyncKeyState(VK_SCROLL) & 0x8000) != 0;

		if (isHeld && !isCaptureKeyHeld && keyCaptureStep == 0)
		{
			keyCaptureCount++;
			keyCaptureStep = KEY_CAPTURE_STEPS;
			LogNote("Diagnostic: Scroll Lock capture %u.", keyCaptureCount);
			DumpTileRing();
		}

		isCaptureKeyHeld = isHeld;

		if (keyCaptureStep == 0)
		{
			return;
		}

		// Take this frame's capture
		//
		// One readback buffer, so one capture a frame: the screen first, then the colour
		// and depth the game restores every frame.
		char name[64];
		sprintf_s(name, sizeof(name), "scvk-key-%u-%s", keyCaptureCount, KEY_CAPTURE_KINDS[keyCaptureStep - 1]);

		char path[MAX_PATH];
		if (LogFilePath(name, path, sizeof(path)))
		{
			if (keyCaptureStep == 3)
			{
				vulkan->RequestCapture(path);
			}
			else
			{
				vulkan->RequestRegionCapture(path, keyCaptureStep == 1);
			}
		}

		keyCaptureStep--;
	}
}
