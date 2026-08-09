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
 * The texture environment: modulate multiplies the texture by the vertex
 * colour, replace ignores the vertex colour entirely. The game uses both, and
 * an untextured build renders a fully textured scene as solid white because
 * the vertex colours are white and the texture carries the image.
 *
 * The texture stage combiners: a two stage network of sources, operands,
 * combine modes and output scales, which the terrain uses to blend a second
 * texture over the first. With only the first stage honoured the terrain
 * renders in flat pale patches wherever the second was meant to contribute.
 *
 * The alpha test: a fixed function comparison that discards fragments below a
 * threshold. It was removed from the programmable pipeline, so it becomes an
 * explicit discard here, driven by the comparison and reference the game sets.
 *
 * Draws with no texture bound sample a 1x1 white texture, so the multiply is a
 * no-op and no shader variant is needed for them.
 */

#version 450

layout(push_constant) uniform Push
{
    mat4 mvp;

    // x: alpha comparison, in the game's own order, which matches OpenGL's:
    //    0 never, 1 less, 2 equal, 3 lequal, 4 greater, 5 notequal,
    //    6 gequal, 7 always. Negative means the test is disabled.
    // y: reference value.
    // z: 0 for modulate, 1 for replace. Used only by the single stage path.
    // w: which mode this draw is in.
    //    1 one texture stage, coordinates from the vertex
    //    2 two texture stages, coordinates from the vertex
    //    3 one texture stage, coordinates generated from the eye-space position
    vec4 fragmentState;

    // Two slots with two meanings. See the vertex stage for why they share.
    //
    // Mode 3 reads them as texture generation rows, which only the vertex
    // stage needs, so this stage ignores them entirely.
    //
    // Mode 2 reads aliasA as the combiner network, carried as raw bits, one
    // packed word per stage per channel: x stage 0 rgb, y stage 0 alpha,
    // z stage 1 rgb, w stage 1 alpha. Within a word:
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

    // The global ambient light colour, and the diffuse material alpha in w.
    //
    // This is the whole of SimCity 4's lighting. It never configures a light
    // source, so there is no diffuse term and the fixed function equation
    // collapses to the ambient light times the ambient material, which colour
    // material takes from the vertex colour. It is what carries day and night.
    vec4 sceneTint;
} push;

// Image and sampler are separate objects here. The game drives filter and
// wrap as global state applied to whatever is bound, so the sampler has to be
// free to change per draw while a texture's descriptor stays fixed.
layout(set = 0, binding = 0) uniform texture2D texImage0;
layout(set = 1, binding = 0) uniform texture2D texImage1;
layout(set = 2, binding = 0) uniform sampler   texSampler;

layout(location = 0) in vec4 fragColour;
layout(location = 1) in vec2 fragTexCoord0;
layout(location = 2) in vec2 fragTexCoord1;

layout(location = 0) out vec4 outColour;

// Sources, in the game's order: texture, previous, constant, primary colour.
vec4 combinerSource(uint source, vec4 texel, vec4 previous)
{
    if (source == 0u) { return texel; }
    if (source == 1u) { return previous; }
    if (source == 2u) { return push.aliasB; }
    return fragColour;
}

// Operands, reduced to the four the fixed function pipeline allows: the
// colour, its complement, the alpha, and its complement.
vec3 operandRGB(uint operand, vec4 value)
{
    if (operand == 0u) { return value.rgb; }
    if (operand == 1u) { return vec3(1.0) - value.rgb; }
    if (operand == 2u) { return vec3(value.a); }
    return vec3(1.0 - value.a);
}

float operandAlpha(uint operand, vec4 value)
{
    // Only the alpha operands are meaningful on the alpha channel, so a
    // colour operand is read as its alpha counterpart.
    if (operand == 1u || operand == 3u) { return 1.0 - value.a; }
    return value.a;
}

vec3 combineRGB(uint mode, vec3 a0, vec3 a1, vec3 a2)
{
    if (mode == 0u) { return a0; }
    if (mode == 1u) { return a0 * a1; }
    if (mode == 2u) { return a0 + a1; }
    if (mode == 3u) { return a0 + a1 - vec3(0.5); }
    if (mode == 4u) { return a0 * a2 + a1 * (vec3(1.0) - a2); }

    // Dot3, which returns one value replicated across the three channels.
    return vec3(4.0 * dot(a0 - vec3(0.5), a1 - vec3(0.5)));
}

