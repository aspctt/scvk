# Compiles the GLSL sources to SPIR-V and regenerates src/ShaderBinaries.h.
#
# The generated header is committed, so building scvk needs no shader compiler. Run this
# only when a shader changes.
#
#   pwsh shaders/compile.ps1
#
# glslc comes with the Vulkan SDK. Set VULKAN_SDK_32 or VULKAN_SDK, or pass -Glslc with an
# explicit path.

param(
	[string]$Glslc = $null
)

$ErrorActionPreference = "Stop"

#// Constants

$shaderDirectory = $PSScriptRoot
$outputFile      = Join-Path (Split-Path $shaderDirectory -Parent) "src\ShaderBinaries.h"

$header = @"
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

/*
 * GENERATED FILE. Do not edit.
 *
 * Produced from the GLSL in shaders/ by shaders/compile.ps1. It is committed
 * so that building scvk requires no shader compiler; regenerate it only when
 * a shader changes.
 */

#pragma once
#include <stdint.h>

namespace scvk
{
"@

# The vertex stage is built once per attribute combination. A shader may not declare an
# input the pipeline does not supply, and the game's vertex formats disagree about which
# of colour and texture coordinate are present.
#
# The names match the order the backend indexes them: the colour flag times three, plus
# the number of texture coordinate sets.
$stages = @(
	@{ file = "geometry.vert"; name = "GEOMETRY_VERTEX_SPIRV_NONE";            defines = @("SCVK_HAS_COLOUR=0", "SCVK_TEXTURE_COORDINATE_SETS=0") },
	@{ file = "geometry.vert"; name = "GEOMETRY_VERTEX_SPIRV_TEXTURE";         defines = @("SCVK_HAS_COLOUR=0", "SCVK_TEXTURE_COORDINATE_SETS=1") },
	@{ file = "geometry.vert"; name = "GEOMETRY_VERTEX_SPIRV_TEXTURE2";        defines = @("SCVK_HAS_COLOUR=0", "SCVK_TEXTURE_COORDINATE_SETS=2") },
	@{ file = "geometry.vert"; name = "GEOMETRY_VERTEX_SPIRV_COLOUR";          defines = @("SCVK_HAS_COLOUR=1", "SCVK_TEXTURE_COORDINATE_SETS=0") },
	@{ file = "geometry.vert"; name = "GEOMETRY_VERTEX_SPIRV_COLOUR_TEXTURE";  defines = @("SCVK_HAS_COLOUR=1", "SCVK_TEXTURE_COORDINATE_SETS=1") },
	@{ file = "geometry.vert"; name = "GEOMETRY_VERTEX_SPIRV_COLOUR_TEXTURE2"; defines = @("SCVK_HAS_COLOUR=1", "SCVK_TEXTURE_COORDINATE_SETS=2") },
	@{ file = "geometry.frag"; name = "GEOMETRY_FRAGMENT_SPIRV";               defines = @() }
)

# SPIR-V words per line of the generated arrays.
$wordsPerLine = 8

#// Private Functions

# Finds glslc in the SDKs this machine is known to have, when no path was given.
function Find-Glslc {
	foreach ($root in @($env:VULKAN_SDK_32, $env:VULKAN_SDK, "S:\VulkanSDK\1.3.296.0")) {
		if ($root -and (Test-Path "$root\Bin\glslc.exe")) {
			return "$root\Bin\glslc.exe"
		}
	}

	return $null
}

# Compiles one stage and returns its SPIR-V as a C++ array definition.
function Convert-Stage {
	param([hashtable]$Stage)

	# Compile it to a temporary file
	$source    = Join-Path $shaderDirectory $Stage.file
	$spirvFile = [System.IO.Path]::GetTempFileName() + ".spv"

	$compilerArguments = @("-O", "--target-env=vulkan1.0")
	foreach ($define in $Stage.defines) { $compilerArguments += "-D$define" }
	$compilerArguments += @($source, "-o", $spirvFile)

	& $Glslc @compilerArguments | Out-Host
	if ($LASTEXITCODE -ne 0) { throw "glslc failed on $($Stage.name)" }

	$bytes = [System.IO.File]::ReadAllBytes($spirvFile)
	Remove-Item $spirvFile -Force

	if ($bytes.Length % 4 -ne 0) { throw "$($Stage.file): SPIR-V length is not a multiple of 4" }

	# Write it out as words, a line at a time
	$words = New-Object "System.Collections.Generic.List[string]"
	for ($i = 0; $i -lt $bytes.Length; $i += 4) {
		$word = [System.BitConverter]::ToUInt32($bytes, $i)
		$words.Add(("0x{0:x8}u" -f $word))
	}

	$definition = "`tinline constexpr uint32_t $($Stage.name)[] = {`n"
	for ($i = 0; $i -lt $words.Count; $i += $wordsPerLine) {
		$slice = $words[$i..([Math]::Min($i + $wordsPerLine - 1, $words.Count - 1))]
		$definition += "`t`t" + ($slice -join ", ") + ",`n"
	}
	$definition += "`t};`n`n"

	Write-Host ("{0,-40} {1,6} bytes, {2} words" -f $Stage.name, $bytes.Length, $words.Count)
	return $definition
}

#// Entry Point

if (-not $Glslc) {
	$Glslc = Find-Glslc
}

if (-not $Glslc -or -not (Test-Path $Glslc)) {
	throw "glslc not found. Pass -Glslc <path>, or set VULKAN_SDK."
}

$body = ""
foreach ($stage in $stages) {
	$body += Convert-Stage -Stage $stage
}

$footer = "}`n"

[System.IO.File]::WriteAllText($outputFile, $header + "`n" + $body + $footer)
Write-Host "wrote $outputFile"
