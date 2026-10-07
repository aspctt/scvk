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
 * Buffer regions turned out not to be optional. They read as an incremental redraw
 * optimisation, where the game saves part of the framebuffer and puts it back next frame
 * rather than drawing it again, so they were declined while the draw path was still being
 * built. But a city trace showed the game asking for a region 633 times, being refused
 * every time, and simply never drawing the terrain: it renders the city once into a
 * region and restores it from there every frame, and with nowhere to save to, it draws
 * nothing at all. The UI still appeared because the UI is drawn directly.
 *
 * Note also that declining was never as decisive as it looked. The game calls
 * NewBufferRegion straight after querying the extension without ever calling
 * BufferRegionEnabled to ask whether it is available, so this interface has to stay safe
 * when called rather than merely consistent.
 *
 * Lighting is present because the interface requires it, though SimCity 4 does not appear
 * to drive it directly - it expects a single directional light to have been set up by the
 * driver itself.
 */

//// Dependencies

#include "cVKDriver.h"
#include "Logger.h"
#include "VulkanBackend.h"

#include <cIGZBuffer.h>
#include <cIGZFrameWork.h>
#include <cIGZGraphicSystem.h>
#include <cRZCOMDllDirector.h>

#include <algorithm>
#include <string.h>

namespace scvk
{
	//// Constants

	namespace
	{
		// The region types the game asks for: the back colour buffer and the depth
		// buffer.
		constexpr int32_t GD_BUFFER_REGION_COLOUR = 0;
		constexpr int32_t GD_BUFFER_REGION_DEPTH  = 1;

		// The graphic system's service ID, as the game itself asks the framework for it
		// (0x66d240) and as SCGL does.
		constexpr uint32_t RZSRVID_GRAPHIC_SYSTEM = 0xC416025C;

		// The flags the DirectX driver locks a snapshot buffer with around its copy
		// (0x883c20): the dirty update the interface names, and 0x80, which it does not.
		constexpr uint32_t SNAPSHOT_LOCK_FLAGS = cIGZBuffer::IsDirtyUpdate | 0x80u;

		// What a snapshot buffer holds where the rectangle ran off the window, in the
		// buffer's A8R8G8B8 byte order.
		constexpr uint8_t OPAQUE_BLACK[4] = { 0x00, 0x00, 0x00, 0xff };
	}

	//// Private Functions

	namespace
	{
		/** A new, uninitialised buffer from the game's graphic system, or null. */
		cIGZBuffer* CreateGameBuffer(void)
		{
			cIGZFrameWork* const frameWork = RZGetFrameWork();
			if (frameWork == nullptr)
			{
				return nullptr;
			}

			// The framework hands the service out as a void pointer to the interface
			// asked for, which is the graphic system's.
			void* service = nullptr;
			if (!frameWork->GetSystemService(RZSRVID_GRAPHIC_SYSTEM, GZIID_cIGZGraphicSystem, &service) || service == nullptr)
			{
				return nullptr;
			}

			cIGZGraphicSystem* const graphicSystem = static_cast<cIGZGraphicSystem*>(service);

			cIGZBuffer* buffer = nullptr;
			if (!graphicSystem->CreateBuffer(&buffer))
			{
				buffer = nullptr;
			}

			graphicSystem->Release();
			return buffer;
		}
	}

	//// Public API

	// cIGZGBufferRegionExtension

	bool cVKDriver::BufferRegionEnabled(void)
	{
		SCVK_CALL("");
		return true;
	}

	uint32_t cVKDriver::NewBufferRegion(int32_t gdBufferRegionType)
	{
		SCVK_CALL("%d", gdBufferRegionType);

		// Anything but the two known types is not something this interface can express.
		if (gdBufferRegionType != GD_BUFFER_REGION_COLOUR && gdBufferRegionType != GD_BUFFER_REGION_DEPTH)
		{
			return 0;
		}

		return vulkan->CreateBufferRegion(gdBufferRegionType == GD_BUFFER_REGION_DEPTH);
	}

	bool cVKDriver::DeleteBufferRegion(int32_t bufferRegion)
	{
		SCVK_CALL("%d", bufferRegion);

		if (bufferRegion <= 0)
		{
			return false;
		}

		// Positive, checked just above.
		vulkan->DestroyBufferRegion(static_cast<uint32_t>(bufferRegion));
		return true;
	}