float combineAlpha(uint mode, float a0, float a1, float a2)
{
    if (mode == 0u) { return a0; }
    if (mode == 1u) { return a0 * a1; }
    if (mode == 2u) { return a0 + a1; }
    if (mode == 3u) { return a0 + a1 - 0.5; }
    if (mode == 4u) { return a0 * a2 + a1 * (1.0 - a2); }

    // Dot3 writes the same value to alpha as to the colour channels.
    return a0;
}

float scaleFactor(uint packed)
{
    uint scale = (packed >> 18) & 3u;
    return (scale == 0u) ? 1.0 : ((scale == 1u) ? 2.0 : 4.0);
}

vec4 runStage(uint packedRGB, uint packedAlpha, vec4 texel, vec4 previous)
{
    vec4 s0 = combinerSource((packedRGB >> 3)  & 3u, texel, previous);
    vec4 s1 = combinerSource((packedRGB >> 8)  & 3u, texel, previous);
    vec4 s2 = combinerSource((packedRGB >> 13) & 3u, texel, previous);

    vec3 rgb = combineRGB(packedRGB & 7u,
        operandRGB((packedRGB >> 5)  & 7u, s0),
        operandRGB((packedRGB >> 10) & 7u, s1),
        operandRGB((packedRGB >> 15) & 7u, s2));

    vec4 a0 = combinerSource((packedAlpha >> 3)  & 3u, texel, previous);
    vec4 a1 = combinerSource((packedAlpha >> 8)  & 3u, texel, previous);
    vec4 a2 = combinerSource((packedAlpha >> 13) & 3u, texel, previous);

    float alpha = combineAlpha(packedAlpha & 7u,
        operandAlpha((packedAlpha >> 5)  & 7u, a0),
        operandAlpha((packedAlpha >> 10) & 7u, a1),
        operandAlpha((packedAlpha >> 15) & 7u, a2));

    return vec4(rgb * scaleFactor(packedRGB), alpha * scaleFactor(packedAlpha));
}

// Flat identifying colours, one per blend configuration, used only when the
// driver asks for them. A pass that cannot be found by reasoning about state
// can be found by looking at which colour lands on the pixels in question.
vec3 passColour(int pass)
{
    if (pass == 0) { return vec3(1.0, 0.0, 0.0); }  // opaque, one/zero
    if (pass == 1) { return vec3(0.0, 1.0, 0.0); }  // opaque, one/one
    if (pass == 2) { return vec3(0.0, 0.4, 1.0); }  // additive, srcalpha/one
    if (pass == 3) { return vec3(1.0, 1.0, 0.0); }  // alpha blended
    if (pass == 4) { return vec3(1.0, 0.0, 1.0); }  // other, blend off
    return vec3(0.0, 1.0, 1.0);                     // other, blend on
}

void main()
{
    // Pass identification overrides everything, including the alpha test, so
    // that a pass cannot hide by discarding.
    if (push.fragmentState.w > 9.5)
    {
        outColour = vec4(passColour(int(push.fragmentState.w) - 10), 1.0);
        return;
    }

    vec4 texel0 = texture(sampler2D(texImage0, texSampler), fragTexCoord0);
    vec4 result;

    if (push.fragmentState.w < 1.5 || push.fragmentState.w > 2.5)
    {
        // One stage, driven by the texture environment rather than the
        // combiner network. This is the path everything but the terrain is on,
        // and it is left exactly as it was: replace takes the texture alone,
        // modulate scales it by the vertex colour.
        result = mix(texel0 * fragColour, texel0, push.fragmentState.z);
    }
    else
    {
        vec4 texel1 = texture(sampler2D(texImage1, texSampler), fragTexCoord1);

        // The first stage has no predecessor, so the primary colour stands in
        // as "previous", which is what the fixed function pipeline defines.
        uvec4 combiner = floatBitsToUint(push.aliasA);
        result = runStage(combiner.x, combiner.y, texel0, fragColour);
        result = runStage(combiner.z, combiner.w, texel1, result);
    }

    result *= push.sceneTint;
    result = clamp(result, 0.0, 1.0);

    int  comparison = int(push.fragmentState.x);
    float reference = push.fragmentState.y;

    if (comparison >= 0)
    {
        bool passed = true;

        if      (comparison == 0) passed = false;
        else if (comparison == 1) passed = result.a <  reference;
        else if (comparison == 2) passed = result.a == reference;
        else if (comparison == 3) passed = result.a <= reference;
        else if (comparison == 4) passed = result.a >  reference;
        else if (comparison == 5) passed = result.a != reference;
        else if (comparison == 6) passed = result.a >= reference;

        if (!passed) discard;
    }

    outColour = result;
}
