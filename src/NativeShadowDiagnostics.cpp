/*
 *  SCD3D11 - a free Direct3D 11 driver for SimCity 4's SimGL interface
 *  Copyright (C) 2026
 *
 *  This library is free software; you can redistribute it and/or
 *  modify it under the terms of the GNU Lesser General Public
 *  License as published by the Free Software Foundation, under
 *  version 2.1 of the License, or (at your option) any later version.
 */

// Phase 1 instrumentation for SimCity 4's two native shadow paths.
//
// This file only observes. It installs four hooks, each of which runs the
// original code unchanged and writes one record per call to SC4ShadowDiag.log.
// Nothing here alters a gate, geometry, texture or decal lifetime, so a run
// with -NativeShadowDiag must look identical to a run without it.
//
// SimCity 4 Deluxe 1.1.641, Windows x86, image base 0x00400000.
//
// Path A - cSC4ModelMaker::CreateOccupantShadow (0x00491230), the mesh-to-
// terrain-decal projector used by buildings, flora, props and power poles:
//
//   0x0049147C  CALL 0x00497180   model resolve, __cdecl, seven arguments,
//                                 argument 6 is the in/out resource key
//   0x0049195E  MOV ECX,EBP / PUSH EDI / CALL [EDX+0x2C]
//                                 cSTEOverlayManager::AddShadow, RET 0x18
//
// AddShadow's arguments, reconstructed against the Mac signature
// AddShadow(cS3DTextureBinding const*, int, cS3DVector3 const*,
//           cS3DVector2 const*, cS3DBoundingBox const&, cS3DTransform const&):
//
//   ECX        overlay manager (EBP); EDX is its vtable
//   EDI        cS3DTextureBinding*, from the material at +0x10
//   [ESP+0x00] vertex count (MOVZX ESI,AX at 0x00491862, so at most 0xFFFF)
//   [ESP+0x04] positions, vertex attribute 0
//   [ESP+0x08] UVs, vertex attribute 7
//   [ESP+0x0C] bounding box: frame +0x40 when ShadowQuality < 5, else +0xC4
//   [ESP+0x10] cS3DTransform, frame +0x58
//
// The frame offsets below are relative to the body stack pointer B, which at
// the AddShadow site is ESP + 0x14 because five arguments are already pushed.
// B is the entry ESP minus 0xDC, so B+0xDC holds the return address and
// B+0xE0/E4/E8 the three stack arguments (occupant, zoom, rotation).
//
// The transform layout is read straight off its construction at
// 0x004915FE-0x0049166A: two bools, a 3x3 matrix, a translation and a uniform
// scale, 0x38 bytes ending at B+0x90, well clear of the UV vector at B+0xA8.
//
// Path B - cSC4NetworkOccupantWithPreBuiltModel::UpdateShadow (0x0061E340),
// the texture-decal generator used by prebuilt network and puzzle pieces:
//
//   0x0061E340  MOV EAX,[0x00B43CC4]           prologue, detoured
//   0x0061E584  MOV ECX,EDI / CALL [EBX+0x38]  MapShadowTexture, RET 0xC
//
// MapShadowTexture returns the instance of shadow resource
// {0x7AB50E44, 0x2BC2759A, instance}. A zero return aborts UpdateShadow at
// 0x0061E5A0 after stamping -2 into both decal slots, so that one number
// decides the whole path. The decals live at this+0x1B8 and this+0x1BC and are
// recorded before and after the original call.
//
// Every patched site is guarded by its exact 1.1.641 bytes and restored during
// PreAppShutdown.

#include "NativeShadowDiagnostics.h"

#include "ModuleLog.h"

#include <algorithm>
#include <array>
#include <cstdarg>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <initializer_list>
#include <iterator>
#include <mutex>
#include <share.h>
#include <vector>

#include <windows.h>

namespace scvk::NativeShadowDiagnostics {
	namespace {

		constexpr uintptr_t kImageBase = 0x00400000;

		constexpr uintptr_t kResolveModelCallVA = 0x0049147C;
		constexpr uintptr_t kResolveModelTargetVA = 0x00497180;
		constexpr uintptr_t kAddShadowSiteVA = 0x0049195E;
		constexpr uintptr_t kAddShadowRejoinVA = 0x00491964;
		constexpr uintptr_t kUpdateShadowVA = 0x0061E340;
		constexpr uintptr_t kUpdateShadowRejoinVA = 0x0061E345;
		constexpr uintptr_t kMapShadowTextureSiteVA = 0x0061E584;
		constexpr uintptr_t kMapShadowTextureRejoinVA = 0x0061E589;
		constexpr uintptr_t kRenderPropertiesSlotVA = 0x00B43CC4;
		// GetShadowDirection copies the three floats at this+0x64 into its out
		// parameter. The baker has to project along exactly this vector, so it
		// is read from the running game rather than guessed.
		constexpr uintptr_t kGetShadowDirectionVA = 0x007D6B50;
		constexpr uintptr_t kGetShadowDirectionRejoinVA = 0x007D6B57;

		// CreateOccupantShadow frame offsets, relative to B.
		constexpr ptrdiff_t kFrameMeshIndex = 0x18;
		constexpr ptrdiff_t kFrameLowBoundingBox = 0x40;
		constexpr ptrdiff_t kFrameTransform = 0x58;
		constexpr ptrdiff_t kFrameHighBoundingBox = 0xC4;
		constexpr ptrdiff_t kFrameReturnAddress = 0xDC;
		// Distance from the AddShadow site's stack pointer back to B: the
		// transform, box, UV, position and count arguments are already pushed.
		constexpr ptrdiff_t kAddShadowArgumentBytes = 0x14;
		// Distance from the resolve thunk's stack pointer back to B: seven
		// arguments plus the return address pushed by the CALL.
		constexpr ptrdiff_t kResolveArgumentBytes = 0x20;