	bool cVKDriver::ReadBufferRegion(uint32_t region, int32_t x, int32_t y, int32_t width, int32_t height, int32_t screenX, int32_t screenY)
	{
		SCVK_CALL("%u, %d,%d %dx%d <- %d,%d", region, x, y, width, height, screenX, screenY);

		// Trace the save, closing a tile when it is part of the scene
		bool const isPartial = !IsFullWindowCopy(x, y, width, height, screenX, screenY);

		if (isPartial)
		{
			isRegionFrameInteresting = true;
		}

		AttachRegionPending();
		NoteRegionStep("save r%u region %d,%d %dx%d <- screen %d,%d", region, x, y, width, height, screenX, screenY);

		if (isPartial)
		{
			NoteTileSave(screenX, screenY, width, height);
		}

		// Save it
		//
		// The framebuffer is the source. The first pair of coordinates addresses the
		// region and the last pair addresses the screen, which is the convention the
		// matching draw call uses in reverse.
		return vulkan->SaveBufferRegion(region, x, y, width, height, screenX, screenY);
	}

	bool cVKDriver::DrawBufferRegion(uint32_t region, int32_t x, int32_t y, int32_t width, int32_t height, int32_t screenX, int32_t screenY)
	{
		SCVK_CALL("%u, %d,%d %dx%d -> %d,%d", region, x, y, width, height, screenX, screenY);

		// Trace the restore, which starts a fresh tile
		hasRegionFrameRestored = true;
		NoteRegionStep("restore r%u region %d,%d %dx%d -> screen %d,%d", region, x, y, width, height, screenX, screenY);
		ResetTile();

		// Restore it
		//
		// The region is the source, at the first pair of coordinates, and the screen is
		// the destination, at the last pair.
		return vulkan->RestoreBufferRegion(region, x, y, width, height, screenX, screenY);
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

	// cIGZGSnapshotExtension

	cIGZBuffer* cVKDriver::CopyColorBuffer(int32_t x, int32_t y, int32_t width, int32_t height, cIGZBuffer* buffer)
	{
		// Nominally an extension, but declining it in QueryInterface crashes the game
		// during load, so it has to exist. This follows the DirectX driver (0x883950):
		// the game usually passes no buffer and expects one back holding the rectangle,
		// with its top left corner in the buffer's.
		SCVK_CALL("%d,%d %dx%d, %p", x, y, width, height, buffer);

		if (width <= 0 || height <= 0)
		{
			return buffer;
		}

		// Clip the rectangle to the window
		//
		// The window is far below INT32_MAX, so its size converts unchanged.
		int32_t const frameWidth  = static_cast<int32_t>(vulkan->FrameWidth());
		int32_t const frameHeight = static_cast<int32_t>(vulkan->FrameHeight());

		int32_t const left   = std::max(x, 0);
		int32_t const top    = std::max(y, 0);
		int32_t const right  = std::min(x + width, frameWidth);
		int32_t const bottom = std::min(y + height, frameHeight);

		bool const isWhollyInside = left == x && top == y && right == x + width && bottom == y + height;
		bool const hasOverlap     = right > left && bottom > top;

		// Read the screen
		//
		// The extents are positive once there is an overlap, and both corners are inside
		// the window, so not negative.
		uint32_t const copyWidth  = hasOverlap ? static_cast<uint32_t>(right - left) : 0u;
		uint32_t const copyHeight = hasOverlap ? static_cast<uint32_t>(bottom - top) : 0u;

		std::vector<uint8_t> pixels(size_t{ copyWidth } * copyHeight * 4u);

		bool const isRead = hasOverlap && vulkan->ReadFramePixels(static_cast<uint32_t>(left), static_cast<uint32_t>(top), copyWidth, copyHeight, pixels.data());

		// Carry on with a black picture when nothing could be read
		//
		// The DirectX driver hands the caller's buffer back unchanged when it cannot lock
		// its back buffer, which is null for a photo, and the game's photo code uses the
		// result without checking it. An earlier version did the same and the game
		// crashed taking a photo.
		if (hasOverlap && !isRead)
		{
			LogWarn("CopyColorBuffer: the screen could not be read back; handing over black.");
		}

		// Make a buffer when the game passes none, sized to the rectangle
		//
		// The width and height were checked positive above.
		if (buffer == nullptr)
		{
			buffer = CreateGameBuffer();

			if (buffer == nullptr)
			{
				LogWarn("CopyColorBuffer: the graphic system made no buffer.");
				return nullptr;
			}
		}

		if (!buffer->IsReady() && !buffer->Init(static_cast<uint32_t>(width), static_cast<uint32_t>(height), cGZBufferColorType::A8R8G8B8, 32))
		{
			return buffer;
		}

		// Refuse a buffer that is not four bytes a pixel
		//
		// The DirectX driver writes 32-bit pixels whatever the buffer is. Every buffer it
		// makes itself is A8R8G8B8, so only one the game passes in could differ.
		if (buffer->GetBytesPerPixel() != 4 || !buffer->Lock(SNAPSHOT_LOCK_FLAGS))
		{
			return buffer;
		}

		// Find its pixels
		//
		// The interface hands the surface over untyped. Rows are found by a stride in bytes,
		// so it is addressed as bytes.
		uint8_t* const bits          = static_cast<uint8_t*>(buffer->GetColorSurfaceBits());
		uint32_t const stride        = buffer->GetColorSurfaceStride();
		uint32_t const bufferRows    = static_cast<uint32_t>(std::max(buffer->Height(), 0));
		uint32_t const bufferColumns = static_cast<uint32_t>(std::max(buffer->Width(), 0));

		if (bits != nullptr)
		{
			// Clear it to opaque black when the rectangle ran off the window
			//
			// What the DirectX driver does, so the part with nothing behind it is not
			// whatever the buffer last held. The same goes for a screen that could not be
			// read at all.
			if (!isWhollyInside || !isRead)
			{
				for (uint32_t row = 0; row < bufferRows; row++)
				{
					for (uint32_t column = 0; column < bufferColumns; column++)
					{
						memcpy(bits + size_t{ row } * stride + size_t{ column } * 4u, OPAQUE_BLACK, sizeof(OPAQUE_BLACK));
					}
				}
			}

			// Copy the rows into its top left corner
			//
			// Kept inside the buffer, which the DirectX driver does not check.
			uint32_t const rows    = isRead ? std::min(copyHeight, bufferRows) : 0u;
			uint32_t const columns = std::min(copyWidth, bufferColumns);

			for (uint32_t row = 0; row < rows; row++)
			{
				memcpy(bits + size_t{ row } * stride, pixels.data() + size_t{ row } * copyWidth * 4u, size_t{ columns } * 4u);
			}
		}

		buffer->Unlock(SNAPSHOT_LOCK_FLAGS);

		// Say what was handed over
		//
		// The call trace is long spent by the time anyone takes a photo, and these are
		// rare enough to log every one.
		LogDebug("CopyColorBuffer: %d,%d %dx%d into a %dx%d buffer, %s.", x, y, width, height, buffer->Width(), buffer->Height(), isRead ? "read from the screen" : "black");
		return buffer;
	}

	// cIGZGDriverLightingExtension

	void cVKDriver::EnableLighting(bool isEnabled)
	{
		SCVK_CALL("%d", isEnabled);
	}

	void cVKDriver::EnableLight(uint32_t light, bool isEnabled)
	{
		SCVK_CALL("%u, %d", light, isEnabled);
	}

	void cVKDriver::LightModelAmbient(float red, float green, float blue, float alpha)
	{
		SCVK_CALL("%.3f, %.3f, %.3f, %.3f", red, green, blue, alpha);
	}

	void cVKDriver::LightColor(uint32_t light, uint32_t type, float const* colour)
	{
		SCVK_CALL("%u, %u, %p", light, type, colour);
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

	void cVKDriver::MaterialColor(uint32_t type, float const* colour)
	{
		SCVK_CALL("%u, %p", type, colour);
	}

	void cVKDriver::MaterialColor(float const* ambient, float const* diffuse, float const* specular, float const* emission, float shininess)
	{
		SCVK_CALL("%p, %p, %p, %p, %.3f", ambient, diffuse, specular, emission, shininess);
	}

	// cIGZGDriverVertexBufferExtension
	//
	// Not exposed through QueryInterface, so none of this should run. The implementations
	// exist to satisfy the interface and to make it obvious in the trace if that
	// assumption turns out to be wrong.

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

	uint32_t cVKDriver::GetVertices(int32_t count, bool isUnknownFlagSet)
	{
		SCVK_CALL("%d, %d  [UNEXPECTED]", count, isUnknownFlagSet);
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

	void cVKDriver::DrawPrims(uint32_t unknown, uint32_t gdPrimitiveType, void* primitives, uint32_t count)
	{
		SCVK_CALL("%u, %u, %p, %u  [UNEXPECTED]", unknown, gdPrimitiveType, primitives, count);
	}

	void cVKDriver::DrawPrimsIndexed(uint32_t unknown, uint32_t gdPrimitiveType, uint32_t count, uint16_t* indices, void* primitives, uint32_t secondCount)
	{
		SCVK_CALL("%u, %u, %u, %p, %p, %u  [UNEXPECTED]", unknown, gdPrimitiveType, count, indices, primitives, secondCount);
	}

	void cVKDriver::Reset(void)
	{
		SCVK_CALL("  [UNEXPECTED]");
	}
}
