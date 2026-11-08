/*
 *  SCD3D11 - a free Direct3D 11 driver for SimCity 4's SimGL interface
 *  Copyright (C) 2026
 *
 *  This library is free software; you can redistribute it and/or
 *  modify it under the terms of the GNU Lesser General Public
 *  License as published by the Free Software Foundation, under
 *  version 2.1 of the License, or (at your option) any later version.
 */

//// Dependencies

#include "TerrainShadowMap.h"

#include <algorithm>
#include <cfloat>
#include <cmath>

namespace scvk::TerrainShadowMap
{
	//// Constants

	namespace
	{
		// The most texels a side. A large city needs about 700 at half-cell spacing.
		constexpr uint32_t MAXIMUM_TEXELS = 8192;

		constexpr double HALF_PI = 1.5707963267948966;

		// How close to the horizon and the vertical the sun's limbs are taken, so the drop
		// per unit stays finite and positive
		constexpr double LIMB_MARGIN = 0.01;
	}

	//// Private Functions

	namespace
	{
		bool IsValid(HeightField const& terrain)
		{
			size_t const vertices = size_t{ terrain.verticesX } * terrain.verticesZ;
			return terrain.verticesX >= 2 && terrain.verticesZ >= 2 && terrain.cellWidth > 0.0f && terrain.altitudes.size() == vertices && (terrain.flipped.empty() || terrain.flipped.size() == vertices);
		}

		void ToWorld(Map const& map, float a, float b, float& x, float& z)
		{
			// along and across are orthonormal, so the inverse is the transpose
			x = a * map.along[0] + b * map.across[0];
			z = a * map.along[1] + b * map.across[1];
		}
	}

	//// Public API

	float Altitude(HeightField const& terrain, float x, float z)
	{
		// The cell the point lies in, and where in it
		float const lastX = static_cast<float>(terrain.verticesX - 1);
		float const lastZ = static_cast<float>(terrain.verticesZ - 1);
		float const cellX = std::clamp(x / terrain.cellWidth, 0.0f, lastX);
		float const cellZ = std::clamp(z / terrain.cellWidth, 0.0f, lastZ);

		uint32_t const x0 = std::min(static_cast<uint32_t>(cellX), terrain.verticesX - 2);
		uint32_t const z0 = std::min(static_cast<uint32_t>(cellZ), terrain.verticesZ - 2);
		float const tx = cellX - static_cast<float>(x0);
		float const tz = cellZ - static_cast<float>(z0);

		size_t const index = size_t{ z0 } * terrain.verticesX + x0;
		float const h00 = terrain.altitudes[index];
		float const h10 = terrain.altitudes[index + 1];
		float const h01 = terrain.altitudes[index + terrain.verticesX];
		float const h11 = terrain.altitudes[index + terrain.verticesX + 1];

		// Interpolate on the triangle the point is on
		if (!terrain.flipped.empty() && terrain.flipped[index] != 0)
		{
			if (tx + tz <= 1.0f)
			{
				return h00 + (h10 - h00) * tx + (h01 - h00) * tz;
			}

			return h10 * (1.0f - tz) + h01 * (1.0f - tx) + h11 * (tx + tz - 1.0f);
		}

		// cSTETerrain::GetAltitude (0x00741260) splits unflipped cells this way
		if (tx >= tz)
		{
			return h00 + (h10 - h00) * tx + (h11 - h10) * tz;
		}

		return h00 + (h11 - h01) * tx + (h01 - h00) * tz;
	}

	void VertexNormal(HeightField const& terrain, uint32_t x, uint32_t z, float normal[3])
	{
		size_t const index = size_t{ z } * terrain.verticesX + x;
		float value[3] = {};

		if (terrain.normals.size() == terrain.altitudes.size() * 3)
		{
			for (unsigned axis = 0; axis < 3; axis++)
			{
				value[axis] = terrain.normals[index * 3 + axis];
			}
		}
		else
		{
			// The central difference, one-sided at the city's edge
			uint32_t const left  = (x > 0) ? x - 1 : x;
			uint32_t const right = (x + 1 < terrain.verticesX) ? x + 1 : x;
			uint32_t const back  = (z > 0) ? z - 1 : z;
			uint32_t const front = (z + 1 < terrain.verticesZ) ? z + 1 : z;

			auto const altitudeAt = [&](uint32_t column, uint32_t row) { return terrain.altitudes[size_t{ row } * terrain.verticesX + column]; };

			value[0] = -(altitudeAt(right, z) - altitudeAt(left, z)) / (static_cast<float>(right - left) * terrain.cellWidth);
			value[1] = 1.0f;
			value[2] = -(altitudeAt(x, front) - altitudeAt(x, back)) / (static_cast<float>(front - back) * terrain.cellWidth);
		}

		if (value[1] < 0.0f)
		{
			for (float& component : value)
			{
				component = -component;
			}
		}

		float const length = std::sqrt(value[0] * value[0] + value[1] * value[1] + value[2] * value[2]);
		if (!(length > 1.0e-6f) || !std::isfinite(length))
		{
			normal[0] = 0.0f;
			normal[1] = 1.0f;
			normal[2] = 0.0f;
			return;
		}

		for (unsigned axis = 0; axis < 3; axis++)
		{
			normal[axis] = value[axis] / length;
		}
	}

