/*
 * scvk - a native Vulkan renderer for SimCity 4
 *
 * Live shadows carried over from SCD3D11 (cGDriver_LiveShadows.cpp), Copyright (C) 2026
 * the SCD3D11 authors, under the same licence.
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
 * Live indexed shadows for True3D props and prebuilt network pieces (-LiveShadows:on), as
 * SCD3D11 draws them. The game's own shadow paths only see positions and coordinates;
 * this runs where the index buffer and the alpha texture still exist.
 *
 * The same shadow map also takes every shadow record the game's AddShadow made
 * (ShadowRegistry): buildings, flora, props and poles cast through the game's own
 * projector and prerendered texture, so their silhouettes match the game's while landing
 * on slopes and other buildings, and overlaps no longer stack.
 *
 * The same composite also darkens whatever lies in the terrain's own shadow
 * (TerrainShadows), which the game never drew.
 *
 * Without -LiveShadows:on no draw matches and nothing is captured. The drawing itself is
 * the backend's: VulkanBackend::DrawShadowCasters and CompositeShadows.
 */

//// Dependencies

#include "cVKDriver.h"
#include "LiveShadowHooks.h"
#include "Logger.h"
#include "ModuleLog.h"
#include "ShadowRegistry.h"
#include "TerrainShadows.h"
#include "VulkanBackend.h"

#include <VertexFormatUtils.h>

#include <windows.h>
#include <algorithm>
#include <array>
#include <cfloat>
#include <cmath>
#include <cstring>
#include <new>

namespace scvk
{
	//// Constants

	namespace
	{
		// The game's point and line primitives, in its own numbering.
		constexpr uint32_t GD_PRIMITIVE_POINTS     = 3;
		constexpr uint32_t GD_PRIMITIVE_LINES      = 4;
		constexpr uint32_t GD_PRIMITIVE_LINE_STRIP = 5;

		constexpr float SHADOW_MAP_SIZE = 2048.0f;
		constexpr float SHADOW_DEPTH_BIAS_WORLD = 0.05f;

		// The black the casters composite with until the game's own shadow parameters have
		// been read
		constexpr float FALLBACK_SHADOW_OPACITY = 0.38f;

		// A registry caster's ground quad sits this far above the occupant's placement
		// height, so the terrain under it is behind it by more than the composite bias
		// along the 45 degree sun.
		constexpr float GROUND_LIFT_WORLD = 0.2f;

		// Used when the game reports no alpha test scale, and for True3D props cast
		// through their own coordinates.
		constexpr float DEFAULT_REGISTRY_ALPHA_REFERENCE = 0.5f;

		// How far below a captured caster its shadow is assumed to fall when the terrain
		// altitude under it is unknown; only sizes redisplayed regions.
		constexpr float UNKNOWN_GROUND_DROP = 32.0f;

		// Two captures are the same caster when their shape matches and their world boxes
		// agree this closely.
		constexpr float SAME_CASTER_TOLERANCE = 0.05f;

		// Network models used as textured road surfaces sit just above the terrain. Their
		// self-shadow only darkens the surface, while an elevated deck must still cast, so
		// every vertex is checked against the terrain and slopes do not make a piece lying
		// on the ground look raised.
		constexpr float GROUND_NETWORK_CASTER_CLEARANCE = 0.5f;

		// A receiver starts to darken this far below the terrain's shadow ceiling, and its
		// penumbra, the gap down to the umbra, is never narrower than this, so contact edges
		// stay smooth
		constexpr float TERRAIN_SHADOW_BIAS     = 0.5f;
		constexpr float TERRAIN_SHADOW_SOFTNESS = 3.0f;

		// A terrain draw's vertex lies this close to the height field when its model space
		// is world space.
		constexpr float TERRAIN_VERTEX_TOLERANCE = 0.05f;

		// A pixel this close to the drawn terrain surface is terrain.
		constexpr float TERRAIN_PIXEL_TOLERANCE = 1.0f;

		// The floats of a captured vertex: position, then texture coordinate.
		constexpr size_t VERTEX_FLOATS = 5;

		// The game's filter and wrap numbering: linear, then clamp or repeat.
		constexpr uint32_t CLAMP_SAMPLER[4] = { 1, 1, 2, 2 };
		constexpr uint32_t WRAP_SAMPLER[4]  = { 1, 1, 3, 3 };
	}

	//// Private Functions

	namespace
	{
		void TransformPoint(float const* matrix, float const point[3], float out[3])
		{
			out[0] = matrix[0] * point[0] + matrix[4] * point[1] + matrix[8] * point[2] + matrix[12];
			out[1] = matrix[1] * point[0] + matrix[5] * point[1] + matrix[9] * point[2] + matrix[13];
			out[2] = matrix[2] * point[0] + matrix[6] * point[1] + matrix[10] * point[2] + matrix[14];
		}

		float Dot(float const a[3], float const b[3])
		{
			return a[0] * b[0] + a[1] * b[1] + a[2] * b[2];
		}

		void Normalize(float vector[3])
		{
			float const length = std::sqrt(Dot(vector, vector));
			if (length > 1.0e-6f)
			{
				vector[0] /= length;
				vector[1] /= length;
				vector[2] /= length;
			}
		}

		void Cross(float const a[3], float const b[3], float out[3])
		{
			out[0] = a[1] * b[2] - a[2] * b[1];
			out[1] = a[2] * b[0] - a[0] * b[2];
			out[2] = a[0] * b[1] - a[1] * b[0];
		}

		/** An orthographic light matrix around a view-space box, looking along the sun. Returns its depth range. */
		float BuildLightMatrix(float const minimum[3], float const maximum[3], float const shadowDirection[3], float matrix[16])
		{
			float forward[3] = { shadowDirection[0], shadowDirection[1], shadowDirection[2] };
			Normalize(forward);

			float const helper[3] = { (std::fabs(forward[1]) < 0.95f) ? 0.0f : 1.0f, (std::fabs(forward[1]) < 0.95f) ? 1.0f : 0.0f, 0.0f };

			float right[3] = {};
			Cross(helper, forward, right);
			Normalize(right);

			float up[3] = {};
			Cross(forward, right, up);
			Normalize(up);

			float low[3]  = { FLT_MAX, FLT_MAX, FLT_MAX };
			float high[3] = { -FLT_MAX, -FLT_MAX, -FLT_MAX };

			for (unsigned mask = 0; mask < 8; mask++)
			{
				float const point[3] = { (mask & 1) ? maximum[0] : minimum[0], (mask & 2) ? maximum[1] : minimum[1], (mask & 4) ? maximum[2] : minimum[2] };
				float const values[3] = { Dot(right, point), Dot(up, point), Dot(forward, point) };

				for (unsigned axis = 0; axis < 3; axis++)
				{
					low[axis]  = std::min(low[axis], values[axis]);
					high[axis] = std::max(high[axis], values[axis]);
				}
			}

			// The footprint bounds x and y; depth leaves ample room for receivers, since the
			// scene depth decides where shadows land
			low[0]  -= 1.0f;
			high[0] += 1.0f;
			low[1]  -= 1.0f;
			high[1] += 1.0f;
			low[2]  -= 4096.0f;
			high[2] += 4096.0f;

			std::memset(matrix, 0, sizeof(float) * 16);

			float const scales[3] = { 2.0f / (high[0] - low[0]), 2.0f / (high[1] - low[1]), 1.0f / (high[2] - low[2]) };
			for (unsigned column = 0; column < 3; column++)
			{
				matrix[column * 4 + 0] = right[column] * scales[0];
				matrix[column * 4 + 1] = up[column] * scales[1];
				matrix[column * 4 + 2] = forward[column] * scales[2];
			}

			matrix[12] = -(high[0] + low[0]) / (high[0] - low[0]);
			matrix[13] = -(high[1] + low[1]) / (high[1] - low[1]);
			matrix[14] = -low[2] / (high[2] - low[2]);
			matrix[15] = 1.0f;
			return high[2] - low[2];
		}

