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

//// Dependencies

#include "ModuleLog.h"
#include "Logger.h"

#include <stdarg.h>
#include <stdio.h>

namespace scvk
{
	//// Private Functions

	namespace
	{
		char const* CategoryName(LogCategory category)
		{
			switch (category)
			{
			case LogCategory::Initialization: return "init";
			case LogCategory::Capabilities:   return "caps";
			case LogCategory::SwapChain:      return "swapchain";
			case LogCategory::Resource:       return "resource";
			case LogCategory::Grid:           return "grid";
			case LogCategory::Unsupported:    return "unsupported";
			case LogCategory::Window:         return "window";
			default:                          return "module";
			}
		}

		void Write(LogLevel level, LogCategory category, char const* format, va_list arguments)
		{
			if (!IsLogged(level))
			{
				return;
			}

			// Prefix the category, then hand the line on
			char tagged[1024];
			sprintf_s(tagged, sizeof(tagged), "[%s] %s", CategoryName(category), format);
			LogMessage(level, tagged, arguments);
		}
	}

	//// Public API

	void Log(LogCategory category, char const* format, ...)
	{
		va_list arguments;
		va_start(arguments, format);
		Write(LOG_LEVEL_INFO, category, format, arguments);
		va_end(arguments);
	}

	void LogTrace(LogCategory category, char const* format, ...)
	{
		va_list arguments;
		va_start(arguments, format);
		Write(LOG_LEVEL_TRACE, category, format, arguments);
		va_end(arguments);
	}
}
