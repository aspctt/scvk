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

//// Dependencies

#include <stdint.h>

namespace scvk
{
	//// Types

	/**
	 * The log categories of SCD3D11, for the modules carried over from it.
	 *
	 * The shadow modules and the thumbnail guard log through SCD3D11's Log and LogTrace.
	 * Keeping those names lets them stay as they are upstream; each line goes to
	 * scvk.log at info level, or at trace level for LogTrace, tagged with its category.
	 */
	enum class LogCategory
	{
		Initialization,
		Capabilities,
		SwapChain,
		Resource,
		Grid,
		Unsupported,
		Window,
		Count
	};

	//// Public API

	void Log(LogCategory category, char const* format, ...);
	void LogTrace(LogCategory category, char const* format, ...);
}