		void Multiply(float const* left, float const* right, float* out)
		{
			for (unsigned column = 0; column < 4; column++)
			{
				for (unsigned row = 0; row < 4; row++)
				{
					float sum = 0.0f;
					for (unsigned k = 0; k < 4; k++)
					{
						sum += left[k * 4 + row] * right[column * 4 + k];
					}

					out[column * 4 + row] = sum;
				}
			}
		}

		void TransformDirection(float const* matrix, float const direction[3], float out[3])
		{
			for (unsigned row = 0; row < 3; row++)
			{
				out[row] = matrix[row] * direction[0] + matrix[4 + row] * direction[1] + matrix[8 + row] * direction[2];
			}
		}

		/** A general 4x4 inverse by cofactors. */
		bool Invert(float const* m, float* out)
		{
			double inverse[16];
			inverse[0]  =  double{ m[5] } * m[10] * m[15] - double{ m[5] } * m[11] * m[14] - double{ m[9] } * m[6] * m[15] + double{ m[9] } * m[7] * m[14] + double{ m[13] } * m[6] * m[11] - double{ m[13] } * m[7] * m[10];
			inverse[4]  = -double{ m[4] } * m[10] * m[15] + double{ m[4] } * m[11] * m[14] + double{ m[8] } * m[6] * m[15] - double{ m[8] } * m[7] * m[14] - double{ m[12] } * m[6] * m[11] + double{ m[12] } * m[7] * m[10];
			inverse[8]  =  double{ m[4] } * m[9] * m[15] - double{ m[4] } * m[11] * m[13] - double{ m[8] } * m[5] * m[15] + double{ m[8] } * m[7] * m[13] + double{ m[12] } * m[5] * m[11] - double{ m[12] } * m[7] * m[9];
			inverse[12] = -double{ m[4] } * m[9] * m[14] + double{ m[4] } * m[10] * m[13] + double{ m[8] } * m[5] * m[14] - double{ m[8] } * m[6] * m[13] - double{ m[12] } * m[5] * m[10] + double{ m[12] } * m[6] * m[9];
			inverse[1]  = -double{ m[1] } * m[10] * m[15] + double{ m[1] } * m[11] * m[14] + double{ m[9] } * m[2] * m[15] - double{ m[9] } * m[3] * m[14] - double{ m[13] } * m[2] * m[11] + double{ m[13] } * m[3] * m[10];
			inverse[5]  =  double{ m[0] } * m[10] * m[15] - double{ m[0] } * m[11] * m[14] - double{ m[8] } * m[2] * m[15] + double{ m[8] } * m[3] * m[14] + double{ m[12] } * m[2] * m[11] - double{ m[12] } * m[3] * m[10];
			inverse[9]  = -double{ m[0] } * m[9] * m[15] + double{ m[0] } * m[11] * m[13] + double{ m[8] } * m[1] * m[15] - double{ m[8] } * m[3] * m[13] - double{ m[12] } * m[1] * m[11] + double{ m[12] } * m[3] * m[9];
			inverse[13] =  double{ m[0] } * m[9] * m[14] - double{ m[0] } * m[10] * m[13] - double{ m[8] } * m[1] * m[14] + double{ m[8] } * m[2] * m[13] + double{ m[12] } * m[1] * m[10] - double{ m[12] } * m[2] * m[9];
			inverse[2]  =  double{ m[1] } * m[6] * m[15] - double{ m[1] } * m[7] * m[14] - double{ m[5] } * m[2] * m[15] + double{ m[5] } * m[3] * m[14] + double{ m[13] } * m[2] * m[7] - double{ m[13] } * m[3] * m[6];
			inverse[6]  = -double{ m[0] } * m[6] * m[15] + double{ m[0] } * m[7] * m[14] + double{ m[4] } * m[2] * m[15] - double{ m[4] } * m[3] * m[14] - double{ m[12] } * m[2] * m[7] + double{ m[12] } * m[3] * m[6];
			inverse[10] =  double{ m[0] } * m[5] * m[15] - double{ m[0] } * m[7] * m[13] - double{ m[4] } * m[1] * m[15] + double{ m[4] } * m[3] * m[13] + double{ m[12] } * m[1] * m[7] - double{ m[12] } * m[3] * m[5];
			inverse[14] = -double{ m[0] } * m[5] * m[14] + double{ m[0] } * m[6] * m[13] + double{ m[4] } * m[1] * m[14] - double{ m[4] } * m[2] * m[13] - double{ m[12] } * m[1] * m[6] + double{ m[12] } * m[2] * m[5];
			inverse[3]  = -double{ m[1] } * m[6] * m[11] + double{ m[1] } * m[7] * m[10] + double{ m[5] } * m[2] * m[11] - double{ m[5] } * m[3] * m[10] - double{ m[9] } * m[2] * m[7] + double{ m[9] } * m[3] * m[6];
			inverse[7]  =  double{ m[0] } * m[6] * m[11] - double{ m[0] } * m[7] * m[10] - double{ m[4] } * m[2] * m[11] + double{ m[4] } * m[3] * m[10] + double{ m[8] } * m[2] * m[7] - double{ m[8] } * m[3] * m[6];
			inverse[11] = -double{ m[0] } * m[5] * m[11] + double{ m[0] } * m[7] * m[9] + double{ m[4] } * m[1] * m[11] - double{ m[4] } * m[3] * m[9] - double{ m[8] } * m[1] * m[7] + double{ m[8] } * m[3] * m[5];
			inverse[15] =  double{ m[0] } * m[5] * m[10] - double{ m[0] } * m[6] * m[9] - double{ m[4] } * m[1] * m[10] + double{ m[4] } * m[2] * m[9] + double{ m[8] } * m[1] * m[6] - double{ m[8] } * m[2] * m[5];

			double const determinant = double{ m[0] } * inverse[0] + double{ m[1] } * inverse[4] + double{ m[2] } * inverse[8] + double{ m[3] } * inverse[12];
			if (!std::isfinite(determinant) || std::fabs(determinant) < 1.0e-12)
			{
				return false;
			}

			for (unsigned index = 0; index < 16; index++)
			{
				out[index] = static_cast<float>(inverse[index] / determinant);
			}

			return true;
		}

		enum class ViewportOverlap
		{
			Outside,
			Partial,
			Inside,
		};

		/** Where a world-space box lands in a pass's NDC square, which is the pass's own viewport. */
		ViewportOverlap BoxInViewport(float const low[3], float const high[3], float const* worldToClip)
		{
			float ndcLow[2]  = { FLT_MAX, FLT_MAX };
			float ndcHigh[2] = { -FLT_MAX, -FLT_MAX };

			for (unsigned mask = 0; mask < 8; mask++)
			{
				float const corner[3] = { (mask & 1) ? high[0] : low[0], (mask & 2) ? high[1] : low[1], (mask & 4) ? high[2] : low[2] };

				for (unsigned axis = 0; axis < 2; axis++)
				{
					float const ndc = worldToClip[axis] * corner[0] + worldToClip[4 + axis] * corner[1] + worldToClip[8 + axis] * corner[2] + worldToClip[12 + axis];
					ndcLow[axis]  = std::min(ndcLow[axis], ndc);
					ndcHigh[axis] = std::max(ndcHigh[axis], ndc);
				}
			}

			if (ndcHigh[0] < -1.0f || ndcLow[0] > 1.0f || ndcHigh[1] < -1.0f || ndcLow[1] > 1.0f)
			{
				return ViewportOverlap::Outside;
			}

			if (ndcLow[0] >= -1.0f && ndcHigh[0] <= 1.0f && ndcLow[1] >= -1.0f && ndcHigh[1] <= 1.0f)
			{
				return ViewportOverlap::Inside;
			}

			return ViewportOverlap::Partial;
		}

