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
 * Live shadows for prebuilt network pieces and True3D props.
 *
 * SimCity 4 Deluxe 1.1.641, Windows x86, image base 0x00400000. Opt-in with
 * -LiveShadows:on. Every site is guarded by its exact bytes and restored during
 * PreAppShutdown. The game decides which draws are casters; cVKDriver captures their
 * indexed, alpha-tested triangles and renders them into its own shadow map.
 *
 * Props. The game never shadows a prop without Is Ground Model (0x8A5E5DB8):
 *
 *   C 0x0049134B   5 bytes  the Is Ground Model property test
 *   D 0x0049195E   6 bytes  the cSTEOverlayManager::AddShadow call
 *
 * Site C lets such a prop through CreateOccupantShadow and remembers it. Site D lets its
 * AddShadow call run for ShadowRegistry, which keeps the mesh with its own coordinates.
 * Without the registry, site D records a signature of the position and coordinate streams
 * instead and skips the call, whose single affine projector draws tall True3D geometry as
 * filled slabs; cVKDriver then hashes each draw the same way to recognise the prop.
 *
 * Prebuilt network pieces. Pieces that pass UpdateShadow's HasNetworkFlag(1) &&
 * HasNetworkFlag(4) && quality test get the game's flat shadow decal from an FSH at
 * {0x7AB50E44, 0x2BC2759A, base + zoom}, and the NAM ships thousands. They cast live
 * instead, so the decal would only darken the same ground twice:
 *
 *   A 0x0061E35C  35 bytes  the test, replaced by a jump to its reject branch (XOR BL,BL
 *                           at 0x0061E383), so no decal is mapped; left alone with
 *                           -NetworkShadowMasks:on
 *
 * A piece is two entries in the view's sorted drawable list, each drawn through virtual
 * slot +0x30 by DrawStaticView_ (0x007C7370):
 *
 *   E 0x0061E860   5 bytes  cSC4NetworkOccupantWithPreBuiltModel::Draw wrapper
 *   F 0x0061E8A0   8 bytes  cSC4NetworkOccupantWithPreBuiltModel::GetModelInstances
 *   G 0x006147D0  11 bytes  the cS3DModelInstance drawable thunk (vtable +0x30)
 *
 * - The occupant itself, whose Draw (site E, to 0x0061A850) renders only the network's
 *   flat textured quads. 0x0061E860 is __thiscall with two arguments, RET 0x8. Bridge
 *   occupants (class 0x49CC1BCD) route their Draw through the same wrapper by the thunk at
 *   0x00605320, so one bracket covers prebuilt and bridge puzzle pieces alike.
 * - The piece's model, a cS3DModelInstance that GetModelInstances creates from the RKT0
 *   key {0x5AD0E817, 0xBADB57F1} and keeps at occupant +0x1C4 and +0x1C8. It is drawn
 *   through the thunk at 0x006147D0, then 0x00801750, RenderModelInstance and RenderMesh,
 *   which is where cVKDriver finally sees the triangles.
 *
 * Site F records the two model instances an occupant owns, and site G brackets the draw
 * of a recorded instance. Ownership is checked against occupant +0x1C4 and +0x1C8 on every
 * hit, so an instance that was freed and whose address was reused cannot pose as a
 * caster: a live occupant holds a reference to its own instance, so a matching slot means
 * the entry is still real.
 */

//// Dependencies

#include "LiveShadowHooks.h"
#include "GamePatch.h"
#include "ModuleLog.h"
#include "SC4Version.h"
#include "Settings.h"
#include "ShadowRegistry.h"

#include <windows.h>
#include <algorithm>
#include <atomic>
#include <cmath>
#include <mutex>
#include <shared_mutex>
#include <string.h>
#include <unordered_map>
#include <vector>

namespace scvk::LiveShadowHooks
{
	//// Types

	/**
	 * AddShadow's arguments as site D finds them on the stack, the binding in EDI aside.
	 * Outside the anonymous namespace, since the stub's C function takes it.
	 */
	struct AddShadowArguments
	{
		uint32_t    count;
		void const* positions;
		void const* coordinates;
		void const* box;
		void const* transform;
	};

