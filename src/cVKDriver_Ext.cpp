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
 * The optional driver extensions.
 *
 * Buffer regions turned out not to be optional. They read as an incremental
 * redraw optimisation, where the game saves part of the framebuffer and puts it
 * back next frame rather than drawing it again, so they were declined while the
 * draw path was still being built. But a city trace showed the game asking for
 * a region 633 times, being refused every time, and simply never drawing the
 * terrain: it renders the city once into a region and restores it from there
 * every frame, and with nowhere to save to, it draws nothing at all. The UI
 * still appeared because the UI is drawn directly.
 *
 * Note also that declining was never as decisive as it looked. The game calls
 * NewBufferRegion straight after querying the extension without ever calling
 * BufferRegionEnabled to ask whether it is available, so this interface has to
 * stay safe when called rather than merely consistent.
 *
 * Lighting is present because the interface requires it, though SimCity 4 does
 * not appear to drive it directly - it expects a single directional light to
 * have been set up by the driver itself.
 */

#include "cVKDriver.h"
#include "Logger.h"
#include "VulkanBackend.h"

#include <cstdarg>
#include <cstdio>
#include <cstring>

namespace scvk
{
	// -------------------------------------------------------------------
	// cIGZGBufferRegionExtension
	// -------------------------------------------------------------------

	bool cVKDriver::BufferRegionEnabled(void)
	{
		SCVK_CALL("");
		return true;
	}

	uint32_t cVKDriver::NewBufferRegion(int32_t gdBufferRegionType)
	{
		SCVK_CALL("%d", gdBufferRegionType);

		// Type 0 is the back colour buffer and type 1 is the depth buffer.
		// Anything else is not something this interface can express.
		if (gdBufferRegionType != 0 && gdBufferRegionType != 1)
		{
			return 0;
		}

		return vulkan->CreateBufferRegion(gdBufferRegionType == 1);
	}

	bool cVKDriver::DeleteBufferRegion(int32_t bufferRegion)
	{
		SCVK_CALL("%d", bufferRegion);

		if (bufferRegion <= 0)
		{
			return false;
		}

		vulkan->DestroyBufferRegion(static_cast<uint32_t>(bufferRegion));
		return true;
	}

	bool cVKDriver::ReadBufferRegion(uint32_t region, int32_t x, int32_t y, int32_t width, int32_t height, int32_t destX, int32_t destY)
	{
		SCVK_CALL("%u, %d,%d %dx%d <- %d,%d", region, x, y, width, height, destX, destY);

		if (!IsFullWindowCopy(x, y, width, height, destX, destY))
		{
			regionFrameInteresting = true;
		}

		AttachRegionPending();
		NoteRegionOp("save r%u region %d,%d %dx%d <- screen %d,%d", region, x, y, width, height, destX, destY);

		if (!IsFullWindowCopy(x, y, width, height, destX, destY))
		{
			NoteTileSave(destX, destY, width, height);
		}

		// Saving: the framebuffer is the source. The first pair of coordinates
		// addresses the region and the last pair addresses the screen, which is
		// the convention the matching draw call uses in reverse.
		return vulkan->SaveBufferRegion(region, x, y, width, height, destX, destY);
	}

	bool cVKDriver::DrawBufferRegion(uint32_t region, int32_t x, int32_t y, int32_t width, int32_t height, int32_t destX, int32_t destY)
	{
		SCVK_CALL("%u, %d,%d %dx%d -> %d,%d", region, x, y, width, height, destX, destY);

		regionFrameRestored = true;
		NoteRegionOp("restore r%u region %d,%d %dx%d -> screen %d,%d", region, x, y, width, height, destX, destY);
		ResetTile();

		// Restoring: the region is the source, at the first pair of
		// coordinates, and the screen is the destination, at the last pair.
		return vulkan->RestoreBufferRegion(region, x, y, width, height, destX, destY);
	}

	bool cVKDriver::IsFullWindowCopy(int32_t x, int32_t y, int32_t width, int32_t height,
		int32_t screenX, int32_t screenY) const
	{
		return x == 0 && y == 0 && screenX == 0 && screenY == 0 &&
			width == windowWidth && height == windowHeight;
	}

