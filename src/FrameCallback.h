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

#pragma once

/*
 * scvk's side of the frame callback another plugin registers through the exports
 * described in SCVKFrameCallback.h.
 */

//// Dependencies

#include "VulkanApi.h"
#include "SCVKFrameCallback.h"

namespace scvk
{
	//// Public API

	/** A new device generation, unique across every backend in the process. */
	uint32_t NextDeviceGeneration(void);

	/** Whether a callback is registered, so a frame only prepares for one when it is. */
	bool IsFrameCallbackRegistered(void);

	/** Calls the registered callback, and returns whether there was one. */
	bool InvokeFrameCallback(SCVKFrameContext const& frame);
}