	static_assert(sizeof(AddShadowArguments) == 0x14, "the five arguments AddShadow takes from the stack");

	//// Constants

	namespace
	{
		constexpr uint16_t SUPPORTED_GAME_VERSION = 641;

		// Site A, the network shadow mask test, and the branch where it rejects a piece
		constexpr uintptr_t NETWORK_GATE_ADDRESS        = 0x0061E35C;
		constexpr uintptr_t NETWORK_GATE_REJECT_ADDRESS = 0x0061E383;

		// Sites E, F and G, with the addresses their trampolines carry on at
		constexpr uintptr_t PREBUILT_DRAW_ADDRESS             = 0x0061E860;
		constexpr uintptr_t PREBUILT_DRAW_REJOIN_ADDRESS      = 0x0061E865;
		constexpr uintptr_t PREBUILT_MODELS_ADDRESS           = 0x0061E8A0;
		constexpr uintptr_t PREBUILT_MODELS_REJOIN_ADDRESS    = 0x0061E8A8;
		constexpr uintptr_t MODEL_INSTANCE_DRAW_THUNK_ADDRESS = 0x006147D0;
		constexpr uintptr_t MODEL_INSTANCE_DRAW_ADDRESS       = 0x00801750;

		// The two cS3DModelInstance interface pointers a prebuilt piece owns
		constexpr ptrdiff_t PREBUILT_MODEL_SLOTS[] = { 0x1C4, 0x1C8 };

		// Site C, the render properties the displaced load reads, and the property getter
		// the stub calls in its place
		constexpr uintptr_t RENDER_PROPERTIES_POINTER_ADDRESS = 0x00B43CC4;
		constexpr uintptr_t PROP_GROUND_MODEL_ADDRESS         = 0x0049134B;
		constexpr uintptr_t PROP_GROUND_MODEL_REJOIN_ADDRESS  = 0x0049136A;
		constexpr uintptr_t GET_BOOL_PROPERTY_ADDRESS         = 0x005FD390;

		// Site D. CreateOccupantShadow's occupant lies this far into its frame, which starts
		// right above AddShadow's stack arguments.
		constexpr uintptr_t ADD_SHADOW_CALL_ADDRESS   = 0x0049195E;
		constexpr uintptr_t ADD_SHADOW_REJOIN_ADDRESS = 0x00491964;
		constexpr ptrdiff_t FRAME_OCCUPANT            = 0xE0;

		// What the site D stub does with the AddShadow call, as ScvkLiveShadowRegisterProp
		// answers: let it run, or skip it as though it had failed. The stub compares with the
		// literal 2, so the two must change together.
		constexpr uint32_t ADD_SHADOW_RUN  = 0;
		constexpr uint32_t ADD_SHADOW_SKIP = 2;

		// The game's renderer, whose slot +0x24 returns the lighting manager (DrawShadows,
		// 0x00736C49). The lighting manager's table 0x00ABB530 has GetShadowDirection
		// (0x007D6B50) at +0x34.
		constexpr uintptr_t RENDERER_POINTER_ADDRESS       = 0x00B43DD0;
		constexpr size_t    RENDERER_LIGHTING_SLOT         = 0x24 / sizeof(void*);
		constexpr size_t    LIGHTING_SHADOW_DIRECTION_SLOT = 0x34 / sizeof(void*);

		// SetTimeOfDay (0x007D9DF0) writes max((lighting red - 0.8) / 0.2, 0) here and sends
		// it as the daylight transition parameter
		constexpr ptrdiff_t LIGHTING_DAYLIGHT = 0xBC;

		// GetShadowParams (0x007D6B70) only copies these globals out
		constexpr uintptr_t SHADOW_STRENGTH_ADDRESS = 0x00B0DC78;
		constexpr uintptr_t SHADOW_COLOUR_ADDRESS   = 0x00B4E34C;

		// The fewest and most vertices a prop mesh can have. Anything outside is not a mesh.
		constexpr uint32_t MINIMUM_PROP_VERTICES = 3;
		constexpr uint32_t MAXIMUM_PROP_VERTICES = 0x100000;

