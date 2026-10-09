/*
 * scvk - a native Vulkan renderer for SimCity 4
 *
 * ReShade integration carried over from SCD3D11 (cGDriver_ReShade.cpp), Copyright (C)
 * 2026 the SCD3D11 authors, under the same licence.
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
 * ReShade, and the frame callback other plugins draw with.
 *
 * Installed as its Vulkan layer, ReShade wraps the device scvk creates and draws its
 * effects at present, over the whole frame, interface included. When ReShade is loaded
 * scvk registers itself as an add-on instead, as SCD3D11 does, and
 *
 *  - draws the effects at the end of cSC43DRender::Draw, once the city view is complete
 *    and before any interface is drawn over it;
 *  - binds the scene depth to the DEPTH semantic, encoded so that ReShade.fxh's
 *    perspective linearization returns the game's depth, which is linear because the
 *    camera is orthographic;
 *  - publishes the sc4_* uniforms effects can ask for.
 *
 * The game keeps its back buffer from one frame to the next and redraws only what
 * changed, so effects drawn into it persist until the city view is redrawn. Frames that
 * only redraw the interface must not get effects again at present; FinishReShadeFrame
 * marks those as done. -ReShade:off leaves ReShade's default behaviour alone.
 *
 * The same hook on the 3D view's draw also drives the live shadows, so it is installed
 * whether or not ReShade is there.
 *
 * Vulkan has no immediate context to share, so before ReShade records its effects the
 * frame so far is submitted, the back buffer left a render target and the encoded depth
 * ready to sample. ReShade submits its own commands after it, and the frame carries on in
 * a command buffer that waits for them.
 */

//// Dependencies

#include "VulkanApi.h"

#include "cVKDriver.h"
#include "Logger.h"
#include "ModuleLog.h"
#include "NativeShadowMasks.h"
#include "NativeShadowRegistry.h"
#include "Settings.h"
#include "VulkanBackend.h"

#include <scvk_frame.h>
#include <reshade.hpp>

#include <windows.h>
#include <condition_variable>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <type_traits>
#include <vector>

//// State

namespace
{
	// The frame callback, as SCD3D11 keeps its own: one at a time, and unregistering waits
	// for a call in progress elsewhere to return.
	std::mutex              callbackMutex;
	std::condition_variable callbackIdle;
	SCVKFrameCallback       frameCallback      = nullptr;
	void*                   frameCallbackData  = nullptr;
	uint32_t                callbacksInFlight  = 0;
	thread_local bool       isInsideCallback   = false;
}

//// Exports

extern "C" BOOL __stdcall SCVKRegisterFrameCallback(SCVKFrameCallback callback, void* userData)
{
	if (callback == nullptr)
	{
		return FALSE;
	}

	scvk::Log(scvk::LogCategory::Grid, "SCVKRegisterFrameCallback(callback=%p, userData=%p)", reinterpret_cast<void*>(callback), userData);

	std::lock_guard<std::mutex> const lock(callbackMutex);
	if (frameCallback != nullptr)
	{
		return (frameCallback == callback && frameCallbackData == userData) ? TRUE : FALSE;
	}

	frameCallback     = callback;
	frameCallbackData = userData;
	return TRUE;
}

extern "C" BOOL __stdcall SCVKUnregisterFrameCallback(SCVKFrameCallback callback, void* userData)
{
	std::unique_lock<std::mutex> lock(callbackMutex);
	if (callback == nullptr || frameCallback != callback || frameCallbackData != userData)
	{
		return FALSE;
	}

	frameCallback     = nullptr;
	frameCallbackData = nullptr;

	// A callback may unregister itself, and then the call itself is the barrier
	if (!isInsideCallback)
	{
		callbackIdle.wait(lock, [] { return callbacksInFlight == 0; });
	}

	return TRUE;
}

// Exported under their plain names, as SCD3D11 exports its pair
#if defined(_MSC_VER) && defined(_M_IX86)
#pragma comment(linker, "/EXPORT:SCVKRegisterFrameCallback=_SCVKRegisterFrameCallback@8")
#pragma comment(linker, "/EXPORT:SCVKUnregisterFrameCallback=_SCVKUnregisterFrameCallback@8")
#endif

