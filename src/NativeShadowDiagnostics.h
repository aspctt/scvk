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

// Phase 1 instrumentation for SimCity 4's two native shadow paths. Read-only:
// it records what the game already does and changes no game behaviour.
//
// Disabled unless -NativeShadowDiag[:<budget>|:all] is on the command line.
// Independent of NativeShadowExperiment; the two patch disjoint sites and may
// be enabled together.

namespace scvk::NativeShadowDiagnostics {

	bool Install();
	void Uninstall();

} // namespace scvk::NativeShadowDiagnostics
