/*
 * scvk - a native Vulkan renderer for SimCity 4
 *
 * Copyright (C) 2026 aspctt
 * SPDX-License-Identifier: LGPL-2.1-or-later
 *
 * Vertex stage for the fixed function geometry path.
 *
 * SimCity 4 has a dozen vertex formats and they do not agree on which attributes are
 * present: V3F_C4UB has a colour and no texture coordinate, V3F_T2F has the reverse,
 * V3F_C4UB_T2F has both, and V3F has neither. The terrain adds a second coordinate set on
 * top of that. A shader may not declare an input the pipeline does not supply, so this is
 * compiled once per combination and the backend picks the variant matching the format it
 * was given.
 *
 * Every variant emits the same varyings, so one fragment shader serves all of them.
 */

#version 450

//// Constants

// The path value from which the draw's coordinates are generated from the eye-space
// position rather than read from the vertex.
const float PATH_GENERATED_COORDINATES = 2.5;

// The texture environment mode carries 8 on top when the primary colour's alpha comes from
// the vertex, so anything from here up has the flag.
const float ALPHA_FROM_VERTEX_THRESHOLD = 7.5;

//// References

// Must match the fragment stage exactly: a push constant block is shared across the
// stages that declare it.
layout(push_constant) uniform PushConstants
{
	// Projection times modelview, already combined on the CPU and corrected for Vulkan's
	// clip space.
	mat4 modelViewProjection;

	// Used only by the fragment stage, apart from two parts. z carries the alpha source
	// flag on top of the environment mode. w selects how the two aliased slots below are
	// read:
	//   1 one texture stage, coordinates from the vertex
	//   2 two texture stages, coordinates from the vertex
	//   3 one texture stage, coordinates generated from the eye-space position
	vec4 fragmentState;

	// Two slots with two meanings, because the push constant block is at the 128 byte
	// guaranteed minimum and both meanings will not fit side by side.
	//
	// Path 2 reads them as the combiner network and the environment colour. Path 3 reads
	// them as the two rows of the texture generation matrix that matter for a 2D sample.
	// The two are mutually exclusive in the interface: generated coordinates arrive on a
	// single stage pass, and the combiner only means anything when a second stage is live.
	vec4 aliasA;
	vec4 aliasB;

	vec4 sceneTint;
} push;

layout(location = 0) in vec3 inPosition;
#if SCVK_HAS_COLOUR
layout(location = 1) in vec4 inColour;
#endif
#if SCVK_TEXTURE_COORDINATE_SETS >= 1
layout(location = 2) in vec2 inTextureCoordinate0;
#endif
#if SCVK_TEXTURE_COORDINATE_SETS >= 2
layout(location = 3) in vec2 inTextureCoordinate1;
#endif

layout(location = 0) out vec4 fragmentColour;
layout(location = 1) out vec2 fragmentTextureCoordinate0;
layout(location = 2) out vec2 fragmentTextureCoordinate1;

// The terrain is drawn in several passes over the same geometry, each on its own pipeline,
// and each later pass depth tests against the first. OpenGL's fixed function transform is
// invariant across such passes; a shader output is only guaranteed to be when declared so.
// Without it a later pass can land a hair behind the first and fail the test, leaving the
// first pass showing.
invariant gl_Position;

//// Entry Point

void main()
{
	gl_Position = push.modelViewProjection * vec4(inPosition, 1.0);

	// Read the vertex colour
	//
	// The game packs vertex colours as BGRA, which is why its OpenGL driver requires the
	// vertex_array_bgra extension. The attribute is declared R8G8B8A8 because that format
	// is universally supported for vertex buffers, so the swizzle happens here instead.
	// Geometry with no colour takes the fixed function current colour, which the game
	// never sets and which starts white.
#if SCVK_HAS_COLOUR
	vec4 vertexColour = inColour.bgra;
#else
	vec4 vertexColour = vec4(1.0);
#endif

	// Light it into the primary colour
	//
	// That is the colour the texture environment consumes. sceneTint.rgb is the light
	// weight the driver collapsed the ambient and diffuse terms into; the alpha comes from
	// the vertex when colour material maps it onto the diffuse material, and from the
	// alpha multiplier when it does not.
	fragmentColour = vec4(vertexColour.rgb * push.sceneTint.rgb, (push.fragmentState.z >= ALPHA_FROM_VERTEX_THRESHOLD) ? vertexColour.a : push.sceneTint.a);

	// Pass the first coordinate set on
	//
	// Geometry with no texture coordinate samples the 1x1 white default texture, so any
	// coordinate gives the same result.
#if SCVK_TEXTURE_COORDINATE_SETS >= 1
	fragmentTextureCoordinate0 = inTextureCoordinate0;
#else
	fragmentTextureCoordinate0 = vec2(0.0);
#endif

	// Or generate it from the camera-space position
	//
	// This is what the cloud shadows are drawn with: the shadow texture is projected
	// across the terrain and scrolled by the texture matrix rather than following the
	// terrain's own coordinates. Reading the vertex set instead stamps the texture once per
	// terrain cell, which is why the shadows were square. The two rows arrive already
	// multiplied through the modelview, so the object position is all that is needed here.
	if (push.fragmentState.w > PATH_GENERATED_COORDINATES)
	{
		vec4 position = vec4(inPosition, 1.0);
		fragmentTextureCoordinate0 = vec2(dot(push.aliasA, position), dot(push.aliasB, position));
	}

	// Pass the second coordinate set on
	//
	// A single set feeds both stages otherwise. Only geometry that carries two sets ever
	// has a second stage bound, so that is never the one sampled.
#if SCVK_TEXTURE_COORDINATE_SETS >= 2
	fragmentTextureCoordinate1 = inTextureCoordinate1;
#else
	fragmentTextureCoordinate1 = fragmentTextureCoordinate0;
#endif
}
