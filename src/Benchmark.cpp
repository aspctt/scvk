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

#include "Benchmark.h"
#include "Logger.h"
#include "SC4Version.h"
#include "version.h"

#include <GZServPtrs.h>
#include <SC4String.h>
#include <cIGZFrameWork.h>
#include <cIGZGDriver.h>
#include <cIGZGraphicSystem2.h>
#include <cIGZMessage2.h>
#include <cIGZMessageServer2.h>
#include <cIGZMessageTarget2.h>
#include <cIGZSystemService.h>
#include <cIGZWin.h>
#include <cISC4App.h>
#include <cISC4Nation.h>
#include <cISC4Region.h>
#include <cISC4RegionalCity.h>
#include <cISC4Simulator.h>
#include <cISC4View3DWin.h>
#include <cRZAutoRefCount.h>
#include <cRZCOMDllDirector.h>

#include <Windows.h>
#include <algorithm>
#include <functional>
#include <iterator>
#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <vector>

namespace scvk
{
	//// Types

	namespace
	{
		/** What the camera does while a scene is timed. */
		enum class Motion : uint32_t
		{
			STILL,
			SCROLL,
			ROTATE,
			ZOOM,
		};

		/**
		 * One timed stretch of a run. Zoom levels run from 1, the farthest, to 5, the
		 * closest; the view clamps anything else to its limits (0x7e6380).
		 *
		 * A scrolling scene goes right and back left in legs of equal length, an even
		 * number of them, so it ends where it started. Wider zooms cover more ground in a
		 * second of scrolling, so they take shorter legs to stay over the city: in the first
		 * runs, with two legs a scene, zoom 3 covered about 800 world units a leg while
		 * zooms 2 and 1 ran off the city's edge.
		 */
		struct Scene
		{
			char const* name;
			int32_t     zoom;
			int32_t     otherZoom;
			int32_t     rotation;
			Motion      motion;
			uint32_t    scrollLegs;
		};

		/** Where a run is. */
		enum class Phase : uint32_t
		{
			WAITING_FOR_REGION,
			LOADING_CITY,
			SETTLING,
			WARMING_UP,
			RECORDING,
			FINISHED,
		};

		/** The moments a run times, each measured from the process's creation. */
		enum Milestone : uint32_t
		{
			MILESTONE_PLUGIN_STARTED,
			MILESTONE_APP_STARTED,
			MILESTONE_REGION_LOADING,
			MILESTONE_REGION_LOADED,
			MILESTONE_REGION_SHOWN,
			MILESTONE_REGION_SWITCHING,
			MILESTONE_CITY_REQUESTED,
			MILESTONE_CITY_LOADING,
			MILESTONE_CITY_LOADED,
			MILESTONE_CITY_COMPLETE,
			MILESTONE_CITY_SHOWN,
			MILESTONE_SCENES_STARTED,
			MILESTONE_SCENES_FINISHED,
			MILESTONE_COUNT,
		};

		struct Settings
		{
			bool     isEnabled;
			bool     isDirectX;
			bool     shouldQuitWhenDone;
			uint32_t sceneSeconds;
			char     region[MAX_PATH];
			char     city[MAX_PATH];
		};

		/** The ground under the middle of the window, in world coordinates. */
		struct TerrainPoint
		{
			bool  isValid;
			float x;
			float y;
			float z;
		};

		struct SceneRecord
		{
			size_t       sceneIndex;
			size_t       firstFrame;
			size_t       frameCount;
			TerrainPoint start;
			TerrainPoint middle;
			TerrainPoint end;
			float        scrollSpeed;
		};

		struct FrameStatistics
		{
			size_t frames;
			double seconds;
			double averageFps;
			double onePercentLowFps;
			double pointOnePercentLowFps;
			double slowestMilliseconds;
		};

		// cIGZGDriver::Flush as the game calls it: thiscall, with the driver in ECX and
		// nothing on the stack. A fastcall function takes ECX and EDX and also leaves the
		// stack alone, so it can stand in for the method and call through to it.
		using FlushFunction = void(__fastcall*)(cIGZGDriver* driver, void* unusedEdx);
	}

	//// Constants

	namespace
	{
		// The message IDs, from the game's own table that pairs each message's ID with its
		// name (0xb08678 for the region's, 0xb082a8 for the city's).
		constexpr uint32_t MESSAGE_PRE_REGION_INIT         = 0xABB5BB44;
		constexpr uint32_t MESSAGE_POST_REGION_INIT        = 0xCBB5BB45;
		constexpr uint32_t MESSAGE_PRE_CITY_INIT           = 0x26D31EC0;
		constexpr uint32_t MESSAGE_POST_CITY_INIT          = 0x26D31EC1;
		constexpr uint32_t MESSAGE_POST_CITY_INIT_COMPLETE = 0xEA8AE29A;

		constexpr uint32_t LISTENED_MESSAGES[] = {
			MESSAGE_PRE_REGION_INIT,
			MESSAGE_POST_REGION_INIT,
			MESSAGE_PRE_CITY_INIT,
			MESSAGE_POST_CITY_INIT,
			MESSAGE_POST_CITY_INIT_COMPLETE,
		};

		// The interface message targets answer to, as the game's own do (0x497220).
		constexpr uint32_t GZIID_MESSAGE_TARGET2 = 0x452294AA;

		// The tick service's ID, which only has to differ from every other service's.
		constexpr uint32_t BENCHMARK_SERVICE_ID = 0x5C4B0002;

		// cIGZGDriver::Flush, the frame boundary for both renderers: the game's DirectX
		// driver ends its scene and flips there (0x884ba0), and scvk submits and presents.
		// Its place in the interface's table, which starts with cIGZUnknown's three.
		constexpr size_t FLUSH_SLOT = 0xAC / sizeof(void*);

		// The windows the 3D view sits in, as gzcom-dll's SC4UI finds it.
		constexpr uint32_t WINDOW_ID_SC4_APP = 0x6104489A;
		constexpr uint32_t WINDOW_ID_VIEW_3D = 0x9A47B417;

