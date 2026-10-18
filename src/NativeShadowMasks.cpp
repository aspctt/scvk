/*
 *  SCD3D11 - a free Direct3D 11 driver for SimCity 4's SimGL interface
 *  Copyright (C) 2026
 *
 *  This library is free software; you can redistribute it and/or
 *  modify it under the terms of the GNU Lesser General Public
 *  License as published by the Free Software Foundation, under
 *  version 2.1 of the License, or (at your option) any later version.
 */

// Native shadows for prebuilt network pieces and True3D props.
//
// SimCity 4 Deluxe 1.1.641, Windows x86, image base 0x00400000. Opt-in with
// -NativeShadowMasks:network, :props or :all; :replace is :all plus
// NativeShadowRegistry. Every site is guarded by its exact bytes and restored
// during PreAppShutdown.
//
// What the Phase 1 instrumentation established, and why the patches are shaped
// this way (see docs/true3d-shadows-phase1.md):
//
//  * A prebuilt network piece casts a native shadow if and only if an FSH exists at
//    {0x7AB50E44, 0x2BC2759A, base + zoom}. Blanking those masks removed every
//    network shadow in the city, so the decal, its terrain following and its
//    lifetime need no patching at all. Nothing here creates decals.
//  * What does block pieces is the gate at 0x0061E35C: UpdateShadow requires
//    HasNetworkFlag(1) && HasNetworkFlag(4) before it will even look a texture
//    up. Pieces that fail it log `decal0 = -1 -> -1` and never reach the
//    mapping. That gate is the one thing worth relaxing, and it is site A.
//  * Relaxing it alone would ask for masks that may not exist, so site B takes
//    the mapped instance and, for a piece that only got through because of the
//    relaxation, returns 0 when no mask is available. A zero return is the
//    game's own "no shadow texture" path at 0x0061E5A0, so a missing mask falls
//    back to exactly the original behaviour.
// Verified sites:
//
//   A 0x0061E35C  35 bytes  the HasNetworkFlag(1)/(4)/quality gate
//   B 0x0061E589   7 bytes  TEST EAX,EAX / MOV ECX,-2 after MapShadowTexture
//   C 0x0049134B   5 bytes  the Is Ground Model property test
//   D 0x0049195E   6 bytes  the cSTEOverlayManager::AddShadow call
//
// Prebuilt-network Draw (site E) and the rejected True3D prop path identify the
// corresponding cGDriver draws. The driver still has the index buffer and can
// therefore project the actual alpha-tested triangles. Existing network mask
// decals are retained for non-prebuilt/fallback geometry.
// Sites C and D are shared with the diagnostics and research modules. This
// module installs first; their byte guards make them skip rather than overlap.
//
//   E 0x0061E860   5 bytes  cSC4NetworkOccupantWithPreBuiltModel::Draw wrapper
//
// Verified in Ghidra (SimCity 4.exe): 0x0061E860 is __thiscall(self, 2 args,
// RET 0x8); it refreshes UpdateShadow (CALL 0x0061E340) on quality change and
// forwards both args to the base renderer at 0x0061A850. Bridge occupants
// (class 0x49CC1BCD) route their Draw through the same wrapper: slot 12 of
// their renderable subobject vtable (0x00AA7190) holds the this-adjusting thunk
// at 0x00605320 (SUB ECX,[ECX-4]; SUB ECX,0x18; JMP 0x0061E860). So one bracket
// covers prebuilt and bridge puzzle pieces alike.
//
// Site E is not enough on its own, which is why sites F and G exist (see
// docs/true3d-shadows-model-instance-path.md):
//
//   F 0x0061E8A0   8 bytes  cSC4NetworkOccupantWithPreBuiltModel::GetModelInstances
//   G 0x006147D0  11 bytes  the cS3DModelInstance drawable thunk (vtable +0x30)
//
// A piece is two separate entries in the view's sorted drawable list, each
// drawn through virtual slot +0x30 by DrawStaticView_ (0x007C7370):
//
//   * the occupant itself, whose Draw (site E -> 0x0061A850) renders only the
//     network's flat textured quads - its callees are DrawPrims/DrawPrimsIndexed
//     and nothing else, so the piece's True3D deck is never inside that bracket;
//   * the piece's model, a cS3DModelInstance created in GetModelInstances from
//     the RKT0 key {0x5AD0E817, 0xBADB57F1} and kept at occupant +0x1C4/+0x1C8,
//     drawn through the thunk at 0x006147D0 -> 0x00801750 -> RenderModelInstance
//     -> RenderMesh, which is where cGDriver finally sees the triangles.
//
// Nor can the piece take the prop route: cSC4ModelMaker::AddModels (0x00494390)
// calls CreateOccupantShadow for props, buildings and power poles but not for
// network occupants (0xC772BF98), and those three are its only callers, so no
// prop-style mesh signature for a deck can ever be registered.
//
// Site F records the two model instances an occupant owns; site G brackets the
// draw of a recorded instance. Ownership is re-checked against occupant +0x1C4 /
// +0x1C8 on every hit, so an instance that was freed and whose address was
// recycled cannot masquerade as a caster: a live occupant holds a reference to
// its own instance, so a matching slot means the entry is still real.

