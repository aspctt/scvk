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
 * clears whole for its terrain decals after the water is drawn.
 *
 * The level is kept for each pixel, since one level does not hold while the view scrolls.
 * A scroll moves the saved view by whole pixels and draws the strips it uncovers, while
 * the camera also moves a little in depth, by a different amount each frame. The pixels
 * moved keep the depths they were drawn with, so after a few frames the screen held water
 * at several levels, and taking the newest strip's for all of it lost every reflection
 * until the game redrew the view whole, at the widest zooms within a frame or two. So the
 * backend keeps the level each pixel's water was drawn at: every water draw fills its
 * pass's rectangle, and a scroll moves the levels with the saved view (NoteViewShift). A
 * move along the screen also raises the moved pixels' heights by as much as the world's
 * up leans into it, which every kept level gains, as an offset they are kept against.
 *
 * The game puts its saved scene back before every redraw of the 3D view, so the
 * reflections are worked out again each time from the clean picture and never stack. They
 * show what is in the picture, boats and cars included.
 *
 * With live shadows on, the network pieces and True3D props they keep, off screen ones
 * included, are also drawn mirrored in the water's level (DrawMirroredPieces, in
 * cVKDriver_LiveShadows.cpp beside the casters it reads). At the closest zoom a bridge's
 * deck lies off the top of the screen while its reflection should still lie in the water
 * below, and the picture never shows the deck from beneath. The mirror is drawn again
 * only after a static pass, which is when the pieces and the view change.
 */

//// Dependencies

