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
 * The blits' colour: SCD3D11's blit pixel shader. The source alpha is used or replaced
 * by one, a texel matching the colour key turns transparent, and the result is
 * multiplied by the modulate colour.
 */

#version 450

layout(push_constant) uniform BlitConstants
{
	vec4 rectangle;
	vec4 sourceExtent;
	vec4 modulate;
	vec4 colourKey;
	vec4 options;
} push;

layout(set = 0, binding = 0) uniform texture2D sourceImage;
layout(set = 1, binding = 0) uniform sampler   sourceSampler;

layout(location = 0) in  vec2 textureCoordinate;
layout(location = 0) out vec4 outColour;

void main()
{
	vec4 colour = texture(sampler2D(sourceImage, sourceSampler), textureCoordinate);
	colour.a = mix(1.0, colour.a, push.options.x);

	if (push.colourKey.a > 0.5 && all(lessThan(abs(colour.rgb - push.colourKey.rgb), vec3(0.5 / 255.0))))
	{
		colour.a = 0.0;
	}

	outColour = colour * push.modulate;
}
