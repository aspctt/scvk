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
 * Which texels of a caster cast: SCD3D11's CasterPS and RegistryPS in one.
 *
 * A captured caster passes the alpha test it was drawn with, or any alpha at all without
 * one. A registry caster casts where SimCity 4's own projected shadow would: the projector
 * maps the point onto the prerendered texture, the rectangle clips it, and the alpha is
 * tested against the shadow's reference. A True3D prop has real coordinates instead and
 * is tested through them.
 *
 * Every texel reads the texture's base level. A shadow map texel covers several texture
 * texels, so a mip would average thin leaves and poles below the alpha cut, and the game's
 * own DrawShadows filters without mipmaps anyway.
 *
 * Beside its depth, each map texel keeps the occupant number of the registry caster that
 * drew it, from 1, or 0 for a captured caster. The composite reads it so that a prerendered
 * model never shadows itself, since its picture already holds its own shadows.
 */

#version 450

layout(constant_id = 0) const bool IS_REGISTRY = false;

layout(push_constant) uniform ShadowConstants
{
	mat4 lightMatrix;
	vec4 rowS;
	vec4 rowT;
	vec4 material;
	vec4 uvBounds;
} push;

layout(set = 0, binding = 0) uniform texture2D sourceImage;
layout(set = 1, binding = 0) uniform sampler   sourceSampler;

layout(location = 0) in vec2 textureCoordinate;
layout(location = 1) in vec3 worldPosition;

layout(location = 0) out uint outOccupant;

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
	if (IS_REGISTRY)
	{
		// The occupant's number for this pass, which the driver passes as a float
		uint occupant = uint(push.material.x);

		if (push.material.z != 0.0)
		{
			if (textureLod(sampler2D(sourceImage, sourceSampler), textureCoordinate, 0.0).a < push.material.y)
			{
				discard;
			}

			outOccupant = occupant;
			return;
		}

		vec4 world = vec4(worldPosition, 1.0);
		vec2 projected = vec2(dot(push.rowS, world), dot(push.rowT, world));
		vec2 inside = step(push.uvBounds.xy, projected) * step(projected, push.uvBounds.zw);

		if (inside.x * inside.y < 0.5 || textureLod(sampler2D(sourceImage, sourceSampler), projected, 0.0).a < push.material.y)
		{
			discard;
		}

		outOccupant = occupant;
		return;
	}

	float alpha = (push.material.w != 0.0) ? textureLod(sampler2D(sourceImage, sourceSampler), textureCoordinate, 0.0).a : 1.0;

	if (!PassesAlphaTest(alpha))
	{
		discard;
	}

	// A captured caster is no prerendered model, so it has no occupant
	outOccupant = 0u;
}