		// How far the city search reaches, in region tiles each way. The region looks a
		// city up by its corner tile in a map (0x4a4930), so an empty tile costs one failed
		// lookup and nothing else.
		constexpr uint32_t REGION_SEARCH_TILES = 1024;

		// How long a run waits at each step, in seconds: the region on screen before the
		// city is opened, the city on screen and paused before the first scene, each
		// scene's camera before its timing starts, and between rotations or zoom changes.
		constexpr double REGION_WAIT_SECONDS     = 3.0;
		constexpr double SETTLE_SECONDS          = 10.0;
		constexpr double WARMUP_SECONDS          = 2.0;
		constexpr double MOTION_INTERVAL_SECONDS = 1.0;

		// SetScrolling takes a direction and a speed, whatever gzcom-dll's parameter names
		// say: the view's keyboard handler (0x7e6c60) hands it an angle in radians from a
		// table of eighth turns, 0 for the right arrow and pi for the left, and the speed
		// worked out below. Scrolling scenes go right, then back left.
		constexpr float SCROLL_ANGLE_OUT  = 0.0f;
		constexpr float SCROLL_ANGLE_BACK = 3.14159265f;

		// The arrow keys' speed, as that handler works it out: the speed the view read
		// from its properties (0x7ef8a5) times 1.2 raised to a whole number its camera
		// control keeps. These are the view's own fields, so they are only read on the
		// build they came from.
		constexpr uint16_t  SCROLL_GAME_VERSION          = 641;
		constexpr ptrdiff_t VIEW_SCROLL_SPEED_OFFSET     = 0x340;
		constexpr ptrdiff_t VIEW_CAMERA_CONTROL_OFFSET   = 0xF0;
		constexpr ptrdiff_t CAMERA_SPEED_EXPONENT_OFFSET = 0x108;
		constexpr double    SCROLL_SPEED_BASE            = 1.2;

		constexpr uint32_t DEFAULT_SCENE_SECONDS = 20;
		constexpr uint32_t LONGEST_SCENE_SECONDS = 600;

		// Room for the frame times of a whole run up front, so the frame clock never waits
		// on the vector growing.
		constexpr size_t FRAMES_RESERVED = size_t{ 1 } << 20;

		constexpr Scene SCENES[] = {
			{ "close-still",     5, 5, 0, Motion::STILL,  0  },
			{ "close-scroll",    5, 5, 0, Motion::SCROLL, 2  },
			{ "near-scroll",     4, 4, 0, Motion::SCROLL, 2  },
			{ "middle-scroll",   3, 3, 0, Motion::SCROLL, 4  },
			{ "far-scroll",      2, 2, 0, Motion::SCROLL, 8  },
			{ "farthest-scroll", 1, 1, 0, Motion::SCROLL, 16 },
			{ "rotate",          3, 3, 0, Motion::ROTATE, 0  },
			{ "zoom",            2, 4, 0, Motion::ZOOM,   0  },
		};

		constexpr char const* MILESTONE_NAMES[MILESTONE_COUNT] = {
			"plugin-started",
			"app-started",
			"region-loading",
			"region-loaded",
			"region-shown",
			"region-switching",
			"city-requested",
			"city-loading",
			"city-loaded",
			"city-complete",
			"city-shown",
			"scenes-started",
			"scenes-finished",
		};

		constexpr char const* MOTION_NAMES[] = {
			"still",
			"scroll",
			"rotate",
			"zoom",
		};

		constexpr char const* RESULTS_FILE_NAME         = "scvk-benchmark.csv";
		constexpr char const* PARTIAL_RESULTS_FILE_NAME = "scvk-benchmark.csv.partial";
	}

	//// References and State

	namespace
	{
		struct BenchmarkState
		{
			Phase    phase             = Phase::WAITING_FOR_REGION;
			bool     isStarted         = false;
			bool     isWritten         = false;
			bool     isLoadingCity     = false;
			bool     hasMiddlePoint    = false;
			int64_t  processStartTicks = 0;
			int64_t  phaseStartTicks   = 0;
			int64_t  lastFrameTicks    = 0;
			int64_t  lastMotionTicks   = 0;
			uint64_t framesPresented   = 0;
			int64_t  regionLoadedTicks = 0;
			int64_t  regionShownTicks  = 0;
			float    scrollSpeed       = 0.0f;
			size_t   sceneIndex        = 0;
			uint32_t motionStep        = 0;

			bool   hasReached[MILESTONE_COUNT] = {};
			double milestones[MILESTONE_COUNT] = {};

			std::vector<float>       frameMilliseconds;
			std::vector<SceneRecord> sceneRecords;

			void**        flushSlot     = nullptr;
			FlushFunction originalFlush = nullptr;

			cISC4View3DWin* view   = nullptr;
			HWND            window = nullptr;

			char status[128] = "incomplete";
		};

		BenchmarkState state;
	}

	//// Private Functions

	namespace
	{
		/** Whether a true or false setting in the [Benchmark] section is true. */
		bool IsSettingTrue(char const* settingsPath, char const* key, bool isTrueByDefault)
		{
			char value[16] = {};
			GetPrivateProfileStringA("Benchmark", key, isTrueByDefault ? "true" : "false", value, sizeof(value), settingsPath);

			return _stricmp(value, "true") == 0 || strcmp(value, "1") == 0;
		}

		Settings ReadSettings(void)
		{
			Settings settings           = {};
			settings.sceneSeconds       = DEFAULT_SCENE_SECONDS;
			settings.shouldQuitWhenDone = true;

			char settingsPath[MAX_PATH] = {};
			if (!LogFilePath("scvk.ini", settingsPath, sizeof(settingsPath)))
			{
				return settings;
			}

			// Read the section
			char renderer[32] = {};
			GetPrivateProfileStringA("Benchmark", "Renderer", "scvk", renderer, sizeof(renderer), settingsPath);
			GetPrivateProfileStringA("Benchmark", "Region", "", settings.region, sizeof(settings.region), settingsPath);
			GetPrivateProfileStringA("Benchmark", "City", "", settings.city, sizeof(settings.city), settingsPath);

			UINT const sceneSeconds = GetPrivateProfileIntA("Benchmark", "SceneSeconds", DEFAULT_SCENE_SECONDS, settingsPath);

			settings.isEnabled          = IsSettingTrue(settingsPath, "Enabled", false);
			settings.isDirectX          = _stricmp(renderer, "DirectX") == 0;
			settings.shouldQuitWhenDone = IsSettingTrue(settingsPath, "QuitWhenDone", true);
			settings.sceneSeconds       = std::clamp<uint32_t>(sceneSeconds, 1, LONGEST_SCENE_SECONDS);
			return settings;
		}