	void cVKDriver::NoteRegionOp(char const* fmt, ...)
	{
		if (regionTraceFrames <= 0)
		{
			return;
		}

		AppendRegionDrawCount();

		if (regionLineCount + 1 > kRegionLinesPerFrame)
		{
			regionLinesDropped++;
			return;
		}

		char step[kRegionLineLength];
		va_list args;
		va_start(args, fmt);
		vsnprintf(step, sizeof(step), fmt, args);
		va_end(args);

		sprintf_s(regionLines[regionLineCount++], kRegionLineLength,
			"  %s  (viewport %d,%d %dx%d)", step, viewportX, viewportY, viewportWidth, viewportHeight);
	}

	void cVKDriver::AppendRegionDrawCount(void)
	{
		if (regionDrawsSinceOp == 0)
		{
			return;
		}

		if (regionLineCount + 1 > kRegionLinesPerFrame)
		{
			regionLinesDropped++;
		}
		else if (regionSubViewport[2] > 0)
		{
			sprintf_s(regionLines[regionLineCount++], kRegionLineLength,
				"    %u draws, last sub-viewport %d,%d %dx%d", regionDrawsSinceOp,
				regionSubViewport[0], regionSubViewport[1], regionSubViewport[2], regionSubViewport[3]);
		}
		else
		{
			sprintf_s(regionLines[regionLineCount++], kRegionLineLength,
				"    %u draws, no sub-viewport", regionDrawsSinceOp);
		}

		regionDrawsSinceOp   = 0;
		regionSubViewport[2] = 0;
	}

	void cVKDriver::AttachRegionPending(void)
	{
		if (regionTraceFrames <= 0 || regionPendingCount == 0)
		{
			return;
		}

		AppendRegionDrawCount();

		if (regionLineCount + 1 <= kRegionLinesPerFrame)
		{
			sprintf_s(regionLines[regionLineCount++], kRegionLineLength,
				"      first %d of %d draws under that sub-viewport:", regionPendingCount, regionPendingTotal);
		}

		for (int i = 0; i < regionPendingCount; i++)
		{
			if (regionLineCount + 1 > kRegionLinesPerFrame)
			{
				regionLinesDropped++;
				break;
			}

			memcpy(regionLines[regionLineCount++], regionPending[i], kRegionLineLength);
		}

		regionPendingCount = 0;
		regionPendingTotal = 0;
	}

	void cVKDriver::NoteRegionSubViewport(void)
	{
		regionPendingCount = 0;
		regionPendingTotal = 0;
	}

	void cVKDriver::NoteRegionDraw(uint32_t gdPrimType, int32_t count, int32_t first,
		void const* indices, bool indicesAre32Bit)
	{
		NoteTileDraw(count, first, indices, indicesAre32Bit);

		if (regionTraceFrames <= 0)
		{
			return;
		}

		regionDrawsSinceOp++;

		bool const sub = viewportX != 0 || viewportY != 0 ||
			viewportWidth != windowWidth || viewportHeight != windowHeight;

		if (!sub)
		{
			return;
		}

		regionSubViewport[0] = viewportX;
		regionSubViewport[1] = viewportY;
		regionSubViewport[2] = viewportWidth;
		regionSubViewport[3] = viewportHeight;

		regionPendingTotal++;

		if (!regionFrameRestored || regionPendingCount >= kRegionPendingDraws)
		{
			return;
		}

		float zMin;
		float zMax;
		SampleWindowDepth(count, first, indices, indicesAre32Bit, zMin, zMax);

		sprintf_s(regionPending[regionPendingCount++], kRegionLineLength,
			"        fmt 0x%x prim %u n=%d  tex %u%s / %u%s  blend %d(%u,%u)  atest %d %u@%.2f  depth %d/%d func %u  cw %d  "
			"env %d/%d  tint %.2f %.2f %.2f a %.2f  z %.6f..%.6f",
			vertexFormat, gdPrimType, count,
			boundTexture, texStageEnabled[0] ? "" : " (off)",
			stage1Texture, texStageEnabled[1] ? "" : " (off)",
			enabledCapabilities[kGDCapability_Blend] ? 1 : 0, blendSrcFactor, blendDstFactor,
			enabledCapabilities[kGDCapability_AlphaTest] ? 1 : 0, alphaFunc, alphaRef,
			enabledCapabilities[kGDCapability_DepthTest] ? 1 : 0, depthWrite ? 1 : 0, depthCompare,
			colourWrite ? 1 : 0, texEnvMode[0], texEnvMode[1],
			colourMultiplier[0], colourMultiplier[1], colourMultiplier[2], colourMultiplier[3],
			zMin, zMax);
	}

