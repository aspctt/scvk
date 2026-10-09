/*
 *  SCD3D11 - a free Direct3D 11 driver for SimCity 4's SimGL interface
 *  Copyright (C) 2026
 *
 *  This library is free software; you can redistribute it and/or
 *  modify it under the terms of the GNU Lesser General Public
 *  License as published by the Free Software Foundation, under
 *  version 2.1 of the License, or (at your option) any later version.
 */

#include "TerrainShadowMap.h"

#include <algorithm>
#include <cfloat>
#include <cmath>

namespace scvk::TerrainShadowMap {
	namespace {
		// D3D10's texture size limit; a large city needs about 700 texels at
		// half-cell spacing.
		constexpr uint32_t kMaxTexels = 8192;
		// Changes smaller than this cannot move a shadow edge visibly.
		constexpr float kChangeTolerance = 0.01f;
		// Receivers sit on or above the terrain, so a ceiling that stays this
		// far below it shades nothing either way.
		constexpr float kReceiverSlack = 1.0f;
		// ChangedRegions groups texels into square tiles of this many.
		constexpr uint32_t kTile = 16;

		bool Valid(HeightField const &terrain) {
			size_t const vertices = static_cast<size_t>(terrain.verticesX) * terrain.verticesZ;
			return terrain.verticesX >= 2 && terrain.verticesZ >= 2 && terrain.cellWidth > 0.0f &&
			       terrain.altitudes.size() == vertices &&
			       (terrain.flipped.empty() || terrain.flipped.size() == vertices);
		}

		void ToWorld(Map const &map, float a, float b, float &x, float &z) {
			// along and across are orthonormal, so the inverse is the transpose.
			x = a * map.along[0] + b * map.across[0];
			z = a * map.along[1] + b * map.across[1];
		}
	} // namespace

	float Altitude(HeightField const &terrain, float x, float z) {
		float const lastX = static_cast<float>(terrain.verticesX - 1);
		float const lastZ = static_cast<float>(terrain.verticesZ - 1);
		float const fx = (std::min)((std::max)(x / terrain.cellWidth, 0.0f), lastX);
		float const fz = (std::min)((std::max)(z / terrain.cellWidth, 0.0f), lastZ);
		uint32_t const x0 = (std::min)(static_cast<uint32_t>(fx), terrain.verticesX - 2);
		uint32_t const z0 = (std::min)(static_cast<uint32_t>(fz), terrain.verticesZ - 2);
		float const tx = fx - static_cast<float>(x0);
		float const tz = fz - static_cast<float>(z0);
		size_t const index = static_cast<size_t>(z0) * terrain.verticesX + x0;
		float const h00 = terrain.altitudes[index];
		float const h10 = terrain.altitudes[index + 1];
		float const h01 = terrain.altitudes[index + terrain.verticesX];
		float const h11 = terrain.altitudes[index + terrain.verticesX + 1];
		if (!terrain.flipped.empty() && terrain.flipped[index] != 0) {
			if (tx + tz <= 1.0f) return h00 + (h10 - h00) * tx + (h01 - h00) * tz;
			return h10 * (1.0f - tz) + h01 * (1.0f - tx) + h11 * (tx + tz - 1.0f);
		}
		// cSTETerrain::GetAltitude (0x00741260) splits unflipped cells this way.
		if (tx >= tz) return h00 + (h10 - h00) * tx + (h11 - h10) * tz;
		return h00 + (h11 - h01) * tx + (h01 - h00) * tz;
	}

	void VertexNormal(HeightField const &terrain, uint32_t x, uint32_t z, float normal[3]) {
		size_t const index = static_cast<size_t>(z) * terrain.verticesX + x;
		float value[3]{};
		if (terrain.normals.size() == terrain.altitudes.size() * 3) {
			for (unsigned axis = 0; axis < 3; ++axis) value[axis] = terrain.normals[index * 3 + axis];
		} else {
			uint32_t const left = x > 0 ? x - 1 : x, right = x + 1 < terrain.verticesX ? x + 1 : x;
			uint32_t const back = z > 0 ? z - 1 : z, front = z + 1 < terrain.verticesZ ? z + 1 : z;
			auto const at = [&](uint32_t column, uint32_t row) {
				return terrain.altitudes[static_cast<size_t>(row) * terrain.verticesX + column];
			};
			value[0] = -(at(right, z) - at(left, z)) / (static_cast<float>(right - left) * terrain.cellWidth);
			value[1] = 1.0f;
			value[2] = -(at(x, front) - at(x, back)) / (static_cast<float>(front - back) * terrain.cellWidth);
		}
		if (value[1] < 0.0f) for (float &component: value) component = -component;
		float const length = std::sqrt(value[0] * value[0] + value[1] * value[1] + value[2] * value[2]);
		if (!(length > 1.0e-6f) || !std::isfinite(length)) {
			normal[0] = 0.0f;
			normal[1] = 1.0f;
			normal[2] = 0.0f;
			return;
		}
		for (unsigned axis = 0; axis < 3; ++axis) normal[axis] = value[axis] / length;
	}

