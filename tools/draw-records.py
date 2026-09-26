# Reads the draw records Scroll Lock writes when the scvk-record-tile-draws marker is
# present, and shows which draws of each saved tile could have reached a pixel.
#
#   python tools/draw-records.py <scvk-key-N-draws.bin> tiles
#   python tools/draw-records.py <scvk-key-N-draws.bin> pixel <x> <y>
#   python tools/draw-records.py <scvk-key-N-draws.bin> tile <index>
#
# Pixels are counted from the top-left of the window, as in the captures. A draw counts as
# reaching a pixel when its window bounds contain it, which is an over-estimate: the bounds
# are a rectangle around the vertices, not the triangles themselves.

#// Dependencies

import struct
import sys
from collections import Counter

#// Constants

# Must match DrawFileHeader, DrawFileTile and DrawRecord in the driver.
HEADER_FORMAT = "<8s5I"
TILE_FORMAT = "<I4i4iII"
DRAW_FORMAT = "<8I8B6f8B10f2H5I8f8B"

MAGIC = b"SCVKDRW1"

DRAW_FIELDS = [
	"sequence", "frame", "vertex_format", "primitive_type", "count", "texture0", "texture1", "flags",
	"blend_source", "blend_destination", "depth_comparison", "alpha_comparison", "environment0", "environment1", "texture_levels", "texture_uploaded_levels",
	"alpha_reference", "tint_red", "tint_green", "tint_blue", "tint_alpha", "diffuse_light",
	"colour_minimum_blue", "colour_minimum_green", "colour_minimum_red", "colour_minimum_alpha",
	"colour_maximum_blue", "colour_maximum_green", "colour_maximum_red", "colour_maximum_alpha",
	"left", "top", "right", "bottom", "depth_minimum", "depth_maximum",
	"u_minimum", "u_maximum", "v_minimum", "v_maximum",
	"texture_width", "texture_height",
	"texture_uploads", "vertex_address", "lowest_vertex", "highest_vertex", "geometry_hash",
	"matrix_s0", "matrix_s1", "matrix_s2", "matrix_s3", "matrix_t0", "matrix_t1", "matrix_t2", "matrix_t3",
	"magnification_filter", "minification_filter", "wrap_s", "wrap_t", "active_stage", "coordinate_source0", "coordinate_source1", "padding",
]

# The bits of the flags field, in the driver's order.
FLAG_NAMES = [
	"stage0", "stage1", "blend", "depth_test", "depth_write", "colour_write", "alpha_test",
	"generated", "ambient_vertex", "diffuse_vertex", "indexed", "texture_live", "behind_camera", "vertices_capped",
]

# The game's comparison enumeration follows OpenGL's order.
COMPARISON_NAMES = ["never", "less", "equal", "lequal", "greater", "notequal", "gequal", "always"]

#// Private Functions

# Reads the whole file into a header, a list of tiles and a list of draws.
def read_records(path):
	data = open(path, "rb").read()

	magic, record_size, tile_count, draw_count, window_width, window_height = struct.unpack_from(HEADER_FORMAT, data, 0)
	if magic != MAGIC:
		raise SystemExit("not a draw record file: " + path)

	if record_size != struct.calcsize(DRAW_FORMAT):
		raise SystemExit("record size %d does not match this reader's %d" % (record_size, struct.calcsize(DRAW_FORMAT)))

	offset = struct.calcsize(HEADER_FORMAT)
	tiles = []

	for _ in range(tile_count):
		values = struct.unpack_from(TILE_FORMAT, data, offset)
		offset += struct.calcsize(TILE_FORMAT)
		tiles.append({"frame": values[0], "save": values[1:5], "sub_viewport": values[5:9], "first_draw": values[9], "end_draw": values[10]})

	draws = {}

	for _ in range(draw_count):
		values = struct.unpack_from(DRAW_FORMAT, data, offset)
		offset += record_size
		draw = dict(zip(DRAW_FIELDS, values))
		draw["flag_set"] = {name for bit, name in enumerate(FLAG_NAMES) if draw["flags"] & (1 << bit)}
		draws[draw["sequence"]] = draw

	return (window_width, window_height), tiles, draws


# The draws of a tile that are still in the file.
def tile_draws(tile, draws):
	return [draws[sequence] for sequence in range(tile["first_draw"], tile["end_draw"]) if sequence in draws]


