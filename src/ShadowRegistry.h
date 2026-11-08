/*
 *  SCD3D11 - a free Direct3D 11 driver for SimCity 4's SimGL interface
 *  Copyright (C) 2026
 *
 *  This library is free software; you can redistribute it and/or
 *  modify it under the terms of the GNU Lesser General Public
 *  License as published by the Free Software Foundation, under
 *  version 2.1 of the License, or (at your option) any later version.
 */

#pragma once

//// Dependencies

#include <memory>
#include <stddef.h>
#include <stdint.h>
#include <vector>

/**
 * Replaces the game's projected BAT shadows (buildings, flora, BAT props, poles) with
 * casters in cVKDriver's live shadow map, as part of -LiveShadows:on.
 *
 * cSTEOverlayManager::AddShadow already turns each caster mesh into a world to texture
 * projector for the game's prerendered shadow texture. This module keeps that projector,
 * the texture and the mesh for every shadow record the game makes, takes those records
 * out of the list DrawShadows draws, and hands the driver the ones each static pass would
 * have drawn. True3D props the game would not shadow (no Is Ground Model) come through the
 * same call once LiveShadowHooks lets it run, and cast with their own coordinates. The
 * game tracks every record's lifetime and dirties its rectangle when it changes, so none of
 * these casters needs a full redraw to stay right.
 *
 * Carried over from SCD3D11. Only game version 641 is changed.
 */
namespace scvk::ShadowRegistry
{
	//// Constants

	/** The floats of a caster vertex: world position, then the mesh's coordinate, zero unless hasMeshCoordinates. */
	constexpr size_t VERTEX_FLOATS = 5;

	//// Types

	/** One shadow record the game made, kept in world space. */
	struct Caster
	{
		uint32_t    id      = 0;
		void const* manager = nullptr;
		void const* binding = nullptr;

		// VERTEX_FLOATS per vertex, laid out as the driver's shadow pass reads them, so a pass
		// copies them as they are
		std::vector<float> vertices;

		// A triangle list into the vertices. Empty when the mesh's topology could not be
		// recovered, and the caster then casts from its ground quad alone.
		std::vector<uint32_t> indices;

		// The record's +0x1C: world position to texture coordinate, column major, the same
		// all along the sun's direction
		float projector[16] = {};

		// u and v minimum, then maximum, over the mesh's coordinates, where DrawShadows clips
		float uvBounds[4] = {};

		float low[3]  = {};
		float high[3] = {};

		// The occupant's placement height, where its ground quad sits
		float baseHeight = 0.0f;

		// A True3D prop the game itself would not shadow. It casts with its own coordinates,
		// kept in the vertices, rather than through the projector, which only fits a
		// prerendered view.
		bool hasMeshCoordinates = false;

		// Taken out of DrawShadows but not cast: a relaxed prop the registry could not keep,
		// which casts through the per-draw path instead
		bool isSuppressedOnly = false;
	};

	/** A caster as one pass draws it. */
	struct PassCaster
	{
		std::shared_ptr<Caster const> caster;

		// The driver's texture name, read through the record's texture binding when the pass
		// was gathered
		uint32_t texture = 0;

		// Record flag bit 1: DrawShadows samples with repeat instead of clamp
		bool isWrapped = false;
	};

	/** Everything the DrawShadows calls handed over since the last TakePass. */
	struct Pass
	{
		// The projection the gather culled the casters against, when isCulled is set
		float projection[16] = {};
		bool  isCulled       = false;

		// DrawShadows' first argument: eye space to world, the texture generation transform
		// the game composes with each record's projector
		float eyeToWorld[16] = {};

		// The world direction the light travels, from GetShadowDirection
		float sunDirection[3] = {};
		bool  isSunValid      = false;

		// GetShadowParams: the colour DrawShadows blends, its strength, and the alpha test
		// scale, the reference being strength times scale
		float colour[3]  = {};
		float strength   = 0.0f;
		float alphaScale = 0.0f;

		std::vector<PassCaster> casters;
	};

	/** What the hooks did since the last TakeStats. The times are counted only while the log is at debug level. */
	struct Stats
	{
		uint32_t registrations = 0;

		// AddShadow calls made on another thread than DrawShadows
		uint32_t offThreadRegistrations = 0;

		// Performance counter ticks in AddShadow's registration, DrawShadows' gather, and
		// the game's own DrawShadows for the records the registry left it
		int64_t registerTicks   = 0;
		int64_t gatherTicks     = 0;
		int64_t nativeDrawTicks = 0;
	};

	//// Public API

	/** Hooks the game's shadow records, once LiveShadowHooks is in place. Returns whether it is. */
	bool Install(void);

	/** Puts the game's code back as it was. */
	void Uninstall(void);

	/** Whether the registry is in place. */
	bool Enabled(void);

	/** Marks the next AddShadow on this thread as a relaxed True3D prop, to be kept with its own coordinates. */
	void ExpectMeshCaster(void);

	/**
	 * The driver's projection, whenever it changes. A static pass's DrawShadows runs under
	 * that pass's projection, so the gather culls the pass's casters against it rather than
	 * handing over thousands the pass cannot see. Render thread only.
	 */
	void NoteProjection(float const projection[16]);

	/** Moves out the pass gathered since the last call. False when no DrawShadows call ran in between. */
	bool TakePass(Pass& pass);

	/** Moves out the hooks' counts and times since the last call. */
	Stats TakeStats(void);

	/**
	 * The ground quad a caster adds for silhouettes its mesh does not cover from the sun,
	 * such as tree cards and thin LOD shells: its box pushed down the sun onto the plane at
	 * its placement height. The sun is the normalised world direction light travels. Every
	 * point of the quad shares its coordinate with the terrain point below it along the sun,
	 * so on flat ground it reproduces the game's projected shadow. False when the sun is at or
	 * above the horizon.
	 */
	bool GroundQuad(Caster const& caster, float const sun[3], float quad[4][3]);

	/**
	 * Whether the world box holding a caster and its quad, when it has one, lies outside the
	 * NDC square of worldToClip, an affine column-major matrix. Nothing the caster shades is in
	 * that view then.
	 */
	bool IsReachOutside(Caster const& caster, bool hasQuad, float const quad[4][3], float const* worldToClip);

	/**
	 * Asks the game to redraw the terrain cells under a world rectangle in x and z, the way
	 * RedisplayStaticOverlay does when a shadow record is added or removed. The redraw comes
	 * as a partial static pass on a later frame.
	 */
	bool RedisplayWorldRect(float minimumX, float minimumZ, float maximumX, float maximumZ);

	/** The same for a terrain the caller already has. */
	bool RedisplayTerrainRect(void* terrain, bool shouldUpdateView, float minimumX, float minimumZ, float maximumX, float maximumZ);

	/**
	 * The game's SimGL primitive numbering to a triangle list, appended to triangles. Points
	 * and lines append nothing and return false.
	 */
	bool AppendTriangles(uint32_t primitive, uint32_t const* indices, uint32_t count, std::vector<uint32_t>& triangles);
}