namespace scvk
{
	//// Constants

	namespace
	{
		constexpr uintptr_t IMAGE_BASE = 0x00400000;

		// The cSC43DRender vtable slot for cSC43DRender::Draw, its only reference
		// (SimCity 4 1.1.641).
		constexpr uintptr_t DRAW_SLOT_ADDRESS = 0x00ABABB4;
		constexpr uintptr_t DRAW_ADDRESS      = 0x007CB530;

		// DrawPostStaticView is called after every static view submission, dirty
		// rectangle updates included, and before the game saves the image to its backing
		// store.
		constexpr uintptr_t DRAW_POST_STATIC_VIEW_ADDRESS = 0x007C3ED0;

		// ReShade.fxh's default far plane.
		constexpr float DEFAULT_FAR_PLANE = 1000.0f;
	}

	//// State

	namespace
	{
		using DrawFunction = bool(__fastcall*)(void* self, void* edx);

		DrawFunction                   originalDraw            = nullptr;
		[[maybe_unused]] uintptr_t     drawPostStaticViewRejoin = 0;
		cVKDriver*                     hookedDriver            = nullptr;
		reshade::api::effect_runtime*  effectRuntime           = nullptr;
		VkImageView                    sceneDepthView          = VK_NULL_HANDLE;
		bool                           isDepthBound            = false;
		float                          farPlane                = DEFAULT_FAR_PLANE;
		bool                           isAddonRegistered       = false;

		// Uniforms whose "source" annotation names something ReShade never writes are the
		// add-on's to fill; their handles are found once per effect reload.
		struct ShadowUniformBindings
		{
			std::vector<reshade::api::effect_uniform_variable> valid;
			std::vector<reshade::api::effect_uniform_variable> depthScale;
			std::vector<reshade::api::effect_uniform_variable> worldPerScreen;

			void Clear(void)
			{
				valid.clear();
				depthScale.clear();
				worldPerScreen.clear();
			}

			bool Any(void) const
			{
				return !valid.empty() || !depthScale.empty() || !worldPerScreen.empty();
			}
		};

		ShadowUniformBindings shadowBindings;
	}

	//// Private Functions

	namespace
	{
		/** A Vulkan handle's value: the handle itself in a 32-bit build, a pointer in a 64-bit one. */
		template <typename Handle>
		uint64_t HandleValue(Handle handle)
		{
			if constexpr (std::is_pointer_v<Handle>)
			{
				return static_cast<uint64_t>(reinterpret_cast<uintptr_t>(handle));
			}
			else
			{
				return static_cast<uint64_t>(handle);
			}
		}

		/** A view as ReShade names it: the Vulkan handle's value. */
		reshade::api::resource_view ViewHandle(VkImageView view)
		{
			return reshade::api::resource_view{ HandleValue(view) };
		}

