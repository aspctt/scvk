/*
 *  SCD3D11 - a free Direct3D 11 driver for SimCity 4's SimGL interface
 *  Copyright (C) 2026
 *
 *  This library is free software; you can redistribute it and/or
 *  modify it under the terms of the GNU Lesser General Public
 *  License as published by the Free Software Foundation, under
 *  version 2.1 of the License, or (at your option) any later version.
 */

// Shadow records for the live shadow map (docs/true3d-shadows-replace-all.md).
//
// SimCity 4 Deluxe 1.1.641, Windows x86, image base 0x00400000. Opt-in with
// -NativeShadowMasks:replace. Every site is guarded by its exact bytes or slot
// value and restored during PreAppShutdown.
//
//   0x00AB3A14  cSTEOverlayManager vtable +0x2C, AddShadow 0x0073C0D0
//   0x00AB3A24  cSTEOverlayManager vtable +0x3C, RemoveOverlay 0x007388C0
//   0x00736BF0  DrawShadows entry, SUB ESP,0x1C4
//   0x00737870  DrawShadowsRough entry, SUB ESP,0x9C
//   0x00491894  CreateOccupantShadow, the last point the current mesh is in EDI
//
// DrawShadows and DrawShadowsRough are each called once, from DrawOverlays at
// 0x0073935F and 0x00739373. The hooks sit on the functions rather than those
// calls because SC4RenderServices owns the calls: it rewrites their rel32,
// calls the target it found, and uninstalls and reinstalls later, restoring
// the rel32 it saved - which silently dropped a call-site hook installed in
// between. It always ends up calling the functions themselves, so both run in
// either order.
//
// AddShadow (__thiscall, RET 0x18) returns the record's slot index, never a
// negative handle. The record is 0xB4 bytes in the slot array at manager +0x98:
// +0x00 is -1 while the slot is in use, +0x04 the flags (AddShadow ORs in 8),
// +0x1C the CalcShadowProjection matrix and +0xAC the AddRef'd texture binding,
// whose first word points at the texture object whose first word is the
// cGDriver texture name (DrawShadows passes **binding to SetTexture).
//
// Why the draw is filtered rather than switched off at 0x00739340: AddShadowDecal
// (+0x24, 0x0073B3B0) also ORs flag 8 into records in the same slot array, so the
// network FSH decal shadows are drawn by the same DrawShadows call, and its only
// filter is (flags & 9) == 9. Switching the call off would remove them too. The
// entry hooks instead drop exactly the ids this registry owns and draw the rest
// natively. They also run where SC4 knows what a pass needs: the list is
// the pass's visible overlays, the first argument is the eye-to-world texgen
// matrix (texgen source 0x10 is eye space, so DrawShadows' texture matrix is
// projector * eyeToWorld), and ShadowQuality/shadow-toggle gating has happened.
// Enable state is read from the record's flags there, which follows
// SetOverlayEnabled and any other path that changes them.
//
// AddShadow receives positions and UVs but no index buffer. The mesh
// CreateOccupantShadow is iterating is in EDI from 0x00491841 until 0x004918AF
// replaces it with the material's texture binding, so the site at 0x00491894
// records it. Its layout matches SC4DrawContext::RenderMesh (0x007D4A80):
// +0 vertex buffer {+0 format, +6 u16 count, +8 data}, +4 index holder whose
// first word is the u16 index array (null for DrawArrays meshes), +8 primitive
// list {+4 begin, +8 end} of {primitive, start, count}.
//
// Props without Is Ground Model (0x8A5E5DB8) are never shadowed by SC4. With
// -NativeShadowMasks:replace, NativeShadowMasks' site D lets their AddShadow run
// and flags it (ExpectMeshCaster); the record then only exists for its lifetime
// and dirty rectangle. Such a prop floating above the terrain - the building
// zots AddBuildingZotProp places, 46-56 m up - stays unshadowed; one whose UVs
// the projector reproduces is a prerendered BAT prop and casts through the
// projector; anything else is a True3D unwrap and casts through its own UVs.
//
// RedisplayWorldRect reuses what RedisplayStaticOverlay (0x00737D40) does: the
// manager's terrain (+0x88) RedisplayTerrain (vtable +0x110, 0x00749850) with a
// rectangle of terrain cells, which queues a partial static pass.

#include "NativeShadowRegistry.h"

#include "ModuleLog.h"
#include "NativeShadowMasks.h"
#include "Settings.h"

#include <algorithm>
#include <array>
#include <cfloat>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <initializer_list>
#include <mutex>
#include <unordered_map>
#include <unordered_set>

#include <intrin.h>
#include <windows.h>

namespace scvk::NativeShadowRegistry {
	namespace {
		constexpr uintptr_t kImageBase = 0x00400000;

		constexpr uintptr_t kAddShadowSlotVA = 0x00AB3A14;
		constexpr uintptr_t kAddShadowVA = 0x0073C0D0;
		constexpr uintptr_t kRemoveOverlaySlotVA = 0x00AB3A24;
		constexpr uintptr_t kRemoveOverlayVA = 0x007388C0;
		constexpr uintptr_t kDrawShadowsVA = 0x00736BF0;
		constexpr uintptr_t kDrawShadowsRejoinVA = 0x00736BF6;
		constexpr uintptr_t kDrawShadowsRoughVA = 0x00737870;
		constexpr uintptr_t kDrawShadowsRoughRejoinVA = 0x00737876;
		constexpr uintptr_t kMeshStashVA = 0x00491894;
		constexpr uintptr_t kMeshStashRejoinVA = 0x0049189F;
		// SC4 renderer; +0x24 returns the lighting manager (DrawShadows 0x00736C49).
		constexpr uintptr_t kRendererPointerVA = 0x00B43DD0;
		constexpr size_t kRendererLightingSlot = 0x24 / sizeof(void *);
		// Lighting manager vtable 0x00ABB530 +0x34, GetShadowDirection 0x007D6B50.
		constexpr size_t kLightingShadowDirectionSlot = 0x34 / sizeof(void *);
		// SetTimeOfDay (0x007D9DF0) writes max((lighting red - 0.8) / 0.2, 0)
		// here and sends it as the daylight transition parameter.
		constexpr ptrdiff_t kLightingDaylight = 0xBC;
		// GetShadowParams (0x007D6B70) only copies these globals out.
		constexpr uintptr_t kShadowStrengthVA = 0x00B0DC78;
		constexpr uintptr_t kShadowAlphaScaleVA = 0x00B4E330;
		constexpr uintptr_t kShadowColourVA = 0x00B4E34C;