		/**
		 * The ground quad a registry caster adds for silhouettes its mesh does not cover
		 * from the sun: the caster's box pushed down the sun onto the plane at its
		 * placement height.
		 */
		bool GroundQuad(ShadowRegistry::Caster const& caster, float const sun[3], float quad[4][3])
		{
			if (sun[1] > -1.0e-3f)
			{
				return false;
			}

			float const height = caster.baseHeight + GROUND_LIFT_WORLD;
			float low[2]  = { FLT_MAX, FLT_MAX };
			float high[2] = { -FLT_MAX, -FLT_MAX };

			for (unsigned mask = 0; mask < 8; mask++)
			{
				float const corner[3] = { (mask & 1) ? caster.high[0] : caster.low[0], (mask & 2) ? caster.high[1] : caster.low[1], (mask & 4) ? caster.high[2] : caster.low[2] };
				float const along = (height - corner[1]) / sun[1];
				float const x = corner[0] + sun[0] * along;
				float const z = corner[2] + sun[2] * along;

				low[0]  = std::min(low[0], std::min(x, corner[0]));
				low[1]  = std::min(low[1], std::min(z, corner[2]));
				high[0] = std::max(high[0], std::max(x, corner[0]));
				high[1] = std::max(high[1], std::max(z, corner[2]));
			}

			float const corners[4][2] = { { low[0], low[1] }, { high[0], low[1] }, { high[0], high[1] }, { low[0], high[1] } };
			for (unsigned index = 0; index < 4; index++)
			{
				quad[index][0] = corners[index][0];
				quad[index][1] = height;
				quad[index][2] = corners[index][1];
			}

			return true;
		}

		/** The two rows of a column-major texture matrix a 2D sample reads. */
		void TextureRows(float const* matrix, float* constants)
		{
			float const rows[8] = { matrix[0], matrix[4], matrix[8], matrix[12], matrix[1], matrix[5], matrix[9], matrix[13] };
			std::memcpy(constants, rows, sizeof(rows));
		}

		/** A registry caster that survived culling, and where its geometry lands in the upload. */
		struct RegistryDraw
		{
			ShadowRegistry::PassCaster const* caster = nullptr;
			float    quad[4][3]  = {};
			bool     hasQuad     = false;
			bool     isQuadDrawn = false;
		};
	}

	namespace
	{
		// The game's blend factor for one, as BlendFunc takes it
		constexpr uint32_t BLEND_FACTOR_ONE = 1;
	}

	bool cVKDriver::MatchesLiveShadowMesh(uint8_t const* vertices, uint32_t vertexCount)
	{
		// The game-side prebuilt network hook brackets exactly the occupant renderer that
		// matters, so it is both cheaper and more precise than hashing the meshes
		if (LiveShadowHooks::NetworkDrawActive())
		{
			return true;
		}

		uint32_t const coordinateSets = RZVertexFormatNumElements(vertexFormat, kGDElementType_TexCoord);
		if (!LiveShadowHooks::HasPropMeshes() || vertices == nullptr || vertexCount == 0 || vertexStride < sizeof(float) * 3 || coordinateSets == 0)
		{
			return false;
		}

		// Hash the positions and first coordinates, as the props were registered
		uint32_t const coordinateOffset = RZVertexFormatElementOffset(vertexFormat, kGDElementType_TexCoord, 0);
		uint64_t signature = 0xCBF29CE484222325ull;
		auto const hashWord = [&](uint32_t word) { signature = (signature ^ word) * 0x100000001B3ull; };

		hashWord(vertexCount);
		for (uint32_t index = 0; index < vertexCount; index++)
		{
			uint8_t const* const vertex = vertices + size_t{ index } * vertexStride;

			uint32_t words[5] = {};
			std::memcpy(words, vertex, sizeof(float) * 3);
			std::memcpy(words + 3, vertex + coordinateOffset, sizeof(float) * 2);

			for (uint32_t word : words)
			{
				hashWord(word);
			}
		}

		return LiveShadowHooks::MatchPropSignature(signature);
	}

	void cVKDriver::NoteLiveShadowCandidate(uint32_t gdPrimitiveType, uint8_t const* firstVertex, uint32_t vertexCount, void const* indices, uint32_t indexCount, bool isIndex32Bit, uint32_t indexBase)
	{
		// Nothing can match unless a shadow module is installed, which keeps every draw
		// path at the cost of two loads when the shadows are off
		if (!LiveShadowHooks::NetworkDrawActive() && !LiveShadowHooks::HasPropMeshes())
		{
			return;
		}

		if (firstVertex == nullptr || vertexCount == 0 || !MatchesLiveShadowMesh(firstVertex, vertexCount))
		{
			return;
		}

		// Leave out light and glow
		//
		// A True3D piece's street lamp is drawn in the same bracket as the road, and so is
		// the disc of light under it: blended over the scene, added (destination factor
		// one) or without writing depth. In the shadow map that disc is solid, and every
		// lamp along a highway cast a large oval shadow onto the road. Opaque geometry
		// and alpha-blended leaves that still write depth keep casting.
		if (isCapabilityEnabled[kGDCapability_Blend] && (blendDestinationFactor == BLEND_FACTOR_ONE || !isDepthWriteEnabled))
		{
			if (liveShadowLightsLeftOut++ == 0)
			{
				Log(LogCategory::Initialization, "live shadows: first light or glow draw left out of the shadow map (blend %u/%u, depth write %d)", blendSourceFactor, blendDestinationFactor, isDepthWriteEnabled ? 1 : 0);
			}

			return;
		}

		try
		{
			// Gather the indices relative to the first vertex, then turn them into triangles
			std::vector<uint32_t> source;
			if (indices == nullptr)
			{
				source.resize(vertexCount);
				for (uint32_t index = 0; index < vertexCount; index++)
				{
					source[index] = index;
				}
			}
			else
			{
				source.resize(indexCount);
				for (uint32_t index = 0; index < indexCount; index++)
				{
					// The game passes its indices untyped, in 16 or 32 bits as the draw says
					uint32_t const value = isIndex32Bit ? static_cast<uint32_t const*>(indices)[index] : static_cast<uint16_t const*>(indices)[index];
					source[index] = value - indexBase;
				}
			}

			// Points and lines cast too, as SCD3D11 draws a caster with the topology the
			// game gave it: points as they are, lines and line strips as a list of lines.
			// Everything else becomes triangles.
			std::vector<uint32_t> primitives;
			uint32_t              topology = SHADOW_CASTER_TRIANGLES;

			switch (gdPrimitiveType)
			{
			case GD_PRIMITIVE_POINTS:
				topology   = SHADOW_CASTER_POINTS;
				primitives = source;
				break;

			case GD_PRIMITIVE_LINES:
				topology = SHADOW_CASTER_LINES;
				primitives.assign(source.begin(), source.begin() + static_cast<ptrdiff_t>(source.size() & ~size_t{ 1 }));
				break;

			case GD_PRIMITIVE_LINE_STRIP:
				topology = SHADOW_CASTER_LINES;
				for (size_t index = 1; index < source.size(); index++)
				{
					primitives.push_back(source[index - 1]);
					primitives.push_back(source[index]);
				}
				break;

			default:
				if (!ShadowRegistry::AppendTriangles(gdPrimitiveType, source.data(), static_cast<uint32_t>(source.size()), primitives))
				{
					return;
				}
				break;
			}

			if (primitives.empty())
			{
				return;
			}

			CaptureLiveShadowDraw(firstVertex, vertexCount, primitives, topology);
		}
		catch (std::bad_alloc const&)
		{
		}
	}

