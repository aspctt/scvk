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
 * Render state.
 *
 * Almost none of this maps onto Vulkan directly. Alpha test, fog, shade model and the
 * matrix stack were removed from the programmable pipeline entirely, so they are
 * reproduced in shader code driven by push constants. The depth, stencil and blend state
 * does survive, but as immutable fields of a pipeline object rather than as commands,
 * which is why all of it is held here and forwarded to the backend, which folds it into
 * the key that selects a cached pipeline.
 */

//// Dependencies

#include "cVKDriver.h"
#include "Logger.h"
#include "VulkanBackend.h"

#include <string.h>

namespace scvk
{
	//// Constants

	namespace
	{
		// The game's clear mask, in its own encoding rather than GL's. Stencil is 0x2000,
		// and there is no stencil attachment to clear.
		constexpr uint32_t CLEAR_DEPTH  = 0x1000;
		constexpr uint32_t CLEAR_COLOUR = 0x4000;

		// The game's matrix targets: 0 modelview, 1 projection. It also names texture and
		// colour matrices, but never selects them.
		constexpr uint32_t PROJECTION_MATRIX = 1;

		constexpr float IDENTITY_MATRIX[16] = {
			1, 0, 0, 0,
			0, 1, 0, 0,
			0, 0, 1, 0,
			0, 0, 0, 1,
		};

		// The tint is reported quantised to eighths, so the day and night cycle reports
		// as a handful of steps rather than once per frame.
		constexpr float TINT_REPORT_STEPS = 8.0f;

		// The game's fog parameter types, in the order of SCGL's table and of the
		// DirectX driver's switch: mode, colour, density, start, end. Only the mode
		// arrives as an integer.
		constexpr uint32_t GD_FOG_MODE    = 0;
		constexpr uint32_t GD_FOG_COLOUR  = 1;
		constexpr uint32_t GD_FOG_DENSITY = 2;
		constexpr uint32_t GD_FOG_START   = 3;
		constexpr uint32_t GD_FOG_END     = 4;

		// The game's fog modes run exponential, squared exponential, linear.
		constexpr uint32_t GD_FOG_MODE_LINEAR = 2;

		// The fog the 3D view's constructor (0x7c9b10) sets up and sends whenever its own
		// fog is switched on: linear and white, from 10000 to 15000 units in front of the
		// camera. A city sits around 9500 units out.
		constexpr float FORCED_FOG_START     = 10000.0f;
		constexpr float FORCED_FOG_END       = 15000.0f;
		constexpr float FORCED_FOG_COLOUR[4] = { 1.0f, 1.0f, 1.0f, 1.0f };
	}

	//// Private Functions

	void cVKDriver::SetCapability(uint32_t gdCapability, bool isEnabled)
	{
		// Texturing is per stage, not global: it applies to whichever stage TexStage last
		// selected. Treating it as one flag leaves the second stage looking live when the
		// game has switched it off, which paints the terrain with the placeholder bound
		// to it.
		if (gdCapability == kGDCapability_Texture2D)
		{
			SetTextureStageEnabled(isEnabled);
			return;
		}

		if (gdCapability >= kGDNumCapabilities)
		{
			SetLastError(DriverError::INVALID_ENUM);
			return;
		}

		// Record it, and forward what the backend keys on
		isCapabilityEnabled[gdCapability] = isEnabled;

		if (gdCapability == kGDCapability_Blend)
		{
			PushBlendState();
		}

		if (gdCapability == kGDCapability_AlphaTest)
		{
			PushAlphaTest();
		}

		if (gdCapability == kGDCapability_DepthTest)
		{
			PushDepthState();
		}

		if (gdCapability == kGDCapability_Fog)
		{
			PushFog();
		}
	}

	void cVKDriver::PushBlendState(void)
	{
		vulkan->SetBlendState(isCapabilityEnabled[kGDCapability_Blend], blendSourceFactor, blendDestinationFactor);
	}

	void cVKDriver::PushDepthState(void)
	{
		vulkan->SetDepthState(isCapabilityEnabled[kGDCapability_DepthTest], isDepthWriteEnabled, depthComparison);
	}

	void cVKDriver::PushAlphaTest(void)
	{
		// A negative comparison is how the shader is told the test is off. The game's
		// comparisons run 0 to 7, so they fit an int.
		int const comparison = isCapabilityEnabled[kGDCapability_AlphaTest] ? static_cast<int>(alphaComparison) : -1;
		vulkan->SetAlphaTest(comparison, alphaReference);
	}