#include "cVKDriver.h"
#include "Logger.h"
#include "Settings.h"
#include "VulkanBackend.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <new>

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

		// How far apart one pass's water draws are before the log says so, in view units
		constexpr float LOGGED_LEVEL_SPREAD = 0.25f;

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

		// The same level in the world, and the view units per world unit along up, for the
		// pieces mirrored in it
		waterWorldLevel = position[1];
		waterViewScale  = length;

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

		// The pixels this pass draws hold water at this level, whatever the saved view
		// around them holds
		//
		// Kept against the projection's own corner, which the game moves by a fraction of a
		// pixel from one water draw to the next, even within one pass: a view unit at the
		// widest zooms, past what a pixel on the water may read off. The level a pixel
		// gives through the pass's corner holds whichever pass's corner reads it. A pass's
		// water draws can still differ by that much, so the rectangle keeps the lowest and
		// the highest.
		float const relativeLevel = level - (waterReflectionOrigin[0] * up[0] + waterReflectionOrigin[1] * up[1] + waterReflectionOrigin[2] * up[2]);

		int32_t const rectangle[4] = { viewportX, windowHeight - viewportY - viewportHeight, viewportWidth, viewportHeight };

		bool const isSameRectangle = !waterLevelFills.empty() && std::memcmp(waterLevelFills.back().rectangle, rectangle, sizeof(rectangle)) == 0;
		if (isSameRectangle)
		{
			WaterLevelFill& last = waterLevelFills.back();
			last.lowest  = std::min(last.lowest, relativeLevel);
			last.highest = std::max(last.highest, relativeLevel);
		}
		else
		{
			WaterLevelFill fill;
			std::memcpy(fill.rectangle, rectangle, sizeof(rectangle));
			fill.lowest  = relativeLevel;
			fill.highest = relativeLevel;

			try
			{
				waterLevelFills.push_back(fill);
			}
			catch (std::bad_alloc const&)
			{
			}
		}

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
			LogInfo("Water reflections: water drawn, up %.3f/%.3f/%.3f, level %.2f, %.4f/%.4f view units per pixel, %.1f per unit of depth, world height %.2f.", static_cast<double>(up[0]), static_cast<double>(up[1]), static_cast<double>(up[2]), static_cast<double>(level), static_cast<double>(waterReflectionScale[0]), static_cast<double>(waterReflectionScale[1]), static_cast<double>(waterReflectionScale[2]), static_cast<double>(waterWorldLevel));
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

		// The water levels as this frame left them, and the pieces mirrored in the water,
		// drawn first when they changed
		FlushWaterLevels();

		float tallest = 0.0f;
		bool const hasMirror = DrawMirroredPieces(UNDERSIDE_SHADE, tallest);

		// The kept levels are read back against what the scrolls added since
		WaterReflectionConstants constants{};
		std::memcpy(constants.origin, waterReflectionOrigin, sizeof(constants.origin));
		constants.origin[3]  = static_cast<float>(waterLevelOffset);
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

		constants.mirror[0] = hasMirror ? 1.0f : 0.0f;
		constants.mirror[1] = tallest;

		vulkan->DrawWaterReflections(constants);

		static bool hasLoggedPass = false;
		if (!hasLoggedPass)
		{
			hasLoggedPass = true;
			LogInfo("Water reflections: first pass %s, water within %.2f of its level.", vulkan->CanDrawReflections() ? "drawn" : "skipped, since this device cannot sample its depth", static_cast<double>(tolerance));
		}
	}

	void cVKDriver::NoteViewShift(int32_t regionX, int32_t regionY, int32_t width, int32_t height, int32_t screenX, int32_t screenY)
	{
		// A scroll restores most of the saved view moved by whole pixels, then the game draws
		// the strips it uncovered. Every other restore puts pixels back where they were, or
		// is a small part of the view.
		int32_t const right = screenX - regionX;
		int32_t const down  = screenY - regionY;

		bool const isMoved      = right != 0 || down != 0;
		bool const isMostOfView = width >= windowWidth / 2 && height >= windowHeight / 2;
		if (!areWaterReflectionsOn || !isMoved || !isMostOfView)
		{
			return;
		}

		// The colour and the depth are restored alike, so a second restore of the same move
		// with nothing drawn between is that move again
		bool const isSameMove = hasWaterLevelShift && waterLevelShift[0] == right && waterLevelShift[1] == down && waterLevelFills.empty();
		if (isSameMove)
		{
			return;
		}

		// A move the reflections have not taken yet goes first, with the strips after it
		FlushWaterLevels();

		waterLevelShift[0] = right;
		waterLevelShift[1] = down;
		hasWaterLevelShift = true;

		mirrorPendingShift[0] += right;
		mirrorPendingShift[1] += down;
		hasMirrorPendingShift  = true;
	}

	void cVKDriver::FlushWaterLevels(void)
	{
		// The move first, since the game moved the saved view before it drew the strips
		//
		// A moved pixel's view position lies the move's view units further along the screen,
		// which the world's up leans into, so its height and every kept level gain as much.
		if (hasWaterLevelShift)
		{
			hasWaterLevelShift = false;
			vulkan->ShiftWaterLevels(waterLevelShift[0], waterLevelShift[1]);

			waterLevelOffset += static_cast<double>(waterLevelShift[0]) * waterReflectionScale[0] * waterReflectionUp[0] + static_cast<double>(waterLevelShift[1]) * waterReflectionScale[1] * waterReflectionUp[1];
		}

		// Then the water drawn since, kept against the offset as it now stands
		for (WaterLevelFill const& fill : waterLevelFills)
		{
			vulkan->FillWaterLevels(fill.rectangle, static_cast<float>(static_cast<double>(fill.lowest) - waterLevelOffset), static_cast<float>(static_cast<double>(fill.highest) - waterLevelOffset));

			// Say once how far apart one pass's water draws were, past a quarter of a view unit
			static bool hasLoggedSpread = false;
			if (!hasLoggedSpread && fill.highest - fill.lowest > LOGGED_LEVEL_SPREAD)
			{
				hasLoggedSpread = true;
				LogInfo("Water reflections: one pass drew its water %.2f view units apart.", static_cast<double>(fill.highest - fill.lowest));
			}
		}

		waterLevelFills.clear();
	}
}
