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
 * Water reflections (-Reflections:on).
 *
 * The game draws its water as one pass of terrain-shaped strips just above the seabed,
 * after the terrain and the cloud shadows, in a state no other draw of its uses (see
 * VulkanBackend::IsWaterDraw). That draw gives the view and the water's level in it. Once
 * the 3D view is finished, before any interface is drawn over it, a full-screen pass takes
 * every pixel whose depth puts it on that level for water, mirrors the view in it, and
 * follows the reflected ray up the screen through the scene depth.
 *
 * Water is told by its level rather than by a mark in the stencil, which SC4RenderServices
 * clears whole for its terrain decals after the water is drawn. The level holds while the
 * view scrolls: an orthographic camera moved along the ground leaves every point's height
 * where it was, and only a zoom or a rotation changes the view, both of which redraw the
 * water.
 *
 * The game puts its saved scene back before every redraw of the 3D view, so the
 * reflections are worked out again each time from the clean picture and never stack. They
 * show what is in the picture, boats and cars included, and nothing off screen.
 */

//// Dependencies

#include "cVKDriver.h"
#include "Logger.h"
#include "Settings.h"
#include "VulkanBackend.h"

#include <algorithm>
#include <cmath>
#include <cstring>

namespace scvk
{
	//// Constants

	namespace
	{
		// How much of the reflected colour is laid over the water. Real water reflects far
		// less at the angles SimCity 4 looks down from, but the game paints its water
		// bright and flat, and a weaker reflection vanished into it.
		constexpr float REFLECTION_STRENGTH = 0.35f;

		// The longest march, as a share of the back buffer's height. A tall pylon's
		// reflection at the closest zoom runs that far, and a shorter march cut it off.
		constexpr float MARCH_SHARE = 1.0f;

		// How far the waves tilt the water's normal, and how dark an underside the
		// picture never shows is taken to be, against the colour of its top
		constexpr float WAVE_TILT       = 0.03f;
		constexpr float UNDERSIDE_SHADE = 0.35f;

		// The least thickness a surface is taken to have, in view units
		constexpr float MINIMUM_THICKNESS = 2.0f;

		// How far off the water's level a pixel still counts as water, at least, in view
		// units, and in steps of a 16-bit depth buffer along the world's up, which is what
		// the depth of a pixel on the water is good for
		constexpr float MINIMUM_WATER_TOLERANCE = 0.5f;
		constexpr float WATER_TOLERANCE_STEPS   = 3.0f;
		constexpr float DEPTH_STEPS             = 65536.0f;

		// Shows the water mask in magenta instead of the reflections, to check which
		// pixels the driver takes for water
		constexpr char const* DEBUG_REFLECTIONS_MARKER = "scvk-debug-reflections";
	}

	//// Public API

	void cVKDriver::StartWaterReflections(void)
	{
		areWaterReflectionsOn = GetSettings().areWaterReflectionsEnabled;
		if (!areWaterReflectionsOn)
		{
			return;
		}

		shouldShowWaterMask = HasMarkerFile(DEBUG_REFLECTIONS_MARKER);
		LogInfo("Water reflections: on%s.", shouldShowWaterMask ? ", showing the water mask instead" : "");
	}