	bool Build(HeightField const &terrain, float const sun[3], float spacing, Map &map) {
		if (!Valid(terrain) || !(spacing > 0.0f)) return false;
		double const horizontal = std::sqrt(static_cast<double>(sun[0]) * sun[0] + static_cast<double>(sun[2]) * sun[2]);
		if (!(sun[1] < 0.0f) || !(horizontal > 1.0e-6)) return false;

		Map built;
		built.along[0] = static_cast<float>(sun[0] / horizontal);
		built.along[1] = static_cast<float>(sun[2] / horizontal);
		built.across[0] = -built.along[1];
		built.across[1] = built.along[0];
		built.spacing = spacing;
		float const drop = static_cast<float>(-sun[1] / horizontal);
		float const extentX = static_cast<float>(terrain.verticesX - 1) * terrain.cellWidth;
		float const extentZ = static_cast<float>(terrain.verticesZ - 1) * terrain.cellWidth;

		float low[2]{FLT_MAX, FLT_MAX}, high[2]{-FLT_MAX, -FLT_MAX};
		for (unsigned corner = 0; corner < 4; ++corner) {
			float const x = (corner & 1) ? extentX : 0.0f;
			float const z = (corner & 2) ? extentZ : 0.0f;
			float const axes[2]{
				x * built.along[0] + z * built.along[1], x * built.across[0] + z * built.across[1]
			};
			for (unsigned axis = 0; axis < 2; ++axis) {
				low[axis] = (std::min)(low[axis], axes[axis]);
				high[axis] = (std::max)(high[axis], axes[axis]);
			}
		}
		double const width = std::ceil((high[0] - low[0]) / spacing) + 1.0;
		double const height = std::ceil((high[1] - low[1]) / spacing) + 1.0;
		if (width > kMaxTexels || height > kMaxTexels) return false;
		built.width = static_cast<uint32_t>(width);
		built.height = static_cast<uint32_t>(height);
		built.originAlong = low[0];
		built.originAcross = low[1];
		built.ceiling.resize(static_cast<size_t>(built.width) * built.height);

		// Terrain beyond the city edge is not drawn, so it casts nothing; the
		// floor there follows the nearest edge so filtering stays smooth.
		float const tolerance = terrain.cellWidth * 1.0e-3f;
		float const step = spacing * drop;
		for (uint32_t row = 0; row < built.height; ++row) {
			float const b = built.originAcross + static_cast<float>(row) * spacing;
			float *const out = built.ceiling.data() + static_cast<size_t>(row) * built.width;
			float upstream = -FLT_MAX;
			for (uint32_t column = 0; column < built.width; ++column) {
				float const a = built.originAlong + static_cast<float>(column) * spacing;
				float x = 0.0f, z = 0.0f;
				ToWorld(built, a, b, x, z);
				float const ground = Altitude(terrain, x, z);
				out[column] = (std::max)(upstream, ground - kFloorDepth);
				bool const inside = x >= -tolerance && x <= extentX + tolerance && z >= -tolerance &&
				                    z <= extentZ + tolerance;
				if (inside) upstream = (std::max)(upstream, ground);
				if (upstream > -FLT_MAX) upstream -= step;
			}
		}
		map = std::move(built);
		return true;
	}

	float Ceiling(Map const &map, float x, float z) {
		if (map.width == 0 || map.height == 0) return -FLT_MAX;
		float const a = x * map.along[0] + z * map.along[1];
		float const b = x * map.across[0] + z * map.across[1];
		// Texel centres sit at integer coordinates; clamp as the sampler does.
		float const fa = (std::min)((std::max)((a - map.originAlong) / map.spacing, 0.0f),
		                            static_cast<float>(map.width - 1));
		float const fb = (std::min)((std::max)((b - map.originAcross) / map.spacing, 0.0f),
		                            static_cast<float>(map.height - 1));
		uint32_t const a0 = static_cast<uint32_t>(fa);
		uint32_t const b0 = static_cast<uint32_t>(fb);
		uint32_t const a1 = (std::min)(a0 + 1, map.width - 1);
		uint32_t const b1 = (std::min)(b0 + 1, map.height - 1);
		float const ta = fa - static_cast<float>(a0);
		float const tb = fb - static_cast<float>(b0);
		auto const at = [&](uint32_t column, uint32_t row) {
			return map.ceiling[static_cast<size_t>(row) * map.width + column];
		};
		float const lower = at(a0, b0) + (at(a1, b0) - at(a0, b0)) * ta;
		float const upper = at(a0, b1) + (at(a1, b1) - at(a0, b1)) * ta;
		return lower + (upper - lower) * tb;
	}

