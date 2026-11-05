# Compares the runs scvk's benchmark writes (scvk-benchmark.csv), as tools/benchmark.ps1
# gathers them: frame rates for each scene and the load times, for each renderer. The first
# of DirectX, dgVoodoo, scd3d11 and scvk with runs is the one the others are compared with.
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
# Frame pacing looks at the frames one by one, since a few long frames between short ones
# feel like stutter while hardly moving the averages. The 99th percentile is the frame time
# 99% of frames stay at or under. A stutter is a frame that takes more than twice the median
# of the frames just before it and at least one refresh of a 60 Hz screen, counted per
# minute. Jitter is the average change in frame
# time from one frame to the next. The frame clock is the game's own Flush, so a stall in
# the game counts as much as one in the driver.
#
# With a folder, the report is also written there as report.md.

#// Dependencies

import math
import os
import statistics
import sys

#// Constants

# The order the renderers' columns take, the first one present being what the others are
# compared with. Any other name, such as a renderer with a variant, follows in the order
# its runs came.
RENDERER_ORDER = ["DirectX", "dgVoodoo", "scd3d11", "scvk"]

# The tables, as (heading, summary key, digits after the point).
FRAME_RATE_TABLES = [
	("Average frame rate", "average", 0),
	("1% low", "low1", 0),
	("0.1% low", "low01", 0),
]

PACING_TABLES = [
	("Median frame time, ms", "median_time", 2),
	("99th percentile frame time, ms", "time99", 2),
	("Stutters per minute", "stutters", 1),
	("Jitter, ms", "jitter", 2),
]

# The milestones the load times are measured between, as the benchmark names them.
LOAD_TIMES = [
	("startup", None, "region-shown"),
	("city load", "city-requested", "city-shown"),
]

# How far apart, in world units, the same point of a scene may be in two runs before the
# report says the runs did not cover the same ground.
POINT_TOLERANCE = 50.0

# A frame is a stutter when it takes more than this many times the median of the frames
# before it. Twice its neighbours is about where a single long frame starts to show.
STUTTER_FACTOR = 2.0

# How many frames before a frame make up the median it is held against. Short enough to
# follow a scene that speeds up or slows down, long enough that one stutter does not hide
# the next.
STUTTER_WINDOW = 30

# The shortest frame that counts as a stutter, one refresh of a 60 Hz screen. Without it a
# 3 ms frame among 1.4 ms ones counted, and rotating counted hundreds a minute that nobody
# could see.
STUTTER_MINIMUM = 1000.0 / 60.0

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


# The frame time a share of the frames stay at or under, by nearest rank.
def percentile(fastest_first, share):
	index = min(len(fastest_first) - 1, max(0, math.ceil(share * len(fastest_first)) - 1))
	return fastest_first[index]


# How many frames take more than STUTTER_FACTOR times the median of the frames before them,
# and at least STUTTER_MINIMUM.
def count_stutters(frame_times):
	count = 0
	for index in range(1, len(frame_times)):
		if frame_times[index] < STUTTER_MINIMUM:
			continue

		recent = frame_times[max(0, index - STUTTER_WINDOW):index]
		if frame_times[index] > STUTTER_FACTOR * statistics.median(recent):
			count += 1

	return count