		constexpr ptrdiff_t kManagerRecords = 0x98;
		constexpr size_t kRecordSize = 0xB4;
		constexpr ptrdiff_t kRecordFlags = 0x04;
		constexpr ptrdiff_t kRecordProjector = 0x1C;
		constexpr ptrdiff_t kRecordBinding = 0xAC;
		// RedisplayStaticOverlay (0x00737D40) redisplays a record through the
		// terrain at manager +0x88, passing manager +0x16 as updateView.
		constexpr ptrdiff_t kManagerTerrain = 0x88;
		constexpr ptrdiff_t kManagerUpdateView = 0x16;
		// cSTETerrain vtable 0x00AB3D18 (cISTETerrainMap first): +0x18 CellWidth
		// (float, 16), +0x1C CellCountX, +0x20 CellCountZ, +0x34 GetAltitude(x, z)
		// (after GetAltitude(index): MSVC reverses overloads), +0x110
		// RedisplayTerrain(updateTerrain, updateView, rect, flags) 0x00749850,
		// whose rect is {x1, z1, x2, z2} in cells.
		constexpr size_t kTerrainCellWidthSlot = 0x18 / sizeof(void *);
		constexpr size_t kTerrainCellCountXSlot = 0x1C / sizeof(void *);
		constexpr size_t kTerrainCellCountZSlot = 0x20 / sizeof(void *);
		constexpr size_t kTerrainAltitudeSlot = 0x34 / sizeof(void *);
		constexpr size_t kTerrainRedisplaySlot = 0x110 / sizeof(void *);
		constexpr uint32_t kShadowDrawFlags = 9;
		constexpr uint32_t kWrapFlag = 2;

		// CreateOccupantShadow's body stack pointer B. At the AddShadow call the
		// six arguments and the return address sit below it; at the stash site
		// the six arguments of the position extraction do.
		constexpr ptrdiff_t kReturnSlotToFrame = 0x1C;
		constexpr ptrdiff_t kStashToFrame = 0x18;
		constexpr ptrdiff_t kFramePositions = 0x24;
		constexpr ptrdiff_t kFrameTransform = 0x58;
		constexpr ptrdiff_t kFrameUVs = 0xA8;

		// Relaxed props - those without Is Ground Model (0x8A5E5DB8), which SC4
		// never shadows - are sorted by what their mesh shows. One whose lowest
		// point floats this far above the terrain is not a ground model at all
		// (the building zots are such props) and casts nothing, as in vanilla.
		constexpr float kFloatingPropHeight = 2.0f;
		// UVs the projector reproduces this closely are a prerendered view (a BAT
		// prop without the flag), which casts through the projector like any BAT
		// caster; anything else is a real unwrap and casts through its own UVs.
		constexpr double kPrerenderedResidual = 0.01;

		constexpr uint32_t kMaxVertices = 0xFFFF;
		constexpr uint32_t kMaxPrimitiveRuns = 4096;
		constexpr uint32_t kMaxIndices = 0x40000;
		constexpr size_t kMaxIdsPerPass = 1u << 20;
		constexpr size_t kMaxPatchLength = 11;

		struct PatchSite {
			uintptr_t virtualAddress = 0;
			size_t length = 0;
			std::array<uint8_t, kMaxPatchLength> expected{};
			std::array<uint8_t, kMaxPatchLength> replacement{};
			std::array<uint8_t, kMaxPatchLength> saved{};
			uint8_t *address = nullptr;
			bool installed = false;
		};

		// cS3DTransform as built at 0x004915FE-0x0049166A. Flag byte 1 bit 4
		// means the rotation applies, bit 2 the uniform scale; the matrix is
		// row-major (world = R * local), which the Phase 1 capture confirms.
		struct Transform {
			uint8_t flags[2];
			uint8_t padding[2];
			float rotation[9];
			float translation[3];
			float scale;
		};
		static_assert(sizeof(Transform) == 0x38, "cS3DTransform must stay 56 bytes");

		struct MeshHeader {
			void const *vertices;
			void const *indexHolder;
			void const *primitives;
		};

		struct PrimitiveRun {
			uint32_t primitive;
			uint32_t start;
			uint32_t count;
		};

		struct IdList {
			int32_t *begin;
			int32_t *end;
			int32_t *capacity;
		};

		using AddShadowFunction = int32_t(__thiscall *)(
			void *, void const *, int32_t, void const *, void const *, void const *, void const *);
		using RemoveOverlayFunction = void(__thiscall *)(void *, int32_t);
		using DrawShadowsFunction = void(__thiscall *)(void *, float const *, void *, IdList *);

		PatchSite gAddShadowSlot;
		PatchSite gRemoveOverlaySlot;
		PatchSite gDrawShadowsEntry;
		PatchSite gDrawShadowsRoughEntry;
		PatchSite gMeshStash;

		bool gInstalled = false;
		AddShadowFunction gOriginalAddShadow = nullptr;
		RemoveOverlayFunction gOriginalRemoveOverlay = nullptr;
		uintptr_t gDrawShadowsRejoin = 0;
		uintptr_t gDrawShadowsRoughRejoin = 0;
		uintptr_t gMeshStashRejoin = 0;
		uintptr_t gRendererPointer = 0;
		uintptr_t gShadowStrength = 0;
		uintptr_t gShadowAlphaScale = 0;
		uintptr_t gShadowColour = 0;

		// Written by the stash site and read by the AddShadow hook further down
		// the same CreateOccupantShadow iteration. Plain globals: the frame
		// pointer is compared on use, so a stale or foreign value only costs the
		// index recovery, never correctness.
		void const *gStashedMesh = nullptr;
		uint8_t const *gStashedFrame = nullptr;
		// Set by NativeShadowMasks' site D stub for a relaxed prop, consumed by
		// the AddShadow call that follows on the same thread.
		thread_local bool tExpectMeshCaster = false;

		std::mutex gMutex;
		std::unordered_map<uint32_t, std::shared_ptr<Caster const>> gCasters;
		void const *gManager = nullptr;
		Pass gPass;
		bool gPassValid = false;
		std::unordered_set<uint32_t> gPassIds;

		unsigned gRegistered = 0;
		unsigned gMeshCasters = 0;
		unsigned gMeshFallbacks = 0;
		unsigned gFloatingProps = 0;
		unsigned gPrerenderedProps = 0;
		unsigned gWithIndices = 0;
		unsigned gQuadOnly = 0;
		unsigned gRemoved = 0;
		unsigned gStaleRecords = 0;
		unsigned gPasses = 0;
		unsigned gProjectorChecks = 0;
		unsigned gDrawCalls = 0;
		unsigned gGathered = 0;
		unsigned gNotGathered = 0;

		// Why a DrawShadows call was left entirely to the native draw; each
		// reason is logged once.
		void NoteNotGathered(char const *reason, void const *manager) {
			++gNotGathered;
			static std::unordered_set<char const *> logged;
			if (logged.insert(reason).second) {
				Log(LogCategory::Initialization, "shadow registry: DrawShadows call left native (%s, manager %p, registry %p)",
				    reason, manager, gManager);
			}
		}

		bool DiagnosticsEnabled() {
			static bool const enabled = std::strstr(GetCommandLineA(), "-LiveShadowDiag") != nullptr;
			return enabled;
		}

