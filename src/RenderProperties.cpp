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

#include "RenderProperties.h"

#include <GZServPtrs.h>
#include <cISC4App.h>
#include <cISC4RenderProperties.h>

#include <windows.h>
#include <string.h>

namespace scvk::RenderProperties
{
	//// Public API

	cISC4RenderProperties* Find(void)
	{
		cISC4AppPtr app;
		return app ? app->GetRenderProperties() : nullptr;
	}

#if defined(_MSC_VER) && defined(_M_IX86)
	bool FindBool(cISC4RenderProperties* properties, char const* name, int32_t& id)
	{
		__try
		{
			int32_t const found = properties->BoolPropertyIDFromName(name);
			char const* const readBack = properties->NameFromBoolPropertyID(found);
			if (readBack == nullptr || _stricmp(readBack, name) != 0)
			{
				return false;
			}

			id = found;
			return true;
		}
		__except (EXCEPTION_EXECUTE_HANDLER)
		{
			return false;
		}
	}

	bool FindInt(cISC4RenderProperties* properties, char const* name, int32_t& id)
	{
		__try
		{
			int32_t const found = properties->IntPropertyIDFromName(name);
			char const* const readBack = properties->NameFromIntPropertyID(found);
			if (readBack == nullptr || _stricmp(readBack, name) != 0)
			{
				return false;
			}

			id = found;
			return true;
		}
		__except (EXCEPTION_EXECUTE_HANDLER)
		{
			return false;
		}
	}

	bool ReadBool(cISC4RenderProperties* properties, int32_t id, bool& value)
	{
		__try
		{
			value = properties->BoolValue(id);
			return true;
		}
		__except (EXCEPTION_EXECUTE_HANDLER)
		{
			return false;
		}
	}

	bool ReadInt(cISC4RenderProperties* properties, int32_t id, int32_t& value)
	{
		__try
		{
			value = properties->IntValue(id);
			return true;
		}
		__except (EXCEPTION_EXECUTE_HANDLER)
		{
			return false;
		}
	}
#else
	bool FindBool(cISC4RenderProperties*, char const*, int32_t&)
	{
		return false;
	}

	bool FindInt(cISC4RenderProperties*, char const*, int32_t&)
	{
		return false;
	}

	bool ReadBool(cISC4RenderProperties*, int32_t, bool&)
	{
		return false;
	}

	bool ReadInt(cISC4RenderProperties*, int32_t, int32_t&)
	{
		return false;
	}
#endif
}
