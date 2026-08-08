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
 * Almost none of this maps onto Vulkan directly. Alpha test, fog, shade model
 * and the matrix stack were removed from the programmable pipeline entirely,
 * so they will have to be reproduced in shader code driven by uniforms. The
 * depth, stencil and blend state does survive, but as immutable fields of a
 * pipeline object rather than as commands, which is why the eventual design
 * collapses all of this into a state key that selects a cached pipeline.
 *
 * For now each method records that it was called.
 */

#include "cVKDriver.h"
#include "Logger.h"
#include "VulkanBackend.h"

#include <string.h>

namespace scvk
{
	namespace
	{
		// The game's clear mask, in its own encoding rather than GL's.
		constexpr uint32_t kClearDepth   = 0x1000;
		constexpr uint32_t kClearStencil = 0x2000;
		constexpr uint32_t kClearColour  = 0x4000;
	}

	void cVKDriver::PushDepthState(void)
	{
		vulkan->SetDepthState(enabledCapabilities[kGDCapability_DepthTest], depthWrite, depthCompare);
	}

	void cVKDriver::PushBlendState(void)
	{
		vulkan->SetBlendState(enabledCapabilities[kGDCapability_Blend], blendSrcFactor, blendDstFactor);
	}

	void cVKDriver::PushAlphaTest(void)
	{
		// A negative comparison is how the shader is told the test is off.
		vulkan->SetAlphaTest(enabledCapabilities[kGDCapability_AlphaTest] ? static_cast<int>(alphaFunc) : -1, alphaRef);
	}

	void cVKDriver::Clear(uint32_t mask)
	{
		SCVK_CALL("0x%x", mask);

		// Only the colour buffer for now. There is no depth or stencil
		// attachment yet, because nothing renders through a pipeline.
		if ((mask & kClearColour) != 0)
		{
			vulkan->Clear(clearColour[0], clearColour[1], clearColour[2], clearColour[3]);
		}

		if ((mask & kClearDepth) != 0)
		{
			vulkan->ClearDepth(clearDepthValue);
		}
	}

	void cVKDriver::ClearColor(float r, float g, float b, float a)
	{
		SCVK_CALL("%.3f, %.3f, %.3f, %.3f", r, g, b, a);

		clearColour[0] = r;
		clearColour[1] = g;
		clearColour[2] = b;
		clearColour[3] = a;
	}

	void cVKDriver::ClearDepth(double depth)
	{
		SCVK_CALL("%.3f", depth);

		// Kept until the game asks for a clear, matching how ClearColor works.
		// The value needs no conversion: OpenGL's depth clear is already 0 to 1,
		// and the Vulkan clip correction puts depth in the same range.
		clearDepthValue = static_cast<float>(depth);
	}

	void cVKDriver::ClearStencil(int32_t s)
	{
		SCVK_CALL("%d", s);
	}

	void cVKDriver::ColorMask(bool flag)
	{
		SCVK_CALL("%d", flag);
	}

	void cVKDriver::DepthFunc(uint32_t gdTestFunc)
	{
		SCVK_CALL("%u", gdTestFunc);

		depthCompare = gdTestFunc;
		PushDepthState();
	}

	void cVKDriver::DepthMask(bool flag)
	{
		SCVK_CALL("%d", flag);

		depthWrite = flag;
		PushDepthState();
	}

	void cVKDriver::StencilFunc(uint32_t gdTestFunc, int32_t ref, uint32_t mask)
	{
		SCVK_CALL("%u, %d, 0x%x", gdTestFunc, ref, mask);
	}

	void cVKDriver::StencilMask(uint32_t mask)
	{
		SCVK_CALL("0x%x", mask);
	}

	void cVKDriver::StencilOp(uint32_t gdStencilOp, uint32_t gdStencilOp2, uint32_t gdStencilOp3)
	{
		SCVK_CALL("%u, %u, %u", gdStencilOp, gdStencilOp2, gdStencilOp3);
	}

	void cVKDriver::BlendFunc(uint32_t gdBlendFunc, uint32_t gdBlend)
	{
		SCVK_CALL("%u, %u", gdBlendFunc, gdBlend);

		blendSrcFactor = gdBlendFunc;
		blendDstFactor = gdBlend;
		PushBlendState();
	}

