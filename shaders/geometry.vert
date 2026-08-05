/*
 * scvk - a native Vulkan renderer for SimCity 4
 *
 * Copyright (C) 2026 aspctt
 * SPDX-License-Identifier: LGPL-2.1-or-later
 *
 * Vertex stage for the fixed function geometry path.
 *
 * SimCity 4 supplies only two vertex formats: V3F_C4UB (stride 16) and
 * V3F_C4UB_T2F (stride 24). Both start with a float3 position at offset 0 and
 * a packed 8-bit colour at offset 12, so one shader covers both and the
 * pipelines differ only in stride. Texture coordinates are not consumed yet.
 */

#version 450

layout(push_constant) uniform Push
{
    // Projection times modelview, already combined on the CPU and corrected
    // for Vulkan's clip space.
    mat4 mvp;
} push;

layout(location = 0) in vec3 inPosition;
layout(location = 1) in vec4 inColour;

layout(location = 0) out vec4 fragColour;

void main()
{
    gl_Position = push.mvp * vec4(inPosition, 1.0);

    // The game packs vertex colours as BGRA, which is why its OpenGL driver
    // requires the vertex_array_bgra extension. The attribute is declared
    // R8G8B8A8 because that format is universally supported for vertex
    // buffers, so the swizzle happens here instead.
    fragColour = inColour.bgra;
}