		// cISC4Occupant::GetType, the slot CreateOccupantShadow itself calls at
		// 0x004913AE to reach its 0x2890D4DE power-pole branch.
		constexpr size_t kOccupantGetTypeSlot = 0x1C / sizeof(void *);

		// Render properties singleton: ShadowQuality is index 0xC of the int
		// block, so 0x1C + 0x28 * 0xC = 0x1FC.
		constexpr ptrdiff_t kRenderIntBlock = 0x18;
		constexpr ptrdiff_t kShadowQuality = 0x1FC;

		// Prebuilt network occupant fields. +0x130..+0x138 are floats: a logged
		// record gave 272.98 and 1083.44, i.e. a terrain height and a world
		// coordinate, which is how a piece in the trace is located on the map.
		// +0x13C is the classification bitfield, and its low 13 bits are the
		// edge configuration handed to MapShadowTexture.
		constexpr ptrdiff_t kNetworkClassification = 0x130;
		constexpr ptrdiff_t kNetworkFirstDecal = 0x1B8;
		constexpr ptrdiff_t kNetworkSecondDecal = 0x1BC;

		constexpr uint32_t kShadowResourceType = 0x7AB50E44;
		constexpr uint32_t kShadowResourceGroup = 0x2BC2759A;

		constexpr size_t kMaxPatchLength = 16;
		constexpr unsigned kDefaultRecordBudget = 4000;
		constexpr size_t kSignatureSlots = 8192; // power of two

		struct Vector3 {
			float x, y, z;
		};

		struct Vector2 {
			float x, y;
		};

		struct BoundingBox {
			Vector3 minimum;
			Vector3 maximum;
		};

		// Laid out exactly as CreateOccupantShadow builds it at B+0x58.
		struct Transform {
			uint8_t flags[2];
			uint8_t padding[2];
			float rotation[9];
			float translation[3];
			float scale;
		};

		static_assert(sizeof(BoundingBox) == 0x18, "cS3DBoundingBox must stay 24 bytes");
		static_assert(sizeof(Transform) == 0x38, "cS3DTransform must stay 56 bytes");

		struct PatchSite {
			uintptr_t virtualAddress = 0;
			size_t length = 0;
			std::array<uint8_t, kMaxPatchLength> expected{};
			std::array<uint8_t, kMaxPatchLength> replacement{};
			std::array<uint8_t, kMaxPatchLength> saved{};
			uint8_t *address = nullptr;
			bool installed = false;
		};

		PatchSite gResolveModelCall;
		PatchSite gAddShadowSite;
		PatchSite gUpdateShadow;
		PatchSite gMapShadowTextureSite;
		PatchSite gGetShadowDirection;

		bool gInstalled = false;
		bool gDeduplicate = true;
		unsigned gRecordBudget = kDefaultRecordBudget;

		// Read by the naked stubs. These are absolute addresses in the live
		// SimCity 4 module, not link-time DLL addresses.
		uintptr_t gAddShadowRejoin = 0;
		uintptr_t gUpdateShadowRejoin = 0;
		uintptr_t gMapShadowTextureRejoin = 0;
		uintptr_t gGetShadowDirectionRejoin = 0;
		uintptr_t gRenderPropertiesSlot = 0;
		uintptr_t gResolveModelTarget = 0;

		std::mutex gTraceMutex;
		FILE *gTrace = nullptr;
		unsigned gRecordsWritten = 0;
		unsigned gRecordsSuppressed = 0;
		enum Category { kOccupant, kAddShadow, kNetworkShadow, kNetworkMap, kSunDirection, kCategoryCount };
		unsigned gCounts[kCategoryCount]{};
		std::array<uint64_t, kSignatureSlots> gSignatures{};
		size_t gSignatureCount = 0;

		// ------------------------------------------------------------------
		// Guarded reads. Every pointer here comes from the game, so a stale or
		// unexpected one must never take the process down.
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

		uint32_t SafeOccupantType(void *occupant) {
			__try {
				void **const vtable = *reinterpret_cast<void ***>(occupant);
				using GetType = uint32_t(__thiscall *)(void *);
				return reinterpret_cast<GetType>(vtable[kOccupantGetTypeSlot])(occupant);
			} __except (EXCEPTION_EXECUTE_HANDLER) {
				return 0;
			}
		}

		// One SEH frame for the whole scan rather than one per vertex: a mesh
		// can carry 65535 of them. Results are accumulated through the caller's
		// pointers, so a fault part-way still leaves the vertices read so far.
		void SafeScanAttribute(void const *data, uint32_t count, size_t stride, uint32_t components,
		                       float *minimum, float *maximum, uint32_t *scanned) {
			__try {
				uint8_t const *cursor = static_cast<uint8_t const *>(data);
				for (uint32_t index = 0; index < count; ++index, cursor += stride) {
					float const *const component = reinterpret_cast<float const *>(cursor);
					for (uint32_t axis = 0; axis < components; ++axis) {
						float const value = component[axis];
						if (*scanned == 0) {
							minimum[axis] = maximum[axis] = value;
						} else {
							if (value < minimum[axis]) minimum[axis] = value;
							if (value > maximum[axis]) maximum[axis] = value;
						}
					}
					++*scanned;
				}
			} __except (EXCEPTION_EXECUTE_HANDLER) {
			}
		}
#else
		bool SafeCopy(void const *, void *, size_t) { return false; }

