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
 * The blits: one quad over a rectangle of the back buffer, drawn as a four vertex strip
 * with no vertex input, the way SCD3D11 draws its blits.
 */

#version 450

layout(push_constant) uniform BlitConstants
{
	// The destination in Vulkan's normalised coordinates: left, top, right, bottom.
	vec4 rectangle;

	// The far corner of the source in the blit image's coordinates, then nothing.
	vec4 sourceExtent;

	// Multiplies the colour; its alpha is the constant alpha of the Alpha blits.
	vec4 modulate;

	// The colour key, in effect when its alpha is above one half.
	vec4 colourKey;

	// Whether the source alpha is used, then nothing.
	vec4 options;
} push;

layout(location = 0) out vec2 textureCoordinate;

void main()
{
	// 0 top left, 1 top right, 2 bottom left, 3 bottom right
	float right  = float(gl_VertexIndex & 1);
	float bottom = float((gl_VertexIndex >> 1) & 1);

	gl_Position       = vec4(mix(push.rectangle.x, push.rectangle.z, right), mix(push.rectangle.y, push.rectangle.w, bottom), 0.0, 1.0);
	textureCoordinate = vec2(right, bottom) * push.sourceExtent.xy;
}