#include "NativeShadowMasks.h"

#include "ModuleLog.h"
#include "NativeShadowRegistry.h"
#include "Settings.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <initializer_list>
#include <mutex>
#include <shared_mutex>
#include <unordered_map>
#include <vector>

#include <windows.h>

namespace scvk::NativeShadowMasks {
	namespace {

		constexpr uintptr_t kImageBase = 0x00400000;

		constexpr uintptr_t kNetworkGateVA = 0x0061E35C;
		constexpr uintptr_t kNetworkGateAcceptVA = 0x0061E37F; // MOV BL,1
		constexpr uintptr_t kNetworkGateRejectVA = 0x0061E383; // XOR BL,BL
		constexpr uintptr_t kMapResultVA = 0x0061E589;
		constexpr uintptr_t kMapResultRejoinVA = 0x0061E590;
		constexpr uintptr_t kPrebuiltDrawVA = 0x0061E860;
		constexpr uintptr_t kPrebuiltDrawRejoinVA = 0x0061E865;
		constexpr uintptr_t kPrebuiltModelsVA = 0x0061E8A0;
		constexpr uintptr_t kPrebuiltModelsRejoinVA = 0x0061E8A8;
		constexpr uintptr_t kModelInstanceDrawThunkVA = 0x006147D0;
		constexpr uintptr_t kModelInstanceDrawVA = 0x00801750;
		// The two cS3DModelInstance interface pointers a prebuilt piece owns.
		constexpr ptrdiff_t kPrebuiltModelSlots[]{0x1C4, 0x1C8};
		constexpr uintptr_t kRenderPropertiesPointerVA = 0x00B43CC4;
		constexpr uintptr_t kPropGroundModelVA = 0x0049134B;
		constexpr uintptr_t kPropGroundModelRejoinVA = 0x0049136A;
		constexpr uintptr_t kGetBoolPropertyVA = 0x005FD390;
		constexpr uintptr_t kAddShadowSiteVA = 0x0049195E;
		constexpr uintptr_t kAddShadowRejoinVA = 0x00491964;
		constexpr ptrdiff_t kFrameOccupant = 0xE0;
		constexpr ptrdiff_t kAddShadowArgumentBytes = 0x14;
		// HasNetworkFlag on the base subobject at this+4.
		constexpr size_t kHasNetworkFlagSlot = 0x80 / sizeof(void *);

		constexpr size_t kMaxPatchLength = 40;

		enum class Mode { Off, Network, Props, All };

		struct PatchSite {
			uintptr_t virtualAddress = 0;
			size_t length = 0;
			std::array<uint8_t, kMaxPatchLength> expected{};
			std::array<uint8_t, kMaxPatchLength> replacement{};
			std::array<uint8_t, kMaxPatchLength> saved{};
			uint8_t *address = nullptr;
			bool installed = false;
		};

		PatchSite gNetworkGate;
		PatchSite gMapResult;
		PatchSite gPrebuiltDraw;
		PatchSite gPrebuiltModels;
		PatchSite gModelInstanceDraw;
		PatchSite gPropGroundModel;
		PatchSite gAddShadowSite;

		Mode gMode = Mode::Off;
		bool gInstalled = false;

		uintptr_t gNetworkGateAccept = 0;
		uintptr_t gNetworkGateReject = 0;
		uintptr_t gMapResultRejoin = 0;
		uintptr_t gPrebuiltDrawRejoin = 0;
		uintptr_t gPrebuiltModelsRejoin = 0;
		uintptr_t gModelInstanceDrawTarget = 0;
		uintptr_t gRenderPropertiesPointer = 0;
		uintptr_t gPropGroundModelRejoin = 0;
		uintptr_t gAddShadowRejoin = 0;
		uintptr_t gGetBoolProperty = 0;

		// Available mask instances, sorted for binary search, plus explicit
		// source -> generated remappings. Loaded once; never touched during a
		// frame, so lookups need no lock.
		std::vector<uint32_t> gMaskInstances;
		std::vector<std::pair<uint32_t, uint32_t>> gMaskRemaps;

		// Set by the gate stub, read by the mapping stub a few calls later on
		// the same thread inside the same UpdateShadow.
		thread_local bool tGatePassedOriginally = false;
		thread_local uint32_t tLiveNetworkDrawDepth = 0;
		thread_local void *tRelaxedPropOccupant = nullptr;

		std::mutex gPropMutex;
		struct LivePropMesh {
			uint64_t signature = 0;
		};
		std::vector<LivePropMesh> gLivePropMeshes;
		// Whether gLivePropMeshes has anything, readable without the lock:
		// HasLivePropMeshes runs on every draw the game makes.
		std::atomic<bool> gHaveLivePropMeshes{false};

		// Model instances owned by prebuilt network pieces, mapped to the
		// occupant that owns them so a hit can be re-validated. Read once per
		// cS3DModelInstance draw, written only when a piece loads its models, so
		// a shared lock is the right shape - and the atomic count keeps the
		// common "nothing registered" case off the lock entirely.
		std::shared_mutex gLiveModelMutex;
		std::unordered_map<void const *, void const *> gLiveModelInstances;
		std::atomic<size_t> gLiveModelInstanceCount{0};
		// Entries are only replaced, never erased, because there is no cheap
		// notification when an occupant dies. The cap stops a pathological
		// session from growing the map without bound; the validation below is
		// what keeps stale entries harmless.
		constexpr size_t kMaxLiveModelInstances = 1u << 16;

