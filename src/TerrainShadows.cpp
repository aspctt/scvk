/*
 *  SCD3D11 - a free Direct3D 11 driver for SimCity 4's SimGL interface
 *  Copyright (C) 2026
 *
 *  This library is free software; you can redistribute it and/or
 *  modify it under the terms of the GNU Lesser General Public
 *  License as published by the Free Software Foundation, under
 *  version 2.1 of the License, or (at your option) any later version.
 */

/*
 * The city side of terrain shadows.
 *
 * SimCity 4 Deluxe 1.1.641. The terrain is reached through gzcom-dll's interfaces, so it
 * is known in a city without a single shadow record. cSTETerrain (vtable 0x00AB3D18)
 * keeps one 0x24-byte record per vertex at +0x6C: the altitude at +0x00, the normal its
 * vertex lighting uses at +0x04 (what DoLighting, 0x00742930, hands
 * GetTerrainVertexColors), and flags at +0x22, where 0x3000 means the cell with that
 * lowest corner is split along its other diagonal (IsTriangulationFlipped, 0x00741C20;
 * cliff management and DJEM fixes set them). +0x28 and +0x2C are the cell counts and
 * +0x38 and +0x3C the vertex counts, one more each. GetAltitudes (+0x3C, 0x00742A00)
 * copies an inclusive rectangle of vertices without bounds checks, (rect, out, row
 * stride, column stride), so the rectangle is always built from the terrain's own counts.
 * There is no interface for the normals and flags in bulk, so they are read from the
 * records, and only kept when every record's altitude matches GetAltitudes. CellWidth
 * (+0x18, 0x0074C5C0) returns a float on the x87 stack although cISTETerrainMap declares
 * a uint32_t, so it is called through a float-typed pointer.
 *
 * The game has no terrain revision counter to watch (GetAltitudeCheckSum is a plain sum),
 * so the height field, under 70 thousand floats for a large city, is read again on a sun
 * change or every few seconds, and the map rebuilt only when it or the sun differs.
 *
 * The game's own terrain self-shading: cSC4LightingManager keeps a shadow height grid
 * (+0x198, ground heights at +0x18C) and ShadowDepth_ (0x007D6950) returns how far a point
 * lies below it. GetTerrainLighting, GetTerrainColor, GetModelLight and GetColor3 and 6,
 * its only callers, bend the lighting normal away from the sun when that depth is
 * positive (BendShadowedNormal_, 0x007D68B0), which bakes soft, vertex-sized shadows into
 * the terrain's vertex colours and into model lighting. Drawn under these shadows it
 * darkens the same slopes twice, so Install makes ShadowDepth_ return 0.
 *
 * The grids themselves must stay: CalcShadowTerrainRect (0x007D6CC0) marches every BAT
 * shadow down the sun across the ground heights until it meets the ground, and the
 * overlay manager files the shadow under the rectangle it covers. The byte at +0x1D4 that
 * gates the self-shading also gates the terrain change handler's refresh of those
 * heights. Switching it off left them at zero, so on raised terrain every shadow ran down
 * to altitude 0 and covered hundreds of spatial buckets, stalling SDSRemove_ for seconds
 * whenever a zoom or rotation replaced the city's shadows.
 */

//// Dependencies

#include "TerrainShadows.h"
#include "GamePatch.h"
#include "LiveShadowHooks.h"
#include "ModuleLog.h"
#include "Settings.h"

#include <GZServPtrs.h>
#include <SC4Rect.h>
#include <cISC4App.h>
#include <cISC4City.h>
#include <cISC4RenderProperties.h>
#include <cISTETerrain.h>

#include <windows.h>
#include <cmath>
#include <string.h>
#include <utility>

namespace scvk::TerrainShadows
{
	//// Constants

	namespace
	{
		// Half a terrain cell: shadow edges follow the terrain's 16 m facets closely, and a
		// large city stays under 700 texels a side
		constexpr float SPACING_CELLS = 0.5f;

		// The sun's apparent radius for the penumbra, in radians. The real sun is 0.27
		// degrees; 3 degrees stands in for the sky light that softens real hill shadows, and
		// is the setting to change if edges look too hard or too soft.
		constexpr float SUN_RADIUS = 3.0f * 0.0174532925f;

