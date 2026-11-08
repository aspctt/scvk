/*
 *  SCD3D11 - a free Direct3D 11 driver for SimCity 4's SimGL interface
 *  Copyright (C) 2026
 *
 *  This library is free software; you can redistribute it and/or
 *  modify it under the terms of the GNU Lesser General Public
 *  License as published by the Free Software Foundation, under
 *  version 2.1 of the License, or (at your option) any later version.
 */

#pragma once

//// Dependencies

#include <stdint.h>

/**
 * The game-side half of live shadows, opt-in with -LiveShadows:on.
 *
 * Prebuilt network pieces and True3D props are told apart here, and cVKDriver captures
 * their draws as indexed casters. The game's lighting is read here for the composite: the
 * direction its sun shines, its shadow colour and strength, and how much of the day is
 * left in them.
 *
 * Carried over from SCD3D11. Only game version 641 is changed.
 */
namespace scvk::LiveShadowHooks
{
	//// Public API

	/** Patches the game, when live shadows are switched on. Returns whether the hooks are in place. */
	bool Install(void);

	/** Puts the game's code back as it was. */
	void Uninstall(void);

	/** Whether the hooks are in place, which is what turns live shadows on. */
	bool Enabled(void);

	/** Whether a prebuilt network piece or its model is being drawn on this thread. */
	bool NetworkDrawActive(void);

	/** Whether any relaxed prop's mesh has been recorded, cheap enough to ask on every draw. */
	bool HasPropMeshes(void);

	/** Whether a draw's mesh, hashed as RegisterPropMesh hashes it, is a relaxed prop's. */
	bool MatchPropSignature(uint64_t signature);

	/**
	 * Records a relaxed prop's mesh, from the position and coordinate streams the game hands
	 * AddShadow, for cVKDriver to recognise when the prop is drawn. ShadowRegistry falls back
	 * on it for a prop it cannot keep.
	 */
	void RegisterPropMesh(uint32_t count, void const* positions, void const* coordinates);

	/** The world direction the game's shadows travel, from its GetShadowDirection. */
	bool SunDirection(float direction[3]);

	/** The game's shadow colour and strength, as its GetShadowParams hands them out. */
	bool ShadowParams(float colour[3], float& strength);

	/** How much of the day the shadow tint carries, 0 at night to 1 by day. */
	bool ShadowDaylight(float& daylight);
}
