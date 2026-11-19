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
 * Water reflections in screen space: each water pixel follows the view ray mirrored in the
 * water up the screen through the scene depth, and where it passes behind a surface takes
 * that surface's colour from a copy of the frame, blended over the water.
 *
 * SimCity 4's water is one level surface, so a pixel is water when its depth puts it on
 * that level. The stencil cannot hold a mark for it: SC4RenderServices clears every bit
 * of it for its terrain decals once the water is drawn.
 *
 * The camera is orthographic, so a pixel's view position is an affine function of its
 * coordinates and depth: the position of the top left corner at depth 0, plus the view
 * units per pixel and per unit of depth. The water is level, so its normal is the world's
 * up in view space, and a point's height is its position along it.
 *
 * Still water mirrors a pylon as a pylon of the same length hanging from the waterline,
 * which read as the pylon itself carrying on into the water. Small waves tilt the normal a
 * little, by a few sines over the world's ground plane, so a reflection wavers more the
 * further it lies from the waterline, as on real water. They stand still, as the game's
 * own water does: drifting over it they looked out of place.
 *
 * Only what the camera sees can be reflected. A ray that reaches a surface from behind,
 * the underside of a bridge deck that the picture shows from above, takes the surface's
 * colour darkened, as an underside in its own shade would be.
 */

#version 450

layout(set = 0, binding = 0, std140) uniform ReflectionConstants
{
	vec4 origin;      // the view position of the top left corner at depth 0; the water's height along up
	vec4 scale;       // view units per pixel right, per pixel down, per unit of depth; strength
	vec4 up;          // the world's up in view space, facing the camera; the longest march, as a share of the height
	vec4 options;     // 1 to show the water mask instead; the least thickness a surface has; how far off the water's height a pixel still counts as water; the shade of an underside
	vec4 axisX;       // the world's x axis in view space
	vec4 axisZ;       // the world's z axis in view space; how far the waves tilt the normal
	vec4 translation; // the view position of the world's origin
} constants;

layout(set = 0, binding = 1) uniform sampler2D sceneDepth;
layout(set = 0, binding = 2) uniform sampler2D sceneColour;

layout(location = 0) out vec4 outColour;

// The march: at most this many steps over the longest march, none shorter than the
// least step, then the halvings that narrow down a crossing
const int   MAXIMUM_STEPS = 160;
const float LEAST_STEP    = 1.5;
const int   REFINEMENTS   = 6;

// The share of the screen's height over which a reflection fades out towards an edge,
// and the share of the march after which it fades towards the march's end
const float EDGE_FADE  = 0.06;
const float MARCH_FADE = 0.75;

// The depth the game clears to, behind everything it draws
const float BACKGROUND_DEPTH = 0.999999;

// How far a hit surface may face away from the ray before it counts as seen from behind
const float BACK_FACING = 0.2;

float SceneDepth(vec2 pixel)
{
	ivec2 size = textureSize(sceneDepth, 0);
	return texelFetch(sceneDepth, clamp(ivec2(pixel), ivec2(0), size - 1), 0).r;
}

vec3 ViewPosition(vec2 pixel, float depth)
{
	return constants.origin.xyz + vec3(pixel * constants.scale.xy, depth * constants.scale.z);
}

// Whether a pixel lies on the water's level, which nothing but the water and the shore
// right at its edge does
bool IsWater(vec2 pixel, float depth)
{
	if (depth >= BACKGROUND_DEPTH)
	{
		return false;
	}

	float height = dot(ViewPosition(pixel, depth), constants.up.xyz) - constants.origin.w;
	return abs(height) <= constants.options.z;
}

// The surface normal at a pixel, rebuilt from the neighbouring depths and facing the
// camera. Of each pair of neighbours the one on the same surface, the smaller depth step,
// keeps silhouette edges from bending it.
vec3 SurfaceNormal(vec2 pixel)
{
	vec3 centre = ViewPosition(pixel, SceneDepth(pixel));

	vec3 right = ViewPosition(pixel + vec2(1.0, 0.0), SceneDepth(pixel + vec2(1.0, 0.0))) - centre;
	vec3 left  = centre - ViewPosition(pixel - vec2(1.0, 0.0), SceneDepth(pixel - vec2(1.0, 0.0)));
	vec3 down  = ViewPosition(pixel + vec2(0.0, 1.0), SceneDepth(pixel + vec2(0.0, 1.0))) - centre;
	vec3 up    = centre - ViewPosition(pixel - vec2(0.0, 1.0), SceneDepth(pixel - vec2(0.0, 1.0)));

	vec3 across = (abs(right.z) < abs(left.z)) ? right : left;
	vec3 along  = (abs(down.z) < abs(up.z)) ? down : up;

	vec3 normal = normalize(cross(along, across));
	return (normal.z < 0.0) ? -normal : normal;
}