		// The sun is locked to the view and only moves with a rotation
		constexpr float SUN_TOLERANCE = 1.0e-5f;

		// How often an unchanged sun reads the height field again to catch terraforming
		constexpr ULONGLONG TERRAIN_POLL_MILLISECONDS = 2000;

		constexpr size_t   CELL_WIDTH_SLOT  = 0x18 / sizeof(void*);
		constexpr uint32_t MAXIMUM_CELLS    = 4096;
		constexpr float    MAXIMUM_CELL_WIDTH = 1024.0f;

		// The terrain's vertex records
		constexpr ptrdiff_t TERRAIN_RECORDS = 0x6C;
		constexpr size_t    RECORD_SIZE     = 0x24;
		constexpr ptrdiff_t RECORD_NORMAL   = 0x04;
		constexpr ptrdiff_t RECORD_FLAGS    = 0x22;
		constexpr uint16_t  FLIPPED_FLAGS   = 0x3000;

		// ShadowDepth_: PUSH EBX, PUSH ESI, PUSH EDI, MOV EDI,[ESP+0x10], FLD [EDI], a
		// __thiscall with RET 4 and a float result on the x87 stack. The patch makes it FLDZ
		// and RET 4, never below the shadow, and keeps the bytes after as they are.
		constexpr uintptr_t SHADOW_DEPTH_ADDRESS = 0x007D6950;

		// RenderShadows' id before the first lookup, and once it cannot be read
		constexpr int32_t PROPERTY_NOT_LOOKED_UP = -2;
		constexpr int32_t PROPERTY_UNREADABLE    = -1;
	}

	//// State

	namespace
	{
		GamePatch shadowDepth;
		bool      isEnabled = false;

		TerrainShadowMap::HeightField field;
		TerrainShadowMap::HeightField scratchField;
		TerrainShadowMap::Map         map;
		bool                          isMapValid    = false;
		void const*                   mapTerrain    = nullptr;
		float                         mapSun[3]     = {};
		uint64_t                      mapGeneration = 0;
		ULONGLONG                     lastRead      = 0;
		unsigned                      buildCount    = 0;
		int32_t                       renderShadowsId = PROPERTY_NOT_LOOKED_UP;
	}

	//// Private Functions

	namespace
	{
		cISTETerrain* FindCityTerrain(void)
		{
			cISC4AppPtr app;
			if (!app)
			{
				return nullptr;
			}

			cISC4City* const city = app->GetCity();
			return (city != nullptr) ? city->GetTerrain() : nullptr;
		}

#if defined(_MSC_VER) && defined(_M_IX86)
		bool ReadTerrainGeometry(cISTETerrain* terrain, uint32_t& cellCountX, uint32_t& cellCountZ, int32_t& rowStride, float& cellWidth)
		{
			void* const cellWidthGetter = ReadVirtualFunction(terrain, CELL_WIDTH_SLOT);
			if (cellWidthGetter == nullptr)
			{
				return false;
			}

			// CellWidth returns a float on the x87 stack, whatever cISTETerrainMap declares
			using CellWidth = float (__thiscall*)(void*);
			__try
			{
				cellCountX = terrain->CellCountX();
				cellCountZ = terrain->CellCountZ();
				rowStride  = terrain->GetVertIndex(0, 1);
				cellWidth  = reinterpret_cast<CellWidth>(cellWidthGetter)(terrain);
				return true;
			}
			__except (EXCEPTION_EXECUTE_HANDLER)
			{
				return false;
			}
		}

		bool ReadAltitudes(cISTETerrain* terrain, uint32_t verticesX, uint32_t verticesZ, float* out)
		{
			// The counts are at most MAXIMUM_CELLS + 1, so they fit an int32_t
			SC4Rect<int32_t> const rectangle = { 0, 0, static_cast<int32_t>(verticesX - 1), static_cast<int32_t>(verticesZ - 1) };
			int32_t const stride = static_cast<int32_t>(verticesX);

			__try
			{
				terrain->GetAltitudes(rectangle, out, stride, 1);
				return true;
			}
			__except (EXCEPTION_EXECUTE_HANDLER)
			{
				return false;
			}
		}