		bool __fastcall DrawHook(void* self, void* edx)
		{
			// A translated update shifts the backing store's pixels and redraws only the
			// strips exposed. That knows a prop's own footprint but not its shadow's, so
			// while the native network or prop shadows are on, translated updates, camera
			// changes and partial shadow passes ask for the game's own full redraw. With
			// -NativeShadowMasks:replace the strips are correct as drawn.
			if (self != nullptr && NativeShadowMasks::RequiresCleanTranslatedRedraw())
			{
				uint8_t* const bytes = static_cast<uint8_t*>(self);

				uint32_t horizontal = 0;
				uint32_t vertical   = 0;
				std::memcpy(&horizontal, bytes + 0xE0, sizeof(horizontal));
				std::memcpy(&vertical, bytes + 0xE4, sizeof(vertical));

				bool const isTranslated     = (horizontal != 0 || vertical != 0) && !NativeShadowRegistry::Enabled();
				bool const isPartialShadow  = hookedDriver != nullptr && hookedDriver->ConsumeLiveShadowCleanRedraw();

				// cSC43DRender+0x8c owns cSC4CameraControl, whose zoom and rotation are the
				// integers at +0x108 and +0x10c, written before Draw is entered
				static void*   lastRender      = nullptr;
				static int32_t lastZoom        = 0;
				static int32_t lastRotation    = 0;
				static bool    hasCameraState  = false;

				uint8_t* camera = nullptr;
				std::memcpy(&camera, bytes + 0x8C, sizeof(camera));

				bool isCameraChanged = false;
				if (camera != nullptr)
				{
					int32_t zoom     = 0;
					int32_t rotation = 0;
					std::memcpy(&zoom, camera + 0x108, sizeof(zoom));
					std::memcpy(&rotation, camera + 0x10C, sizeof(rotation));

					isCameraChanged = hasCameraState && lastRender == self && (zoom != lastZoom || rotation != lastRotation);
					lastRender      = self;
					lastZoom        = zoom;
					lastRotation    = rotation;
					hasCameraState  = true;
				}

				if (isTranslated || isCameraChanged || isPartialShadow)
				{
					// cSC43DRender::mbBackingStoreValid
					bytes[0x65] = 0;

					static bool hasLoggedTranslation = false;
					static bool hasLoggedCamera      = false;

					if (isTranslated && !hasLoggedTranslation)
					{
						hasLoggedTranslation = true;
						Log(LogCategory::Initialization, "native shadows: translated backing-store updates force a clean static redraw");
					}

					if (isCameraChanged && !hasLoggedCamera)
					{
						hasLoggedCamera = true;
						Log(LogCategory::Initialization, "native shadows: zoom/rotation changes force a clean static redraw");
					}
				}
			}

			bool const isDrawn = originalDraw(self, edx);
			if (isDrawn && hookedDriver != nullptr)
			{
				hookedDriver->RenderSceneEffects();
			}

			return isDrawn;
		}

#if defined(_MSC_VER) && defined(_M_IX86)
		// The instructions the patch replaces, then back into the function after them
		__declspec(naked) void __fastcall DrawPostStaticViewOriginal(void*, void*)
		{
			__asm
			{
				push esi
				push edi
				mov  edi, ecx
				mov  eax, dword ptr [edi + 0x11c]
				jmp  dword ptr [drawPostStaticViewRejoin]
			}
		}

		void __fastcall DrawPostStaticViewHook(void* self, void* edx)
		{
			DrawPostStaticViewOriginal(self, edx);

			if (hookedDriver != nullptr)
			{
				hookedDriver->RenderLivePropShadows(true);
			}
		}
#endif

		void OnReloadedEffects(reshade::api::effect_runtime* runtime)
		{
			char value[32] = {};
			float const definedFarPlane = runtime->get_preprocessor_definition("RESHADE_DEPTH_LINEARIZATION_FAR_PLANE", value) ? std::strtof(value, nullptr) : DEFAULT_FAR_PLANE;
			farPlane = (definedFarPlane >= 1.0f) ? definedFarPlane : DEFAULT_FAR_PLANE;

			shadowBindings.Clear();
			runtime->enumerate_uniform_variables(nullptr, [](reshade::api::effect_runtime* effects, reshade::api::effect_uniform_variable variable)
			{
				char source[32] = {};
				if (!effects->get_annotation_string_from_uniform_variable(variable, "source", source))
				{
					return;
				}

				if (std::strcmp(source, "bufready_depth") == 0)
				{
					effects->set_uniform_value_bool(variable, true);
				}
				else if (std::strcmp(source, "sc4_sun_valid") == 0)
				{
					shadowBindings.valid.push_back(variable);
				}
				else if (std::strcmp(source, "sc4_depth_scale") == 0)
				{
					shadowBindings.depthScale.push_back(variable);
				}
				else if (std::strcmp(source, "sc4_world_per_screen_height") == 0)
				{
					shadowBindings.worldPerScreen.push_back(variable);
				}
			});
		}

