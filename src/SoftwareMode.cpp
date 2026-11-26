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

/*
 * How the game ends up on its Software driver, from the game's startup (0x44c320):
 *
 * - When the graphics system has no working canvas once its driver has started, the
 *   preferences get the Software driver, and the game says it "will run with the
 *   software driver when next launched" (0x44cc2a).
 * - When the driver drawing reports no initialised video mode, as the Software driver
 *   does once every hardware slot has failed its Init, the game says it "Could not
 *   initialize the hardware driver" and keeps the Software driver in its preferences
 *   (0x44ac30). A hardware driver that works puts the DirectX driver back in them there.
 *
 * Both also mark the preferences as new (prefsFirstTime, 0x7cfe10), so the next start
 * runs Graphics Rules.sgr's first-time settings, which choose the driver again. Where
 * they choose Software the game stays there, as scd3d11 found, until a launch option
 * says otherwise. On the test machine a failed start lasted one launch, and even a
 * preference set to Software by hand was hardware again before this runs, so only this
 * side was tested there, with the game put on Software at this point. scd3d11 brings
 * its own slot back the same way, so this leaves the game to it when it is installed.
 */

//// Dependencies

#include "SoftwareMode.h"
#include "Logger.h"
#include "OptionsApiRenderer.h"
#include "RendererChoice.h"
#include "SC4Version.h"
#include "VulkanBackend.h"

#include <cIGZCmdLine.h>
#include <cIGZFrameWork.h>
#include <cIGZGraphicSystem2.h>
#include <cRZBaseString.h>
#include <cRZCOMDllDirector.h>
#include <GZServPtrs.h>
#include <SC4VideoPreferences.h>

#include <windows.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <wchar.h>

namespace scvk::SoftwareMode
{
	//// Constants

	namespace
	{
		constexpr uint16_t SUPPORTED_GAME_VERSION = 641;

		// The preferences' driver values the game reads at startup (0x44ce40)
		constexpr uint8_t SOFTWARE_DRIVER_TYPE = 0;
		constexpr uint8_t DIRECTX_DRIVER_TYPE  = 1;

		// Written next to scvk.dll before a try, holding why scvk failed when it knows,
		// and deleted once scvk has started
		constexpr char const* MARKER_FILE_NAME = "scvk-software.txt";
		constexpr size_t      REASON_LENGTH    = 256;

		// The game's switch for its driver, which -d:soft and -d:DirectX use
		constexpr char const* DRIVER_SWITCH = "d";

		// SC4GraphicsOptions picks a driver from its settings, which it reads from its own
		// folder
		constexpr char const*    GRAPHICS_OPTIONS_MODULE_NAME   = "SC4GraphicsOptions.dll";
		constexpr wchar_t const* GRAPHICS_OPTIONS_SETTINGS_NAME = L"SC4GraphicsOptions.ini";
		constexpr wchar_t const* GRAPHICS_OPTIONS_SECTION       = L"GraphicsOptions";
		constexpr wchar_t const* GRAPHICS_OPTIONS_DRIVER        = L"Driver";
		constexpr DWORD          DRIVER_NAME_LENGTH             = 64;

		// scd3d11's DLL, by the name its release ships it under
		constexpr char const* SCD3D11_MODULE_NAME = "SCD3D11.dll";
	}

	//// State

	namespace
	{
		bool        hasEarlierFailure = false;
		std::string earlierFailure;
	}

	//// Private Functions

	namespace
	{
		/** Whether the launch options name a driver, as -d:soft does. */
		bool IsDriverSwitchGiven(void)
		{
			cIGZCmdLine* const commandLine = RZGetFrameWork()->CommandLine();

			cRZBaseString value;
			return commandLine != nullptr && commandLine->IsSwitchPresent(cRZBaseString(DRIVER_SWITCH), value, true);
		}

		/** Whether SC4GraphicsOptions is installed and its settings name a driver. */
		bool IsGraphicsOptionsDriverSet(void)
		{
			HMODULE const graphicsOptions = GetModuleHandleA(GRAPHICS_OPTIONS_MODULE_NAME);
			if (graphicsOptions == nullptr)
			{
				return false;
			}

			// Find its settings next to it
			//
			// Read through the wide functions, since the folder can be anywhere the player
			// keeps plugins, under any name.
			wchar_t     path[MAX_PATH] = {};
			DWORD const pathLength     = GetModuleFileNameW(graphicsOptions, path, MAX_PATH);

			if (pathLength == 0 || pathLength >= MAX_PATH)
			{
				return false;
			}

			wchar_t* const separator = wcsrchr(path, L'\\');
			if (separator == nullptr)
			{
				return false;
			}

			separator[1] = L'\0';
			if (wcscat_s(path, MAX_PATH, GRAPHICS_OPTIONS_SETTINGS_NAME) != 0)
			{
				return false;
			}

			wchar_t driver[DRIVER_NAME_LENGTH] = {};
			GetPrivateProfileStringW(GRAPHICS_OPTIONS_SECTION, GRAPHICS_OPTIONS_DRIVER, L"", driver, DRIVER_NAME_LENGTH, path);

			return driver[0] != L'\0';
		}