		// Entries are only replaced, never erased, because nothing cheaply says when an
		// occupant dies. The cap keeps a long session from growing the map without bound;
		// the ownership check is what keeps stale entries harmless.
		constexpr size_t MAXIMUM_LIVE_MODEL_INSTANCES = size_t{ 1 } << 16;

		// FNV-1a over 32-bit words, as cVKDriver hashes a draw
		constexpr uint64_t HASH_OFFSET = 0xCBF29CE484222325ull;
		constexpr uint64_t HASH_PRIME  = 0x100000001B3ull;
	}

	//// State

	namespace
	{
		GamePatch networkGate;
		GamePatch prebuiltDraw;
		GamePatch prebuiltModels;
		GamePatch modelInstanceDraw;
		GamePatch propGroundModel;
		GamePatch addShadowCall;

		bool isInstalled = false;

		// Where the stubs jump and call, in this process. The stubs read them as memory
		// operands, so they stay plain variables.
		uint8_t* prebuiltDrawRejoin      = nullptr;
		uint8_t* prebuiltModelsRejoin    = nullptr;
		uint8_t* modelInstanceDrawTarget = nullptr;
		uint8_t* renderPropertiesPointer = nullptr;
		uint8_t* propGroundModelRejoin   = nullptr;
		uint8_t* addShadowRejoin         = nullptr;
		uint8_t* getBoolProperty         = nullptr;

		// The game's lighting
		uint8_t const* rendererPointer = nullptr;
		uint8_t const* shadowStrength  = nullptr;
		uint8_t const* shadowColour    = nullptr;

		thread_local uint32_t networkDrawDepth    = 0;
		thread_local void*    relaxedPropOccupant = nullptr;

		// Sorted signatures of the relaxed props' meshes, and whether there are any, which
		// HasPropMeshes reads without the lock on every draw the game makes
		std::mutex            propMutex;
		std::vector<uint64_t> propMeshes;
		std::atomic<bool>     isAnyPropMeshRecorded{ false };

		// Model instances owned by prebuilt network pieces, each with the occupant that owns
		// it so a hit can be checked. Read on every cS3DModelInstance draw and written only
		// when a piece loads its models, so a shared lock fits, and the count keeps the
		// common case of nothing recorded off the lock entirely.
		std::shared_mutex                            liveModelMutex;
		std::unordered_map<void const*, void const*> liveModelInstances;
		std::atomic<size_t>                          liveModelInstanceCount{ 0 };

		// Counts for the log at shutdown
		unsigned networkDraws          = 0;
		unsigned liveModelsRegistered  = 0;
		unsigned liveModelDraws        = 0;
		unsigned liveModelsRejected    = 0;
		unsigned propMeshesRegistered  = 0;
		unsigned propCallsSuppressed   = 0;
	}

	//// Private Functions

	namespace
	{
#if defined(_MSC_VER) && defined(_M_IX86)
		/** The game's lighting manager, or null before the renderer exists. */
		void* FindLighting(void)
		{
			void* renderer = nullptr;
			if (!ReadGameMemory(rendererPointer, renderer) || renderer == nullptr)
			{
				return nullptr;
			}

			void* const function = ReadVirtualFunction(renderer, RENDERER_LIGHTING_SLOT);
			if (function == nullptr)
			{
				return nullptr;
			}

			// The slot holds the renderer's GetLighting, a __thiscall taking nothing else.
			// The call is guarded in case the renderer is being torn down.
			using GetLighting = void* (__thiscall*)(void*);
			__try
			{
				return reinterpret_cast<GetLighting>(function)(renderer);
			}
			__except (EXCEPTION_EXECUTE_HANDLER)
			{
				return nullptr;
			}
		}

		bool ReadShadowDirection(void* lighting, float direction[3])
		{
			void* const function = ReadVirtualFunction(lighting, LIGHTING_SHADOW_DIRECTION_SLOT);
			if (function == nullptr)
			{
				return false;
			}

			// GetShadowDirection fills the vector it is handed and returns it
			using GetDirection = float* (__thiscall*)(void*, float*);
			__try
			{
				reinterpret_cast<GetDirection>(function)(lighting, direction);
				return true;
			}
			__except (EXCEPTION_EXECUTE_HANDLER)
			{
				return false;
			}
		}
#else
		void* FindLighting(void)
		{
			return nullptr;
		}