def summarise(frame_times):
	if not frame_times:
		return None

	slowest_first = sorted(frame_times, reverse=True)
	total = sum(frame_times)

	# The change from each frame to the next, for a scene of a single frame none
	changes = [abs(later - earlier) for earlier, later in zip(frame_times, frame_times[1:])]

	return {
		"frames": len(frame_times),
		"average": 1000.0 * len(frame_times) / total,
		"low1": low_fps(slowest_first, 100),
		"low01": low_fps(slowest_first, 1000),
		"slowest": slowest_first[0],
		"median_time": statistics.median(frame_times),
		"time99": percentile(slowest_first[::-1], 0.99),
		"stutters": count_stutters(frame_times) / (total / 60000.0),
		"jitter": statistics.fmean(changes) if changes else 0.0,
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

# A median with its change from the baseline's in brackets, when there is one.
def format_with_change(value, baseline_value, digits):
	difference = change(baseline_value, value)
	return format_number(value, digits) + (" (%s)" % difference if difference else "")


# The lines of a table of one figure for every scene: each renderer's median over its runs,
# with its change from the first renderer's.
def scene_table(heading, key, digits, renderers, scene_names, scene_summaries):
	table = ["### " + heading, "", "| Scene | " + " | ".join(renderers) + " |", "|---|" + "---:|" * len(renderers)]

	for index in sorted(scene_names):
		medians = [median_of(scene_summaries[index][renderer], key) for renderer in renderers]
		cells = [format_number(medians[0], digits)] + [format_with_change(median, medians[0], digits) for median in medians[1:]]
		table.append("| %s | %s |" % (scene_names[index], " | ".join(cells)))

	table.append("")
	return table

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

# Order the renderers, the known ones first and the rest as their runs came
renderers = [renderer for renderer in RENDERER_ORDER if any(run["renderer"] == renderer for run in complete)]
for run in complete:
	if run["renderer"] not in renderers:
		renderers.append(run["renderer"])

if not renderers:
	raise SystemExit("No complete runs to report.")

by_renderer = {renderer: [run for run in complete if run["renderer"] == renderer] for renderer in renderers}

scene_names = {}
for run in complete:
	for index, scene in run["scenes"].items():
		scene_names[index] = scene["name"]

# Summarise every scene of every run once, for all the tables
scene_summaries = {
	index: {renderer: [summarise(run["frames"].get(index, [])) for run in by_renderer[renderer]] for renderer in renderers}
	for index in scene_names
}

# Compare the scenes
lines.append("## Frame rates")
lines.append("")
lines.append("Median of %s. In brackets, the change from %s." % (", ".join("%d %s run(s)" % (len(by_renderer[renderer]), renderer) for renderer in renderers), renderers[0]))
lines.append("")
for heading, key, digits in FRAME_RATE_TABLES:
	lines += scene_table(heading, key, digits, renderers, scene_names, scene_summaries)

# Compare the frame pacing
lines.append("## Frame pacing")
lines.append("")
lines.append("Median of the same runs; lower is better.")
lines.append("")
for heading, key, digits in PACING_TABLES:
	lines += scene_table(heading, key, digits, renderers, scene_names, scene_summaries)

# Compare the load times
lines.append("## Load times")
lines.append("")
lines.append("Median seconds. Startup counts from the game's process starting, so it includes everything the game loads, plugins too.")
lines.append("")
lines.append("| | " + " | ".join(renderers) + " |")
lines.append("|---|" + "---:|" * len(renderers))

for name, start, end in LOAD_TIMES:
	medians = []
	for renderer in renderers:
		times = [load_time(run, start, end) for run in by_renderer[renderer]]
		times = [time for time in times if time is not None]
		medians.append(statistics.median(times) if times else None)

	cells = [format_number(medians[0], 1)] + [format_with_change(median, medians[0], 1) for median in medians[1:]]
	lines.append("| %s | %s |" % (name, " | ".join(cells)))

lines.append("")

# Startup ends with the first region on screen, before any switch to the benchmark's, so
# it only compares runs that opened in the same place
switched = {"region-switching" in run["milestones"] for run in complete}
if len(switched) > 1:
	lines.append("Some runs opened in the benchmark's region and others had to switch to it, so their startups opened different regions.")
	lines.append("")

# Say whether the runs saw the same ground
spread_warnings = []
for index in sorted(scene_names):
	spread = point_spread(complete, index)
	if spread > POINT_TOLERANCE:
		spread_warnings.append("- %s: the camera's points differ by up to %.0f between runs" % (scene_names[index], spread))

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
