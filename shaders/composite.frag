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

/*
 * The shadow composite, SCD3D11's CompositePS: darkens the back buffer wherever the
 * scene depth lies in the shadow map's shadow or below the terrain's shadow ceiling.
 *
 * Each pixel's view position is rebuilt from the depth through the pass's own
 * projection. A shadow takes away the sun's direct light, so a face turned partly away
 * from the sun is darkened less and one turned fully away not at all; terrain pixels use
 * the vertex normals SimCity 4 lights the terrain with rather than their facets'.
 *
 * The depth is Direct3D's, 0 to 1, and the shadow map is laid out as SCD3D11's: the
 * caster pass flips y so that both read the same.
 */

#version 450

layout(set = 0, binding = 0, std140) uniform CompositeConstants
{
	mat4 lightMatrix;
	vec4 material;      // x: the depth bias in light space
	vec4 projection0;   // p[0], p[5], p[10], p[12]
	vec4 projection1;   // p[13], p[14], shadow map texel, opacity
	vec4 viewport;      // the pass's viewport: x, y, width, height, from the top left
	vec4 tone;          // the shadow colour
	vec4 sun;           // the direction light travels in view space; w: 1 / N.L of flat ground, 0 for all faces
	mat4 eyeToWorld;    // view space to world, for the terrain maps
	vec4 terrainAxes;   // world xz to the map's along (xy) and across (zw) axes
	vec4 terrainGrid;   // map uv = (along * x + y, across * z + w)
	vec4 terrainShade;  // terrain map on, bias, 1 / softness, shadow map on
	vec4 terrainCell;   // cell width, how close to the terrain a terrain pixel lies, vertices x, z
} constants;

layout(set = 0, binding = 1) uniform sampler2D sceneDepth;
layout(set = 0, binding = 2) uniform sampler2D shadowMap;
layout(set = 0, binding = 3) uniform sampler2D terrainCeiling;
layout(set = 0, binding = 4) uniform sampler2D terrainVertices; // altitude, normal x, normal z, cell flipped

layout(location = 0) out vec4 outColour;

float SceneDepth(ivec2 pixel)
{
	ivec2 size = textureSize(sceneDepth, 0);
	return texelFetch(sceneDepth, clamp(pixel, ivec2(0), size - 1), 0).r;
}

vec3 ViewPosition(ivec2 pixel, float depth)
{
	// A partial static pass renders its dirty rectangle through a projection fitted to
	// that rectangle, so NDC spans the viewport, not the window.
	vec2  uv   = (vec2(pixel) + 0.5 - constants.viewport.xy) / constants.viewport.zw;
	float ndcX = uv.x * 2.0 - 1.0;
	float ndcY = 1.0 - uv.y * 2.0;

	vec3 viewPosition;
	viewPosition.x = (ndcX - constants.projection0.w) / constants.projection0.x;
	viewPosition.y = (ndcY - constants.projection1.x) / constants.projection0.y;
	viewPosition.z = (depth * 2.0 - 1.0 - constants.projection1.y) / constants.projection0.z;
	return viewPosition;
}

// Of the two neighbours along one axis, the one on the same surface: the smaller depth
// step. Keeps silhouette edges from bending the normal.
vec3 SurfaceStep(ivec2 pixel, vec3 centre, ivec2 axis)
{
	vec3 forward  = ViewPosition(pixel + axis, SceneDepth(pixel + axis)) - centre;
	vec3 backward = centre - ViewPosition(pixel - axis, SceneDepth(pixel - axis));
	return (abs(forward.z) < abs(backward.z)) ? forward : backward;
}

vec4 TerrainVertex(ivec2 cell)
{
	return texelFetch(terrainVertices, cell, 0);
}

// The terrain under a world position as SimCity 4 draws it: the cell's two triangles,
// split along the diagonal its flip flag picks, with the vertex normals its lighting uses
// interpolated across them. Returns the altitude.
float TerrainSurface(vec2 xz, out vec3 normal)
{
	vec2  grid = xz / constants.terrainCell.x;
	ivec2 cell = clamp(ivec2(floor(grid)), ivec2(0), ivec2(constants.terrainCell.zw) - 2);
	vec2  f    = clamp(grid - vec2(cell), 0.0, 1.0);

	vec4 v00 = TerrainVertex(cell);
	vec4 v10 = TerrainVertex(cell + ivec2(1, 0));
	vec4 v01 = TerrainVertex(cell + ivec2(0, 1));
	vec4 v11 = TerrainVertex(cell + ivec2(1, 1));

	vec4 v;
	if (v00.w != 0.0)
	{
		v = (f.x + f.y <= 1.0) ? v00 * (1.0 - f.x - f.y) + v10 * f.x + v01 * f.y
		                       : v11 * (f.x + f.y - 1.0) + v01 * (1.0 - f.x) + v10 * (1.0 - f.y);
	}
	else
	{
		v = (f.x >= f.y) ? v00 * (1.0 - f.x) + v10 * (f.x - f.y) + v11 * f.y
		                 : v00 * (1.0 - f.y) + v01 * (f.y - f.x) + v11 * f.x;
	}

	normal = normalize(vec3(v.y, sqrt(clamp(1.0 - v.y * v.y - v.z * v.z, 0.0, 1.0)), v.z));
	return v.x;
}

