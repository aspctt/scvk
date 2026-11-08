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

#pragma once

//// Dependencies

#include <stdint.h>

namespace scvk
{
	//// Types

	/** What a launch option that turns something on or off says. See ReadFlagSwitch. */
	enum class FlagSwitch
	{
		Absent,  // not given, or given a value that is no flag, which is logged
		On,
		Off,
	};

	/**
	 * The renderer's launch options. Every one has a default that suits playing, and the
	 * game's command line can change them with the switches SCD3D11 takes, read the way
	 * it reads them, so a launcher set up for one renderer behaves the same with the
	 * other. The few settings older than these live in scvk.ini, which is optional.
	 */
	struct Settings
	{
		// -VSync:off. Off presents with MAILBOX, or IMMEDIATE where that is all
		// there is, instead of FIFO.
		bool isVSyncEnabled = true;

		// -GPU:default. Off takes the first device Vulkan lists, the way Windows
		// picks the adapter, instead of preferring a discrete GPU.
		bool shouldPreferHighPerformanceGpu = true;

		// -GPU:<n>. The nth device in scvk.log's list, counted from 1, whatever its kind. 0
		// leaves the choice to the setting above.
		uint32_t gpuNumber = 0;

		// -DoctorDialog:off, or DoctorDialog=false in scvk.ini. Off leaves the setup
		// problems in scvk.log, without the dialog when the region view opens.
		bool isDoctorDialogEnabled = true;

		// -ViewportCull:off. Sends every draw of a scroll's redraw to the card, instead of
		// dropping those that lie wholly outside the strip being redrawn.
		bool isViewportCullEnabled = true;

		// -ReShade:off. Registers scvk as a ReShade add-on so effects render
		// under the interface, with the city's depth supplied.
		bool isReShadeIntegrationEnabled = true;

		// -FlipModel:off. A window or borderless fullscreen presents with the fewest
		// swapchain images the surface allows, as exclusive fullscreen always does,
		// instead of one more for the flip model. See VulkanBackend::CreateSwapchain.
		bool isFlipModelPreferred = true;

		// -LiveShadows:on. Off by default, as in SCD3D11, leaving SC4's own shadows alone,
		// because of what they cost in large cities. On, every True3D prop and network piece
		// casts live in the shadow map, the shadow registry takes over the shadow records
		// SC4 draws as flat decals, and the terrain casts its own shadows. See
		// LiveShadowHooks, ShadowRegistry and TerrainShadows.
		bool areLiveShadowsEnabled = false;

		// -NetworkShadowMasks:on. With live shadows on, keeps the flat FSH shadow masks of
		// prebuilt network pieces, which those pieces otherwise replace by casting live: the
		// two would darken the same ground twice.
		bool shouldKeepNetworkShadowMasks = false;
	};

	//// Public API

	/** The settings, read on the first call. */
	Settings const& GetSettings(void);

	/**
	 * Whether the game's command line holds a switch, compared without regard to case.
	 * The switch is matched whole: -Borderless does not match -BorderlessFoo.
	 */
	bool HasCommandLineSwitch(char const* name);

	/** The text after a switch such as -MaxFPS:, up to the next space, or false when absent. */
	bool GetCommandLineValue(char const* prefix, char* outValue, uint32_t capacity);

	/**
	 * Reads a launch option that turns something on or off, the way SCD3D11 reads its
	 * own: alone, such as -VSync, it means on, and after a colon it takes on, off, true,
	 * false, enabled, disabled, 1 or 0, in any case. The name goes without its dash.
	 */
	FlagSwitch ReadFlagSwitch(char const* name);

	/** Reads one of the words ReadFlagSwitch takes, for the same settings in scvk.ini. False when it is none of them. */
	bool ParseFlag(char const* text, bool& outIsOn);

	/**
	 * A value from an ini file without any comment after it, or the fallback when the key
	 * is absent. Not for a value that may hold a ; or #, such as a city's file name.
	 */
	void ReadIniValue(char const* path, char const* section, char const* key, char const* fallback, char* outValue, uint32_t capacity);
}
