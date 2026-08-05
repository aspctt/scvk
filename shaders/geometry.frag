/*
 * scvk - a native Vulkan renderer for SimCity 4
 *
 * Copyright (C) 2026 aspctt
 * SPDX-License-Identifier: LGPL-2.1-or-later
 *
 * Fragment stage for the fixed function geometry path.
 *
 * Vertex colour only for now. This is where the texture stages, the two-stage
 * combiner network, alpha test and fog will eventually be reproduced, since
 * Vulkan has no fixed function equivalent for any of them.
 */

#version 450

layout(location = 0) in vec4 fragColour;

layout(location = 0) out vec4 outColour;

void main()
{
    outColour = fragColour;
}