	void cVKDriver::SampleWindowDepth(int32_t count, int32_t first, void const* indices,
		bool indicesAre32Bit, float& zMin, float& zMax) const
	{
		// With the default depth range.
		zMin = 2.0f;
		zMax = -2.0f;
		int const sampled = (count < 8) ? count : 8;

		for (int i = 0; i < sampled; i++)
		{
			size_t index;

			if (indices == nullptr)
			{
				index = static_cast<size_t>(first + i);
			}
			else if (indicesAre32Bit)
			{
				index = static_cast<uint32_t const*>(indices)[i];
			}
			else
			{
				index = static_cast<uint16_t const*>(indices)[i];
			}

			float const* p = reinterpret_cast<float const*>(
				static_cast<uint8_t const*>(vertexPointer) + index * vertexStride);

			float eye[4] = { 0.0f, 0.0f, 0.0f, 0.0f };
			for (int row = 0; row < 4; row++)
			{
				for (int k = 0; k < 4; k++)
				{
					eye[row] += modelViewMatrix[k * 4 + row] * ((k < 3) ? p[k] : 1.0f);
				}
			}

			float clipZ = 0.0f;
			float clipW = 0.0f;
			for (int k = 0; k < 4; k++)
			{
				clipZ += projectionMatrix[k * 4 + 2] * eye[k];
				clipW += projectionMatrix[k * 4 + 3] * eye[k];
			}

			if (clipW > -1e-6f && clipW < 1e-6f)
			{
				continue;
			}

			float const window = (clipZ / clipW + 1.0f) * 0.5f;
			if (window < zMin) { zMin = window; }
			if (window > zMax) { zMax = window; }
		}
	}

	void cVKDriver::ResetTile(void)
	{
		tileCurrent.subDraws     = 0;
		tileCurrent.fullDraws    = 0;
		tileCurrent.otherClasses = 0;
		tileCurrent.classCount   = 0;
		tileCurrent.sub[2]       = 0;
		tileHazardsAtStart       = vulkan->TextureHazardCount();
	}

	void cVKDriver::NoteTileDraw(int32_t count, int32_t first, void const* indices, bool indicesAre32Bit)
	{
		bool const sub = viewportX != 0 || viewportY != 0 ||
			viewportWidth != windowWidth || viewportHeight != windowHeight;

		if (!sub)
		{
			tileCurrent.fullDraws++;
			return;
		}

		tileCurrent.subDraws++;
		tileCurrent.sub[0] = viewportX;
		tileCurrent.sub[1] = viewportY;
		tileCurrent.sub[2] = viewportWidth;
		tileCurrent.sub[3] = viewportHeight;

		// Everything that decides whether a draw can land on the screen.
		uint32_t const key =
			(vertexFormat & 0xffu) |
			((enabledCapabilities[kGDCapability_Blend] ? 1u : 0u) << 8) |
			((blendSrcFactor & 0xfu) << 9) |
			((blendDstFactor & 0xfu) << 13) |
			((enabledCapabilities[kGDCapability_DepthTest] ? 1u : 0u) << 17) |
			((depthWrite ? 1u : 0u) << 18) |
			((depthCompare & 7u) << 19) |
			((colourWrite ? 1u : 0u) << 22) |
			((texStageEnabled[0] ? 1u : 0u) << 23) |
			((texStageEnabled[1] ? 1u : 0u) << 24) |
			((IsCloudShadowDraw() ? 1u : 0u) << 25) |
			((enabledCapabilities[kGDCapability_AlphaTest] ? 1u : 0u) << 26);

		float zMin;
		float zMax;
		SampleWindowDepth(count, first, indices, indicesAre32Bit, zMin, zMax);

		for (int i = 0; i < tileCurrent.classCount; i++)
		{
			TileClass& entry = tileCurrent.classes[i];

			if (entry.key == key)
			{
				entry.count++;
				if (zMin < entry.zMin) { entry.zMin = zMin; }
				if (zMax > entry.zMax) { entry.zMax = zMax; }
				return;
			}
		}

		if (tileCurrent.classCount >= kTileClasses)
		{
			tileCurrent.otherClasses++;
			return;
		}

		TileClass& entry   = tileCurrent.classes[tileCurrent.classCount++];
		entry.key          = key;
		entry.count        = 1;
		entry.firstTexture = boundTexture;
		entry.zMin         = zMin;
		entry.zMax         = zMax;
	}