		// ------------------------------------------------------------------
		// Guarded access to game memory
		// ------------------------------------------------------------------

#if defined(_MSC_VER)
		bool SafeCopy(void const *source, void *destination, size_t size) {
			__try {
				std::memcpy(destination, source, size);
				return true;
			} __except (EXCEPTION_EXECUTE_HANDLER) {
				return false;
			}
		}

		bool SafeShadowDirection(uintptr_t rendererPointer, float direction[3]) {
			__try {
				void *const renderer = *reinterpret_cast<void **>(rendererPointer);
				if (renderer == nullptr) return false;
				using GetLighting = void *(__thiscall *)(void *);
				void *const lighting =
					reinterpret_cast<GetLighting>((*reinterpret_cast<void ***>(renderer))[kRendererLightingSlot])(
						renderer);
				if (lighting == nullptr) return false;
				using GetDirection = float *(__thiscall *)(void *, float *);
				reinterpret_cast<GetDirection>(
					(*reinterpret_cast<void ***>(lighting))[kLightingShadowDirectionSlot])(lighting, direction);
				return true;
			} __except (EXCEPTION_EXECUTE_HANDLER) {
				return false;
			}
		}

		bool SafeShadowDaylight(uintptr_t rendererPointer, float &daylight) {
			__try {
				void *const renderer = *reinterpret_cast<void **>(rendererPointer);
				if (renderer == nullptr) return false;
				using GetLighting = void *(__thiscall *)(void *);
				void *const lighting =
					reinterpret_cast<GetLighting>((*reinterpret_cast<void ***>(renderer))[kRendererLightingSlot])(
						renderer);
				if (lighting == nullptr) return false;
				return SafeCopy(static_cast<uint8_t const *>(lighting) + kLightingDaylight,
				                &daylight, sizeof(daylight));
			} __except (EXCEPTION_EXECUTE_HANDLER) {
				return false;
			}
		}

		struct TerrainGrid {
			float cellWidth;
			uint32_t cellsX;
			uint32_t cellsZ;
		};

		bool SafeTerrainGrid(void *terrain, TerrainGrid &grid) {
			__try {
				void **const vtable = *reinterpret_cast<void ***>(terrain);
				grid.cellWidth = reinterpret_cast<float(__thiscall *)(void *)>(vtable[kTerrainCellWidthSlot])(terrain);
				grid.cellsX = reinterpret_cast<uint32_t(__thiscall *)(void *)>(vtable[kTerrainCellCountXSlot])(terrain);
				grid.cellsZ = reinterpret_cast<uint32_t(__thiscall *)(void *)>(vtable[kTerrainCellCountZSlot])(terrain);
				return true;
			} __except (EXCEPTION_EXECUTE_HANDLER) {
				return false;
			}
		}

		bool SafeRedisplayTerrain(void *terrain, bool updateView, int32_t const rect[4]) {
			__try {
				using Redisplay = void(__thiscall *)(void *, bool, bool, int32_t const *, uint32_t);
				reinterpret_cast<Redisplay>((*reinterpret_cast<void ***>(terrain))[kTerrainRedisplaySlot])(
					terrain, false, updateView, rect, 0);
				return true;
			} __except (EXCEPTION_EXECUTE_HANDLER) {
				return false;
			}
		}

		bool SafeTerrainAltitude(void *terrain, float x, float z, float &altitude) {
			__try {
				using GetAltitude = float(__thiscall *)(void *, float, float);
				altitude = reinterpret_cast<GetAltitude>((*reinterpret_cast<void ***>(terrain))[kTerrainAltitudeSlot])(
					terrain, x, z);
				return true;
			} __except (EXCEPTION_EXECUTE_HANDLER) {
				return false;
			}
		}
#else
		bool SafeCopy(void const *, void *, size_t) { return false; }
		bool SafeShadowDirection(uintptr_t, float *) { return false; }
		bool SafeShadowDaylight(uintptr_t, float &) { return false; }
		struct TerrainGrid {
			float cellWidth;
			uint32_t cellsX;
			uint32_t cellsZ;
		};
		bool SafeTerrainGrid(void *, TerrainGrid &) { return false; }
		bool SafeRedisplayTerrain(void *, bool, int32_t const *) { return false; }
		bool SafeTerrainAltitude(void *, float, float, float &) { return false; }
#endif

		template<typename T>
		bool Read(void const *source, T &value) {
			return SafeCopy(source, &value, sizeof(T));
		}

		// The terrain and view-update flag of the overlay manager the registry
		// belongs to, i.e. of the loaded city.
		bool CityTerrain(void *&terrain, bool &updateView) {
			void const *manager = nullptr;
			{
				std::lock_guard<std::mutex> lock(gMutex);
				manager = gManager;
			}
			uint8_t update = 0;
			if (manager == nullptr ||
			    !Read(static_cast<uint8_t const *>(manager) + kManagerTerrain, terrain) || terrain == nullptr ||
			    !Read(static_cast<uint8_t const *>(manager) + kManagerUpdateView, update))
				return false;
			updateView = update != 0;
			return true;
		}

		uint8_t const *RecordAddress(void const *manager, int32_t id) {
			uint8_t const *records = nullptr;
			if (!Read(static_cast<uint8_t const *>(manager) + kManagerRecords, records) || records == nullptr)
				return nullptr;
			return records + static_cast<size_t>(id) * kRecordSize;
		}

		// ------------------------------------------------------------------
		// Capture
		// ------------------------------------------------------------------

		void ToWorld(Transform const &transform, float const local[3], float world[3]) {
			float rotated[3]{local[0], local[1], local[2]};
			if ((transform.flags[1] & 4) != 0) {
				for (unsigned row = 0; row < 3; ++row) {
					rotated[row] = transform.rotation[row * 3 + 0] * local[0] +
					               transform.rotation[row * 3 + 1] * local[1] +
					               transform.rotation[row * 3 + 2] * local[2];
				}
			}
			float const scale = (transform.flags[1] & 2) != 0 ? transform.scale : 1.0f;
			for (unsigned axis = 0; axis < 3; ++axis)
				world[axis] = rotated[axis] * scale + transform.translation[axis];
		}

