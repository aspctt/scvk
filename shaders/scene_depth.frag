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
 * The scene depth re-encoded for ReShade's DEPTH semantic, as SCD3D11 encodes it.
 *
 * SimCity 4's camera is orthographic, so its depth is already linear. ReShade.fxh
 * linearizes as if it were perspective, d / (far - d * (far - 1)); this is the inverse, so
 * the linearization returns the depth unchanged.
 */

#version 450

layout(push_constant) uniform SceneDepthConstants
{
	float farPlane;
} push;

layout(set = 0, binding = 0) uniform sampler2D sceneDepth;

layout(location = 0) out float outDepth;

void main()
{
	float depth = texelFetch(sceneDepth, ivec2(gl_FragCoord.xy), 0).r;
	outDepth = depth * push.farPlane / (1.0 + depth * (push.farPlane - 1.0));
}