		bool ReadShadowDirection(void*, float*)
		{
			return false;
		}
#endif

		void HashWord(uint64_t& hash, uint32_t word)
		{
			hash = (hash ^ word) * HASH_PRIME;
		}

		void RecordPropMesh(uint32_t count, void const* positionStream, void const* coordinateStream)
		{
			// Copy both streams out of the game, all or nothing
			std::vector<float> positions(size_t{ count } * 3);
			std::vector<float> coordinates(size_t{ count } * 2);

			if (!CopyGameMemory(positionStream, positions.data(), positions.size() * sizeof(float)) || !CopyGameMemory(coordinateStream, coordinates.data(), coordinates.size() * sizeof(float)))
			{
				return;
			}

			// Hash each vertex's position and coordinate as five words, as the draw is hashed
			uint64_t signature = HASH_OFFSET;
			HashWord(signature, count);

			for (uint32_t index = 0; index < count; index++)
			{
				uint32_t words[5] = {};
				memcpy(words, positions.data() + size_t{ index } * 3, sizeof(float) * 3);
				memcpy(words + 3, coordinates.data() + size_t{ index } * 2, sizeof(float) * 2);

				for (uint32_t word : words)
				{
					HashWord(signature, word);
				}
			}

			std::lock_guard<std::mutex> lock(propMutex);
			auto const found = std::lower_bound(propMeshes.begin(), propMeshes.end(), signature);
			if (found != propMeshes.end() && *found == signature)
			{
				return;
			}

			propMeshes.insert(found, signature);
			isAnyPropMeshRecorded.store(true, std::memory_order_relaxed);
			propMeshesRegistered++;

			Log(LogCategory::Initialization, "live shadows: registered prop mesh sig=%08X%08X verts=%u", static_cast<unsigned>(signature >> 32), static_cast<unsigned>(signature & 0xFFFFFFFFu), count);
		}

		/**
		 * Records the model instances a prebuilt piece owns, read back out of the occupant
		 * once GetModelInstances has refreshed both slots.
		 */
		void RegisterPrebuiltModelInstances(void const* occupant)
		{
			if (occupant == nullptr)
			{
				return;
			}

			for (ptrdiff_t const slot : PREBUILT_MODEL_SLOTS)
			{
				void const* instance = nullptr;
				if (!ReadGameMemory(static_cast<uint8_t const*>(occupant) + slot, instance) || instance == nullptr)
				{
					continue;
				}

				std::unique_lock<std::shared_mutex> lock(liveModelMutex);
				if (liveModelInstances.size() >= MAXIMUM_LIVE_MODEL_INSTANCES)
				{
					liveModelInstances.clear();
					Log(LogCategory::Initialization, "live shadows: prebuilt model registry hit its cap, cleared");
				}

				if (liveModelInstances.insert_or_assign(instance, occupant).second)
				{
					liveModelsRegistered++;
				}

				liveModelInstanceCount.store(liveModelInstances.size(), std::memory_order_relaxed);
			}
		}

		/**
		 * Whether an instance is still one of its occupant's prebuilt models.
		 *
		 * The second half is what makes the record safe: nothing says when an instance dies,
		 * so a freed address the allocator handed to some other model would otherwise stay
		 * a caster for good. A live occupant holds a reference to its own instance, so a slot
		 * that still points here means the entry is real.
		 */
		bool IsPrebuiltModelInstance(void const* instance)
		{
			if (instance == nullptr || liveModelInstanceCount.load(std::memory_order_relaxed) == 0)
			{
				return false;
			}

			void const* occupant = nullptr;
			{
				std::shared_lock<std::shared_mutex> lock(liveModelMutex);
				auto const found = liveModelInstances.find(instance);
				if (found == liveModelInstances.end())
				{
					return false;
				}

				occupant = found->second;
			}

			for (ptrdiff_t const slot : PREBUILT_MODEL_SLOTS)
			{
				void const* current = nullptr;
				if (ReadGameMemory(static_cast<uint8_t const*>(occupant) + slot, current) && current == instance)
				{
					return true;
				}
			}

			liveModelsRejected++;
			return false;
		}

