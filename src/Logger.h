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

#include <stdarg.h>

//// Dependencies

#include <stddef.h>
#include <stdint.h>

namespace scvk
{
	//// Types

	/**
	 * How much goes into the log, most to least. A level writes its own messages and every
	 * more severe one; the names are the ones -LogLevel:<name> on the command line takes.
	 *
	 *   trace     the ordered trace of the game's calls into the driver, from startup
	 *   debug     diagnostics: the heartbeat's statistics, draw probes, state changes
	 *   info      what scvk is doing: the device, the video mode, captures
	 *   warn      something was skipped or worked around, and the game carries on
	 *   error     something failed and part of the picture or a feature is lost
	 *   critical  scvk cannot draw at all
	 *   off       no log file
	 */
	enum LogLevel : uint32_t
	{
		LOG_LEVEL_TRACE,
		LOG_LEVEL_DEBUG,
		LOG_LEVEL_INFO,
		LOG_LEVEL_WARN,
		LOG_LEVEL_ERROR,
		LOG_LEVEL_CRITICAL,
		LOG_LEVEL_OFF,
	};

	/**
	 * One record per traced method, held in a function-local static so it is constructed
	 * on the method's first call and costs a single predictable branch thereafter. Each
	 * site chains itself onto a global list as it is created, so the shutdown summary can
	 * report every method SimCity 4 actually touched, in the order it first touched them.
	 */
	struct CallSite
	{
		explicit CallSite(char const* methodName);

		char const* name;
		uint64_t    calls;
		uint32_t    ordinal;
		CallSite*   next;
	};

	//// Public API

	/**
	 * Opens the trace file. Safe to call any number of times.
	 *
	 * Reads -LogLevel:<name> from the game's command line on the first call, and opens nothing when it
	 * is off. Truncates only on the first open in a process. Reopening never discards what is
	 * already there, because the game may drive the driver through more than one
	 * lifecycle and losing the earlier one would hide exactly the sequence we are trying
	 * to record.
	 */
	void LogOpen(void);

	/**
	 * Writes the call-site summary, at the debug level. Does not close the file.
	 *
	 * Called whenever a driver lifecycle ends. The file is never closed: every line is
	 * flushed as it is written, so it is complete at all times, and leaving it open means
	 * anything the game does afterwards is still recorded.
	 */
	void LogSummary(char const* reason);

	/** Directory the log is written to, with a trailing separator. */
	bool LogDirectory(char* outDirectory, size_t capacity);

	/** Path of a file with this name next to the log. Returns false when it does not fit. */
	bool LogFilePath(char const* name, char* outPath, size_t capacity);

	/**
	 * Whether a marker file with this name sits next to the log. Diagnostics are switched
	 * on this way rather than by build constants somebody forgets to flip back.
	 */
	bool HasMarkerFile(char const* name);

	/** Whether messages at this level reach the log. */
	bool IsLogged(LogLevel level);

	/**
	 * Write a free-form line at their level, when the LogLevel setting lets it through.
	 * None of them counts against the trace budget.
	 */
	void LogTrace(char const* format, ...);
	void LogDebug(char const* format, ...);
	void LogInfo(char const* format, ...);
	void LogWarn(char const* format, ...);
	void LogError(char const* format, ...);
	void LogCritical(char const* format, ...);

	/** Writes a line at a level from arguments already gathered, for wrappers of these. */
	void LogMessage(LogLevel level, char const* format, va_list arguments);

	/** Records a call against its site. Called via SCVK_CALL, not directly. */
	void LogCall(CallSite& site, char const* argumentFormat, ...);

	/**
	 * Whether SCVK_CALL records anything: at the debug level and below, where the call
	 * summary and the trace are written. Set when the log is opened.
	 */
	extern bool areCallsRecorded;
}

/**
 * Records the calling method. The counter advances at the debug level and below, where
 * the call summary reports it; the ordered trace line is written only at the trace
 * level, and only while the trace budget lasts. At the default level the macro costs one
 * test, which matters on the draw path: the game makes several calls a draw, and over a
 * hundred thousand draws in a city redraw at the widest zoom.
 *
 * The budget exists because the two things we want are in tension. Boot order is the
 * interesting signal, and it is a few thousand calls. Steady-state rendering is tens of
 * thousands of draw calls per second, which would bury the boot sequence in minutes of
 * noise and gigabytes of file. Bounding the ordered trace keeps the boot sequence
 * readable; the per-site counters, which are never bounded, still describe the steady
 * state.
 *
 * The first argument is a printf format for the method's own arguments; pass "" for a
 * method that takes none.
 */
#define SCVK_CALL(...)                                           \
	do {                                                         \
		if (::scvk::areCallsRecorded)                            \
		{                                                        \
			static ::scvk::CallSite scvkCallSite(__FUNCTION__);  \
			::scvk::LogCall(scvkCallSite, __VA_ARGS__);          \
		}                                                        \
	} while (0)
