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

#include "TerrainShadowMap.h"

#include <stdint.h>

/**
 * Terrain shadows, part of -LiveShadows:on: the loaded city's height field, read through
 * cISC4App, cISC4City and cISTETerrain, kept as a TerrainShadowMap for cVKDriver's live
 * shadow composite. Everything here runs on the render thread, from the static pass.
 *
 * Carried over from SCD3D11. Only game version 641 is changed.
 */
namespace scvk::TerrainShadows
{
	//// Public API

	/**
	 * Takes over the game's own terrain self-shading, once LiveShadowHooks is in place and
	 * before the game lights anything.
	 *
	 * The game already darkens terrain and models below the terrain's shadow, softly and at
	 * vertex resolution: cSC4LightingManager bends their lighting normals by how deep they
	 * lie under its own shadow height grid. These shadows replace that, so Install makes
	 * that depth always zero, leaving the grids the game also sizes BAT shadows with alone.
	 * -TerrainShadows:off keeps the game's own, for comparing pictures.
	 */
	void Install(void);

	/** Puts the game's self-shading back. */
	void Uninstall(void);

	/** Whether the terrain's own shadows are drawn, which Install decided. */
	bool Enabled(void);

	/**
	 * The game's RenderShadows render property, the switch that also decides whether
	 * DrawOverlays calls DrawShadows. True when it cannot be read.
	 */
	bool ShadowsRendered(void);

	/**
	 * Brings the map up to date with the city's height field and the sun, the world
	 * direction the light travels. generation changes whenever the map does.
	 *
	 * Returns null without a city, or with the sun at or above the horizon.
	 */
	TerrainShadowMap::Map const* Update(float const sun[3], uint64_t& generation);

	/** The height field the map the last Update returned was built from. */
	TerrainShadowMap::HeightField const& Terrain(void);

	/** The altitude in the height field the last Update read. False outside the city or before a map exists. */
	bool Altitude(float x, float z, float& altitude);
}
