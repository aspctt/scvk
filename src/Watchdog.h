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

namespace scvk
{
	//// Public API

	/**
	 * The render watchdog, as SCD3D11 has it.
	 *
	 * The render thread says what it is doing and when a frame completes. A background
	 * thread notices when no frame has completed for five seconds once frames were
	 * flowing, and logs where the render thread is: the phase it last named, its
	 * instruction pointer and the likely return addresses on its stack, with the state of
	 * the window. That turns "the game froze" into a line saying where. Phases must be
	 * string literals, since only the pointer is kept.
	 */
	void NoteRenderPhase(char const* phase);
	void NoteRenderFrame(void);
	void StartRenderWatchdog(void* window);
	void StopRenderWatchdog(void);

	/**
	 * Writes the likely return addresses on a stack to the log, each as module+offset,
	 * starting at the stack pointer given and stopping at the end of its stack. For the
	 * fault report, which otherwise names only the faulting instruction.
	 */
	void LogStackFrom(unsigned long stackPointer, char const* prefix);
}
