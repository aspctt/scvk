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
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

namespace scvk
{
	//// Constants

	namespace
	{
		// More than any machine holds, and small enough to rule out a stray long number.
		constexpr unsigned long MAXIMUM_GPU_NUMBER = 16;

		// The words a flag takes, the same set SCD3D11 takes, so -VSync:false works with
		// either renderer.
		constexpr char const* FLAG_ON_WORDS[]  = { "on", "true", "enabled", "1" };
		constexpr char const* FLAG_OFF_WORDS[] = { "off", "false", "disabled", "0" };

		// Room for the longest flag switch scvk reads, its dash and colon included.
		constexpr size_t FLAG_SWITCH_LENGTH = 32;

		// Room for the longest flag word. A longer value is cut short and then read as no
		// flag, which it is.
		constexpr uint32_t FLAG_VALUE_LENGTH = 16;
	}

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

		/** The number in -GPU:<n>, or 0 when there is no switch or it names no device. */
		uint32_t ReadGpuNumber(void)
		{
			char value[32] = {};
			if (!GetCommandLineValue("-GPU:", value, sizeof(value)) || _stricmp(value, "default") == 0 || _stricmp(value, "high-performance") == 0)
			{
				return 0;
			}

			char* end = nullptr;
			unsigned long const number = strtoul(value, &end, 10);

			if (end == value || *end != '\0' || number == 0 || number > MAXIMUM_GPU_NUMBER)
			{
				LogWarn("-GPU:%s is neither default nor a number from 1 to %lu; ignoring it.", value, MAXIMUM_GPU_NUMBER);
				return 0;
			}

			// At most 16, checked just above.
			return static_cast<uint32_t>(number);
		}

		/** -DoctorDialog on the command line, then DoctorDialog in scvk.ini, on when neither says. */
		bool ReadDoctorDialogChoice(void)
		{
			FlagSwitch const flag = ReadFlagSwitch("DoctorDialog");
			if (flag != FlagSwitch::Absent)
			{
				return flag == FlagSwitch::On;
			}

			char settingsPath[MAX_PATH];
			if (!LogFilePath("scvk.ini", settingsPath, sizeof(settingsPath)))
			{
				return true;
			}

			char value[FLAG_VALUE_LENGTH] = {};
			ReadIniValue(settingsPath, "scvk", "DoctorDialog", "true", value, sizeof(value));

			bool isOn = true;
			if (!ParseFlag(value, isOn))
			{
				LogWarn("DoctorDialog=%s in scvk.ini is neither true nor false; showing the dialog.", value);
				return true;
			}

			return isOn;
		}

		/** Changes a setting to what its flag switch says, when the switch is given. */
		void ReadFlagSetting(char const* name, bool& setting)
		{
			FlagSwitch const flag = ReadFlagSwitch(name);
			if (flag != FlagSwitch::Absent)
			{
				setting = flag == FlagSwitch::On;
			}
		}

		Settings ReadSettings(void)
		{
			Settings settings;

			// The defaults are chosen to need no setting at all; SCD3D11's command line
			// switches still change them for whoever passes one
			ReadFlagSetting("VSync", settings.isVSyncEnabled);

			if (HasCommandLineSwitch("-GPU:default"))
			{
				settings.shouldPreferHighPerformanceGpu = false;
			}

			settings.gpuNumber = ReadGpuNumber();
			settings.isDoctorDialogEnabled = ReadDoctorDialogChoice();

			ReadFlagSetting("ViewportCull", settings.isViewportCullEnabled);
			ReadFlagSetting("ReShade", settings.isReShadeIntegrationEnabled);

			// The shadow modules, off unless a switch names a mode. SCD3D11 folded its modes
			// into -LiveShadows, on alone or with :on, which is what Replace does here. A
			// mode named as well wins, being the more exact of the two.
			FlagSwitch const liveShadows = ReadFlagSwitch("LiveShadows");
			if (liveShadows == FlagSwitch::On)
			{
				settings.nativeShadowMode = NativeShadowMode::Replace;
			}
			else if (liveShadows == FlagSwitch::Off)
			{
				settings.nativeShadowMode = NativeShadowMode::Off;
			}

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

			ReadFlagSetting("FlipModel", settings.isFlipModelPreferred);

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

	FlagSwitch ReadFlagSwitch(char const* name)
	{
		// Alone, which means on
		char text[FLAG_SWITCH_LENGTH];
		sprintf_s(text, sizeof(text), "-%s", name);

		if (HasCommandLineSwitch(text))
		{
			return FlagSwitch::On;
		}

		// Then with a value
		strcat_s(text, sizeof(text), ":");

		char value[FLAG_VALUE_LENGTH] = {};
		if (!GetCommandLineValue(text, value, sizeof(value)))
		{
			return FlagSwitch::Absent;
		}

		bool isOn = false;
		if (!ParseFlag(value, isOn))
		{
			LogWarn("-%s:%s is neither on nor off; ignoring it.", name, value);
			return FlagSwitch::Absent;
		}

		return isOn ? FlagSwitch::On : FlagSwitch::Off;
	}

	bool ParseFlag(char const* text, bool& outIsOn)
	{
		for (char const* const word : FLAG_ON_WORDS)
		{
			if (_stricmp(text, word) == 0)
			{
				outIsOn = true;
				return true;
			}
		}

		for (char const* const word : FLAG_OFF_WORDS)
		{
			if (_stricmp(text, word) == 0)
			{
				outIsOn = false;
				return true;
			}
		}

		return false;
	}

	void ReadIniValue(char const* path, char const* section, char const* key, char const* fallback, char* outValue, uint32_t capacity)
	{
		GetPrivateProfileStringA(section, key, fallback, outValue, capacity, path);

		// Windows keeps a comment after a value as part of it, so
		// Use32BitDepth=true ; for banding read as false with nothing to say so. A value
		// cut short by the buffer loses only comment.
		size_t length = strcspn(outValue, ";#");

		while (length > 0 && (outValue[length - 1] == ' ' || outValue[length - 1] == '\t'))
		{
			length--;
		}

		outValue[length] = '\0';
	}
}