// The ceiling filtered bilinearly and clamped to its edge, worked out here because linear
// filtering of 32-bit float images is not something every device offers.
float TerrainCeiling(vec2 uv)
{
	ivec2 size   = textureSize(terrainCeiling, 0);
	vec2  texel  = uv * vec2(size) - 0.5;
	ivec2 origin = ivec2(floor(texel));
	vec2  f      = texel - vec2(origin);

	ivec2 last = size - 1;
	float c00 = texelFetch(terrainCeiling, clamp(origin, ivec2(0), last), 0).r;
	float c10 = texelFetch(terrainCeiling, clamp(origin + ivec2(1, 0), ivec2(0), last), 0).r;
	float c01 = texelFetch(terrainCeiling, clamp(origin + ivec2(0, 1), ivec2(0), last), 0).r;
	float c11 = texelFetch(terrainCeiling, clamp(origin + ivec2(1, 1), ivec2(0), last), 0).r;

	return mix(mix(c00, c10, f.x), mix(c01, c11, f.x), f.y);
}

void main()
{
	ivec2 pixel = ivec2(gl_FragCoord.xy);
	float depth = SceneDepth(pixel);

	if (depth >= 0.999999)
	{
		discard;
	}

	vec3 viewPosition = ViewPosition(pixel, depth);
	bool isTerrainMapUsed = constants.terrainShade.x != 0.0;

	// The surface's normal, rebuilt from the neighbouring depths, facing the camera
	vec3 surfaceNormal = normalize(cross(SurfaceStep(pixel, viewPosition, ivec2(0, 1)), SurfaceStep(pixel, viewPosition, ivec2(1, 0))));
	if (surfaceNormal.z < 0.0)
	{
		surfaceNormal = -surfaceNormal;
	}
	vec3 world = isTerrainMapUsed ? (constants.eyeToWorld * vec4(viewPosition, 1.0)).xyz : vec3(0.0);

	// Darken fully where N.L is that of flat ground, what SimCity 4's shadow strength is
	// set for, less on a face turned partly away, and not at all on one turned fully away
	float facing = 1.0;

	if (constants.sun.w != 0.0)
	{
		vec3 normal = surfaceNormal;

		float light = dot(normal, -constants.sun.xyz);

		if (isTerrainMapUsed)
		{
			vec3  vertexNormal;
			float ground      = TerrainSurface(world.xz, vertexNormal);
			mat3  toWorld     = mat3(constants.eyeToWorld);
			vec3  worldNormal = toWorld * normal;

			if (abs(world.y - ground) < constants.terrainCell.y && worldNormal.y > 0.3 * length(worldNormal))
			{
				light = dot(vertexNormal, -normalize(toWorld * constants.sun.xyz));
			}
		}

		facing = clamp(light * constants.sun.w, 0.0, 1.0);
		if (facing < 0.001)
		{
			discard;
		}
	}

	// The casters' shadow, filtered over the nine texels around the pixel's
	float shade = 0.0;

	if (constants.terrainShade.w != 0.0)
	{
		// Look the shadow up from just off the surface, along its normal
		//
		// A caster is also a receiver: a raised road deck or a ramp lies in the shadow
		// map as well as on screen, and compared with its own depth, rounded to texels
		// a few metres wide, it shadowed itself in bands on the side facing the sun.
		// Pushing the lookup out by one and a half texels, the reach of the 3x3 filter,
		// keeps every sample off the surface the pixel is on, while a real caster above
		// it still covers it. One texel is 2 / size in the light's clip space, which the
		// light matrix's first row scales from view space.
		float texelWorld = 2.0 * constants.projection1.z / length(vec3(constants.lightMatrix[0][0], constants.lightMatrix[1][0], constants.lightMatrix[2][0]));
		vec3  lookup     = viewPosition + surfaceNormal * (1.5 * texelWorld);

		vec4 light    = constants.lightMatrix * vec4(lookup, 1.0);
		vec2 shadowUV = light.xy * vec2(0.5, -0.5) + 0.5;

		if (all(greaterThanEqual(shadowUV, vec2(0.0))) && all(lessThanEqual(shadowUV, vec2(1.0))) && light.z >= 0.0 && light.z <= 1.0)
		{
			for (int y = -1; y <= 1; y++)
			{
				for (int x = -1; x <= 1; x++)
				{
					float caster = textureLod(shadowMap, shadowUV + vec2(x, y) * constants.projection1.z, 0.0).r;
					shade += (light.z > caster + constants.material.x) ? 1.0 : 0.0;
				}
			}

			shade /= 9.0;
		}
	}

	// The terrain's own shadow: a point below the ceiling the map holds for its (x, z).
	// The larger of the two shadows wins, so they never stack.
	if (isTerrainMapUsed)
	{
		vec2  axes    = vec2(dot(world.xz, constants.terrainAxes.xy), dot(world.xz, constants.terrainAxes.zw));
		float ceiling = TerrainCeiling(axes * constants.terrainGrid.xz + constants.terrainGrid.yw);
		shade = max(shade, clamp((ceiling - world.y - constants.terrainShade.y) * constants.terrainShade.z, 0.0, 1.0));
	}

	shade *= facing;
	if (shade < 0.001)
	{
		discard;
	}

	outColour = vec4(constants.tone.rgb, shade * constants.projection1.w);
}
