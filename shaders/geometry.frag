/*
 * scvk - a native Vulkan renderer for SimCity 4
 *
 * Copyright (C) 2026 aspctt
 * SPDX-License-Identifier: LGPL-2.1-or-later
 *
 * Fragment stage for the fixed function geometry path.
 *
 * Reproduces two things Vulkan has no equivalent for.
 *
 * The texture environment: modulate multiplies the texture by the vertex
 * colour, replace ignores the vertex colour entirely. The game uses both, and
 * an untextured build renders a fully textured scene as solid white because
 * the vertex colours are white and the texture carries the image.
 *
 * The alpha test: a fixed function comparison that discards fragments below a
 * threshold. It was removed from the programmable pipeline, so it becomes an
 * explicit discard here, driven by the comparison and reference the game sets.
 *
 * Draws with no texture bound sample a 1x1 white texture, so the multiply is a
 * no-op and no shader variant is needed for them.
 */

#version 450

layout(push_constant) uniform Push
{
    mat4 mvp;

    // x: alpha comparison, in the game's own order, which matches OpenGL's:
    //    0 never, 1 less, 2 equal, 3 lequal, 4 greater, 5 notequal,
    //    6 gequal, 7 always. Negative means the test is disabled.
    // y: reference value.
    // z: 0 for modulate, 1 for replace.
    vec4 fragmentState;
} push;

layout(set = 0, binding = 0) uniform sampler2D texSampler;

layout(location = 0) in vec4 fragColour;
layout(location = 1) in vec2 fragTexCoord;

layout(location = 0) out vec4 outColour;

void main()
{
    vec4 texel = texture(texSampler, fragTexCoord);

    // Replace takes the texture alone; modulate scales it by the vertex
    // colour. Selected without branching, since both operands are cheap.
    vec4 result = mix(texel * fragColour, texel, push.fragmentState.z);

    int  comparison = int(push.fragmentState.x);
    float reference = push.fragmentState.y;

    if (comparison >= 0)
    {
        bool passed = true;

        if      (comparison == 0) passed = false;
        else if (comparison == 1) passed = result.a <  reference;
        else if (comparison == 2) passed = result.a == reference;
        else if (comparison == 3) passed = result.a <= reference;
        else if (comparison == 4) passed = result.a >  reference;
        else if (comparison == 5) passed = result.a != reference;
        else if (comparison == 6) passed = result.a >= reference;

        if (!passed) discard;
    }

    outColour = result;
}
