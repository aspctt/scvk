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

		// Saving: the framebuffer is the source. The first pair of coordinates
		// addresses the region and the last pair addresses the screen, which is
		// the convention the matching draw call uses in reverse.
		return vulkan->SaveBufferRegion(region, x, y, width, height, destX, destY);
	}

	bool cVKDriver::DrawBufferRegion(uint32_t region, int32_t x, int32_t y, int32_t width, int32_t height, int32_t destX, int32_t destY)
	{
		SCVK_CALL("%u, %d,%d %dx%d -> %d,%d", region, x, y, width, height, destX, destY);

		// Restoring: the region is the source, at the first pair of
		// coordinates, and the screen is the destination, at the last pair.
		return vulkan->RestoreBufferRegion(region, x, y, width, height, destX, destY);
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
