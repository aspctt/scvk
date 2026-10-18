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
 * The lighting extension, and Direct3D 7's fixed function lighting behind it.
 *
 * SimCity 4 lights the city through the colour multiplier, the alpha multiplier and the
 * vertex colour mapping, which the geometry shader reproduces on its own (see
 * cVKDriver::PushLighting). That is all of its lighting while no light is switched on,
 * no material glows and the ambient material is white, which is how the game leaves
 * them.
 *
 * The extension can change any of that, as SCD3D11 lets it: up to eight lights with
 * ambient and diffuse colours, directional or positional, and a material with ambient,
 * diffuse and emissive colours. A draw that needs any of it is lit here, on the CPU, with
 * Direct3D 7's equation as SimGLDX7 set it up:
 *
 *   colour = emission + global ambient * ambient material
 *          + for each light on: light ambient * ambient material
 *                               + light diffuse * diffuse material * max(0, N.L)
 *   alpha  = the diffuse material's alpha
 *
 * clamped to 0 to 1 before texturing, with no specular term, and no diffuse term for a
 * vertex that carries no normal. The ambient and diffuse materials are the vertex colour
 * where the game maps it onto them. Lights are given in eye space, as SCD3D11 takes them.
 *
 * Per pixel, as SCD3D11 lights. Direct3D 7 lights per vertex, and so did scvk at first, but
 * on a long triangle with vertices only at its ends a positional light near one vertex is
 * stretched across the whole triangle. SimCity 4 itself was not seen using the extension,
 * whose lights other plugins may set; the strips of light along True3D highways at night
 * come from elsewhere and look the same under SCD3D11. Here the
 * CPU works out per vertex what does not depend on direction, the eye-space normal and
 * the vector to each light, into a copy of the vertices (LitVertex), and the shader's lit
 * variant adds each light's diffuse term per pixel.
 *
 * Per vertex remains for a device that cannot take the lit variant's vertex attributes,
 * and for a draw without normals, whose lighting has no direction in it: there the lit
 * colour goes into a copy of the vertices that carries a colour, which the shader passes
 * through.
 */

//// Dependencies

#include "cVKDriver.h"
#include "Logger.h"
#include "VulkanBackend.h"

#include <VertexFormatUtils.h>

#include <algorithm>
#include <iterator>
#include <cmath>
#include <cstring>

namespace scvk
{
	//// Constants

	namespace
	{
		// The material and light colour slots, in the game's numbering.
		constexpr uint32_t COLOUR_AMBIENT   = 0;
		constexpr uint32_t COLOUR_DIFFUSE   = 1;
		constexpr uint32_t COLOUR_SPECULAR  = 2;
		constexpr uint32_t COLOUR_EMISSION  = 3;
		constexpr uint32_t COLOUR_SHININESS = 4;

		// The emissive colour is the slot after the specular one, the last a colour fills.
		static_assert(COLOUR_EMISSION == COLOUR_SPECULAR + 1 && COLOUR_SHININESS == COLOUR_EMISSION + 1, "the game's material slots");

		// The most lit vertex data one draw may make, beyond which it is drawn unlit
		// rather than taking that much memory in a 32-bit process.
		constexpr size_t MAXIMUM_LIT_BYTES = 64u * 1024u * 1024u;

		// Below this a vector is taken to have no direction.
		constexpr float LENGTH_EPSILON = 1.0e-12f;
	}

	//// Private Functions

	namespace
	{
		float Clamp01(float value)
		{
			return std::min(std::max(value, 0.0f), 1.0f);
		}

		/** Scales a vector to unit length, or leaves it zero when it has none. */
		void Normalise(float vector[3])
		{
			float const lengthSquared = vector[0] * vector[0] + vector[1] * vector[1] + vector[2] * vector[2];
			if (lengthSquared <= LENGTH_EPSILON)
			{
				vector[0] = vector[1] = vector[2] = 0.0f;
				return;
			}

			float const scale = 1.0f / std::sqrt(lengthSquared);
			vector[0] *= scale;
			vector[1] *= scale;
			vector[2] *= scale;
		}

