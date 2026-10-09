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
 * The shadow casters, drawn into the shadow map from the sun. SCD3D11's CasterVS and
 * RegistryVS in one: a captured caster carries its own texture coordinates through the
 * first stage's texture matrix, a registry caster passes its world position on for the
 * fragment stage to project.
 *
 * The light matrix maps to Direct3D's clip space, y up. Vulkan's is y down, so y is
 * flipped here, which leaves the shadow map laid out exactly as SCD3D11's and lets the
 * composite read it with the same formula.
 */

#version 450

layout(constant_id = 0) const bool IS_REGISTRY = false;

layout(push_constant) uniform ShadowConstants
{
	mat4 lightMatrix;

	// The two rows of the texture matrix a 2D sample reads.
	vec4 rowS;
	vec4 rowT;

	// Captured casters: alpha function, reference, alpha test enabled, textured.
	// Registry casters: unused, alpha reference, has its own coordinates, unused.
	vec4 material;

	// Registry casters: the projected texture's rectangle, u and v minimum then maximum.
	vec4 uvBounds;
} push;

layout(location = 0) in vec3 inPosition;
layout(location = 1) in vec2 inTextureCoordinate;

layout(location = 0) out vec2 textureCoordinate;
layout(location = 1) out vec3 worldPosition;

void main()
{
	gl_Position   = push.lightMatrix * vec4(inPosition, 1.0);
	gl_Position.y = -gl_Position.y;

	// One pixel, as Direct3D draws a point, for the casters the game drew as points.
	// Ignored for triangles and lines.
	gl_PointSize = 1.0;

	worldPosition = inPosition;

	if (IS_REGISTRY)
	{
		textureCoordinate = inTextureCoordinate;
	}
	else
	{
		vec4 source = vec4(inTextureCoordinate, 0.0, 1.0);
		textureCoordinate = vec2(dot(push.rowS, source), dot(push.rowT, source));
	}
}
