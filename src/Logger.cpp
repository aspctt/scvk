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

#include "Logger.h"
#include "version.h"

#include <Windows.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

namespace scvk
{
	//// Types

	namespace
	{
		struct LogLevelEntry
		{
			char const* name;
			LogLevel    level;
		};
	}

	//// Constants

	namespace
	{
		// The names the LogLevel setting takes, the same set SC4DisableFpsLimits takes for
		// its own, so the two plugins' settings read alike.
		constexpr LogLevelEntry LOG_LEVEL_NAMES[] = {
			{ "trace",    LOG_LEVEL_TRACE },
			{ "debug",    LOG_LEVEL_DEBUG },
			{ "info",     LOG_LEVEL_INFO },
			{ "warn",     LOG_LEVEL_WARN },
			{ "error",    LOG_LEVEL_ERROR },
			{ "critical", LOG_LEVEL_CRITICAL },
			{ "off",      LOG_LEVEL_OFF },
		};

		constexpr LogLevel DEFAULT_LOG_LEVEL = LOG_LEVEL_INFO;

		// How many ordered trace lines to write before falling back to counters only.
		// Boot reaches the first rendered frame well inside this.
		constexpr uint32_t TRACE_BUDGET = 20000;

		// Snapshots of the counters come this many calls apart once the trace is spent.
		constexpr uint64_t SUMMARY_INTERVAL = 250000;
	}

	//// State

	namespace
	{
		FILE*     logFile       = nullptr;
		uint32_t  tracedCount   = 0;
		uint64_t  totalCalls    = 0;
		uint64_t  nextSummaryAt = 0;
		uint32_t  nextOrdinal   = 0;
		CallSite* callSites     = nullptr;
		bool      hasEverOpened = false;

		// Read from scvk.ini on the first open, and kept for the rest of the process.
		LogLevel  logLevel        = DEFAULT_LOG_LEVEL;
		bool      hasReadLogLevel = false;
		bool      isLogLevelKnown = true;
		char      logLevelSetting[16] = {};

		// Repeat collapsing. Notes are not subject to the trace budget, because they
		// carry the explanations rather than the call sequence. That was a mistake in the
		// first tracing build: a blit reporting "not supported" once per call produced
		// 899,697 identical lines and a 25 MB log that said almost nothing. Collapsing
		// identical consecutive notes keeps the signal without capping it.
		char     lastNote[512] = {};
		uint32_t repeatCount   = 0;
	}

	//// Private Functions

	namespace
	{
		/** Writes how many times the last note repeated, if it did. */
		void FlushRepeats(void)
		{
			if (repeatCount == 0 || logFile == nullptr)
			{
				return;
			}

			fprintf(logFile, "  (previous line repeated %u more times)\n", repeatCount);
			repeatCount = 0;
		}

		/** Writes the directory holding this DLL, with a trailing separator. */
		bool ModuleDirectory(char* outDirectory, size_t capacity)
		{
			// Find the module this code lives in
			//
			// The flag makes the API read its second argument as an address inside the
			// module rather than as a name, which is why a function pointer is passed
			// where the signature asks for a string.
			HMODULE module = nullptr;
			DWORD const flags = GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT;
			if (!GetModuleHandleExA(flags, reinterpret_cast<LPCSTR>(&ModuleDirectory), &module))
			{
				return false;
			}

			// Read its path and cut it after the last separator
			DWORD const written = GetModuleFileNameA(module, outDirectory, capacity);
			if (written == 0 || written >= capacity)
			{
				return false;
			}

			char* const lastSeparator = strrchr(outDirectory, '\\');
			if (lastSeparator == nullptr)
			{
				return false;
			}

			lastSeparator[1] = '\0';
			return true;
		}

		/** The LogLevel setting from scvk.ini, or the default when it is absent or unknown. */
		void ReadLogLevel(void)
		{
			hasReadLogLevel = true;

			char path[MAX_PATH];
			if (!LogFilePath("scvk.ini", path, sizeof(path)))
			{
				return;
			}

			GetPrivateProfileStringA("scvk", "LogLevel", "info", logLevelSetting, sizeof(logLevelSetting), path);

			for (LogLevelEntry const& entry : LOG_LEVEL_NAMES)
			{
				if (_stricmp(logLevelSetting, entry.name) == 0)
				{
					logLevel = entry.level;
					return;
				}
			}

			isLogLevelKnown = false;
		}