		/**
		 * The inverse transpose of a column-major matrix's upper 3x3, which carries normals
		 * into eye space, as rows. A singular matrix leaves them as they are.
		 */
		void MakeNormalMatrix(float const matrix[16], float out[3][3])
		{
			float const a00 = matrix[0], a01 = matrix[4], a02 = matrix[8];
			float const a10 = matrix[1], a11 = matrix[5], a12 = matrix[9];
			float const a20 = matrix[2], a21 = matrix[6], a22 = matrix[10];

			float const c00 = a11 * a22 - a12 * a21, c01 = a12 * a20 - a10 * a22, c02 = a10 * a21 - a11 * a20;
			float const c10 = a02 * a21 - a01 * a22, c11 = a00 * a22 - a02 * a20, c12 = a01 * a20 - a00 * a21;
			float const c20 = a01 * a12 - a02 * a11, c21 = a02 * a10 - a00 * a12, c22 = a00 * a11 - a01 * a10;

			float const determinant = a00 * c00 + a01 * c01 + a02 * c02;
			if (std::fabs(determinant) < LENGTH_EPSILON)
			{
				float const identity[3][3] = { { 1, 0, 0 }, { 0, 1, 0 }, { 0, 0, 1 } };
				std::memcpy(out, identity, sizeof(identity));
				return;
			}

			float const scale = 1.0f / determinant;
			out[0][0] = c00 * scale; out[0][1] = c01 * scale; out[0][2] = c02 * scale;
			out[1][0] = c10 * scale; out[1][1] = c11 * scale; out[1][2] = c12 * scale;
			out[2][0] = c20 * scale; out[2][1] = c21 * scale; out[2][2] = c22 * scale;
		}

		/** A colour from 0 to 1 as a vertex colour byte. */
		uint8_t ColourByte(float value)
		{
			return static_cast<uint8_t>(Clamp01(value) * 255.0f + 0.5f);
		}
	}

	//// Private API

	bool cVKDriver::IsFixedFunctionLightingNeeded(void) const
	{
		if (!isFixedLightingEnabled)
		{
			return false;
		}

		for (uint32_t light = 0; light < LIGHT_COUNT; light++)
		{
			if (isLightEnabled[light])
			{
				return true;
			}
		}

		bool const isEmissive        = materialEmission[0] != 0.0f || materialEmission[1] != 0.0f || materialEmission[2] != 0.0f;
		bool const isAmbientTinted   = !isVertexColourAmbient && (materialAmbient[0] != 1.0f || materialAmbient[1] != 1.0f || materialAmbient[2] != 1.0f);

		return isEmissive || isAmbientTinted;
	}