		uint32_t SafeOccupantType(void *) { return 0; }

		void SafeScanAttribute(void const *, uint32_t, size_t, uint32_t, float *, float *, uint32_t *) {}
#endif

		template<typename T>
		bool ReadStruct(void const *source, T &destination) {
			return source != nullptr && SafeCopy(source, &destination, sizeof(T));
		}

		uint32_t ReadDword(void const *source, uint32_t fallback = 0) {
			uint32_t value = fallback;
			return SafeCopy(source, &value, sizeof(value)) ? value : fallback;
		}

		void const *OffsetOf(void const *base, ptrdiff_t offset) {
			return base == nullptr ? nullptr : static_cast<uint8_t const *>(base) + offset;
		}

		int32_t ShadowQuality() {
			uint32_t const singleton = ReadDword(reinterpret_cast<void const *>(gRenderPropertiesSlot));
			if (singleton == 0) return -1;
			uint32_t const block = ReadDword(reinterpret_cast<void const *>(singleton + kRenderIntBlock));
			if (block == 0) return -1;
			return static_cast<int32_t>(
				ReadDword(reinterpret_cast<void const *>(block + kShadowQuality), static_cast<uint32_t>(-1)));
		}

		// ------------------------------------------------------------------
		// Record text
		// ------------------------------------------------------------------

		// A truncating appender, so a long record degrades instead of running
		// off the end of the buffer or rewinding the offset on truncation.
		class TextBuilder {
		public:
			TextBuilder(char *data, size_t capacity) : data_(data), capacity_(capacity) {
				if (capacity_ > 0) data_[0] = '\0';
			}

			void Append(char const *format, ...) {
				if (capacity_ == 0 || offset_ + 1 >= capacity_) return;
				va_list arguments;
				va_start(arguments, format);
				int const written =
					_vsnprintf_s(data_ + offset_, capacity_ - offset_, _TRUNCATE, format, arguments);
				va_end(arguments);
				offset_ = written < 0 ? capacity_ - 1 : offset_ + static_cast<size_t>(written);
			}

			char const *Text() const { return data_; }

		private:
			char *data_;
			size_t capacity_;
			size_t offset_ = 0;
		};

		// Component-wise bounds over a vertex attribute array. The projector's
		// whole argument is those numbers, so the bounds are what the pole and
		// the network piece have to be compared on.
		void AppendAttributeBounds(TextBuilder &builder, char const *label, void const *data, uint32_t count,
		                           size_t stride, uint32_t components) {
			if (data == nullptr || count == 0) {
				builder.Append(" %s=none", label);
				return;
			}
			float minimum[3] = {0, 0, 0};
			float maximum[3] = {0, 0, 0};
			uint32_t read = 0;
			SafeScanAttribute(data, count, stride, components, minimum, maximum, &read);
			if (read == 0) {
				builder.Append(" %s=unreadable", label);
			} else if (components == 2) {
				builder.Append(" %s=%u[%g,%g..%g,%g]", label, read, minimum[0], minimum[1], maximum[0],
				               maximum[1]);
			} else {
				builder.Append(" %s=%u[%g,%g,%g..%g,%g,%g]", label, read, minimum[0], minimum[1], minimum[2],
				               maximum[0], maximum[1], maximum[2]);
			}
		}

		// ------------------------------------------------------------------
		// Trace file
		// ------------------------------------------------------------------

		std::filesystem::path TracePath() {
			HMODULE module = nullptr;
			wchar_t path[MAX_PATH]{};
			if (!GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
			                        GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
			                        reinterpret_cast<LPCWSTR>(&TracePath), &module) ||
			    GetModuleFileNameW(module, path, MAX_PATH) == 0) {
				return std::filesystem::path(L"SC4ShadowDiag.log");
			}
			return std::filesystem::path(path).parent_path().parent_path() / L"SC4ShadowDiag.log";
		}

		// True when this exact call shape has not been seen before, which keeps
		// the trace readable: one viaduct contributes a handful of lines rather
		// than one per redraw. -NativeShadowDiag:all turns it off.
		bool FirstSighting(uint64_t signature) {
			if (!gDeduplicate) return true;
			if (signature == 0) signature = 1;
			size_t index = static_cast<size_t>((signature * 0x9E3779B97F4A7C15ull) >> 48) & (kSignatureSlots - 1);
			for (size_t probe = 0; probe < kSignatureSlots; ++probe) {
				uint64_t const occupied = gSignatures[index];
				if (occupied == signature) return false;
				if (occupied == 0) {
					// Stop inserting once the table is three-quarters full so
					// probing stays short; later shapes are simply all logged.
					if (gSignatureCount * 4 < kSignatureSlots * 3) {
						gSignatures[index] = signature;
						++gSignatureCount;
					}
					return true;
				}
				index = (index + 1) & (kSignatureSlots - 1);
			}
			return true;
		}

