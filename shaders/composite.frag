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
 * The shadow is multiplied into the scene: it takes the sun's direct light away and keeps
 * the ground's own colour, tinted by SimCity 4's shadow colour. Blending towards that
 * colour instead washed large terrain shadows out in blue.
 *
 * Each pixel's view position is rebuilt from the depth through the pass's own
 * projection. A shadow takes away the sun's direct light, so a face turned partly away
 * from the sun is darkened less and one turned fully away not at all; terrain pixels use
 * the vertex normals SimCity 4 lights the terrain with rather than their facets'.
 *
 * A prerendered model has its shading and its own shadows painted into its texture, and
 * its depth is a box around the picture that ignores how the picture is turned. The driver
 * marks its pixels in the stencil, and they take the shadow as flat ground does and never
 * take their own model's.
 *
 * The depth is Direct3D's, 0 to 1, and the shadow map is laid out as SCD3D11's: the
 * caster pass flips y so that both read the same.
 */

#version 450

layout(set = 0, binding = 0, std140) uniform CompositeConstants
{
	mat4 lightMatrix;
	vec4 material;      // depth bias in light space, normal offset in view units, occupant box margin, occupant boxes bound
	vec4 projection0;   // p[0], p[5], p[10], p[12]
	vec4 projection1;   // p[13], p[14], shadow map texel, opacity
	vec4 viewport;      // the pass's viewport: x, y, width, height, from the top left
	vec4 tone;          // the shadow colour
	vec4 sun;           // the direction light travels in view space; w: 1 / N.L of flat ground
	mat4 eyeToWorld;    // view space to world
	vec4 terrainAxes;   // world xz to the map's along (xy) and across (zw) axes
	vec4 terrainGrid;   // map uv = (along * x + y, across * z + w)
	vec4 terrainShade;  // terrain map on, bias, minimum penumbra, shadow map on
	vec4 terrainCell;   // cell width, how close to the terrain a terrain pixel lies, vertices x, z
} constants;

layout(set = 0, binding = 1) uniform sampler2D sceneDepth;
layout(set = 0, binding = 2) uniform sampler2D shadowMap;
layout(set = 0, binding = 3) uniform sampler2D terrainCeiling;    // ceiling, umbra
layout(set = 0, binding = 4) uniform sampler2D terrainVertices;   // altitude, normal x, normal z, cell flipped
layout(set = 0, binding = 5) uniform usampler2D sceneStencil;     // the depth buffer's stencil
layout(set = 0, binding = 6) uniform usampler2D shadowOccupants;  // each map texel's caster's occupant number, 0 for none

// Occupant n's world box: its low corner at 2n - 2 and its high corner at 2n - 1
layout(set = 0, binding = 7, std430) readonly buffer OccupantBoxes
{
	vec4 corners[];
} occupantBoxes;

layout(location = 0) out vec4 outColour;

// The stencil bit the driver sets on a prerendered model's pixels. SC4RenderServices keeps
// its decal numbers in the seven bits below it.
const uint PAINTED_STENCIL_BIT = 0x80u;

// A pixel further than this above the drawn terrain is moved off its surface before the
// shadow lookup. The terrain casts nothing into the map, and the registry casters' ground
// quads lie 0.2 above it, so the terrain stays where it is.
const float TERRAIN_RECEIVER_GAP = 0.3;

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

// Whether the caster in a shadow map texel is the occupant a painted pixel belongs to: the
// texel keeps its caster's occupant number, and a pixel inside that occupant's box, with a
// margin for the depth buffer's steps, is the occupant's own. A number past the boxes
// handed over is read as no occupant rather than past the buffer's end.
bool IsOwnOccupant(ivec2 texel, vec3 world)
{
	uint occupant = texelFetch(shadowOccupants, texel, 0).r;
	if (occupant == 0u || occupant * 2u > uint(occupantBoxes.corners.length()))
	{
		return false;
	}

	vec3 low  = occupantBoxes.corners[occupant * 2u - 2u].xyz - constants.material.z;
	vec3 high = occupantBoxes.corners[occupant * 2u - 1u].xyz + constants.material.z;
	return all(greaterThanEqual(world, low)) && all(lessThanEqual(world, high));
}