	void cVKDriver::PushSceneTint(void)
	{
		PushLighting();

		// Report each distinct tint once
		//
		// Quantised, so the day and night cycle reports as a handful of steps rather than
		// once per frame, and a constant value reports once. The tint is never negative,
		// so its quantised steps convert to unsigned whole numbers.
		uint32_t const red   = static_cast<uint32_t>(colourMultiplier[0] * TINT_REPORT_STEPS) & 0xff;
		uint32_t const green = static_cast<uint32_t>(colourMultiplier[1] * TINT_REPORT_STEPS) & 0xff;
		uint32_t const blue  = static_cast<uint32_t>(colourMultiplier[2] * TINT_REPORT_STEPS) & 0xff;

		uint32_t const key = red | (green << 8) | (blue << 16) | ((isVertexColourAmbient ? 1u : 0u) << 24) | ((isVertexColourDiffuse ? 1u : 0u) << 25);

		if (NoteOnce(NOTE_SCENE_TINT, key))
		{
			LogNote("  TINT rgb %.3f %.3f %.3f alpha %.3f (vertex colours: ambient %d, diffuse %d)", colourMultiplier[0], colourMultiplier[1], colourMultiplier[2], colourMultiplier[3], isVertexColourAmbient ? 1 : 0, isVertexColourDiffuse ? 1 : 0);
		}
	}

	void cVKDriver::PushLighting(void)
	{
		// Reduce the game's lighting to what its DirectX driver draws
		//
		// The colour multiplier is the global ambient light, the alpha multiplier the
		// diffuse material's alpha, and the vertex colour stands in for the ambient
		// material, the diffuse material, both or neither. The DirectX driver keeps the
		// ambient material white.
		//
		// There is a directional light too, but the DirectX driver never hands Direct3D a
		// normal, and Direct3D lights a vertex without one with a dot product of zero. The
		// light adds nothing there, which leaves:
		//
		//   rgb   = ambient light * vertex colour, when ambient is mapped
		//         = ambient light,                when it is not
		//   alpha = the vertex alpha or the alpha multiplier, see AlphaMultiplier
		//
		// OpenGL lights the default normal (0,0,1) instead, which adds about 0.59 of the
		// vertex colour wherever diffuse is mapped. scvk did that once, and it washed the
		// sea out to white and brightened the shaded cliff faces by half.
		vulkan->SetSceneTint(colourMultiplier[0], colourMultiplier[1], colourMultiplier[2], colourMultiplier[3], isVertexColourAmbient, isAlphaFromVertexColour);
	}

	void cVKDriver::PushFog(void)
	{
		// Stand in the 3D view's own fog while the marker forces it
		//
		// Only where the game has the fog off, so fog the game does send still draws as
		// sent. The interface lies at or just behind the camera and never reaches the
		// start distance, so it stays clear.
		if (shouldForceFog && !isCapabilityEnabled[kGDCapability_Fog])
		{
			vulkan->SetFog(true, GD_FOG_MODE_LINEAR, 1.0f, FORCED_FOG_START, FORCED_FOG_END, FORCED_FOG_COLOUR);
			return;
		}

		vulkan->SetFog(isCapabilityEnabled[kGDCapability_Fog], fogMode, fogDensity, fogStart, fogEnd, fogColour);
	}

	//// Public API

	void cVKDriver::Clear(uint32_t mask)
	{
		SCVK_CALL("0x%x", mask);

		// Trace the clear
		//
		// A clear in a frame that restored the scene is rare enough to be worth the whole
		// frame's steps.
		if (hasRegionFrameRestored)
		{
			isRegionFrameInteresting = true;
		}

		NoteRegionStep("clear 0x%x (colour write %d, depth write %d)", mask, isColourWriteEnabled ? 1 : 0, isDepthWriteEnabled ? 1 : 0);

		// Clear what the write masks allow
		//
		// The masks apply to clears as they do to draws, so a buffer masked off is left
		// alone.
		if ((mask & CLEAR_COLOUR) != 0 && isColourWriteEnabled)
		{
			vulkan->Clear(clearColour[0], clearColour[1], clearColour[2], clearColour[3]);
		}

		if ((mask & CLEAR_DEPTH) != 0 && isDepthWriteEnabled)
		{
			vulkan->ClearDepth(clearDepthValue);
		}
	}

	void cVKDriver::ClearColor(float red, float green, float blue, float alpha)
	{
		SCVK_CALL("%.3f, %.3f, %.3f, %.3f", red, green, blue, alpha);

		clearColour[0] = red;
		clearColour[1] = green;
		clearColour[2] = blue;
		clearColour[3] = alpha;
	}