	void cVKDriver::NoteTileSave(int32_t x, int32_t y, int32_t width, int32_t height)
	{
		// The colour and depth saves come as a pair with nothing drawn
		// between them, so only the first one closes a tile.
		if (tileCurrent.subDraws == 0)
		{
			return;
		}

		tileCurrent.frame   = frameCounter;
		tileCurrent.save[0] = x;
		tileCurrent.save[1] = y;
		tileCurrent.save[2] = width;
		tileCurrent.save[3] = height;
		tileCurrent.hazards = vulkan->TextureHazardCount() - tileHazardsAtStart;

		tileRing[tileRingNext] = tileCurrent;
		tileRingNext = (tileRingNext + 1) % kTileRing;
		if (tileRingCount < kTileRing) { tileRingCount++; }

		ResetTile();
	}

	void cVKDriver::DumpTileRing(void)
	{
		LogNote("=== last %u saved tiles, oldest first ===", tileRingCount);

		uint32_t const start = (tileRingNext + kTileRing - tileRingCount) % kTileRing;

		for (uint32_t n = 0; n < tileRingCount; n++)
		{
			TileRecord const& tile = tileRing[(start + n) % kTileRing];

			LogNote("  TILE frame %u  save %d,%d %dx%d  sub %d,%d %dx%d  draws %u sub, %u full  hazards %llu%s",
				tile.frame, tile.save[0], tile.save[1], tile.save[2], tile.save[3],
				tile.sub[0], tile.sub[1], tile.sub[2], tile.sub[3],
				tile.subDraws, tile.fullDraws, static_cast<unsigned long long>(tile.hazards),
				tile.otherClasses > 0 ? "  (classes overflowed)" : "");

			for (int i = 0; i < tile.classCount; i++)
			{
				TileClass const& entry = tile.classes[i];
				uint32_t const k = entry.key;

				LogNote("    n=%-5u fmt 0x%-2x blend %u(%u,%u) depth %u/%u func %u cw %u tex %u/%u gen %u atest %u  first tex %u  z %.5f..%.5f",
					entry.count, k & 0xffu, (k >> 8) & 1u, (k >> 9) & 0xfu, (k >> 13) & 0xfu,
					(k >> 17) & 1u, (k >> 18) & 1u, (k >> 19) & 7u, (k >> 22) & 1u,
					(k >> 23) & 1u, (k >> 24) & 1u, (k >> 25) & 1u, (k >> 26) & 1u,
					entry.firstTexture, entry.zMin, entry.zMax);
			}
		}

		LogNote("=== end of saved tiles ===");
	}

	void cVKDriver::FlushRegionTrace(void)
	{
		if (regionTraceFrames > 0 && regionFrameInteresting)
		{
			regionTraceFrames--;

			LogNote("  REGION frame %u, %d steps%s:", frameCounter, regionLineCount,
				regionLinesDropped > 0 ? " (some dropped)" : "");

			for (int i = 0; i < regionLineCount; i++)
			{
				LogNote("  REGION %s", regionLines[i]);
			}

			if (regionDrawsSinceOp > 0)
			{
				LogNote("  REGION     %u draws before the frame ended", regionDrawsSinceOp);
			}
		}

		ResetTile();

		regionLineCount        = 0;
		regionLinesDropped     = 0;
		regionFrameInteresting = false;
		regionFrameRestored    = false;
		regionDrawsSinceOp     = 0;
		regionSubViewport[2]   = 0;
		regionPendingCount     = 0;
		regionPendingTotal     = 0;
	}

	bool cVKDriver::IsBufferRegion(uint32_t bufferRegion)
	{
		SCVK_CALL("%u", bufferRegion);
		return vulkan->IsBufferRegion(bufferRegion);
	}

	bool cVKDriver::CanDoPartialRegionWrites(void)
	{
		SCVK_CALL("");

		// A copy can address any sub-rectangle, so both of these are free.
		return true;
	}

	bool cVKDriver::CanDoOffsetReads(void)
	{
		SCVK_CALL("");
		return true;
	}

	bool cVKDriver::DeleteAllBufferRegions(void)
	{
		SCVK_CALL("");

		vulkan->DestroyAllBufferRegions();
		return true;
	}

	// -------------------------------------------------------------------
	// cIGZGSnapshotExtension
	// -------------------------------------------------------------------