		/** The name the setting uses for a level. */
		char const* LogLevelName(LogLevel level)
		{
			for (LogLevelEntry const& entry : LOG_LEVEL_NAMES)
			{
				if (entry.level == level)
				{
					return entry.name;
				}
			}

			return "?";
		}

		/** Writes one line at a level, collapsing a run of identical lines into a count. */
		void WriteLine(LogLevel level, char const* format, va_list arguments)
		{
			if (logFile == nullptr || !IsLogged(level))
			{
				return;
			}

			// Format the line
			char message[sizeof(lastNote)];
			vsnprintf(message, sizeof(message), format, arguments);

			// Count a repeat instead of writing it
			if (strcmp(message, lastNote) == 0)
			{
				repeatCount++;
				return;
			}

			// Write it
			FlushRepeats();

			fputs(message, logFile);
			fputc('\n', logFile);
			fflush(logFile);

			strcpy_s(lastNote, sizeof(lastNote), message);
		}
	}

	//// Public API

	CallSite::CallSite(char const* methodName) : name(methodName), calls(0), ordinal(nextOrdinal++), next(callSites)
	{
		callSites = this;
	}

	bool LogDirectory(char* outDirectory, size_t capacity)
	{
		return ModuleDirectory(outDirectory, capacity);
	}

	bool LogFilePath(char const* name, char* outPath, size_t capacity)
	{
		if (!ModuleDirectory(outPath, capacity) || strlen(outPath) + strlen(name) >= capacity)
		{
			return false;
		}

		strcat_s(outPath, capacity, name);
		return true;
	}

	bool HasMarkerFile(char const* name)
	{
		char path[MAX_PATH];
		return LogFilePath(name, path, sizeof(path)) && GetFileAttributesA(path) != INVALID_FILE_ATTRIBUTES;
	}

	void LogOpen(void)
	{
		if (logFile != nullptr)
		{
			return;
		}

		// Read the level, and open nothing when it is off
		if (!hasReadLogLevel)
		{
			ReadLogLevel();
		}

		if (logLevel == LOG_LEVEL_OFF)
		{
			return;
		}

		// Truncate on the first open only
		//
		// If the game tears the driver down and builds another, reopening must not throw
		// away the record of the first lifecycle.
		char path[MAX_PATH];
		char const* const mode = hasEverOpened ? "a" : "w";

		// Open beside the DLL, or in the temp directory
		//
		// Beside the DLL is where a user looking for it will think to look. A Plugins
		// folder under Program Files may not be writable though, and a driver that cannot
		// open its log should still start, so it falls back rather than failing.
		if (LogFilePath("scvk.log", path, sizeof(path)))
		{
			fopen_s(&logFile, path, mode);
		}

		if (logFile == nullptr && GetTempPathA(sizeof(path), path) != 0 && strlen(path) + strlen("scvk.log") < sizeof(path))
		{
			strcat_s(path, sizeof(path), "scvk.log");
			fopen_s(&logFile, path, mode);
		}

		if (logFile == nullptr)
		{
			return;
		}

		// Write the header, or mark the reopening
		if (!hasEverOpened)
		{
			fprintf(logFile, "scvk %s - SimCity 4 Vulkan driver\n", SCVK_VERSION_STRING);
			fprintf(logFile, "Log level %s.\n", LogLevelName(logLevel));

			if (!isLogLevelKnown)
			{
				fprintf(logFile, "LogLevel=%s in scvk.ini is not a level; using %s.\n", logLevelSetting, LogLevelName(logLevel));
			}

			if (logLevel == LOG_LEVEL_TRACE)
			{
				fprintf(logFile, "Trace budget %u calls, then counters only.\n", TRACE_BUDGET);
			}

			fputc('\n', logFile);
			hasEverOpened = true;
		}
		else
		{
			fprintf(logFile, "\n--- log reopened ---\n");
		}

		fflush(logFile);
	}

