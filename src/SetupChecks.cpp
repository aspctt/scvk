/*
 * scvk - a native Vulkan renderer for SimCity 4
 *
 * Copyright (C) 2026 aspctt
 *
 * Game version detection follows the approach used by SC4Fix
 * (Copyright (c) 2015 Nelson Gomez, MIT License), including the sentinel byte
 * table used when the executable carries no version resource.
 *
 * The checks follow SCD3D11's Doctor (see Third Party Notices.txt), leaving out what
 * only concerns Direct3D 11, and the launch switch names come from it.
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

#include "SetupChecks.h"
#include "AccountRedaction.h"
#include "Benchmark.h"
#include "Logger.h"
#include "RendererChoice.h"
#include "SC4Version.h"
#include "Settings.h"

#include <GZServPtrs.h>
#include <cIGZCmdLine.h>
#include <cIGZFrameWork.h>
#include <cIGZGDriver.h>
#include <cIGZGraphicSystem2.h>
#include <cIGZMessage2.h>
#include <cIGZMessageServer2.h>
#include <cIGZMessageTarget2.h>
#include <cISC4App.h>
#include <cRZBaseString.h>
#include <cRZCOMDllDirector.h>

#include <windows.h>
#include <tlhelp32.h>
#include <algorithm>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <string>
#include <vector>

#if defined(_MSC_VER)
#pragma comment(lib, "version.lib")
#endif

namespace scvk
{
	//// Types

	namespace
	{
		/** The first three fields of a file version, which is what plugins release by. */
		struct PluginVersion
		{
			uint32_t major;
			uint32_t minor;
			uint32_t patch;
		};

		/** A DLL in the game's process. */
		struct LoadedModule
		{
			HMODULE     handle;
			std::string path;
			std::string fileName;
		};

		/** A launch switch as the game reads it: -Name, -Name:value, or -r<size> with no colon. */
		struct LaunchSwitch
		{
			std::string token;
			std::string name;
			std::string value;
		};

		/** A section of scvk.ini and the keys scvk reads from it. */
		struct SettingsSection
		{
			char const*        name;
			char const* const* keys;
			size_t             keyCount;
		};
	}

	//// Constants

	namespace
	{
		constexpr char const* RENDER_SERVICES_FILE_NAME = "SC4RenderServices.dll";
		constexpr char const* GRAPHICS_OPTIONS_FILE_NAME = "SC4GraphicsOptions.dll";
		constexpr char const* GRAPHICS_OPTIONS_SECTION = "GraphicsOptions";

		// The first version that sends the game's own stencil operation numbers. Older
		// ones send SCGL's, one lower, so scvk's stencil marks nothing and no terrain decal
		// is drawn back over the shadows.
		constexpr PluginVersion RENDER_SERVICES_MINIMUM = { 0, 8, 3 };

		// The signature every VS_FIXEDFILEINFO carries, from the Windows SDK
		// documentation.
		constexpr DWORD FIXED_FILE_INFORMATION_SIGNATURE = 0xfeef04bd;

		constexpr uint16_t SUPPORTED_GAME_VERSION = 641;

		constexpr uint32_t MESSAGE_POST_REGION_INIT = 0xCBB5BB45;
		constexpr uint32_t GZIID_MESSAGE_TARGET2    = 0x452294AA;

		// The game's notification dialog, the one sc4-dll-utilities calls too. It takes the
		// caption and the text and works in every window mode, where a Windows message box
		// can sit behind an exclusive fullscreen game. It starts with SUB ESP,0x28 and a
		// call, which is checked before it is called.
		constexpr uintptr_t GAME_DIALOG_ADDRESS_641 = 0x78DD80;
		constexpr uint8_t   GAME_DIALOG_START[]     = { 0x83, 0xEC, 0x28, 0xE8, 0x08, 0xB3, 0xE6, 0xFF };

		// Longer text is cut off, and no message comes near it.
		constexpr size_t MAXIMUM_MESSAGE_LENGTH = 1024;

		// The game's dialog cannot scroll, and in a test its eighth line was the last to show.
		// Seven lines of about 70 characters leaves room for the way it wraps at words.
		constexpr size_t DIALOG_LINE_COUNT = 7;
		constexpr size_t DIALOG_LINE_WIDTH = 70;

		// How many letters a name may differ by and still count as a typo, and how long it
		// has to be for that to mean anything: -w is one letter from -f.
		constexpr size_t TYPO_DISTANCE = 2;
		constexpr size_t TYPO_MINIMUM_LENGTH = 4;

		// Switches scvk reads. They are listed by hand so a typo can be told from a switch
		// of some other plugin; keep them level with Settings.cpp, FpsLimit.cpp, Logger.cpp,
		// VideoMode.cpp and the shadow modules.
		constexpr char const* SCVK_SWITCHES[] =
		{
			"VSync", "GPU", "ReShade", "ViewportCull", "LiveShadows", "NativeShadowMasks", "FlipModel", "DepthBuffer", "Depth16", "MaxFPS",
			"LogLevel", "DoctorDialog", "windowed", "fullscreen", "b", "borderless", "FullscreenMode",
			"LiveShadowDiag", "NativeShadowDiag", "LiveShadowAllFaces", "LiveShadowKeepPartial",
			"NativeShadowExperiment", "TerrainShadows", "ShadowRegistry",
		};

		// Switches the game itself and common plugins read, from SCD3D11's list.
		constexpr char const* OTHER_SWITCHES[] =
		{
			// SimCity 4
			"w", "f", "r", "d", "l", "gp", "Intro", "CustomResolution", "CustomCursors", "AutoZoomChange", "CPUPriority",
			"CPUCount", "UserDir", "Music", "Radio", "Sound", "Audio", "IME", "Verbose", "ExceptionHandling",
			"Restart", "AllowMultipleInstances", "Logging", "DebugLevel", "LuaScript", "ForceIdle",
			"CreateException", "IgnoreMissingModelDataBugs", "WriteLog", "NetCommandGenerator",

			// SC4CPUOptions and SC4DBPFLoading
			"ParallelCull", "SimTickCap", "SimTickStats", "ModelSlice", "OverlayIndex", "StartupDBPFLoadTrace",
		};

		// Switches only SCD3D11 reads. A launcher set up for it may well carry them.
		constexpr char const* SCD3D11_SWITCHES[] =
		{
			"NetworkShadowMasks", "DrawStats", "Doctor", "UpdateCheck", "VertexCacheSegments", "VertexCacheSize",
		};

		constexpr char const* SCVK_SETTINGS[]      = { "MaxFPS", "Use32BitDepth", "LogLevel", "DoctorDialog" };
		constexpr char const* BENCHMARK_SETTINGS[] = { "Enabled", "Renderer", "Region", "City", "SceneSeconds", "QuitWhenDone" };
		constexpr char const* ADMIN_SETTINGS[]     = { "Enabled" };

		constexpr SettingsSection SETTINGS_SECTIONS[] =
		{
			{ "scvk", SCVK_SETTINGS, ARRAYSIZE(SCVK_SETTINGS) },
			{ "Benchmark", BENCHMARK_SETTINGS, ARRAYSIZE(BENCHMARK_SETTINGS) },
			{ "Admin", ADMIN_SETTINGS, ARRAYSIZE(ADMIN_SETTINGS) },
		};

		// What a section can hold is well under this, comments included.
		constexpr DWORD SETTINGS_SECTION_SIZE = 8192;
	}

	//// State

	namespace
	{
		bool isListening = false;
		bool isDialogShown = false;

		// Another copy of scvk loaded before this one.
		bool isLaterCopy = false;

		// Another plugin's renderer won, so scvk's findings stay in its log.
		bool isAnotherRendererDrawing = false;

		/** The problems waiting for the dialog. A function's static, so it exists from the first report. */
		std::vector<std::string>& PendingProblems(void)
		{
			static std::vector<std::string> problems;
			return problems;
		}
	}

	//// Private Functions

	namespace
	{
		std::string ToLower(std::string text)
		{
			// tolower takes a character's value as an unsigned char, and a plain char
			// holding an accented letter from a folder name is negative.
			for (char& character : text)
			{
				character = static_cast<char>(tolower(static_cast<unsigned char>(character)));
			}

			return text;
		}

		std::string FormatMessageText(char const* format, ...)
		{
			char text[MAXIMUM_MESSAGE_LENGTH];

			va_list arguments;
			va_start(arguments, format);
			vsnprintf(text, sizeof(text), format, arguments);
			va_end(arguments);

			return text;
		}

		/** A path's file name, or the whole path when it has no folder. */
		std::string FileNameOf(std::string const& path)
		{
			size_t const separator = path.find_last_of("\\/");
			return (separator == std::string::npos) ? path : path.substr(separator + 1);
		}

		/** A path's folder in lower case. Empty when it has none. */
		std::string FolderOf(std::string const& path)
		{
			size_t const separator = path.find_last_of("\\/");
			return (separator == std::string::npos) ? std::string() : ToLower(path.substr(0, separator));
		}

		bool EndsWithSeparator(std::string const& path)
		{
			return !path.empty() && (path.back() == '\\' || path.back() == '/');
		}

		/** The module whose code or data holds an address, or null. */
		HMODULE FindModuleAt(void const* address)
		{
			// The call takes the address as if it were a name, which the flag tells it to
			// read as an address.
			HMODULE module = nullptr;
			DWORD const flags = GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT;
			GetModuleHandleExA(flags, static_cast<LPCSTR>(address), &module);
			return module;
		}

		std::string ReadModulePath(HMODULE module)
		{
			char path[MAX_PATH] = {};
			DWORD const length = GetModuleFileNameA(module, path, MAX_PATH);
			return (length == 0 || length == MAX_PATH) ? std::string() : std::string(path);
		}

		/** Reads a DLL's file version. False when it has no version resource. */
		bool ReadPluginVersion(char const* path, PluginVersion& outVersion)
		{
			// Read the version resource
			DWORD handle = 0;
			DWORD const size = GetFileVersionInfoSizeA(path, &handle);
			if (size == 0)
			{
				return false;
			}

			std::vector<uint8_t> versionData(size);
			if (!GetFileVersionInfoA(path, handle, size, versionData.data()))
			{
				return false;
			}

			// Find the fixed file information in it
			//
			// The query hands back a pointer into the resource through a void pointer out
			// parameter, so the typed pointer has to be passed as one.
			VS_FIXEDFILEINFO* fileInformation = nullptr;
			UINT fileInformationSize = 0;
			if (!VerQueryValueA(versionData.data(), "\\", reinterpret_cast<LPVOID*>(&fileInformation), &fileInformationSize) || fileInformation == nullptr)
			{
				return false;
			}

			if (fileInformation->dwSignature != FIXED_FILE_INFORMATION_SIGNATURE)
			{
				return false;
			}

			outVersion.major = HIWORD(fileInformation->dwFileVersionMS);
			outVersion.minor = LOWORD(fileInformation->dwFileVersionMS);
			outVersion.patch = HIWORD(fileInformation->dwFileVersionLS);
			return true;
		}

		bool IsOlderThan(PluginVersion const& version, PluginVersion const& minimum)
		{
			if (version.major != minimum.major)
			{
				return version.major < minimum.major;
			}

			if (version.minor != minimum.minor)
			{
				return version.minor < minimum.minor;
			}

			return version.patch < minimum.patch;
		}

		/** Every module in the process. */
		std::vector<LoadedModule> ListLoadedModules(void)
		{
			std::vector<LoadedModule> modules;

			// A snapshot is the one way to see two modules of the same name, since asking for
			// a module by name only ever finds the first.
			HANDLE const snapshot = CreateToolhelp32Snapshot(TH32CS_SNAPMODULE, GetCurrentProcessId());
			if (snapshot == INVALID_HANDLE_VALUE)
			{
				return modules;
			}

			MODULEENTRY32 entry{};
			entry.dwSize = sizeof(entry);

			for (BOOL hasEntry = Module32First(snapshot, &entry); hasEntry; hasEntry = Module32Next(snapshot, &entry))
			{
				modules.push_back({ entry.hModule, entry.szExePath, entry.szModule });
			}

			CloseHandle(snapshot);
			return modules;
		}

		void CheckRenderServices(char const* path)
		{
			PluginVersion version{};
			if (!ReadPluginVersion(path, version))
			{
				return;
			}

			// Leave builds from source alone
			//
			// SC4RenderServices only stamps its real version into release builds, and any
			// other build says 0.0.1 whatever its code is.
			if (version.major == 0 && version.minor == 0)
			{
				return;
			}

			if (IsOlderThan(version, RENDER_SERVICES_MINIMUM))
			{
				ReportSetupProblem("SC4RenderServices %u.%u.%u is older than %u.%u.%u, so terrain decals under shadows stay hidden. Update it.", version.major, version.minor, version.patch, RENDER_SERVICES_MINIMUM.major, RENDER_SERVICES_MINIMUM.minor, RENDER_SERVICES_MINIMUM.patch);
			}
		}

		/** Logs what is loaded with its version, and warns about anything loaded twice. */
		void CheckLoadedModules(HMODULE ownModule)
		{
			std::vector<LoadedModule> const modules = ListLoadedModules();

			// Leave out Windows' own files
			//
			// A plugin that stands in for a Windows DLL loads the original as well, which is
			// normal, and nobody asked about a driver's dozen helpers.
			char windowsFolder[MAX_PATH] = {};
			UINT const windowsFolderLength = GetWindowsDirectoryA(windowsFolder, MAX_PATH);
			std::string const windowsPrefix = ToLower(windowsFolder);

			std::vector<LoadedModule const*> outsideWindows;
			for (LoadedModule const& module : modules)
			{
				bool const isInWindows = windowsFolderLength != 0 && ToLower(module.path).rfind(windowsPrefix, 0) == 0;
				if (!isInWindows)
				{
					outsideWindows.push_back(&module);
				}
			}

			// Say what runs, so a report shows it without asking
			for (LoadedModule const* module : outsideWindows)
			{
				PluginVersion version{};
				if (ReadPluginVersion(module->path.c_str(), version))
				{
					LogInfo("Loaded: %s (%u.%u.%u)", module->path.c_str(), version.major, version.minor, version.patch);
				}
				else
				{
					LogInfo("Loaded: %s", module->path.c_str());
				}

				if (_stricmp(module->fileName.c_str(), RENDER_SERVICES_FILE_NAME) == 0)
				{
					CheckRenderServices(module->path.c_str());
				}
			}

			// Find anything loaded from two folders, usually the game's Plugins and the user's
			std::string const ownPath = ReadModulePath(ownModule);
			std::string const ownFileName = FileNameOf(ownPath);

			for (size_t first = 0; first < outsideWindows.size(); first++)
			{
				for (size_t second = first + 1; second < outsideWindows.size(); second++)
				{
					LoadedModule const& one = *outsideWindows[first];
					LoadedModule const& other = *outsideWindows[second];

					if (_stricmp(one.fileName.c_str(), other.fileName.c_str()) != 0 || FolderOf(one.path) == FolderOf(other.path))
					{
						continue;
					}

					// An old copy of scvk can win without a word, so this one is worth a dialog.
					// Every copy runs these checks and finds the same, so the first to load
					// shows it, or a player clicks through the same dialog twice.
					if (_stricmp(one.fileName.c_str(), ownFileName.c_str()) == 0)
					{
						ReportSetupProblem("Two copies of scvk are loaded, %s and %s. Delete one of them.", one.path.c_str(), other.path.c_str());
						isLaterCopy = isLaterCopy || other.handle == ownModule;
					}
					else
					{
						LogWarn("%s is loaded twice, from %s and %s. Remove one.", one.fileName.c_str(), one.path.c_str(), other.path.c_str());
					}
				}
			}
		}

		/** Says so when a driver other than scvk ended up drawing. */
		void CheckDriver(HMODULE ownModule)
		{
			// A benchmark of DirectX leaves the driver to the game on purpose
			if (IsBenchmarkOnDirectX())
			{
				return;
			}

			cIGZGraphicSystem2Ptr graphics;
			cIGZGDriver* const driver = graphics ? graphics->GetGDriver() : nullptr;
			if (driver == nullptr)
			{
				return;
			}

			// Find whose code it is
			//
			// An object starts with a pointer to its table of functions, which lives in the
			// module that made the class, so reading it as a pointer says which one that is.
			HMODULE const owner = FindModuleAt(*reinterpret_cast<void**>(driver));
			if (owner == ownModule)
			{
				return;
			}

			// Another renderer is a choice someone may have made, so a dialog would nag, and
			// the plugins scvk found a problem with are for that renderer's own checks
			if (owner != GetModuleHandleA(nullptr))
			{
				LogWarn("SimCity 4 is drawing with %s, not scvk. With SCD3D11 installed as well, Driver=OpenGL in SC4GraphicsOptions.ini or -d:Vulkan picks scvk.", ReadModulePath(owner).c_str());
				isAnotherRendererDrawing = true;
				return;
			}

			uint32_t const driverClass = driver->GetGZCLSID();
			if (driverClass == SOFTWARE_DRIVER_GZCLSID)
			{
				ReportSetupProblem("SimCity 4 is drawing with its own software driver, not scvk. -d:DirectX, or Driver=DirectX in SC4GraphicsOptions.ini, goes back.");
			}
			else if (driverClass == DIRECTX_DRIVER_GZCLSID)
			{
				ReportSetupProblem("SimCity 4 is drawing with its own DirectX driver, not scvk, because Vulkan could not start. The lines above in scvk.log say why.");
			}
			else
			{
				ReportSetupProblem("SimCity 4 is drawing with its own driver %08X, not scvk.", driverClass);
			}
		}

		/** The lower-case names of the DLLs a DLL imports, read without running anything. */
		std::vector<std::string> ReadImports(char const* path)
		{
			std::vector<std::string> names;

			// Map it as an image, which leaves what it imports alone
			HMODULE const image = LoadLibraryExA(path, nullptr, LOAD_LIBRARY_AS_IMAGE_RESOURCE);
			if (image == nullptr)
			{
				return names;
			}

			// Find the import table
			//
			// The low two bits of the handle mark it as a resource mapping, and the rest is
			// the address it is mapped at. The headers and the table are read at their offsets
			// from there, which takes a cast each.
			uint8_t const* const base = reinterpret_cast<uint8_t const*>(reinterpret_cast<uintptr_t>(image) & ~uintptr_t{ 3 });
			IMAGE_DOS_HEADER const* const dosHeader = reinterpret_cast<IMAGE_DOS_HEADER const*>(base);
			IMAGE_NT_HEADERS32 const* const headers = reinterpret_cast<IMAGE_NT_HEADERS32 const*>(base + dosHeader->e_lfanew);

			// A 64-bit DLL has other headers, and the game could not load it anyway
			if (headers->OptionalHeader.Magic == IMAGE_NT_OPTIONAL_HDR32_MAGIC)
			{
				IMAGE_DATA_DIRECTORY const& imports = headers->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_IMPORT];

				if (imports.VirtualAddress != 0)
				{
					for (IMAGE_IMPORT_DESCRIPTOR const* entry = reinterpret_cast<IMAGE_IMPORT_DESCRIPTOR const*>(base + imports.VirtualAddress); entry->Name != 0; entry++)
					{
						names.push_back(ToLower(reinterpret_cast<char const*>(base + entry->Name)));
					}
				}
			}

			FreeLibrary(image);
			return names;
		}

		/** The runtime DLLs a plugin built against the shared Visual C++ runtime imports. */
		bool IsVisualCppRuntime(std::string const& name)
		{
			return name.rfind("msvcp140", 0) == 0 || name.rfind("vcruntime140", 0) == 0 || name == "concrt140.dll";
		}

		/** The runtime DLLs a debug build imports instead, which no Redistributable has. */
		bool IsDebugVisualCppRuntime(std::string const& name)
		{
			return name == "ucrtbased.dll" || name == "msvcp140d.dll" || name == "vcruntime140d.dll" || name == "vcruntime140_1d.dll" || name == "msvcp140d_atomic_wait.dll" || name == "concrt140d.dll";
		}

		/** Whether the loader finds a DLL by name, searching where it would for a plugin's imports. */
		bool CanFindLibrary(std::string const& name)
		{
			if (GetModuleHandleA(name.c_str()) != nullptr)
			{
				return true;
			}

			HMODULE const library = LoadLibraryExA(name.c_str(), nullptr, LOAD_LIBRARY_AS_DATAFILE);
			if (library == nullptr)
			{
				return false;
			}

			FreeLibrary(library);
			return true;
		}

		/** The folders the game loads DLLs from. */
		std::vector<std::string> ListPluginFolders(void)
		{
			std::vector<std::string> folders;

			cISC4AppPtr app;
			if (!app)
			{
				return folders;
			}

			// The game loads the DLLs at the top of these two folders and no deeper (0x451260)
			cRZBaseString gameFolder;
			cRZBaseString userFolder;

			if (app->GetPluginDirectory(gameFolder))
			{
				folders.emplace_back(gameFolder.ToChar());
			}

			if (app->GetUserPluginDirectory(userFolder))
			{
				folders.emplace_back(userFolder.ToChar());
			}

			for (std::string& folder : folders)
			{
				if (!EndsWithSeparator(folder))
				{
					folder += '\\';
				}
			}

			// Both are one folder when -UserDir names the game's own
			if (folders.size() == 2 && _stricmp(folders[0].c_str(), folders[1].c_str()) == 0)
			{
				folders.pop_back();
			}

			return folders;
		}

		/** Looks through the plugin DLLs on disk, since these checks are about ones that fail to load. */
		void CheckPluginFiles(void)
		{
			std::string pluginsMissingRuntime;
			std::string missingRuntimes;
			std::string debugPlugins;
			std::string missingDebugRuntimes;

			for (std::string const& folder : ListPluginFolders())
			{
				WIN32_FIND_DATAA found{};
				HANDLE const search = FindFirstFileA((folder + "*.dll").c_str(), &found);
				if (search == INVALID_HANDLE_VALUE)
				{
					continue;
				}

				do
				{
					bool isMissingRuntime      = false;
					bool isMissingDebugRuntime = false;

					for (std::string const& import : ReadImports((folder + found.cFileName).c_str()))
					{
						// Plugins built for SC4RenderServices before 0.8 need the imgui.dll it no
						// longer ships, and an old one itself is reported by its version
						if (import == "imgui.dll" && _stricmp(found.cFileName, RENDER_SERVICES_FILE_NAME) != 0)
						{
							LogWarn("%s is built for SC4RenderServices before 0.8. It does not load, or cannot show its windows. Update it.", found.cFileName);
						}

						// The game skips a plugin Windows cannot load without a word
						//
						// A debug build's runtime starts like the normal one but comes from
						// nowhere a player can install it, so it goes first and gets its own
						// advice.
						if (IsDebugVisualCppRuntime(import))
						{
							if (!CanFindLibrary(import))
							{
								isMissingDebugRuntime = true;

								if (missingDebugRuntimes.find(import) == std::string::npos)
								{
									missingDebugRuntimes += (missingDebugRuntimes.empty() ? "" : ", ") + import;
								}
							}
						}
						else if (IsVisualCppRuntime(import) && !CanFindLibrary(import))
						{
							isMissingRuntime = true;

							if (missingRuntimes.find(import) == std::string::npos)
							{
								missingRuntimes += (missingRuntimes.empty() ? "" : ", ") + import;
							}
						}
					}

					if (isMissingRuntime)
					{
						pluginsMissingRuntime += (pluginsMissingRuntime.empty() ? "" : ", ") + std::string(found.cFileName);
					}

					if (isMissingDebugRuntime)
					{
						debugPlugins += (debugPlugins.empty() ? "" : ", ") + std::string(found.cFileName);
					}
				}
				while (FindNextFileA(search, &found));

				FindClose(search);
			}

			if (!pluginsMissingRuntime.empty())
			{
				ReportSetupProblem("SimCity 4 skips %s, because Windows cannot find %s from the Microsoft Visual C++ Redistributable. Install its x86 version from https://aka.ms/vs/17/release/vc_redist.x86.exe, also on 64-bit Windows.", pluginsMissingRuntime.c_str(), missingRuntimes.c_str());
			}

			if (!debugPlugins.empty())
			{
				ReportSetupProblem("SimCity 4 skips %s, a debug build that needs %s. Only Visual Studio installs those, the Visual C++ Redistributable does not have them. Ask the plugin's author for a release build.", debugPlugins.c_str(), missingDebugRuntimes.c_str());
			}
		}

		/** Whether a compatibility layer asks for an old version of Windows or an old display. */
		bool IsOldSystemLayer(std::string const& layer)
		{
			std::string const name = ToLower(layer);
			return name.rfind("win", 0) == 0 || name.rfind("vista", 0) == 0 || name.rfind("nt4", 0) == 0 || name == "256color" || name == "16bitcolor" || name == "640x480";
		}

		void CheckCompatibilityMode(void)
		{
			// Windows sets this for a program in compatibility mode
			char layers[512] = {};
			DWORD const length = GetEnvironmentVariableA("__COMPAT_LAYER", layers, sizeof(layers));
			if (length == 0 || length >= sizeof(layers))
			{
				return;
			}

			// Pick out the old system modes
			//
			// The rest of the list is settings such as RunAsAdmin or HighDpiAware, which do
			// not change what the game is told it runs on.
			std::string oldModes;
			char* context = nullptr;

			for (char* layer = strtok_s(layers, " ", &context); layer != nullptr; layer = strtok_s(nullptr, " ", &context))
			{
				if (IsOldSystemLayer(layer))
				{
					oldModes += (oldModes.empty() ? "" : " ") + std::string(layer);
				}
			}

			if (!oldModes.empty())
			{
				LogWarn("SimCity 4 runs in compatibility mode (%s), which scvk is not tested with. Turn it off under Properties, Compatibility of SimCity 4.exe, or on whatever started the game.", oldModes.c_str());
			}
		}

		void CheckGame(void)
		{
			// Read the flag the 4GB patch sets in the exe's header
			//
			// The module's handle is the address its headers are mapped at, which takes a
			// cast each to read.
			uint8_t const* const base = reinterpret_cast<uint8_t const*>(GetModuleHandleA(nullptr));
			IMAGE_DOS_HEADER const* const dosHeader = reinterpret_cast<IMAGE_DOS_HEADER const*>(base);
			IMAGE_NT_HEADERS32 const* const headers = reinterpret_cast<IMAGE_NT_HEADERS32 const*>(base + dosHeader->e_lfanew);

			if ((headers->FileHeader.Characteristics & IMAGE_FILE_LARGE_ADDRESS_AWARE) == 0)
			{
				ReportSetupProblem("SimCity 4.exe can only use 2 GB of memory. Large cities and big Plugins folders run out of it, and then the game crashes. Apply the 4GB patch to SimCity 4.exe.");
			}

			if (GetGameVersion() != SUPPORTED_GAME_VERSION)
			{
				ReportSetupProblem("This is not SimCity 4 version 1.1.%u. Live shadows and scvk's other changes to the game stay off. Update the game.", SUPPORTED_GAME_VERSION);
			}

			CheckCompatibilityMode();
		}

		/** How many single letter changes turn one text into the other. */
		size_t MeasureDistance(std::string const& one, std::string const& other)
		{
			std::vector<size_t> row(other.size() + 1);
			for (size_t column = 0; column < row.size(); column++)
			{
				row[column] = column;
			}

			for (size_t line = 1; line <= one.size(); line++)
			{
				size_t diagonal = row[0];
				row[0] = line;

				for (size_t column = 1; column <= other.size(); column++)
				{
					size_t const above = row[column];
					size_t const change = diagonal + (one[line - 1] == other[column - 1] ? 0 : 1);

					row[column] = (std::min)({ above + 1, row[column - 1] + 1, change });
					diagonal = above;
				}
			}

			return row[other.size()];
		}

		template <size_t COUNT>
		bool IsListed(char const* const (&names)[COUNT], std::string const& name)
		{
			for (char const* const listed : names)
			{
				if (_stricmp(listed, name.c_str()) == 0)
				{
					return true;
				}
			}

			return false;
		}

		/** The entry a name looks like a typo of, or null. */
		char const* FindClosestName(char const* const* names, size_t count, std::string const& name)
		{
			if (name.size() < TYPO_MINIMUM_LENGTH)
			{
				return nullptr;
			}

			std::string const wanted = ToLower(name);

			for (size_t i = 0; i < count; i++)
			{
				std::string const listed = ToLower(names[i]);
				if (listed.size() >= TYPO_MINIMUM_LENGTH && MeasureDistance(wanted, listed) <= TYPO_DISTANCE)
				{
					return names[i];
				}
			}

			return nullptr;
		}

		/** Splits an argument into a switch. False when it does not start with a dash. */
		bool ParseSwitch(std::string const& token, LaunchSwitch& outSwitch)
		{
			if (token.size() < 2 || token[0] != '-')
			{
				return false;
			}

			outSwitch.token = token;

			// -r1920x1080x32, the one switch with no colon before its value
			std::string const body = token.substr(1);
			if ((body[0] == 'r' || body[0] == 'R') && body.size() > 1 && isdigit(static_cast<unsigned char>(body[1])))
			{
				outSwitch.name  = "r";
				outSwitch.value = body.substr(1);
				return true;
			}

			size_t const colon = body.find(':');
			outSwitch.name  = body.substr(0, colon);
			outSwitch.value = (colon == std::string::npos) ? std::string() : body.substr(colon + 1);
			return true;
		}

		/** A size written without its r reads as a switch nothing knows. */
		bool LooksLikeSize(std::string const& name)
		{
			unsigned int width  = 0;
			unsigned int height = 0;
			return sscanf_s(name.c_str(), "%ux%u", &width, &height) == 2;
		}

		/** Warns about one switch that nothing will read, and adds any unknown one to the list. */
		void CheckSwitchName(LaunchSwitch const& entry, std::string& unknown)
		{
			// -VSync=off reads as one long name
			size_t const equals = entry.name.find('=');
			if (equals != std::string::npos)
			{
				LogWarn("%s is not read. Use a colon instead: -%s:%s", entry.token.c_str(), entry.name.substr(0, equals).c_str(), entry.name.substr(equals + 1).c_str());
				return;
			}

			if (_stricmp(entry.name.c_str(), "r") == 0)
			{
				// The game reads the value straight after the letter
				if (entry.token.size() > 2 && entry.token[2] == ':')
				{
					LogWarn("The game ignores %s. Write it without the colon: -r%s", entry.token.c_str(), entry.value.c_str());
				}

				return;
			}

			if (entry.value.empty() && LooksLikeSize(entry.name))
			{
				LogWarn("The game ignores %s. A size needs an r in front: -r%s", entry.token.c_str(), entry.name.c_str());
				return;
			}

			if (IsListed(SCD3D11_SWITCHES, entry.name))
			{
				LogInfo("%s is an SCD3D11 switch, which scvk does not read.", entry.token.c_str());
				return;
			}

			if (IsListed(SCVK_SWITCHES, entry.name) || IsListed(OTHER_SWITCHES, entry.name))
			{
				return;
			}

			// Look for what it was meant to be, scvk's own switches first
			char const* closest = FindClosestName(SCVK_SWITCHES, ARRAYSIZE(SCVK_SWITCHES), entry.name);
			if (closest == nullptr)
			{
				closest = FindClosestName(OTHER_SWITCHES, ARRAYSIZE(OTHER_SWITCHES), entry.name);
			}

			if (closest != nullptr)
			{
				LogWarn("%s is not a switch anything reads. Did you mean -%s?", entry.token.c_str(), closest);
				return;
			}

			unknown += (unknown.empty() ? "" : ", ") + entry.token;
		}

		/** Warns when -UserDir names a folder that is not there. */
		void CheckUserFolder(LaunchSwitch const& entry)
		{
			if (_stricmp(entry.name.c_str(), "UserDir") != 0 || entry.value.empty())
			{
				return;
			}

			// The game keeps any quotes in the value
			std::string folder = entry.value;
			folder.erase(std::remove(folder.begin(), folder.end(), '"'), folder.end());

			if (GetFileAttributesA(folder.c_str()) == INVALID_FILE_ATTRIBUTES)
			{
				LogWarn("The -UserDir folder %s does not exist, so the game cannot load the plugins in it.", folder.c_str());
			}
		}

		void CheckCommandLine(void)
		{
			cIGZFrameWork* const frameWork = RZGetFrameWork();
			cIGZCmdLine* const commandLine = (frameWork != nullptr) ? frameWork->CommandLine() : nullptr;
			if (commandLine == nullptr)
			{
				return;
			}

			// Split the arguments the way the game did, which is what a stray word shows
			std::vector<LaunchSwitch> switches;
			std::string strays;

			for (uint32_t i = 1; i < commandLine->argc(); i++)
			{
				// The interface numbers its arguments with a signed index, and a command line
				// holds far fewer than that reaches.
				char const* const argument = commandLine->argv(static_cast<int32_t>(i));
				if (argument == nullptr || argument[0] == '\0')
				{
					continue;
				}

				LaunchSwitch entry;
				if (ParseSwitch(argument, entry))
				{
					switches.push_back(entry);
				}
				else
				{
					strays += (strays.empty() ? "" : ", ") + std::string(argument);
				}
			}

			if (!strays.empty())
			{
				LogWarn("Not a switch on the command line: %s. A -UserDir path with spaces must be in quotes, such as -UserDir:\"D:\\My Games\\SimCity 4\\\".", strays.c_str());
			}

			// A shortcut's target is cut off, which can leave half a quoted path behind
			char const* const rawCommandLine = GetCommandLineA();
			std::string const raw = (rawCommandLine != nullptr) ? rawCommandLine : "";

			if (std::count(raw.begin(), raw.end(), '"') % 2 != 0)
			{
				LogWarn("The command line has a quote that is never closed. A shortcut cuts its target off after 259 characters, so check that nothing at the end is missing.");
			}

			// Go through the switches
			std::string unknown;

			for (size_t i = 0; i < switches.size(); i++)
			{
				CheckSwitchName(switches[i], unknown);
				CheckUserFolder(switches[i]);

				for (size_t later = i + 1; later < switches.size(); later++)
				{
					bool const isSameName = _stricmp(switches[i].name.c_str(), switches[later].name.c_str()) == 0;
					if (isSameName && _stricmp(switches[i].value.c_str(), switches[later].value.c_str()) != 0)
					{
						LogWarn("%s and %s are both on the command line. Keep one.", switches[i].token.c_str(), switches[later].token.c_str());
					}
				}
			}

			if (!unknown.empty())
			{
				LogInfo("Switches scvk does not know, fine if another plugin reads them: %s", unknown.c_str());
			}
		}

		/** Warns about keys in one section of scvk.ini that nothing reads. */
		void CheckSettingsSection(char const* path, SettingsSection const& section)
		{
			// List the section
			//
			// The buffer comes back as one null-terminated line after another, ending with
			// an empty one.
			std::vector<char> lines(SETTINGS_SECTION_SIZE);
			if (GetPrivateProfileSectionA(section.name, lines.data(), SETTINGS_SECTION_SIZE, path) == 0)
			{
				return;
			}

			for (char const* line = lines.data(); *line != '\0'; line += strlen(line) + 1)
			{
				if (*line == ';' || *line == '#')
				{
					continue;
				}

				std::string key = line;
				key = key.substr(0, key.find('='));

				while (!key.empty() && (key.back() == ' ' || key.back() == '\t'))
				{
					key.pop_back();
				}

				bool isKnown = false;
				for (size_t i = 0; i < section.keyCount; i++)
				{
					isKnown = isKnown || _stricmp(section.keys[i], key.c_str()) == 0;
				}

				if (isKnown)
				{
					continue;
				}

				char const* const closest = FindClosestName(section.keys, section.keyCount, key);
				if (closest != nullptr)
				{
					LogWarn("scvk.ini has %s, which is not a setting. Did you mean %s?", key.c_str(), closest);
				}
				else
				{
					LogWarn("scvk.ini has %s under [%s], which is not a setting scvk reads.", key.c_str(), section.name);
				}
			}
		}

		void CheckSettingsFile(void)
		{
			char path[MAX_PATH];
			if (!LogFilePath("scvk.ini", path, sizeof(path)) || GetFileAttributesA(path) == INVALID_FILE_ATTRIBUTES)
			{
				return;
			}

			for (SettingsSection const& section : SETTINGS_SECTIONS)
			{
				CheckSettingsSection(path, section);
			}
		}

		/** What boost reads as a bool, which is what SC4GraphicsOptions parses its ini with. */
		bool IsGraphicsOptionsBoolean(char const* value)
		{
			return strcmp(value, "true") == 0 || strcmp(value, "false") == 0 || strcmp(value, "1") == 0 || strcmp(value, "0") == 0;
		}

		/** Finds the first line SC4GraphicsOptions.ini cannot do without that is missing or unreadable. */
		void CheckGraphicsOptions(void)
		{
			HMODULE const module = GetModuleHandleA(GRAPHICS_OPTIONS_FILE_NAME);
			if (module == nullptr)
			{
				return;
			}

			std::string path = ReadModulePath(module);
			size_t const extension = path.find_last_of('.');
			if (extension == std::string::npos)
			{
				return;
			}

			path = path.substr(0, extension) + ".ini";

			// Read the keys in the plugin's order
			//
			// It stops at the first one that fails and keeps its defaults for the rest, a
			// 1024x768 window among them. The size is not read for borderless.
			struct RequiredKey
			{
				char const* name;
				bool        isBoolean;
				bool        isWindowSize;
			};

			constexpr RequiredKey REQUIRED_KEYS[] =
			{
				{ "EnableIntroVideo", true, false },
				{ "PauseGameOnFocusLoss", true, false },
				{ "Driver", false, false },
				{ "WindowMode", false, false },
				{ "ColorDepth", false, false },
				{ "WindowWidth", false, true },
				{ "WindowHeight", false, true },
			};

			char windowMode[32] = {};
			GetPrivateProfileStringA(GRAPHICS_OPTIONS_SECTION, "WindowMode", "", windowMode, sizeof(windowMode), path.c_str());
			bool const isBorderless = _strnicmp(windowMode, "Borderless", strlen("Borderless")) == 0;

			for (RequiredKey const& key : REQUIRED_KEYS)
			{
				if (key.isWindowSize && isBorderless)
				{
					continue;
				}

				char value[64] = {};
				GetPrivateProfileStringA(GRAPHICS_OPTIONS_SECTION, key.name, "", value, sizeof(value), path.c_str());

				if (value[0] == '\0')
				{
					ReportSetupProblem("SC4GraphicsOptions.ini has no %s. The plugin stops reading there and uses its defaults for the rest, which can be a 1024x768 window.", key.name);
					return;
				}

				if (key.isBoolean && !IsGraphicsOptionsBoolean(value))
				{
					ReportSetupProblem("SC4GraphicsOptions.ini cannot read %s=%s. Use true or false. The plugin stops reading there and uses its defaults for the rest, which can be a 1024x768 window.", key.name, value);
					return;
				}
			}
		}

		void ShowDialog(std::string const& text)
		{
			// Use the game's own dialog where it is known to be
			//
			// The address is a plain number, which has to become a pointer to be read and
			// then a function pointer to be called.
			uint8_t const* const dialogStart = reinterpret_cast<uint8_t const*>(GAME_DIALOG_ADDRESS_641);
			if (GetGameVersion() == SUPPORTED_GAME_VERSION && memcmp(dialogStart, GAME_DIALOG_START, sizeof(GAME_DIALOG_START)) == 0)
			{
				using GameDialog = bool (__cdecl*)(cIGZString const& caption, cIGZString const& message);
				GameDialog const showDialog = reinterpret_cast<GameDialog>(GAME_DIALOG_ADDRESS_641);

				showDialog(cRZBaseString("scvk"), cRZBaseString(text.c_str()));
				return;
			}

			MessageBoxA(nullptr, text.c_str(), "scvk", MB_OK | MB_ICONWARNING | MB_SETFOREGROUND);
		}

		/** Shows the waiting problems once the region view is up. Static, so its count is only for show. */
		class ProblemDialog final : public cIGZMessageTarget2
		{
		public:
			bool QueryInterface(uint32_t riid, void** ppvObj) override
			{
				if (riid == GZIID_MESSAGE_TARGET2 || riid == GZIID_cIGZUnknown)
				{
					*ppvObj = static_cast<cIGZMessageTarget2*>(this);
					AddRef();
					return true;
				}

				*ppvObj = nullptr;
				return false;
			}

			uint32_t AddRef(void) override
			{
				return ++referenceCount;
			}

			uint32_t Release(void) override
			{
				if (referenceCount > 0)
				{
					--referenceCount;
				}

				return referenceCount;
			}

			bool DoMessage(cIGZMessage2* message) override
			{
				// The game sends this for every region it opens, and once is enough
				if (message->GetType() != MESSAGE_POST_REGION_INIT || isDialogShown)
				{
					return true;
				}

				isDialogShown = true;

				std::vector<std::string> const& problems = PendingProblems();
				if (problems.empty())
				{
					return true;
				}

				// Fill the dialog
				//
				// The way to the log comes first, where it cannot run off the bottom, and whole
				// problems follow while they and the line counting the rest still fit. The first
				// always goes in, cut off or not, since an empty dialog would help nobody.
				std::string text = "Details in scvk.log. DoctorDialog=false in scvk.ini hides this.\n";
				size_t lineCount = 2;
				size_t shownCount = 0;

				for (std::string const& problem : problems)
				{
					lineCount += (problem.size() + DIALOG_LINE_WIDTH - 1) / DIALOG_LINE_WIDTH;
					if (shownCount > 0 && lineCount > DIALOG_LINE_COUNT)
					{
						break;
					}

					text += problem + "\n";
					shownCount++;
				}

				if (shownCount < problems.size())
				{
					text += FormatMessageText("And %zu more in scvk.log.", problems.size() - shownCount);
				}

				ShowDialog(text);
				return true;
			}

		private:
			uint32_t referenceCount = 0;
		};

		ProblemDialog problemDialog;
	}

	//// Public API

	void RunSetupChecks(void)
	{
		HMODULE const ownModule = FindModuleAt(reinterpret_cast<void const*>(&RunSetupChecks));
		if (ownModule == nullptr)
		{
			return;
		}

		CheckDriver(ownModule);
		CheckLoadedModules(ownModule);
		CheckPluginFiles();
		CheckGame();
		CheckCommandLine();
		CheckSettingsFile();
		CheckGraphicsOptions();

		// Wait for the region view, where the game can show a dialog and a player can read it
		//
		// A dialog would hold up a benchmark, which nobody is watching, and a player using
		// another renderer did not ask scvk for its opinion.
		cIGZMessageServer2Ptr messageServer;
		if (messageServer && GetSettings().isDoctorDialogEnabled && !IsBenchmarkEnabled() && !isLaterCopy && !isAnotherRendererDrawing && !isListening)
		{
			messageServer->AddNotification(&problemDialog, MESSAGE_POST_REGION_INIT);
			isListening = true;
		}
	}

	void StopSetupChecks(void)
	{
		if (!isListening)
		{
			return;
		}

		cIGZMessageServer2Ptr messageServer;
		if (messageServer)
		{
			messageServer->RemoveNotification(&problemDialog, MESSAGE_POST_REGION_INIT);
		}

		isListening = false;
	}

	void ReportSetupProblem(char const* format, ...)
	{
		char formatted[MAXIMUM_MESSAGE_LENGTH];

		va_list arguments;
		va_start(arguments, format);
		vsnprintf(formatted, sizeof(formatted), format, arguments);
		va_end(arguments);

		// Take the Windows account out of its paths here rather than only in the log,
		// since players post screenshots of the dialog as well
		std::string const text = RedactAccount(formatted, GetCurrentAccount());

		// The game sets up a driver for every slot, so the renderer finds the same thing
		// more than once
		std::vector<std::string>& problems = PendingProblems();
		if (std::find(problems.begin(), problems.end(), text) != problems.end())
		{
			return;
		}

		problems.push_back(text);
		LogError("%s", text.c_str());
	}
}