		unsigned gNetworkGateRelaxed = 0;
		unsigned gNetworkMaskSupplied = 0;
		unsigned gNetworkFellBack = 0;
		unsigned gLiveNetworkDraws = 0;
		unsigned gLiveModelsRegistered = 0;
		unsigned gLiveModelDraws = 0;
		unsigned gLiveModelsRejected = 0;
		unsigned gPropMeshesRegistered = 0;
		unsigned gPropCallsSuppressed = 0;

		// ------------------------------------------------------------------
		// Guarded reads
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

		bool SafeHasNetworkFlag(void *flags, uint32_t bit, bool &result) {
			__try {
				void **const vtable = *reinterpret_cast<void ***>(flags);
				using Query = uint8_t(__thiscall *)(void *, uint32_t);
				result = reinterpret_cast<Query>(vtable[kHasNetworkFlagSlot])(flags, bit) != 0;
				return true;
			} __except (EXCEPTION_EXECUTE_HANDLER) {
				return false;
			}
		}
#else
		bool SafeCopy(void const *, void *, size_t) { return false; }
		bool SafeHasNetworkFlag(void *, uint32_t, bool &) { return false; }
#endif

		bool HasGeneratedMask(uint32_t instance) {
			return std::binary_search(gMaskInstances.begin(), gMaskInstances.end(), instance);
		}

		uint32_t RemapInstance(uint32_t instance) {
			for (auto const &entry: gMaskRemaps) {
				if (entry.first == instance) return entry.second;
			}
			return 0;
		}

		void HashWord(uint64_t &hash, uint32_t word) {
			hash = (hash ^ word) * 0x100000001B3ull;
		}

		bool LiveShadowDiagnosticsEnabled() {
			static bool const enabled = std::strstr(GetCommandLineA(), "-LiveShadowDiag") != nullptr;
			return enabled;
		}

		// ------------------------------------------------------------------
		// Prebuilt model instances
		// ------------------------------------------------------------------

		// Records the model instances a prebuilt piece owns, read straight back
		// out of the occupant once GetModelInstances has refreshed both slots.
		void RegisterPrebuiltModelInstances(void const *occupant) {
			if (occupant == nullptr) return;
			for (ptrdiff_t const slot: kPrebuiltModelSlots) {
				void const *instance = nullptr;
				if (!SafeCopy(static_cast<uint8_t const *>(occupant) + slot, &instance, sizeof(instance)) ||
				    instance == nullptr)
					continue;
				std::unique_lock<std::shared_mutex> lock(gLiveModelMutex);
				if (gLiveModelInstances.size() >= kMaxLiveModelInstances) {
					gLiveModelInstances.clear();
					Log(LogCategory::Initialization,
					    "live shadows: prebuilt model registry hit its cap, cleared");
				}
				if (gLiveModelInstances.insert_or_assign(instance, occupant).second) ++gLiveModelsRegistered;
				gLiveModelInstanceCount.store(gLiveModelInstances.size(), std::memory_order_relaxed);
			}
		}

		// True while this instance is still one of its occupant's prebuilt
		// models. The second half is what makes the registry safe: nothing tells
		// us when an instance dies, so a freed address that the allocator handed
		// to some other model would otherwise stay a caster forever. A live
		// occupant holds a reference to its own instance, so a slot that still
		// points here means the entry is real.
		bool IsPrebuiltModelInstance(void const *instance) {
			if (instance == nullptr) return false;
			if (gLiveModelInstanceCount.load(std::memory_order_relaxed) == 0) return false;
			void const *occupant = nullptr;
			{
				std::shared_lock<std::shared_mutex> lock(gLiveModelMutex);
				auto const found = gLiveModelInstances.find(instance);
				if (found == gLiveModelInstances.end()) return false;
				occupant = found->second;
			}
			for (ptrdiff_t const slot: kPrebuiltModelSlots) {
				void const *current = nullptr;
				if (SafeCopy(static_cast<uint8_t const *>(occupant) + slot, &current, sizeof(current)) &&
				    current == instance)
					return true;
			}
			++gLiveModelsRejected;
			return false;
		}

		// ------------------------------------------------------------------
		// Manifest
		// ------------------------------------------------------------------

		std::filesystem::path ManifestPath() {
			HMODULE module = nullptr;
			wchar_t path[MAX_PATH]{};
			if (!GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
			                        GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
			                        reinterpret_cast<LPCWSTR>(&ManifestPath), &module) ||
			    GetModuleFileNameW(module, path, MAX_PATH) == 0) {
				return std::filesystem::path(L"SC4ShadowMasks.txt");
			}
			return std::filesystem::path(path).parent_path() / L"SC4ShadowMasks.txt";
		}