	uint32_t cVKDriver::LightVerticesPerPixel(uint32_t firstVertex, uint32_t vertexCount)
	{
		// Size the copy to the indices the draw reads, so they still apply to it
		size_t const bytes = (size_t{ firstVertex } + vertexCount) * sizeof(LitVertex);
		if (bytes > MAXIMUM_LIT_BYTES)
		{
			return 0;
		}

		if (litVertices.size() < bytes)
		{
			litVertices.resize(bytes);
		}

		// Find what the game's vertices carry; the caller made sure of the normal
		bool const     hasColour      = RZVertexFormatNumElements(vertexFormat, kGDElementType_Color) != 0;
		uint32_t const colourOffset   = hasColour ? RZVertexFormatElementOffset(vertexFormat, kGDElementType_Color, 0) : 0;
		uint32_t const normalOffset   = RZVertexFormatElementOffset(vertexFormat, kGDElementType_Normal, 0);
		uint32_t const coordinateSets = std::min(RZVertexFormatNumElements(vertexFormat, kGDElementType_TexCoord), 2u);

		uint32_t sourceCoordinates[2] = {};
		for (uint32_t set = 0; set < coordinateSets; set++)
		{
			sourceCoordinates[set] = RZVertexFormatElementOffset(vertexFormat, kGDElementType_TexCoord, set);
		}

		// Gather what is the same for every vertex: the directional lights as unit
		// vectors, the positional ones in eye space, and the normal matrix
		float directions[LIGHT_COUNT][3] = {};
		float positions[LIGHT_COUNT][3]  = {};

		for (uint32_t light = 0; light < LIGHT_COUNT; light++)
		{
			if (!isLightEnabled[light])
			{
				continue;
			}

			float const w = lightPosition[light][3];
			if (w == 0.0f)
			{
				std::memcpy(directions[light], lightPosition[light], sizeof(directions[light]));
				Normalise(directions[light]);
			}
			else
			{
				for (int axis = 0; axis < 3; axis++)
				{
					positions[light][axis] = lightPosition[light][axis] / w;
				}
			}
		}

		float normalMatrix[3][3] = {};
		MakeNormalMatrix(modelViewMatrix, normalMatrix);

		float const* const m = modelViewMatrix;
		static_assert(LIGHT_COUNT == LIT_LIGHT_COUNT, "the lit vertex carries every light");

		// Prepare each vertex the draw reads
		for (uint32_t index = firstVertex; index < firstVertex + vertexCount; index++)
		{
			uint8_t const* const source = static_cast<uint8_t const*>(vertexPointer) + size_t{ index } * vertexStride;
			LitVertex&           out    = reinterpret_cast<LitVertex*>(litVertices.data())[index];

			std::memcpy(out.position, source, sizeof(out.position));

			// The coordinates, with a single set standing in for both, as the shader's
			// one-set variants read it for either stage
			for (uint32_t set = 0; set < 2; set++)
			{
				if (coordinateSets == 0)
				{
					out.coordinates[set][0] = out.coordinates[set][1] = 0.0f;
				}
				else
				{
					std::memcpy(out.coordinates[set], source + sourceCoordinates[std::min(set, coordinateSets - 1)], sizeof(out.coordinates[set]));
				}
			}

			// The vertex colour, stored BGRA, or white where there is none
			float vertexColour[4] = { 1.0f, 1.0f, 1.0f, 1.0f };
			if (hasColour)
			{
				uint8_t const* const bgra = source + colourOffset;
				vertexColour[0] = static_cast<float>(bgra[2]) / 255.0f;
				vertexColour[1] = static_cast<float>(bgra[1]) / 255.0f;
				vertexColour[2] = static_cast<float>(bgra[0]) / 255.0f;
				vertexColour[3] = static_cast<float>(bgra[3]) / 255.0f;
			}

			float const* const ambientMaterial = isVertexColourAmbient ? vertexColour : materialAmbient;
			float const* const diffuseMaterial = isAlphaFromVertexColour ? vertexColour : materialDiffuse;

			// What does not depend on direction: emission, the global ambient light, and
			// each light's ambient term
			float base[3];
			for (int channel = 0; channel < 3; channel++)
			{
				base[channel] = materialEmission[channel] + colourMultiplier[channel] * ambientMaterial[channel];
			}

			// The eye-space position and normal
			float objectNormal[3];
			std::memcpy(objectNormal, source + normalOffset, sizeof(objectNormal));

			float eye[3];
			for (int row = 0; row < 3; row++)
			{
				eye[row]        = m[row] * out.position[0] + m[4 + row] * out.position[1] + m[8 + row] * out.position[2] + m[12 + row];
				out.normal[row] = normalMatrix[row][0] * objectNormal[0] + normalMatrix[row][1] * objectNormal[1] + normalMatrix[row][2] * objectNormal[2];
			}

			Normalise(out.normal);

			// Each light: its ambient term into the base, and its direction and diffuse
			// colour for the shader, black when it is off
			for (uint32_t light = 0; light < LIGHT_COUNT; light++)
			{
				if (!isLightEnabled[light])
				{
					std::memset(out.toLight[light], 0, sizeof(out.toLight[light]));
					std::memset(out.lightColour[light], 0, sizeof(out.lightColour[light]));
					continue;
				}

				for (int axis = 0; axis < 3; axis++)
				{
					out.toLight[light][axis] = (lightPosition[light][3] == 0.0f) ? directions[light][axis] : positions[light][axis] - eye[axis];
				}

				for (int channel = 0; channel < 3; channel++)
				{
					base[channel] += lightAmbient[light][channel] * ambientMaterial[channel];
					out.lightColour[light][channel] = ColourByte(lightDiffuse[light][channel] * diffuseMaterial[channel]);
				}

				out.lightColour[light][3] = 0xff;
			}

			// The base colour as BGRA, with the diffuse material's alpha
			out.colour[0] = ColourByte(base[2]);
			out.colour[1] = ColourByte(base[1]);
			out.colour[2] = ColourByte(base[0]);
			out.colour[3] = ColourByte(diffuseMaterial[3]);
		}

		return LIT_VERTEX_FORMAT;
	}

