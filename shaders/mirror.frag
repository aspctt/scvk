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
 * The colour of a mirrored piece: its texture in the colour the game lit it with, cut by
 * the alpha test it was drawn with, and its height above the water beside it, which the
 * reflections weigh against what the screen itself shows.
 *
 * The mirror shows a piece from below, where the game draws no underside, so what shows
 * is the back of a deck's top face. A face is shaded towards the shade of an underside by
 * how level it lies: a deck darkens and a pylon keeps its colour, as the reflections shade
 * an underside they find on screen.
 */

#version 450

layout(push_constant) uniform MirrorConstants
{
	mat4 modelToClip;
	vec4 heightRow;
	vec4 rowS;
	vec4 rowT;
	vec4 material;
} push;

layout(set = 0, binding = 0) uniform texture2D sourceImage;
layout(set = 1, binding = 0) uniform sampler   sourceSampler;

layout(location = 0) in vec2 textureCoordinate;
layout(location = 1) in vec3 colour;
layout(location = 2) in vec3 modelPosition;

layout(location = 0) out vec4 outColour;

bool PassesAlphaTest(float alpha)
{
	uint  function  = uint(push.material.x);
	float reference = push.material.y;

	if (push.material.z == 0.0)
	{
		return alpha > (1.0 / 255.0);
	}

	switch (function)
	{
	case 0u: return false;
	case 1u: return alpha < reference;
	case 2u: return abs(alpha - reference) <= (1.0 / 255.0);
	case 3u: return alpha <= reference;
	case 4u: return alpha > reference;
	case 5u: return abs(alpha - reference) > (1.0 / 255.0);
	case 6u: return alpha >= reference;
	default: return true;
	}
}

void main()
{
	// How level the face lies: its normal against the direction the height grows in, both
	// in model space. A piece is placed by a rotation and a move, which keep the angle
	// between them as it is in the world.
	//
	// Worked out before the alpha test, since a discarded neighbour leaves the derivatives
	// undefined.
	vec3  normal       = cross(dFdx(modelPosition), dFdy(modelPosition));
	float normalLength = length(normal);
	float level        = (normalLength > 0.0) ? abs(dot(normal / normalLength, normalize(push.heightRow.xyz))) : 0.0;

	// An untextured piece samples the white texture, which leaves its colour and passes
	// its alpha
	vec4 texel = texture(sampler2D(sourceImage, sourceSampler), textureCoordinate);

	if (!PassesAlphaTest(texel.a))
	{
		discard;
	}

	outColour = vec4(texel.rgb * colour * mix(1.0, push.material.w, level), gl_FragCoord.z);
}