		// One available hex instance per line, or "source -> generated" to point
		// a piece at a mask baked under a different id. Blank lines and # are ignored.
		void LoadManifest() {
			gMaskInstances.clear();
			gMaskRemaps.clear();
			std::filesystem::path const path = ManifestPath();
			FILE *file = nullptr;
			if (_wfopen_s(&file, path.c_str(), L"r") != 0 || file == nullptr) {
				Log(LogCategory::Initialization,
				    "native shadow masks: no manifest at the DLL folder, relaxed pieces will fall back");
				return;
			}
			char line[256]{};
			while (std::fgets(line, sizeof(line), file) != nullptr) {
				char *cursor = line;
				while (*cursor == ' ' || *cursor == '\t') ++cursor;
				if (*cursor == '#' || *cursor == '\0' || *cursor == '\n' || *cursor == '\r') continue;
				// Old experimental prop-proxy records are ignored. Geometry-only
				// proxies are known to generate solid slabs and are no longer a
				// supported runtime mode.
				if (*cursor == 'P' || *cursor == 'p') continue;
				char *end = nullptr;
				unsigned long const source = std::strtoul(cursor, &end, 16);
				if (end == cursor) continue;
				while (*end == ' ' || *end == '\t') ++end;
				if (end[0] == '-' && end[1] == '>') {
					end += 2;
					char *target = nullptr;
					unsigned long const generated = std::strtoul(end, &target, 16);
					if (target != end) {
						gMaskRemaps.emplace_back(static_cast<uint32_t>(source),
						                         static_cast<uint32_t>(generated));
						gMaskInstances.push_back(static_cast<uint32_t>(generated));
						continue;
					}
				}
				gMaskInstances.push_back(static_cast<uint32_t>(source));
			}
			std::fclose(file);
			std::sort(gMaskInstances.begin(), gMaskInstances.end());
			gMaskInstances.erase(std::unique(gMaskInstances.begin(), gMaskInstances.end()),
			                     gMaskInstances.end());
			Log(LogCategory::Initialization,
			    "native shadow masks: manifest has %u instances and %u remappings",
			    static_cast<unsigned>(gMaskInstances.size()), static_cast<unsigned>(gMaskRemaps.size()));
		}

	} // namespace

	// ----------------------------------------------------------------------
	// Hook bodies
	// ----------------------------------------------------------------------

	// Replaces the whole HasNetworkFlag(1) && HasNetworkFlag(4) && quality > 2
	// gate. Returns the value the original would have produced unless a mask
	// manifest is loaded, in which case the two network-type bits stop being a
	// reason to skip the piece. ShadowQuality is never relaxed, so quality 0-2
	// behaves exactly as before.
	extern "C" uint32_t __cdecl SCD3D11_MaskNetworkGate(void *occupant, void *flags, int32_t quality) {
		(void) occupant;
		bool flagOne = false;
		bool flagFour = false;
		bool const readable =
			SafeHasNetworkFlag(flags, 1, flagOne) && SafeHasNetworkFlag(flags, 4, flagFour);
		bool const original = readable && flagOne && flagFour && quality > 2;
		tGatePassedOriginally = original;
		if (original) return 1;
		if (quality <= 2 || gMaskInstances.empty()) return 0;
		++gNetworkGateRelaxed;
		return 1;
	}

	// Runs on MapShadowTexture's return value. A piece that would have passed
	// the original gate keeps whatever the game mapped. A piece that is only
	// here because the gate was relaxed needs a mask we generated; without one
	// it returns 0, which is the game's own abort at 0x0061E5A0.
	extern "C" uint32_t __cdecl SCD3D11_MaskShadowTexture(uint32_t instance) {
		if (instance == 0) return 0;
		if (uint32_t const remapped = RemapInstance(instance); remapped != 0) {
			++gNetworkMaskSupplied;
			return remapped;
		}
		if (HasGeneratedMask(instance)) {
			++gNetworkMaskSupplied;
			return instance;
		}
		if (tGatePassedOriginally) return instance;
		++gNetworkFellBack;
		return 0;
	}

	extern "C" void __cdecl SCD3D11_MaskNoteRelaxedProp(void *occupant) {
		tRelaxedPropOccupant = occupant;
	}

	// The rejected prop has reached the point where SC4 extracted its position
	// and UV streams. Record a stable mesh signature for the indexed renderer,
	// then suppress AddShadow: its one affine projector is exactly the path that
	// produced the giant filled slabs. With the shadow registry installed the
	// call goes through instead: the registry keeps the mesh with its own UVs,
	// takes the record out of DrawShadows, and SC4 keeps the record's lifetime
	// and dirty rectangles, so the prop needs no per-draw capture at all.
	extern "C" uint32_t __cdecl SCD3D11_RegisterLiveProp(uint32_t *stackArguments) {
		uint8_t const *const frameBase =
			reinterpret_cast<uint8_t const *>(stackArguments) + kAddShadowArgumentBytes;
		uint32_t occupant = 0;
		if (!SafeCopy(frameBase + kFrameOccupant, &occupant, sizeof(occupant)) || occupant == 0 ||
		    reinterpret_cast<void *>(occupant) != tRelaxedPropOccupant) return 0;

		uint32_t const count = stackArguments[0];
		void const *const positions = reinterpret_cast<void const *>(stackArguments[1]);
		void const *const uvs = reinterpret_cast<void const *>(stackArguments[2]);
		if (count < 3 || count > 0x100000 || positions == nullptr || uvs == nullptr) return 2;
		if (NativeShadowRegistry::Enabled()) {
			NativeShadowRegistry::ExpectMeshCaster();
			return 0;
		}
		RegisterLivePropMesh(count, positions, uvs);
		++gPropCallsSuppressed;
		return 2;
	}

	void RegisterLivePropMesh(uint32_t count, void const *positionStream, void const *uvStream) {
		struct Vector3 { float x, y, z; };
		struct Vector2 { float u, v; };
		auto const *const positions = static_cast<Vector3 const *>(positionStream);
		auto const *const uvs = static_cast<Vector2 const *>(uvStream);
		if (count < 3 || count > 0x100000 || positions == nullptr || uvs == nullptr) return;
		uint64_t signature = 0xCBF29CE484222325ull;
		HashWord(signature, count);
		for (uint32_t index = 0; index < count; ++index) {
			Vector3 position{};
			Vector2 uv{};
			if (!SafeCopy(positions + index, &position, sizeof(position)) ||
			    !SafeCopy(uvs + index, &uv, sizeof(uv))) return;
			uint32_t words[5]{};
			std::memcpy(words, &position, sizeof(position));
			std::memcpy(words + 3, &uv, sizeof(uv));
			for (uint32_t word: words) HashWord(signature, word);
		}
		{
			std::lock_guard<std::mutex> lock(gPropMutex);
			auto const found = std::lower_bound(
				gLivePropMeshes.begin(), gLivePropMeshes.end(), signature,
				[](LivePropMesh const &mesh, uint64_t value) { return mesh.signature < value; });
			bool const inserted = found == gLivePropMeshes.end() || found->signature != signature;
			if (inserted) {
				LivePropMesh mesh{};
				mesh.signature = signature;
				gLivePropMeshes.insert(found, std::move(mesh));
				gHaveLivePropMeshes.store(true, std::memory_order_relaxed);
				++gPropMeshesRegistered;
				Log(LogCategory::Initialization,
				    "native shadows: registered live prop mesh sig=%08X%08X verts=%u",
				    static_cast<unsigned>(signature >> 32), static_cast<unsigned>(signature), count);
			}
			if (!inserted && LiveShadowDiagnosticsEnabled()) {
				// Re-registration is the steady state (one AddShadow call per
				// mesh per redraw), so this must stay a one-shot: logging every
				// hit exhausts the init log budget in seconds and suppresses
				// every later diagnostic line.
				static bool loggedReuse = false;
				if (!loggedReuse) {
					loggedReuse = true;
					Log(LogCategory::Initialization,
					    "liveshadow-reg reused sig=%08X%08X verts=%u",
					    static_cast<unsigned>(signature >> 32), static_cast<unsigned>(signature), count);
				}
			}
		}
	}

	bool LivePropsEnabled() {
		return gInstalled && (gMode == Mode::Props || gMode == Mode::All);
	}

	bool LiveNetworkEnabled() {
		return gInstalled && (gMode == Mode::Network || gMode == Mode::All);
	}

	bool LiveNetworkDrawActive() {
		return LiveNetworkEnabled() && tLiveNetworkDrawDepth != 0;
	}

	bool RequiresCleanTranslatedRedraw() {
		return gInstalled && gMode != Mode::Off;
	}

	bool HasLivePropMeshes() {
		return LivePropsEnabled() && gHaveLivePropMeshes.load(std::memory_order_relaxed);
	}

	bool MatchLivePropSignature(uint64_t signature) {
		if (!LivePropsEnabled()) return false;
		std::lock_guard<std::mutex> lock(gPropMutex);
		auto const found = std::lower_bound(
			gLivePropMeshes.begin(), gLivePropMeshes.end(), signature,
			[](LivePropMesh const &mesh, uint64_t value) { return mesh.signature < value; });
		return found != gLivePropMeshes.end() && found->signature == signature;
	}

	namespace {

#if defined(_MSC_VER) && defined(_M_IX86)

		// Windows 1.1.641 0x0061E860 is the two-argument __thiscall wrapper used
		// to draw cSC4NetworkOccupantWithPreBuiltModel. It refreshes UpdateShadow,
		// then calls the ordinary network-occupant renderer. Bracketing that call
		// lets cGDriver recognize its indexed S3D draws without admitting terrain,
		// water, UI or unrelated overlays into the live shadow map.
		__declspec(naked) void PrebuiltDrawOriginal() {
			__asm {
				push edx
				mov  edx, dword ptr [gRenderPropertiesPointer]
				mov  eax, dword ptr [edx]
				pop  edx
				jmp  dword ptr [gPrebuiltDrawRejoin]
			}
		}

		uint32_t __fastcall PrebuiltDrawLiveShadowHook(
			void *self, void *, void *drawContext, void *drawState) {
			using Original = uint32_t (__thiscall *)(void *, void *, void *);
			++tLiveNetworkDrawDepth;
			uint32_t const result =
				reinterpret_cast<Original>(&PrebuiltDrawOriginal)(self, drawContext, drawState);
			--tLiveNetworkDrawDepth;
			++gLiveNetworkDraws;
			return result;
		}

		// Site F. 0x0061E8A0 is __thiscall(self, 5 stack args, RET 0x14); the two
		// displaced MOVs read arguments off ESP, which is why the trampoline has
		// to be entered by a call carrying the same arguments.
		__declspec(naked) void PrebuiltModelsOriginal() {
			__asm {
				mov eax, dword ptr [esp + 0x14]
				mov edx, dword ptr [esp + 0x0c]
				jmp dword ptr [gPrebuiltModelsRejoin]
			}
		}

		void __fastcall PrebuiltModelsHook(
			void *self, void *, int zoom, int rotation, void **instances, int capacity, int *count) {
			using Original = void (__thiscall *)(void *, int, int, void **, int, int *);
			reinterpret_cast<Original>(&PrebuiltModelsOriginal)(
				self, zoom, rotation, instances, capacity, count);
			// Read the instances back out of the occupant rather than out of the
			// array: the array also carries whatever the base network occupant
			// contributed, and +0x1C4/+0x1C8 are the slots the validation checks.
			RegisterPrebuiltModelInstances(self);
		}

		// Site G. Replaces the whole this-adjusting thunk, so ECX here is still
		// the interface pointer the drawable list stores - the same value
		// GetModelInstances handed out - and no pointer arithmetic is needed to
		// compare the two. The trampoline replays the adjustment the thunk did.
		__declspec(naked) void ModelInstanceDrawOriginal() {
			__asm {
				sub ecx, dword ptr [ecx - 4]
				sub ecx, 0x40
				jmp dword ptr [gModelInstanceDrawTarget]
			}
		}

		uint32_t __fastcall ModelInstanceDrawHook(void *self, void *, void *drawContext, uint32_t lit) {
			using Original = uint32_t (__thiscall *)(void *, void *, uint32_t);
			bool const caster = IsPrebuiltModelInstance(self);
			if (caster) ++tLiveNetworkDrawDepth;
			uint32_t const result =
				reinterpret_cast<Original>(&ModelInstanceDrawOriginal)(self, drawContext, lit);
			if (caster) {
				--tLiveNetworkDrawDepth;
				++gLiveModelDraws;
				static bool loggedFirst = false;
				if (!loggedFirst) {
					loggedFirst = true;
					Log(LogCategory::Initialization,
					    "live shadows: first prebuilt model bracketed (instance %p)", self);
				}
			}
			return result;
		}

		// ESI is the occupant, EDI the flag subobject at ESI+4 and EBX holds
		// ShadowQuality. Rejoins at MOV BL,1 or XOR BL,BL, so the original
		// register contract is preserved either way.
		__declspec(naked) void NetworkGateStub() {
			__asm {
				push ecx
				push edx
				push edi
				push ebx
				push edi
				push esi
				call SCD3D11_MaskNetworkGate
				add  esp, 0xc
				pop  edi
				pop  edx
				pop  ecx
				test al, al
				jz   reject
				jmp  dword ptr [gNetworkGateAccept]
			reject:
				jmp  dword ptr [gNetworkGateReject]
			}
		}

		// EAX holds the mapped instance. The replayed TEST/MOV reproduce the
		// two displaced instructions, and the JZ at 0x0061E5A0 still reads the
		// flags the TEST sets.
		__declspec(naked) void MapResultStub() {
			__asm {
				push ecx
				push edx
				push eax
				call SCD3D11_MaskShadowTexture
				add  esp, 4
				pop  edx
				pop  ecx
				test eax, eax
				mov  ecx, 0xfffffffe
				jmp  dword ptr [gMapResultRejoin]
			}
		}

		__declspec(naked) void PropGroundModelStub() {
			__asm {
				push eax
				push ecx
				push edx
				push 0
				call SCD3D11_MaskNoteRelaxedProp
				add  esp, 4
				pop  edx
				pop  ecx
				pop  eax
				lea  ecx, [esp + 0x13]
				push ecx
				push 0x8a5e5db8
				push ebp
				call dword ptr [gGetBoolProperty]
				add  esp, 0xc
				test al, al
				jz   relaxed
				mov  al, byte ptr [esp + 0x13]
				test al, al
				jz   relaxed
				jmp  dword ptr [gPropGroundModelRejoin]
			relaxed:
				push eax
				push ecx
				push edx
				push esi
				call SCD3D11_MaskNoteRelaxedProp
				add  esp, 4
				pop  edx
				pop  ecx
				pop  eax
				jmp  dword ptr [gPropGroundModelRejoin]
			}
		}

		__declspec(naked) void AddShadowLivePropStub() {
			__asm {
				pushad
				pushfd
				lea  eax, [esp + 0x24]
				push eax
				call SCD3D11_RegisterLiveProp
				add  esp, 4
				mov  dword ptr [esp + 0x20], eax
				popfd
				popad
				cmp  eax, 2
				je   suppress
				mov  ecx, ebp
				push edi
				call dword ptr [edx + 0x2c]
				jmp  dword ptr [gAddShadowRejoin]
			suppress:
				add  esp, 0x14
				or   eax, 0xffffffff
				jmp  dword ptr [gAddShadowRejoin]
			}
		}

#endif // _MSC_VER && _M_IX86

		// ------------------------------------------------------------------
		// Patch plumbing
		// ------------------------------------------------------------------

		void ConfigureSite(PatchSite &site, uintptr_t virtualAddress, std::initializer_list<uint8_t> expected) {
			site = PatchSite{};
			site.virtualAddress = virtualAddress;
			site.length = expected.size();
			std::copy(expected.begin(), expected.end(), site.expected.begin());
		}

		void ConfigureJumpSite(PatchSite &site, uintptr_t virtualAddress, std::initializer_list<uint8_t> expected,
		                       uint8_t *target, void *destination) {
			ConfigureSite(site, virtualAddress, expected);
			site.replacement.fill(0x90);
			site.replacement[0] = 0xE9;
			int32_t const displacement = static_cast<int32_t>(reinterpret_cast<uintptr_t>(destination) -
			                                                 (reinterpret_cast<uintptr_t>(target) + 5));
			std::memcpy(site.replacement.data() + 1, &displacement, sizeof(displacement));
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

		Mode ReadMode() {
			// scvk: from the settings, replace by default, rather than only from the
			// command line. replace = every live path plus NativeShadowRegistry, which
			// takes over the AddShadow records these paths leave to SC4.
			switch (GetSettings().nativeShadowMode) {
				case NativeShadowMode::All:
				case NativeShadowMode::Replace: return Mode::All;
				case NativeShadowMode::Props: return Mode::Props;
				case NativeShadowMode::Network: return Mode::Network;
				case NativeShadowMode::Off:
				default: return Mode::Off;
			}
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
				Log(LogCategory::Initialization, "native shadow masks: not restoring modified site 0x%08lX",
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
		Mode const mode = ReadMode();
		if (mode == Mode::Off) return false;
		if (!IsSupportedGameVersion()) {
			Log(LogCategory::Initialization, "native shadow masks: requires SimCity 4 1.1.641, skipping");
			return false;
		}

		uint8_t *const module = reinterpret_cast<uint8_t *>(GetModuleHandleW(nullptr));
		if (module == nullptr) return false;
		auto const resolve = [module](uintptr_t virtualAddress) {
			return module + (virtualAddress - kImageBase);
		};

		gNetworkGateAccept = reinterpret_cast<uintptr_t>(resolve(kNetworkGateAcceptVA));
		gNetworkGateReject = reinterpret_cast<uintptr_t>(resolve(kNetworkGateRejectVA));
		gMapResultRejoin = reinterpret_cast<uintptr_t>(resolve(kMapResultRejoinVA));
		gPrebuiltDrawRejoin = reinterpret_cast<uintptr_t>(resolve(kPrebuiltDrawRejoinVA));
		gPrebuiltModelsRejoin = reinterpret_cast<uintptr_t>(resolve(kPrebuiltModelsRejoinVA));
		gModelInstanceDrawTarget = reinterpret_cast<uintptr_t>(resolve(kModelInstanceDrawVA));
		gRenderPropertiesPointer = reinterpret_cast<uintptr_t>(resolve(kRenderPropertiesPointerVA));
		gPropGroundModelRejoin = reinterpret_cast<uintptr_t>(resolve(kPropGroundModelRejoinVA));
		gAddShadowRejoin = reinterpret_cast<uintptr_t>(resolve(kAddShadowRejoinVA));
		gGetBoolProperty = reinterpret_cast<uintptr_t>(resolve(kGetBoolPropertyVA));
		ConfigureJumpSite(gNetworkGate, kNetworkGateVA,
		                  {0x6A, 0x01, 0x8B, 0xCF, 0xFF, 0x92, 0x80, 0x00, 0x00, 0x00, 0x84, 0xC0, 0x74,
		                   0x19, 0x8B, 0x07, 0x6A, 0x04, 0x8B, 0xCF, 0xFF, 0x90, 0x80, 0x00, 0x00, 0x00,
		                   0x84, 0xC0, 0x74, 0x09, 0x83, 0xFB, 0x02, 0x7E, 0x04},
		                  resolve(kNetworkGateVA), &NetworkGateStub);
		ConfigureJumpSite(gMapResult, kMapResultVA, {0x85, 0xC0, 0xB9, 0xFE, 0xFF, 0xFF, 0xFF},
		                  resolve(kMapResultVA), &MapResultStub);
		ConfigureJumpSite(gPrebuiltDraw, kPrebuiltDrawVA, {0xA1, 0xC4, 0x3C, 0xB4, 0x00},
		                  resolve(kPrebuiltDrawVA), &PrebuiltDrawLiveShadowHook);
		ConfigureJumpSite(gPrebuiltModels, kPrebuiltModelsVA,
		                  {0x8B, 0x44, 0x24, 0x14, 0x8B, 0x54, 0x24, 0x0C},
		                  resolve(kPrebuiltModelsVA), &PrebuiltModelsHook);
		// The displacement in the thunk's own JMP is image-base independent, so
		// it is part of the guard.
		ConfigureJumpSite(gModelInstanceDraw, kModelInstanceDrawThunkVA,
		                  {0x2B, 0x49, 0xFC, 0x83, 0xE9, 0x40, 0xE9, 0x75, 0xCF, 0x1E, 0x00},
		                  resolve(kModelInstanceDrawThunkVA), &ModelInstanceDrawHook);
		ConfigureJumpSite(gPropGroundModel, kPropGroundModelVA, {0x8D, 0x4C, 0x24, 0x13, 0x51},
		                  resolve(kPropGroundModelVA), &PropGroundModelStub);
		ConfigureJumpSite(gAddShadowSite, kAddShadowSiteVA, {0x8B, 0xCD, 0x57, 0xFF, 0x52, 0x2C},
		                  resolve(kAddShadowSiteVA), &AddShadowLivePropStub);
		PatchSite *const networkSites[]{&gNetworkGate, &gMapResult, &gPrebuiltDraw, &gPrebuiltModels,
		                                &gModelInstanceDraw};
		PatchSite *const propSites[]{&gPropGroundModel, &gAddShadowSite};
		std::vector<PatchSite *> sites;
		if (mode == Mode::Network || mode == Mode::All)
			sites.insert(sites.end(), std::begin(networkSites), std::end(networkSites));
		if (mode == Mode::Props || mode == Mode::All)
			sites.insert(sites.end(), std::begin(propSites), std::end(propSites));
		for (PatchSite *site: sites) site->address = resolve(site->virtualAddress);
		for (PatchSite const *site: sites) {
			if (!BytesMatch(*site)) {
				Log(LogCategory::Initialization,
				    "native shadow masks: byte guard failed at 0x%08lX, skipping",
				    static_cast<unsigned long>(site->virtualAddress));
				return false;
			}
		}

		if (mode == Mode::Network || mode == Mode::All) LoadManifest();
		else {
			gMaskInstances.clear();
			gMaskRemaps.clear();
		}
		{
			std::lock_guard<std::mutex> lock(gPropMutex);
			gLivePropMeshes.clear();
			gHaveLivePropMeshes.store(false, std::memory_order_relaxed);
		}
		{
			std::unique_lock<std::shared_mutex> lock(gLiveModelMutex);
			gLiveModelInstances.clear();
			gLiveModelInstanceCount.store(0, std::memory_order_relaxed);
		}
		gMode = mode;
		tLiveNetworkDrawDepth = 0;
		for (PatchSite *site: sites) {
			if (!WriteSite(*site)) {
				Log(LogCategory::Initialization,
				    "native shadow masks: failed to write site 0x%08lX, restoring",
				    static_cast<unsigned long>(site->virtualAddress));
				for (PatchSite *written: sites) RestoreSite(*written);
				gMode = Mode::Off;
				return false;
			}
		}

		gInstalled = true;
		Log(LogCategory::Initialization, "native shadows installed (%s), %u manifest instances",
		    mode == Mode::All ? "all" : mode == Mode::Props ? "props" : "network",
		    static_cast<unsigned>(gMaskInstances.size()));
		Log(LogCategory::Initialization,
		    "live shadows: capture indexed+array+terrain, textured+untextured; bracket tracing %s",
		    std::strstr(GetCommandLineA(), "-LiveShadowDiag") != nullptr ? "on" : "off");
		if (mode == Mode::Network || mode == Mode::All)
			Log(LogCategory::Initialization,
			    "live shadows: bracketing prebuilt piece models at the cS3DModelInstance drawable");
		return true;
#else
		return false;
#endif
	}

	void Uninstall() {
		if (!gInstalled) return;
		RestoreSite(gAddShadowSite);
		RestoreSite(gPropGroundModel);
		RestoreSite(gModelInstanceDraw);
		RestoreSite(gPrebuiltModels);
		RestoreSite(gPrebuiltDraw);
		RestoreSite(gMapResult);
		RestoreSite(gNetworkGate);
		gMode = Mode::Off;
		gInstalled = false;
		tLiveNetworkDrawDepth = 0;
		{
			std::lock_guard<std::mutex> lock(gPropMutex);
			gLivePropMeshes.clear();
			gHaveLivePropMeshes.store(false, std::memory_order_relaxed);
		}
		{
			std::unique_lock<std::shared_mutex> lock(gLiveModelMutex);
			gLiveModelInstances.clear();
			gLiveModelInstanceCount.store(0, std::memory_order_relaxed);
		}
		Log(LogCategory::Initialization,
		    "native shadows uninstalled: %u live network draws, %u pieces relaxed, %u masks supplied, %u fell back, "
		    "%u live prop meshes, %u native prop calls suppressed",
		    gLiveNetworkDraws, gNetworkGateRelaxed, gNetworkMaskSupplied, gNetworkFellBack,
		    gPropMeshesRegistered, gPropCallsSuppressed);
		Log(LogCategory::Initialization,
		    "prebuilt models: %u instances registered, %u bracketed draws, %u stale entries rejected",
		    gLiveModelsRegistered, gLiveModelDraws, gLiveModelsRejected);
	}

} // namespace scvk::NativeShadowMasks