	bool Build(HeightField const& terrain, float const sun[3], float spacing, float sunRadius, Map& map)
	{
		double const horizontal = std::sqrt(double{ sun[0] } * sun[0] + double{ sun[2] } * sun[2]);
		if (!IsValid(terrain) || !(spacing > 0.0f) || !(sun[1] < 0.0f) || !(horizontal > 1.0e-6))
		{
			return false;
		}

		// The map's axes, downwind and across the light
		Map built;
		built.along[0]  = static_cast<float>(sun[0] / horizontal);
		built.along[1]  = static_cast<float>(sun[2] / horizontal);
		built.across[0] = -built.along[1];
		built.across[1] = built.along[0];
		built.spacing   = spacing;

		// The two limbs of the sun, as height lost per unit travelled downwind
		double const elevation = std::atan(-sun[1] / horizontal);
		float const outerDrop = static_cast<float>(std::tan(std::max(elevation - sunRadius, LIMB_MARGIN)));
		float const innerDrop = static_cast<float>(std::tan(std::min(elevation + sunRadius, HALF_PI - LIMB_MARGIN)));

		// The city's corners on the map's axes bound the map
		float const extentX = static_cast<float>(terrain.verticesX - 1) * terrain.cellWidth;
		float const extentZ = static_cast<float>(terrain.verticesZ - 1) * terrain.cellWidth;

		float low[2]  = { FLT_MAX, FLT_MAX };
		float high[2] = { -FLT_MAX, -FLT_MAX };

		for (unsigned corner = 0; corner < 4; corner++)
		{
			float const x = (corner & 1) ? extentX : 0.0f;
			float const z = (corner & 2) ? extentZ : 0.0f;
			float const axes[2] = { x * built.along[0] + z * built.along[1], x * built.across[0] + z * built.across[1] };

			for (unsigned axis = 0; axis < 2; axis++)
			{
				low[axis]  = std::min(low[axis], axes[axis]);
				high[axis] = std::max(high[axis], axes[axis]);
			}
		}

		double const width  = std::ceil((high[0] - low[0]) / spacing) + 1.0;
		double const height = std::ceil((high[1] - low[1]) / spacing) + 1.0;
		if (width > MAXIMUM_TEXELS || height > MAXIMUM_TEXELS)
		{
			return false;
		}

		built.width        = static_cast<uint32_t>(width);
		built.height       = static_cast<uint32_t>(height);
		built.originAlong  = low[0];
		built.originAcross = low[1];
		built.ceiling.resize(size_t{ built.width } * built.height);
		built.umbra.resize(built.ceiling.size());

		// Sweep each row downwind
		//
		// Terrain beyond the city edge is not drawn, so it casts nothing, and the floor there
		// follows the nearest edge so filtering stays smooth.
		float const tolerance = terrain.cellWidth * 1.0e-3f;
		float const outerStep = spacing * outerDrop;
		float const innerStep = spacing * innerDrop;

		for (uint32_t row = 0; row < built.height; row++)
		{
			float const  b        = built.originAcross + static_cast<float>(row) * spacing;
			size_t const rowStart = size_t{ row } * built.width;
			float outer = -FLT_MAX;
			float inner = -FLT_MAX;

			for (uint32_t column = 0; column < built.width; column++)
			{
				float const a = built.originAlong + static_cast<float>(column) * spacing;
				float x = 0.0f;
				float z = 0.0f;
				ToWorld(built, a, b, x, z);

				float const ground = Altitude(terrain, x, z);
				built.ceiling[rowStart + column] = std::max(outer, ground - FLOOR_DEPTH);
				built.umbra[rowStart + column]   = std::max(inner, ground - FLOOR_DEPTH);

				bool const isInside = x >= -tolerance && x <= extentX + tolerance && z >= -tolerance && z <= extentZ + tolerance;
				if (isInside)
				{
					outer = std::max(outer, ground);
					inner = std::max(inner, ground);
				}

				if (outer > -FLT_MAX)
				{
					outer -= outerStep;
				}

				if (inner > -FLT_MAX)
				{
					inner -= innerStep;
				}
			}
		}

		map = std::move(built);
		return true;
	}
}
