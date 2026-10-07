/*
 * scvk - a native Vulkan renderer for SimCity 4
 *
 * Copyright (C) 2026 aspctt
 * SPDX-License-Identifier: LGPL-3.0-or-later
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

// Thresholds on a stage's source in the draw record, halfway between the values the
// backend sends: 0 the first coordinate set, 1 the second, 2 the object position.
const float SOURCE_SECOND_SET_MINIMUM = 0.5;
const float SOURCE_POSITION_MINIMUM   = 1.5;

// The bits added to the texture environment mode when the primary colour's alpha, and its
// colour, come from the vertex rather than from the material.
const int ALPHA_FROM_VERTEX_FLAG  = 8;
const int COLOUR_FROM_VERTEX_FLAG = 16;

// Thresholds on the fog record's mode, halfway between the values the backend sends: 0
// off, 1 exponential, 2 squared exponential, 3 linear.
const float FOG_ENABLED_MINIMUM     = 0.5;
const float FOG_EXPONENTIAL_MAXIMUM = 1.5;
const float FOG_SQUARED_MAXIMUM     = 2.5;

//// References

// Must match the fragment stage exactly: a push constant block is shared across the
// stages that declare it.
layout(push_constant) uniform PushConstants
{
	// Projection times modelview, already combined on the CPU and corrected for Vulkan's
	// clip space.
	mat4 modelViewProjection;

	// Used only by the fragment stage, apart from z, which carries the colour and alpha
	// source flags on top of the environment mode.
	vec4 fragmentState;

	// Used only by the fragment stage.
	uvec4 combinerNetwork;
	vec4  environmentColour;

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

// The draw record, on a second binding read per instance, since the push constant block
// is at the 128 byte guaranteed minimum. Every draw is a single instance, so every vertex
// reads the same record.
//
// The fog's row gives the eye distance as a dot product with the position; its
// parameters hold the mode, the density, and the linear equation's scale and offset.
// Each stage has the two rows of its texture matrix that a 2D sample reads, and the
// sources say where each stage's input comes from.
layout(location = 4)  in vec4 inFogDistanceRow;
layout(location = 5)  in vec4 inFogParameters;
layout(location = 6)  in vec4 inFogColour;
layout(location = 7)  in vec4 inFirstStageRowS;
layout(location = 8)  in vec4 inFirstStageRowT;
layout(location = 9)  in vec4 inSecondStageRowS;
layout(location = 10) in vec4 inSecondStageRowT;
layout(location = 11) in vec4 inStageSources;

layout(location = 0) out vec4 fragmentColour;
layout(location = 1) out vec2 fragmentTextureCoordinate0;
layout(location = 2) out vec2 fragmentTextureCoordinate1;
layout(location = 3) flat out vec3 fragmentFogColour;
layout(location = 4) out float fragmentFogFactor;

// The terrain is drawn in several passes over the same geometry, each on its own pipeline,
// and each later pass depth tests against the first. OpenGL's fixed function transform is
// invariant across such passes; a shader output is only guaranteed to be when declared so.
// Without it a later pass can land a hair behind the first and fail the test, leaving the
// first pass showing.
invariant gl_Position;

//// Private Functions

// What a stage's rows apply to: the object position when the stage generates its
// coordinates, otherwise the coordinate set it names as (s, t, 0, 1). A set the format
// lacks falls back to the first. Geometry with no set at all samples the 1x1 white default
// texture, so any coordinate gives the same result.
vec4 stageInput(float source)
{
	if (source > SOURCE_POSITION_MINIMUM)
	{
		return vec4(inPosition, 1.0);
	}

#if SCVK_TEXTURE_COORDINATE_SETS >= 2
	if (source > SOURCE_SECOND_SET_MINIMUM)
	{
		return vec4(inTextureCoordinate1, 0.0, 1.0);
	}
#endif

#if SCVK_TEXTURE_COORDINATE_SETS >= 1
	return vec4(inTextureCoordinate0, 0.0, 1.0);
#else
	return vec4(0.0, 0.0, 0.0, 1.0);
#endif
}

//// Entry Point

void main()
{
	gl_Position = push.modelViewProjection * vec4(inPosition, 1.0);

	// Read the vertex colour
	//
	// The game packs vertex colours as BGRA, which is why its OpenGL driver requires the
	// vertex_array_bgra extension. The attribute is declared R8G8B8A8 because that format
	// is universally supported for vertex buffers, so the swizzle happens here instead.
	// Direct3D lights geometry with no colour from the material instead, which the game's
	// DirectX driver keeps white with the alpha multiplier as its alpha.
#if SCVK_HAS_COLOUR
	vec4 vertexColour = inColour.bgra;
#else
	vec4 vertexColour = vec4(1.0, 1.0, 1.0, push.sceneTint.a);
#endif

	// Light it into the primary colour
	//
	// That is the colour the texture environment consumes. sceneTint.rgb is the ambient
	// light, which scales the vertex colour where the game maps it onto the ambient
	// material and the white material where it does not. The alpha comes from the vertex
	// or from the alpha multiplier, as the driver decided. See cVKDriver::PushLighting.
	//
	// Fixed function lighting clamps the lit colour to 0 to 1 before texturing (OpenGL
	// 2.1 spec, section 2.14.6), and so does Direct3D. An ambient light above one would
	// otherwise brighten every modulated texture.
	int   lightingSources = int(push.fragmentState.z);
	vec3  materialColour  = ((lightingSources & COLOUR_FROM_VERTEX_FLAG) != 0) ? vertexColour.rgb : vec3(1.0);
	float materialAlpha   = ((lightingSources & ALPHA_FROM_VERTEX_FLAG) != 0) ? vertexColour.a : push.sceneTint.a;
	fragmentColour = clamp(vec4(materialColour * push.sceneTint.rgb, materialAlpha), 0.0, 1.0);

	// Work out each stage's coordinates
	//
	// Through the stage's texture matrix, which OpenGL applies to the coordinates the
	// vertex carries as well as to generated ones. The foundations rely on it: their
	// coordinates lie far outside their clamped textures until the matrix brings them
	// back. The rows are the identity when the game has set no matrix, which leaves the
	// coordinate exact.
	//
	// The cloud shadows generate theirs from the camera-space position instead: the shadow
	// texture is projected across the terrain and scrolled by the matrix rather than
	// following the terrain's own coordinates. Reading the vertex set stamped it once per
	// terrain cell, which is why the shadows were square. Generated rows arrive already
	// multiplied through the modelview, so the object position is all that is needed.
	//
	// Both stages on every draw. The second is only sampled when the draw runs it: the
	// building shadows' mask, the terrain's second set, and the window lights laid over a
	// building through its one set.
	vec4 firstStageInput  = stageInput(inStageSources.x);
	vec4 secondStageInput = stageInput(inStageSources.y);

	fragmentTextureCoordinate0 = vec2(dot(inFirstStageRowS, firstStageInput), dot(inFirstStageRowT, firstStageInput));
	fragmentTextureCoordinate1 = vec2(dot(inSecondStageRowS, secondStageInput), dot(inSecondStageRowT, secondStageInput));

	// Work out the fog factor
	//
	// Per vertex and interpolated, as the game's DirectX driver gets from Direct3D's vertex
	// fog, which OpenGL allows too (OpenGL 2.1 spec, section 3.10). The distance is the
	// eye-space depth in front of the camera, which is what Direct3D measures and what
	// OpenGL lets stand in for the true distance. The result is clamped to 0 to 1 in all
	// three equations, and one means no fog at all.
	float fogMode   = inFogParameters.x;
	float fogFactor = 1.0;

	if (fogMode > FOG_ENABLED_MINIMUM)
	{
		float distance = dot(inFogDistanceRow, vec4(inPosition, 1.0));
		float scaled   = inFogParameters.y * distance;

		if (fogMode < FOG_EXPONENTIAL_MAXIMUM)
		{
			fogFactor = exp(-scaled);
		}
		else if (fogMode < FOG_SQUARED_MAXIMUM)
		{
			fogFactor = exp(-scaled * scaled);
		}
		else
		{
			fogFactor = inFogParameters.z * distance + inFogParameters.w;
		}
	}

	fragmentFogFactor = clamp(fogFactor, 0.0, 1.0);
	fragmentFogColour = inFogColour.rgb;
}
