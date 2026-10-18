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

	/**
	 * The renderer's settings. There is no settings file: every one has a default that
	 * suits playing, and the game's command line can still change them with the switches
	 * SCD3D11 takes, so a launcher set up for one renderer behaves the same with the other.
	 */
	/** Which of the game-side shadow modules run. See NativeShadowMasks and NativeShadowRegistry. */
	enum class NativeShadowMode
	{
		Off,      // SC4's own shadows only
		Network,  // live True3D shadows of networks
		Props,    // live True3D shadows of props and buildings
		All,      // both: every live True3D shadow
		Replace,  // all of them, and the shadow registry and the terrain's own shadows on top
	};

	struct Settings
	{
		// -VSync:off. Off presents with MAILBOX, or IMMEDIATE where that is all
		// there is, instead of FIFO.
		bool isVSyncEnabled = true;

		// -GPU:default. Off takes the first device Vulkan lists, the way Windows
		// picks the adapter, instead of preferring a discrete GPU.
		bool shouldPreferHighPerformanceGpu = true;

		// -Borderless, -FullscreenMode:Borderless. A fullscreen mode becomes
		// a monitor-sized window without a display mode change, the picture scaled to it.
		bool isBorderlessFullscreen = false;

		// -ReShade:off. Registers scvk as a ReShade add-on so effects render
		// under the interface, with the city's depth supplied.
		bool isReShadeIntegrationEnabled = true;

		// -FlipModel:off. A window or borderless fullscreen presents with the fewest
		// swapchain images the surface allows, as exclusive fullscreen always does,
		// instead of one more for the flip model. See VulkanBackend::CreateSwapchain.
		bool isFlipModelPreferred = true;

		// -NativeShadowMasks:off|network|props|all|replace. All by default: every
		// True3D prop and network piece drawn live in the shadow map, the rest of the
		// shadows left to SC4. Replace adds the registry, which takes over the shadow
		// records SC4 draws as flat decals, and the terrain's own shadows; it is
		// experimental, as in SCD3D11, where it is off by default too, and in a city of
		// dense parks and raised roads it laid blocks of shadow across the roads. Off
		// leaves SC4's own shadows alone.
		NativeShadowMode nativeShadowMode = NativeShadowMode::All;

		// -GridDebug. Logs the terrain grid pass's texture state once a second.
		bool isGridDebugEnabled = false;
	};

	//// Public API

	/** The settings, read on the first call. */
	Settings const& GetSettings(void);

	/**
	 * Whether the game's command line holds a switch, compared without regard to case.
	 * The switch is matched whole: -Borderless does not match -BorderlessFoo.
	 */
	bool HasCommandLineSwitch(char const* name);

	/** The text after a switch such as -NativeShadowMasks:, up to the next space, or false when absent. */
	bool GetCommandLineValue(char const* prefix, char* outValue, uint32_t capacity);
}