		// Reads the mesh the stash site recorded, provided it belongs to the
		// CreateOccupantShadow frame this AddShadow was called from. Every value
		// is checked against what AddShadow itself received.
		bool RecoverIndices(uint8_t const *returnSlot, uint32_t count, void const *positions, void const *uvs,
		                    void const *transform, std::vector<uint32_t> &triangles) {
			uint8_t const *const frame = returnSlot + kReturnSlotToFrame;
			if (gStashedFrame == nullptr || gStashedFrame + kStashToFrame != frame) return false;
			void const *framePositions = nullptr;
			void const *frameUVs = nullptr;
			if (!Read(frame + kFramePositions, framePositions) || !Read(frame + kFrameUVs, frameUVs) ||
			    framePositions != positions || frameUVs != uvs || frame + kFrameTransform != transform)
				return false;

			MeshHeader mesh{};
			uint16_t vertexCount = 0;
			if (!Read(gStashedMesh, mesh) || mesh.vertices == nullptr || mesh.primitives == nullptr ||
			    !Read(static_cast<uint8_t const *>(mesh.vertices) + 6, vertexCount) || vertexCount != count)
				return false;
			uint16_t const *indexBase = nullptr;
			if (mesh.indexHolder != nullptr && (!Read(mesh.indexHolder, indexBase) || indexBase == nullptr))
				return false;
			PrimitiveRun const *runs = nullptr;
			PrimitiveRun const *runsEnd = nullptr;
			if (!Read(static_cast<uint8_t const *>(mesh.primitives) + 4, runs) ||
			    !Read(static_cast<uint8_t const *>(mesh.primitives) + 8, runsEnd) || runsEnd < runs ||
			    static_cast<size_t>(runsEnd - runs) > kMaxPrimitiveRuns)
				return false;

			std::vector<uint32_t> source;
			std::vector<uint16_t> packed;
			for (PrimitiveRun const *cursor = runs; cursor != runsEnd; ++cursor) {
				PrimitiveRun run{};
				if (!Read(cursor, run) || run.count > kMaxIndices || run.start > kMaxIndices) return false;
				source.resize(run.count);
				if (indexBase != nullptr) {
					packed.resize(run.count);
					if (run.count != 0 &&
					    !SafeCopy(indexBase + run.start, packed.data(), run.count * sizeof(uint16_t)))
						return false;
					std::copy(packed.begin(), packed.end(), source.begin());
				} else {
					for (uint32_t index = 0; index < run.count; ++index) source[index] = run.start + index;
				}
				if (std::any_of(source.begin(), source.end(), [count](uint32_t index) { return index >= count; }))
					return false;
				AppendTriangles(run.primitive, source.data(), run.count, triangles);
				if (triangles.size() > kMaxIndices) return false;
			}
			return !triangles.empty();
		}

		// RMS distance between the mesh's UVs and what the projector gives for
		// the same world positions.
		double ProjectorResidual(Caster const &caster, float const *uvs, uint32_t count) {
			double squared = 0.0;
			for (uint32_t vertex = 0; vertex < count; ++vertex) {
				float const *const world = caster.positions.data() + vertex * 3;
				float const *const m = caster.projector;
				double const du = m[0] * world[0] + m[4] * world[1] + m[8] * world[2] + m[12] - uvs[vertex * 2 + 0];
				double const dv = m[1] * world[0] + m[5] * world[1] + m[9] * world[2] + m[13] - uvs[vertex * 2 + 1];
				squared += du * du + dv * dv;
			}
			return count == 0 ? 0.0 : std::sqrt(squared / count);
		}

		// -LiveShadowDiag checks 2 and 3 of the design: the projector reproduces
		// the mesh's UVs from world positions, and the transformed mesh sits
		// where AddShadow's bounding box says the model is.
		void LogProjectorCheck(Caster const &caster, float const *uvs, uint32_t count, void const *box) {
			if (!DiagnosticsEnabled() || gProjectorChecks >= 8) return;
			++gProjectorChecks;
			double squared = 0.0;
			double worst = 0.0;
			for (uint32_t vertex = 0; vertex < count; ++vertex) {
				float const *const world = caster.positions.data() + vertex * 3;
				float const *const m = caster.projector;
				float const u = m[0] * world[0] + m[4] * world[1] + m[8] * world[2] + m[12];
				float const v = m[1] * world[0] + m[5] * world[1] + m[9] * world[2] + m[13];
				double const du = u - uvs[vertex * 2 + 0];
				double const dv = v - uvs[vertex * 2 + 1];
				squared += du * du + dv * dv;
				worst = (std::max)(worst, std::sqrt(du * du + dv * dv));
			}
			float boxValues[6]{};
			bool const haveBox = SafeCopy(box, boxValues, sizeof(boxValues));
			Log(LogCategory::Initialization,
			    "shadow registry check: id=%u verts=%u indices=%u uv=[%.3g,%.3g..%.3g,%.3g] residual rms=%.4g max=%.4g "
			    "world=[%.1f,%.1f,%.1f..%.1f,%.1f,%.1f] box=%s[%.1f,%.1f,%.1f..%.1f,%.1f,%.1f] base=%.2f",
			    caster.id, count, static_cast<unsigned>(caster.indices.size()), caster.uvBounds[0],
			    caster.uvBounds[1], caster.uvBounds[2], caster.uvBounds[3], std::sqrt(squared / count), worst,
			    caster.low[0], caster.low[1], caster.low[2], caster.high[0], caster.high[1], caster.high[2],
			    haveBox ? "" : "?", boxValues[0], boxValues[1], boxValues[2], boxValues[3], boxValues[4],
			    boxValues[5], caster.baseHeight);
		}

