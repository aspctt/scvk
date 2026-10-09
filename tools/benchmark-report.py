# Compares the runs scvk's benchmark writes (scvk-benchmark.csv), as tools/benchmark.ps1
# gathers them: frame rates for each scene and the load times, for each renderer.
#
#   python tools/benchmark-report.py logs/benchmark-<timestamp>
#   python tools/benchmark-report.py <results.csv> [<results.csv> ...]
#
# The average is frames over time. A 1% or 0.1% low is the frame rate of the slowest 1% or
# 0.1% of a scene's frames on their own, the way benchmark tools usually count them. Every
# figure is the median over a renderer's runs. Startup runs from the game's process being
# created to the region on screen, and the city load from the benchmark asking for the city
# to the city on screen. Startup ends with the first region on screen, before any switch to
# the benchmark's region.
#
# With a folder, the report is also written there as report.md.

#// Dependencies

import math
import os
import statistics
import sys

#// Constants

RENDERERS = ["DirectX", "scvk"]

# The milestones the load times are measured between, as the benchmark names them.
LOAD_TIMES = [
	("startup", None, "region-shown"),
	("city load", "city-requested", "city-shown"),
]

# How far apart, in world units, the same point of a scene may be in two runs before the
# report says the runs did not cover the same ground.
POINT_TOLERANCE = 50.0

#// Private Functions

def read_results(path):
	results = {
		"path": path,
		"renderer": None,
		"status": None,
		"milestones": {},
		"scenes": {},
		"points": {},
		"frames": {},
	}

	with open(path, encoding="utf-8") as file:
		for line in file:
			line = line.rstrip("\n")
			if not line or line.startswith("#"):
				continue

			fields = line.split(",")
			kind = fields[0]

			if kind == "renderer":
				results["renderer"] = fields[1]
			elif kind == "status":
				results["status"] = fields[1]
			elif kind == "milestone":
				results["milestones"][fields[1]] = float(fields[2])
			elif kind == "scene":
				index = int(fields[1])
				results["scenes"][index] = {"name": fields[2], "motion": fields[6]}
				results["frames"][index] = []
			elif kind == "point":
				results["points"].setdefault(int(fields[1]), {})[fields[2]] = tuple(float(value) for value in fields[3:6])
			elif kind == "frame":
				results["frames"][int(fields[1])].append(float(fields[2]))

	return results


