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

#include <cstddef>
#include <cstdint>
#include <vector>

// Shadows cast by the terrain itself, which SC4 never drew
// (docs/true3d-shadows-terrain.md).
//
// SC4's terrain is a height field, so whether the terrain shadows a point
// depends only on how high the point is: (x, y, z) is in shadow when terrain
// between it and the sun rises above the ray towards the sun. The map keeps,
// for every (x, z), the altitude below which that happens - the shadow
// ceiling - so the composite needs one sample per pixel and treats ground,
// walls and roofs alike.
//
// The map is laid out along the light, a row running downwind, so a single
// sweep per row fills it:
//
//     ceiling[i + 1] = max(ceiling[i], terrain[i]) - spacing * drop
//
// where drop is how far the ray to the sun falls per unit of horizontal
// distance. That is exact for the sampled terrain and costs O(1) per texel.

namespace scvk::TerrainShadowMap {
	// The terrain as SC4 draws it. Each cell is two triangles, split along
	// (0, 0)-(1, 1) unless the cell is flipped, which splits it along
	// (1, 0)-(0, 1); cliff management and terrain mods such as a DJEM fix flip
	// cells, so interpolating bilinearly would miss the drawn surface by metres
	// on steep ground.
	struct HeightField {
		uint32_t verticesX = 0;
		uint32_t verticesZ = 0;
		float cellWidth = 0.0f;
		// Row-major, z * verticesX + x; vertex (x, z) lies at world (x, z) * cellWidth.
		std::vector<float> altitudes;
		// Indexed like altitudes: non-zero when the cell whose lowest corner is
		// that vertex is flipped. Empty means none is.
		std::vector<uint8_t> flipped;
		// Indexed like altitudes, three floats each: the normals SC4 lights the
		// terrain with. Empty when unknown.
		std::vector<float> normals;
	};

	struct Map {
		// Texels along the light (one row) and across it (the rows).
		uint32_t width = 0;
		uint32_t height = 0;
		// World (x, z) to map axes: a = x * along[0] + z * along[1] runs
		// downwind, b = x * across[0] + z * across[1] runs across the light.
		float along[2]{};
		float across[2]{};
		// a and b at the centre of texel (0, 0), and the pitch between texels.
		float originAlong = 0.0f;
		float originAcross = 0.0f;
		float spacing = 0.0f;
		// Row-major, b * width + a: the altitude below which the terrain up-sun
		// shadows a point. Never more than kFloorDepth below the terrain, so
		// that filtering between a shadowed and an unshadowed texel lands the
		// edge between them rather than on the shadowed one.
		std::vector<float> ceiling;
	};

	struct Region {
		float minX = 0.0f;
		float minZ = 0.0f;
		float maxX = 0.0f;
		float maxZ = 0.0f;
	};

	// How far the stored ceiling may sit below the terrain.
	constexpr float kFloorDepth = 32.0f;

	// Altitude of the drawn surface, clamped to the city.
	float Altitude(HeightField const &terrain, float x, float z);

	// The unit normal SC4 lights vertex (x, z) with, or the height field's
	// central difference there when those are unknown. Always points up.
	void VertexNormal(HeightField const &terrain, uint32_t x, uint32_t z, float normal[3]);

	// Builds the map for light travelling along sun (world space, any length),
	// one texel per spacing world units. False for a malformed height field or
	// a sun that is not below the horizon and off the vertical.
	bool Build(HeightField const &terrain, float const sun[3], float spacing, Map &map);

	// The ceiling at a world position, filtered as the composite samples it.
	float Ceiling(Map const &map, float x, float z);

	// Whether two maps put their texels at the same world positions, i.e. can
	// be compared texel by texel.
	bool SameLayout(Map const &a, Map const &b);

	// World rectangles covering every texel inside the city whose shading can
	// differ between before and after, which must share a layout. More than
	// maxRegions collapse into their union.
	void ChangedRegions(Map const &before, Map const &after, HeightField const &terrain, size_t maxRegions,
	                    std::vector<Region> &regions);
} // namespace scvk::TerrainShadowMap