		void Register(void *manager, int32_t id, void const *binding, int32_t count, void const *positions,
		              void const *uvs, void const *box, void const *transform, uint8_t const *returnSlot,
		              bool meshCaster) {
			auto erase = [&] {
				std::lock_guard<std::mutex> lock(gMutex);
				gCasters.erase(static_cast<uint32_t>(id));
			};
			if (binding == nullptr || positions == nullptr || uvs == nullptr || transform == nullptr || count < 3 ||
			    static_cast<uint32_t>(count) > kMaxVertices) {
				erase();
				return;
			}
			uint32_t const vertexCount = static_cast<uint32_t>(count);
			auto caster = std::make_shared<Caster>();
			caster->id = static_cast<uint32_t>(id);
			caster->manager = manager;
			caster->binding = binding;

			uint8_t const *const record = RecordAddress(manager, id);
			void const *recordBinding = nullptr;
			Transform xf{};
			std::vector<float> local(static_cast<size_t>(vertexCount) * 3);
			std::vector<float> uvValues(static_cast<size_t>(vertexCount) * 2);
			if (record == nullptr || !SafeCopy(record + kRecordProjector, caster->projector, sizeof(caster->projector)) ||
			    !Read(record + kRecordBinding, recordBinding) || recordBinding != binding || !Read(transform, xf) ||
			    !SafeCopy(positions, local.data(), local.size() * sizeof(float)) ||
			    !SafeCopy(uvs, uvValues.data(), uvValues.size() * sizeof(float))) {
				erase();
				return;
			}

			caster->positions.resize(local.size());
			for (unsigned axis = 0; axis < 3; ++axis) {
				caster->low[axis] = FLT_MAX;
				caster->high[axis] = -FLT_MAX;
			}
			caster->uvBounds[0] = caster->uvBounds[1] = FLT_MAX;
			caster->uvBounds[2] = caster->uvBounds[3] = -FLT_MAX;
			for (uint32_t vertex = 0; vertex < vertexCount; ++vertex) {
				float *const world = caster->positions.data() + vertex * 3;
				ToWorld(xf, local.data() + vertex * 3, world);
				for (unsigned axis = 0; axis < 3; ++axis) {
					caster->low[axis] = (std::min)(caster->low[axis], world[axis]);
					caster->high[axis] = (std::max)(caster->high[axis], world[axis]);
				}
				for (unsigned axis = 0; axis < 2; ++axis) {
					caster->uvBounds[axis] = (std::min)(caster->uvBounds[axis], uvValues[vertex * 2 + axis]);
					caster->uvBounds[axis + 2] = (std::max)(caster->uvBounds[axis + 2], uvValues[vertex * 2 + axis]);
				}
			}
			caster->baseHeight = xf.translation[1];
			if (!std::isfinite(caster->low[0]) || !std::isfinite(caster->high[0]) ||
			    !std::isfinite(caster->uvBounds[0]) || !std::isfinite(caster->uvBounds[2])) {
				erase();
				return;
			}

			bool const indexed = RecoverIndices(returnSlot, vertexCount, positions, uvs, transform, caster->indices);
			if (!indexed) caster->indices.clear();
			if (meshCaster) {
				void *terrain = nullptr;
				float ground = caster->low[1];
				if (Read(static_cast<uint8_t const *>(manager) + kManagerTerrain, terrain) && terrain != nullptr) {
					float altitude = 0.0f;
					if (SafeTerrainAltitude(terrain, (caster->low[0] + caster->high[0]) * 0.5f,
					                        (caster->low[2] + caster->high[2]) * 0.5f, altitude) &&
					    std::isfinite(altitude))
						ground = altitude;
				}
				double const residual = ProjectorResidual(*caster, uvValues.data(), vertexCount);
				bool const floating = caster->low[1] - ground > kFloatingPropHeight;
				if (DiagnosticsEnabled() && gMeshCasters + gFloatingProps + gPrerenderedProps < 24) {
					Log(LogCategory::Initialization,
					    "shadow registry: relaxed prop id=%d verts=%u lowest %.2f above terrain, residual %.4f -> %s",
					    id, vertexCount, caster->low[1] - ground, residual,
					    floating ? "floating, no shadow" : residual < kPrerenderedResidual ? "projector"
						    : indexed ? "own UVs" : "per-draw");
				}
				if (floating) {
					caster->suppressOnly = true;
					++gFloatingProps;
				} else if (residual < kPrerenderedResidual) {
					++gPrerenderedProps;
				} else if (indexed) {
					caster->meshUVs = true;
					caster->uvs = std::move(uvValues);
					++gMeshCasters;
				} else {
					// Without triangles the UVs cannot be drawn. The prop casts
					// through the per-draw path instead, and its record - whose
					// projector would stamp a slab - is only kept out of DrawShadows.
					caster->suppressOnly = true;
					NativeShadowMasks::RegisterLivePropMesh(vertexCount, positions, uvs);
					++gMeshFallbacks;
				}
			}
			if (!caster->meshUVs) LogProjectorCheck(*caster, uvValues.data(), vertexCount, box);

			std::lock_guard<std::mutex> lock(gMutex);
			if (gManager != manager) {
				// A new overlay manager is a new city: every id belongs to it now.
				if (gManager != nullptr) {
					Log(LogCategory::Initialization, "shadow registry: overlay manager changed, %u casters dropped",
					    static_cast<unsigned>(gCasters.size()));
				}
				gCasters.clear();
				gPassIds.clear();
				gPass.casters.clear();
				gManager = manager;
			}
			gCasters[caster->id] = std::move(caster);
			++gRegistered;
			if (indexed) ++gWithIndices;
			else ++gQuadOnly;
			static bool loggedFirst = false;
			if (!loggedFirst) {
				loggedFirst = true;
				Log(LogCategory::Initialization, "shadow registry: first caster registered (id %d, %d vertices, %s)",
				    id, count, indexed ? "indexed" : "ground quad only");
			}
		}

		// Called by the DrawShadows call-site hooks with the pass's overlay ids.
		// Registry-owned records are moved into the pass; every other id, and
		// any record that no longer matches what was registered, is kept for the
		// native draw. False leaves the caller to draw the original list.
		bool Gather(void *manager, float const *eyeToWorld, IdList const *ids, std::vector<int32_t> &kept) {
			kept.clear();
			IdList list{};
			if (!Read(ids, list) || list.begin == nullptr || list.end <= list.begin) {
				NoteNotGathered("empty or unreadable id list", manager);
				return false;
			}
			size_t const count = static_cast<size_t>(list.end - list.begin);
			if (count > kMaxIdsPerPass) {
				NoteNotGathered("id list too long", manager);
				return false;
			}
			kept.resize(count);
			if (!SafeCopy(list.begin, kept.data(), count * sizeof(int32_t))) {
				NoteNotGathered("id list unreadable", manager);
				return false;
			}

			float matrix[16]{};
			float sun[3]{};
			float colour[3]{};
			float strength = 0.0f;
			float alphaScale = 0.0f;
			bool const haveMatrix = eyeToWorld != nullptr && SafeCopy(eyeToWorld, matrix, sizeof(matrix));
			bool const haveSun = SafeShadowDirection(gRendererPointer, sun);
			Read(reinterpret_cast<void const *>(gShadowStrength), strength);
			Read(reinterpret_cast<void const *>(gShadowAlphaScale), alphaScale);
			SafeCopy(reinterpret_cast<void const *>(gShadowColour), colour, sizeof(colour));
			if (!haveMatrix) {
				NoteNotGathered("eye-to-world matrix unreadable", manager);
				return false;
			}

			std::lock_guard<std::mutex> lock(gMutex);
			if (manager != gManager) {
				NoteNotGathered("not the registry's overlay manager", manager);
				return false;
			}
			if (gCasters.empty()) {
				NoteNotGathered("registry empty", manager);
				return false;
			}
			++gGathered;
			size_t write = 0;
			for (size_t read = 0; read < kept.size(); ++read) {
				int32_t const id = kept[read];
				auto const found = id >= 0 ? gCasters.find(static_cast<uint32_t>(id)) : gCasters.end();
				if (found == gCasters.end()) {
					kept[write++] = id;
					continue;
				}
				Caster const &caster = *found->second;
				uint8_t const *const record = RecordAddress(manager, id);
				int32_t inUse = 0;
				uint32_t flags = 0;
				void const *binding = nullptr;
				void const *textureObject = nullptr;
				uint32_t texture = 0;
				if (record == nullptr || !Read(record, inUse) || !Read(record + kRecordFlags, flags) ||
				    !Read(record + kRecordBinding, binding) || inUse != -1 || binding != caster.binding) {
					// The slot was freed or reused by something that never went
					// through AddShadow, e.g. a decal. It is not ours any more.
					++gStaleRecords;
					gCasters.erase(found);
					kept[write++] = id;
					continue;
				}
				if ((flags & kShadowDrawFlags) != kShadowDrawFlags) continue; // disabled: native skips it too
				if (caster.suppressOnly) continue;
				if (!Read(binding, textureObject) || textureObject == nullptr || !Read(textureObject, texture)) {
					kept[write++] = id;
					continue;
				}
				if (gPassIds.insert(caster.id).second) {
					gPass.casters.push_back(PassCaster{found->second, texture, (flags & kWrapFlag) != 0});
				}
			}
			kept.resize(write);
			std::memcpy(gPass.eyeToWorld, matrix, sizeof(matrix));
			if (haveSun) std::memcpy(gPass.sunDirection, sun, sizeof(sun));
			gPass.sunValid = haveSun;
			std::memcpy(gPass.colour, colour, sizeof(colour));
			gPass.strength = strength;
			gPass.alphaScale = alphaScale;
			gPassValid = true;
			return true;
		}

	} // namespace