def low_fps(slowest_first, share_divisor):
	count = max(1, len(slowest_first) // share_divisor)
	return 1000.0 * count / sum(slowest_first[:count])


def summarise(frame_times):
	if not frame_times:
		return None

	slowest_first = sorted(frame_times, reverse=True)
	total = sum(frame_times)

	return {
		"frames": len(frame_times),
		"average": 1000.0 * len(frame_times) / total,
		"low1": low_fps(slowest_first, 100),
		"low01": low_fps(slowest_first, 1000),
		"slowest": slowest_first[0],
	}


def median_of(summaries, key):
	values = [summary[key] for summary in summaries if summary]
	return statistics.median(values) if values else None


def change(old, new):
	if old is None or new is None or old == 0:
		return ""

	return "%+.0f%%" % (100.0 * (new - old) / old)


def format_number(value, digits=0):
	return "-" if value is None else ("%." + str(digits) + "f") % value


def load_time(results, start, end):
	milestones = results["milestones"]
	if end not in milestones or (start is not None and start not in milestones):
		return None

	return (milestones[end] - (milestones[start] if start else 0.0)) / 1000.0


def point_spread(runs, scene_index):
	# The largest distance between any two runs' copies of the same point of a scene
	spread = 0.0
	for label in ("start", "middle", "end"):
		points = [run["points"].get(scene_index, {}).get(label) for run in runs]
		points = [point for point in points if point]
		for first in range(len(points)):
			for second in range(first + 1, len(points)):
				spread = max(spread, math.dist(points[first], points[second]))

	return spread

#// Entry Point

if len(sys.argv) < 2:
	raise SystemExit("usage: benchmark-report.py <folder | results.csv ...>")

# Gather the result files
folder = None
paths = []
for argument in sys.argv[1:]:
	if os.path.isdir(argument):
		folder = argument
		paths += sorted(os.path.join(argument, name) for name in os.listdir(argument) if name.endswith(".csv"))
	else:
		paths.append(argument)

runs = [read_results(path) for path in paths]
lines = []

# List the runs
lines.append("## Runs")
lines.append("")
for run in runs:
	lines.append("- %s: %s, %s" % (os.path.basename(run["path"]), run["renderer"], run["status"]))
lines.append("")

complete = [run for run in runs if run["status"] == "complete"]
by_renderer = {renderer: [run for run in complete if run["renderer"] == renderer] for renderer in RENDERERS}

# Compare the scenes
scene_names = {}
for run in complete:
	for index, scene in run["scenes"].items():
		scene_names[index] = scene["name"]

lines.append("## Frame rates")
lines.append("")
lines.append("Median of %s. Change is scvk against DirectX." % ", ".join("%d %s run(s)" % (len(by_renderer[renderer]), renderer) for renderer in RENDERERS))
lines.append("")
lines.append("| Scene | DirectX avg | 1% low | 0.1% low | scvk avg | 1% low | 0.1% low | Change avg | Change 1% | Change 0.1% |")
lines.append("|---|---:|---:|---:|---:|---:|---:|---:|---:|---:|")

spread_warnings = []
for index in sorted(scene_names):
	figures = {}
	for renderer in RENDERERS:
		summaries = [summarise(run["frames"].get(index, [])) for run in by_renderer[renderer]]
		figures[renderer] = {key: median_of(summaries, key) for key in ("average", "low1", "low01")}

	direct, vulkan = figures["DirectX"], figures["scvk"]
	lines.append("| %s | %s | %s | %s | %s | %s | %s | %s | %s | %s |" % (
		scene_names[index],
		format_number(direct["average"]), format_number(direct["low1"]), format_number(direct["low01"]),
		format_number(vulkan["average"]), format_number(vulkan["low1"]), format_number(vulkan["low01"]),
		change(direct["average"], vulkan["average"]), change(direct["low1"], vulkan["low1"]), change(direct["low01"], vulkan["low01"]),
	))

	spread = point_spread(complete, index)
	if spread > POINT_TOLERANCE:
		spread_warnings.append("- %s: the camera's points differ by up to %.0f between runs" % (scene_names[index], spread))

lines.append("")

# Compare the load times
lines.append("## Load times")
lines.append("")
lines.append("Median seconds. Startup counts from the game's process starting, so it includes everything the game loads, plugins too.")
lines.append("")
lines.append("| | DirectX | scvk | Change |")
lines.append("|---|---:|---:|---:|")

for name, start, end in LOAD_TIMES:
	medians = {}
	for renderer in RENDERERS:
		times = [load_time(run, start, end) for run in by_renderer[renderer]]
		times = [time for time in times if time is not None]
		medians[renderer] = statistics.median(times) if times else None

	lines.append("| %s | %s | %s | %s |" % (name, format_number(medians["DirectX"], 1), format_number(medians["scvk"], 1), change(medians["DirectX"], medians["scvk"])))

lines.append("")

# Startup ends with the first region on screen, before any switch to the benchmark's, so
# it only compares runs that opened in the same place
switched = {"region-switching" in run["milestones"] for run in complete}
if len(switched) > 1:
	lines.append("Some runs opened in the benchmark's region and others had to switch to it, so their startups opened different regions.")
	lines.append("")

# Say whether the runs saw the same ground
if spread_warnings:
	lines.append("## Camera paths")
	lines.append("")
	lines.extend(spread_warnings)
	lines.append("")

report = "\n".join(lines)
print(report)

if folder:
	with open(os.path.join(folder, "report.md"), "w", encoding="utf-8", newline="\n") as file:
		file.write(report)
