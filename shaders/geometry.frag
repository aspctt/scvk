/*
 * scvk - a native Vulkan renderer for SimCity 4
 *
 * Copyright (C) 2026 aspctt
 * SPDX-License-Identifier: LGPL-2.1-or-later
 *
 * Fragment stage for the fixed function geometry path.
 *
 * Reproduces three things Vulkan has no equivalent for.
 *
 * The texture environment: modulate multiplies the texture by the vertex colour, replace
 * ignores the vertex colour entirely. The game uses both, and an untextured build renders
 * a fully textured scene as solid white because the vertex colours are white and the
 * texture carries the image.
 *
 * The texture stage combiners: a two stage network of sources, operands, combine modes
 * and output scales, which the terrain uses to blend a second texture over the first.
 * With only the first stage honoured the terrain renders in flat pale patches wherever
 * the second was meant to contribute.
 *
 * The alpha test: a fixed function comparison that discards fragments below a threshold.
 * It was removed from the programmable pipeline, so it becomes an explicit discard here,
 * driven by the comparison and reference the game sets.
 *
 * Draws with no texture bound sample a 1x1 white texture, so the multiply is a no-op and
 * no shader variant is needed for them.
 */

#version 450

//// Constants

// Thresholds on the draw's path in fragmentState.w, halfway between the values the
// backend sends: 1 and 3 take the single stage, 2 the two stages, 10 and up a pass
// colour, 20 and up an input channel.
const float PATH_TWO_STAGES_MINIMUM = 1.5;
const float PATH_TWO_STAGES_MAXIMUM = 2.5;
const float PASS_COLOURS_MINIMUM    = 9.5;
const float PASS_COLOURS_MAXIMUM    = 19.5;
const float CHANNELS_MINIMUM        = 19.5;

// The flag added to the texture environment mode when the primary colour's alpha comes
// from the vertex, which the vertex stage reads, and the value from which a mode has it.
const float ALPHA_FROM_VERTEX_FLAG      = 8.0;
const float ALPHA_FROM_VERTEX_THRESHOLD = 7.5;

//// References

layout(push_constant) uniform PushConstants
{
	mat4 modelViewProjection;

	// x: alpha comparison, in the game's own order, which matches OpenGL's:
	//    0 never, 1 less, 2 equal, 3 lequal, 4 greater, 5 notequal,
	//    6 gequal, 7 always. Negative means the test is disabled.
	// y: reference value.
	// z: texture environment mode in the game's own order, 0 replace,
	//    1 modulate, 2 decal, plus 8 when the primary colour's alpha comes
	//    from the vertex rather than from the alpha multiplier. The vertex
	//    stage reads that flag; this one reads the mode.
	// w: which path this draw takes.
	//    1 one texture stage, coordinates from the vertex
	//    2 two texture stages, coordinates from the vertex
	//    3 one texture stage, coordinates generated from the eye-space position
	//    10 and up: a diagnostic, see passColour and the channel view below
	vec4 fragmentState;

	// Two slots with two meanings. See the vertex stage for why they share.
	//
	// Path 3 reads them as texture generation rows, which only the vertex stage needs, so
	// this stage ignores them entirely.
	//
	// Path 2 reads aliasA as the combiner network, carried as raw bits, one packed word
	// per stage per channel: x stage 0 rgb, y stage 0 alpha, z stage 1 rgb, w stage 1
	// alpha. Within a word:
	//
	//   bits 0..2   combine mode
	//   bits 3..4   source 0      bits 5..7    operand 0
	//   bits 8..9   source 1      bits 10..12  operand 1
	//   bits 13..14 source 2      bits 15..17  operand 2
	//   bits 18..19 output scale, 0 for x1, 1 for x2, 2 for x4
	//
	// and aliasB as the environment colour a combiner may name as a source.
	vec4 aliasA;
	vec4 aliasB;

	// The light weight the ambient and diffuse terms collapsed into, and the diffuse
	// material's alpha in w. Both are consumed by the vertex stage, which builds the
	// primary colour from them, so this stage ignores them.
	//
	// This is the whole of SimCity 4's lighting: one fixed directional light and an
	// ambient colour that carries day and night.
	vec4 sceneTint;
} push;