		void ClearRecords(void)
		{
			{
				std::lock_guard<std::mutex> lock(propMutex);
				propMeshes.clear();
				isAnyPropMeshRecorded.store(false, std::memory_order_relaxed);
			}

			{
				std::unique_lock<std::shared_mutex> lock(liveModelMutex);
				liveModelInstances.clear();
				liveModelInstanceCount.store(0, std::memory_order_relaxed);
			}

			networkDrawDepth = 0;
		}
	}

	//// Hook Bodies

	/** Site C's stub: remembers the occupant a relaxed prop's shadow is made for, or none. */
	extern "C" void __cdecl ScvkLiveShadowNoteRelaxedProp(void* occupant)
	{
		relaxedPropOccupant = occupant;
	}

	/**
	 * Site D's stub: the relaxed prop has reached the point where the game extracted its
	 * position and coordinate streams.
	 *
	 * With the registry the call runs and the registry keeps the mesh. Without it the mesh's
	 * signature is recorded and the call is skipped. Any other occupant's call runs as it is.
	 */
	extern "C" uint32_t __cdecl ScvkLiveShadowRegisterProp(AddShadowArguments const* arguments)
	{
		// The frame of CreateOccupantShadow starts right above the arguments, and the
		// offsets into it are in bytes
		uint8_t const* const frame = reinterpret_cast<uint8_t const*>(arguments) + sizeof(AddShadowArguments);

		void* occupant = nullptr;
		if (!ReadGameMemory(frame + FRAME_OCCUPANT, occupant) || occupant == nullptr || occupant != relaxedPropOccupant)
		{
			return ADD_SHADOW_RUN;
		}

		if (arguments->count < MINIMUM_PROP_VERTICES || arguments->count > MAXIMUM_PROP_VERTICES || arguments->positions == nullptr || arguments->coordinates == nullptr)
		{
			return ADD_SHADOW_SKIP;
		}

		if (ShadowRegistry::Enabled())
		{
			ShadowRegistry::ExpectMeshCaster();
			return ADD_SHADOW_RUN;
		}

		RecordPropMesh(arguments->count, arguments->positions, arguments->coordinates);
		propCallsSuppressed++;
		return ADD_SHADOW_SKIP;
	}

	namespace
	{
#if defined(_MSC_VER) && defined(_M_IX86)
		/**
		 * Site E's trampoline: the displaced load of the render properties, then on into the
		 * game's Draw wrapper.
		 *
		 * The wrapper refreshes UpdateShadow, then calls the ordinary network occupant
		 * renderer. Bracketing that call lets cVKDriver recognise its indexed S3D draws
		 * without letting terrain, water, the interface or other overlays into the map.
		 */
		__declspec(naked) void PrebuiltDrawOriginal(void)
		{
			__asm
			{
				push edx
				mov  edx, dword ptr [renderPropertiesPointer]
				mov  eax, dword ptr [edx]
				pop  edx
				jmp  dword ptr [prebuiltDrawRejoin]
			}
		}

		uint32_t __fastcall PrebuiltDrawHook(void* self, void*, void* drawContext, void* drawState)
		{
			// The trampoline carries on into the game's wrapper, so it is called as that
			// __thiscall function
			using Original = uint32_t (__thiscall*)(void*, void*, void*);

			networkDrawDepth++;
			uint32_t const result = reinterpret_cast<Original>(&PrebuiltDrawOriginal)(self, drawContext, drawState);
			networkDrawDepth--;

			networkDraws++;
			return result;
		}

		/**
		 * Site F's trampoline. 0x0061E8A0 is __thiscall with five stack arguments, RET 0x14.
		 * The two displaced loads read arguments off ESP, which is why the trampoline has to
		 * be entered by a call carrying the same arguments.
		 */
		__declspec(naked) void PrebuiltModelsOriginal(void)
		{
			__asm
			{
				mov eax, dword ptr [esp + 0x14]
				mov edx, dword ptr [esp + 0x0c]
				jmp dword ptr [prebuiltModelsRejoin]
			}
		}