	void cVKDriver::CaptureLiveShadowDraw(uint8_t const* vertices, uint32_t vertexCount, std::vector<uint32_t> const& primitives, uint32_t topology)
	{
		LiveShadowDraw draw;

		// Keep the positions and the first coordinate set, all a caster needs
		bool const hasCoordinates = RZVertexFormatNumElements(vertexFormat, kGDElementType_TexCoord) != 0;
		uint32_t const coordinateOffset = hasCoordinates ? RZVertexFormatElementOffset(vertexFormat, kGDElementType_TexCoord, 0) : 0;

		draw.vertices.resize(size_t{ vertexCount } * VERTEX_FLOATS);
		for (uint32_t index = 0; index < vertexCount; index++)
		{
			uint8_t const* const vertex = vertices + size_t{ index } * vertexStride;
			float* const out = draw.vertices.data() + size_t{ index } * VERTEX_FLOATS;

			std::memcpy(out, vertex, sizeof(float) * 3);
			if (hasCoordinates)
			{
				std::memcpy(out + 3, vertex + coordinateOffset, sizeof(float) * 2);
			}
		}

		draw.indices  = primitives;
		draw.topology = topology;

		// Keep the texture by handle and serial, so a later frame can tell it still names
		// the same picture. Untextured geometry casts a solid silhouette.
		if (isTextureStageEnabled[0] && vulkan->TextureSerial(boundTexture) != 0)
		{
			draw.texture       = boundTexture;
			draw.textureSerial = vulkan->TextureSerial(boundTexture);
			vulkan->GetStageParameters(0, draw.samplerParameters);
		}

		std::memcpy(draw.modelView, modelViewMatrix, sizeof(draw.modelView));
		std::memcpy(draw.projection, projectionMatrix, sizeof(draw.projection));
		std::memcpy(draw.textureMatrix, textureStageMatrices[0], sizeof(draw.textureMatrix));
		draw.alphaFunction  = alphaComparison;
		draw.alphaReference = alphaReference;
		draw.isAlphaTested  = isCapabilityEnabled[kGDCapability_AlphaTest];
		draw.isNetwork      = LiveShadowHooks::NetworkDrawActive();

		size_t const loggedVertices = vertexCount;
		size_t const loggedIndices  = draw.indices.size();
		bool const   isNetwork      = draw.isNetwork;
		liveShadowDraws.push_back(std::move(draw));

		static bool hasLoggedProp    = false;
		static bool hasLoggedNetwork = false;
		bool& hasLogged = isNetwork ? hasLoggedNetwork : hasLoggedProp;

		if (!hasLogged)
		{
			hasLogged = true;
			Log(LogCategory::Initialization, "live shadows: matched first %s caster (%u vertices, %u indices)", isNetwork ? "prebuilt network" : "prop", static_cast<unsigned>(loggedVertices), static_cast<unsigned>(loggedIndices));
		}
	}

	void cVKDriver::NoteLiveShadowTerrainView(uint8_t const* firstVertex)
	{
		if (isLiveShadowTerrainViewValid || !LiveShadowHooks::Enabled() || firstVertex == nullptr)
		{
			return;
		}

		// Called with a vertex of a terrain draw, whose format leads with the position
		std::memcpy(liveShadowTerrainView, modelViewMatrix, sizeof(liveShadowTerrainView));
		std::memcpy(liveShadowTerrainVertex, firstVertex, sizeof(liveShadowTerrainVertex));
		isLiveShadowTerrainViewValid = true;
	}

	bool cVKDriver::ConsumeLiveShadowCleanRedraw(void)
	{
		bool const isPending = isLiveShadowCleanRedrawPending;
		isLiveShadowCleanRedrawPending = false;
		return isPending;
	}

	bool cVKDriver::TrackLiveShadowWorldCasters(float const* eyeToWorld, float const* worldToView, float const* projection, float const sun[3], bool isSunKnown, bool isPartialPass)
	{
		// With the registry, live shadows keep the captured casters of every static pass in
		// world space. A full pass replaces the set. A partial pass updates it: a caster
		// drawn again replaces its entry, one that should have been redrawn but was not is
		// gone, and one not seen before is new. The shadow of a caster that went, or of a
		// new one reaching past the rectangle, lands on pixels the game did not repaint,
		// so that region is redisplayed.
		uint64_t const serial = ++liveShadowPassSerial;

		auto const placeInWorld = [&](LiveShadowDraw& draw)
		{
			Multiply(eyeToWorld, draw.modelView, draw.modelToWorld);

			for (unsigned axis = 0; axis < 3; axis++)
			{
				draw.worldLow[axis]  = FLT_MAX;
				draw.worldHigh[axis] = -FLT_MAX;
			}

			for (size_t index = 0; index < draw.vertices.size(); index += VERTEX_FLOATS)
			{
				float world[3] = {};
				TransformPoint(draw.modelToWorld, draw.vertices.data() + index, world);

				for (unsigned axis = 0; axis < 3; axis++)
				{
					draw.worldLow[axis]  = std::min(draw.worldLow[axis], world[axis]);
					draw.worldHigh[axis] = std::max(draw.worldHigh[axis], world[axis]);
				}
			}

			uint64_t shape = 0xCBF29CE484222325ull;
			auto const mix = [&](uint64_t value) { shape = (shape ^ value) * 0x100000001B3ull; };

			mix(draw.texture);
			mix(draw.textureSerial);
			mix(draw.vertices.size());
			mix(draw.indices.size());
			mix(draw.topology);

			if (!draw.vertices.empty())
			{
				uint32_t words[3] = {};
				std::memcpy(words, draw.vertices.data(), sizeof(words));
				for (uint32_t word : words)
				{
					mix(word);
				}
			}

			draw.shape    = shape;
			draw.seenPass = serial;
		};

		// The box the caster's shadow can reach: its own box pushed down the sun to the
		// terrain under it
		auto const placeShadow = [&](LiveShadowDraw& draw)
		{
			float ground   = draw.worldLow[1] - UNKNOWN_GROUND_DROP;
			float altitude = 0.0f;

			if (TerrainShadows::Altitude((draw.worldLow[0] + draw.worldHigh[0]) * 0.5f, (draw.worldLow[2] + draw.worldHigh[2]) * 0.5f, altitude))
			{
				ground = std::min(altitude, draw.worldLow[1]);
			}

			std::memcpy(draw.shadowLow, draw.worldLow, sizeof(draw.shadowLow));
			std::memcpy(draw.shadowHigh, draw.worldHigh, sizeof(draw.shadowHigh));
			draw.shadowLow[1] = ground;

			if (!isSunKnown || sun[1] > -1.0e-3f)
			{
				float const reach = draw.worldHigh[1] - ground;
				for (unsigned axis : { 0u, 2u })
				{
					draw.shadowLow[axis]  -= reach;
					draw.shadowHigh[axis] += reach;
				}

				return;
			}

			for (unsigned mask = 0; mask < 8; mask++)
			{
				float const corner[3] = { (mask & 1) ? draw.worldHigh[0] : draw.worldLow[0], (mask & 2) ? draw.worldHigh[1] : draw.worldLow[1], (mask & 4) ? draw.worldHigh[2] : draw.worldLow[2] };
				float const along = (ground - corner[1]) / sun[1];

				for (unsigned axis : { 0u, 2u })
				{
					float const landed = corner[axis] + sun[axis] * along;
					draw.shadowLow[axis]  = std::min(draw.shadowLow[axis], landed);
					draw.shadowHigh[axis] = std::max(draw.shadowHigh[axis], landed);
				}
			}
		};

		for (LiveShadowDraw& draw : liveShadowDraws)
		{
			placeInWorld(draw);
		}

		if (!isPartialPass)
		{
			for (LiveShadowDraw& draw : liveShadowDraws)
			{
				placeShadow(draw);
			}

			liveShadowWorldCasters = std::move(liveShadowDraws);
			liveShadowDraws.clear();
			isLiveShadowWorldValid = true;
			return true;
		}

		if (!isLiveShadowWorldValid)
		{
			return false;
		}

		float worldToClip[16] = {};
		Multiply(projection, worldToView, worldToClip);

		std::vector<std::array<float, 4>> redisplay;
		auto const queueRedisplay = [&](LiveShadowDraw const& draw)
		{
			redisplay.push_back({ draw.shadowLow[0], draw.shadowLow[2], draw.shadowHigh[0], draw.shadowHigh[2] });
		};

		size_t const known = liveShadowWorldCasters.size();

		for (LiveShadowDraw& draw : liveShadowDraws)
		{
			auto const isSame = [&](LiveShadowDraw const& cached)
			{
				if (cached.shape != draw.shape)
				{
					return false;
				}

				for (unsigned axis = 0; axis < 3; axis++)
				{
					if (std::fabs(cached.worldLow[axis] - draw.worldLow[axis]) > SAME_CASTER_TOLERANCE || std::fabs(cached.worldHigh[axis] - draw.worldHigh[axis]) > SAME_CASTER_TOLERANCE)
					{
						return false;
					}
				}

				return true;
			};

			auto const cachedEnd = liveShadowWorldCasters.begin() + static_cast<ptrdiff_t>(known);
			auto const match     = std::find_if(liveShadowWorldCasters.begin(), cachedEnd, isSame);

			if (match != cachedEnd)
			{
				std::memcpy(draw.shadowLow, match->shadowLow, sizeof(draw.shadowLow));
				std::memcpy(draw.shadowHigh, match->shadowHigh, sizeof(draw.shadowHigh));
				*match = std::move(draw);
				continue;
			}

			placeShadow(draw);
			if (BoxInViewport(draw.shadowLow, draw.shadowHigh, worldToClip) != ViewportOverlap::Inside)
			{
				queueRedisplay(draw);
			}

			liveShadowWorldCasters.push_back(std::move(draw));
		}

		liveShadowDraws.clear();

		auto const isGone = [&](LiveShadowDraw const& cached)
		{
			if (cached.seenPass == serial || BoxInViewport(cached.worldLow, cached.worldHigh, worldToClip) == ViewportOverlap::Outside)
			{
				return false;
			}

			queueRedisplay(cached);
			return true;
		};

		liveShadowWorldCasters.erase(std::remove_if(liveShadowWorldCasters.begin(), liveShadowWorldCasters.end(), isGone), liveShadowWorldCasters.end());

		for (auto const& box : redisplay)
		{
			ShadowRegistry::RedisplayWorldRect(box[0], box[1], box[2], box[3]);
		}

		return true;
	}

