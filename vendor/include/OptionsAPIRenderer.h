/*
 * SC4 Options API - modern graphics, play and mod options for SimCity 4
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

#pragma once

//// Dependencies

#include "cIGZUnknown.h"

#include <stdint.h>

/**
 * The contract between SC4 Options API and a renderer for the renderer the player chose.
 *
 * The game picks its driver by class ID from three slots: DirectX (0xBADB6906), OpenGL
 * (0xC4554841) and Software (0x7ACA35C6). The plugin asks the game for one of them at every
 * start, after every plugin's PreFrameWorkInit and before any service starts, through
 * cIGZGraphicSystem2::SetDefaultDriverClassID and the video preferences' driver type:
 *
 * - Vulkan: the OpenGL slot, which scvk claims
 * - DirectX 11: the DirectX slot, which scd3d11 claims
 * - Software: the Software slot, the game's own
 * - Legacy Hardware: the DirectX slot, which the plugin then claims itself above every
 *   renderer, with the game's own DirectX 7 driver in it
 *
 * A renderer reads the choice so it can tell a renderer the player chose from one the game
 * fell back to, and leave the first alone. A launch option such as -d leaves the driver to
 * the game and the renderers for that start, and the plugin then answers None.
 *
 * A renderer copies this file unchanged. Version 1.
 */

//// Types

/** The renderer the plugin asked the game for. The values are fixed, since they cross between DLLs. */
enum class OptionsAPIRenderer : uint32_t
{
	// scvk, in the OpenGL slot
	Vulkan = 0,

	// scd3d11, in the DirectX slot
	DirectX11 = 1,

	// The game's own Software renderer
	Software = 2,

	// The game's own DirectX 7, in the DirectX slot, which no renderer gets
	LegacyHardware = 3,

	// The plugin asked for nothing this start, so the game and the renderers choose as they
	// would without it
	None = 0xFFFFFFFF,
};

//// Constants

// The class SC4 Options API registers with the game's COM, and its interface
static const uint32_t GZCLSID_cOptionsAPIRenderer = 0x38A084CA;
static const uint32_t GZIID_cIOptionsAPIRenderer = 0x8B89D52C;

// The version of this file
static const uint32_t OPTIONS_API_RENDERER_VERSION = 1;

//// Interfaces

/**
 * What SC4 Options API tells a renderer about the player's choice.
 *
 * A renderer gets it from the game's COM once the plugins are loaded:
 * RZGetFramework()->GetCOMObject()->GetClassObject(GZCLSID_cOptionsAPIRenderer,
 * GZIID_cIOptionsAPIRenderer, &renderer). The call fails when the plugin is not installed.
 * The object comes with a reference, to release once done.
 */
class cIOptionsAPIRenderer : public cIGZUnknown
{
public:
	/** The version of this file the plugin was built with, at least 1. */
	virtual uint32_t GetVersion(void) = 0;

	/**
	 * The renderer the plugin asks the game for at this start. It is settled once the
	 * plugins are loaded and stays the same until the game quits; a choice made in the
	 * Graphic Options window waits for the next start.
	 */
	virtual OptionsAPIRenderer GetRenderer(void) = 0;
};