	uint32_t cVKDriver::LightVertices(uint32_t firstVertex, uint32_t vertexCount)
	{
		if (vertexPointer == nullptr || vertexStride == 0 || vertexCount == 0)
		{
			return 0;
		}

		// Per pixel wherever a light can give direction to the colour
		bool const isAnyLightOn = std::any_of(std::begin(isLightEnabled), std::end(isLightEnabled), [](bool isOn) { return isOn; });
		if (isAnyLightOn && RZVertexFormatNumElements(vertexFormat, kGDElementType_Normal) != 0 && vulkan->IsPerPixelLightingSupported())
		{
			uint32_t const format = LightVerticesPerPixel(firstVertex, vertexCount);
			if (format != 0)
			{
				return format;
			}
		}

		// Choose a format with a colour and the same coordinate sets
		//
		// The normal is left out: the colour it went into is all the shader needs.
		uint32_t const coordinateSets = std::min(RZVertexFormatNumElements(vertexFormat, kGDElementType_TexCoord), 2u);
		eGDVertexFormat const target = (coordinateSets == 0) ? kGDVertexFormat_V3F_C4UB : ((coordinateSets == 1) ? kGDVertexFormat_V3F_C4UB_T2F : kGDVertexFormat_V3F_C4UB_2T2F);

		uint32_t const litFormat = RZMakeVertexFormat(target);
		uint32_t const litStride = RZVertexFormatStride(litFormat);

		// Size the copy to the indices the draw reads, so they still apply to it
		size_t const bytes = (size_t{ firstVertex } + vertexCount) * litStride;
		if (litStride == 0 || bytes > MAXIMUM_LIT_BYTES)
		{
			return 0;
		}

		if (litVertices.size() < bytes)
		{
			litVertices.resize(bytes);
		}

		// Find what the game's vertices carry
		bool const     hasColour      = RZVertexFormatNumElements(vertexFormat, kGDElementType_Color) != 0;
		bool const     hasNormal      = RZVertexFormatNumElements(vertexFormat, kGDElementType_Normal) != 0;
		uint32_t const colourOffset   = hasColour ? RZVertexFormatElementOffset(vertexFormat, kGDElementType_Color, 0) : 0;
		uint32_t const normalOffset   = hasNormal ? RZVertexFormatElementOffset(vertexFormat, kGDElementType_Normal, 0) : 0;
		uint32_t const litColour      = RZVertexFormatElementOffset(litFormat, kGDElementType_Color, 0);

		uint32_t sourceCoordinates[2] = {};
		uint32_t litCoordinates[2]    = {};
		for (uint32_t set = 0; set < coordinateSets; set++)
		{
			sourceCoordinates[set] = RZVertexFormatElementOffset(vertexFormat, kGDElementType_TexCoord, set);
			litCoordinates[set]    = RZVertexFormatElementOffset(litFormat, kGDElementType_TexCoord, set);
		}

		// Gather what is the same for every vertex: the lights that are on, directional
		// ones as unit vectors, and the normal matrix
		uint32_t litLights[LIGHT_COUNT] = {};
		uint32_t litLightCount = 0;
		float    directions[LIGHT_COUNT][3] = {};

		for (uint32_t light = 0; light < LIGHT_COUNT; light++)
		{
			if (!isLightEnabled[light])
			{
				continue;
			}

			litLights[litLightCount++] = light;

			if (lightPosition[light][3] == 0.0f)
			{
				std::memcpy(directions[light], lightPosition[light], sizeof(directions[light]));
				Normalise(directions[light]);
			}
		}

		float normalMatrix[3][3] = {};
		if (hasNormal && litLightCount != 0)
		{
			MakeNormalMatrix(modelViewMatrix, normalMatrix);
		}

		float const* const m = modelViewMatrix;

		// Light each vertex the draw reads
		for (uint32_t index = firstVertex; index < firstVertex + vertexCount; index++)
		{
			uint8_t const* const source = static_cast<uint8_t const*>(vertexPointer) + size_t{ index } * vertexStride;
			uint8_t* const       out    = litVertices.data() + size_t{ index } * litStride;

			float position[3];
			std::memcpy(position, source, sizeof(position));

			// The vertex colour, stored BGRA, or white where there is none, as Direct3D
			// takes a vertex without a diffuse colour
			float vertexColour[4] = { 1.0f, 1.0f, 1.0f, 1.0f };
			if (hasColour)
			{
				uint8_t const* const bgra = source + colourOffset;
				vertexColour[0] = static_cast<float>(bgra[2]) / 255.0f;
				vertexColour[1] = static_cast<float>(bgra[1]) / 255.0f;
				vertexColour[2] = static_cast<float>(bgra[0]) / 255.0f;
				vertexColour[3] = static_cast<float>(bgra[3]) / 255.0f;
			}

			float const* const ambientMaterial = isVertexColourAmbient ? vertexColour : materialAmbient;
			float const* const diffuseMaterial = isAlphaFromVertexColour ? vertexColour : materialDiffuse;

			// Emission and the global ambient light
			float lit[3];
			for (int channel = 0; channel < 3; channel++)
			{
				lit[channel] = materialEmission[channel] + colourMultiplier[channel] * ambientMaterial[channel];
			}

			// Each light on
			if (litLightCount != 0)
			{
				float normal[3] = {};
				float eye[3]    = {};

				if (hasNormal)
				{
					float objectNormal[3];
					std::memcpy(objectNormal, source + normalOffset, sizeof(objectNormal));

					for (int row = 0; row < 3; row++)
					{
						normal[row] = normalMatrix[row][0] * objectNormal[0] + normalMatrix[row][1] * objectNormal[1] + normalMatrix[row][2] * objectNormal[2];
					}

					Normalise(normal);

					for (int row = 0; row < 3; row++)
					{
						eye[row] = m[row] * position[0] + m[4 + row] * position[1] + m[8 + row] * position[2] + m[12 + row];
					}
				}

				for (uint32_t which = 0; which < litLightCount; which++)
				{
					uint32_t const light = litLights[which];

					// Without a normal the diffuse term is zero, so only a vertex with one
					// needs the direction to the light
					float diffuse = 0.0f;
					if (hasNormal)
					{
						float toLight[3];
						if (lightPosition[light][3] == 0.0f)
						{
							std::memcpy(toLight, directions[light], sizeof(toLight));
						}
						else
						{
							float const w = lightPosition[light][3];
							for (int axis = 0; axis < 3; axis++)
							{
								toLight[axis] = lightPosition[light][axis] / w - eye[axis];
							}

							Normalise(toLight);
						}

						diffuse = std::max(0.0f, normal[0] * toLight[0] + normal[1] * toLight[1] + normal[2] * toLight[2]);
					}

					for (int channel = 0; channel < 3; channel++)
					{
						lit[channel] += lightAmbient[light][channel] * ambientMaterial[channel] + lightDiffuse[light][channel] * diffuseMaterial[channel] * diffuse;
					}
				}
			}

			// Write the vertex: the position, the lit colour as BGRA, the coordinates
			std::memcpy(out, position, sizeof(position));

			uint8_t* const colour = out + litColour;
			colour[0] = ColourByte(lit[2]);
			colour[1] = ColourByte(lit[1]);
			colour[2] = ColourByte(lit[0]);
			colour[3] = ColourByte(diffuseMaterial[3]);

			for (uint32_t set = 0; set < coordinateSets; set++)
			{
				std::memcpy(out + litCoordinates[set], source + sourceCoordinates[set], sizeof(float) * 2);
			}
		}

		return litFormat;
	}