// Image and sampler are separate objects here. Filter and wrap belong to the texture
// object in the game's OpenGL driver, and are applied when a draw binds it, so the
// sampler has to be free to change while a texture's descriptor stays fixed.
layout(set = 0, binding = 0) uniform texture2D textureImage0;
layout(set = 1, binding = 0) uniform texture2D textureImage1;
layout(set = 2, binding = 0) uniform sampler   textureSampler;

layout(location = 0) in vec4 fragmentColour;
layout(location = 1) in vec2 fragmentTextureCoordinate0;
layout(location = 2) in vec2 fragmentTextureCoordinate1;

layout(location = 0) out vec4 outColour;

//// Private Functions

// Sources, in the game's order: texture, previous, constant, primary colour.
vec4 combinerSource(uint source, vec4 texel, vec4 previous)
{
	if (source == 0u) { return texel; }
	if (source == 1u) { return previous; }
	if (source == 2u) { return push.aliasB; }
	return fragmentColour;
}

// Operands, reduced to the four the fixed function pipeline allows: the colour, its
// complement, the alpha, and its complement.
vec3 operandRgb(uint operand, vec4 value)
{
	if (operand == 0u) { return value.rgb; }
	if (operand == 1u) { return vec3(1.0) - value.rgb; }
	if (operand == 2u) { return vec3(value.a); }
	return vec3(1.0 - value.a);
}

// Only the alpha operands are meaningful on the alpha channel, so a colour operand is
// read as its alpha counterpart.
float operandAlpha(uint operand, vec4 value)
{
	if (operand == 1u || operand == 3u) { return 1.0 - value.a; }
	return value.a;
}

vec3 combineRgb(uint mode, vec3 argument0, vec3 argument1, vec3 argument2)
{
	if (mode == 0u) { return argument0; }
	if (mode == 1u) { return argument0 * argument1; }
	if (mode == 2u) { return argument0 + argument1; }
	if (mode == 3u) { return argument0 + argument1 - vec3(0.5); }
	if (mode == 4u) { return argument0 * argument2 + argument1 * (vec3(1.0) - argument2); }

	// Dot3, which returns one value replicated across the three channels.
	return vec3(4.0 * dot(argument0 - vec3(0.5), argument1 - vec3(0.5)));
}

float combineAlpha(uint mode, float argument0, float argument1, float argument2)
{
	if (mode == 0u) { return argument0; }
	if (mode == 1u) { return argument0 * argument1; }
	if (mode == 2u) { return argument0 + argument1; }
	if (mode == 3u) { return argument0 + argument1 - 0.5; }
	if (mode == 4u) { return argument0 * argument2 + argument1 * (1.0 - argument2); }

	// Dot3 writes the same value to alpha as to the colour channels.
	return argument0;
}

// The output scale a packed word asks for.
float scaleFactor(uint packed)
{
	uint scale = (packed >> 18) & 3u;
	return (scale == 0u) ? 1.0 : ((scale == 1u) ? 2.0 : 4.0);
}

// Runs one stage of the combiner network on its texel and the previous stage's result.
vec4 runStage(uint packedRgb, uint packedAlpha, vec4 texel, vec4 previous)
{
	// Combine the colour
	vec4 source0 = combinerSource((packedRgb >> 3)  & 3u, texel, previous);
	vec4 source1 = combinerSource((packedRgb >> 8)  & 3u, texel, previous);
	vec4 source2 = combinerSource((packedRgb >> 13) & 3u, texel, previous);

	vec3 rgb = combineRgb(packedRgb & 7u, operandRgb((packedRgb >> 5) & 7u, source0), operandRgb((packedRgb >> 10) & 7u, source1), operandRgb((packedRgb >> 15) & 7u, source2));

	// Combine the alpha
	vec4 alphaSource0 = combinerSource((packedAlpha >> 3)  & 3u, texel, previous);
	vec4 alphaSource1 = combinerSource((packedAlpha >> 8)  & 3u, texel, previous);
	vec4 alphaSource2 = combinerSource((packedAlpha >> 13) & 3u, texel, previous);

	float alpha = combineAlpha(packedAlpha & 7u, operandAlpha((packedAlpha >> 5) & 7u, alphaSource0), operandAlpha((packedAlpha >> 10) & 7u, alphaSource1), operandAlpha((packedAlpha >> 15) & 7u, alphaSource2));

	return vec4(rgb * scaleFactor(packedRgb), alpha * scaleFactor(packedAlpha));
}

