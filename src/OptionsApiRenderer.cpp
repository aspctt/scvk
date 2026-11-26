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

//// Dependencies

#include "OptionsApiRenderer.h"
#include "Logger.h"

#include <cIGZCOM.h>
#include <cIGZFrameWork.h>
#include <cRZCOMDllDirector.h>

namespace scvk::OptionsApiRenderer
{
	//// Private Functions

	namespace
	{
		/** The plugin's renderer interface with a reference to release, or null when it is not installed. */
		cIOptionsAPIRenderer* AcquireRenderer(void)
		{
			cIGZFrameWork* const frameWork = RZGetFramework();
			cIGZCOM* const       com       = (frameWork != nullptr) ? frameWork->GetCOMObject() : nullptr;

			if (com == nullptr)
			{
				return nullptr;
			}

			// GZCOM hands the object back through an untyped pointer, and the interface asked
			// for is what it points at.
			cIOptionsAPIRenderer* renderer = nullptr;
			if (!com->GetClassObject(GZCLSID_cOptionsAPIRenderer, GZIID_cIOptionsAPIRenderer, reinterpret_cast<void**>(&renderer)))
			{
				return nullptr;
			}

			return renderer;
		}

		/** Whether a renderer is one of the five the contract names, since it crosses between DLLs. */
		bool IsKnownRenderer(OptionsAPIRenderer renderer)
		{
			switch (renderer)
			{
			case OptionsAPIRenderer::Vulkan:
			case OptionsAPIRenderer::DirectX11:
			case OptionsAPIRenderer::Software:
			case OptionsAPIRenderer::LegacyHardware:
			case OptionsAPIRenderer::None:
				return true;
			default:
				return false;
			}
		}
	}

	//// Public API

	OptionsAPIRenderer ReadRenderer(void)
	{
		cIOptionsAPIRenderer* const renderer = AcquireRenderer();
		if (renderer == nullptr)
		{
			return OptionsAPIRenderer::None;
		}

		// Only ask an interface that has the function
		//
		// GetVersion comes first in the table in every version, so it is safe to call on any
		// of them, and version 1 is the first with GetRenderer.
		uint32_t const           version = renderer->GetVersion();
		OptionsAPIRenderer const chosen  = (version >= 1) ? renderer->GetRenderer() : OptionsAPIRenderer::None;
		renderer->Release();

		// Say once what the plugin answered, which every choice scvk makes from it is checked
		// against. The answer stays the same until the game quits.
		static bool hasLogged = false;
		if (!hasLogged)
		{
			hasLogged = true;
			LogInfo("SC4 Options API: renderer %s (%u, its renderer interface version %u, scvk's %u).", RendererName(chosen), static_cast<uint32_t>(chosen), version, OPTIONS_API_RENDERER_VERSION);
		}

		// Treat a value from a newer contract as no choice, so scvk falls back to its own
		// rules rather than guessing what the value means
		if (!IsKnownRenderer(chosen))
		{
			return OptionsAPIRenderer::None;
		}

		return chosen;
	}

	char const* RendererName(OptionsAPIRenderer renderer)
	{
		switch (renderer)
		{
		case OptionsAPIRenderer::Vulkan:         return "Vulkan";
		case OptionsAPIRenderer::DirectX11:      return "DirectX 11";
		case OptionsAPIRenderer::Software:       return "Software";
		case OptionsAPIRenderer::LegacyHardware: return "Legacy Hardware";
		case OptionsAPIRenderer::None:           return "none";
		default:                                 return "unknown";
		}
	}
}
