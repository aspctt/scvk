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
	//// Constants

	namespace
	{
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
			fprintf(logFile, "Trace budget %u calls, then counters only.\n\n", TRACE_BUDGET);
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
		if (logFile == nullptr)
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

	void LogNote(char const* format, ...)
	{
		if (logFile == nullptr)
		{
			return;
		}

		// Format the note
		char message[sizeof(lastNote)];

		va_list arguments;
		va_start(arguments, format);
		vsnprintf(message, sizeof(message), format, arguments);
		va_end(arguments);

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

		if (tracedCount >= TRACE_BUDGET)
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