		/** The [Benchmark] settings, read on first use and kept for the process. */
		Settings const& GetSettings(void)
		{
			static Settings const settings = ReadSettings();
			return settings;
		}

		int64_t Now(void)
		{
			LARGE_INTEGER counter = {};
			QueryPerformanceCounter(&counter);
			return counter.QuadPart;
		}

		double ReadTicksPerSecond(void)
		{
			LARGE_INTEGER frequency = {};
			QueryPerformanceFrequency(&frequency);

			// The counter's rate is a whole number of ticks a second, far inside a double's
			// exact range, so the conversion loses nothing.
			return static_cast<double>(frequency.QuadPart);
		}

		double TicksPerSecond(void)
		{
			static double const ticksPerSecond = ReadTicksPerSecond();
			return ticksPerSecond;
		}

		double MillisecondsBetween(int64_t from, int64_t to)
		{
			// A run is minutes long, so the tick count is far inside a double's exact range.
			return static_cast<double>(to - from) * 1000.0 / TicksPerSecond();
		}

		/** The performance counter's reading at the moment the process was created. */
		int64_t ProcessStartTicks(void)
		{
			// Read both clocks as close together as possible
			int64_t const nowTicks = Now();

			FILETIME now = {};
			GetSystemTimePreciseAsFileTime(&now);

			FILETIME creation   = {};
			FILETIME exitTime   = {};
			FILETIME kernelTime = {};
			FILETIME userTime   = {};
			if (!GetProcessTimes(GetCurrentProcess(), &creation, &exitTime, &kernelTime, &userTime))
			{
				return nowTicks;
			}

			// Step back by the time since creation
			//
			// File times count 100 ns intervals. The difference is seconds to minutes, far
			// inside a double's exact range, and the result is whole ticks again.
			uint64_t const created = (uint64_t{ creation.dwHighDateTime } << 32) | creation.dwLowDateTime;
			uint64_t const current = (uint64_t{ now.dwHighDateTime } << 32) | now.dwLowDateTime;

			double const secondsSinceCreation = static_cast<double>(current - created) / 10000000.0;
			return nowTicks - static_cast<int64_t>(secondsSinceCreation * TicksPerSecond());
		}

		bool HasReached(Milestone milestone)
		{
			return state.hasReached[milestone];
		}

		/** Notes a milestone the first time it is reached. Later calls change nothing. */
		void Mark(Milestone milestone)
		{
			if (HasReached(milestone))
			{
				return;
			}

			state.hasReached[milestone] = true;
			state.milestones[milestone] = MillisecondsBetween(state.processStartTicks, Now());
			LogInfo("Benchmark: %s at %.0f ms.", MILESTONE_NAMES[milestone], state.milestones[milestone]);
		}

		void OnFramePresented(void)
		{
			int64_t const now = Now();

			// Time the frame while a scene is recorded
			if (state.phase == Phase::RECORDING && state.lastFrameTicks != 0)
			{
				// A frame time is a few milliseconds, which a float holds to well under a
				// microsecond.
				state.frameMilliseconds.push_back(static_cast<float>(MillisecondsBetween(state.lastFrameTicks, now)));
			}

			state.lastFrameTicks = now;
			state.framesPresented++;

			// Note the first frame shown after each load
			//
			// The city's waits for the load to return, since the game draws its loading
			// screen while the city is still being built.
			if (HasReached(MILESTONE_REGION_LOADED))
			{
				Mark(MILESTONE_REGION_SHOWN);
			}

			if (state.regionLoadedTicks != 0 && state.regionShownTicks == 0)
			{
				state.regionShownTicks = now;
			}

			if (HasReached(MILESTONE_CITY_COMPLETE) && !state.isLoadingCity)
			{
				Mark(MILESTONE_CITY_SHOWN);
			}
		}

		/** Stands in for the active driver's Flush, timing each frame as it ends. */
		void __fastcall HookedFlush(cIGZGDriver* driver, void* unusedEdx)
		{
			state.originalFlush(driver, unusedEdx);
			OnFramePresented();
		}

		/** Writes one entry of a method table, restoring the page protection after. */
		bool WriteTableEntry(void** entry, void* value)
		{
			DWORD oldProtection = 0;
			if (!VirtualProtect(entry, sizeof(*entry), PAGE_READWRITE, &oldProtection))
			{
				return false;
			}

			*entry = value;

			DWORD restoredProtection = 0;
			VirtualProtect(entry, sizeof(*entry), oldProtection, &restoredProtection);
			return true;
		}

		/**
		 * Routes the active driver's Flush through the frame clock, once a driver exists.
		 *
		 * The entry changed is the driver class's, so every driver of that class goes
		 * through the clock. Only the active one is ever flushed, though: the game keeps a
		 * driver for each slot but draws with one.
		 */
		void InstallFrameClock(void)
		{
			if (state.flushSlot != nullptr)
			{
				return;
			}

			// Find the driver the game draws with
			cIGZGraphicSystem2Ptr graphicSystem;
			if (!graphicSystem)
			{
				return;
			}

			cIGZGDriver* const driver = graphicSystem->GetGDriver();
			if (driver == nullptr)
			{
				return;
			}

			// Swap its Flush for ours
			//
			// An object's first field is its method table, which C++ has no way to name,
			// and the table holds plain code addresses, which have to become a function
			// pointer to be called and back again to be stored.
			void** const methodTable = *reinterpret_cast<void***>(driver);
			void** const flushEntry  = methodTable + FLUSH_SLOT;

			state.originalFlush = reinterpret_cast<FlushFunction>(*flushEntry);

			if (!WriteTableEntry(flushEntry, reinterpret_cast<void*>(&HookedFlush)))
			{
				LogError("Benchmark: could not time frames at the driver's Flush, error %lu.", GetLastError());
				state.originalFlush = nullptr;
				return;
			}

			state.flushSlot = flushEntry;
			LogInfo("Benchmark: timing frames at the Flush of driver %08x.", driver->GetGZCLSID());
		}