	void cVKDriver::ClearDepth(double depth)
	{
		SCVK_CALL("%.3f", depth);

		// Kept until the game asks for a clear, matching how ClearColor works. The value
		// needs no conversion beyond precision: OpenGL's depth clear is already 0 to 1,
		// and the Vulkan clip correction puts depth in the same range.
		clearDepthValue = static_cast<float>(depth);
	}

	void cVKDriver::ClearStencil(int32_t stencil)
	{
		SCVK_CALL("%d", stencil);
	}

	void cVKDriver::ColorMask(bool isEnabled)
	{
		SCVK_CALL("%d", isEnabled);

		// One flag for all four channels. A pass drawn with it off would otherwise paint
		// whatever it carries over the scene.
		isColourWriteEnabled = isEnabled;
		vulkan->SetColourWrite(isEnabled);
	}

	void cVKDriver::DepthFunc(uint32_t gdComparison)
	{
		SCVK_CALL("%u", gdComparison);

		depthComparison = gdComparison;
		PushDepthState();
	}

	void cVKDriver::DepthMask(bool isEnabled)
	{
		SCVK_CALL("%d", isEnabled);

		isDepthWriteEnabled = isEnabled;
		PushDepthState();
	}

	void cVKDriver::StencilFunc(uint32_t gdComparison, int32_t reference, uint32_t mask)
	{
		SCVK_CALL("%u, %d, 0x%x", gdComparison, reference, mask);
	}

	void cVKDriver::StencilMask(uint32_t mask)
	{
		SCVK_CALL("0x%x", mask);
	}

	void cVKDriver::StencilOp(uint32_t gdStencilFailOperation, uint32_t gdDepthFailOperation, uint32_t gdPassOperation)
	{
		SCVK_CALL("%u, %u, %u", gdStencilFailOperation, gdDepthFailOperation, gdPassOperation);
	}

	void cVKDriver::BlendFunc(uint32_t gdSourceFactor, uint32_t gdDestinationFactor)
	{
		SCVK_CALL("%u, %u", gdSourceFactor, gdDestinationFactor);

		blendSourceFactor      = gdSourceFactor;
		blendDestinationFactor = gdDestinationFactor;
		PushBlendState();
	}

	void cVKDriver::AlphaFunc(uint32_t gdComparison, float reference)
	{
		// No fixed function alpha test in Vulkan; this becomes a discard in the fragment
		// shader, with the comparison and reference pushed as constants.
		SCVK_CALL("%u, %.3f", gdComparison, reference);

		alphaComparison = gdComparison;
		alphaReference  = reference;
		PushAlphaTest();
	}

	void cVKDriver::ShadeModel(uint32_t gdShade)
	{
		SCVK_CALL("%u", gdShade);
	}

	void cVKDriver::Fog(uint32_t gdFogParameterType, uint32_t gdFogParameter)
	{
		SCVK_CALL("%u, %u", gdFogParameterType, gdFogParameter);

		// Only the mode is an integer
		//
		// The DirectX driver refuses the other types through this overload too. The game
		// sets linear fog, and only when its 3D view's fog is on.
		if (gdFogParameterType != GD_FOG_MODE || gdFogParameter > GD_FOG_MODE_LINEAR)
		{
			SetLastError(DriverError::INVALID_ENUM);
			return;
		}

		fogMode = gdFogParameter;
		PushFog();
	}

	void cVKDriver::Fog(uint32_t gdFogParameterType, float const* parameters)
	{
		SCVK_CALL("%u, %p", gdFogParameterType, parameters);

		if (parameters == nullptr)
		{
			SetLastError(DriverError::INVALID_VALUE);
			return;
		}

		// Record the parameter
		//
		// OpenGL clamps each colour component to 0 to 1 as it is set, and refuses a
		// negative density (OpenGL 2.1 spec, section 3.10).
		switch (gdFogParameterType)
		{
		case GD_FOG_COLOUR:
			for (int component = 0; component < 4; component++)
			{
				float const value = parameters[component];
				fogColour[component] = (value < 0.0f) ? 0.0f : ((value > 1.0f) ? 1.0f : value);
			}

			break;

		case GD_FOG_DENSITY:
			if (parameters[0] < 0.0f)
			{
				SetLastError(DriverError::INVALID_VALUE);
				return;
			}

			fogDensity = parameters[0];
			break;

		case GD_FOG_START:
			fogStart = parameters[0];
			break;

		case GD_FOG_END:
			fogEnd = parameters[0];
			break;

		default:
			SetLastError(DriverError::INVALID_ENUM);
			return;
		}

		PushFog();
	}