// The ceiling and the umbra filtered bilinearly and clamped to the edge, worked out here
// because linear filtering of 32-bit float images is not something every device offers.
vec2 TerrainCeiling(vec2 uv)
{
	ivec2 size   = textureSize(terrainCeiling, 0);
	vec2  texel  = uv * vec2(size) - 0.5;
	ivec2 origin = ivec2(floor(texel));
	vec2  f      = texel - vec2(origin);

	ivec2 last = size - 1;
	vec2 c00 = texelFetch(terrainCeiling, clamp(origin, ivec2(0), last), 0).rg;
	vec2 c10 = texelFetch(terrainCeiling, clamp(origin + ivec2(1, 0), ivec2(0), last), 0).rg;
	vec2 c01 = texelFetch(terrainCeiling, clamp(origin + ivec2(0, 1), ivec2(0), last), 0).rg;
	vec2 c11 = texelFetch(terrainCeiling, clamp(origin + ivec2(1, 1), ivec2(0), last), 0).rg;

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
	vec3 world = (constants.eyeToWorld * vec4(viewPosition, 1.0)).xyz;

	// Beyond the city's edge lies the game's backdrop, below the terrain and so under its
	// shadow ceiling. Nothing out there casts, and shading it only leaves patches where
	// edge strips are redrawn without the composite.
	vec2 cityExtent = (constants.terrainCell.zw - 1.0) * constants.terrainCell.x;
	if (isTerrainMapUsed && (any(lessThan(world.xz, vec2(-1.0))) || any(greaterThan(world.xz, cityExtent + 1.0))))
	{
		discard;
	}

	bool isPainted = (texelFetch(sceneStencil, pixel, 0).r & PAINTED_STENCIL_BIT) != 0u;

	// The surface's normal, rebuilt from the neighbouring depths, facing the camera
	vec3 surfaceNormal = normalize(cross(SurfaceStep(pixel, viewPosition, ivec2(0, 1)), SurfaceStep(pixel, viewPosition, ivec2(1, 0))));
	if (surfaceNormal.z < 0.0)
	{
		surfaceNormal = -surfaceNormal;
	}

	// How far the pixel lies from the drawn terrain, and the game's lighting normal there
	vec3  vertexNormal = vec3(0.0, 1.0, 0.0);
	float terrainGap   = 1.0e9;

	if (isTerrainMapUsed)
	{
		terrainGap = abs(world.y - TerrainSurface(world.xz, vertexNormal));
	}

	// Darken fully where N.L is that of flat ground, what SimCity 4's shadow strength is
	// set for, less on a face turned partly away, and not at all on one turned fully away.
	// A painted pixel's faces say nothing about its picture, so it counts as flat ground.
	float light = isPainted ? 1.0 / constants.sun.w : dot(surfaceNormal, -constants.sun.xyz);

	// The terrain is lit through vertex normals interpolated across each triangle. The
	// facets' own normals would switch the shadow on and off along every crease, so
	// terrain pixels, and painted ones on the terrain, use the game's.
	mat3 toWorld     = mat3(constants.eyeToWorld);
	vec3 worldNormal = toWorld * surfaceNormal;

	if (terrainGap < constants.terrainCell.y && (isPainted || worldNormal.y > 0.3 * length(worldNormal)))
	{
		light = dot(vertexNormal, -normalize(toWorld * constants.sun.xyz));
	}

	float facing = clamp(light * constants.sun.w, 0.0, 1.0);
	if (facing < 0.001)
	{
		discard;
	}

	// The casters' shadow
	float shade = 0.0;

	if (constants.terrainShade.w != 0.0)
	{
		// Look the shadow up from off the surface, along its normal
		//
		// A surface that casts finds its own depth in the map, one filter tap away in any
		// direction, and shadowed itself in stripes. Moved out along its normal by as much
		// as its slope to the light needs, it no longer does, while a real caster above it
		// still covers it. A face turned straight at the sun does not move at all.
		vec3 receiver = viewPosition;

		if (terrainGap > TERRAIN_RECEIVER_GAP)
		{
			float facingSun = clamp(dot(surfaceNormal, -constants.sun.xyz), 0.0, 1.0);
			receiver += surfaceNormal * (constants.material.y * sqrt(1.0 - facingSun * facingSun));
		}

		vec4 lightPosition = constants.lightMatrix * vec4(receiver, 1.0);
		vec2 shadowUV      = lightPosition.xy * vec2(0.5, -0.5) + 0.5;

		if (all(greaterThanEqual(shadowUV, vec2(0.0))) && all(lessThanEqual(shadowUV, vec2(1.0))) && lightPosition.z >= 0.0 && lightPosition.z <= 1.0)
		{
			// Filter over a box three texels wide around the receiver
			//
			// Each texel counts by how much of it the box covers. The outer ones are only
			// partly in, so the shadow fades smoothly across texels instead of stepping, and
			// the weights add up to nine.
			ivec2 size    = textureSize(shadowMap, 0);
			vec2  centre  = shadowUV * vec2(size) - 0.5;
			ivec2 first   = ivec2(floor(centre)) - 1;
			vec2  part    = centre - floor(centre);
			vec4  weightX = vec4(1.0 - part.x, 1.0, 1.0, part.x);
			vec4  weightY = vec4(1.0 - part.y, 1.0, 1.0, part.y);

			// A prerendered model never shadows itself, since its picture already does
			bool isSelfTested = isPainted && constants.material.w != 0.0;

			for (int y = 0; y < 4; y++)
			{
				for (int x = 0; x < 4; x++)
				{
					ivec2 tap = clamp(first + ivec2(x, y), ivec2(0), size - 1);
					bool  isBehind = lightPosition.z > texelFetch(shadowMap, tap, 0).r + constants.material.x;

					if (isBehind && !(isSelfTested && IsOwnOccupant(tap, world)))
					{
						shade += weightX[x] * weightY[y];
					}
				}
			}

			shade /= 9.0;
		}
	}

	// The terrain's own shadow: a point below the ceiling the map holds for its (x, z),
	// fading in over the penumbra down to the umbra, which widens with the distance from the
	// ridge. The larger of the two shadows wins, so they never stack.
	if (isTerrainMapUsed)
	{
		vec2  axes     = vec2(dot(world.xz, constants.terrainAxes.xy), dot(world.xz, constants.terrainAxes.zw));
		vec2  ceiling  = TerrainCeiling(axes * constants.terrainGrid.xz + constants.terrainGrid.yw);
		float penumbra = max(ceiling.x - ceiling.y, constants.terrainShade.z);
		shade = max(shade, clamp((ceiling.x - world.y - constants.terrainShade.y) / penumbra, 0.0, 1.0));
	}

	shade *= facing;
	if (shade < 0.001)
	{
		discard;
	}

	outColour = vec4(mix(vec3(1.0), constants.tone.rgb, shade * constants.projection1.w), 1.0);
}
