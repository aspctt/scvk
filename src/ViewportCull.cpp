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
 * The test behind the viewport cull.
 *
 * While the camera scrolls, the game shifts the picture it already has and redraws only
 * the strip that came into view, through a small viewport. It leaves out most of what lies
 * elsewhere by itself, but about 30% of the draws it still sends fall outside the strip,
 * going by the benchmark's scroll scenes. Vulkan would throw those away after their
 * vertices were copied, their state recorded and their vertex shader run. A draw whose
 * vertices all lie past one edge of the viewport shows nothing, so it can be dropped
 * before any of that. They are small draws, so this saves about 1% of a frame.
 *
 * The idea is SCD3D11's, from its ViewportCull.cpp, under the LGPL. See NOTICE.
 */

//// Dependencies

#include "ViewportCull.h"

#include <emmintrin.h>
#include <string.h>

namespace scvk
{
	//// Constants

	namespace
	{
		// One bit for each edge of clip space a vertex can lie beyond.
		constexpr uint32_t BEYOND_LEFT   = 1;
		constexpr uint32_t BEYOND_RIGHT  = 2;
		constexpr uint32_t BEYOND_BOTTOM = 4;
		constexpr uint32_t BEYOND_TOP    = 8;
		constexpr uint32_t BEYOND_ANY    = BEYOND_LEFT | BEYOND_RIGHT | BEYOND_BOTTOM | BEYOND_TOP;

		// What _mm_movemask_ps gives when all four lanes compared true.
		constexpr int ALL_LANES = 15;

		// Vertices taken together. A run must leave at least one vertex after it, see
		// the loop.
		constexpr uint32_t VERTICES_AT_ONCE = 4;
	}

	//// Private Functions

	namespace
	{
		/** The edge's bit when all four lanes compared true, and nothing otherwise. */
		uint32_t EdgeIfAllLanes(__m128 const& comparison, uint32_t edge)
		{
			return (_mm_movemask_ps(comparison) == ALL_LANES) ? edge : 0u;
		}
	}

	//// Public API

	bool AreVerticesBeyondClipEdge(float const* transform, float limitX, float limitY, uint8_t const* positions, uint32_t stride, uint32_t count)
	{
		// Take only an orthographic transform
		//
		// Its w is the same for every vertex, so the test needs no w per vertex, and a
		// vertex can never sit behind the camera, where the sides of the test swap.
		float const w = transform[15];

		if (count == 0 || transform[3] != 0.0f || transform[7] != 0.0f || transform[11] != 0.0f || !(w > 0.0f))
		{
			return false;
		}

		float const right = limitX * w;
		float const top   = limitY * w;

		// The edges every vertex so far lies beyond. Any comparison against a NaN is
		// false, so such a vertex lies beyond nothing and keeps the draw.
		uint32_t sharedEdges = BEYOND_ANY;
		uint32_t index       = 0;

		// Test four vertices at a time
		//
		// This runs for every vertex of every draw a strip drops. A load takes 16 bytes,
		// one float more than a position, which the condition keeps in bounds: there is
		// always another vertex after the last of the four. Every load is unaligned, since
		// the vertices are client memory.
		__m128 const columnX[4] = { _mm_set1_ps(transform[0]), _mm_set1_ps(transform[4]), _mm_set1_ps(transform[8]), _mm_set1_ps(transform[12]) };
		__m128 const columnY[4] = { _mm_set1_ps(transform[1]), _mm_set1_ps(transform[5]), _mm_set1_ps(transform[9]), _mm_set1_ps(transform[13]) };
		__m128 const rightLanes = _mm_set1_ps(right);
		__m128 const topLanes   = _mm_set1_ps(top);
		__m128 const leftLanes  = _mm_set1_ps(-right);
		__m128 const lowLanes   = _mm_set1_ps(-top);

		for (; index + VERTICES_AT_ONCE < count; index += VERTICES_AT_ONCE)
		{
			uint8_t const* const first = positions + size_t{ index } * stride;

			// The intrinsic reads its unaligned source through a float pointer.
			__m128 positionX = _mm_loadu_ps(reinterpret_cast<float const*>(first));
			__m128 positionY = _mm_loadu_ps(reinterpret_cast<float const*>(first + stride));
			__m128 positionZ = _mm_loadu_ps(reinterpret_cast<float const*>(first + size_t{ stride } * 2u));
			__m128 unused    = _mm_loadu_ps(reinterpret_cast<float const*>(first + size_t{ stride } * 3u));
			_MM_TRANSPOSE4_PS(positionX, positionY, positionZ, unused);

			__m128 const x = _mm_add_ps(_mm_add_ps(_mm_add_ps(_mm_mul_ps(columnX[0], positionX), _mm_mul_ps(columnX[1], positionY)), _mm_mul_ps(columnX[2], positionZ)), columnX[3]);
			__m128 const y = _mm_add_ps(_mm_add_ps(_mm_add_ps(_mm_mul_ps(columnY[0], positionX), _mm_mul_ps(columnY[1], positionY)), _mm_mul_ps(columnY[2], positionZ)), columnY[3]);

			sharedEdges &= EdgeIfAllLanes(_mm_cmplt_ps(x, leftLanes), BEYOND_LEFT) | EdgeIfAllLanes(_mm_cmpgt_ps(x, rightLanes), BEYOND_RIGHT) | EdgeIfAllLanes(_mm_cmplt_ps(y, lowLanes), BEYOND_BOTTOM) | EdgeIfAllLanes(_mm_cmpgt_ps(y, topLanes), BEYOND_TOP);

			if (sharedEdges == 0)
			{
				return false;
			}
		}

		// Test the last few one at a time
		for (; index < count; index++)
		{
			float position[3];
			memcpy(position, positions + size_t{ index } * stride, sizeof(position));

			float const x = transform[0] * position[0] + transform[4] * position[1] + transform[8] * position[2] + transform[12];
			float const y = transform[1] * position[0] + transform[5] * position[1] + transform[9] * position[2] + transform[13];

			sharedEdges &= ((x < -right) ? BEYOND_LEFT : 0u) | ((x > right) ? BEYOND_RIGHT : 0u) | ((y < -top) ? BEYOND_BOTTOM : 0u) | ((y > top) ? BEYOND_TOP : 0u);

			if (sharedEdges == 0)
			{
				return false;
			}
		}

		return true;
	}
}