	void cVKDriver::ColorMultiplier(float red, float green, float blue)
	{
		SCVK_CALL("%.3f, %.3f, %.3f", red, green, blue);

		// The global ambient light colour. With the one fixed light, this is the whole of
		// SimCity 4's lighting, and it is what carries the day and night cycle. See
		// PushLighting.
		colourMultiplier[0] = red;
		colourMultiplier[1] = green;
		colourMultiplier[2] = blue;
		PushSceneTint();
	}

	void cVKDriver::AlphaMultiplier(float alpha)
	{
		SCVK_CALL("%.3f", alpha);

		colourMultiplier[3] = alpha;

		// Take the alpha from the material while it is below one
		//
		// The DirectX driver points the diffuse material back at its own colour for an
		// alpha below one, and back at the vertex colour at one if the vertex colour feeds
		// the diffuse term. A fade therefore wins over the vertex alpha until
		// EnableVertexColors next says otherwise.
		if (alpha < 1.0f)
		{
			isAlphaFromVertexColour = false;
		}
		else if (isVertexColourDiffuse)
		{
			isAlphaFromVertexColour = true;
		}

		PushSceneTint();
	}

	void cVKDriver::EnableVertexColors(bool shouldFeedAmbient, bool shouldFeedDiffuse)
	{
		SCVK_CALL("%d, %d", shouldFeedAmbient, shouldFeedDiffuse);

		// Whether the vertex colour feeds the ambient and diffuse material terms. The
		// diffuse one decides the alpha source outright, whatever the alpha multiplier
		// set before. See PushLighting.
		isVertexColourAmbient   = shouldFeedAmbient;
		isVertexColourDiffuse   = shouldFeedDiffuse;
		isAlphaFromVertexColour = shouldFeedDiffuse;
		PushSceneTint();
	}

	void cVKDriver::MatrixMode(uint32_t gdMatrixTarget)
	{
		SCVK_CALL("%u", gdMatrixTarget);
		activeMatrix = gdMatrixTarget;
	}

	void cVKDriver::LoadMatrix(float const* matrix)
	{
		SCVK_CALL("%p", matrix);

		if (matrix == nullptr)
		{
			return;
		}

		float* const target = (activeMatrix == PROJECTION_MATRIX) ? projectionMatrix : modelViewMatrix;
		memcpy(target, matrix, sizeof(float) * 16);
	}

	void cVKDriver::LoadIdentity(void)
	{
		SCVK_CALL("");

		float* const target = (activeMatrix == PROJECTION_MATRIX) ? projectionMatrix : modelViewMatrix;
		memcpy(target, IDENTITY_MATRIX, sizeof(IDENTITY_MATRIX));
	}

	void cVKDriver::Enable(uint32_t gdCapability)
	{
		SCVK_CALL("%u", gdCapability);
		SetCapability(gdCapability, true);
	}

	void cVKDriver::Disable(uint32_t gdCapability)
	{
		SCVK_CALL("%u", gdCapability);
		SetCapability(gdCapability, false);
	}

	bool cVKDriver::IsEnabled(uint32_t gdCapability)
	{
		SCVK_CALL("%u", gdCapability);

		if (gdCapability == kGDCapability_Texture2D)
		{
			return isTextureStageEnabled[activeTextureStage];
		}

		if (gdCapability >= kGDNumCapabilities)
		{
			SetLastError(DriverError::INVALID_ENUM);
			return false;
		}

		return isCapabilityEnabled[gdCapability];
	}

	void cVKDriver::GetBoolean(uint32_t gdParameter, bool* outValues)
	{
		SCVK_CALL("%u, %p", gdParameter, outValues);

		if (outValues != nullptr)
		{
			*outValues = false;
		}
	}

	void cVKDriver::GetInteger(uint32_t gdParameter, int32_t* outValues)
	{
		SCVK_CALL("%u, %p", gdParameter, outValues);

		if (outValues != nullptr)
		{
			*outValues = 0;
		}
	}

	void cVKDriver::GetFloat(uint32_t gdParameter, float* outValues)
	{
		SCVK_CALL("%u, %p", gdParameter, outValues);

		if (outValues != nullptr)
		{
			*outValues = 0.0f;
		}
	}

	void cVKDriver::PolygonOffset(int32_t offset)
	{
		SCVK_CALL("%d", offset);
	}
}
