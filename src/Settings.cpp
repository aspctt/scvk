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

#include "Settings.h"
#include "Logger.h"

#include <windows.h>
#include <string.h>

namespace scvk
{
	//// Private Functions

	namespace
	{
		/** Whether a character ends a command line switch. */
		bool IsSwitchEnd(char character)
		{
			return character == '\0' || character == ' ' || character == '\t' || character == '"';
		}

		/** The first occurrence of a switch, matched without regard to case, or null. */
		char const* FindSwitch(char const* commandLine, char const* name, bool isPrefix)
		{
			size_t const length = strlen(name);
			if (commandLine == nullptr || length == 0)
			{
				return nullptr;
			}

			for (char const* at = commandLine; *at != '\0'; at++)
			{
				// Only at the start of an argument
				bool const isArgumentStart = at == commandLine || at[-1] == ' ' || at[-1] == '\t' || at[-1] == '"';
				if (!isArgumentStart || _strnicmp(at, name, length) != 0)
				{
					continue;
				}

				if (isPrefix || IsSwitchEnd(at[length]))
				{
					return at;
				}
			}

			return nullptr;
		}

		Settings ReadSettings(void)
		{
			Settings settings;

			// The defaults are chosen to need no setting at all; SCD3D11's command line
			// switches still change them for whoever passes one
			if (HasCommandLineSwitch("-VSync:off"))
			{
				settings.isVSyncEnabled = false;
			}
			else if (HasCommandLineSwitch("-VSync:on"))
			{
				settings.isVSyncEnabled = true;
			}

			if (HasCommandLineSwitch("-GPU:default"))
			{
				settings.shouldPreferHighPerformanceGpu = false;
			}

			if (HasCommandLineSwitch("-Borderless") || HasCommandLineSwitch("-FullscreenMode:Borderless"))
			{
				settings.isBorderlessFullscreen = true;
			}

			if (HasCommandLineSwitch("-ReShade:off"))
			{
				settings.isReShadeIntegrationEnabled = false;
			}

			// The shadow modules, all unless a switch names another mode
			if (HasCommandLineSwitch("-NativeShadowMasks:off"))
			{
				settings.nativeShadowMode = NativeShadowMode::Off;
			}
			else if (HasCommandLineSwitch("-NativeShadowMasks:network"))
			{
				settings.nativeShadowMode = NativeShadowMode::Network;
			}
			else if (HasCommandLineSwitch("-NativeShadowMasks:props"))
			{
				settings.nativeShadowMode = NativeShadowMode::Props;
			}
			else if (HasCommandLineSwitch("-NativeShadowMasks:all"))
			{
				settings.nativeShadowMode = NativeShadowMode::All;
			}
			else if (HasCommandLineSwitch("-NativeShadowMasks:replace"))
			{
				settings.nativeShadowMode = NativeShadowMode::Replace;
			}

			if (HasCommandLineSwitch("-FlipModel:off"))
			{
				settings.isFlipModelPreferred = false;
			}

			settings.isGridDebugEnabled = HasCommandLineSwitch("-GridDebug");
			return settings;
		}
	}

	//// Public API

	Settings const& GetSettings(void)
	{
		static Settings const settings = ReadSettings();
		return settings;
	}

	bool HasCommandLineSwitch(char const* name)
	{
		return FindSwitch(GetCommandLineA(), name, false) != nullptr;
	}

	bool GetCommandLineValue(char const* prefix, char* outValue, uint32_t capacity)
	{
		char const* const found = FindSwitch(GetCommandLineA(), prefix, true);
		if (found == nullptr || outValue == nullptr || capacity == 0)
		{
			return false;
		}

		// Copy up to the end of the argument
		char const* value = found + strlen(prefix);
		uint32_t    count = 0;

		while (!IsSwitchEnd(value[count]) && count + 1 < capacity)
		{
			outValue[count] = value[count];
			count++;
		}

		outValue[count] = '\0';
		return true;
	}
}