		/** Pushes the projection's scales into every effect that asked for them. */
		void PublishShadowUniforms(reshade::api::effect_runtime* runtime, bool isValid, float depthScale, float worldPerScreenHeight)
		{
			if (!shadowBindings.Any())
			{
				return;
			}

			for (auto const variable : shadowBindings.valid)
			{
				runtime->set_uniform_value_bool(variable, isValid);
			}

			if (!isValid)
			{
				return;
			}

			for (auto const variable : shadowBindings.depthScale)
			{
				runtime->set_uniform_value_float(variable, depthScale);
			}

			for (auto const variable : shadowBindings.worldPerScreen)
			{
				runtime->set_uniform_value_float(variable, worldPerScreenHeight);
			}
		}

		void OnInitEffectRuntime(reshade::api::effect_runtime* runtime)
		{
			effectRuntime = runtime;
			OnReloadedEffects(runtime);
			Log(LogCategory::Initialization, "reshade: effect runtime attached");
		}

		void OnDestroyEffectRuntime(reshade::api::effect_runtime* runtime)
		{
			if (runtime == effectRuntime)
			{
				effectRuntime = nullptr;
			}
		}

		// Runs inside render_effects, after ReShade's own depth add-on picked its buffer
		void OnBeginEffects(reshade::api::effect_runtime* runtime, reshade::api::command_list*, reshade::api::resource_view, reshade::api::resource_view)
		{
			if (sceneDepthView == VK_NULL_HANDLE)
			{
				return;
			}

			runtime->update_texture_bindings("DEPTH", ViewHandle(sceneDepthView), ViewHandle(sceneDepthView));
			isDepthBound = true;
		}

		/** Points the 3D view's vtable slot for Draw at the hook, if it holds what is expected. */
		bool PatchDrawSlot(void* replacement, void* expected)
		{
			void** const slot = reinterpret_cast<void**>(reinterpret_cast<uintptr_t>(GetModuleHandleW(nullptr)) + (DRAW_SLOT_ADDRESS - IMAGE_BASE));

			DWORD protection = 0;
			if (*slot != expected || !VirtualProtect(slot, sizeof(*slot), PAGE_READWRITE, &protection))
			{
				return false;
			}

			*slot = replacement;
			VirtualProtect(slot, sizeof(*slot), protection, &protection);
			return true;
		}

		/** Jumps from the start of DrawPostStaticView to its hook, if the bytes are the expected ones. */
		bool PatchDrawPostStaticView(void* module)
		{
#if defined(_MSC_VER) && defined(_M_IX86)
			uint8_t* const target = static_cast<uint8_t*>(module) + (DRAW_POST_STATIC_VIEW_ADDRESS - IMAGE_BASE);
			uint8_t const  expected[] = { 0x56, 0x57, 0x8B, 0xF9, 0x8B, 0x87, 0x1C, 0x01, 0x00, 0x00 };

			if (std::memcmp(target, expected, sizeof(expected)) != 0)
			{
				return false;
			}

			uint8_t replacement[sizeof(expected)] = {};
			std::memset(replacement, 0x90, sizeof(replacement));
			replacement[0] = 0xE9;

			int32_t const displacement = static_cast<int32_t>(reinterpret_cast<uintptr_t>(&DrawPostStaticViewHook) - (reinterpret_cast<uintptr_t>(target) + 5));
			std::memcpy(replacement + 1, &displacement, sizeof(displacement));
			drawPostStaticViewRejoin = reinterpret_cast<uintptr_t>(target + sizeof(expected));

			DWORD protection = 0;
			if (!VirtualProtect(target, sizeof(expected), PAGE_EXECUTE_READWRITE, &protection))
			{
				return false;
			}

			std::memcpy(target, replacement, sizeof(replacement));

			DWORD ignored = 0;
			VirtualProtect(target, sizeof(expected), protection, &ignored);
			FlushInstructionCache(GetCurrentProcess(), target, sizeof(expected));
			return true;
#else
			(void)module;
			return false;
#endif
		}