	void cVKDriver::AlphaFunc(uint32_t gdTestFunc, float ref)
	{
		// No fixed function alpha test in Vulkan; this becomes a discard in the
		// fragment shader, with the comparison and reference pushed as
		// constants.
		SCVK_CALL("%u, %.3f", gdTestFunc, ref);

		alphaFunc = gdTestFunc;
		alphaRef  = ref;
		PushAlphaTest();
	}

	void cVKDriver::ShadeModel(uint32_t gdShade)
	{
		SCVK_CALL("%u", gdShade);
	}

	void cVKDriver::Fog(uint32_t gdFogParamType, uint32_t gdFogParam)
	{
		SCVK_CALL("%u, %u", gdFogParamType, gdFogParam);
	}

	void cVKDriver::Fog(uint32_t gdFogParamType, float const* params)
	{
		SCVK_CALL("%u, %p", gdFogParamType, params);
	}

	void cVKDriver::ColorMultiplier(float r, float g, float b)
	{
		SCVK_CALL("%.3f, %.3f, %.3f", r, g, b);
	}

	void cVKDriver::AlphaMultiplier(float a)
	{
		SCVK_CALL("%.3f", a);
	}

	void cVKDriver::EnableVertexColors(bool ambient, bool diffuse)
	{
		SCVK_CALL("%d, %d", ambient, diffuse);
	}

	void cVKDriver::MatrixMode(uint32_t gdMatrixTarget)
	{
		SCVK_CALL("%u", gdMatrixTarget);

		// The game's own mapping is 0 modelview, 1 projection. It also names
		// texture and colour matrices, but never selects them.
		activeMatrix = gdMatrixTarget;
	}

	void cVKDriver::LoadMatrix(float const* m)
	{
		SCVK_CALL("%p", m);

		if (m == nullptr)
		{
			return;
		}

		float* target = (activeMatrix == 1) ? projectionMatrix : modelViewMatrix;
		memcpy(target, m, sizeof(float) * 16);
	}

	void cVKDriver::LoadIdentity(void)
	{
		SCVK_CALL("");

		static float const identity[16] = {
			1, 0, 0, 0,
			0, 1, 0, 0,
			0, 0, 1, 0,
			0, 0, 0, 1,
		};

		float* target = (activeMatrix == 1) ? projectionMatrix : modelViewMatrix;
		memcpy(target, identity, sizeof(identity));
	}

	void cVKDriver::Enable(uint32_t gdDriverState)
	{
		SCVK_CALL("%u", gdDriverState);

		if (gdDriverState < kGDNumCapabilities)
		{
			enabledCapabilities[gdDriverState] = true;

			if (gdDriverState == kGDCapability_Blend)     PushBlendState();
			if (gdDriverState == kGDCapability_AlphaTest) PushAlphaTest();
			if (gdDriverState == kGDCapability_DepthTest) PushDepthState();
		}
		else
		{
			SetLastError(DriverError::InvalidEnum);
		}
	}

	void cVKDriver::Disable(uint32_t gdDriverState)
	{
		SCVK_CALL("%u", gdDriverState);

		if (gdDriverState < kGDNumCapabilities)
		{
			enabledCapabilities[gdDriverState] = false;

			if (gdDriverState == kGDCapability_Blend)     PushBlendState();
			if (gdDriverState == kGDCapability_AlphaTest) PushAlphaTest();
			if (gdDriverState == kGDCapability_DepthTest) PushDepthState();
		}
		else
		{
			SetLastError(DriverError::InvalidEnum);
		}
	}

	bool cVKDriver::IsEnabled(uint32_t gdDriverState)
	{
		SCVK_CALL("%u", gdDriverState);

		if (gdDriverState >= kGDNumCapabilities)
		{
			SetLastError(DriverError::InvalidEnum);
			return false;
		}

		return enabledCapabilities[gdDriverState];
	}

	void cVKDriver::GetBoolean(uint32_t gdParameter, bool* params)
	{
		SCVK_CALL("%u, %p", gdParameter, params);

		if (params != nullptr)
		{
			*params = false;
		}
	}

	void cVKDriver::GetInteger(uint32_t gdParameter, int32_t* params)
	{
		SCVK_CALL("%u, %p", gdParameter, params);

		if (params != nullptr)
		{
			*params = 0;
		}
	}

	void cVKDriver::GetFloat(uint32_t gdParameter, float* params)
	{
		SCVK_CALL("%u, %p", gdParameter, params);

		if (params != nullptr)
		{
			*params = 0.0f;
		}
	}

	void cVKDriver::PolygonOffset(int32_t offset)
	{
		SCVK_CALL("%d", offset);
	}
}