	void cVKDriver::DrawLit(uint32_t gdPrimitiveType, uint32_t firstVertex, uint32_t vertexCount, void const* indices, uint32_t indexCount, bool isIndex32Bit)
	{
		uint32_t const litFormat = LightVertices(firstVertex, vertexCount);

		// Count how the lighting extension is used, and describe its first draw, so a log
		// says whether a picture's lighting comes through it at all
		if (litFormat == LIT_VERTEX_FORMAT)
		{
			litDrawsPerPixel++;
		}
		else if (litFormat != 0)
		{
			litDrawsPerVertex++;
		}

		if (litDrawsPerPixel + litDrawsPerVertex == 1)
		{
			for (uint32_t light = 0; light < LIGHT_COUNT; light++)
			{
				if (isLightEnabled[light])
				{
					LogInfo("Lighting extension: first lit draw (%s, format 0x%x, %u vertices): light %u at %.2f %.2f %.2f w %.0f, diffuse %.2f %.2f %.2f, ambient %.2f %.2f %.2f.", litFormat == LIT_VERTEX_FORMAT ? "per pixel" : "per vertex", vertexFormat, vertexCount, light, static_cast<double>(lightPosition[light][0]), static_cast<double>(lightPosition[light][1]), static_cast<double>(lightPosition[light][2]), static_cast<double>(lightPosition[light][3]), static_cast<double>(lightDiffuse[light][0]), static_cast<double>(lightDiffuse[light][1]), static_cast<double>(lightDiffuse[light][2]), static_cast<double>(lightAmbient[light][0]), static_cast<double>(lightAmbient[light][1]), static_cast<double>(lightAmbient[light][2]));
				}
			}
		}

		if (litFormat == 0)
		{
			if (indices == nullptr)
			{
				vulkan->DrawVertices(gdPrimitiveType, vertexFormat, vertexPointer, firstVertex, vertexCount);
			}
			else
			{
				vulkan->DrawIndexedVertices(gdPrimitiveType, vertexFormat, vertexPointer, indices, indexCount, isIndex32Bit);
			}

			return;
		}

		// Draw with the shader taking the colour and alpha from the vertices as they are,
		// then put the shader's own lighting back for the next draw
		vulkan->SetSceneTint(1.0f, 1.0f, 1.0f, 1.0f, true, true);

		if (indices == nullptr)
		{
			vulkan->DrawVertices(gdPrimitiveType, litFormat, litVertices.data(), firstVertex, vertexCount);
		}
		else
		{
			vulkan->DrawIndexedVertices(gdPrimitiveType, litFormat, litVertices.data(), indices, indexCount, isIndex32Bit);
		}

		PushLighting();
	}

