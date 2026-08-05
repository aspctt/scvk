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

#pragma once
#include <stdint.h>

namespace scvk
{
	/**
	 * One record per traced method, held in a function-local static so it is
	 * constructed on the method's first call and costs a single predictable
	 * branch thereafter. Each site chains itself onto a global list as it is
	 * created, so the shutdown summary can report every method SimCity 4
	 * actually touched, in the order it first touched them.
	 */
	struct CallSite
	{
		explicit CallSite(char const* name);

		char const* name;
		uint64_t    calls;
		uint32_t    ordinal;
		CallSite*   next;
	};

	/**
	 * Opens the trace file. Safe to call any number of times.
	 *
	 * Truncates only on the first open in a process. Reopening never discards
	 * what is already there, because the game may drive the driver through
	 * more than one lifecycle and losing the earlier one would hide exactly
	 * the sequence we are trying to record.
	 */
	void LogOpen(void);

	/**
	 * Writes the call-site summary. Does not close the file.
	 *
	 * Called whenever a driver lifecycle ends. The file is never closed: every
	 * line is flushed as it is written, so it is complete at all times, and
	 * leaving it open means anything the game does afterwards is still
	 * recorded.
	 */
	void LogSummary(char const* reason);

	/** Directory the log is written to, with a trailing separator. */
	bool LogDirectory(char* out, size_t size);

	/** Writes a free-form line, always, regardless of the trace budget. */
	void LogNote(char const* fmt, ...);

	/** Records a call against its site. Called via SCVK_CALL, not directly. */
	void LogCall(CallSite& site, char const* argFmt, ...);
}

/**
 * Records the calling method. The counter always advances; the ordered trace
 * line is written only while the trace budget lasts.
 *
 * The budget exists because the two things we want are in tension. Boot order
 * is the interesting signal, and it is a few thousand calls. Steady-state
 * rendering is tens of thousands of draw calls per second, which would bury
 * the boot sequence in minutes of noise and gigabytes of file. Bounding the
 * ordered trace keeps the boot sequence readable; the per-site counters, which
 * are never bounded, still describe the steady state.
 *
 * The first argument is a printf format for the method's own arguments; pass
 * "" for a method that takes none.
 */
#define SCVK_CALL(...)                                    \
	do {                                                  \
		static ::scvk::CallSite scvk_site_(__FUNCTION__); \
		::scvk::LogCall(scvk_site_, __VA_ARGS__);         \
	} while (0)