		// Decides whether a record is wanted before it is formatted. AddShadow
		// runs for every mesh of every shadowed occupant on every redraw, and
		// formatting one record scans the whole vertex array, so the repeats
		// have to be discarded before that work happens.
		bool ClaimRecord(Category category, uint64_t signature) {
			std::lock_guard<std::mutex> const lock(gTraceMutex);
			if (gTrace == nullptr) return false;
			if (!FirstSighting(signature)) return false;
			if (gRecordsWritten >= gRecordBudget) {
				if (gRecordsSuppressed == 0) {
					std::fputs("# record budget reached, further records suppressed\n", gTrace);
					std::fflush(gTrace);
				}
				++gRecordsSuppressed;
				return false;
			}
			++gRecordsWritten;
			++gCounts[category];
			return true;
		}

		void EmitRecord(char const *text) {
			std::lock_guard<std::mutex> const lock(gTraceMutex);
			if (gTrace == nullptr) return;
			std::fputs(text, gTrace);
			std::fputc('\n', gTrace);
			// Flushed per record so an in-game crash still leaves the evidence.
			std::fflush(gTrace);
		}

		void WriteRecord(Category category, uint64_t signature, char const *text) {
			if (ClaimRecord(category, signature)) EmitRecord(text);
		}

		// ------------------------------------------------------------------
		// Per-thread correlation state
		// ------------------------------------------------------------------

		struct OccupantContext {
			bool valid = false;
			uint32_t sequence = 0;
			void *occupant = nullptr;
			void *occupantVTable = nullptr;
			uint32_t occupantType = 0;
			int32_t zoom = 0;
			int32_t rotation = 0;
			uint32_t groundModelFlag = 0;
			uint32_t keyType = 0;
			uint32_t keyGroup = 0;
			uint32_t keyInstance = 0;
			void *model = nullptr;
			uint32_t caller = 0;
		};

		thread_local OccupantContext tOccupant;
		thread_local char tPendingAddShadow[1536];
		thread_local bool tPendingAddShadowValid = false;
		thread_local char tPendingMap[512];
		thread_local uint64_t tPendingMapSignature = 0;
		thread_local bool tPendingMapValid = false;
		thread_local uint32_t tNetworkSequence = 0;

		uint32_t NextSequence() {
			static volatile LONG counter = 0;
			return static_cast<uint32_t>(InterlockedIncrement(&counter));
		}

		uint64_t MixSignature(std::initializer_list<uint64_t> parts) {
			uint64_t hash = 0xCBF29CE484222325ull;
			for (uint64_t const part: parts) {
				hash = (hash ^ part) * 0x100000001B3ull;
			}
			return hash;
		}

		volatile LONG gSunDirectionBits[3]{};
		volatile LONG gSunDirectionCaptured = 0;

		// Called from a float-safe hook, not from the detour itself.
		void EmitSunDirectionIfCaptured() {
			if (gSunDirectionCaptured == 0) return;
			LONG const raw[3] = {gSunDirectionBits[0], gSunDirectionBits[1], gSunDirectionBits[2]};
			float direction[3]{};
			std::memcpy(direction, raw, sizeof(direction));
			char storage[256]{};
			TextBuilder builder(storage, sizeof(storage));
			builder.Append("sundir dir=[%.9g,%.9g,%.9g] bits=[%08X,%08X,%08X]", direction[0], direction[1],
			               direction[2], raw[0], raw[1], raw[2]);
			WriteRecord(kSunDirection,
			            MixSignature({static_cast<uint64_t>(static_cast<uint32_t>(raw[0])),
			                          static_cast<uint64_t>(static_cast<uint32_t>(raw[1])),
			                          static_cast<uint64_t>(static_cast<uint32_t>(raw[2])), 0xE5}),
			            builder.Text());
		}