	bool AppendTriangles(uint32_t primitive, uint32_t const *indices, uint32_t count,
	                     std::vector<uint32_t> &triangles) {
		switch (primitive) {
			case 0: // triangles
				for (uint32_t i = 0; i + 2 < count; i += 3)
					triangles.insert(triangles.end(), {indices[i], indices[i + 1], indices[i + 2]});
				return count >= 3;
			case 1: // triangle strip; the shadow pass does not cull, so winding is irrelevant
				for (uint32_t i = 0; i + 2 < count; ++i)
					triangles.insert(triangles.end(), {indices[i], indices[i + 1], indices[i + 2]});
				return count >= 3;
			case 2: // triangle fan
				for (uint32_t i = 1; i + 1 < count; ++i)
					triangles.insert(triangles.end(), {indices[0], indices[i], indices[i + 1]});
				return count >= 3;
			case 6: // quads
				for (uint32_t i = 0; i + 3 < count; i += 4)
					triangles.insert(triangles.end(), {
						                 indices[i], indices[i + 1], indices[i + 2],
						                 indices[i], indices[i + 2], indices[i + 3]
					                 });
				return count >= 4;
			case 7: // quad strip
				for (uint32_t i = 0; i + 3 < count; i += 2)
					triangles.insert(triangles.end(), {
						                 indices[i], indices[i + 1], indices[i + 2],
						                 indices[i + 2], indices[i + 1], indices[i + 3]
					                 });
				return count >= 4;
			default:
				return false;
		}
	}

	// ----------------------------------------------------------------------
	// Hook bodies
	// ----------------------------------------------------------------------

	namespace {
		int32_t __fastcall AddShadowHook(void *manager, void *, void const *binding, int32_t count,
		                                 void const *positions, void const *uvs, void const *box,
		                                 void const *transform) {
			bool const meshCaster = tExpectMeshCaster;
			tExpectMeshCaster = false;
			int32_t const id = gOriginalAddShadow(manager, binding, count, positions, uvs, box, transform);
			if (id >= 0) {
				try {
					Register(manager, id, binding, count, positions, uvs, box, transform,
					         static_cast<uint8_t const *>(_AddressOfReturnAddress()), meshCaster);
				} catch (std::bad_alloc const &) {
					std::lock_guard<std::mutex> lock(gMutex);
					gCasters.erase(static_cast<uint32_t>(id));
				}
			} else if (meshCaster && count > 0) {
				// CalcShadowProjection rejected the fit, so there is no record to
				// keep: the prop casts through the per-draw path as before.
				NativeShadowMasks::RegisterLivePropMesh(static_cast<uint32_t>(count), positions, uvs);
				++gMeshFallbacks;
			}
			return id;
		}

		void __fastcall RemoveOverlayHook(void *manager, void *, int32_t id) {
			gOriginalRemoveOverlay(manager, id);
			// Negative ids are handles into the manager's +0xAC table; AddShadow
			// never returns one, and a group's members come back through here.
			if (id < 0) return;
			std::lock_guard<std::mutex> lock(gMutex);
			if (manager == gManager && gCasters.erase(static_cast<uint32_t>(id)) != 0) ++gRemoved;
		}

		void DrawFiltered(DrawShadowsFunction original, void *manager, float const *eyeToWorld, void *drawContext,
		                  IdList *ids) {
			// Only the render thread draws overlays, so one scratch list is enough.
			static std::vector<int32_t> kept;
			++gDrawCalls;
			bool gathered = false;
			try {
				gathered = Gather(manager, eyeToWorld, ids, kept);
			} catch (std::bad_alloc const &) {
			}
			if (!gathered) {
				original(manager, eyeToWorld, drawContext, ids);
				return;
			}
			// DrawShadows reads only begin and end, and returns at once when they
			// are equal.
			IdList filtered{};
			if (!kept.empty()) filtered = IdList{kept.data(), kept.data() + kept.size(), kept.data() + kept.size()};
			original(manager, eyeToWorld, drawContext, &filtered);
		}

#if defined(_MSC_VER) && defined(_M_IX86)
		// Entered by a call carrying the original arguments, so the replayed
		// SUB ESP builds the original frame and the body's RET 0xC returns here.
		__declspec(naked) void DrawShadowsOriginal() {
			__asm {
				sub  esp, 0x1c4
				jmp  dword ptr [gDrawShadowsRejoin]
			}
		}

		__declspec(naked) void DrawShadowsRoughOriginal() {
			__asm {
				sub  esp, 0x9c
				jmp  dword ptr [gDrawShadowsRoughRejoin]
			}
		}

		void __fastcall DrawShadowsHook(void *manager, void *, float const *eyeToWorld, void *drawContext,
		                                IdList *ids) {
			DrawFiltered(reinterpret_cast<DrawShadowsFunction>(&DrawShadowsOriginal), manager, eyeToWorld,
			             drawContext, ids);
		}

		void __fastcall DrawShadowsRoughHook(void *manager, void *, float const *eyeToWorld, void *drawContext,
		                                     IdList *ids) {
			DrawFiltered(reinterpret_cast<DrawShadowsFunction>(&DrawShadowsRoughOriginal), manager, eyeToWorld,
			             drawContext, ids);
		}

		// Replays the three displaced instructions; ESP is the original's, so the
		// ESP-relative load is unchanged.
		__declspec(naked) void MeshStashStub() {
			__asm {
				mov  dword ptr [gStashedMesh], edi
				mov  dword ptr [gStashedFrame], esp
				mov  eax, dword ptr [edi]
				mov  ecx, dword ptr [eax]
				mov  ebx, dword ptr [esp + 0xc0]
				jmp  dword ptr [gMeshStashRejoin]
			}
		}
#endif

		// ------------------------------------------------------------------
		// Patch plumbing
		// ------------------------------------------------------------------

		void ConfigureSite(PatchSite &site, uintptr_t virtualAddress, std::initializer_list<uint8_t> expected) {
			site = PatchSite{};
			site.virtualAddress = virtualAddress;
			site.length = expected.size();
			std::copy(expected.begin(), expected.end(), site.expected.begin());
		}