	void cVKDriver::NoteWaterDraw(uint8_t const* firstVertex)
	{
		// Sanity check
		float const* const p = projectionMatrix;

		bool const isOrthographic = p[3] == 0.0f && p[7] == 0.0f && p[11] == 0.0f;
		if (firstVertex == nullptr || !isOrthographic || viewportWidth <= 0 || viewportHeight <= 0 || std::fabs(p[0]) < 1.0e-12f || std::fabs(p[5]) < 1.0e-12f || std::fabs(p[10]) < 1.0e-12f)
		{
			return;
		}

		// The water's vertices are world positions, as the terrain's are, so its model view
		// is the view itself, and the image of its y axis the world's up in view space
		//
		// The matrices are column major.
		float const* const m = modelViewMatrix;
		float up[3] = { m[4], m[5], m[6] };

		float const length = std::sqrt(up[0] * up[0] + up[1] * up[1] + up[2] * up[2]);
		if (!(length > 1.0e-6f))
		{
			return;
		}

		// Facing the camera, which looks down -z
		float const facing = (up[2] < 0.0f) ? -1.0f : 1.0f;
		for (float& component : up)
		{
			component *= facing / length;
		}

		// The water's level: its first vertex's position along the world's up, in view space
		//
		// The vertex array is untyped, and its format starts with the position's three
		// floats.
		float position[3] = {};
		std::memcpy(position, firstVertex, sizeof(position));

		float viewPosition[3] = {};
		for (unsigned row = 0; row < 3; row++)
		{
			viewPosition[row] = m[row] * position[0] + m[4 + row] * position[1] + m[8 + row] * position[2] + m[12 + row];
		}

		float const level = viewPosition[0] * up[0] + viewPosition[1] * up[1] + viewPosition[2] * up[2];

		// The view position of a pixel and depth
		//
		// The projection is orthographic, so it is an affine function of the coordinates and
		// the depth: the top left corner's position at depth 0, plus the view units per pixel
		// and per unit of depth. The viewport counts up from the bottom, pixels down from the
		// top, and the depth runs 0 to 1 over -1 to 1. A partial pass's projection, fitted to
		// its rectangle, gives the same function with its own viewport. Viewport sizes are a
		// few thousand at most, which a float holds exactly.
		float const width  = static_cast<float>(viewportWidth);
		float const height = static_cast<float>(viewportHeight);
		float const left   = static_cast<float>(viewportX);
		float const top    = static_cast<float>(windowHeight - viewportY - viewportHeight);

		waterReflectionScale[0] = 2.0f / (width * p[0]);
		waterReflectionScale[1] = -2.0f / (height * p[5]);
		waterReflectionScale[2] = 2.0f / p[10];

		waterReflectionOrigin[0] = (-2.0f * left / width - 1.0f - p[12]) / p[0];
		waterReflectionOrigin[1] = (1.0f + 2.0f * top / height - p[13]) / p[5];
		waterReflectionOrigin[2] = (-1.0f - p[14]) / p[10];
		waterReflectionOrigin[3] = level;

		// The world's ground axes in view space and its origin's view position, which fix
		// the waves to the ground. The model view of the water is rigid, so its columns
		// only need normalising against rounding.
		float const lengthX = std::sqrt(m[0] * m[0] + m[1] * m[1] + m[2] * m[2]);
		float const lengthZ = std::sqrt(m[8] * m[8] + m[9] * m[9] + m[10] * m[10]);
		if (!(lengthX > 1.0e-6f) || !(lengthZ > 1.0e-6f))
		{
			return;
		}

		for (unsigned axis = 0; axis < 3; axis++)
		{
			waterReflectionUp[axis]     = up[axis];
			waterReflectionAxisX[axis]  = m[axis] / lengthX;
			waterReflectionAxisZ[axis]  = m[8 + axis] / lengthZ;
			waterReflectionOffset[axis] = m[12 + axis];
		}

		// Say once what the first water draw gave, which a missing reflection is checked
		// against. %f takes doubles.
		if (!isWaterReflectionViewValid)
		{
			LogInfo("Water reflections: water drawn, up %.3f/%.3f/%.3f, level %.2f, %.4f/%.4f view units per pixel, %.1f per unit of depth.", static_cast<double>(up[0]), static_cast<double>(up[1]), static_cast<double>(up[2]), static_cast<double>(level), static_cast<double>(waterReflectionScale[0]), static_cast<double>(waterReflectionScale[1]), static_cast<double>(waterReflectionScale[2]));
		}

		isWaterReflectionViewValid = true;
	}

	void cVKDriver::RenderWaterReflections(void)
	{
		if (!areWaterReflectionsOn || !isWaterReflectionViewValid)
		{
			return;
		}

		// How far off the level a pixel on the water can read, from the depth's steps
		float const depthStep = std::fabs(waterReflectionScale[2]) / DEPTH_STEPS;
		float const tolerance = std::max(MINIMUM_WATER_TOLERANCE, WATER_TOLERANCE_STEPS * depthStep * std::fabs(waterReflectionUp[2]));

		WaterReflectionConstants constants{};
		std::memcpy(constants.origin, waterReflectionOrigin, sizeof(constants.origin));
		constants.scale[0]   = waterReflectionScale[0];
		constants.scale[1]   = waterReflectionScale[1];
		constants.scale[2]   = waterReflectionScale[2];
		constants.scale[3]   = REFLECTION_STRENGTH;
		constants.up[0]      = waterReflectionUp[0];
		constants.up[1]      = waterReflectionUp[1];
		constants.up[2]      = waterReflectionUp[2];
		constants.up[3]      = MARCH_SHARE;
		constants.options[0] = shouldShowWaterMask ? 1.0f : 0.0f;
		constants.options[1] = MINIMUM_THICKNESS;
		constants.options[2] = tolerance;
		constants.options[3] = UNDERSIDE_SHADE;

		std::memcpy(constants.axisX, waterReflectionAxisX, sizeof(waterReflectionAxisX));
		std::memcpy(constants.axisZ, waterReflectionAxisZ, sizeof(waterReflectionAxisZ));
		std::memcpy(constants.translation, waterReflectionOffset, sizeof(waterReflectionOffset));
		constants.axisZ[3] = WAVE_TILT;

		vulkan->DrawWaterReflections(constants);

		static bool hasLoggedPass = false;
		if (!hasLoggedPass)
		{
			hasLoggedPass = true;
			LogInfo("Water reflections: first pass %s, water within %.2f of its level.", vulkan->CanDrawReflections() ? "drawn" : "skipped, since this device cannot sample its depth", static_cast<double>(tolerance));
		}
	}
}
