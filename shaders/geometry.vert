/*
 * scvk - a native Vulkan renderer for SimCity 4
 *
 * Copyright (C) 2026 aspctt
 * SPDX-License-Identifier: LGPL-2.1-or-later
 *
 * Vertex stage for the fixed function geometry path.
 *
 * SimCity 4 has a dozen vertex formats and they do not agree on which
 * attributes are present: V3F_C4UB has a colour and no texture coordinate,
 * V3F_T2F has the reverse, V3F_C4UB_T2F has both, and V3F has neither. The
 * terrain adds a second coordinate set on top of that. A shader may not
 * declare an input the pipeline does not supply, so this is compiled once per
 * combination and the backend picks the variant matching the format it was
 * given.
 *
 * Every variant emits the same varyings, so one fragment shader serves all of
 * them.
 */

#version 450

// Must match the fragment stage exactly: a push constant block is shared
// across the stages that declare it.
layout(push_constant) uniform Push
{
    // Projection times modelview, already combined on the CPU and corrected
    // for Vulkan's clip space.
    mat4 mvp;

    // Used only by the fragment stage, declared here to keep the blocks
    // identical.
    // w selects how the two aliased slots below are read:
    //   1 one texture stage, coordinates from the vertex
    //   2 two texture stages, coordinates from the vertex
    //   3 one texture stage, coordinates generated from the eye-space position
    vec4  fragmentState;

    // Two slots with two meanings, because the push constant block is at the
    // 128 byte guaranteed minimum and both meanings will not fit side by side.
    //
    // Mode 2 reads them as the combiner network and the environment colour.
    // Mode 3 reads them as the two rows of the texture generation matrix that
    // matter for a 2D sample. The two are mutually exclusive in the interface:
    // generated coordinates arrive on a single stage pass, and the combiner
    // only means anything when a second stage is live.
    vec4  aliasA;
    vec4  aliasB;

    vec4  sceneTint;
} push;

layout(location = 0) in vec3 inPosition;
#if SCVK_HAS_COLOUR
layout(location = 1) in vec4 inColour;
#endif
#if SCVK_TEXCOORD_SETS >= 1
layout(location = 2) in vec2 inTexCoord0;
#endif
#if SCVK_TEXCOORD_SETS >= 2
layout(location = 3) in vec2 inTexCoord1;
#endif

layout(location = 0) out vec4 fragColour;
layout(location = 1) out vec2 fragTexCoord0;
layout(location = 2) out vec2 fragTexCoord1;

// The terrain is drawn in several passes over the same geometry, each on its
// own pipeline, and each later pass depth tests against the first. OpenGL's
// fixed function transform is invariant across such passes; a shader output
// is only guaranteed to be when declared so. Without it a later pass can land
// a hair behind the first and fail the test, leaving the first pass showing.
invariant gl_Position;

void main()
{
    gl_Position = push.mvp * vec4(inPosition, 1.0);

#if SCVK_HAS_COLOUR
    // The game packs vertex colours as BGRA, which is why its OpenGL driver
    // requires the vertex_array_bgra extension. The attribute is declared
    // R8G8B8A8 because that format is universally supported for vertex
    // buffers, so the swizzle happens here instead.
    vec4 vertexColour = inColour.bgra;
#else
    // Geometry with no colour takes the fixed function current colour, which
    // the game never sets and which starts white.
    vec4 vertexColour = vec4(1.0);
#endif

    // The primary colour, which is the lit colour the texture environment
    // consumes. sceneTint.rgb is the light weight the driver collapsed the
    // ambient and diffuse terms into; the alpha comes from the vertex when
    // colour material maps it onto the diffuse material, and from the alpha
    // multiplier when it does not.
    fragColour = vec4(vertexColour.rgb * push.sceneTint.rgb,
        (push.fragmentState.z >= 7.5) ? vertexColour.a : push.sceneTint.a);

#if SCVK_TEXCOORD_SETS >= 1
    fragTexCoord0 = inTexCoord0;
#else
    // Geometry with no texture coordinate samples the 1x1 white default
    // texture, so any coordinate gives the same result.
    fragTexCoord0 = vec2(0.0);
#endif

    // Coordinates generated from the camera-space position, which is what the
    // cloud shadows are drawn with: the shadow texture is projected across the
    // terrain and scrolled by the texture matrix rather than following the
    // terrain's own coordinates. Reading the vertex set instead stamps the
    // texture once per terrain cell, which is why the shadows were square.
    //
    // The two rows arrive already multiplied through the modelview, so the
    // object position is all that is needed here.
    if (push.fragmentState.w > 2.5)
    {
        vec4 position = vec4(inPosition, 1.0);
        fragTexCoord0 = vec2(dot(push.aliasA, position), dot(push.aliasB, position));
    }

#if SCVK_TEXCOORD_SETS >= 2
    fragTexCoord1 = inTexCoord1;
#else
    // A single set feeds both stages. Only geometry that carries two sets
    // ever has a second stage bound, so this is never the one sampled.
    fragTexCoord1 = fragTexCoord0;
#endif
}