		void EmitNetworkShadowRecord(void *self, uint32_t on, uint32_t sequence, int32_t firstBefore,
		                             int32_t secondBefore) {
			int32_t const firstAfter =
				static_cast<int32_t>(ReadDword(OffsetOf(self, kNetworkFirstDecal), 0xCCCCCCCC));
			int32_t const secondAfter =
				static_cast<int32_t>(ReadDword(OffsetOf(self, kNetworkSecondDecal), 0xCCCCCCCC));
			uint32_t const classification0 = ReadDword(OffsetOf(self, kNetworkClassification));
			uint32_t const classification1 = ReadDword(OffsetOf(self, kNetworkClassification + 4));
			uint32_t const classification2 = ReadDword(OffsetOf(self, kNetworkClassification + 8));
			uint32_t const classification3 = ReadDword(OffsetOf(self, kNetworkClassification + 12));
			uint32_t const vtable = ReadDword(self);

			char storage[512]{};
			TextBuilder builder(storage, sizeof(storage));
			builder.Append("netshadow seq=%u this=%p vtbl=0x%08X on=%u quality=%d", sequence, self, vtable, on,
			               ShadowQuality());
			builder.Append(" w130=0x%08X w134=0x%08X w138=0x%08X w13c=0x%08X", classification0,
			               classification1, classification2, classification3);
			float position[3]{};
			std::memcpy(position, &classification0, sizeof(position));
			builder.Append(" at=[%g,%g,%g]", position[0], position[1], position[2]);
			builder.Append(" decal0=%d->%d decal1=%d->%d", firstBefore, firstAfter, secondBefore, secondAfter);
			WriteRecord(kNetworkShadow,
			            MixSignature({vtable, classification2, classification3, on,
			                          static_cast<uint64_t>(firstAfter < 0 ? firstAfter : 0), 0xD4}),
			            builder.Text());
		}

	} // namespace

	// ----------------------------------------------------------------------
	// Hook bodies. extern "C" so the naked stubs can call them by plain name.
	// ----------------------------------------------------------------------

	extern "C" void __cdecl SCD3D11_ShadowNoteResolve(uint32_t const *arguments, void const *frameBase,
	                                                  void *model) {
		OccupantContext &context = tOccupant;
		context = OccupantContext{};
		context.sequence = NextSequence();
		context.occupant = reinterpret_cast<void *>(arguments[0]);
		context.zoom = static_cast<int32_t>(arguments[1]);
		context.rotation = static_cast<int32_t>(arguments[2]);
		context.groundModelFlag = arguments[4];
		context.model = model;
		context.caller = ReadDword(OffsetOf(frameBase, kFrameReturnAddress));

		uint32_t resolved[3]{};
		if (ReadStruct(reinterpret_cast<void const *>(arguments[5]), resolved)) {
			context.keyType = resolved[0];
			context.keyGroup = resolved[1];
			context.keyInstance = resolved[2];
		}
		if (context.occupant != nullptr) {
			context.occupantVTable = reinterpret_cast<void *>(ReadDword(context.occupant));
			context.occupantType = SafeOccupantType(context.occupant);
		}
		context.valid = true;

		char storage[512]{};
		TextBuilder builder(storage, sizeof(storage));
		builder.Append("occupant seq=%u occ=%p vtbl=%p type=0x%08X", context.sequence, context.occupant,
		               context.occupantVTable, context.occupantType);
		builder.Append(" zoom=%d rot=%d flag=%u key=%08X-%08X-%08X", context.zoom, context.rotation,
		               context.groundModelFlag, context.keyType, context.keyGroup, context.keyInstance);
		builder.Append(" model=%p caller=0x%08X%s", context.model, context.caller,
		               context.model == nullptr ? " MISS(zoom fallback)" : "");
		WriteRecord(kOccupant,
		            MixSignature({reinterpret_cast<uintptr_t>(context.occupantVTable), context.keyInstance,
		                          static_cast<uint64_t>(context.zoom), static_cast<uint64_t>(context.rotation),
		                          context.model == nullptr ? 1ull : 0ull, 0xA1}),
		            builder.Text());
	}

	extern "C" void __cdecl SCD3D11_ShadowNoteAddShadowCall(uint32_t const *stackArguments,
	                                                        void *overlayManager, void *binding) {
		tPendingAddShadowValid = false;
		void const *const frameBase =
			static_cast<uint8_t const *>(static_cast<void const *>(stackArguments)) + kAddShadowArgumentBytes;

		uint32_t const vertexCount = stackArguments[0];
		void const *const positions = reinterpret_cast<void const *>(stackArguments[1]);
		void const *const uvs = reinterpret_cast<void const *>(stackArguments[2]);
		void const *const boundingBox = reinterpret_cast<void const *>(stackArguments[3]);
		void const *const transform = reinterpret_cast<void const *>(stackArguments[4]);

		OccupantContext const &context = tOccupant;
		uint32_t const meshIndex = ReadDword(OffsetOf(frameBase, kFrameMeshIndex));

		uint64_t const signature = MixSignature(
			{reinterpret_cast<uintptr_t>(context.occupantVTable), context.keyInstance,
			 static_cast<uint64_t>(context.zoom), static_cast<uint64_t>(context.rotation), meshIndex, 0xB2});
		if (!ClaimRecord(kAddShadow, signature)) return;

		// Which of the two candidate boxes the ShadowQuality >= 5 switch picked.
		char const *boxSource = "?";
		if (boundingBox == OffsetOf(frameBase, kFrameLowBoundingBox)) boxSource = "low";
		else if (boundingBox == OffsetOf(frameBase, kFrameHighBoundingBox)) boxSource = "high";
		bool const transformIsFrameSlot = transform == OffsetOf(frameBase, kFrameTransform);

		TextBuilder builder(tPendingAddShadow, sizeof(tPendingAddShadow));
		builder.Append("addshadow seq=%u mesh=%u verts=%u quality=%d", context.valid ? context.sequence : 0u,
		               meshIndex, vertexCount, ShadowQuality());
		builder.Append(" overlay=%p binding=%p box=%p(%s) xform=%p(%s)", overlayManager, binding, boundingBox,
		               boxSource, transform, transformIsFrameSlot ? "frame" : "other");

		BoundingBox box{};
		if (ReadStruct(boundingBox, box)) {
			builder.Append(" boxv=[%g,%g,%g..%g,%g,%g]", box.minimum.x, box.minimum.y, box.minimum.z,
			               box.maximum.x, box.maximum.y, box.maximum.z);
		} else {
			builder.Append(" boxv=unreadable");
		}

		AppendAttributeBounds(builder, "pos", positions, vertexCount, sizeof(Vector3), 3);
		AppendAttributeBounds(builder, "uv", uvs, vertexCount, sizeof(Vector2), 2);

		Transform matrix{};
		if (ReadStruct(transform, matrix)) {
			builder.Append(" xf.flags=%u,%u xf.m=[%g,%g,%g,%g,%g,%g,%g,%g,%g]", matrix.flags[0], matrix.flags[1],
			               matrix.rotation[0], matrix.rotation[1], matrix.rotation[2], matrix.rotation[3],
			               matrix.rotation[4], matrix.rotation[5], matrix.rotation[6], matrix.rotation[7],
			               matrix.rotation[8]);
			builder.Append(" xf.t=[%g,%g,%g] xf.s=%g", matrix.translation[0], matrix.translation[1],
			               matrix.translation[2], matrix.scale);
		} else {
			builder.Append(" xf=unreadable");
		}

		// The texture binding's layout is not decoded yet, so the head of the
		// object is dumped raw; the FSH instance is identified offline by
		// matching it against the model's material.
		uint32_t bindingWords[12]{};
		if (ReadStruct(binding, bindingWords)) {
			builder.Append(" bind=[%08X,%08X,%08X,%08X,%08X,%08X,%08X,%08X,%08X,%08X,%08X,%08X]",
			               bindingWords[0], bindingWords[1], bindingWords[2], bindingWords[3], bindingWords[4],
			               bindingWords[5], bindingWords[6], bindingWords[7], bindingWords[8], bindingWords[9],
			               bindingWords[10], bindingWords[11]);
		} else {
			builder.Append(" bind=unreadable");
		}

		if (context.valid) {
			builder.Append(" type=0x%08X key=%08X-%08X-%08X caller=0x%08X", context.occupantType,
			               context.keyType, context.keyGroup, context.keyInstance, context.caller);
		}

		tPendingAddShadowValid = true;
	}

	extern "C" void __cdecl SCD3D11_ShadowNoteAddShadowResult(int32_t result) {
		EmitSunDirectionIfCaptured();
		if (!tPendingAddShadowValid) return;
		tPendingAddShadowValid = false;
		char storage[1600]{};
		TextBuilder builder(storage, sizeof(storage));
		builder.Append("%s decal=%d%s", tPendingAddShadow, result,
		               result == -1 ? " REJECTED(AddShadow returned -1)" : "");
		EmitRecord(builder.Text());
	}

	extern "C" void __cdecl SCD3D11_ShadowNoteMapCall(uint32_t const *stackArguments, void *mapper) {
		tPendingMapValid = false;
		uint32_t pointed[4]{};
		bool const havePointed = ReadStruct(reinterpret_cast<void const *>(stackArguments[1]), pointed);

		TextBuilder builder(tPendingMap, sizeof(tPendingMap));
		builder.Append("netmap seq=%u mapper=%p vtbl=0x%08X", tNetworkSequence, mapper, ReadDword(mapper));
		builder.Append(" a1=0x%08X a2=0x%08X a3=0x%08X", stackArguments[0], stackArguments[1],
		               stackArguments[2]);
		if (havePointed) {
			builder.Append(" a2data=[%08X,%08X,%08X,%08X]", pointed[0], pointed[1], pointed[2], pointed[3]);
		} else {
			builder.Append(" a2data=unreadable");
		}
		tPendingMapSignature = MixSignature({stackArguments[0], stackArguments[2], 0xC3});
		tPendingMapValid = true;
	}

	extern "C" void __cdecl SCD3D11_ShadowNoteMapResult(uint32_t result) {
		if (!tPendingMapValid) return;
		tPendingMapValid = false;
		char storage[640]{};
		TextBuilder builder(storage, sizeof(storage));
		builder.Append("%s -> instance=0x%08X key=%08X-%08X-%08X%s", tPendingMap, result, kShadowResourceType,
		               kShadowResourceGroup, result,
		               result == 0 ? " ABORT(no shadow texture mapping)" : "");
		WriteRecord(kNetworkMap, MixSignature({tPendingMapSignature, result}), builder.Text());
	}

	namespace {

#if defined(_MSC_VER) && defined(_M_IX86)

		// Replays the displaced MOV EAX,[0x00B43CC4] and rejoins UpdateShadow.
		// __fastcall matches __thiscall exactly for one stack argument: ECX is
		// the object and the callee's RET 0x4 cleans the argument.
		__declspec(naked) void __fastcall UpdateShadowOriginal(void * /*self*/, void * /*unused*/,
		                                                       uint32_t /*on*/) {
			__asm {
				mov eax, dword ptr [gRenderPropertiesSlot]
				mov eax, dword ptr [eax]
				jmp dword ptr [gUpdateShadowRejoin]
			}
		}

		void __fastcall UpdateShadowDetour(void *self, void *unused, uint32_t on) {
			uint32_t const sequence = NextSequence();
			uint32_t const previous = tNetworkSequence;
			tNetworkSequence = sequence;
			int32_t const firstBefore =
				static_cast<int32_t>(ReadDword(OffsetOf(self, kNetworkFirstDecal), 0xCCCCCCCC));
			int32_t const secondBefore =
				static_cast<int32_t>(ReadDword(OffsetOf(self, kNetworkSecondDecal), 0xCCCCCCCC));

			UpdateShadowOriginal(self, unused, on);

			EmitNetworkShadowRecord(self, on, sequence, firstBefore, secondBefore);
			tNetworkSequence = previous;
		}

		// Replays MOV EDX,[ECX+0x64] / MOV EAX,[ESP+4] and rejoins. The routine
		// leaves the out pointer in EAX at its RET 0x4 - the usual "return the
		// filled struct" convention - and CalcShadowProjection relies on it, so
		// both this thunk and the detour have to carry that value through.
		__declspec(naked) float *__fastcall GetShadowDirectionOriginal(void * /*self*/, void * /*unused*/,
		                                                               float * /*out*/) {
			__asm {
				mov edx, dword ptr [ecx + 0x64]
				mov eax, dword ptr [esp + 4]
				jmp dword ptr [gGetShadowDirectionRejoin]
			}
		}

		// GetShadowDirection is called from inside CalcShadowProjection, which
		// keeps values live on the x87 stack across the call. Formatting a float
		// here pushes onto that stack and corrupts the caller: it crashed the game
		// at 0x007D8E3C. The detour therefore copies raw bit patterns with integer
		// moves only, and the record is emitted later from the AddShadow hook,
		// which is a context that already formats floats safely.
		float *__fastcall GetShadowDirectionDetour(void *self, void *unused, float *out) {
			float *const result = GetShadowDirectionOriginal(self, unused, out);
			if (out != nullptr) {
				// Integer moves only: the caller keeps values on the x87 stack
				// across this call, so the record is formatted elsewhere.
				LONG const *const bits = reinterpret_cast<LONG const *>(out);
				gSunDirectionBits[0] = bits[0];
				gSunDirectionBits[1] = bits[1];
				gSunDirectionBits[2] = bits[2];
				gSunDirectionCaptured = 1;
			}
			return result;
		}

		// Replaces CALL 0x00497180 at 0x0049147C. The resolver is __cdecl with
		// seven caller-cleaned arguments, so the thunk forwards them by value
		// and leaves the caller's ADD ESP,0x1C to do the cleanup. The resource
		// key is filled in place, which is why the record is taken on the way
		// out rather than on the way in.
		__declspec(naked) void ResolveModelThunk() {
			__asm {
				push dword ptr [esp + 0x1c] // argument 7
				push dword ptr [esp + 0x1c] // argument 6, and so on down to 1
				push dword ptr [esp + 0x1c]
				push dword ptr [esp + 0x1c]
				push dword ptr [esp + 0x1c]
				push dword ptr [esp + 0x1c]
				push dword ptr [esp + 0x1c]
				call dword ptr [gResolveModelTarget]
				add  esp, 0x1c

				pushad
				pushfd
				push eax                   // model
				lea  eax, [esp + 0x48]     // B, the CreateOccupantShadow frame
				push eax
				lea  eax, [esp + 0x30]     // the seven arguments
				push eax
				call SCD3D11_ShadowNoteResolve
				add  esp, 0xc
				popfd
				popad                      // restores EAX, the resolved model
				ret
			}
		}

		// Replaces MOV ECX,EBP / PUSH EDI / CALL [EDX+0x2C] at 0x0049195E.
		__declspec(naked) void AddShadowSiteStub() {
			__asm {
				pushad
				pushfd
				push edi                   // cS3DTextureBinding*
				push ebp                   // overlay manager
				lea  eax, [esp + 0x2c]     // the five pushed arguments
				push eax
				call SCD3D11_ShadowNoteAddShadowCall
				add  esp, 0xc
				popfd
				popad

				mov  ecx, ebp
				push edi
				call dword ptr [edx + 0x2c]

				pushfd
				pushad
				push eax                   // decal id, or -1
				call SCD3D11_ShadowNoteAddShadowResult
				add  esp, 4
				popad                      // restores EAX, the decal id
				popfd
				jmp  dword ptr [gAddShadowRejoin]
			}
		}

		// Replaces MOV ECX,EDI / CALL [EBX+0x38] at 0x0061E584.
		__declspec(naked) void MapShadowTextureSiteStub() {
			__asm {
				pushad
				pushfd
				push edi                   // the mapping object
				lea  eax, [esp + 0x28]     // the three pushed arguments
				push eax
				call SCD3D11_ShadowNoteMapCall
				add  esp, 8
				popfd
				popad

				mov  ecx, edi
				call dword ptr [ebx + 0x38]

				pushfd
				pushad
				push eax                   // shadow texture instance, or 0
				call SCD3D11_ShadowNoteMapResult
				add  esp, 4
				popad
				popfd
				jmp  dword ptr [gMapShadowTextureRejoin]
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

		// A five-byte E8 rel32 whose displacement is retargeted; the original
		// callee is reached through gResolveModelTarget instead.
		void ConfigureCallSite(PatchSite &site, uintptr_t virtualAddress, std::initializer_list<uint8_t> expected,
		                       uint8_t *target, void *destination) {
			ConfigureSite(site, virtualAddress, expected);
			site.replacement[0] = 0xE8;
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

		bool ReadOptions() {
			char const *const commandLine = GetCommandLineA();
			char const *argument = std::strstr(commandLine, "-NativeShadowDiag");
			if (argument == nullptr) return false;
			argument += sizeof("-NativeShadowDiag") - 1;
			gRecordBudget = kDefaultRecordBudget;
			gDeduplicate = true;
			if (*argument != ':') {
				return *argument == '\0' || *argument == ' ' || *argument == '\t';
			}
			++argument;
			if (_strnicmp(argument, "all", 3) == 0) {
				gRecordBudget = 1000000;
				gDeduplicate = false;
				return true;
			}
			unsigned long const budget = std::strtoul(argument, nullptr, 10);
			if (budget != 0) gRecordBudget = static_cast<unsigned>(budget);
			return true;
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
			// Do not overwrite a later patch that replaced this hook while the
			// process was running.
			if (std::memcmp(site.address, site.replacement.data(), site.length) != 0) {
				Log(LogCategory::Initialization,
				    "native shadow diagnostics: not restoring modified site 0x%08lX",
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
		if (!ReadOptions()) return false;
		if (!IsSupportedGameVersion()) {
			Log(LogCategory::Initialization,
			    "native shadow diagnostics: requires SimCity 4 1.1.641, skipping");
			return false;
		}

		uint8_t *const module = reinterpret_cast<uint8_t *>(GetModuleHandleW(nullptr));
		if (module == nullptr) return false;
		auto const resolve = [module](uintptr_t virtualAddress) {
			return module + (virtualAddress - kImageBase);
		};

		gAddShadowRejoin = reinterpret_cast<uintptr_t>(resolve(kAddShadowRejoinVA));
		gUpdateShadowRejoin = reinterpret_cast<uintptr_t>(resolve(kUpdateShadowRejoinVA));
		gMapShadowTextureRejoin = reinterpret_cast<uintptr_t>(resolve(kMapShadowTextureRejoinVA));
		gGetShadowDirectionRejoin = reinterpret_cast<uintptr_t>(resolve(kGetShadowDirectionRejoinVA));
		gRenderPropertiesSlot = reinterpret_cast<uintptr_t>(resolve(kRenderPropertiesSlotVA));
		gResolveModelTarget = reinterpret_cast<uintptr_t>(resolve(kResolveModelTargetVA));

		ConfigureCallSite(gResolveModelCall, kResolveModelCallVA, {0xE8, 0xFF, 0x5C, 0x00, 0x00},
		                  resolve(kResolveModelCallVA), &ResolveModelThunk);
		ConfigureJumpSite(gAddShadowSite, kAddShadowSiteVA, {0x8B, 0xCD, 0x57, 0xFF, 0x52, 0x2C},
		                  resolve(kAddShadowSiteVA), &AddShadowSiteStub);
		ConfigureJumpSite(gUpdateShadow, kUpdateShadowVA, {0xA1, 0xC4, 0x3C, 0xB4, 0x00},
		                  resolve(kUpdateShadowVA), &UpdateShadowDetour);
		ConfigureJumpSite(gMapShadowTextureSite, kMapShadowTextureSiteVA, {0x8B, 0xCF, 0xFF, 0x53, 0x38},
		                  resolve(kMapShadowTextureSiteVA), &MapShadowTextureSiteStub);
		ConfigureJumpSite(gGetShadowDirection, kGetShadowDirectionVA,
		                  {0x8B, 0x51, 0x64, 0x8B, 0x44, 0x24, 0x04}, resolve(kGetShadowDirectionVA),
		                  &GetShadowDirectionDetour);

		PatchSite *const sites[] = {&gResolveModelCall, &gAddShadowSite, &gUpdateShadow,
		                            &gMapShadowTextureSite, &gGetShadowDirection};
		for (PatchSite *site: sites) {
			site->address = resolve(site->virtualAddress);
		}
		for (PatchSite const *site: sites) {
			if (!BytesMatch(*site)) {
				Log(LogCategory::Initialization,
				    "native shadow diagnostics: byte guard failed at 0x%08lX, skipping",
				    static_cast<unsigned long>(site->virtualAddress));
				return false;
			}
		}

		std::filesystem::path const path = TracePath();
		// _SH_DENYWR rather than _wfopen_s, so the trace can be tailed while
		// the game is running.
		gTrace = _wfsopen(path.c_str(), L"w", _SH_DENYWR);
		if (gTrace == nullptr) {
			gTrace = nullptr;
			Log(LogCategory::Initialization, "native shadow diagnostics: cannot open the trace file");
			return false;
		}
		std::fputs("# SC4 native shadow diagnostics (read-only), SimCity 4 1.1.641\n"
		           "# occupant  CreateOccupantShadow model resolve, 0x0049147C\n"
		           "# addshadow cSTEOverlayManager::AddShadow call site, 0x0049195E\n"
		           "# netshadow cSC4NetworkOccupantWithPreBuiltModel::UpdateShadow, 0x0061E340\n"
		           "# netmap    MapShadowTexture call site, 0x0061E584\n",
		           gTrace);
		std::fflush(gTrace);

		for (PatchSite *site: sites) {
			if (!WriteSite(*site)) {
				Log(LogCategory::Initialization,
				    "native shadow diagnostics: failed to write site 0x%08lX, restoring",
				    static_cast<unsigned long>(site->virtualAddress));
				for (PatchSite *written: sites) RestoreSite(*written);
				std::fclose(gTrace);
				gTrace = nullptr;
				return false;
			}
		}

		gInstalled = true;
		Log(LogCategory::Initialization,
		    "native shadow diagnostics installed: budget %u records, dedup %s", gRecordBudget,
		    gDeduplicate ? "on" : "off");
		return true;
#else
		return false;
#endif
	}

	void Uninstall() {
		if (!gInstalled) return;
		EmitSunDirectionIfCaptured();
		RestoreSite(gGetShadowDirection);
		RestoreSite(gMapShadowTextureSite);
		RestoreSite(gUpdateShadow);
		RestoreSite(gAddShadowSite);
		RestoreSite(gResolveModelCall);
		gInstalled = false;

		std::lock_guard<std::mutex> const lock(gTraceMutex);
		if (gTrace != nullptr) {
			std::fprintf(gTrace,
			             "# totals: occupant=%u addshadow=%u netshadow=%u netmap=%u written=%u suppressed=%u\n",
			             gCounts[kOccupant], gCounts[kAddShadow], gCounts[kNetworkShadow], gCounts[kNetworkMap],
			             gRecordsWritten, gRecordsSuppressed);
			std::fclose(gTrace);
			gTrace = nullptr;
		}
		Log(LogCategory::Initialization, "native shadow diagnostics uninstalled");
	}

} // namespace scvk::NativeShadowDiagnostics
