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

#include "Logger.h"
#include "version.h"

#include <Windows.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

namespace scvk
{
	namespace
	{
		// How many ordered trace lines to write before falling back to counters
		// only. Boot reaches the first rendered frame well inside this.
		constexpr uint32_t kTraceBudget = 20000;

		FILE*     gLog         = nullptr;
		uint32_t  gTraced      = 0;
		uint64_t  gTotalCalls  = 0;
		uint64_t  gNextSummary = 0;
		uint32_t  gNextOrdinal = 0;
		CallSite* gSites       = nullptr;
		bool      gEverOpened  = false;

		// Repeat collapsing. Notes are not subject to the trace budget, because
		// they carry the explanations rather than the call sequence. That was a
		// mistake in the first tracing build: a blit reporting "not supported"
		// once per call produced 899,697 identical lines and a 25 MB log that
		// said almost nothing. Collapsing identical consecutive notes keeps the
		// signal without capping it.
		char     gLastNote[512] = {};
		uint32_t gRepeatCount   = 0;

		void FlushRepeats(void)
		{
			if (gRepeatCount > 0 && gLog != nullptr)
			{
				fprintf(gLog, "  (previous line repeated %u more times)\n", gRepeatCount);
				gRepeatCount = 0;
			}
		}

		/** Writes the directory holding this DLL, with a trailing separator. */
		bool ModuleDirectory(char* out, size_t size)
		{
			HMODULE self = nullptr;
			if (!GetModuleHandleExA(
					GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
					reinterpret_cast<LPCSTR>(&ModuleDirectory),
					&self))
			{
				return false;
			}

			DWORD written = GetModuleFileNameA(self, out, static_cast<DWORD>(size));
			if (written == 0 || written >= size)
			{
				return false;
			}

			char* lastSeparator = strrchr(out, '\\');
			if (lastSeparator == nullptr)
			{
				return false;
			}

			lastSeparator[1] = '\0';
			return true;
		}
	}

	CallSite::CallSite(char const* name)
		: name(name), calls(0), ordinal(gNextOrdinal++), next(gSites)
	{
		gSites = this;
	}

	bool LogDirectory(char* out, size_t size)
	{
		return ModuleDirectory(out, size);
	}

	void LogOpen(void)
	{
		if (gLog != nullptr)
		{
			return;
		}

		char path[MAX_PATH];

		// Truncate on the first open only. If the game tears the driver down
		// and builds another, reopening must not throw away the record of the
		// first lifecycle.
		char const* mode = gEverOpened ? "a" : "w";

		// Preferred location is beside the DLL, where a user looking for it will
		// think to look. A Plugins folder under Program Files may not be
		// writable though, and a driver that cannot open its log should still
		// start, so fall back to the temp directory rather than failing.
		if (ModuleDirectory(path, sizeof(path)) &&
			strlen(path) + strlen("scvk.log") < sizeof(path))
		{
			strcat_s(path, sizeof(path), "scvk.log");
			fopen_s(&gLog, path, mode);
		}

		if (gLog == nullptr && GetTempPathA(sizeof(path), path) != 0 &&
			strlen(path) + strlen("scvk.log") < sizeof(path))
		{
			strcat_s(path, sizeof(path), "scvk.log");
			fopen_s(&gLog, path, mode);
		}

		if (gLog == nullptr)
		{
			return;
		}

		if (!gEverOpened)
		{
			fprintf(gLog, "scvk %s - SimCity 4 Vulkan driver\n", SCVK_VERSION_STRING);
			fprintf(gLog, "Trace budget %u calls, then counters only.\n\n", kTraceBudget);
			gEverOpened = true;
		}
		else
		{
			fprintf(gLog, "\n--- log reopened ---\n");
		}

		fflush(gLog);
	}

	void LogSummary(char const* reason)
	{
		if (gLog == nullptr)
		{
			return;
		}

		FlushRepeats();

		// The list is built by prepending, so walk it into an ordinal-indexed
		// array to report first-call order rather than reverse order.
		CallSite* byOrdinal[512] = {};
		uint32_t  count = 0;

		for (CallSite* site = gSites; site != nullptr; site = site->next)
		{
			if (site->ordinal < _countof(byOrdinal))
			{
				byOrdinal[site->ordinal] = site;
				count++;
			}
		}

		fprintf(gLog, "\n\n=== call summary (%s): %u methods touched, in first-call order ===\n",
			reason, count);
		fprintf(gLog, "%-6s %-14s %s\n", "#", "calls", "method");

		for (uint32_t i = 0; i < _countof(byOrdinal); i++)
		{
			if (byOrdinal[i] != nullptr)
			{
				fprintf(gLog, "%-6u %-14llu %s\n", i, byOrdinal[i]->calls, byOrdinal[i]->name);
			}
		}

		if (gTraced >= kTraceBudget)
		{
			fprintf(gLog, "\nTrace budget was exhausted; ordered lines above stop at call %u.\n", kTraceBudget);
		}

		fprintf(gLog, "=== end of summary, still recording ===\n\n");

		// Deliberately not closed. Every line is flushed as it is written, so
		// the file is already complete on disk, and staying open means whatever
		// the game does after this point is still captured.
		fflush(gLog);
	}

	void LogNote(char const* fmt, ...)
	{
		if (gLog == nullptr)
		{
			return;
		}

		char message[sizeof(gLastNote)];

		va_list args;
		va_start(args, fmt);
		vsnprintf(message, sizeof(message), fmt, args);
		va_end(args);

		if (strcmp(message, gLastNote) == 0)
		{
			gRepeatCount++;
			return;
		}

		FlushRepeats();

		fputs(message, gLog);
		fputc('\n', gLog);
		fflush(gLog);

		strcpy_s(gLastNote, sizeof(gLastNote), message);
	}

	void LogCall(CallSite& site, char const* argFmt, ...)
	{
		site.calls++;
		gTotalCalls++;

		if (gLog == nullptr)
		{
			return;
		}

		// Snapshot the counters periodically rather than only at shutdown.
		//
		// The ordered trace covers startup and then stops, which is the point
		// of the budget. But the per-method counts are the only view of the
		// steady state, and writing them only in Shutdown means they are lost
		// whenever the game is killed rather than closed. That is the normal
		// case while the renderer is incomplete, so the first 3D session
		// produced a log with no summary in it at all.
		if (gTotalCalls >= gNextSummary)
		{
			// First snapshot when the ordered trace runs out, then at
			// intervals, so a killed session still leaves usable numbers.
			gNextSummary = (gNextSummary == 0) ? kTraceBudget : gTotalCalls + 250000;
			if (gTotalCalls >= kTraceBudget)
			{
				LogSummary("periodic snapshot");
			}
		}

		if (gTraced >= kTraceBudget)
		{
			return;
		}

		FlushRepeats();
		fprintf(gLog, "%6u  %s(", gTraced++, site.name);

		va_list args;
		va_start(args, argFmt);
		vfprintf(gLog, argFmt, args);
		va_end(args);

		fputs(")\n", gLog);

		// Flushed per line deliberately. We fully expect the game to crash
		// partway through while the driver is still stubs, and the last few
		// lines before the crash are the most valuable ones in the file.
		fflush(gLog);
	}
}