		void __fastcall PrebuiltModelsHook(void* self, void*, int zoom, int rotation, void** instances, int capacity, int* count)
		{
			// As in PrebuiltDrawHook: the trampoline stands for GetModelInstances
			using Original = void (__thiscall*)(void*, int, int, void**, int, int*);
			reinterpret_cast<Original>(&PrebuiltModelsOriginal)(self, zoom, rotation, instances, capacity, count);

			// Read the instances back out of the occupant rather than the array, which also
			// carries whatever the base network occupant added, and +0x1C4 and +0x1C8 are the
			// slots the ownership check reads
			RegisterPrebuiltModelInstances(self);
		}

		/**
		 * Site G's trampoline. The patch replaces the whole adjusting thunk, so ECX in the
		 * hook is still the interface pointer the drawable list stores, the same value
		 * GetModelInstances handed out, and the two compare without any arithmetic. The
		 * trampoline replays the adjustment the thunk made.
		 */
		__declspec(naked) void ModelInstanceDrawOriginal(void)
		{
			__asm
			{
				sub ecx, dword ptr [ecx - 4]
				sub ecx, 0x40
				jmp dword ptr [modelInstanceDrawTarget]
			}
		}

		uint32_t __fastcall ModelInstanceDrawHook(void* self, void*, void* drawContext, uint32_t isLit)
		{
			// As in PrebuiltDrawHook: the trampoline stands for the thunk
			using Original = uint32_t (__thiscall*)(void*, void*, uint32_t);

			bool const isCaster = IsPrebuiltModelInstance(self);
			if (!isCaster)
			{
				return reinterpret_cast<Original>(&ModelInstanceDrawOriginal)(self, drawContext, isLit);
			}

			networkDrawDepth++;
			uint32_t const result = reinterpret_cast<Original>(&ModelInstanceDrawOriginal)(self, drawContext, isLit);
			networkDrawDepth--;
			liveModelDraws++;

			static bool hasLoggedFirst = false;
			if (!hasLoggedFirst)
			{
				hasLoggedFirst = true;
				Log(LogCategory::Initialization, "live shadows: first prebuilt model bracketed (instance %p)", self);
			}

			return result;
		}

		/**
		 * Site C's stub: the property test the patch displaced, which notes the occupant in
		 * ESI as a relaxed prop wherever the test fails, and carries on as though it passed.
		 */
		__declspec(naked) void PropGroundModelStub(void)
		{
			__asm
			{
				push eax
				push ecx
				push edx
				push 0
				call ScvkLiveShadowNoteRelaxedProp
				add  esp, 4
				pop  edx
				pop  ecx
				pop  eax
				lea  ecx, [esp + 0x13]
				push ecx
				push 0x8a5e5db8
				push ebp
				call dword ptr [getBoolProperty]
				add  esp, 0xc
				test al, al
				jz   relaxed
				mov  al, byte ptr [esp + 0x13]
				test al, al
				jz   relaxed
				jmp  dword ptr [propGroundModelRejoin]
			relaxed:
				push eax
				push ecx
				push edx
				push esi
				call ScvkLiveShadowNoteRelaxedProp
				add  esp, 4
				pop  edx
				pop  ecx
				pop  eax
				jmp  dword ptr [propGroundModelRejoin]
			}
		}

		/**
		 * Site D's stub: asks ScvkLiveShadowRegisterProp about the call, then makes it, or
		 * drops its five stack arguments and returns -1 as a failed AddShadow does. Its
		 * answer replaces the saved EAX, so it survives the restored registers.
		 */
		__declspec(naked) void AddShadowCallStub(void)
		{
			__asm
			{
				pushad
				pushfd
				lea  eax, [esp + 0x24]
				push eax
				call ScvkLiveShadowRegisterProp
				add  esp, 4
				mov  dword ptr [esp + 0x20], eax
				popfd
				popad
				cmp  eax, 2
				je   suppress
				mov  ecx, ebp
				push edi
				call dword ptr [edx + 0x2c]
				jmp  dword ptr [addShadowRejoin]
			suppress:
				add  esp, 0x14
				or   eax, 0xffffffff
				jmp  dword ptr [addShadowRejoin]
			}
		}
#endif
	}