		void RemoveFrameClock(void)
		{
			if (state.flushSlot == nullptr)
			{
				return;
			}

			// The original was a plain code address before it became a function pointer.
			WriteTableEntry(state.flushSlot, reinterpret_cast<void*>(state.originalFlush));
			state.flushSlot = nullptr;
		}

		void OnGameMessage(uint32_t type)
		{
			switch (type)
			{
				case MESSAGE_PRE_REGION_INIT:
					Mark(MILESTONE_REGION_LOADING);
					break;

				case MESSAGE_POST_REGION_INIT:
					Mark(MILESTONE_REGION_LOADED);
					state.regionLoadedTicks = Now();
					state.regionShownTicks  = 0;
					break;

				case MESSAGE_PRE_CITY_INIT:
					Mark(MILESTONE_CITY_LOADING);
					break;

				case MESSAGE_POST_CITY_INIT:
					Mark(MILESTONE_CITY_LOADED);
					break;

				case MESSAGE_POST_CITY_INIT_COMPLETE:
					Mark(MILESTONE_CITY_COMPLETE);
					break;

				default:
					break;
			}
		}

		/** Whether a path's last part is a file name, ignoring case. */
		bool EndsWithFileName(char const* path, char const* fileName)
		{
			size_t const pathLength     = strlen(path);
			size_t const fileNameLength = strlen(fileName);
			if (fileNameLength == 0 || fileNameLength > pathLength)
			{
				return false;
			}

			// The name has to start a part of the path, so "City - A.sc4" is not found in
			// "City - Big City - A.sc4"
			char const* const ending = path + pathLength - fileNameLength;
			if (ending != path && ending[-1] != '\\' && ending[-1] != '/')
			{
				return false;
			}

			return _stricmp(ending, fileName) == 0;
		}

		/** Lists the region and some of its cities, for a run that could not find its city. */
		void LogRegion(cISC4Region* region)
		{
			LogInfo("Benchmark: the game opened region %s (%s).", region->GetName()->ToChar(), region->GetDirectoryName()->ToChar());

			uint32_t listed = 0;
			for (uint32_t z = 0; z < REGION_SEARCH_TILES && listed < 32; z++)
			{
				for (uint32_t x = 0; x < REGION_SEARCH_TILES && listed < 32; x++)
				{
					cRZAutoRefCount<cISC4RegionalCity>* const entry = region->GetCity(x, z);
					if (entry == nullptr || *entry == nullptr)
					{
						continue;
					}

					SC4String name;
					SC4String savePath;
					(*entry)->GetCityName(name);
					(*entry)->GetCitySaveFilePath(savePath);

					LogInfo("Benchmark:   %u,%u %s (%s)", x, z, name.ToChar(), savePath.ToChar());
					listed++;
				}
			}
		}

		/**
		 * The region's entry for the city the settings name, by city name or save file
		 * name, or null. The entry stays valid while the region is loaded, which is what
		 * LoadCity expects of it: the game keeps it as the current city's (0x4555f0).
		 */
		cRZAutoRefCount<cISC4RegionalCity>* FindCity(cISC4Region* region, char const* wantedCity)
		{
			for (uint32_t z = 0; z < REGION_SEARCH_TILES; z++)
			{
				for (uint32_t x = 0; x < REGION_SEARCH_TILES; x++)
				{
					cRZAutoRefCount<cISC4RegionalCity>* const entry = region->GetCity(x, z);
					if (entry == nullptr || *entry == nullptr)
					{
						continue;
					}

					// The game fills in strings of its own layout, which SC4String matches
					SC4String name;
					SC4String savePath;
					(*entry)->GetCityName(name);
					(*entry)->GetCitySaveFilePath(savePath);

					if (_stricmp(name.ToChar(), wantedCity) == 0 || EndsWithFileName(savePath.ToChar(), wantedCity))
					{
						return entry;
					}
				}
			}

			return nullptr;
		}

		/** The game's 3D view, holding a reference, or null outside a city. */
		cISC4View3DWin* FindView(void)
		{
			cISC4AppPtr app;
			if (!app)
			{
				return nullptr;
			}

			cIGZWin* const mainWindow = app->GetMainWindow();
			if (mainWindow == nullptr)
			{
				return nullptr;
			}

			cIGZWin* const appWindow = mainWindow->GetChildWindowFromID(WINDOW_ID_SC4_APP);
			if (appWindow == nullptr)
			{
				return nullptr;
			}

			// The window hands the interface back through a void pointer, holding a
			// reference for us.
			cISC4View3DWin* view = nullptr;
			if (!appWindow->GetChildAs(WINDOW_ID_VIEW_3D, kGZIID_cISC4View3DWin, reinterpret_cast<void**>(&view)))
			{
				return nullptr;
			}

			return view;
		}

		/** Keeps the largest visible top-level window of the thread, the game's own. */
		BOOL CALLBACK OnThreadWindow(HWND window, LPARAM context)
		{
			// The context is the address of the best window so far, passed as a number
			// because that is all EnumThreadWindows carries.
			HWND* const bestWindow = reinterpret_cast<HWND*>(context);
			if (!IsWindowVisible(window))
			{
				return TRUE;
			}

			RECT area     = {};
			RECT bestArea = {};
			GetClientRect(window, &area);
			if (*bestWindow != nullptr)
			{
				GetClientRect(*bestWindow, &bestArea);
			}

			if (area.right * area.bottom > bestArea.right * bestArea.bottom)
			{
				*bestWindow = window;
			}

			return TRUE;
		}

