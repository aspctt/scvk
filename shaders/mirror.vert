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
 * The pieces standing above the water, drawn mirrored in it for the water reflections:
 * the network pieces and True3D props the live shadows keep, off screen ones included, so
 * a bridge whose deck lies off the top of the screen still shows in the water under it.
 *
 * The matrix takes a piece's model position through its place in the world, the mirror in
 * the water's level and the view to the clip space of the whole back buffer, OpenGL's, y
 * up. Vulkan's is y down, so y is flipped here.
 *
 * The depth is the point's height above the water over the tallest piece's, rather than
 * its distance from the camera. Along one pixel's mirrored ray the height grows with the
 * distance, so the lowest point is the first the ray reaches, and the depth test keeps it.
 * Depth 0 is the water's level and 1 the tallest height, so whatever lies under the water
 * is clipped away.
 */

#version 450

layout(push_constant) uniform MirrorConstants
{
	mat4 modelToClip;

	// The height above the water over the tallest height, as a row of the model position
	vec4 heightRow;

	// The two rows of the texture matrix a 2D sample reads
	vec4 rowS;
	vec4 rowT;

	// Alpha function, reference, alpha test enabled, the shade of an underside
	vec4 material;
} push;

layout(location = 0) in vec3 inPosition;
layout(location = 1) in vec2 inTextureCoordinate;
layout(location = 2) in vec4 inColour;

layout(location = 0) out vec2 textureCoordinate;
layout(location = 1) out vec3 colour;
layout(location = 2) out vec3 modelPosition;

void main()
{
	vec4 position = vec4(inPosition, 1.0);

	gl_Position   = push.modelToClip * position;
	gl_Position.y = -gl_Position.y;
	gl_Position.z = dot(push.heightRow, position) * gl_Position.w;

	// One pixel for the pieces the game drew as points. Ignored for triangles and lines.
	gl_PointSize = 1.0;

	vec4 source = vec4(inTextureCoordinate, 0.0, 1.0);
	textureCoordinate = vec2(dot(push.rowS, source), dot(push.rowT, source));

	colour        = inColour.rgb;
	modelPosition = inPosition;
}