		/** The normals and flip flags, from records whose altitudes must be exactly the ones GetAltitudes returned. */
		bool ReadVertexRecords(cISTETerrain* terrain, size_t vertices, float const* altitudes, uint8_t* flipped, float* normals)
		{
			// The records are found by their offset into the game's terrain object
			uint8_t const* records = nullptr;
			if (!ReadGameMemory(reinterpret_cast<uint8_t const*>(terrain) + TERRAIN_RECORDS, records) || records == nullptr)
			{
				return false;
			}

			__try
			{
				for (size_t index = 0; index < vertices; index++)
				{
					uint8_t const* const record = records + index * RECORD_SIZE;
					if (memcmp(record, altitudes + index, sizeof(float)) != 0)
					{
						return false;
					}

					memcpy(normals + index * 3, record + RECORD_NORMAL, sizeof(float) * 3);

					uint16_t flags = 0;
					memcpy(&flags, record + RECORD_FLAGS, sizeof(flags));
					flipped[index] = ((flags & FLIPPED_FLAGS) != 0) ? 1 : 0;
				}

				return true;
			}
			__except (EXCEPTION_EXECUTE_HANDLER)
			{
				return false;
			}
		}

		/**
		 * The id of a bool render property. The render property interface had not been
		 * called from a DLL before, so the id is only used once its name reads back.
		 */
		bool FindBoolProperty(cISC4RenderProperties* properties, char const* name, int32_t& id)
		{
			__try
			{
				int32_t const found = properties->BoolPropertyIDFromName(name);
				char const* const readBack = properties->NameFromBoolPropertyID(found);
				if (readBack == nullptr || _stricmp(readBack, name) != 0)
				{
					return false;
				}

				id = found;
				return true;
			}
			__except (EXCEPTION_EXECUTE_HANDLER)
			{
				return false;
			}
		}

		bool ReadBoolProperty(cISC4RenderProperties* properties, int32_t id, bool& value)
		{
			__try
			{
				value = properties->BoolValue(id);
				return true;
			}
			__except (EXCEPTION_EXECUTE_HANDLER)
			{
				return false;
			}
		}
#else
		bool ReadTerrainGeometry(cISTETerrain*, uint32_t&, uint32_t&, int32_t&, float&)
		{
			return false;
		}

		bool ReadAltitudes(cISTETerrain*, uint32_t, uint32_t, float*)
		{
			return false;
		}

		bool ReadVertexRecords(cISTETerrain*, size_t, float const*, uint8_t*, float*)
		{
			return false;
		}

		bool FindBoolProperty(cISC4RenderProperties*, char const*, int32_t&)
		{
			return false;
		}

		bool ReadBoolProperty(cISC4RenderProperties*, int32_t, bool&)
		{
			return false;
		}
#endif

		bool ReadHeightField(cISTETerrain* terrain, TerrainShadowMap::HeightField& out)
		{
			// The grid, which must be what GetAltitudes will be asked for
			uint32_t cellCountX = 0;
			uint32_t cellCountZ = 0;
			int32_t  rowStride  = 0;
			float    cellWidth  = 0.0f;

			if (!ReadTerrainGeometry(terrain, cellCountX, cellCountZ, rowStride, cellWidth))
			{
				return false;
			}

			if (cellCountX == 0 || cellCountZ == 0 || cellCountX > MAXIMUM_CELLS || cellCountZ > MAXIMUM_CELLS || rowStride != static_cast<int32_t>(cellCountX + 1) || !(cellWidth > 0.0f && cellWidth < MAXIMUM_CELL_WIDTH))
			{
				static bool hasLogged = false;
				if (!hasLogged)
				{
					hasLogged = true;
					Log(LogCategory::Initialization, "terrain shadows: unexpected terrain geometry (cells %ux%u, row stride %d, cell width %.3f)", cellCountX, cellCountZ, rowStride, static_cast<double>(cellWidth));
				}

				return false;
			}

			// The altitudes
			out.verticesX = cellCountX + 1;
			out.verticesZ = cellCountZ + 1;
			out.cellWidth = cellWidth;
			out.altitudes.resize(size_t{ out.verticesX } * out.verticesZ);

			if (!ReadAltitudes(terrain, out.verticesX, out.verticesZ, out.altitudes.data()))
			{
				return false;
			}

			for (float const altitude : out.altitudes)
			{
				if (!std::isfinite(altitude))
				{
					return false;
				}
			}

			// The normals and flips, where the records agree with the altitudes
			size_t const vertices = out.altitudes.size();
			out.flipped.resize(vertices);
			out.normals.resize(vertices * 3);

			bool const hasRecords = ReadVertexRecords(terrain, vertices, out.altitudes.data(), out.flipped.data(), out.normals.data());
			if (hasRecords)
			{
				for (float const component : out.normals)
				{
					if (!std::isfinite(component))
					{
						out.normals.clear();
						break;
					}
				}
			}
			else
			{
				out.flipped.clear();
				out.normals.clear();
			}

			static bool hasLoggedRecords = false;
			if (!hasLoggedRecords)
			{
				hasLoggedRecords = true;

				size_t flippedCells = 0;
				for (uint8_t const flip : out.flipped)
				{
					flippedCells += flip;
				}

				if (hasRecords)
				{
					Log(LogCategory::Initialization, "terrain shadows: vertex records match GetAltitudes; %u flipped cells, %s normals", static_cast<unsigned>(flippedCells), out.normals.empty() ? "derived" : "SC4's own");
				}
				else
				{
					Log(LogCategory::Initialization, "terrain shadows: vertex records do not match GetAltitudes; cells taken as unflipped, normals derived from the height field");
				}
			}

			return true;
		}