		HWND FindGameWindow(void)
		{
			// The address of the result travels as a number, as the callback expects.
			HWND window = nullptr;
			EnumThreadWindows(GetCurrentThreadId(), &OnThreadWindow, reinterpret_cast<LPARAM>(&window));
			return window;
		}

		/**
		 * The ground under the middle of the window. Noted at the start, middle and end of
		 * every scene, so the results show both renderers covered the same ground.
		 */
		TerrainPoint PickCentre(void)
		{
			TerrainPoint point = {};
			if (state.view == nullptr || state.window == nullptr)
			{
				return point;
			}

			RECT area = {};
			GetClientRect(state.window, &area);

			float world[3] = {};
			if (state.view->PickTerrain(area.right / 2, area.bottom / 2, world, false))
			{
				point = { true, world[0], world[1], world[2] };
			}

			return point;
		}

		/**
		 * The frame rate of the slowest share of frames on their own, which is how the usual
		 * 1% and 0.1% lows are counted. A share smaller than one frame takes the slowest.
		 * The frame times come slowest first.
		 */
		double LowFps(std::vector<float> const& slowestFirst, size_t shareDivisor)
		{
			size_t const lowCount = std::max<size_t>(1, slowestFirst.size() / shareDivisor);

			double lowMilliseconds = 0.0;
			for (size_t index = 0; index < lowCount; index++)
			{
				lowMilliseconds += slowestFirst[index];
			}

			// A count of frames is far inside a double's exact range.
			return 1000.0 * static_cast<double>(lowCount) / lowMilliseconds;
		}

		/** Frame rates over a scene's frame times. */
		FrameStatistics Summarise(float const* frameTimes, size_t count)
		{
			FrameStatistics statistics = {};
			statistics.frames = count;
			if (count == 0)
			{
				return statistics;
			}

			// Slowest first, so the lows are the front of the list
			std::vector<float> slowestFirst(frameTimes, frameTimes + count);
			std::sort(slowestFirst.begin(), slowestFirst.end(), std::greater<float>());

			double totalMilliseconds = 0.0;
			for (float const frameTime : slowestFirst)
			{
				totalMilliseconds += frameTime;
			}

			// A count of frames is far inside a double's exact range.
			statistics.seconds               = totalMilliseconds / 1000.0;
			statistics.averageFps            = 1000.0 * static_cast<double>(count) / totalMilliseconds;
			statistics.onePercentLowFps      = LowFps(slowestFirst, 100);
			statistics.pointOnePercentLowFps = LowFps(slowestFirst, 1000);
			statistics.slowestMilliseconds   = slowestFirst.front();
			return statistics;
		}

		void LogScene(SceneRecord const& record)
		{
			FrameStatistics const statistics = Summarise(state.frameMilliseconds.data() + record.firstFrame, record.frameCount);

			LogInfo(
				"Benchmark: %s, %zu frames in %.1f s: average %.1f fps, 1%% low %.1f, 0.1%% low %.1f, slowest %.1f ms.",
				SCENES[record.sceneIndex].name,
				statistics.frames,
				statistics.seconds,
				statistics.averageFps,
				statistics.onePercentLowFps,
				statistics.pointOnePercentLowFps,
				statistics.slowestMilliseconds
			);
		}

		void WritePoint(FILE* file, size_t sceneIndex, char const* label, TerrainPoint const& point)
		{
			if (point.isValid)
			{
				fprintf(file, "point,%zu,%s,%.2f,%.2f,%.2f\n", sceneIndex, label, point.x, point.y, point.z);
			}
		}

		/**
		 * Writes scvk-benchmark.csv beside the DLL: what was run, the milestones, each
		 * scene and every frame time. It goes to a temporary name first and is renamed
		 * when complete, so whoever waits for the file never reads half of it.
		 */
		void WriteResults(void)
		{
			if (state.isWritten)
			{
				return;
			}

			state.isWritten = true;

			char partialPath[MAX_PATH] = {};
			char finalPath[MAX_PATH]   = {};
			if (!LogFilePath(PARTIAL_RESULTS_FILE_NAME, partialPath, sizeof(partialPath)) || !LogFilePath(RESULTS_FILE_NAME, finalPath, sizeof(finalPath)))
			{
				LogError("Benchmark: no room for the results file's path.");
				return;
			}

			// Binary mode, so every line ends in a bare line feed
			FILE* file = nullptr;
			if (fopen_s(&file, partialPath, "wb") != 0 || file == nullptr)
			{
				LogError("Benchmark: could not write %s.", partialPath);
				return;
			}

			Settings const& settings = GetSettings();

			// Write what was run
			fprintf(file, "# scvk benchmark results\n");
			fprintf(file, "format,1\n");
			fprintf(file, "renderer,%s\n", settings.isDirectX ? "DirectX" : "scvk");
			fprintf(file, "scvk,%s\n", SCVK_VERSION_STRING);
			fprintf(file, "game,%u\n", unsigned{ GetGameVersion() });
			fprintf(file, "region,%s\n", settings.region);
			fprintf(file, "city,%s\n", settings.city);
			fprintf(file, "scene-seconds,%u\n", settings.sceneSeconds);
			fprintf(file, "status,%s\n", state.status);

			fprintf(file, "frames-presented,%llu\n", state.framesPresented);

			for (uint32_t milestone = 0; milestone < MILESTONE_COUNT; milestone++)
			{
				if (state.hasReached[milestone])
				{
					fprintf(file, "milestone,%s,%.1f\n", MILESTONE_NAMES[milestone], state.milestones[milestone]);
				}
			}

			// Then each scene, and every frame of it
			for (SceneRecord const& record : state.sceneRecords)
			{
				// The motion's value is its place in the table of names.
				Scene const& scene      = SCENES[record.sceneIndex];
				char const*  motionName = MOTION_NAMES[static_cast<uint32_t>(scene.motion)];

				fprintf(file, "scene,%zu,%s,%d,%d,%d,%s,%zu,%.3f\n", record.sceneIndex, scene.name, scene.zoom, scene.otherZoom, scene.rotation, motionName, record.frameCount, record.scrollSpeed);

				WritePoint(file, record.sceneIndex, "start", record.start);
				WritePoint(file, record.sceneIndex, "middle", record.middle);
				WritePoint(file, record.sceneIndex, "end", record.end);
			}

			for (SceneRecord const& record : state.sceneRecords)
			{
				for (size_t frame = 0; frame < record.frameCount; frame++)
				{
					fprintf(file, "frame,%zu,%.4f\n", record.sceneIndex, state.frameMilliseconds[record.firstFrame + frame]);
				}
			}

			fclose(file);

			if (!MoveFileExA(partialPath, finalPath, MOVEFILE_REPLACE_EXISTING))
			{
				LogError("Benchmark: could not rename the results to %s, error %lu.", finalPath, GetLastError());
				return;
			}

			LogInfo("Benchmark: wrote %s (%s).", finalPath, state.status);
		}