// The water's normal at a view position, tilted by waves over the ground plane
vec3 WaterNormal(vec3 viewPosition)
{
	vec3 relative = viewPosition - constants.translation.xyz;
	vec2 ground   = vec2(dot(relative, constants.axisX.xyz), dot(relative, constants.axisZ.xyz));

	// A few sines at unrelated angles and wavelengths of several metres, so no pattern
	// repeats in view
	vec2 slope;
	slope.x = 0.55 * sin(0.61 * ground.x + 0.23 * ground.y)
	        + 0.30 * sin(1.37 * ground.x - 0.71 * ground.y + 1.7)
	        + 0.15 * sin(2.93 * ground.x + 1.91 * ground.y + 2.3);
	slope.y = 0.55 * sin(0.47 * ground.y - 0.37 * ground.x + 0.9)
	        + 0.30 * sin(1.21 * ground.y + 0.83 * ground.x + 1.5)
	        + 0.15 * sin(3.11 * ground.y - 1.57 * ground.x - 2.1);

	vec3 tilt = constants.axisZ.w * (slope.x * constants.axisX.xyz + slope.y * constants.axisZ.xyz);
	return normalize(constants.up.xyz + tilt);
}

void main()
{
	vec2  pixel      = gl_FragCoord.xy;
	float startDepth = SceneDepth(pixel);

	if (!IsWater(pixel, startDepth))
	{
		discard;
	}

	if (constants.options.x != 0.0)
	{
		outColour = vec4(1.0, 0.0, 1.0, 0.5);
		return;
	}

	// The view ray mirrored in the water. An orthographic camera looks down -z
	// everywhere.
	vec3 reflected = reflect(vec3(0.0, 0.0, -1.0), WaterNormal(ViewPosition(pixel, startDepth)));

	// Pixels moved per view unit along the ray
	vec2  pixelsPerUnit = reflected.xy / constants.scale.xy;
	float pixelRate     = length(pixelsPerUnit);
	if (pixelRate < 1.0e-6)
	{
		discard;
	}

	// Steps long enough to cover the longest march in the steps there are
	vec2  size         = vec2(textureSize(sceneDepth, 0));
	float marchPixels  = constants.up.w * size.y;
	float stepPixels   = max(LEAST_STEP, marchPixels / float(MAXIMUM_STEPS));
	int   steps        = int(marchPixels / stepPixels);
	float unitsPerStep = stepPixels / pixelRate;
	vec2  pixelStep    = pixelsPerUnit * unitsPerStep;
	float depthStep    = reflected.z * unitsPerStep;

	// A surface counts as hit up to this far behind it. A step can carry the ray well past
	// a steep face at the widest zooms, where a pixel spans metres, so it grows with the
	// step.
	float thickness = max(constants.options.y, 4.0 * unitsPerStep);

	// How far in front of the ray a surface is, in view units: positive once the ray has
	// passed behind it
	float lastAhead = -1.0;
	float hitStep   = -1.0;

	for (int index = 1; index <= steps; index++)
	{
		vec2 samplePixel = pixel + pixelStep * float(index);
		if (any(lessThan(samplePixel, vec2(0.0))) || any(greaterThanEqual(samplePixel, size)))
		{
			break;
		}

		float sampleDepth = SceneDepth(samplePixel);
		float sceneOffset = (sampleDepth - startDepth) * constants.scale.z;
		float ahead       = sceneOffset - depthStep * float(index);

		// A crossing from in front to behind, close enough to be the surface rather than
		// something far in front that the ray slips past
		if (ahead >= 0.0 && lastAhead < 0.0 && ahead <= thickness && !IsWater(samplePixel, sampleDepth))
		{
			hitStep = float(index);
			break;
		}

		lastAhead = ahead;
	}

	if (hitStep < 0.0)
	{
		discard;
	}

	// Narrow the crossing down between the last step in front and the first behind
	float low  = hitStep - 1.0;
	float high = hitStep;

	for (int index = 0; index < REFINEMENTS; index++)
	{
		float middle      = (low + high) * 0.5;
		float sceneOffset = (SceneDepth(pixel + pixelStep * middle) - startDepth) * constants.scale.z;

		if (sceneOffset - depthStep * middle >= 0.0)
		{
			high = middle;
		}
		else
		{
			low = middle;
		}
	}

	vec2 hitPixel = pixel + pixelStep * high;
	vec3 colour   = texture(sceneColour, hitPixel / size).rgb;

	// A surface the ray reaches from behind shows its underside, in its own shade
	if (dot(SurfaceNormal(hitPixel), reflected) > BACK_FACING)
	{
		colour *= constants.options.w;
	}

	// Fade towards the end of the march and the screen's edges, so a reflection never
	// stops at a hard line
	float travelled = high / float(steps);
	float edge      = EDGE_FADE * size.y;
	float edgeFade  = clamp(min(min(hitPixel.y, size.y - hitPixel.y), min(hitPixel.x, size.x - hitPixel.x)) / edge, 0.0, 1.0);
	float fade      = (1.0 - smoothstep(MARCH_FADE, 1.0, travelled)) * edgeFade;

	outColour = vec4(colour, constants.scale.w * fade);
}
