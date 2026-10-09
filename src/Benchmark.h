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
	 * Whether the [Benchmark] section of scvk.ini asks for a benchmark run. Read once and
	 * cached, so the answer cannot change between the driver registration and the run.
	 *
	 * A run opens the named region and city on its own, pauses the city, plays a fixed set
	 * of camera scenes while timing every frame, writes scvk-benchmark.csv beside the DLL
	 * and quits. Load times come from the game's own region and city messages.
	 * tools/benchmark.ps1 sets the section up, alternates the renderers and compares the
	 * results.
	 */
	bool IsBenchmarkEnabled(void);

	/**
	 * Whether this run measures the game's own DirectX 7 driver rather than scvk.
	 *
	 * scvk then claims no driver slot, so the game draws exactly as it does without scvk,
	 * but stays loaded to run the benchmark. Its frame pacing changes apply to both
	 * renderers alike: without them a paused city is held at 30 frames a second, and the
	 * comparison would measure that cap rather than the renderer.
	 */
	bool IsBenchmarkOnDirectX(void);

	/**
	 * Notes when the plugin started, measured from the process's creation, which is where
	 * the startup time counts from. Called as the plugin starts; does nothing unless a run
	 * is asked for.
	 */
	void PrepareBenchmark(void);

	/** Starts a run once the game's framework is up. Does nothing unless one is asked for. */
	void StartBenchmark(void);

	/** Ends a run before the game shuts down, writing whatever it has if it did not finish. */
	void StopBenchmark(void);
}