	//// Public API

	bool Install(void)
	{
#if defined(_MSC_VER) && defined(_M_IX86)
		if (isInstalled)
		{
			return true;
		}

		if (!GetSettings().areLiveShadowsEnabled)
		{
			return false;
		}

		if (GetGameVersion() != SUPPORTED_GAME_VERSION)
		{
			Log(LogCategory::Initialization, "live shadows: requires SimCity 4 1.1.641, skipping");
			return false;
		}

		// Find what the stubs and reads need, wherever the game was loaded
		prebuiltDrawRejoin      = ResolveGameAddress(PREBUILT_DRAW_REJOIN_ADDRESS);
		prebuiltModelsRejoin    = ResolveGameAddress(PREBUILT_MODELS_REJOIN_ADDRESS);
		modelInstanceDrawTarget = ResolveGameAddress(MODEL_INSTANCE_DRAW_ADDRESS);
		renderPropertiesPointer = ResolveGameAddress(RENDER_PROPERTIES_POINTER_ADDRESS);
		propGroundModelRejoin   = ResolveGameAddress(PROP_GROUND_MODEL_REJOIN_ADDRESS);
		addShadowRejoin         = ResolveGameAddress(ADD_SHADOW_REJOIN_ADDRESS);
		getBoolProperty         = ResolveGameAddress(GET_BOOL_PROPERTY_ADDRESS);
		rendererPointer         = ResolveGameAddress(RENDERER_POINTER_ADDRESS);
		shadowStrength          = ResolveGameAddress(SHADOW_STRENGTH_ADDRESS);
		shadowColour            = ResolveGameAddress(SHADOW_COLOUR_ADDRESS);

		if (prebuiltDrawRejoin == nullptr)
		{
			return false;
		}

		// Prepare every site
		//
		// The displacement in the thunk's own jump does not depend on where the image was
		// loaded, so it is part of the guard.
		networkGate       = GamePatch::Jump(NETWORK_GATE_ADDRESS, { 0x6A, 0x01, 0x8B, 0xCF, 0xFF, 0x92, 0x80, 0x00, 0x00, 0x00, 0x84, 0xC0, 0x74, 0x19, 0x8B, 0x07, 0x6A, 0x04, 0x8B, 0xCF, 0xFF, 0x90, 0x80, 0x00, 0x00, 0x00, 0x84, 0xC0, 0x74, 0x09, 0x83, 0xFB, 0x02, 0x7E, 0x04 }, ResolveGameAddress(NETWORK_GATE_REJECT_ADDRESS));
		prebuiltDraw      = GamePatch::Jump(PREBUILT_DRAW_ADDRESS, { 0xA1, 0xC4, 0x3C, 0xB4, 0x00 }, &PrebuiltDrawHook);
		prebuiltModels    = GamePatch::Jump(PREBUILT_MODELS_ADDRESS, { 0x8B, 0x44, 0x24, 0x14, 0x8B, 0x54, 0x24, 0x0C }, &PrebuiltModelsHook);
		modelInstanceDraw = GamePatch::Jump(MODEL_INSTANCE_DRAW_THUNK_ADDRESS, { 0x2B, 0x49, 0xFC, 0x83, 0xE9, 0x40, 0xE9, 0x75, 0xCF, 0x1E, 0x00 }, &ModelInstanceDrawHook);
		propGroundModel   = GamePatch::Jump(PROP_GROUND_MODEL_ADDRESS, { 0x8D, 0x4C, 0x24, 0x13, 0x51 }, &PropGroundModelStub);
		addShadowCall     = GamePatch::Jump(ADD_SHADOW_CALL_ADDRESS, { 0x8B, 0xCD, 0x57, 0xFF, 0x52, 0x2C }, &AddShadowCallStub);

		// The network shadow masks stay with -NetworkShadowMasks:on
		bool const shouldKeepMasks = GetSettings().shouldKeepNetworkShadowMasks;

		std::vector<GamePatch*> patches = { &prebuiltDraw, &prebuiltModels, &modelInstanceDraw, &propGroundModel, &addShadowCall };
		if (!shouldKeepMasks)
		{
			patches.push_back(&networkGate);
		}

		// Leave the game alone where any site was already changed
		for (GamePatch const* patch : patches)
		{
			if (!patch->IsOriginal())
			{
				Log(LogCategory::Initialization, "live shadows: byte guard failed at 0x%08lX, skipping", patch->GameAddress());
				return false;
			}
		}

		ClearRecords();

		for (GamePatch* patch : patches)
		{
			if (patch->Write())
			{
				continue;
			}

			Log(LogCategory::Initialization, "live shadows: failed to write site 0x%08lX, restoring", patch->GameAddress());
			for (GamePatch* written : patches)
			{
				written->Restore();
			}

			return false;
		}

		isInstalled = true;
		Log(LogCategory::Initialization, "live shadows installed: prebuilt network pieces and True3D props, %s", shouldKeepMasks ? "network shadow masks kept" : "network shadow masks removed");
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

		// In the reverse order of writing
		GamePatch* const patches[] = { &addShadowCall, &propGroundModel, &modelInstanceDraw, &prebuiltModels, &prebuiltDraw, &networkGate };
		for (GamePatch* patch : patches)
		{
			if (!patch->Restore())
			{
				Log(LogCategory::Initialization, "live shadows: not restoring modified site 0x%08lX", patch->GameAddress());
			}
		}

		isInstalled = false;
		ClearRecords();

		Log(LogCategory::Initialization, "live shadows uninstalled: %u network draws, %u prop meshes, %u native prop calls suppressed", networkDraws, propMeshesRegistered, propCallsSuppressed);
		Log(LogCategory::Initialization, "prebuilt models: %u instances registered, %u bracketed draws, %u stale entries rejected", liveModelsRegistered, liveModelDraws, liveModelsRejected);
	}