		/** Reads the marker's reason, returning false when there is no marker. */
		bool ReadMarker(char const* path, std::string& reason)
		{
			FILE* file = nullptr;
			if (fopen_s(&file, path, "rb") != 0 || file == nullptr)
			{
				return false;
			}

			char text[REASON_LENGTH] = {};
			size_t const length = fread(text, 1, sizeof(text) - 1, file);
			fclose(file);

			// Keep the first line, which is all NoteStartFailure writes
			text[length] = '\0';
			text[strcspn(text, "\r\n")] = '\0';
			reason = text;
			return true;
		}

		bool WriteMarker(char const* path, char const* reason)
		{
			FILE* file = nullptr;
			if (fopen_s(&file, path, "wb") != 0 || file == nullptr)
			{
				return false;
			}

			bool const isWritten = fputs(reason, file) >= 0;
			return fclose(file) == 0 && isWritten;
		}
	}

	//// Public API

	bool Leave(SC4VideoPreferences& video)
	{
		// Only read the preferences of the build their layout came from
		if (GetGameVersion() != SUPPORTED_GAME_VERSION || video.driverType != SOFTWARE_DRIVER_TYPE)
		{
			return false;
		}

		// Keep the driver SC4 Options API asked for
		//
		// It asks the game for the player's renderer at every start, so Software here is the
		// player's choice, not a failed start. Undoing it would also take Legacy Hardware's
		// DirectX slot for scvk's. It answers None for a start it leaves alone, which the
		// marker below still covers.
		if (OptionsApiRenderer::ReadRenderer() != OptionsAPIRenderer::None)
		{
			return false;
		}

		// Keep a Software driver a launch option or SC4GraphicsOptions asked for
		//
		// Those choices are made again at every start, so they are no failure to undo.
		// -d:soft lasts one launch: the next one without it started on DirectX.
		if (IsDriverSwitchGiven() || IsGraphicsOptionsDriverSet())
		{
			return false;
		}

		// Only come back for scvk
		//
		// With scd3d11 installed it takes the DirectX slot and brings it back itself, by its
		// own marker, which only holds while the preferences still say Software. Without
		// Vulkan the slot goes to the game's own DirectX driver, which may be the one that
		// failed.
		if (GetModuleHandleA(SCD3D11_MODULE_NAME) != nullptr || !IsVulkanAvailable())
		{
			return false;
		}

		char path[MAX_PATH];
		if (!LogFilePath(MARKER_FILE_NAME, path, sizeof(path)))
		{
			return false;
		}

		// Stay when the last try failed
		//
		// A second try could fail the same way, and a start that fails every time would
		// never let the player into the game.
		if (ReadMarker(path, earlierFailure))
		{
			hasEarlierFailure = true;
			LogWarn("Software driver: the game stayed on it, since its last start with scvk failed. Run with -d:DirectX or -d:Vulkan to try scvk again.");
			return false;
		}

		cIGZGraphicSystem2Ptr graphicSystem;
		if (!graphicSystem)
		{
			return false;
		}

		// Write the guard before the try, since a try that fails may not get to write it
		if (!WriteMarker(path, ""))
		{
			LogWarn("Software driver: %s could not be written next to scvk.dll, so the game stays on it.", MARKER_FILE_NAME);
			return false;
		}

		// Ask for the DirectX slot, both ways the game takes it
		//
		// The class is what the graphics system picks a driver by, and the preference is
		// what the game reads it from, so this holds whether the game has read it yet or
		// not.
		graphicSystem->SetDefaultDriverClassID(DIRECTX_DRIVER_GZCLSID);
		video.driverType = DIRECTX_DRIVER_TYPE;

		LogInfo("Software driver: a failed start left the game on it, so the game is asked for scvk in its DirectX slot again.");
		return true;
	}

	void NoteStartFailure(char const* reason)
	{
		char path[MAX_PATH];
		if (LogFilePath(MARKER_FILE_NAME, path, sizeof(path)))
		{
			WriteMarker(path, reason);
		}
	}

	void NoteStarted(void)
	{
		char path[MAX_PATH];
		if (LogFilePath(MARKER_FILE_NAME, path, sizeof(path)) && DeleteFileA(path))
		{
			LogInfo("Software driver: scvk has started, so the failure noted in %s is forgotten.", MARKER_FILE_NAME);
		}
	}

	bool FindEarlierFailure(std::string& reason)
	{
		reason = earlierFailure;
		return hasEarlierFailure;
	}
}