	void cVKDriver::CaptureShadowUniforms(void)
	{
		// From the orthographic projection of the last scene draw. Column major:
		// p[5] = 2 / (top - bottom) and p[10] = -2 / (far - near).
		float const* const p = projectionMatrix;

		bool const isOrthographic = p[3] == 0.0f && p[7] == 0.0f && p[11] == 0.0f;
		if (!isOrthographic || std::fabs(p[5]) < 1e-12f || std::fabs(p[10]) < 1e-12f)
		{
			areShadowUniformsValid = false;
			return;
		}

		std::memcpy(liveShadowSceneProjection, p, sizeof(liveShadowSceneProjection));
		isLiveShadowSceneProjectionValid = true;

		// The 0 to 1 depth range spans far - near = 2 / |p[10]| world units, and the
		// viewport height 2 / |p[5]|
		shadowDepthScale       = 2.0f / std::fabs(p[10]);
		shadowWorldPerScreen   = 2.0f / std::fabs(p[5]);
		areShadowUniformsValid = true;
	}

	void cVKDriver::RenderLivePropShadows(bool isStaticPass)
	{
		// Without live shadows nothing is ever captured, so there is nothing to draw
		if (!LiveShadowHooks::Enabled())
		{
			return;
		}

		// The game updates the static view in place for localised changes, handing over the
		// dirty rectangle as a scissored viewport. A shadow is not local: casters inside
		// throw onto receivers outside, and receivers inside are darkened by casters that
		// were never redrawn. ShadowRegistry's casters arrive complete for the rectangle and
		// the game dirties a record's rectangle when it changes, and
		// TrackLiveShadowWorldCasters does the same for captured casters. A partial pass that
		// cannot be followed, or one whose terrain shadows changed, composites what it
		// honestly has, clipped to the rectangle, and asks for a clean rebuild. A pass over
		// the whole window is never partial, so this cannot drive a redraw loop.
		bool const isPartialPass = IsSubViewport();
		bool const isRegistryOn  = ShadowRegistry::Enabled();

		// What the game's own DrawShadows calls in this pass gathered. Without them the tone
		// and the sun are asked for directly, so a rotation cannot leave a stale sun behind.
		ShadowRegistry::Pass pass;
		bool const hasPass = ShadowRegistry::TakePass(pass);

		if (hasPass)
		{
			std::memcpy(liveShadowColour, pass.colour, sizeof(liveShadowColour));
			liveShadowStrength    = pass.strength;
			isLiveShadowToneValid = true;
		}
		else if (isStaticPass && LiveShadowHooks::ShadowParams(liveShadowColour, liveShadowStrength))
		{
			isLiveShadowToneValid = true;
		}

		if (hasPass && pass.isSunValid)
		{
			std::memcpy(liveShadowSunWorld, pass.sunDirection, sizeof(liveShadowSunWorld));
			isLiveShadowSunWorldValid = true;
		}
		else if (isStaticPass && LiveShadowHooks::SunDirection(liveShadowSunWorld))
		{
			isLiveShadowSunWorldValid = true;
		}

		// The terrain's shadow ceiling, rebuilt only when the height field or the sun moved.
		// The height field is read with -TerrainShadows:off too, since it places the
		// captured casters.
		TerrainShadowMap::Map const* terrainMap = nullptr;
		uint64_t terrainGeneration = 0;

		if (isStaticPass && isLiveShadowSunWorldValid && TerrainShadows::ShadowsRendered())
		{
			try
			{
				terrainMap = TerrainShadows::Update(liveShadowSunWorld, terrainGeneration);
			}
			catch (std::bad_alloc const&)
			{
				terrainMap = nullptr;
			}
		}

		if (!TerrainShadows::Enabled())
		{
			terrainMap = nullptr;
		}

		// A partial pass cannot repaint the rest of the view, so a terrain map other than the
		// one the backing store was shaded with asks for a clean rebuild
		bool const isTerrainChanged = terrainMap != nullptr && terrainGeneration != liveShadowShadedTerrainGeneration;
		if (terrainMap != nullptr)
		{
			liveShadowShadedTerrainGeneration = terrainGeneration;
		}

		// DrawShadows' first argument maps eye space to world for the game's texgen, so its
		// inverse is the view of every draw in this pass. A pass without shadow records takes
		// the modelview of its first terrain draw instead: terrain vertices are world
		// coordinates, which that draw's first vertex lying on the city's height field
		// confirms.
		float worldToView[16] = {};
		float eyeToWorld[16]  = {};
		bool hasView = hasPass && Invert(pass.eyeToWorld, worldToView);

		if (hasView)
		{
			std::memcpy(eyeToWorld, pass.eyeToWorld, sizeof(eyeToWorld));
		}
		else if (isStaticPass && isLiveShadowTerrainViewValid)
		{
			float altitude = 0.0f;
			bool const isOnTerrain = TerrainShadows::Altitude(liveShadowTerrainVertex[0], liveShadowTerrainVertex[2], altitude) && std::fabs(altitude - liveShadowTerrainVertex[1]) <= TERRAIN_VERTEX_TOLERANCE;

			static bool hasLoggedVertexCheck = false;
			if (!hasLoggedVertexCheck)
			{
				hasLoggedVertexCheck = true;
				Log(LogCategory::Initialization, "live shadows: terrain vertex %.2f/%.2f/%.2f %s the height field (%.3f there)", static_cast<double>(liveShadowTerrainVertex[0]), static_cast<double>(liveShadowTerrainVertex[1]), static_cast<double>(liveShadowTerrainVertex[2]), isOnTerrain ? "lies on" : "is off", static_cast<double>(altitude));
			}

			if (isOnTerrain && Invert(liveShadowTerrainView, eyeToWorld))
			{
				std::memcpy(worldToView, liveShadowTerrainView, sizeof(worldToView));
				hasView = true;
			}
		}

		// The next pass, or the next frame after a scroll, has its own view
		isLiveShadowTerrainViewValid = false;

		// Drop network pieces lying on the terrain, which only darken the road surface
		//
		// Where the terrain under a vertex is unknown, the piece keeps casting.
		if (hasView)
		{
			auto const isOnGround = [&](LiveShadowDraw const& draw)
			{
				if (!draw.isNetwork)
				{
					return false;
				}

				float modelToWorld[16] = {};
				Multiply(eyeToWorld, draw.modelView, modelToWorld);

				for (size_t index = 0; index < draw.vertices.size(); index += VERTEX_FLOATS)
				{
					float world[3] = {};
					TransformPoint(modelToWorld, draw.vertices.data() + index, world);

					float terrain = 0.0f;
					if (!TerrainShadows::Altitude(world[0], world[2], terrain) || world[1] - terrain > GROUND_NETWORK_CASTER_CLEARANCE)
					{
						return false;
					}
				}

				return true;
			};

			liveShadowDraws.erase(std::remove_if(liveShadowDraws.begin(), liveShadowDraws.end(), isOnGround), liveShadowDraws.end());
		}

		// The composite rebuilds view positions through the pass's own projection
		float projection[16] = {};
		bool const hasProjection = !liveShadowDraws.empty() || isLiveShadowSceneProjectionValid;
		std::memcpy(projection, !liveShadowDraws.empty() ? liveShadowDraws.front().projection : liveShadowSceneProjection, sizeof(projection));

		// A strip at the city's edge often holds no shadow record and no terrain on the
		// height field, so its view stays unknown. It drew no caster either, so the
		// world-space set is still right. Dropping the set there forced a full redraw on
		// every scroll frame along the edge.
		bool const isEmptyPartialPass = isPartialPass && !hasView && !hasPass && liveShadowDraws.empty();
		bool isTracked = false;

		if (isRegistryOn && isStaticPass && !isEmptyPartialPass)
		{
			if (hasView && hasProjection)
			{
				float sun[3] = { liveShadowSunWorld[0], liveShadowSunWorld[1], liveShadowSunWorld[2] };
				Normalize(sun);
				isTracked = TrackLiveShadowWorldCasters(eyeToWorld, worldToView, projection, sun, isLiveShadowSunWorldValid, isPartialPass);
			}

			if (!isTracked)
			{
				isLiveShadowWorldValid = false;
			}
		}

		// Without the registry a partial pass only ever sees the casters drawn inside its own
		// rectangle, so once one has cast it cannot be trusted. With it, only a static pass
		// the world-space set could not follow is.
		bool const areCastersStale = isRegistryOn ? (isStaticPass && !isTracked && !isEmptyPartialPass && (!liveShadowDraws.empty() || !liveShadowWorldCasters.empty())) : wasLiveShadowEverCaptured;

		if (isPartialPass && (areCastersStale || isTerrainChanged))
		{
			isLiveShadowCleanRedrawPending = true;

			static bool hasLoggedPartial = false;
			if (!hasLoggedPartial)
			{
				hasLoggedPartial = true;
				Log(LogCategory::Initialization, "live shadows: partial static pass (%dx%d at %d,%d) forces a clean redraw", viewportWidth, viewportHeight, viewportX, viewportY);
			}
		}

		std::vector<LiveShadowDraw*> casters;

		if (isTracked)
		{
			float worldToClip[16] = {};
			Multiply(projection, worldToView, worldToClip);

			for (LiveShadowDraw& draw : liveShadowWorldCasters)
			{
				if (BoxInViewport(draw.shadowLow, draw.shadowHigh, worldToClip) == ViewportOverlap::Outside)
				{
					continue;
				}

				Multiply(worldToView, draw.modelToWorld, draw.modelView);
				casters.push_back(&draw);
			}
		}
		else
		{
			for (LiveShadowDraw& draw : liveShadowDraws)
			{
				casters.push_back(&draw);
			}
		}

		// A caster whose texture has since been deleted, or its name reused, is left out
		casters.erase(std::remove_if(casters.begin(), casters.end(), [&](LiveShadowDraw const* draw)
		{
			return draw->texture != 0 && vulkan->TextureSerial(draw->texture) != draw->textureSerial;
		}), casters.end());

		// Terrain shadows are composited in world space, so they need the view
		bool const isTerrainShadowed = terrainMap != nullptr && hasView;

		if (casters.empty() && pass.casters.empty() && !isTerrainShadowed)
		{
			liveShadowDraws.clear();
			return;
		}

		if (!isRegistryOn && !liveShadowDraws.empty())
		{
			wasLiveShadowEverCaptured = true;
		}

		if (!vulkan->CanDrawShadows() || !hasProjection)
		{
			// Captures are frame-local; never draw stale geometry from a frame where the
			// depth buffer was unavailable
			liveShadowDraws.clear();
			return;
		}

		// The sun ray in view space. The game's own GetShadowDirection when a pass reported
		// it; otherwise the camera estimate: in the game's camera basis a point one unit
		// high casts one unit along the ground, so the ray is the screen-right ground vector
		// minus the world-up vector, which every prop placement's modelview carries in its
		// second column.
		float viewUp[3] = {};
		unsigned viewUpSamples = 0;

		for (LiveShadowDraw const* caster : casters)
		{
			float candidate[3] = { caster->modelView[4], caster->modelView[5], caster->modelView[6] };
			float const length = std::sqrt(Dot(candidate, candidate));
			if (length <= 1.0e-6f)
			{
				continue;
			}

			for (float& value : candidate)
			{
				value /= length;
			}

			if (candidate[1] < 0.0f)
			{
				for (float& value : candidate)
				{
					value = -value;
				}
			}

			viewUp[1] += candidate[1];
			viewUp[2] += candidate[2];
			viewUpSamples++;
		}

		viewUp[0] = 0.0f;
		Normalize(viewUp);

		float estimatedDirection[3] = { 1.0f, -viewUp[1], -viewUp[2] };
		Normalize(estimatedDirection);

		float shadowDirection[3] = {};
		bool isGameDirection = false;

		if (hasView && isLiveShadowSunWorldValid)
		{
			TransformDirection(worldToView, liveShadowSunWorld, shadowDirection);
			isGameDirection = Dot(shadowDirection, shadowDirection) > 1.0e-12f;
			Normalize(shadowDirection);

			// The sun only moves with a rotation, which redraws the static view, so dynamic
			// passes reuse it and agree with the static shadows
			if (isGameDirection)
			{
				std::memcpy(liveShadowSunView, shadowDirection, sizeof(liveShadowSunView));
			}

			isLiveShadowSunViewValid = isGameDirection;
		}
		else if (isLiveShadowSunViewValid)
		{
			std::memcpy(shadowDirection, liveShadowSunView, sizeof(shadowDirection));
			isGameDirection = true;
		}

		if (!isGameDirection)
		{
			if (viewUpSamples == 0)
			{
				liveShadowDraws.clear();
				return;
			}

			std::memcpy(shadowDirection, estimatedDirection, sizeof(shadowDirection));
		}

		static bool hasLoggedDirection = false;
		if (!hasLoggedDirection)
		{
			hasLoggedDirection = true;
			Log(LogCategory::Initialization, "live shadows: sun ray %.4f/%.4f/%.4f from %s; %u draws estimate %.4f/%.4f/%.4f", static_cast<double>(shadowDirection[0]), static_cast<double>(shadowDirection[1]), static_cast<double>(shadowDirection[2]), isGameDirection ? "GetShadowDirection" : "the camera estimate", viewUpSamples, static_cast<double>(estimatedDirection[0]), static_cast<double>(estimatedDirection[1]), static_cast<double>(estimatedDirection[2]));
		}

		// The view-space box everything casting lies in
		float boundsLow[3]  = { FLT_MAX, FLT_MAX, FLT_MAX };
		float boundsHigh[3] = { -FLT_MAX, -FLT_MAX, -FLT_MAX };

		auto const includeBounds = [&](float const view[3])
		{
			for (unsigned axis = 0; axis < 3; axis++)
			{
				boundsLow[axis]  = std::min(boundsLow[axis], view[axis]);
				boundsHigh[axis] = std::max(boundsHigh[axis], view[axis]);
			}
		};

		for (LiveShadowDraw const* caster : casters)
		{
			for (size_t index = 0; index < caster->vertices.size(); index += VERTEX_FLOATS)
			{
				float view[3] = {};
				TransformPoint(caster->modelView, caster->vertices.data() + index, view);
				includeBounds(view);
			}
		}

		// Registry casters that can shade this pass's viewport
		std::vector<RegistryDraw> registryDraws;
		unsigned registryCulled = 0;
		unsigned registryMissingTexture = 0;

		if (hasView)
		{
			float sunWorld[3] = { liveShadowSunWorld[0], liveShadowSunWorld[1], liveShadowSunWorld[2] };
			if (!isLiveShadowSunWorldValid)
			{
				TransformDirection(eyeToWorld, shadowDirection, sunWorld);
			}

			Normalize(sunWorld);
			registryDraws.reserve(pass.casters.size());

			for (ShadowRegistry::PassCaster const& passCaster : pass.casters)
			{
				ShadowRegistry::Caster const& caster = *passCaster.caster;

				RegistryDraw draw;
				draw.caster      = &passCaster;
				draw.hasQuad     = GroundQuad(caster, sunWorld, draw.quad);
				draw.isQuadDrawn = draw.hasQuad && !caster.hasMeshCoordinates;

				if (caster.indices.empty() && !draw.isQuadDrawn)
				{
					continue;
				}

				// The caster's screen footprint with its ground shadow bounds everything it
				// can shade, walls included
				float points[12][3] = {};
				unsigned pointCount = 0;

				for (unsigned mask = 0; mask < 8; mask++)
				{
					float const corner[3] = { (mask & 1) ? caster.high[0] : caster.low[0], (mask & 2) ? caster.high[1] : caster.low[1], (mask & 4) ? caster.high[2] : caster.low[2] };
					TransformPoint(worldToView, corner, points[pointCount++]);
				}

				if (draw.hasQuad)
				{
					for (auto const& corner : draw.quad)
					{
						TransformPoint(worldToView, corner, points[pointCount++]);
					}
				}

				float ndcLow[2]  = { FLT_MAX, FLT_MAX };
				float ndcHigh[2] = { -FLT_MAX, -FLT_MAX };

				for (unsigned index = 0; index < pointCount; index++)
				{
					float const* const view = points[index];
					float const ndc[2] = { projection[0] * view[0] + projection[4] * view[1] + projection[8] * view[2] + projection[12], projection[1] * view[0] + projection[5] * view[1] + projection[9] * view[2] + projection[13] };

					for (unsigned axis = 0; axis < 2; axis++)
					{
						ndcLow[axis]  = std::min(ndcLow[axis], ndc[axis]);
						ndcHigh[axis] = std::max(ndcHigh[axis], ndc[axis]);
					}
				}

				if (ndcHigh[0] < -1.0f || ndcLow[0] > 1.0f || ndcHigh[1] < -1.0f || ndcLow[1] > 1.0f)
				{
					registryCulled++;
					continue;
				}

				if (vulkan->TextureSerial(passCaster.texture) == 0)
				{
					registryMissingTexture++;

					static bool hasLoggedMissing = false;
					if (!hasLoggedMissing)
					{
						hasLoggedMissing = true;
						Log(LogCategory::Initialization, "shadow registry: caster %u names texture %u, which the driver does not have", caster.id, passCaster.texture);
					}

					continue;
				}

				for (unsigned index = 0; index < pointCount; index++)
				{
					includeBounds(points[index]);
				}

				registryDraws.push_back(draw);
			}
		}

		if (!registryDraws.empty() || registryCulled != 0 || registryMissingTexture != 0)
		{
			static bool hasLoggedFirstDraw = false;
			if (!hasLoggedFirstDraw)
			{
				hasLoggedFirstDraw = true;
				Log(LogCategory::Initialization, "shadow registry: first pass drew %u of %u casters (culled=%u notex=%u, view %s)", static_cast<unsigned>(registryDraws.size()), static_cast<unsigned>(pass.casters.size()), registryCulled, registryMissingTexture, hasView ? "known" : "unknown");
			}
		}

		// A pass with no caster in reach can still lie in the terrain's shadow
		bool const hasCasters = !casters.empty() || !registryDraws.empty();
		if (!hasCasters && !isTerrainShadowed)
		{
			liveShadowDraws.clear();
			return;
		}

		float lightView[16] = {};
		float lightDepthRange = 1.0f;
		if (hasCasters)
		{
			lightDepthRange = BuildLightMatrix(boundsLow, boundsHigh, shadowDirection, lightView);
		}

		float lightWorld[16] = {};
		if (hasCasters && hasView)
		{
			Multiply(lightView, worldToView, lightWorld);
		}

		// Draw the casters into the shadow map: one upload for the whole pass, then one
		// draw per caster, since each carries its own transform, texture and test
		if (hasCasters)
		{
			std::vector<ShadowVertex>     vertices;
			std::vector<uint32_t>         indices;
			std::vector<ShadowCasterDraw> draws;

			try
			{
				for (LiveShadowDraw const* caster : casters)
				{
					ShadowCasterDraw draw;
					draw.firstIndex   = static_cast<uint32_t>(indices.size());
					draw.indexCount   = static_cast<uint32_t>(caster->indices.size());
					draw.topology     = caster->topology;
					draw.vertexOffset = static_cast<int32_t>(vertices.size());
					draw.texture      = caster->texture;
					std::memcpy(draw.samplerParameters, caster->samplerParameters, sizeof(draw.samplerParameters));

					Multiply(lightView, caster->modelView, draw.constants);
					TextureRows(caster->textureMatrix, draw.constants + 16);
					draw.constants[24] = static_cast<float>(caster->alphaFunction);
					draw.constants[25] = caster->alphaReference;
					draw.constants[26] = caster->isAlphaTested ? 1.0f : 0.0f;
					draw.constants[27] = (caster->texture != 0) ? 1.0f : 0.0f;

					for (size_t index = 0; index < caster->vertices.size(); index += VERTEX_FLOATS)
					{
						float const* const source = caster->vertices.data() + index;
						vertices.push_back(ShadowVertex{ { source[0], source[1], source[2] }, { source[3], source[4] } });
					}

					indices.insert(indices.end(), caster->indices.begin(), caster->indices.end());
					draws.push_back(draw);
				}

				// The game tests strength times alpha against strength times its alpha
				// scale, which is the texture's alpha against the scale
				float const registryAlphaReference = (pass.alphaScale > 0.0f) ? pass.alphaScale : DEFAULT_REGISTRY_ALPHA_REFERENCE;

				for (RegistryDraw const& registryDraw : registryDraws)
				{
					ShadowRegistry::Caster const& caster = *registryDraw.caster->caster;
					size_t const count = caster.positions.size() / 3;

					ShadowCasterDraw draw;
					draw.firstIndex   = static_cast<uint32_t>(indices.size());
					draw.indexCount   = static_cast<uint32_t>(caster.indices.size()) + (registryDraw.isQuadDrawn ? 6u : 0u);
					draw.vertexOffset = static_cast<int32_t>(vertices.size());
					draw.texture      = registryDraw.caster->texture;
					draw.isRegistry   = true;

					// True3D coordinates routinely run past 0 to 1, so those textures repeat
					std::memcpy(draw.samplerParameters, (registryDraw.caster->isWrapped || caster.hasMeshCoordinates) ? WRAP_SAMPLER : CLAMP_SAMPLER, sizeof(draw.samplerParameters));

					std::memcpy(draw.constants, lightWorld, sizeof(lightWorld));
					TextureRows(caster.projector, draw.constants + 16);
					draw.constants[25] = caster.hasMeshCoordinates ? DEFAULT_REGISTRY_ALPHA_REFERENCE : registryAlphaReference;
					draw.constants[26] = caster.hasMeshCoordinates ? 1.0f : 0.0f;
					std::memcpy(draw.constants + 28, caster.uvBounds, sizeof(caster.uvBounds));

					for (size_t vertex = 0; vertex < count; vertex++)
					{
						float const* const position = caster.positions.data() + vertex * 3;
						float const u = caster.hasMeshCoordinates ? caster.coordinates[vertex * 2] : 0.0f;
						float const v = caster.hasMeshCoordinates ? caster.coordinates[vertex * 2 + 1] : 0.0f;
						vertices.push_back(ShadowVertex{ { position[0], position[1], position[2] }, { u, v } });
					}

					indices.insert(indices.end(), caster.indices.begin(), caster.indices.end());

					if (registryDraw.isQuadDrawn)
					{
						uint32_t const first = static_cast<uint32_t>(count);
						for (auto const& corner : registryDraw.quad)
						{
							vertices.push_back(ShadowVertex{ { corner[0], corner[1], corner[2] }, { 0.0f, 0.0f } });
						}

						uint32_t const quad[6] = { first, first + 1, first + 2, first, first + 2, first + 3 };
						indices.insert(indices.end(), quad, quad + 6);
					}

					draws.push_back(draw);
				}
			}
			catch (std::bad_alloc const&)
			{
				liveShadowDraws.clear();
				return;
			}

			if (!vulkan->DrawShadowCasters(vertices.data(), static_cast<uint32_t>(vertices.size()), indices.data(), static_cast<uint32_t>(indices.size()), draws.data(), static_cast<uint32_t>(draws.size())))
			{
				static bool hasLoggedUpload = false;
				if (!hasLoggedUpload)
				{
					hasLoggedUpload = true;
					Log(LogCategory::Resource, "live shadows: the casters could not be drawn (%u vertices, %u indices)", static_cast<unsigned>(vertices.size()), static_cast<unsigned>(indices.size()));
				}

				liveShadowDraws.clear();
				return;
			}
		}

		// The terrain's maps only change with the height field or the sun, so they are
		// uploaded once per change, and again for a new device
		bool isTerrainBound = false;

		if (isTerrainShadowed)
		{
			TerrainShadowMap::Map const& map = *terrainMap;
			bool const isCurrent = liveShadowTerrainGeneration == terrainGeneration && liveShadowDeviceGeneration == vulkan->DeviceGeneration();

			if (!isCurrent)
			{
				TerrainShadowMap::HeightField const& field = TerrainShadows::Terrain();

				// Altitude, normal x and z (its y is up and recovered), and the flip of the
				// cell this vertex is the lowest corner of. The map goes up as the ceiling and
				// the umbra side by side, one texel each.
				std::vector<float> vertices;
				std::vector<float> ceilings;
				try
				{
					vertices.resize(size_t{ field.verticesX } * field.verticesZ * 4);
					ceilings.resize(map.ceiling.size() * 2);
				}
				catch (std::bad_alloc const&)
				{
					vertices.clear();
					ceilings.clear();
				}

				if (!vertices.empty() && !ceilings.empty())
				{
					for (size_t index = 0; index < map.ceiling.size(); index++)
					{
						ceilings[index * 2 + 0] = map.ceiling[index];
						ceilings[index * 2 + 1] = map.umbra[index];
					}

					for (uint32_t z = 0; z < field.verticesZ; z++)
					{
						for (uint32_t x = 0; x < field.verticesX; x++)
						{
							size_t const index = size_t{ z } * field.verticesX + x;

							float normal[3] = {};
							TerrainShadowMap::VertexNormal(field, x, z, normal);

							vertices[index * 4 + 0] = field.altitudes[index];
							vertices[index * 4 + 1] = normal[0];
							vertices[index * 4 + 2] = normal[2];
							vertices[index * 4 + 3] = (!field.flipped.empty() && field.flipped[index] != 0) ? 1.0f : 0.0f;
						}
					}

					if (vulkan->UploadTerrainShadowMaps(ceilings.data(), map.width, map.height, vertices.data(), field.verticesX, field.verticesZ))
					{
						liveShadowTerrainGeneration = terrainGeneration;
						liveShadowDeviceGeneration  = vulkan->DeviceGeneration();
					}
				}
			}

			isTerrainBound = liveShadowTerrainGeneration == terrainGeneration && liveShadowDeviceGeneration == vulkan->DeviceGeneration();
		}

		if (!hasCasters && !isTerrainBound)
		{
			liveShadowDraws.clear();
			return;
		}

		// The composite
		ShadowCompositeConstants composite{};
		std::memcpy(composite.lightMatrix, lightView, sizeof(lightView));
		composite.material[0]     = SHADOW_DEPTH_BIAS_WORLD / lightDepthRange;
		composite.terrainShade[3] = hasCasters ? 1.0f : 0.0f;

		if (isTerrainBound)
		{
			TerrainShadowMap::Map const& map = *terrainMap;
			std::memcpy(composite.eyeToWorld, eyeToWorld, sizeof(eyeToWorld));

			composite.terrainAxes[0] = map.along[0];
			composite.terrainAxes[1] = map.along[1];
			composite.terrainAxes[2] = map.across[0];
			composite.terrainAxes[3] = map.across[1];

			// Texel i's centre, at originAlong + i * spacing, samples at (i + 0.5) / width
			composite.terrainGrid[0] = 1.0f / (map.spacing * static_cast<float>(map.width));
			composite.terrainGrid[1] = (0.5f - map.originAlong / map.spacing) / static_cast<float>(map.width);
			composite.terrainGrid[2] = 1.0f / (map.spacing * static_cast<float>(map.height));
			composite.terrainGrid[3] = (0.5f - map.originAcross / map.spacing) / static_cast<float>(map.height);

			composite.terrainShade[0] = 1.0f;
			composite.terrainShade[1] = TERRAIN_SHADOW_BIAS;
			composite.terrainShade[2] = TERRAIN_SHADOW_SOFTNESS;

			TerrainShadowMap::HeightField const& field = TerrainShadows::Terrain();
			composite.terrainCell[0] = field.cellWidth;
			composite.terrainCell[1] = TERRAIN_PIXEL_TOLERANCE;
			composite.terrainCell[2] = static_cast<float>(field.verticesX);
			composite.terrainCell[3] = static_cast<float>(field.verticesZ);
		}

		composite.projection0[0] = projection[0];
		composite.projection0[1] = projection[5];
		composite.projection0[2] = projection[10];
		composite.projection0[3] = projection[12];
		composite.projection1[0] = projection[13];
		composite.projection1[1] = projection[14];
		composite.projection1[2] = 1.0f / SHADOW_MAP_SIZE;

		// The game's shadow colour and strength, faded with the lighting manager's own day
		// and night weight
		float daylight = 1.0f;
		LiveShadowHooks::ShadowDaylight(daylight);

		composite.projection1[3] = (isLiveShadowToneValid ? liveShadowStrength : FALLBACK_SHADOW_OPACITY) * daylight;
		if (isLiveShadowToneValid)
		{
			std::memcpy(composite.tone, liveShadowColour, sizeof(liveShadowColour));
		}

		std::memcpy(composite.sun, shadowDirection, sizeof(shadowDirection));

		// N.L of flat ground: the sun's elevation. The camera estimate assumes the game's 45
		// degrees.
		float flatLight = 0.70710678f;
		if (isGameDirection)
		{
			float sunWorld[3] = { liveShadowSunWorld[0], liveShadowSunWorld[1], liveShadowSunWorld[2] };
			Normalize(sunWorld);
			flatLight = std::max(-sunWorld[1], 0.05f);
		}

		composite.sun[3] = 1.0f / flatLight;

		// The composite covers the pass's viewport: outside a partial pass's dirty
		// rectangle the game keeps the pixels it has, and darkening them again would blend
		// the same shadow over itself on every update
		int32_t rectangle[4] = { 0, 0, windowWidth, windowHeight };
		if (isPartialPass)
		{
			rectangle[0] = viewportX;
			rectangle[1] = windowHeight - viewportY - viewportHeight;
			rectangle[2] = viewportWidth;
			rectangle[3] = viewportHeight;
		}

		composite.viewport[0] = static_cast<float>(rectangle[0]);
		composite.viewport[1] = static_cast<float>(rectangle[1]);
		composite.viewport[2] = static_cast<float>(std::max(rectangle[2], 1));
		composite.viewport[3] = static_cast<float>(std::max(rectangle[3], 1));

		vulkan->CompositeShadows(composite, rectangle, isTerrainBound);
		liveShadowDraws.clear();
	}
}
