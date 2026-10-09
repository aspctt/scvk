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

#include <cstdint>

// Native shadow support, opt-in with -NativeShadowMasks:network, :props or
// :all (:replace also installs NativeShadowRegistry). Prebuilt network pieces
// and True3D props are captured as indexed live casters and rendered by cGDriver.
// SC4's existing network mask decals remain available as a fallback for
// geometry outside the prebuilt-model draw path.
//
// Independent of NativeShadowExperiment, which stays a research toggle.

namespace scvk::NativeShadowMasks {
	bool Install();
	void Uninstall();

	// Native shadow pixels extend beyond SC4's ordinary object dirty rectangles,
	// so translated backing-store updates must rebuild the static view.
	bool RequiresCleanTranslatedRedraw();
	bool LiveNetworkEnabled();
	bool LiveNetworkDrawActive();
	bool LivePropsEnabled();
	bool HasLivePropMeshes();
	bool MatchLivePropSignature(uint64_t signature);
	// Registers a relaxed prop's mesh (AddShadow's position and UV streams) for
	// per-draw capture; the registry's fallback when it cannot take the prop.
	void RegisterLivePropMesh(uint32_t count, void const *positions, void const *uvs);

} // namespace scvk::NativeShadowMasks
