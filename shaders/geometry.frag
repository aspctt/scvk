/*
 * scvk - a native Vulkan renderer for SimCity 4
 *
 * Copyright (C) 2026 aspctt
 * SPDX-License-Identifier: LGPL-2.1-or-later
 *
 * Fragment stage for the fixed function geometry path.
 *
 * Reproduces the one texture environment mode the game actually relies on:
 * modulate, where the final colour is the texture times the vertex colour.
 * That is why an untextured build renders a fully textured scene as solid
 * white, since the game sets vertex colour to white and lets the texture
 * carry the image.
 *
 * Draws with no texture bound sample a 1x1 white texture, so the multiply is
 * a no-op and no shader variant is needed for them.
 *
 * The two-stage combiner network, alpha test and fog still have no equivalent
 * here.
 */

#version 450

layout(set = 0, binding = 0) uniform sampler2D texSampler;

layout(location = 0) in vec4 fragColour;
layout(location = 1) in vec2 fragTexCoord;

layout(location = 0) out vec4 outColour;

void main()
{
    outColour = texture(texSampler, fragTexCoord) * fragColour;
}