	//// Public API: cIGZGDriverLightingExtension

	void cVKDriver::EnableLighting(bool isEnabled)
	{
		SCVK_CALL("%d", isEnabled);

		isFixedLightingEnabled = isEnabled;
		PushLighting();
	}

	void cVKDriver::EnableLight(uint32_t light, bool isEnabled)
	{
		SCVK_CALL("%u, %d", light, isEnabled);

		if (light >= LIGHT_COUNT)
		{
			SetLastError(DriverError::OUT_OF_RANGE);
			return;
		}

		isLightEnabled[light] = isEnabled;
	}

	void cVKDriver::LightModelAmbient(float red, float green, float blue, float alpha)
	{
		SCVK_CALL("%.3f, %.3f, %.3f, %.3f", red, green, blue, alpha);

		// The global ambient light, which is the colour multiplier: a Direct3D colour,
		// clamped to 0 to 1, its alpha unused
		colourMultiplier[0] = Clamp01(red);
		colourMultiplier[1] = Clamp01(green);
		colourMultiplier[2] = Clamp01(blue);
		PushSceneTint();
	}

	void cVKDriver::LightColor(uint32_t light, uint32_t type, float const* colour)
	{
		SCVK_CALL("%u, %u, %p", light, type, colour);

		if (light >= LIGHT_COUNT || type > COLOUR_SPECULAR || colour == nullptr)
		{
			SetLastError(light >= LIGHT_COUNT ? DriverError::OUT_OF_RANGE : DriverError::INVALID_VALUE);
			return;
		}

		float* const destination = (type == COLOUR_AMBIENT) ? lightAmbient[light] : ((type == COLOUR_DIFFUSE) ? lightDiffuse[light] : lightSpecular[light]);
		std::memcpy(destination, colour, sizeof(float) * 4);
	}