		/** Calls the registered frame callback, if there is one. Returns whether it ran. */
		bool CallFrameCallback(SCVKFrameContext const& frame)
		{
			SCVKFrameCallback callback = nullptr;
			void*             userData = nullptr;

			{
				std::lock_guard<std::mutex> const lock(callbackMutex);
				callback = frameCallback;
				userData = frameCallbackData;

				if (callback != nullptr)
				{
					callbacksInFlight++;
				}
			}

			if (callback == nullptr)
			{
				return false;
			}

			struct InvocationGuard
			{
				InvocationGuard(void)  { isInsideCallback = true; }
				~InvocationGuard(void)
				{
					isInsideCallback = false;

					std::lock_guard<std::mutex> const lock(callbackMutex);
					callbacksInFlight--;
					callbackIdle.notify_all();
				}
			} const guard;

			callback(&frame, userData);
			return true;
		}
	}

	void cVKDriver::InstallReShadeAddon(void)
	{
		// Patch once for the process; a later driver takes the hooks over
		static bool hasAttempted = false;
		if (hasAttempted)
		{
			if (originalDraw != nullptr)
			{
				hookedDriver = this;
			}

			return;
		}

		hasAttempted = true;

		void* const executable = GetModuleHandleW(nullptr);
		void* const draw       = reinterpret_cast<void*>(reinterpret_cast<uintptr_t>(executable) + (DRAW_ADDRESS - IMAGE_BASE));

		if (!PatchDrawSlot(reinterpret_cast<void*>(&DrawHook), draw))
		{
			LogInfo("Scene effects: cSC43DRender::Draw not found; ReShade's effects and the live shadows stay at present.");
			return;
		}

		originalDraw = reinterpret_cast<DrawFunction>(draw);
		hookedDriver = this;

		if (PatchDrawPostStaticView(executable))
		{
			Log(LogCategory::Initialization, "live shadows: static pass hooked before SC4 backing-store save");
		}
		else
		{
			Log(LogCategory::Initialization, "live shadows: DrawPostStaticView byte guard failed; using end-of-scene fallback");
		}

		if (!GetSettings().isReShadeIntegrationEnabled)
		{
			LogInfo("ReShade: integration switched off; ReShade, if loaded, draws at present.");
			return;
		}

		// Register as an add-on, which works only when ReShade 6 or later is loaded
		HMODULE module = nullptr;
		if (!GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT, reinterpret_cast<LPCWSTR>(&DrawHook), &module) || !reshade::register_addon(module))
		{
			LogDebug("ReShade: not loaded.");
			return;
		}

		reshade::register_event<reshade::addon_event::init_effect_runtime>(&OnInitEffectRuntime);
		reshade::register_event<reshade::addon_event::destroy_effect_runtime>(&OnDestroyEffectRuntime);
		reshade::register_event<reshade::addon_event::reshade_begin_effects>(&OnBeginEffects);
		reshade::register_event<reshade::addon_event::reshade_reloaded_effects>(&OnReloadedEffects);

