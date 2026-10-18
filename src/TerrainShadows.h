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

#include "TerrainShadowMap.h"

#include <cstdint>

// Terrain shadows, part of -NativeShadowMasks:replace: the loaded city's height
// field, read through cISC4App -> cISC4City -> cISTETerrain, kept as a
// TerrainShadowMap for cGDriver's live shadow composite. Everything here runs
// on the render thread, from the static pass. See docs/true3d-shadows-terrain.md.

namespace scvk::TerrainShadows {
	// SC4 already darkens terrain and models below the terrain's shadow, softly
	// and at vertex resolution: cSC4LightingManager bends their lighting normals
	// by how deep they lie under its own shadow-height grid. These shadows
	// replace that, so Install makes that depth always zero, leaving the grids
	// SC4 also sizes BAT shadows with untouched. Called after
	// NativeShadowRegistry::Install, before SC4 lights anything.
	void Install();
	void Uninstall();

	// Part of -NativeShadowMasks:replace: true once the registry is installed.
	bool Enabled();

	// SC4's RenderShadows render property, the switch that also decides
	// whether DrawOverlays calls DrawShadows. True when it cannot be read.
	bool ShadowsRendered();

	// Brings the map up to date with the city's height field and the sun (the
	// world direction the light travels). With redisplayChanges, asks SC4 to
	// redraw wherever the shading changed, since a partial pass cannot reach
	// those pixels itself. generation changes whenever the map does. Null
	// without a city, or with the sun at or above the horizon.
	TerrainShadowMap::Map const *Update(float const sun[3], bool redisplayChanges, uint64_t &generation);

	// The height field the map returned by the last Update was built from.
	TerrainShadowMap::HeightField const &Terrain();

	// Altitude in the height field the last Update read; false outside the
	// city or before a map exists.
	bool Altitude(float x, float z, float &altitude);
} // namespace scvk::TerrainShadows