	void cVKDriver::LightColor(uint32_t light, float const* ambient, float const* diffuse, float const* specular)
	{
		SCVK_CALL("%u, %p, %p, %p", light, ambient, diffuse, specular);

		if (ambient != nullptr)  { LightColor(light, COLOUR_AMBIENT, ambient); }
		if (diffuse != nullptr)  { LightColor(light, COLOUR_DIFFUSE, diffuse); }
		if (specular != nullptr) { LightColor(light, COLOUR_SPECULAR, specular); }
	}

	void cVKDriver::LightPosition(uint32_t light, float const* position)
	{
		SCVK_CALL("%u, %p", light, position);

		if (light >= LIGHT_COUNT || position == nullptr)
		{
			SetLastError(light >= LIGHT_COUNT ? DriverError::OUT_OF_RANGE : DriverError::INVALID_VALUE);
			return;
		}

		std::memcpy(lightPosition[light], position, sizeof(lightPosition[light]));
	}

	void cVKDriver::LightDirection(uint32_t light, float const* direction)
	{
		SCVK_CALL("%u, %p", light, direction);

		if (light >= LIGHT_COUNT || direction == nullptr)
		{
			SetLastError(light >= LIGHT_COUNT ? DriverError::OUT_OF_RANGE : DriverError::INVALID_VALUE);
			return;
		}

		// A direction is a position at infinity
		std::memcpy(lightPosition[light], direction, sizeof(float) * 3);
		lightPosition[light][3] = 0.0f;
	}

	void cVKDriver::MaterialColor(uint32_t type, float const* colour)
	{
		SCVK_CALL("%u, %p", type, colour);

		if (type > COLOUR_SHININESS || colour == nullptr)
		{
			SetLastError(DriverError::INVALID_VALUE);
			return;
		}

		if (type == COLOUR_SHININESS)
		{
			materialShininess = colour[0];
			return;
		}

		float* const destination = (type == COLOUR_AMBIENT) ? materialAmbient : ((type == COLOUR_DIFFUSE) ? materialDiffuse : ((type == COLOUR_SPECULAR) ? materialSpecular : materialEmission));
		std::memcpy(destination, colour, sizeof(float) * 4);

		// The diffuse alpha is the alpha multiplier's, which the shader's own lighting reads
		if (type == COLOUR_DIFFUSE)
		{
			colourMultiplier[3] = materialDiffuse[3];
			PushSceneTint();
		}
	}

	void cVKDriver::MaterialColor(float const* ambient, float const* diffuse, float const* specular, float const* emission, float shininess)
	{
		SCVK_CALL("%p, %p, %p, %p, %.3f", ambient, diffuse, specular, emission, shininess);

		if (ambient != nullptr)  { std::memcpy(materialAmbient, ambient, sizeof(materialAmbient)); }
		if (specular != nullptr) { std::memcpy(materialSpecular, specular, sizeof(materialSpecular)); }
		if (emission != nullptr) { std::memcpy(materialEmission, emission, sizeof(materialEmission)); }
		if (shininess >= 0.0f)   { materialShininess = shininess; }

		if (diffuse != nullptr)
		{
			std::memcpy(materialDiffuse, diffuse, sizeof(materialDiffuse));
			colourMultiplier[3] = materialDiffuse[3];
			PushSceneTint();
		}
	}
}