	void LogSummary(char const* reason)
	{
		if (logFile == nullptr || !IsLogged(LOG_LEVEL_DEBUG))
		{
			return;
		}

		FlushRepeats();

		// Order the sites by first call
		//
		// The list is built by prepending, so it is walked into an ordinal-indexed array
		// to report first-call order rather than reverse order.
		CallSite* byOrdinal[512] = {};
		uint32_t  count = 0;

		for (CallSite* site = callSites; site != nullptr; site = site->next)
		{
			if (site->ordinal >= _countof(byOrdinal))
			{
				continue;
			}

			byOrdinal[site->ordinal] = site;
			count++;
		}

		// Write the table
		fprintf(logFile, "\n\n=== call summary (%s): %u methods touched, in first-call order ===\n", reason, count);
		fprintf(logFile, "%-6s %-14s %s\n", "#", "calls", "method");

		for (uint32_t i = 0; i < _countof(byOrdinal); i++)
		{
			if (byOrdinal[i] == nullptr)
			{
				continue;
			}

			fprintf(logFile, "%-6u %-14llu %s\n", i, byOrdinal[i]->calls, byOrdinal[i]->name);
		}

		if (tracedCount >= TRACE_BUDGET)
		{
			fprintf(logFile, "\nTrace budget was exhausted; ordered lines above stop at call %u.\n", TRACE_BUDGET);
		}

		fprintf(logFile, "=== end of summary, still recording ===\n\n");

		// Flush without closing
		//
		// Every line is flushed as it is written, so the file is already complete on
		// disk, and staying open means whatever the game does after this point is still
		// captured.
		fflush(logFile);
	}

	bool IsLogged(LogLevel level)
	{
		return level >= logLevel && logLevel != LOG_LEVEL_OFF;
	}

	void LogTrace(char const* format, ...)
	{
		va_list arguments;
		va_start(arguments, format);
		WriteLine(LOG_LEVEL_TRACE, format, arguments);
		va_end(arguments);
	}

	void LogDebug(char const* format, ...)
	{
		va_list arguments;
		va_start(arguments, format);
		WriteLine(LOG_LEVEL_DEBUG, format, arguments);
		va_end(arguments);
	}

	void LogInfo(char const* format, ...)
	{
		va_list arguments;
		va_start(arguments, format);
		WriteLine(LOG_LEVEL_INFO, format, arguments);
		va_end(arguments);
	}

	void LogWarn(char const* format, ...)
	{
		va_list arguments;
		va_start(arguments, format);
		WriteLine(LOG_LEVEL_WARN, format, arguments);
		va_end(arguments);
	}

	void LogError(char const* format, ...)
	{
		va_list arguments;
		va_start(arguments, format);
		WriteLine(LOG_LEVEL_ERROR, format, arguments);
		va_end(arguments);
	}

	void LogCritical(char const* format, ...)
	{
		va_list arguments;
		va_start(arguments, format);
		WriteLine(LOG_LEVEL_CRITICAL, format, arguments);
		va_end(arguments);
	}

	void LogCall(CallSite& site, char const* argumentFormat, ...)
	{
		// Count the call
		site.calls++;
		totalCalls++;

		if (logFile == nullptr)
		{
			return;
		}

		// Snapshot the counters periodically rather than only at shutdown
		//
		// The ordered trace covers startup and then stops, which is the point of the
		// budget. But the per-method counts are the only view of the steady state, and
		// writing them only in Shutdown means they are lost whenever the game is killed
		// rather than closed. That is the normal case while the renderer is incomplete,
		// so the first 3D session produced a log with no summary in it at all. The first
		// snapshot comes when the ordered trace runs out, then at intervals.
		if (totalCalls >= nextSummaryAt)
		{
			nextSummaryAt = (nextSummaryAt == 0) ? TRACE_BUDGET : totalCalls + SUMMARY_INTERVAL;
			if (totalCalls >= TRACE_BUDGET)
			{
				LogSummary("periodic snapshot");
			}
		}

		if (tracedCount >= TRACE_BUDGET || !IsLogged(LOG_LEVEL_TRACE))
		{
			return;
		}

		// Write the trace line
		FlushRepeats();
		fprintf(logFile, "%6u  %s(", tracedCount++, site.name);

		va_list arguments;
		va_start(arguments, argumentFormat);
		vfprintf(logFile, argumentFormat, arguments);
		va_end(arguments);

		fputs(")\n", logFile);

		// Flush per line
		//
		// The game can crash partway through while the renderer is incomplete, and the
		// last few lines before the crash are the most valuable ones in the file.
		fflush(logFile);
	}
}
