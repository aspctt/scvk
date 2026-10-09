#!/usr/bin/env python3
# Compiles the GLSL sources to SPIR-V and regenerates src/ShaderBinaries.h.
#
# The same job as compile.ps1, for machines with Python rather than PowerShell. Either
# glslc (Vulkan SDK) or glslangValidator does the compiling; spirv-opt, when present,
# optimises the result as glslc -O would.
#
#   python3 shaders/compile.py

import os
import shutil
import struct
import subprocess
import sys
import tempfile

SHADER_DIRECTORY = os.path.dirname(os.path.abspath(__file__))
OUTPUT_FILE      = os.path.join(os.path.dirname(SHADER_DIRECTORY), "src", "ShaderBinaries.h")
WORDS_PER_LINE   = 8

HEADER = """/*
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

/*
 * GENERATED FILE. Do not edit.
 *
 * Produced from the GLSL in shaders/ by shaders/compile.ps1 or shaders/compile.py. It is
 * committed so that building scvk requires no shader compiler; regenerate it only when
 * a shader changes.
 */

#pragma once
#include <stdint.h>

namespace scvk
{
"""

# The vertex stage is built once per attribute combination, and again with a flat colour.
# The names match the order the backend indexes them: flat times six, the colour flag
# times three, plus the number of texture coordinate sets.
STAGES = []
for flat, prefix in ((0, "GEOMETRY_VERTEX_SPIRV"), (1, "GEOMETRY_VERTEX_FLAT_SPIRV")):
    for colour, colourName in ((0, ""), (1, "COLOUR")):
        for sets, setName in ((0, ""), (1, "TEXTURE"), (2, "TEXTURE2")):
            suffix = "_".join(part for part in (colourName, setName) if part) or "NONE"
            STAGES.append(("geometry.vert", prefix + "_" + suffix, ["SCVK_FLAT=%d" % flat, "SCVK_HAS_COLOUR=%d" % colour, "SCVK_TEXTURE_COORDINATE_SETS=%d" % sets]))

STAGES += [
    ("geometry.vert",    "GEOMETRY_VERTEX_LIT_SPIRV",    ["SCVK_FLAT=0", "SCVK_HAS_COLOUR=1", "SCVK_TEXTURE_COORDINATE_SETS=2", "SCVK_LIT=1"]),
    ("geometry.frag",    "GEOMETRY_FRAGMENT_SPIRV",      ["SCVK_FLAT=0"]),
    ("geometry.frag",    "GEOMETRY_FRAGMENT_FLAT_SPIRV", ["SCVK_FLAT=1"]),
    ("geometry.frag",    "GEOMETRY_FRAGMENT_LIT_SPIRV",  ["SCVK_FLAT=0", "SCVK_LIT=1"]),
    ("blit.vert",        "BLIT_VERTEX_SPIRV",            []),
    ("blit.frag",        "BLIT_FRAGMENT_SPIRV",          []),
    ("shadow.vert",      "SHADOW_VERTEX_SPIRV",          []),
    ("shadow.frag",      "SHADOW_FRAGMENT_SPIRV",        []),
    ("fullscreen.vert",  "FULLSCREEN_VERTEX_SPIRV",      []),
    ("composite.frag",   "COMPOSITE_FRAGMENT_SPIRV",     []),
    ("scene_depth.frag", "SCENE_DEPTH_FRAGMENT_SPIRV",   []),
]


def compile_stage(source, defines, output):
    glslc = shutil.which("glslc")
    if glslc:
        command = [glslc, "-O", "--target-env=vulkan1.0"] + ["-D" + define for define in defines] + [source, "-o", output]
        subprocess.check_call(command)
        return

    validator = shutil.which("glslangValidator")
    if not validator:
        sys.exit("neither glslc nor glslangValidator was found")

    command = [validator, "-V", "--target-env", "vulkan1.0", "--quiet"] + ["-D" + define for define in defines] + [source, "-o", output]
    subprocess.check_call(command)

    optimiser = shutil.which("spirv-opt")
    if optimiser:
        subprocess.check_call([optimiser, "-O", "--target-env=vulkan1.0", output, "-o", output])


def convert_stage(file, name, defines):
    with tempfile.TemporaryDirectory() as directory:
        output = os.path.join(directory, "stage.spv")
        compile_stage(os.path.join(SHADER_DIRECTORY, file), defines, output)

        validator = shutil.which("spirv-val")
        if validator:
            subprocess.check_call([validator, "--target-env", "vulkan1.0", output])

        with open(output, "rb") as stream:
            data = stream.read()

    if len(data) % 4 != 0:
        sys.exit("%s: SPIR-V length is not a multiple of 4" % file)

    words = struct.unpack("<%dI" % (len(data) // 4), data)
    lines = ["\tinline constexpr uint32_t %s[] = {" % name]
    for index in range(0, len(words), WORDS_PER_LINE):
        lines.append("\t\t" + ", ".join("0x%08xu" % word for word in words[index:index + WORDS_PER_LINE]) + ",")
    lines.append("\t};")
    lines.append("")

    print("%-44s %6d bytes, %d words" % (name, len(data), len(words)))
    return "\n".join(lines) + "\n"


def main():
    body = "".join(convert_stage(*stage) for stage in STAGES)
    with open(OUTPUT_FILE, "w", newline="\n") as stream:
        stream.write(HEADER + "\n" + body + "}\n")
    print("wrote " + OUTPUT_FILE)


if __name__ == "__main__":
    main()