	bool Enabled(void)
	{
		return isInstalled;
	}

	bool NetworkDrawActive(void)
	{
		return isInstalled && networkDrawDepth != 0;
	}

	bool HasPropMeshes(void)
	{
		return isInstalled && isAnyPropMeshRecorded.load(std::memory_order_relaxed);
	}

	bool MatchPropSignature(uint64_t signature)
	{
		if (!isInstalled)
		{
			return false;
		}

		std::lock_guard<std::mutex> lock(propMutex);
		return std::binary_search(propMeshes.begin(), propMeshes.end(), signature);
	}

	void RegisterPropMesh(uint32_t count, void const* positions, void const* coordinates)
	{
		if (count < MINIMUM_PROP_VERTICES || count > MAXIMUM_PROP_VERTICES || positions == nullptr || coordinates == nullptr)
		{
			return;
		}

		RecordPropMesh(count, positions, coordinates);
	}

	bool SunDirection(float direction[3])
	{
		void* const lighting = isInstalled ? FindLighting() : nullptr;

		float sun[3] = {};
		if (lighting == nullptr || !ReadShadowDirection(lighting, sun) || !std::isfinite(sun[0]) || !std::isfinite(sun[1]) || !std::isfinite(sun[2]))
		{
			return false;
		}

		memcpy(direction, sun, sizeof(sun));
		return true;
	}

	bool ShadowParams(float colour[3], float& strength)
	{
		float tone[3] = {};
		float value   = 0.0f;

		if (!isInstalled || !CopyGameMemory(shadowColour, tone, sizeof(tone)) || !ReadGameMemory(shadowStrength, value))
		{
			return false;
		}

		memcpy(colour, tone, sizeof(tone));
		strength = value;
		return true;
	}

	bool ShadowDaylight(float& daylight)
	{
		void* const lighting = isInstalled ? FindLighting() : nullptr;

		float value = 0.0f;
		if (lighting == nullptr || !ReadGameMemory(static_cast<uint8_t const*>(lighting) + LIGHTING_DAYLIGHT, value) || !std::isfinite(value))
		{
			return false;
		}

		daylight = std::clamp(value, 0.0f, 1.0f);
		return true;
	}
}