		void ConfigureBranchSite(PatchSite &site, uintptr_t virtualAddress, std::initializer_list<uint8_t> expected,
		                         uint8_t opcode, uint8_t *target, void const *destination) {
			ConfigureSite(site, virtualAddress, expected);
			site.replacement.fill(0x90);
			site.replacement[0] = opcode;
			int32_t const displacement = static_cast<int32_t>(reinterpret_cast<uintptr_t>(destination) -
			                                                 (reinterpret_cast<uintptr_t>(target) + 5));
			std::memcpy(site.replacement.data() + 1, &displacement, sizeof(displacement));
		}

		void ConfigureSlotSite(PatchSite &site, uintptr_t virtualAddress, uintptr_t expected, void const *hook) {
			site = PatchSite{};
			site.virtualAddress = virtualAddress;
			site.length = sizeof(uint32_t);
			uint32_t const value = static_cast<uint32_t>(expected);
			uint32_t const replacement = static_cast<uint32_t>(reinterpret_cast<uintptr_t>(hook));
			std::memcpy(site.expected.data(), &value, sizeof(value));
			std::memcpy(site.replacement.data(), &replacement, sizeof(replacement));
		}

		bool IsSupportedGameVersion() {
			wchar_t path[MAX_PATH]{};
			if (GetModuleFileNameW(nullptr, path, MAX_PATH) == 0) return false;
			DWORD ignored = 0;
			DWORD const size = GetFileVersionInfoSizeW(path, &ignored);
			if (size == 0) return false;
			std::vector<uint8_t> data(size);
			if (!GetFileVersionInfoW(path, 0, size, data.data())) return false;
			VS_FIXEDFILEINFO *version = nullptr;
			UINT versionSize = 0;
			if (!VerQueryValueW(data.data(), L"\\", reinterpret_cast<void **>(&version), &versionSize) ||
			    version == nullptr || versionSize < sizeof(VS_FIXEDFILEINFO))
				return false;
			return HIWORD(version->dwFileVersionMS) == 1 && LOWORD(version->dwFileVersionMS) == 1 &&
			       HIWORD(version->dwFileVersionLS) == 641;
		}

		bool Requested() {
			// scvk: from the settings, where replace is the default
			// -ShadowRegistry:off keeps the game's own prop shadows, for comparing pictures
			return GetSettings().nativeShadowMode == NativeShadowMode::Replace && std::strstr(GetCommandLineA(), "-ShadowRegistry:off") == nullptr;
		}

		bool BytesMatch(PatchSite const &site) {
			return site.address != nullptr && std::memcmp(site.address, site.expected.data(), site.length) == 0;
		}

		bool WriteSite(PatchSite &site) {
			DWORD oldProtection = 0;
			if (!VirtualProtect(site.address, site.length, PAGE_EXECUTE_READWRITE, &oldProtection)) return false;
			std::memcpy(site.saved.data(), site.address, site.length);
			std::memcpy(site.address, site.replacement.data(), site.length);
			DWORD ignored = 0;
			VirtualProtect(site.address, site.length, oldProtection, &ignored);
			FlushInstructionCache(GetCurrentProcess(), site.address, site.length);
			site.installed = true;
			return true;
		}

		void RestoreSite(PatchSite &site) {
			if (!site.installed || site.address == nullptr) return;
			if (std::memcmp(site.address, site.replacement.data(), site.length) != 0) {
				Log(LogCategory::Initialization, "shadow registry: not restoring modified site 0x%08lX",
				    static_cast<unsigned long>(site.virtualAddress));
				site.installed = false;
				return;
			}
			DWORD oldProtection = 0;
			if (VirtualProtect(site.address, site.length, PAGE_EXECUTE_READWRITE, &oldProtection)) {
				std::memcpy(site.address, site.saved.data(), site.length);
				DWORD ignored = 0;
				VirtualProtect(site.address, site.length, oldProtection, &ignored);
				FlushInstructionCache(GetCurrentProcess(), site.address, site.length);
			}
			site.installed = false;
		}

	} // namespace

	bool Install() {
#if defined(_MSC_VER) && defined(_M_IX86)
		if (gInstalled) return true;
		if (!Requested()) return false;
		if (!IsSupportedGameVersion()) {
			Log(LogCategory::Initialization, "shadow registry: requires SimCity 4 1.1.641, skipping");
			return false;
		}
		uint8_t *const module = reinterpret_cast<uint8_t *>(GetModuleHandleW(nullptr));
		if (module == nullptr) return false;
		auto const resolve = [module](uintptr_t virtualAddress) {
			return module + (virtualAddress - kImageBase);
		};

		gOriginalAddShadow = reinterpret_cast<AddShadowFunction>(resolve(kAddShadowVA));
		gOriginalRemoveOverlay = reinterpret_cast<RemoveOverlayFunction>(resolve(kRemoveOverlayVA));
		gDrawShadowsRejoin = reinterpret_cast<uintptr_t>(resolve(kDrawShadowsRejoinVA));
		gDrawShadowsRoughRejoin = reinterpret_cast<uintptr_t>(resolve(kDrawShadowsRoughRejoinVA));
		gMeshStashRejoin = reinterpret_cast<uintptr_t>(resolve(kMeshStashRejoinVA));
		gRendererPointer = reinterpret_cast<uintptr_t>(resolve(kRendererPointerVA));
		gShadowStrength = reinterpret_cast<uintptr_t>(resolve(kShadowStrengthVA));
		gShadowAlphaScale = reinterpret_cast<uintptr_t>(resolve(kShadowAlphaScaleVA));
		gShadowColour = reinterpret_cast<uintptr_t>(resolve(kShadowColourVA));

		ConfigureSlotSite(gAddShadowSlot, kAddShadowSlotVA, reinterpret_cast<uintptr_t>(resolve(kAddShadowVA)),
		                  reinterpret_cast<void const *>(&AddShadowHook));
		ConfigureSlotSite(gRemoveOverlaySlot, kRemoveOverlaySlotVA,
		                  reinterpret_cast<uintptr_t>(resolve(kRemoveOverlayVA)),
		                  reinterpret_cast<void const *>(&RemoveOverlayHook));
		ConfigureBranchSite(gDrawShadowsEntry, kDrawShadowsVA, {0x81, 0xEC, 0xC4, 0x01, 0x00, 0x00}, 0xE9,
		                    resolve(kDrawShadowsVA), reinterpret_cast<void const *>(&DrawShadowsHook));
		ConfigureBranchSite(gDrawShadowsRoughEntry, kDrawShadowsRoughVA, {0x81, 0xEC, 0x9C, 0x00, 0x00, 0x00}, 0xE9,
		                    resolve(kDrawShadowsRoughVA), reinterpret_cast<void const *>(&DrawShadowsRoughHook));
		ConfigureBranchSite(gMeshStash, kMeshStashVA,
		                    {0x8B, 0x07, 0x8B, 0x08, 0x8B, 0x9C, 0x24, 0xC0, 0x00, 0x00, 0x00}, 0xE9,
		                    resolve(kMeshStashVA), reinterpret_cast<void const *>(&MeshStashStub));
		PatchSite *const sites[]{
			&gMeshStash, &gRemoveOverlaySlot, &gDrawShadowsEntry, &gDrawShadowsRoughEntry, &gAddShadowSlot
		};
		for (PatchSite *site: sites) site->address = resolve(site->virtualAddress);
		for (PatchSite const *site: sites) {
			if (!BytesMatch(*site)) {
				Log(LogCategory::Initialization, "shadow registry: guard failed at 0x%08lX, skipping",
				    static_cast<unsigned long>(site->virtualAddress));
				return false;
			}
		}
		{
			std::lock_guard<std::mutex> lock(gMutex);
			gCasters.clear();
			gManager = nullptr;
			gPass = Pass{};
			gPassValid = false;
			gPassIds.clear();
		}
		// AddShadow goes last: nothing is registered before the draw filter and
		// the removal hook that keep the registry honest are in place.
		for (PatchSite *site: sites) {
			if (!WriteSite(*site)) {
				Log(LogCategory::Initialization, "shadow registry: failed to write 0x%08lX, restoring",
				    static_cast<unsigned long>(site->virtualAddress));
				for (PatchSite *written: sites) RestoreSite(*written);
				return false;
			}
		}
		gInstalled = true;
		Log(LogCategory::Initialization,
		    "shadow registry installed: AddShadow records render through the live shadow map");
		return true;
#else
		return false;
#endif
	}

