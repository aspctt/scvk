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
 * Shadow records for the live shadow map.
 *
 * SimCity 4 Deluxe 1.1.641, Windows x86, image base 0x00400000. Opt-in with
 * -LiveShadows:on. Every site is guarded by its exact bytes or slot value and restored
 * during PreAppShutdown.
 *
 *   0x00AB3A14  cSTEOverlayManager vtable +0x2C, AddShadow 0x0073C0D0
 *   0x00AB3A24  cSTEOverlayManager vtable +0x3C, RemoveOverlay 0x007388C0
 *   0x00736BF0  DrawShadows entry, SUB ESP,0x1C4
 *   0x00737870  DrawShadowsRough entry, SUB ESP,0x9C
 *   0x00491894  CreateOccupantShadow, the last point the current mesh is in EDI
 *
 * DrawShadows and DrawShadowsRough are each called once, from DrawOverlays at 0x0073935F
 * and 0x00739373. The hooks sit on the functions rather than those calls because
 * SC4RenderServices owns the calls: it rewrites their rel32, calls the target it found,
 * and uninstalls and reinstalls later, restoring the rel32 it saved, which silently
 * dropped a call site hook installed in between. It always ends up calling the functions
 * themselves, so both run in either order.
 *
 * AddShadow (__thiscall, RET 0x18) returns the record's slot index, never a negative
 * handle. The record is 0xB4 bytes in the slot array at manager +0x98: +0x00 is -1 while
 * the slot is in use, +0x04 the flags (AddShadow ORs in 8), +0x1C the CalcShadowProjection
 * matrix and +0xAC the texture binding it holds a reference to, whose first word points at
 * the texture object whose first word is the driver's texture name (DrawShadows passes
 * **binding to SetTexture).
 *
 * Why the draw is filtered rather than switched off at 0x00739340: AddShadowDecal (+0x24,
 * 0x0073B3B0) also ORs flag 8 into records in the same slot array, so the network FSH
 * decal shadows are drawn by the same DrawShadows call, whose only filter is
 * (flags & 9) == 9. Switching the call off would remove them too. The entry hooks instead
 * drop exactly the ids this registry owns and leave the rest to the game. They also run
 * where the game knows what a pass needs: the list is the pass's visible overlays, the
 * first argument is the eye to world texture generation matrix (source 0x10 is eye space,
 * so DrawShadows' texture matrix is projector * eyeToWorld), and the ShadowQuality and
 * shadow toggle tests have happened. Whether a record is enabled is read from its flags
 * there, which follows SetOverlayEnabled and any other path that changes them.
 *
 * AddShadow receives positions and coordinates but no index buffer. The mesh
 * CreateOccupantShadow is iterating is in EDI from 0x00491841 until 0x004918AF replaces it
 * with the material's texture binding, so the site at 0x00491894 records it. Its layout
 * matches SC4DrawContext::RenderMesh (0x007D4A80): +0 the vertex buffer {+0 format, +6
 * 16-bit count, +8 data}, +4 an index holder whose first word is the 16-bit index array
 * (null for DrawArrays meshes), +8 the primitive list {+4 begin, +8 end} of
 * {primitive, start, count}.
 *
 * Props without Is Ground Model (0x8A5E5DB8) are never shadowed by the game. With
 * -LiveShadows:on, LiveShadowHooks' site D lets their AddShadow run and flags it
 * (ExpectMeshCaster), so the record only exists for its lifetime and dirty rectangle. Such
 * a prop floating above the terrain, like the building zots AddBuildingZotProp places 46
 * to 56 m up, stays unshadowed. One whose coordinates the projector reproduces is a
 * prerendered BAT prop and casts through the projector. Anything else is a True3D unwrap
 * and casts through its own coordinates.
 *
 * RedisplayWorldRect reuses what RedisplayStaticOverlay (0x00737D40) does: the manager's
 * terrain (+0x88) RedisplayTerrain (vtable +0x110, 0x00749850) with a rectangle of terrain
 * cells, which queues a partial static pass.
 */

//// Dependencies

#include "ShadowRegistry.h"
#include "GamePatch.h"
#include "LiveShadowHooks.h"
#include "Logger.h"
#include "ModuleLog.h"
#include "SC4Version.h"
#include "Settings.h"

#include <windows.h>
#include <algorithm>
#include <cfloat>
#include <cmath>
#include <mutex>
#include <new>
#include <string.h>
#include <unordered_set>

#if defined(_MSC_VER)
#include <intrin.h>
#endif

namespace scvk::ShadowRegistry
{
	//// Types

	namespace
	{
		/**
		 * cS3DTransform as built at 0x004915FE to 0x0049166A. Flag byte 1 bit 4 means the
		 * rotation applies and bit 2 the uniform scale. The matrix is row major, world = R *
		 * local.
		 */
		struct Transform
		{
			uint8_t flags[2];
			uint8_t padding[2];
			float   rotation[9];
			float   translation[3];
			float   scale;
		};

		static_assert(sizeof(Transform) == 0x38, "cS3DTransform must stay 56 bytes");

		struct MeshHeader
		{
			void const* vertices;
			void const* indexHolder;
			void const* primitives;
		};

		struct PrimitiveRun
		{
			uint32_t primitive;
			uint32_t start;
			uint32_t count;
		};

		/** The vector of overlay ids DrawShadows takes, as the game's STL lays it out. */
		struct IdList
		{
			int32_t* begin;
			int32_t* end;
			int32_t* capacity;
		};

		/** One AddShadow call that made a record, as Register reads it. */
		struct AddShadowCall
		{
			void*          manager     = nullptr;
			int32_t        id          = 0;
			void const*    binding     = nullptr;
			int32_t        count       = 0;
			void const*    positions   = nullptr;
			void const*    coordinates = nullptr;
			void const*    transform   = nullptr;
			uint8_t const* returnSlot  = nullptr;

			// A relaxed prop LiveShadowHooks let through, sorted by what its mesh shows
			bool isRelaxedProp = false;
		};

		/**
		 * A registered caster, with copies of what the gather reads for every id it looks up,
		 * so culling one never touches the caster itself.
		 */
		struct Entry
		{
			// Null where no caster has this id
			std::shared_ptr<Caster const> caster;

			void const* binding    = nullptr;
			float       low[3]     = {};
			float       high[3]    = {};
			float       baseHeight = 0.0f;
			bool        isSuppressedOnly = false;

			// The serial of the pass this caster last joined
			uint32_t passSerial = 0;
		};

		/** The size of a terrain's cell grid. */
		struct TerrainGrid
		{
			float    cellWidth  = 0.0f;
			uint32_t cellCountX = 0;
			uint32_t cellCountZ = 0;
		};

		using AddShadowFunction     = int32_t (__thiscall*)(void*, void const*, int32_t, void const*, void const*, void const*, void const*);
		using RemoveOverlayFunction = void (__thiscall*)(void*, int32_t);
		using DrawShadowsFunction   = void (__thiscall*)(void*, float const*, void*, IdList*);
	}

	//// Constants

	namespace
	{
		constexpr uint16_t SUPPORTED_GAME_VERSION = 641;

		constexpr uintptr_t ADD_SHADOW_SLOT_ADDRESS            = 0x00AB3A14;
		constexpr uintptr_t ADD_SHADOW_ADDRESS                 = 0x0073C0D0;
		constexpr uintptr_t REMOVE_OVERLAY_SLOT_ADDRESS        = 0x00AB3A24;
		constexpr uintptr_t REMOVE_OVERLAY_ADDRESS             = 0x007388C0;
		constexpr uintptr_t DRAW_SHADOWS_ADDRESS               = 0x00736BF0;
		constexpr uintptr_t DRAW_SHADOWS_REJOIN_ADDRESS        = 0x00736BF6;
		constexpr uintptr_t DRAW_SHADOWS_ROUGH_ADDRESS         = 0x00737870;
		constexpr uintptr_t DRAW_SHADOWS_ROUGH_REJOIN_ADDRESS  = 0x00737876;
		constexpr uintptr_t MESH_STASH_ADDRESS                 = 0x00491894;
		constexpr uintptr_t MESH_STASH_REJOIN_ADDRESS          = 0x0049189F;

		// GetShadowParams (0x007D6B70) copies the alpha test scale out of this global
		constexpr uintptr_t SHADOW_ALPHA_SCALE_ADDRESS = 0x00B4E330;

		// The overlay manager's record array, and a record's layout
		constexpr ptrdiff_t MANAGER_RECORDS   = 0x98;
		constexpr size_t    RECORD_SIZE       = 0xB4;
		constexpr ptrdiff_t RECORD_FLAGS      = 0x04;
		constexpr ptrdiff_t RECORD_PROJECTOR  = 0x1C;
		constexpr ptrdiff_t RECORD_BINDING    = 0xAC;
		constexpr int32_t   RECORD_IN_USE     = -1;
		constexpr uint32_t  SHADOW_DRAW_FLAGS = 9;
		constexpr uint32_t  WRAP_FLAG         = 2;

		// RedisplayStaticOverlay (0x00737D40) redisplays a record through the terrain at
		// manager +0x88, passing manager +0x16 as updateView
		constexpr ptrdiff_t MANAGER_TERRAIN     = 0x88;
		constexpr ptrdiff_t MANAGER_UPDATE_VIEW = 0x16;

		// cSTETerrain's table 0x00AB3D18, cISTETerrainMap first: +0x18 CellWidth (a float,
		// 16), +0x1C CellCountX, +0x20 CellCountZ, +0x34 GetAltitude(x, z) (after
		// GetAltitude(index): MSVC reverses overloads), and +0x110
		// RedisplayTerrain(updateTerrain, updateView, rect, flags) 0x00749850, whose rect is
		// {x1, z1, x2, z2} in cells
		constexpr size_t TERRAIN_CELL_WIDTH_SLOT   = 0x18 / sizeof(void*);
		constexpr size_t TERRAIN_CELL_COUNT_X_SLOT = 0x1C / sizeof(void*);
		constexpr size_t TERRAIN_CELL_COUNT_Z_SLOT = 0x20 / sizeof(void*);
		constexpr size_t TERRAIN_ALTITUDE_SLOT     = 0x34 / sizeof(void*);
		constexpr size_t TERRAIN_REDISPLAY_SLOT    = 0x110 / sizeof(void*);

		// CreateOccupantShadow's body stack pointer. At the AddShadow call its six arguments
		// and the return address sit below it, and at the stash site the six arguments of the
		// position extraction do.
		constexpr ptrdiff_t RETURN_SLOT_TO_FRAME = 0x1C;
		constexpr ptrdiff_t STASH_TO_FRAME       = 0x18;
		constexpr ptrdiff_t FRAME_POSITIONS      = 0x24;
		constexpr ptrdiff_t FRAME_TRANSFORM      = 0x58;
		constexpr ptrdiff_t FRAME_COORDINATES    = 0xA8;

		// Where a mesh's vertex buffer keeps its vertex count
		constexpr ptrdiff_t VERTEX_BUFFER_COUNT = 6;

		// Where a primitive list keeps its begin and end
		constexpr ptrdiff_t PRIMITIVE_LIST_BEGIN = 4;
		constexpr ptrdiff_t PRIMITIVE_LIST_END   = 8;

		// Relaxed props, those without Is Ground Model, are sorted by what their mesh shows.
		// One whose lowest point floats this far above the terrain is not a ground model at
		// all (the building zots are such props) and casts nothing, as without live shadows.
		constexpr float FLOATING_PROP_HEIGHT = 2.0f;

		// Coordinates the projector reproduces this closely are a prerendered view, a BAT
		// prop without the flag, which casts through the projector like any BAT caster
		constexpr double PRERENDERED_RESIDUAL = 0.01;

		constexpr uint32_t MAXIMUM_VERTICES       = 0xFFFF;
		constexpr uint32_t MAXIMUM_PRIMITIVE_RUNS = 4096;
		constexpr uint32_t MAXIMUM_INDICES        = 0x40000;
		constexpr size_t   MAXIMUM_IDS_PER_PASS   = size_t{ 1 } << 20;

		// Casters are kept in an array indexed by overlay id, and ids from here up are left to
		// the game. The ids are slots in the manager's record array, far below this.
		constexpr uint32_t MAXIMUM_CASTER_IDS = uint32_t{ 1 } << 20;

		// A registry caster's ground quad sits this far above the occupant's placement
		// height, so the terrain under it is behind it by more than the composite bias along
		// the 45 degree sun
		constexpr float GROUND_LIFT_WORLD = 0.2f;

		// The sun's y below which it is under the horizon and casts ground quads
		constexpr float HORIZON_SUN_Y = -1.0e-3f;

		// The game's SimGL primitives, as AppendTriangles reads them
		constexpr uint32_t PRIMITIVE_TRIANGLES      = 0;
		constexpr uint32_t PRIMITIVE_TRIANGLE_STRIP = 1;
		constexpr uint32_t PRIMITIVE_TRIANGLE_FAN   = 2;
		constexpr uint32_t PRIMITIVE_QUADS          = 6;
		constexpr uint32_t PRIMITIVE_QUAD_STRIP     = 7;
	}

	//// State

	namespace
	{
		GamePatch addShadowSlot;
		GamePatch removeOverlaySlot;
		GamePatch drawShadowsEntry;
		GamePatch drawShadowsRoughEntry;
		GamePatch meshStash;

		bool isInstalled = false;

		AddShadowFunction     originalAddShadow     = nullptr;
		RemoveOverlayFunction originalRemoveOverlay = nullptr;

		// Where the trampolines carry on, and the alpha test scale, in this process. The
		// stubs read the first ones as memory operands, so they stay plain variables.
		uint8_t*       drawShadowsRejoin      = nullptr;
		uint8_t*       drawShadowsRoughRejoin = nullptr;
		uint8_t*       meshStashRejoin        = nullptr;
		uint8_t const* shadowAlphaScale       = nullptr;

		// Written by the stash site and read by the AddShadow hook further down the same
		// CreateOccupantShadow iteration. Plain variables: the frame pointer is compared on
		// use, so a stale or foreign value only costs the index recovery, never correctness.
		void const*    stashedMesh  = nullptr;
		uint8_t const* stashedFrame = nullptr;

		// Set by LiveShadowHooks' site D stub for a relaxed prop, and taken by the AddShadow
		// call that follows on the same thread
		thread_local bool isMeshCasterExpected = false;

		std::mutex casterMutex;

		// Indexed by overlay id. AddShadow's ids are slots in the manager's record array, so
		// they are small and dense, and a gather looking up thousands of them a pass reads an
		// array instead of chasing the nodes of a hash map, as it first did.
		std::vector<Entry> casters;
		size_t             casterCount     = 0;
		uint32_t           highestCasterId = 0;
		void const*        casterManager   = nullptr;
		Pass               pendingPass;
		bool               isPassPending   = false;

		// Bumped whenever the pending pass starts over, so a caster joins each pass once.
		// Entries start at 0, below the first serial.
		uint32_t passSerial = 1;

		// The driver's current projection, for culling. Render thread only, as are the
		// DrawShadows calls that read it.
		float currentProjection[16] = {};
		bool  isProjectionKnown     = false;

		// The hooks' counts and times since the last TakeStats, and the thread DrawShadows
		// last ran on
		Stats hookStats;
		DWORD drawThreadId = 0;

		// Counts for the log at shutdown
		unsigned registeredCount     = 0;
		unsigned meshCasterCount     = 0;
		unsigned meshFallbackCount   = 0;
		unsigned floatingPropCount   = 0;
		unsigned prerenderedCount    = 0;
		unsigned indexedCount        = 0;
		unsigned quadOnlyCount       = 0;
		unsigned removedCount        = 0;
		unsigned staleRecordCount    = 0;
		unsigned passCount           = 0;
		unsigned drawCallCount       = 0;
		unsigned gatheredCount       = 0;
		unsigned notGatheredCount    = 0;
	}

	//// Private Functions

	namespace
	{
		/** Notes why a DrawShadows call was left entirely to the game, logging each reason once. */
		void NoteNotGathered(char const* reason, void const* manager)
		{
			notGatheredCount++;

			static std::unordered_set<char const*> logged;
			if (logged.insert(reason).second)
			{
				Log(LogCategory::Initialization, "shadow registry: DrawShadows call left native (%s, manager %p, registry %p)", reason, manager, casterManager);
			}
		}

		int64_t ReadTicks(void)
		{
			LARGE_INTEGER now{};
			QueryPerformanceCounter(&now);
			return now.QuadPart;
		}

		/** The entry holding the caster at an id, or null where there is none. The lock must be held. */
		Entry* FindEntry(int32_t id)
		{
			if (id < 0 || static_cast<size_t>(id) >= casters.size())
			{
				return nullptr;
			}

			// The id was checked to be positive above
			Entry& entry = casters[static_cast<size_t>(id)];
			return entry.caster ? &entry : nullptr;
		}

		/** Drops the caster at an id. The lock must be held. Returns whether there was one. */
		bool EraseCaster(int32_t id)
		{
			Entry* const entry = FindEntry(id);
			if (entry == nullptr)
			{
				return false;
			}

			*entry = Entry{};
			casterCount--;
			return true;
		}

		/** The inverse of an affine column-major matrix. False for any other, or a singular one. */
		bool InvertAffine(float const* m, float out[16])
		{
			if (m[3] != 0.0f || m[7] != 0.0f || m[11] != 0.0f || m[15] != 1.0f)
			{
				return false;
			}

			// The 3x3 part's inverse is its cofactors over the determinant, and the cofactor
			// rows are the cross products of its columns
			float const columns[3][3] = { { m[0], m[1], m[2] }, { m[4], m[5], m[6] }, { m[8], m[9], m[10] } };
			float rows[3][3] = {};

			for (unsigned row = 0; row < 3; row++)
			{
				float const* const a = columns[(row + 1) % 3];
				float const* const b = columns[(row + 2) % 3];
				rows[row][0] = a[1] * b[2] - a[2] * b[1];
				rows[row][1] = a[2] * b[0] - a[0] * b[2];
				rows[row][2] = a[0] * b[1] - a[1] * b[0];
			}

			float const determinant = columns[0][0] * rows[0][0] + columns[0][1] * rows[0][1] + columns[0][2] * rows[0][2];
			if (!std::isfinite(determinant) || std::fabs(determinant) < 1.0e-12f)
			{
				return false;
			}

			for (unsigned row = 0; row < 3; row++)
			{
				for (unsigned column = 0; column < 3; column++)
				{
					out[column * 4 + row] = rows[row][column] / determinant;
				}

				out[row * 4 + 3] = 0.0f;
			}

			// The translation goes back through the inverted 3x3 part
			for (unsigned row = 0; row < 3; row++)
			{
				out[12 + row] = -(out[row] * m[12] + out[4 + row] * m[13] + out[8 + row] * m[14]);
			}

			out[15] = 1.0f;
			return true;
		}

		/** GroundQuad for a box and placement height, which the gather has without the caster. */
		bool BoxGroundQuad(float const low[3], float const high[3], float baseHeight, float const sun[3], float quad[4][3])
		{
			if (sun[1] > HORIZON_SUN_Y)
			{
				return false;
			}

			// The box's bottom and top land this far along the sun, and the footprint spans the
			// box and both landings. An earlier version pushed all eight corners down the sun
			// one by one, which gives the same footprint for several times the work.
			float const height    = baseHeight + GROUND_LIFT_WORLD;
			float const alongLow  = (height - low[1]) / sun[1];
			float const alongHigh = (height - high[1]) / sun[1];
			float footLow[2]  = {};
			float footHigh[2] = {};

			for (unsigned axis = 0; axis < 2; axis++)
			{
				unsigned const world  = axis * 2;
				float const    atLow  = sun[world] * alongLow;
				float const    atHigh = sun[world] * alongHigh;
				footLow[axis]  = low[world] + std::min({ 0.0f, atLow, atHigh });
				footHigh[axis] = high[world] + std::max({ 0.0f, atLow, atHigh });
			}

			float const corners[4][2] = { { footLow[0], footLow[1] }, { footHigh[0], footLow[1] }, { footHigh[0], footHigh[1] }, { footLow[0], footHigh[1] } };
			for (unsigned index = 0; index < 4; index++)
			{
				quad[index][0] = corners[index][0];
				quad[index][1] = height;
				quad[index][2] = corners[index][1];
			}

			return true;
		}

		/** IsReachOutside for a box, which the gather has without the caster. */
		bool IsBoxReachOutside(float const low[3], float const high[3], bool hasQuad, float const quad[4][3], float const* worldToClip)
		{
			float reachLow[3]  = { low[0], low[1], low[2] };
			float reachHigh[3] = { high[0], high[1], high[2] };

			// The quad's first corner is its low one and its third its high one
			if (hasQuad)
			{
				for (unsigned axis = 0; axis < 3; axis++)
				{
					reachLow[axis]  = std::min(reachLow[axis], quad[0][axis]);
					reachHigh[axis] = std::max(reachHigh[axis], quad[2][axis]);
				}
			}

			// The matrix is affine, so the box's NDC extent is its centre and half size mapped
			// through it, without transforming each corner
			for (unsigned axis = 0; axis < 2; axis++)
			{
				float centre = worldToClip[12 + axis];
				float extent = 0.0f;

				for (unsigned k = 0; k < 3; k++)
				{
					centre += worldToClip[k * 4 + axis] * (reachLow[k] + reachHigh[k]) * 0.5f;
					extent += std::fabs(worldToClip[k * 4 + axis]) * (reachHigh[k] - reachLow[k]) * 0.5f;
				}

				if (centre + extent < -1.0f || centre - extent > 1.0f)
				{
					return true;
				}
			}

			return false;
		}

#if defined(_MSC_VER) && defined(_M_IX86)
		bool ReadTerrainGrid(void* terrain, TerrainGrid& grid)
		{
			void* const cellWidth  = ReadVirtualFunction(terrain, TERRAIN_CELL_WIDTH_SLOT);
			void* const cellCountX = ReadVirtualFunction(terrain, TERRAIN_CELL_COUNT_X_SLOT);
			void* const cellCountZ = ReadVirtualFunction(terrain, TERRAIN_CELL_COUNT_Z_SLOT);
			if (cellWidth == nullptr || cellCountX == nullptr || cellCountZ == nullptr)
			{
				return false;
			}

			// The slots hold the terrain's getters. CellWidth returns a float on the x87 stack
			// although cISTETerrainMap declares a uint32_t, so it is called as a float one.
			using FloatGetter   = float (__thiscall*)(void*);
			using CountGetter   = uint32_t (__thiscall*)(void*);
			__try
			{
				grid.cellWidth  = reinterpret_cast<FloatGetter>(cellWidth)(terrain);
				grid.cellCountX = reinterpret_cast<CountGetter>(cellCountX)(terrain);
				grid.cellCountZ = reinterpret_cast<CountGetter>(cellCountZ)(terrain);
				return true;
			}
			__except (EXCEPTION_EXECUTE_HANDLER)
			{
				return false;
			}
		}

		bool CallRedisplayTerrain(void* terrain, bool shouldUpdateView, int32_t const rectangle[4])
		{
			void* const function = ReadVirtualFunction(terrain, TERRAIN_REDISPLAY_SLOT);
			if (function == nullptr)
			{
				return false;
			}

			// RedisplayTerrain(updateTerrain, updateView, rect, flags), as
			// RedisplayStaticOverlay calls it
			using Redisplay = void (__thiscall*)(void*, bool, bool, int32_t const*, uint32_t);
			__try
			{
				reinterpret_cast<Redisplay>(function)(terrain, false, shouldUpdateView, rectangle, 0);
				return true;
			}
			__except (EXCEPTION_EXECUTE_HANDLER)
			{
				return false;
			}
		}

		bool ReadTerrainAltitude(void* terrain, float x, float z, float& altitude)
		{
			void* const function = ReadVirtualFunction(terrain, TERRAIN_ALTITUDE_SLOT);
			if (function == nullptr)
			{
				return false;
			}

			// GetAltitude(x, z), returning the drawn surface's height there
			using GetAltitude = float (__thiscall*)(void*, float, float);
			__try
			{
				altitude = reinterpret_cast<GetAltitude>(function)(terrain, x, z);
				return true;
			}
			__except (EXCEPTION_EXECUTE_HANDLER)
			{
				return false;
			}
		}
#else
		bool ReadTerrainGrid(void*, TerrainGrid&)
		{
			return false;
		}

		bool CallRedisplayTerrain(void*, bool, int32_t const*)
		{
			return false;
		}

		bool ReadTerrainAltitude(void*, float, float, float&)
		{
			return false;
		}
#endif

		/** The terrain and view update flag of the overlay manager the registry belongs to, the loaded city's. */
		bool ReadCityTerrain(void*& terrain, bool& shouldUpdateView)
		{
			void const* manager = nullptr;
			{
				std::lock_guard<std::mutex> lock(casterMutex);
				manager = casterManager;
			}

			uint8_t update = 0;
			if (manager == nullptr || !ReadGameMemory(static_cast<uint8_t const*>(manager) + MANAGER_TERRAIN, terrain) || terrain == nullptr || !ReadGameMemory(static_cast<uint8_t const*>(manager) + MANAGER_UPDATE_VIEW, update))
			{
				return false;
			}

			shouldUpdateView = update != 0;
			return true;
		}

		uint8_t const* RecordAddress(void const* manager, int32_t id)
		{
			uint8_t const* records = nullptr;
			if (id < 0 || !ReadGameMemory(static_cast<uint8_t const*>(manager) + MANAGER_RECORDS, records) || records == nullptr)
			{
				return nullptr;
			}

			// The id is a slot index, checked to be positive above
			return records + static_cast<size_t>(id) * RECORD_SIZE;
		}

		void ToWorld(Transform const& transform, float const local[3], float world[3])
		{
			float rotated[3] = { local[0], local[1], local[2] };

			if ((transform.flags[1] & 4) != 0)
			{
				for (unsigned row = 0; row < 3; row++)
				{
					rotated[row] = transform.rotation[row * 3 + 0] * local[0] + transform.rotation[row * 3 + 1] * local[1] + transform.rotation[row * 3 + 2] * local[2];
				}
			}

			float const scale = ((transform.flags[1] & 2) != 0) ? transform.scale : 1.0f;
			for (unsigned axis = 0; axis < 3; axis++)
			{
				world[axis] = rotated[axis] * scale + transform.translation[axis];
			}
		}

		/**
		 * Reads the mesh the stash site recorded, provided it belongs to the
		 * CreateOccupantShadow frame this AddShadow was called from. Every value is checked
		 * against what AddShadow itself received.
		 */
		bool RecoverIndices(AddShadowCall const& call, std::vector<uint32_t>& triangles)
		{
			uint8_t const* const frame = call.returnSlot + RETURN_SLOT_TO_FRAME;
			if (stashedFrame == nullptr || stashedFrame + STASH_TO_FRAME != frame)
			{
				return false;
			}

			void const* framePositions   = nullptr;
			void const* frameCoordinates = nullptr;
			if (!ReadGameMemory(frame + FRAME_POSITIONS, framePositions) || !ReadGameMemory(frame + FRAME_COORDINATES, frameCoordinates) || framePositions != call.positions || frameCoordinates != call.coordinates || frame + FRAME_TRANSFORM != call.transform)
			{
				return false;
			}

			// The mesh and its vertex count, which must be the call's
			MeshHeader mesh{};
			uint16_t vertexCount = 0;
			if (!ReadGameMemory(stashedMesh, mesh) || mesh.vertices == nullptr || mesh.primitives == nullptr || !ReadGameMemory(static_cast<uint8_t const*>(mesh.vertices) + VERTEX_BUFFER_COUNT, vertexCount) || vertexCount != call.count)
			{
				return false;
			}

			uint16_t const* indexBase = nullptr;
			if (mesh.indexHolder != nullptr && (!ReadGameMemory(mesh.indexHolder, indexBase) || indexBase == nullptr))
			{
				return false;
			}

			PrimitiveRun const* runs    = nullptr;
			PrimitiveRun const* runsEnd = nullptr;
			if (!ReadGameMemory(static_cast<uint8_t const*>(mesh.primitives) + PRIMITIVE_LIST_BEGIN, runs) || !ReadGameMemory(static_cast<uint8_t const*>(mesh.primitives) + PRIMITIVE_LIST_END, runsEnd) || runsEnd < runs || runsEnd - runs > ptrdiff_t{ MAXIMUM_PRIMITIVE_RUNS })
			{
				return false;
			}

			// Turn each run into triangles, through its indices or as a range of vertices
			std::vector<uint32_t> source;
			std::vector<uint16_t> packed;

			for (PrimitiveRun const* cursor = runs; cursor != runsEnd; cursor++)
			{
				PrimitiveRun run{};
				if (!ReadGameMemory(cursor, run) || run.count > MAXIMUM_INDICES || run.start > MAXIMUM_INDICES)
				{
					return false;
				}

				source.resize(run.count);
				if (indexBase != nullptr)
				{
					packed.resize(run.count);
					if (run.count != 0 && !CopyGameMemory(indexBase + run.start, packed.data(), run.count * sizeof(uint16_t)))
					{
						return false;
					}

					std::copy(packed.begin(), packed.end(), source.begin());
				}
				else
				{
					for (uint32_t index = 0; index < run.count; index++)
					{
						source[index] = run.start + index;
					}
				}

				uint32_t const count = vertexCount;
				if (std::any_of(source.begin(), source.end(), [count](uint32_t index) { return index >= count; }))
				{
					return false;
				}

				AppendTriangles(run.primitive, source.data(), run.count, triangles);
				if (triangles.size() > MAXIMUM_INDICES)
				{
					return false;
				}
			}

			return !triangles.empty();
		}

		/** The root mean square distance between the mesh's coordinates and the projector's for the same positions. */
		double ProjectorResidual(Caster const& caster, float const* coordinates, uint32_t count)
		{
			if (count == 0)
			{
				return 0.0;
			}

			double squared = 0.0;
			float const* const m = caster.projector;

			for (uint32_t vertex = 0; vertex < count; vertex++)
			{
				float const* const world = caster.vertices.data() + size_t{ vertex } * VERTEX_FLOATS;
				double const du = double{ m[0] * world[0] + m[4] * world[1] + m[8] * world[2] + m[12] } - coordinates[size_t{ vertex } * 2 + 0];
				double const dv = double{ m[1] * world[0] + m[5] * world[1] + m[9] * world[2] + m[13] } - coordinates[size_t{ vertex } * 2 + 1];
				squared += du * du + dv * dv;
			}

			return std::sqrt(squared / count);
		}

		void Forget(int32_t id)
		{
			std::lock_guard<std::mutex> lock(casterMutex);
			EraseCaster(id);
		}

		/**
		 * Decides how a relaxed prop casts: not at all when it floats, through the projector
		 * when that reproduces its coordinates, through its own coordinates when its
		 * triangles are known, and otherwise through the per-draw path.
		 */
		void SortRelaxedProp(AddShadowCall const& call, Caster& caster, std::vector<float> const& coordinates, bool isIndexed)
		{
			// The ground under the prop's middle, or its own lowest point where that is unknown
			void* terrain = nullptr;
			float ground  = caster.low[1];

			if (ReadGameMemory(static_cast<uint8_t const*>(call.manager) + MANAGER_TERRAIN, terrain) && terrain != nullptr)
			{
				float altitude = 0.0f;
				if (ReadTerrainAltitude(terrain, (caster.low[0] + caster.high[0]) * 0.5f, (caster.low[2] + caster.high[2]) * 0.5f, altitude) && std::isfinite(altitude))
				{
					ground = altitude;
				}
			}

			uint32_t const vertexCount = static_cast<uint32_t>(call.count);

			if (caster.low[1] - ground > FLOATING_PROP_HEIGHT)
			{
				caster.isSuppressedOnly = true;
				floatingPropCount++;
			}
			else if (ProjectorResidual(caster, coordinates.data(), vertexCount) < PRERENDERED_RESIDUAL)
			{
				prerenderedCount++;
			}
			else if (isIndexed)
			{
				// Into the vertices beside the positions, where the shadow pass reads them
				caster.hasMeshCoordinates = true;
				for (uint32_t vertex = 0; vertex < vertexCount; vertex++)
				{
					memcpy(caster.vertices.data() + size_t{ vertex } * VERTEX_FLOATS + 3, coordinates.data() + size_t{ vertex } * 2, sizeof(float) * 2);
				}

				meshCasterCount++;
			}
			else
			{
				// Without triangles the coordinates cannot be drawn. The prop casts through the
				// per-draw path instead, and its record, whose projector would stamp a slab, is
				// only kept out of DrawShadows.
				caster.isSuppressedOnly = true;
				LiveShadowHooks::RegisterPropMesh(vertexCount, call.positions, call.coordinates);
				meshFallbackCount++;
			}
		}

		/** Keeps the record an AddShadow call made, or forgets its id where the record cannot be read. */
		void Register(AddShadowCall const& call)
		{
			// The id is the record's slot index, never negative
			if (call.binding == nullptr || call.positions == nullptr || call.coordinates == nullptr || call.transform == nullptr || call.count < 3 || static_cast<uint32_t>(call.count) > MAXIMUM_VERTICES || static_cast<uint32_t>(call.id) >= MAXIMUM_CASTER_IDS)
			{
				Forget(call.id);
				return;
			}

			bool const    isTiming    = IsLogged(LOG_LEVEL_DEBUG);
			int64_t const startTicks  = isTiming ? ReadTicks() : 0;
			uint32_t const vertexCount = static_cast<uint32_t>(call.count);
			auto caster = std::make_shared<Caster>();
			caster->id      = static_cast<uint32_t>(call.id);
			caster->manager = call.manager;
			caster->binding = call.binding;

			// Read the record and the mesh's streams
			uint8_t const* const record = RecordAddress(call.manager, call.id);
			void const* recordBinding = nullptr;
			Transform transform{};
			std::vector<float> local(size_t{ vertexCount } * 3);
			std::vector<float> coordinates(size_t{ vertexCount } * 2);

			if (record == nullptr || !CopyGameMemory(record + RECORD_PROJECTOR, caster->projector, sizeof(caster->projector)) || !ReadGameMemory(record + RECORD_BINDING, recordBinding) || recordBinding != call.binding || !ReadGameMemory(call.transform, transform) || !CopyGameMemory(call.positions, local.data(), local.size() * sizeof(float)) || !CopyGameMemory(call.coordinates, coordinates.data(), coordinates.size() * sizeof(float)))
			{
				Forget(call.id);
				return;
			}

			// Place the mesh in the world and bound it and its coordinates. The coordinates only
			// go into the vertices for a caster cast through them, and stay zero otherwise.
			caster->vertices.resize(size_t{ vertexCount } * VERTEX_FLOATS);
			for (unsigned axis = 0; axis < 3; axis++)
			{
				caster->low[axis]  = FLT_MAX;
				caster->high[axis] = -FLT_MAX;
			}

			caster->uvBounds[0] = FLT_MAX;
			caster->uvBounds[1] = FLT_MAX;
			caster->uvBounds[2] = -FLT_MAX;
			caster->uvBounds[3] = -FLT_MAX;

			for (uint32_t vertex = 0; vertex < vertexCount; vertex++)
			{
				float* const world = caster->vertices.data() + size_t{ vertex } * VERTEX_FLOATS;
				ToWorld(transform, local.data() + size_t{ vertex } * 3, world);

				for (unsigned axis = 0; axis < 3; axis++)
				{
					caster->low[axis]  = std::min(caster->low[axis], world[axis]);
					caster->high[axis] = std::max(caster->high[axis], world[axis]);
				}

				for (unsigned axis = 0; axis < 2; axis++)
				{
					float const value = coordinates[size_t{ vertex } * 2 + axis];
					caster->uvBounds[axis]     = std::min(caster->uvBounds[axis], value);
					caster->uvBounds[axis + 2] = std::max(caster->uvBounds[axis + 2], value);
				}
			}

			caster->baseHeight = transform.translation[1];

			if (!std::isfinite(caster->low[0]) || !std::isfinite(caster->high[0]) || !std::isfinite(caster->uvBounds[0]) || !std::isfinite(caster->uvBounds[2]))
			{
				Forget(call.id);
				return;
			}

			bool const isIndexed = RecoverIndices(call, caster->indices);
			if (!isIndexed)
			{
				caster->indices.clear();
			}

			if (call.isRelaxedProp)
			{
				SortRelaxedProp(call, *caster, coordinates, isIndexed);
			}

			// Keep it, starting over for a new city
			std::lock_guard<std::mutex> lock(casterMutex);
			if (casterManager != call.manager)
			{
				// A new overlay manager is a new city, and every id belongs to it now
				if (casterManager != nullptr)
				{
					Log(LogCategory::Initialization, "shadow registry: overlay manager changed, %u casters dropped", static_cast<unsigned>(casters.size()));
				}

				casters.clear();
				casterCount = 0;
				passSerial++;
				pendingPass.casters.clear();
				casterManager = call.manager;
			}

			// Grow by half again rather than to the id, so a city filling its slots in order
			// does not reallocate the array for each one
			size_t const slot = static_cast<size_t>(call.id);
			if (slot >= casters.size())
			{
				casters.resize(std::max(slot + 1, casters.size() + casters.size() / 2));
			}

			Entry& entry = casters[slot];
			if (!entry.caster)
			{
				casterCount++;
			}

			entry = Entry{};
			entry.binding          = caster->binding;
			entry.baseHeight       = caster->baseHeight;
			entry.isSuppressedOnly = caster->isSuppressedOnly;
			memcpy(entry.low, caster->low, sizeof(entry.low));
			memcpy(entry.high, caster->high, sizeof(entry.high));
			entry.caster = std::move(caster);

			highestCasterId = std::max(highestCasterId, static_cast<uint32_t>(call.id));
			registeredCount++;

			if (isTiming)
			{
				hookStats.registrations++;
				hookStats.registerTicks += ReadTicks() - startTicks;

				if (GetCurrentThreadId() != drawThreadId)
				{
					hookStats.offThreadRegistrations++;
				}
			}

			if (isIndexed)
			{
				indexedCount++;
			}
			else
			{
				quadOnlyCount++;
			}

			static bool hasLoggedFirst = false;
			if (!hasLoggedFirst)
			{
				hasLoggedFirst = true;
				Log(LogCategory::Initialization, "shadow registry: first caster registered (id %d, %d vertices, %s)", call.id, call.count, isIndexed ? "indexed" : "ground quad only");
			}
		}

		/**
		 * Takes a pass's overlay ids from a DrawShadows call. The registry's own records move
		 * into the pass, and every other id, along with any record that no longer matches
		 * what was registered, stays in kept for the game to draw. Records that cannot reach
		 * the pass's view are dropped from both. False leaves the caller to draw the original
		 * list.
		 */
		bool Gather(void* manager, float const* eyeToWorld, IdList const* ids, std::vector<int32_t>& kept)
		{
			kept.clear();

			// Copy the ids out of the game's list
			IdList list{};
			if (!ReadGameMemory(ids, list) || list.begin == nullptr || list.end <= list.begin)
			{
				NoteNotGathered("empty or unreadable id list", manager);
				return false;
			}

			size_t const count = static_cast<size_t>(list.end - list.begin);
			if (count > MAXIMUM_IDS_PER_PASS)
			{
				NoteNotGathered("id list too long", manager);
				return false;
			}

			kept.resize(count);
			if (!CopyGameMemory(list.begin, kept.data(), count * sizeof(int32_t)))
			{
				NoteNotGathered("id list unreadable", manager);
				return false;
			}

			// The view, sun and shadow parameters the pass is drawn with
			float matrix[16] = {};
			float sun[3]     = {};
			float colour[3]  = {};
			float strength   = 0.0f;
			float alphaScale = 0.0f;

			bool const hasMatrix = eyeToWorld != nullptr && CopyGameMemory(eyeToWorld, matrix, sizeof(matrix));
			bool const hasSun    = LiveShadowHooks::SunDirection(sun);
			LiveShadowHooks::ShadowParams(colour, strength);
			ReadGameMemory(shadowAlphaScale, alphaScale);

			if (!hasMatrix)
			{
				NoteNotGathered("eye-to-world matrix unreadable", manager);
				return false;
			}

			uint8_t const* records = nullptr;
			if (!ReadGameMemory(static_cast<uint8_t const*>(manager) + MANAGER_RECORDS, records) || records == nullptr)
			{
				NoteNotGathered("record table unreadable", manager);
				return false;
			}

			// The x and y rows of world to clip, for culling against the pass's own view
			//
			// The game hands a strip pass thousands of casters, nearly all far from it. Only an
			// orthographic projection with a known sun culls; anything else hands every caster
			// over, as before.
			float const* const p = currentProjection;
			float worldToEye[16]  = {};
			float worldToClip[16] = {};
			float sunWorld[3]     = { sun[0], sun[1], sun[2] };
			float const sunLength = std::sqrt(sun[0] * sun[0] + sun[1] * sun[1] + sun[2] * sun[2]);
			bool const  isCulled  = hasSun && sunLength > 1.0e-6f && isProjectionKnown && p[3] == 0.0f && p[7] == 0.0f && p[11] == 0.0f && p[15] == 1.0f && InvertAffine(matrix, worldToEye);

			if (isCulled)
			{
				for (float& value : sunWorld)
				{
					value /= sunLength;
				}

				for (unsigned column = 0; column < 4; column++)
				{
					for (unsigned row = 0; row < 2; row++)
					{
						worldToClip[column * 4 + row] = p[row] * worldToEye[column * 4] + p[4 + row] * worldToEye[column * 4 + 1] + p[8 + row] * worldToEye[column * 4 + 2] + p[12 + row] * worldToEye[column * 4 + 3];
					}
				}
			}

			std::lock_guard<std::mutex> lock(casterMutex);
			if (manager != casterManager)
			{
				NoteNotGathered("not the registry's overlay manager", manager);
				return false;
			}

			if (casterCount == 0)
			{
				NoteNotGathered("registry empty", manager);
				return false;
			}

			gatheredCount++;

			// Move the registry's records out of the list, keeping the rest in order
			size_t written = 0;
			for (size_t read = 0; read < kept.size(); read++)
			{
				int32_t const id    = kept[read];
				Entry* const  entry = FindEntry(id);
				if (entry == nullptr)
				{
					kept[written++] = id;
					continue;
				}

				// Cull from the entry alone, before the record is read. A record the game reused
				// without RemoveOverlay is then only noticed once it comes into view.
				if (isCulled)
				{
					float quad[4][3] = {};
					bool const hasQuad = BoxGroundQuad(entry->low, entry->high, entry->baseHeight, sunWorld, quad);
					if (IsBoxReachOutside(entry->low, entry->high, hasQuad, quad, worldToClip))
					{
						continue;
					}
				}

				// The in-use word, the flags and the binding in one guarded read, instead of a
				// guarded read each. FindEntry checked the id to be positive.
				uint8_t header[RECORD_BINDING + sizeof(void*)] = {};
				int32_t     inUse         = 0;
				uint32_t    flags         = 0;
				void const* binding       = nullptr;
				void const* textureObject = nullptr;
				uint32_t    texture       = 0;

				bool const isReadable = CopyGameMemory(records + static_cast<size_t>(id) * RECORD_SIZE, header, sizeof(header));
				if (isReadable)
				{
					memcpy(&inUse, header, sizeof(inUse));
					memcpy(&flags, header + RECORD_FLAGS, sizeof(flags));
					memcpy(&binding, header + RECORD_BINDING, sizeof(binding));
				}

				if (!isReadable || inUse != RECORD_IN_USE || binding != entry->binding)
				{
					// The slot was freed or reused by something that never went through
					// AddShadow, such as a decal. It is not the registry's any more.
					staleRecordCount++;
					EraseCaster(id);
					kept[written++] = id;
					continue;
				}

				// A disabled record, which the game skips too
				if ((flags & SHADOW_DRAW_FLAGS) != SHADOW_DRAW_FLAGS || entry->isSuppressedOnly)
				{
					continue;
				}

				if (!ReadGameMemory(binding, textureObject) || textureObject == nullptr || !ReadGameMemory(textureObject, texture))
				{
					kept[written++] = id;
					continue;
				}

				// The serial keeps a caster listed twice in one pass from joining it twice, where
				// a set of the pass's ids cost an allocation per caster
				if (entry->passSerial != passSerial)
				{
					entry->passSerial = passSerial;
					pendingPass.casters.push_back(PassCaster{ entry->caster, texture, (flags & WRAP_FLAG) != 0 });
				}
			}

			kept.resize(written);

			memcpy(pendingPass.eyeToWorld, matrix, sizeof(matrix));
			if (hasSun)
			{
				memcpy(pendingPass.sunDirection, sun, sizeof(sun));
			}

			pendingPass.isSunValid = hasSun;
			memcpy(pendingPass.colour, colour, sizeof(colour));
			pendingPass.strength   = strength;
			pendingPass.alphaScale = alphaScale;
			pendingPass.isCulled   = isCulled;
			if (isCulled)
			{
				memcpy(pendingPass.projection, currentProjection, sizeof(pendingPass.projection));
			}

			isPassPending = true;
			return true;
		}

		/** The cell index of a world coordinate on one axis, moved by round and clamped to the city. */
		int32_t CellIndex(float value, float cellWidth, uint32_t cellCount, float round)
		{
			double const index = std::floor(double{ value / cellWidth + round });
			return static_cast<int32_t>(std::clamp(index, 0.0, static_cast<double>(cellCount)));
		}

		/** Drops every caster and gives the array's memory back. */
		void ClearCasters(void)
		{
			std::lock_guard<std::mutex> lock(casterMutex);
			std::vector<Entry>().swap(casters);
			casterCount   = 0;
			casterManager = nullptr;
			pendingPass   = Pass{};
			isPassPending = false;
			passSerial++;
		}
	}

	//// Hook Bodies

	namespace
	{
#if defined(_MSC_VER) && defined(_M_IX86)
		/**
		 * AddShadow's replacement in the overlay manager's table.
		 *
		 * Only Install's MSVC path puts it in. _AddressOfReturnAddress is an MSVC intrinsic
		 * GCC declares but never defines, so a GCC Debug build, which keeps the unused
		 * function, failed to link.
		 */
		int32_t __fastcall AddShadowHook(void* manager, void*, void const* binding, int32_t count, void const* positions, void const* coordinates, void const* box, void const* transform)
		{
			bool const isRelaxedProp = isMeshCasterExpected;
			isMeshCasterExpected = false;

			int32_t const id = originalAddShadow(manager, binding, count, positions, coordinates, box, transform);
			if (id < 0)
			{
				// CalcShadowProjection rejected the fit, so there is no record to keep, and a
				// relaxed prop casts through the per-draw path as before
				if (isRelaxedProp && count > 0)
				{
					LiveShadowHooks::RegisterPropMesh(static_cast<uint32_t>(count), positions, coordinates);
					meshFallbackCount++;
				}

				return id;
			}

			// The return address's slot is where CreateOccupantShadow's frame is found from
			AddShadowCall call;
			call.manager       = manager;
			call.id            = id;
			call.binding       = binding;
			call.count         = count;
			call.positions     = positions;
			call.coordinates   = coordinates;
			call.transform     = transform;
			call.returnSlot    = static_cast<uint8_t const*>(_AddressOfReturnAddress());
			call.isRelaxedProp = isRelaxedProp;

			try
			{
				Register(call);
			}
			catch (std::bad_alloc const&)
			{
				Forget(id);
			}

			return id;
		}
#endif

		void __fastcall RemoveOverlayHook(void* manager, void*, int32_t id)
		{
			originalRemoveOverlay(manager, id);

			// Negative ids are handles into the manager's +0xAC table. AddShadow never returns
			// one, and a group's members come back through here.
			if (id < 0)
			{
				return;
			}

			std::lock_guard<std::mutex> lock(casterMutex);
			if (manager == casterManager && EraseCaster(id))
			{
				removedCount++;
			}
		}

		void DrawFiltered(DrawShadowsFunction original, void* manager, float const* eyeToWorld, void* drawContext, IdList* ids)
		{
			// Only the render thread draws overlays, so one list is enough
			static std::vector<int32_t> kept;
			drawCallCount++;

			bool const    isTiming   = IsLogged(LOG_LEVEL_DEBUG);
			int64_t const startTicks = isTiming ? ReadTicks() : 0;

			bool isGathered = false;
			try
			{
				isGathered = Gather(manager, eyeToWorld, ids, kept);
			}
			catch (std::bad_alloc const&)
			{
			}

			int64_t const gatheredTicks = isTiming ? ReadTicks() : 0;

			// DrawShadows reads only begin and end, and returns at once when they are equal
			IdList filtered{};
			if (isGathered && !kept.empty())
			{
				filtered = IdList{ kept.data(), kept.data() + kept.size(), kept.data() + kept.size() };
			}

			original(manager, eyeToWorld, drawContext, isGathered ? &filtered : ids);

			if (isTiming)
			{
				int64_t const drawnTicks = ReadTicks();
				std::lock_guard<std::mutex> lock(casterMutex);
				drawThreadId = GetCurrentThreadId();
				hookStats.gatherTicks     += gatheredTicks - startTicks;
				hookStats.nativeDrawTicks += drawnTicks - gatheredTicks;
			}
		}

#if defined(_MSC_VER) && defined(_M_IX86)
		/**
		 * DrawShadows' trampoline. Entered by a call carrying the original arguments, so the
		 * replayed SUB ESP builds the original frame and the body's RET 0xC returns here.
		 */
		__declspec(naked) void DrawShadowsOriginal(void)
		{
			__asm
			{
				sub  esp, 0x1c4
				jmp  dword ptr [drawShadowsRejoin]
			}
		}

		__declspec(naked) void DrawShadowsRoughOriginal(void)
		{
			__asm
			{
				sub  esp, 0x9c
				jmp  dword ptr [drawShadowsRoughRejoin]
			}
		}

		void __fastcall DrawShadowsHook(void* manager, void*, float const* eyeToWorld, void* drawContext, IdList* ids)
		{
			// The trampoline carries on into DrawShadows, so it is called as that function
			DrawFiltered(reinterpret_cast<DrawShadowsFunction>(&DrawShadowsOriginal), manager, eyeToWorld, drawContext, ids);
		}

		void __fastcall DrawShadowsRoughHook(void* manager, void*, float const* eyeToWorld, void* drawContext, IdList* ids)
		{
			// As in DrawShadowsHook
			DrawFiltered(reinterpret_cast<DrawShadowsFunction>(&DrawShadowsRoughOriginal), manager, eyeToWorld, drawContext, ids);
		}

		/** Records the mesh in EDI and the frame, then replays the three displaced instructions. ESP is the game's, so the load from it is unchanged. */
		__declspec(naked) void MeshStashStub(void)
		{
			__asm
			{
				mov  dword ptr [stashedMesh], edi
				mov  dword ptr [stashedFrame], esp
				mov  eax, dword ptr [edi]
				mov  ecx, dword ptr [eax]
				mov  ebx, dword ptr [esp + 0xc0]
				jmp  dword ptr [meshStashRejoin]
			}
		}
#endif
	}

	//// Public API

	bool AppendTriangles(uint32_t primitive, uint32_t const* indices, uint32_t count, std::vector<uint32_t>& triangles)
	{
		switch (primitive)
		{
		case PRIMITIVE_TRIANGLES:
			for (uint32_t index = 0; index + 2 < count; index += 3)
			{
				triangles.insert(triangles.end(), { indices[index], indices[index + 1], indices[index + 2] });
			}

			return count >= 3;

		// The shadow pass does not cull, so the strip's alternating winding does not matter
		case PRIMITIVE_TRIANGLE_STRIP:
			for (uint32_t index = 0; index + 2 < count; index++)
			{
				triangles.insert(triangles.end(), { indices[index], indices[index + 1], indices[index + 2] });
			}

			return count >= 3;

		case PRIMITIVE_TRIANGLE_FAN:
			for (uint32_t index = 1; index + 1 < count; index++)
			{
				triangles.insert(triangles.end(), { indices[0], indices[index], indices[index + 1] });
			}

			return count >= 3;

		case PRIMITIVE_QUADS:
			for (uint32_t index = 0; index + 3 < count; index += 4)
			{
				triangles.insert(triangles.end(), { indices[index], indices[index + 1], indices[index + 2], indices[index], indices[index + 2], indices[index + 3] });
			}

			return count >= 4;

		case PRIMITIVE_QUAD_STRIP:
			for (uint32_t index = 0; index + 3 < count; index += 2)
			{
				triangles.insert(triangles.end(), { indices[index], indices[index + 1], indices[index + 2], indices[index + 2], indices[index + 1], indices[index + 3] });
			}

			return count >= 4;

		default:
			return false;
		}
	}

	bool Install(void)
	{
#if defined(_MSC_VER) && defined(_M_IX86)
		if (isInstalled)
		{
			return true;
		}

		// Part of live shadows. -ShadowRegistry:off keeps the game's own BAT shadows, for
		// comparing pictures.
		if (!LiveShadowHooks::Enabled() || HasCommandLineSwitch("-ShadowRegistry:off"))
		{
			return false;
		}

		if (GetGameVersion() != SUPPORTED_GAME_VERSION)
		{
			Log(LogCategory::Initialization, "shadow registry: requires SimCity 4 1.1.641, skipping");
			return false;
		}

		// Find the game's functions and the trampolines' way back, wherever it was loaded
		//
		// AddShadow and RemoveOverlay are __thiscall functions of the overlay manager, called
		// through these pointers as the table would.
		originalAddShadow      = reinterpret_cast<AddShadowFunction>(ResolveGameAddress(ADD_SHADOW_ADDRESS));
		originalRemoveOverlay  = reinterpret_cast<RemoveOverlayFunction>(ResolveGameAddress(REMOVE_OVERLAY_ADDRESS));
		drawShadowsRejoin      = ResolveGameAddress(DRAW_SHADOWS_REJOIN_ADDRESS);
		drawShadowsRoughRejoin = ResolveGameAddress(DRAW_SHADOWS_ROUGH_REJOIN_ADDRESS);
		meshStashRejoin        = ResolveGameAddress(MESH_STASH_REJOIN_ADDRESS);
		shadowAlphaScale       = ResolveGameAddress(SHADOW_ALPHA_SCALE_ADDRESS);

		if (originalAddShadow == nullptr)
		{
			return false;
		}

		addShadowSlot         = GamePatch::Pointer(ADD_SHADOW_SLOT_ADDRESS, ADD_SHADOW_ADDRESS, &AddShadowHook);
		removeOverlaySlot     = GamePatch::Pointer(REMOVE_OVERLAY_SLOT_ADDRESS, REMOVE_OVERLAY_ADDRESS, &RemoveOverlayHook);
		drawShadowsEntry      = GamePatch::Jump(DRAW_SHADOWS_ADDRESS, { 0x81, 0xEC, 0xC4, 0x01, 0x00, 0x00 }, &DrawShadowsHook);
		drawShadowsRoughEntry = GamePatch::Jump(DRAW_SHADOWS_ROUGH_ADDRESS, { 0x81, 0xEC, 0x9C, 0x00, 0x00, 0x00 }, &DrawShadowsRoughHook);
		meshStash             = GamePatch::Jump(MESH_STASH_ADDRESS, { 0x8B, 0x07, 0x8B, 0x08, 0x8B, 0x9C, 0x24, 0xC0, 0x00, 0x00, 0x00 }, &MeshStashStub);

		// AddShadow goes last: nothing is registered before the draw filter and the removal
		// hook that keep the registry honest are in place
		GamePatch* const patches[] = { &meshStash, &removeOverlaySlot, &drawShadowsEntry, &drawShadowsRoughEntry, &addShadowSlot };

		for (GamePatch const* patch : patches)
		{
			if (!patch->IsOriginal())
			{
				Log(LogCategory::Initialization, "shadow registry: guard failed at 0x%08lX, skipping", patch->GameAddress());
				return false;
			}
		}

		ClearCasters();

		for (GamePatch* patch : patches)
		{
			if (patch->Write())
			{
				continue;
			}

			Log(LogCategory::Initialization, "shadow registry: failed to write 0x%08lX, restoring", patch->GameAddress());
			for (GamePatch* written : patches)
			{
				written->Restore();
			}

			return false;
		}

		isInstalled = true;
		Log(LogCategory::Initialization, "shadow registry installed: AddShadow records render through the live shadow map");
		return true;
#else
		return false;
#endif
	}

	void Uninstall(void)
	{
		if (!isInstalled)
		{
			return;
		}

		GamePatch* const patches[] = { &addShadowSlot, &drawShadowsRoughEntry, &drawShadowsEntry, &removeOverlaySlot, &meshStash };
		for (GamePatch* patch : patches)
		{
			if (!patch->Restore())
			{
				Log(LogCategory::Initialization, "shadow registry: not restoring modified site 0x%08lX", patch->GameAddress());
			}
		}

		isInstalled = false;

		size_t remaining = 0;
		{
			std::lock_guard<std::mutex> lock(casterMutex);
			remaining = casterCount;
		}

		ClearCasters();

		Log(LogCategory::Initialization, "shadow registry uninstalled: relaxed props: %u True3D kept with their UVs, %u prerendered through the projector, %u floating left unshadowed, %u left to per-draw capture", meshCasterCount, prerenderedCount, floatingPropCount, meshFallbackCount);
		Log(LogCategory::Initialization, "shadow registry uninstalled: %u registered (%u indexed, %u ground quad only), %u removed, %u stale, %u passes, %u live at exit; DrawShadows calls %u (%u gathered, %u left native); highest id %u", registeredCount, indexedCount, quadOnlyCount, removedCount, staleRecordCount, passCount, static_cast<unsigned>(remaining), drawCallCount, gatheredCount, notGatheredCount, highestCasterId);
	}

	bool Enabled(void)
	{
		return isInstalled;
	}

	void ExpectMeshCaster(void)
	{
		isMeshCasterExpected = isInstalled;
	}

	void NoteProjection(float const projection[16])
	{
		if (!isInstalled)
		{
			return;
		}

		memcpy(currentProjection, projection, sizeof(currentProjection));
		isProjectionKnown = true;
	}

	bool TakePass(Pass& pass)
	{
		std::lock_guard<std::mutex> lock(casterMutex);
		if (!isPassPending)
		{
			return false;
		}

		pass = std::move(pendingPass);
		pendingPass   = Pass{};
		isPassPending = false;
		passSerial++;
		passCount++;
		return true;
	}

	Stats TakeStats(void)
	{
		std::lock_guard<std::mutex> lock(casterMutex);
		Stats const taken = hookStats;
		hookStats = Stats{};
		return taken;
	}

	bool GroundQuad(Caster const& caster, float const sun[3], float quad[4][3])
	{
		return BoxGroundQuad(caster.low, caster.high, caster.baseHeight, sun, quad);
	}

	bool IsReachOutside(Caster const& caster, bool hasQuad, float const quad[4][3], float const* worldToClip)
	{
		return IsBoxReachOutside(caster.low, caster.high, hasQuad, quad, worldToClip);
	}

	bool RedisplayWorldRect(float minimumX, float minimumZ, float maximumX, float maximumZ)
	{
		void* terrain = nullptr;
		bool  shouldUpdateView = false;
		return isInstalled && ReadCityTerrain(terrain, shouldUpdateView) && RedisplayTerrainRect(terrain, shouldUpdateView, minimumX, minimumZ, maximumX, maximumZ);
	}

	bool RedisplayTerrainRect(void* terrain, bool shouldUpdateView, float minimumX, float minimumZ, float maximumX, float maximumZ)
	{
		TerrainGrid grid;
		if (!isInstalled || terrain == nullptr || !(minimumX <= maximumX) || !(minimumZ <= maximumZ) || !ReadTerrainGrid(terrain, grid) || !(grid.cellWidth > 0.0f) || grid.cellCountX == 0 || grid.cellCountZ == 0)
		{
			return false;
		}

		// A cell of margin on every side, clamped to the city: RedisplayTerrain reads the
		// altitudes under the rectangle
		int32_t const rectangle[4] =
		{
			CellIndex(minimumX, grid.cellWidth, grid.cellCountX, -1.0f),
			CellIndex(minimumZ, grid.cellWidth, grid.cellCountZ, -1.0f),
			CellIndex(maximumX, grid.cellWidth, grid.cellCountX, 2.0f),
			CellIndex(maximumZ, grid.cellWidth, grid.cellCountZ, 2.0f),
		};

		if (rectangle[2] <= rectangle[0] || rectangle[3] <= rectangle[1])
		{
			return false;
		}

		return CallRedisplayTerrain(terrain, shouldUpdateView, rectangle);
	}
}