	cIGZBuffer* cVKDriver::CopyColorBuffer(int32_t x, int32_t y, int32_t width, int32_t height, cIGZBuffer* buffer)
	{
		// Nominally an extension, but declining it in QueryInterface crashes
		// the game during load, so it has to exist. With no framebuffer to read
		// from yet, the caller's buffer is handed straight back untouched: a
		// screenshot will be blank rather than the game faulting on a null.
		SCVK_CALL("%d,%d %dx%d, %p", x, y, width, height, buffer);
		return buffer;
	}

	// -------------------------------------------------------------------
	// cIGZGDriverLightingExtension
	// -------------------------------------------------------------------

	void cVKDriver::EnableLighting(bool enabled)
	{
		SCVK_CALL("%d", enabled);
	}

	void cVKDriver::EnableLight(uint32_t light, bool enabled)
	{
		SCVK_CALL("%u, %d", light, enabled);
	}

	void cVKDriver::LightModelAmbient(float r, float g, float b, float a)
	{
		SCVK_CALL("%.3f, %.3f, %.3f, %.3f", r, g, b, a);
	}

	void cVKDriver::LightColor(uint32_t light, uint32_t type, float const* color)
	{
		SCVK_CALL("%u, %u, %p", light, type, color);
	}

	void cVKDriver::LightColor(uint32_t light, float const* ambient, float const* diffuse, float const* specular)
	{
		SCVK_CALL("%u, %p, %p, %p", light, ambient, diffuse, specular);
	}

	void cVKDriver::LightPosition(uint32_t light, float const* position)
	{
		SCVK_CALL("%u, %p", light, position);
	}

	void cVKDriver::LightDirection(uint32_t light, float const* direction)
	{
		SCVK_CALL("%u, %p", light, direction);
	}

	void cVKDriver::MaterialColor(uint32_t type, float const* color)
	{
		SCVK_CALL("%u, %p", type, color);
	}

	void cVKDriver::MaterialColor(float const* ambient, float const* diffuse, float const* specular, float const* emission, float shininess)
	{
		SCVK_CALL("%p, %p, %p, %p, %.3f", ambient, diffuse, specular, emission, shininess);
	}

	// -------------------------------------------------------------------
	// cIGZGDriverVertexBufferExtension
	//
	// Not exposed through QueryInterface yet, so none of this should run. The
	// implementations exist to satisfy the interface and to make it obvious in
	// the trace if that assumption turns out to be wrong.
	// -------------------------------------------------------------------

	char const* cVKDriver::GetVertexBufferName(uint32_t gdVertexFormat)
	{
		SCVK_CALL("0x%x  [UNEXPECTED: extension not exposed]", gdVertexFormat);
		return "scvk";
	}

	uint32_t cVKDriver::VertexBufferType(uint32_t unknown)
	{
		SCVK_CALL("%u  [UNEXPECTED]", unknown);
		return 0;
	}

	uint32_t cVKDriver::MaxVertices(uint32_t unknown)
	{
		SCVK_CALL("%u  [UNEXPECTED]", unknown);
		return 0;
	}

	uint32_t cVKDriver::GetVertices(int32_t count, bool unknown)
	{
		SCVK_CALL("%d, %d  [UNEXPECTED]", count, unknown);
		return 0;
	}

	uint32_t cVKDriver::ContinueVertices(uint32_t unknown, uint32_t unknown2)
	{
		SCVK_CALL("%u, %u  [UNEXPECTED]", unknown, unknown2);
		return 0;
	}

	void cVKDriver::ReleaseVertices(uint32_t unknown)
	{
		SCVK_CALL("%u  [UNEXPECTED]", unknown);
	}

	void cVKDriver::DrawPrims(uint32_t unknown, uint32_t gdPrimType, void* prims, uint32_t count)
	{
		SCVK_CALL("%u, %u, %p, %u  [UNEXPECTED]", unknown, gdPrimType, prims, count);
	}

	void cVKDriver::DrawPrimsIndexed(uint32_t unknown, uint32_t gdPrimType, uint32_t count, uint16_t* indices, void* prims, uint32_t count2)
	{
		SCVK_CALL("%u, %u, %u, %p, %p, %u  [UNEXPECTED]", unknown, gdPrimType, count, indices, prims, count2);
	}

	void cVKDriver::Reset(void)
	{
		SCVK_CALL("  [UNEXPECTED]");
	}
}