	bool SameLayout(Map const &a, Map const &b) {
		return a.width == b.width && a.height == b.height && a.spacing == b.spacing &&
		       a.originAlong == b.originAlong && a.originAcross == b.originAcross && a.along[0] == b.along[0] &&
		       a.along[1] == b.along[1] && a.across[0] == b.across[0] && a.across[1] == b.across[1] &&
		       a.ceiling.size() == b.ceiling.size();
	}

	void ChangedRegions(Map const &before, Map const &after, HeightField const &terrain, size_t maxRegions,
	                    std::vector<Region> &regions) {
		regions.clear();
		if (!SameLayout(before, after) || !Valid(terrain) || after.ceiling.empty()) return;
		float const extentX = static_cast<float>(terrain.verticesX - 1) * terrain.cellWidth;
		float const extentZ = static_cast<float>(terrain.verticesZ - 1) * terrain.cellWidth;
		uint32_t const tilesAlong = (after.width + kTile - 1) / kTile;
		uint32_t const tilesAcross = (after.height + kTile - 1) / kTile;
		std::vector<uint8_t> changed(static_cast<size_t>(tilesAlong) * tilesAcross, 0);
		for (uint32_t row = 0; row < after.height; ++row) {
			float const b = after.originAcross + static_cast<float>(row) * after.spacing;
			for (uint32_t column = 0; column < after.width; ++column) {
				size_t const index = static_cast<size_t>(row) * after.width + column;
				float const was = before.ceiling[index];
				float const now = after.ceiling[index];
				if (std::fabs(was - now) <= kChangeTolerance) continue;
				size_t const tile = static_cast<size_t>(row / kTile) * tilesAlong + column / kTile;
				if (changed[tile] != 0) continue;
				float x = 0.0f, z = 0.0f;
				ToWorld(after, after.originAlong + static_cast<float>(column) * after.spacing, b, x, z);
				if (x < 0.0f || x > extentX || z < 0.0f || z > extentZ) continue;
				if ((std::max)(was, now) < Altitude(terrain, x, z) - kReceiverSlack) continue;
				changed[tile] = 1;
			}
		}

		Region all{FLT_MAX, FLT_MAX, -FLT_MAX, -FLT_MAX};
		for (uint32_t tileRow = 0; tileRow < tilesAcross; ++tileRow) {
			for (uint32_t tileColumn = 0; tileColumn < tilesAlong; ++tileColumn) {
				if (changed[static_cast<size_t>(tileRow) * tilesAlong + tileColumn] == 0) continue;
				// Half a texel beyond the outermost centres: filtering reaches that far.
				float const a0 = after.originAlong + (static_cast<float>(tileColumn * kTile) - 0.5f) * after.spacing;
				float const b0 = after.originAcross + (static_cast<float>(tileRow * kTile) - 0.5f) * after.spacing;
				float const a1 = a0 + static_cast<float>(kTile) * after.spacing;
				float const b1 = b0 + static_cast<float>(kTile) * after.spacing;
				Region region{FLT_MAX, FLT_MAX, -FLT_MAX, -FLT_MAX};
				for (unsigned corner = 0; corner < 4; ++corner) {
					float x = 0.0f, z = 0.0f;
					ToWorld(after, (corner & 1) ? a1 : a0, (corner & 2) ? b1 : b0, x, z);
					region.minX = (std::min)(region.minX, x);
					region.minZ = (std::min)(region.minZ, z);
					region.maxX = (std::max)(region.maxX, x);
					region.maxZ = (std::max)(region.maxZ, z);
				}
				region.minX = (std::max)(region.minX, 0.0f);
				region.minZ = (std::max)(region.minZ, 0.0f);
				region.maxX = (std::min)(region.maxX, extentX);
				region.maxZ = (std::min)(region.maxZ, extentZ);
				all.minX = (std::min)(all.minX, region.minX);
				all.minZ = (std::min)(all.minZ, region.minZ);
				all.maxX = (std::max)(all.maxX, region.maxX);
				all.maxZ = (std::max)(all.maxZ, region.maxZ);
				regions.push_back(region);
			}
		}
		if (regions.size() > maxRegions) regions.assign(1, all);
	}
} // namespace scvk::TerrainShadowMap