		/** Ends the run: writes the results, lets go of the view and quits if asked to. */
		void Finish(char const* status)
		{
			if (state.phase == Phase::FINISHED)
			{
				return;
			}

			state.phase = Phase::FINISHED;
			strncpy_s(state.status, sizeof(state.status), status, _TRUNCATE);
			WriteResults();

			if (state.view != nullptr)
			{
				state.view->Release();
				state.view = nullptr;
			}

			// Ask the game to quit, without its prompt and without saving. It only posts a
			// message (0x4546b0), so the shutdown happens after this tick returns.
			if (GetSettings().shouldQuitWhenDone)
			{
				cISC4AppPtr app;
				if (app)
				{
					app->RequestQuit(false, false);
				}
			}
		}

		void Fail(char const* status)
		{
			LogError("Benchmark: %s.", status);
			Finish(status);
		}

		/** Opens the settings' city the way the region screen does when it is clicked (0x7adc20). */
		void RequestCity(void)
		{
			Settings const& settings = GetSettings();

			cISC4AppPtr app;
			if (!app)
			{
				Fail("the game's app service is missing");
				return;
			}

			cISC4Region* const region = app->GetRegion();
			if (region == nullptr)
			{
				Fail("no region is loaded");
				return;
			}

			// Find the city
			cRZAutoRefCount<cISC4RegionalCity>* const entry = FindCity(region, settings.city);
			if (entry == nullptr)
			{
				LogRegion(region);
				Fail("the city is not in the region the game opened");
				return;
			}

			// Hold it while it loads, as the region screen does
			cRZAutoRefCount<cISC4RegionalCity> const city(*entry, cRZAutoRefCount<cISC4RegionalCity>::kAddRef);

			SC4String savePath;
			city->GetCitySaveFilePath(savePath);
			if (savePath.Strlen() == 0)
			{
				Fail("the city has never been saved");
				return;
			}

			LogInfo("Benchmark: opening %s.", savePath.ToChar());

			// Load it
			//
			// LoadCity runs the whole load before it returns, drawing the loading screen and
			// sending the city's messages on the way.
			Mark(MILESTONE_CITY_REQUESTED);
			state.phase         = Phase::LOADING_CITY;
			state.isLoadingCity = true;

			bool const isLoaded = app->LoadCity(savePath, entry);

			state.isLoadingCity = false;

			if (!isLoaded)
			{
				Fail("the game could not load the city");
			}
		}

		/** Whether the game has the settings' region open, or the settings name none. */
		bool IsSettingsRegionActive(void)
		{
			Settings const& settings = GetSettings();
			if (settings.region[0] == '\0')
			{
				return true;
			}

			cISC4AppPtr app;
			if (!app || app->GetRegion() == nullptr)
			{
				return false;
			}

			return _stricmp(app->GetRegion()->GetName()->ToChar(), settings.region) == 0;
		}

		/**
		 * Opens the settings' region the way the region screen's LoadRegion command does
		 * (0x7adac0): the nation makes it the active one, then the app goes back to the
		 * region view, which loads it. The new region's messages arrive as the first one's
		 * did, so the run carries on from there.
		 */
		void SwitchRegion(void)
		{
			Settings const& settings = GetSettings();

			cISC4AppPtr app;
			cISC4Nation* const nation = app ? app->GetNation() : nullptr;
			if (nation == nullptr)
			{
				Fail("the game has no nation to find the region in");
				return;
			}

			// Find the region by the name the region view shows
			int32_t const regionCount = nation->GetRegionCount();
			for (int32_t index = 0; index < regionCount; index++)
			{
				// The count is never negative, so the index is a valid unsigned one.
				cISC4Region* const region = nation->GetRegion(static_cast<uint32_t>(index));
				if (region == nullptr || _stricmp(region->GetName()->ToChar(), settings.region) != 0)
				{
					continue;
				}

				LogInfo("Benchmark: the game opened region %s; switching to %s.", app->GetRegion()->GetName()->ToChar(), settings.region);
				Mark(MILESTONE_REGION_SWITCHING);
				state.regionLoadedTicks = 0;
				state.regionShownTicks  = 0;

				// Make it active and go back to the region view
				//
				// gzcom-dll declares SetActiveRegion with a region pointer, but the game's
				// takes the region's index (0x4972f0), as the LoadRegion command passes it.
				// The index has to travel in the pointer's place.
				nation->SetActiveRegion(reinterpret_cast<cISC4Region*>(static_cast<uintptr_t>(index)));
				app->RequestGoToRegionView(false);
				return;
			}

			Fail("the region is not among the game's regions");
		}

		void PauseSimulation(void)
		{
			cISC4SimulatorPtr simulator;
			if (!simulator)
			{
				LogWarn("Benchmark: no simulator to pause.");
				return;
			}

			simulator->Pause();
			LogInfo("Benchmark: simulation %s.", simulator->IsPaused() ? "paused" : "still running");
		}

		void BeginScene(size_t sceneIndex)
		{
			Scene const& scene = SCENES[sceneIndex];

			state.sceneIndex     = sceneIndex;
			state.motionStep     = 0;
			state.hasMiddlePoint = false;

			state.view->SetZoomAndRotation(scene.zoom, scene.rotation);

			state.phase           = Phase::WARMING_UP;
			state.phaseStartTicks = Now();
		}