		isAddonRegistered = true;
		LogInfo("ReShade: add-on registered; effects render before the interface, with the city's depth.");
	}

	void cVKDriver::RenderSceneEffects(void)
	{
		RenderLivePropShadows(false);

		if (effectRuntime == nullptr || !vulkan->IsReady() || vulkan->IsDeviceLost())
		{
			return;
		}

		// Encode the depth, then hand the frame so far to the queue for ReShade to follow
		sceneDepthView = vulkan->PrepareSceneDepth(farPlane);
		PublishShadowUniforms(effectRuntime, areShadowUniformsValid, shadowDepthScale, shadowWorldPerScreen);

		static bool hasLoggedValid   = false;
		static bool hasLoggedInvalid = false;

		if (areShadowUniformsValid && !hasLoggedValid)
		{
			hasLoggedValid = true;
			LogInfo("ReShade: shadow uniforms published (depth scale %.4f, world per screen height %.4f, %u bindings).", static_cast<double>(shadowDepthScale), static_cast<double>(shadowWorldPerScreen), static_cast<unsigned>(shadowBindings.depthScale.size() + shadowBindings.worldPerScreen.size()));
		}
		else if (!areShadowUniformsValid && !hasLoggedInvalid)
		{
			hasLoggedInvalid = true;
			LogInfo("ReShade: no orthographic projection captured yet; the shadow uniforms stay unset.");
		}

		if (!vulkan->SubmitForExternalWork())
		{
			sceneDepthView = VK_NULL_HANDLE;
			return;
		}

		// Draw the effects into the back buffer, before the interface
		reshade::api::command_queue* const queue    = effectRuntime->get_command_queue();
		reshade::api::command_list* const  commands = queue->get_immediate_command_list();

		effectRuntime->render_effects(commands, ViewHandle(vulkan->BackBufferView(false)), ViewHandle(vulkan->BackBufferView(true)));

		if (isDepthBound)
		{
			// Never leave ReShade holding a view a resize or a lost device may destroy
			effectRuntime->update_texture_bindings("DEPTH", reshade::api::resource_view{ 0 }, reshade::api::resource_view{ 0 });
			isDepthBound = false;
		}

		queue->flush_immediate_command_list();

		sceneDepthView               = VK_NULL_HANDLE;
		isReShadeEffectsInBackBuffer = true;
		isReShadeEffectsThisFrame    = true;
	}

	void cVKDriver::FinishReShadeFrame(void)
	{
		// A null target only marks the frame's effects as drawn, so present does not stack
		// another pass on a back buffer that still holds the last city redraw's
		if (effectRuntime != nullptr && isReShadeEffectsInBackBuffer && !isReShadeEffectsThisFrame)
		{
			effectRuntime->render_effects(effectRuntime->get_command_queue()->get_immediate_command_list(), reshade::api::resource_view{ 0 }, reshade::api::resource_view{ 0 });
		}

		isReShadeEffectsThisFrame = false;
	}

	void cVKDriver::InvokeFrameCallback(void)
	{
		{
			std::lock_guard<std::mutex> const lock(callbackMutex);
			if (frameCallback == nullptr)
			{
				return;
			}
		}

		ExternalFrame borrowed;
		if (!vulkan->BorrowExternalFrame(borrowed))
		{
			return;
		}

		SCVKFrameContext frame{};
		frame.structSize       = sizeof(frame);
		frame.apiVersion       = 1;
		frame.event            = SCVK_EVENT_RENDER;
		frame.deviceGeneration = vulkan->DeviceGeneration();
		frame.instance         = borrowed.instance;
		frame.physicalDevice   = borrowed.physicalDevice;
		frame.device           = borrowed.device;
		frame.queue            = borrowed.queue;
		frame.queueFamilyIndex = borrowed.queueFamily;
		frame.commandBuffer    = borrowed.commandBuffer;
		frame.backBuffer       = borrowed.backBuffer;
		frame.backBufferView   = borrowed.backBufferView;
		frame.backBufferFormat = borrowed.format;
		frame.width            = borrowed.width;
		frame.height           = borrowed.height;
		frame.window           = static_cast<HWND>(windowHandle);

		CallFrameCallback(frame);
		vulkan->ReturnExternalFrame();
	}

	void cVKDriver::NotifyBeforeDeviceDestroy(void)
	{
		// ReShade must not keep a view of the device going away
		if (effectRuntime != nullptr && isDepthBound)
		{
			effectRuntime->update_texture_bindings("DEPTH", reshade::api::resource_view{ 0 }, reshade::api::resource_view{ 0 });
			isDepthBound = false;
		}

		ExternalFrame device;
		vulkan->DescribeDevice(device);

		SCVKFrameContext frame{};
		frame.structSize       = sizeof(frame);
		frame.apiVersion       = 1;
		frame.event            = SCVK_EVENT_BEFORE_DEVICE_DESTROY;
		frame.deviceGeneration = vulkan->DeviceGeneration();
		frame.instance         = device.instance;
		frame.physicalDevice   = device.physicalDevice;
		frame.device           = device.device;
		frame.queue            = device.queue;
		frame.queueFamilyIndex = device.queueFamily;
		frame.window           = static_cast<HWND>(windowHandle);

		CallFrameCallback(frame);
	}

	void cVKDriver::UninstallReShadeAddon(void)
	{
		// The patches stay for the process; they are inert without a driver
		if (hookedDriver == this)
		{
			hookedDriver = nullptr;
		}
	}
}