# Whether a tile's saved rectangle, top-left based, contains a pixel.
def tile_contains(tile, x, y):
	left, top, width, height = tile["save"]
	return left <= x < left + width and top <= y < top + height


# The state that decides whether a draw lands, as one short string.
def pass_key(draw):
	flags = draw["flag_set"]
	blend = "blend %d,%d" % (draw["blend_source"], draw["blend_destination"]) if "blend" in flags else "opaque"
	depth = COMPARISON_NAMES[draw["depth_comparison"] & 7] if "depth_test" in flags else "no-test"
	write = "w" if "depth_write" in flags else "-"
	stages = ("T" if "stage0" in flags else "-") + ("T" if "stage1" in flags else "-")
	extras = "".join([" gen" if "generated" in flags else "", " atest" if "alpha_test" in flags else "", " nocolour" if "colour_write" not in flags else ""])
	return "fmt 0x%-2x %-11s %-7s%s tex %s%s" % (draw["vertex_format"], blend, depth, write, stages, extras)


# One line describing a draw.
def describe_draw(draw):
	return "#%-7d %s  tex %u/%u %ux%u up %u lv %u/%u  n=%-4d  filter %u/%u wrap %u/%u stage %u  texmat %.3g,%.3g,%.3g/%.3g,%.3g,%.3g  box %.0f,%.0f..%.0f,%.0f  z %.5f..%.5f  a %u..%u  tint a %.2f  uv %.2f..%.2f,%.2f..%.2f  verts %u..%u @%08x  hash %08x" % (
		draw["sequence"], pass_key(draw), draw["texture0"], draw["texture1"], draw["texture_width"], draw["texture_height"], draw["texture_uploads"], draw["texture_uploaded_levels"], draw["texture_levels"],
		draw["count"], draw["magnification_filter"], draw["minification_filter"], draw["wrap_s"], draw["wrap_t"], draw["active_stage"], draw["matrix_s0"], draw["matrix_s1"], draw["matrix_s3"], draw["matrix_t0"], draw["matrix_t1"], draw["matrix_t3"], draw["left"], draw["top"], draw["right"], draw["bottom"], draw["depth_minimum"], draw["depth_maximum"],
		draw["colour_minimum_alpha"], draw["colour_maximum_alpha"], draw["tint_alpha"],
		draw["u_minimum"], draw["u_maximum"], draw["v_minimum"], draw["v_maximum"],
		draw["lowest_vertex"], draw["highest_vertex"], draw["vertex_address"], draw["geometry_hash"],
	)


# One line describing a tile.
def describe_tile(index, tile, draws):
	left, top, width, height = tile["save"]
	present = len(tile_draws(tile, draws))
	total = tile["end_draw"] - tile["first_draw"]
	return "tile %3d  frame %6u  save %d,%d %dx%d  draws %d%s" % (index, tile["frame"], left, top, width, height, total, "" if present == total else " (%d still in the file)" % present)


#// Entry Point

if len(sys.argv) < 3:
	raise SystemExit("usage: draw-records.py <file> tiles | pixel <x> <y> | tile <index>")

window, tiles, draws = read_records(sys.argv[1])
command = sys.argv[2]

print("window %dx%d, %d tiles, %d draws" % (window[0], window[1], len(tiles), len(draws)))

if command == "tiles":
	for index, tile in enumerate(tiles):
		print(describe_tile(index, tile, draws))

elif command == "pixel":
	x, y = int(sys.argv[3]), int(sys.argv[4])

	# Walk every tile that saved this pixel, oldest first
	for index, tile in enumerate(tiles):
		if not tile_contains(tile, x, y):
			continue

		print()
		print(describe_tile(index, tile, draws))

		reaching = [draw for draw in tile_draws(tile, draws) if draw["left"] <= x <= draw["right"] and draw["top"] <= y <= draw["bottom"]]
		passes = Counter(pass_key(draw) for draw in reaching)

		for key, count in passes.items():
			print("  %4d  %s" % (count, key))

		for draw in reaching:
			print("    " + describe_draw(draw))

elif command == "tile":
	tile = tiles[int(sys.argv[3])]
	print(describe_tile(int(sys.argv[3]), tile, draws))

	for draw in tile_draws(tile, draws):
		print("  " + describe_draw(draw))

else:
	raise SystemExit("unknown command " + command)