		bool IsSameField(TerrainShadowMap::HeightField const& a, TerrainShadowMap::HeightField const& b)
		{
			return a.verticesX == b.verticesX && a.verticesZ == b.verticesZ && a.cellWidth == b.cellWidth && a.flipped.size() == b.flipped.size() && a.normals.size() == b.normals.size() && memcmp(a.altitudes.data(), b.altitudes.data(), a.altitudes.size() * sizeof(float)) == 0 && memcmp(a.flipped.data(), b.flipped.data(), a.flipped.size()) == 0 && memcmp(a.normals.data(), b.normals.data(), a.normals.size() * sizeof(float)) == 0;
		}

		bool IsSameSun(float const sun[3])
		{
			return isMapValid && std::fabs(sun[0] - mapSun[0]) <= SUN_TOLERANCE && std::fabs(sun[1] - mapSun[1]) <= SUN_TOLERANCE && std::fabs(sun[2] - mapSun[2]) <= SUN_TOLERANCE;
		}

		double MillisecondsSince(LARGE_INTEGER const& start)
		{
			LARGE_INTEGER end{};
			LARGE_INTEGER frequency{};
			QueryPerformanceCounter(&end);
			QueryPerformanceFrequency(&frequency);

			if (frequency.QuadPart == 0)
			{
				return 0.0;
			}

			return static_cast<double>(end.QuadPart - start.QuadPart) * 1000.0 / static_cast<double>(frequency.QuadPart);
		}
	}

	//// Public API

	void Install(void)
	{
		if (isEnabled || !LiveShadowHooks::Enabled() || HasCommandLineSwitch("-TerrainShadows:off"))
		{
			return;
		}

		isEnabled = true;

		// Without the patch the terrain shadows are still drawn, over the game's own
		shadowDepth = GamePatch::Bytes(SHADOW_DEPTH_ADDRESS, { 0x53, 0x56, 0x57, 0x8B, 0x7C, 0x24, 0x10, 0xD9, 0x07 }, { 0xD9, 0xEE, 0xC2, 0x04, 0x00, 0x24, 0x10, 0xD9, 0x07 });
		if (!shadowDepth.IsOriginal())
		{
			Log(LogCategory::Initialization, "terrain shadows: guard failed at 0x%08lX, SC4's terrain self-shading stays on", shadowDepth.GameAddress());
			return;
		}

		if (!shadowDepth.Write())
		{
			return;
		}

		Log(LogCategory::Initialization, "terrain shadows installed: SC4's own terrain self-shading replaced");
	}

	void Uninstall(void)
	{
		if (!isEnabled)
		{
			return;
		}

		shadowDepth.Restore();
		isEnabled  = false;
		isMapValid = false;
		mapTerrain = nullptr;
	}

	bool Enabled(void)
	{
		return isEnabled;
	}