		/** The speed the arrow keys scroll at, at the current zoom, or 0 on another build. */
		float ArrowKeyScrollSpeed(void)
		{
			if (GetGameVersion() != SCROLL_GAME_VERSION)
			{
				return 0.0f;
			}

			// Read the view's fields
			//
			// The interface pointer is the view object itself (its interface query hands
			// back the same address, 0x7e5688), and the fields are plain bytes at known
			// offsets, copied out rather than cast to.
			uint8_t const* const viewBytes = reinterpret_cast<uint8_t const*>(state.view);

			float          speed         = 0.0f;
			uint8_t const* cameraControl = nullptr;
			int32_t        exponent      = 0;
			memcpy(&speed, viewBytes + VIEW_SCROLL_SPEED_OFFSET, sizeof(speed));
			memcpy(&cameraControl, viewBytes + VIEW_CAMERA_CONTROL_OFFSET, sizeof(cameraControl));

			if (cameraControl != nullptr)
			{
				memcpy(&exponent, cameraControl + CAMERA_SPEED_EXPONENT_OFFSET, sizeof(exponent));
			}

			// The handler works in floats; the exponent is a small whole number.
			return speed * static_cast<float>(pow(SCROLL_SPEED_BASE, exponent));
		}

		void StartRecording(void)
		{
			Scene const& scene = SCENES[state.sceneIndex];

			// Open the scene's record
			SceneRecord record = {};
			record.sceneIndex  = state.sceneIndex;
			record.firstFrame  = state.frameMilliseconds.size();
			record.start       = PickCentre();
			state.sceneRecords.push_back(record);

			// Hold the right arrow, in effect
			if (scene.motion == Motion::SCROLL)
			{
				state.scrollSpeed = ArrowKeyScrollSpeed();
				if (state.scrollSpeed <= 0.0f)
				{
					LogWarn("Benchmark: no scroll speed on game version %u, so %s stays still.", unsigned{ GetGameVersion() }, scene.name);
				}

				state.view->SetScrolling(true, SCROLL_ANGLE_OUT, state.scrollSpeed);
				state.sceneRecords.back().scrollSpeed = state.scrollSpeed;
			}

			state.phase           = Phase::RECORDING;
			state.phaseStartTicks = Now();
			state.lastMotionTicks = state.phaseStartTicks;
		}

		/** Moves the camera as the scene asks, partway through its timing. */
		void UpdateMotion(double elapsedSeconds)
		{
			Scene const& scene  = SCENES[state.sceneIndex];
			SceneRecord& record = state.sceneRecords.back();

			// Note the middle point
			double const sceneSeconds = GetSettings().sceneSeconds;
			if (!state.hasMiddlePoint && elapsedSeconds >= sceneSeconds / 2.0)
			{
				record.middle        = PickCentre();
				state.hasMiddlePoint = true;
			}

			// Turn around at the end of each scrolling leg
			//
			// Odd legs go back the way the even ones came, so every pair of legs ends where
			// it began, rather than the scenes walking the camera off across the city.
			if (scene.motion == Motion::SCROLL && scene.scrollLegs > 0)
			{
				// The leg count is small, and truncating the elapsed share is what picks the leg.
				uint32_t const leg = static_cast<uint32_t>(elapsedSeconds * scene.scrollLegs / sceneSeconds);
				if (leg != state.motionStep && leg < scene.scrollLegs)
				{
					state.motionStep = leg;
					state.view->SetScrolling(true, (leg % 2 == 0) ? SCROLL_ANGLE_OUT : SCROLL_ANGLE_BACK, state.scrollSpeed);
				}
			}

			// Rotate or zoom once each interval
			if (scene.motion != Motion::ROTATE && scene.motion != Motion::ZOOM)
			{
				return;
			}

			int64_t const now = Now();
			if (MillisecondsBetween(state.lastMotionTicks, now) < MOTION_INTERVAL_SECONDS * 1000.0)
			{
				return;
			}

			state.lastMotionTicks = now;
			state.motionStep++;

			if (scene.motion == Motion::ROTATE)
			{
				// Four rotations make a full turn; the step count stays tiny.
				int32_t const rotation = (scene.rotation + static_cast<int32_t>(state.motionStep % 4)) % 4;
				state.view->SetZoomAndRotation(scene.zoom, rotation);
			}
			else
			{
				int32_t const zoom = (state.motionStep % 2 == 0) ? scene.zoom : scene.otherZoom;
				state.view->SetZoomAndRotation(zoom, scene.rotation);
			}
		}

		void FinishScene(void)
		{
			Scene const& scene  = SCENES[state.sceneIndex];
			SceneRecord& record = state.sceneRecords.back();

			// Close the scene's record
			record.frameCount = state.frameMilliseconds.size() - record.firstFrame;

			if (scene.motion == Motion::SCROLL)
			{
				state.view->ScrollStop();
			}

			record.end = PickCentre();
			LogScene(record);

			// Then start the next, or finish
			if (state.sceneIndex + 1 < std::size(SCENES))
			{
				BeginScene(state.sceneIndex + 1);
				return;
			}

			Mark(MILESTONE_SCENES_FINISHED);
			Finish("complete");
		}

		double SecondsInPhase(void)
		{
			return MillisecondsBetween(state.phaseStartTicks, Now()) / 1000.0;
		}