	void Uninstall() {
		if (!gInstalled) return;
		RestoreSite(gAddShadowSlot);
		RestoreSite(gDrawShadowsRoughEntry);
		RestoreSite(gDrawShadowsEntry);
		RestoreSite(gRemoveOverlaySlot);
		RestoreSite(gMeshStash);
		gInstalled = false;
		size_t remaining = 0;
		{
			std::lock_guard<std::mutex> lock(gMutex);
			remaining = gCasters.size();
			gCasters.clear();
			gManager = nullptr;
			gPass = Pass{};
			gPassValid = false;
			gPassIds.clear();
		}
		Log(LogCategory::Initialization,
		    "shadow registry uninstalled: relaxed props: %u True3D kept with their UVs, %u prerendered through the "
		    "projector, %u floating left unshadowed, %u left to per-draw capture",
		    gMeshCasters, gPrerenderedProps, gFloatingProps, gMeshFallbacks);
		Log(LogCategory::Initialization,
		    "shadow registry uninstalled: %u registered (%u indexed, %u ground quad only), %u removed, "
		    "%u stale, %u passes, %u live at exit; DrawShadows calls %u (%u gathered, %u left native)",
		    gRegistered, gWithIndices, gQuadOnly, gRemoved, gStaleRecords, gPasses,
		    static_cast<unsigned>(remaining), gDrawCalls, gGathered, gNotGathered);
	}

	bool Enabled() {
		return gInstalled;
	}

	void ExpectMeshCaster() {
		tExpectMeshCaster = gInstalled;
	}

	bool RedisplayWorldRect(float minX, float minZ, float maxX, float maxZ) {
		void *terrain = nullptr;
		bool updateView = false;
		return gInstalled && CityTerrain(terrain, updateView) &&
		       RedisplayTerrainRect(terrain, updateView, minX, minZ, maxX, maxZ);
	}

	bool OverlayTerrain(void *&terrain, bool &updateView) {
		return gInstalled && CityTerrain(terrain, updateView);
	}

	bool RedisplayTerrainRect(void *terrain, bool updateView, float minX, float minZ, float maxX, float maxZ) {
		TerrainGrid grid{};
		if (!gInstalled || terrain == nullptr || !(minX <= maxX) || !(minZ <= maxZ) ||
		    !SafeTerrainGrid(terrain, grid) || !(grid.cellWidth > 0.0f) || grid.cellsX == 0 || grid.cellsZ == 0)
			return false;
		// A cell of margin on every side, clamped to the city: RedisplayTerrain
		// reads the altitudes under the rectangle.
		auto cell = [&](float value, uint32_t count, float round) {
			double const index = std::floor(value / grid.cellWidth + round);
			return static_cast<int32_t>((std::min)((std::max)(index, 0.0), static_cast<double>(count)));
		};
		int32_t const rect[4]{
			cell(minX, grid.cellsX, -1.0f), cell(minZ, grid.cellsZ, -1.0f),
			cell(maxX, grid.cellsX, 2.0f), cell(maxZ, grid.cellsZ, 2.0f)
		};
		if (rect[2] <= rect[0] || rect[3] <= rect[1]) return false;
		return SafeRedisplayTerrain(terrain, updateView, rect);
	}

	bool TerrainAltitude(float x, float z, float &altitude) {
		void *terrain = nullptr;
		bool updateView = false;
		return gInstalled && CityTerrain(terrain, updateView) && SafeTerrainAltitude(terrain, x, z, altitude) &&
		       std::isfinite(altitude);
	}

	bool SunDirection(float direction[3]) {
		float sun[3]{};
		if (!gInstalled || !SafeShadowDirection(gRendererPointer, sun) || !std::isfinite(sun[0]) ||
		    !std::isfinite(sun[1]) || !std::isfinite(sun[2]))
			return false;
		std::memcpy(direction, sun, sizeof(sun));
		return true;
	}

	bool ShadowParams(float colour[3], float &strength) {
		float tone[3]{};
		float value = 0.0f;
		if (!gInstalled || !SafeCopy(reinterpret_cast<void const *>(gShadowColour), tone, sizeof(tone)) ||
		    !Read(reinterpret_cast<void const *>(gShadowStrength), value))
			return false;
		std::memcpy(colour, tone, sizeof(tone));
		strength = value;
		return true;
	}

	bool ShadowDaylight(float &daylight) {
		// The replacement hooks are optional, but the live per-draw caster path
		// needs the same transition. Resolve the supported game's renderer here.
		static bool const supported = IsSupportedGameVersion();
		if (!supported) return false;
		auto const *module = reinterpret_cast<uint8_t const *>(GetModuleHandleW(nullptr));
		if (module == nullptr) return false;
		float value = 0.0f;
		if (!SafeShadowDaylight(reinterpret_cast<uintptr_t>(module + (kRendererPointerVA - kImageBase)), value) ||
		    !std::isfinite(value)) return false;
		daylight = (std::clamp)(value, 0.0f, 1.0f);
		return true;
	}

	bool TakePass(Pass &pass) {
		std::lock_guard<std::mutex> lock(gMutex);
		if (!gPassValid) return false;
		pass = std::move(gPass);
		gPass = Pass{};
		gPassValid = false;
		gPassIds.clear();
		++gPasses;
		return true;
	}
} // namespace scvk::NativeShadowRegistry