// Flat identifying colours, one per blend configuration, used only when the driver asks
// for them. A pass that cannot be found by reasoning about state can be found by looking
// at which colour lands on the pixels in question.
vec3 passColour(int pass)
{
	if (pass == 0) { return vec3(1.0, 0.0, 0.0); }  // opaque, one/zero
	if (pass == 1) { return vec3(0.0, 1.0, 0.0); }  // opaque, one/one
	if (pass == 2) { return vec3(0.0, 0.4, 1.0); }  // additive, srcalpha/one
	if (pass == 3) { return vec3(1.0, 1.0, 0.0); }  // alpha blended
	if (pass == 4) { return vec3(1.0, 0.0, 1.0); }  // other, blend off
	return vec3(0.0, 1.0, 1.0);                     // other, blend on
}

//// Entry Point

void main()
{
	// Show the pass colour
	//
	// Pass identification overrides everything, including the alpha test, so that a pass
	// cannot hide by discarding.
	if (push.fragmentState.w > PASS_COLOURS_MINIMUM && push.fragmentState.w < PASS_COLOURS_MAXIMUM)
	{
		outColour = vec4(passColour(int(push.fragmentState.w) - 10), 1.0);
		return;
	}

	vec4 texel0 = texture(sampler2D(textureImage0, textureSampler), fragmentTextureCoordinate0);

	// Show one input channel
	//
	// So a capture says which input carries a wrong value. Opaque, so that an alpha
	// blended pass overwrites rather than mixing, and ahead of the alpha test so nothing
	// can hide.
	if (push.fragmentState.w > CHANNELS_MINIMUM)
	{
		int channel = int(push.fragmentState.w) - 20;

		if      (channel == 0) { outColour = vec4(texel0.rgb, 1.0); }
		else if (channel == 1) { outColour = vec4(vec3(texel0.a), 1.0); }
		else if (channel == 2) { outColour = vec4(fragmentColour.rgb, 1.0); }
		else                   { outColour = vec4(vec3(fragmentColour.a), 1.0); }

		return;
	}

	vec4 result;

	if (push.fragmentState.w < PATH_TWO_STAGES_MINIMUM || push.fragmentState.w > PATH_TWO_STAGES_MAXIMUM)
	{
		// Apply the texture environment on one stage
		//
		// This is the path everything but the terrain is on. The game's own order is not
		// the obvious one: replace comes first, then modulate, then decal.
		float mode = push.fragmentState.z;
		if (mode >= ALPHA_FROM_VERTEX_THRESHOLD) { mode -= ALPHA_FROM_VERTEX_FLAG; }

		if (mode < 0.5)
		{
			result = texel0;                                    // replace
		}
		else if (mode < 1.5)
		{
			result = texel0 * fragmentColour;                   // modulate
		}
		else
		{
			// Decal: the texture's own alpha weighs it against what came before, and the
			// alpha passes through untouched.
			result = vec4(mix(fragmentColour.rgb, texel0.rgb, texel0.a), fragmentColour.a);
		}
	}
	else
	{
		// Run the combiner network on two stages
		//
		// The first stage has no predecessor, so the primary colour stands in as
		// "previous", which is what the fixed function pipeline defines.
		vec4 texel1 = texture(sampler2D(textureImage1, textureSampler), fragmentTextureCoordinate1);

		uvec4 combiner = floatBitsToUint(push.aliasA);
		result = runStage(combiner.x, combiner.y, texel0, fragmentColour);
		result = runStage(combiner.z, combiner.w, texel1, result);
	}

	result = clamp(result, 0.0, 1.0);

	// Apply the alpha test
	int   comparison = int(push.fragmentState.x);
	float reference  = push.fragmentState.y;

	if (comparison >= 0)
	{
		bool hasPassed = true;

		if      (comparison == 0) { hasPassed = false; }
		else if (comparison == 1) { hasPassed = result.a <  reference; }
		else if (comparison == 2) { hasPassed = result.a == reference; }
		else if (comparison == 3) { hasPassed = result.a <= reference; }
		else if (comparison == 4) { hasPassed = result.a >  reference; }
		else if (comparison == 5) { hasPassed = result.a != reference; }
		else if (comparison == 6) { hasPassed = result.a >= reference; }

		if (!hasPassed) { discard; }
	}

	outColour = result;
}