		/** Runs once a game tick, on the game's main thread, and moves the run along. */
		void Tick(void)
		{
			InstallFrameClock();

			// Leave the load alone
			//
			// The game may tick while LoadCity is running, and nothing here should start
			// until the city is on screen.
			if (state.isLoadingCity)
			{
				return;
			}

			switch (state.phase)
			{
				case Phase::WAITING_FOR_REGION:
				{
					if (state.regionShownTicks == 0)
					{
						return;
					}

					if (MillisecondsBetween(state.regionShownTicks, Now()) < REGION_WAIT_SECONDS * 1000.0)
					{
						return;
					}

					// Open the settings' region first, when the game opened another
					if (!IsSettingsRegionActive())
					{
						if (HasReached(MILESTONE_REGION_SWITCHING))
						{
							Fail("the region did not change");
							return;
						}

						SwitchRegion();
						return;
					}

					RequestCity();
					return;
				}

				case Phase::LOADING_CITY:
				{
					if (!HasReached(MILESTONE_CITY_SHOWN))
					{
						return;
					}

					PauseSimulation();
					state.phase           = Phase::SETTLING;
					state.phaseStartTicks = Now();
					return;
				}

				case Phase::SETTLING:
				{
					if (SecondsInPhase() < SETTLE_SECONDS)
					{
						return;
					}

					// Find the view the scenes move
					state.view   = FindView();
					state.window = FindGameWindow();
					if (state.view == nullptr)
					{
						Fail("the city has no 3D view");
						return;
					}

					Mark(MILESTONE_SCENES_STARTED);
					BeginScene(0);
					return;
				}

				case Phase::WARMING_UP:
				{
					if (SecondsInPhase() < WARMUP_SECONDS)
					{
						return;
					}

					StartRecording();
					return;
				}

				case Phase::RECORDING:
				{
					double const elapsedSeconds = SecondsInPhase();
					UpdateMotion(elapsedSeconds);

					if (elapsedSeconds < GetSettings().sceneSeconds)
					{
						return;
					}

					FinishScene();
					return;
				}

				case Phase::FINISHED:
				default:
					return;
			}
		}

		/** Hears the game's region and city messages. Static, so its count is only for show. */
		class BenchmarkListener final : public cIGZMessageTarget2
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
				OnGameMessage(message->GetType());
				return true;
			}

		private:
			uint32_t referenceCount = 0;
		};

		/** Gives the run a tick from the game's own loop. Static, like the listener. */
		class BenchmarkService final : public cIGZSystemService
		{
		public:
			bool QueryInterface(uint32_t riid, void** ppvObj) override
			{
				if (riid == GZIID_cIGZSystemService || riid == GZIID_cIGZUnknown)
				{
					*ppvObj = static_cast<cIGZSystemService*>(this);
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

			uint32_t GetServiceID(void) override
			{
				return serviceId;
			}

			cIGZSystemService* SetServiceID(uint32_t newServiceId) override
			{
				serviceId = newServiceId;
				return this;
			}

			int32_t GetServicePriority(void) override
			{
				return 0;
			}

			bool IsServiceRunning(void) override
			{
				return isRunning;
			}

			cIGZSystemService* SetServiceRunning(bool shouldRun) override
			{
				isRunning = shouldRun;
				return this;
			}

			bool Init(void) override
			{
				return true;
			}

			bool Shutdown(void) override
			{
				return true;
			}

			bool OnTick([[maybe_unused]] uint32_t unknown) override
			{
				Tick();
				return true;
			}

			bool OnIdle([[maybe_unused]] uint32_t unknown) override
			{
				return true;
			}

			int32_t GetServiceTickPriority(void) override
			{
				return 0;
			}

		private:
			uint32_t referenceCount = 0;
			uint32_t serviceId      = BENCHMARK_SERVICE_ID;
			bool     isRunning      = false;
		};

		BenchmarkListener& Listener(void)
		{
			static BenchmarkListener listener;
			return listener;
		}

		BenchmarkService& Service(void)
		{
			static BenchmarkService service;
			return service;
		}
	}

	//// Public API

	bool IsBenchmarkEnabled(void)
	{
		return GetSettings().isEnabled;
	}

	bool IsBenchmarkOnDirectX(void)
	{
		return GetSettings().isEnabled && GetSettings().isDirectX;
	}

	void PrepareBenchmark(void)
	{
		if (!IsBenchmarkEnabled() || state.processStartTicks != 0)
		{
			return;
		}

		state.processStartTicks = ProcessStartTicks();

		Settings const& settings = GetSettings();
		LogInfo("Benchmark: measuring %s on %s, %u seconds a scene.", settings.isDirectX ? "the game's DirectX driver" : "scvk", settings.city, settings.sceneSeconds);
		Mark(MILESTONE_PLUGIN_STARTED);
	}

	void StartBenchmark(void)
	{
		if (!IsBenchmarkEnabled() || state.isStarted)
		{
			return;
		}

		PrepareBenchmark();
		state.isStarted = true;
		state.frameMilliseconds.reserve(FRAMES_RESERVED);
		Mark(MILESTONE_APP_STARTED);

		// Hear the region and city loads
		cIGZMessageServer2Ptr messageServer;
		if (messageServer)
		{
			for (uint32_t const message : LISTENED_MESSAGES)
			{
				messageServer->AddNotification(&Listener(), message);
			}
		}

		// Tick with the game
		cIGZFrameWork* const frameWork = RZGetFrameWork();
		if (frameWork != nullptr)
		{
			frameWork->AddSystemService(&Service());
			frameWork->AddToTick(&Service());
		}

		InstallFrameClock();
	}

	void StopBenchmark(void)
	{
		if (!state.isStarted)
		{
			return;
		}

		state.isStarted = false;

		// Keep what a cut-short run measured
		if (state.phase != Phase::FINISHED)
		{
			char status[64] = {};
			snprintf(status, sizeof(status), "stopped in phase %u", static_cast<uint32_t>(state.phase));
			state.phase = Phase::FINISHED;
			strncpy_s(state.status, sizeof(state.status), status, _TRUNCATE);
			WriteResults();
		}

		// Let go of everything the run holds
		cIGZMessageServer2Ptr messageServer;
		if (messageServer)
		{
			for (uint32_t const message : LISTENED_MESSAGES)
			{
				messageServer->RemoveNotification(&Listener(), message);
			}
		}

		cIGZFrameWork* const frameWork = RZGetFrameWork();
		if (frameWork != nullptr)
		{
			frameWork->RemoveFromTick(&Service());
			frameWork->RemoveSystemService(&Service());
		}

		if (state.view != nullptr)
		{
			state.view->Release();
			state.view = nullptr;
		}

		RemoveFrameClock();
	}
}