	bool ShadowsRendered(void)
	{
		cISC4AppPtr app;
		cISC4RenderProperties* const properties = app ? app->GetRenderProperties() : nullptr;
		if (properties == nullptr)
		{
			return true;
		}

		// Find the property once
		if (renderShadowsId == PROPERTY_NOT_LOOKED_UP)
		{
			int32_t id = PROPERTY_UNREADABLE;
			renderShadowsId = FindBoolProperty(properties, "RenderShadows", id) ? id : PROPERTY_UNREADABLE;

			bool value = true;
			bool const isRead = renderShadowsId >= 0 && ReadBoolProperty(properties, renderShadowsId, value);
			Log(LogCategory::Initialization, "terrain shadows: RenderShadows render property %s%s", isRead ? "is " : "could not be read, shadows assumed on", isRead ? (value ? "on" : "off") : "");
		}

		bool value = true;
		return renderShadowsId < 0 || !ReadBoolProperty(properties, renderShadowsId, value) || value;
	}

	TerrainShadowMap::Map const* Update(float const sun[3], uint64_t& generation)
	{
		cISTETerrain* const terrain = FindCityTerrain();
		bool const isSameSun = IsSameSun(sun);

		// Reading and comparing the height field is the costly part, so an unchanged sun over
		// the same terrain reads it at most every TERRAIN_POLL_MILLISECONDS. A terraform shows
		// up on the first pass after that, as a new generation.
		ULONGLONG const now = GetTickCount64();
		if (isSameSun && terrain != nullptr && terrain == mapTerrain && now - lastRead < TERRAIN_POLL_MILLISECONDS)
		{
			generation = mapGeneration;
			return &map;
		}

		lastRead = now;

		if (terrain == nullptr || !ReadHeightField(terrain, scratchField))
		{
			isMapValid = false;
			mapTerrain = nullptr;
			return nullptr;
		}

		bool const isSameTerrain = isMapValid && terrain == mapTerrain && IsSameField(scratchField, field);
		if (isSameTerrain && isSameSun)
		{
			generation = mapGeneration;
			return &map;
		}

		// Build the map again
		LARGE_INTEGER start{};
		QueryPerformanceCounter(&start);

		TerrainShadowMap::Map next;
		if (!TerrainShadowMap::Build(scratchField, sun, scratchField.cellWidth * SPACING_CELLS, SUN_RADIUS, next))
		{
			static bool hasLogged = false;
			if (!hasLogged)
			{
				hasLogged = true;
				Log(LogCategory::Initialization, "terrain shadows: no map for sun %.4f/%.4f/%.4f over %ux%u vertices", static_cast<double>(sun[0]), static_cast<double>(sun[1]), static_cast<double>(sun[2]), scratchField.verticesX, scratchField.verticesZ);
			}

			isMapValid = false;
			return nullptr;
		}

		double const buildTime = MillisecondsSince(start);
		bool const isNewCity = terrain != mapTerrain;

		std::swap(field, scratchField);
		map        = std::move(next);
		isMapValid = true;
		mapTerrain = terrain;
		memcpy(mapSun, sun, sizeof(mapSun));
		mapGeneration++;
		buildCount++;

		if (isNewCity || buildCount <= 2)
		{
			Log(LogCategory::Initialization, "terrain shadows: %s %ux%u vertices (cell %.1f), map %ux%u at %.1f, sun %.4f/%.4f/%.4f, %.2f ms", isNewCity ? "city" : (isSameSun ? "terrain changed," : "sun changed,"), field.verticesX, field.verticesZ, static_cast<double>(field.cellWidth), map.width, map.height, static_cast<double>(map.spacing), static_cast<double>(sun[0]), static_cast<double>(sun[1]), static_cast<double>(sun[2]), buildTime);
		}

		generation = mapGeneration;
		return &map;
	}

	TerrainShadowMap::HeightField const& Terrain(void)
	{
		return field;
	}

	bool Altitude(float x, float z, float& altitude)
	{
		if (!isMapValid)
		{
			return false;
		}

		float const extentX = static_cast<float>(field.verticesX - 1) * field.cellWidth;
		float const extentZ = static_cast<float>(field.verticesZ - 1) * field.cellWidth;
		if (!(x >= 0.0f && x <= extentX && z >= 0.0f && z <= extentZ))
		{
			return false;
		}

		altitude = TerrainShadowMap::Altitude(field, x, z);
		return true;
	}
}
