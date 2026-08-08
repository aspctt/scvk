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
 * V3F_T2F has the reverse, V3F_C4UB_T2F has both, and V3F has neither. A
 * shader may not declare an input the pipeline does not supply, so this is
 * compiled once per combination and the backend picks the variant matching
 * the format it was given.
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
    vec4 fragmentState;
} push;

layout(location = 0) in vec3 inPosition;
#if SCVK_HAS_COLOUR
layout(location = 1) in vec4 inColour;
#endif
#if SCVK_HAS_TEXCOORD
layout(location = 2) in vec2 inTexCoord;
#endif

layout(location = 0) out vec4 fragColour;
layout(location = 1) out vec2 fragTexCoord;

void main()
{
    gl_Position = push.mvp * vec4(inPosition, 1.0);

#if SCVK_HAS_COLOUR
    // The game packs vertex colours as BGRA, which is why its OpenGL driver
    // requires the vertex_array_bgra extension. The attribute is declared
    // R8G8B8A8 because that format is universally supported for vertex
    // buffers, so the swizzle happens here instead.
    fragColour = inColour.bgra;
#else
    fragColour = vec4(1.0);
#endif

#if SCVK_HAS_TEXCOORD
    fragTexCoord = inTexCoord;
#else
    // Geometry with no texture coordinate samples the 1x1 white default
    // texture, so any coordinate gives the same result.
    fragTexCoord = vec2(0.0);
#endif
}
