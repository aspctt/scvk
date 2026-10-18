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

namespace scvk::NativeShadowExperiment {

	// Disabled unless -NativeShadowExperiment:prebuilt or :all is present.
	bool Install();
	void Uninstall();

} // namespace scvk::NativeShadowExperiment
