# Compiles the GLSL sources to SPIR-V and regenerates src/ShaderBinaries.h.
#
# The generated header is committed, so building scvk needs no shader compiler.
# Run this only when a shader changes.
#
#   pwsh shaders/compile.ps1
#
# glslc comes with the Vulkan SDK. Set VULKAN_SDK_32 or VULKAN_SDK, or pass
# -Glslc with an explicit path.

param(
    [string]$Glslc = $null
)

$ErrorActionPreference = 'Stop'

if (-not $Glslc) {
    foreach ($root in @($env:VULKAN_SDK_32, $env:VULKAN_SDK, 'S:\VulkanSDK\1.3.296.0')) {
        if ($root -and (Test-Path "$root\Bin\glslc.exe")) { $Glslc = "$root\Bin\glslc.exe"; break }
    }
}

if (-not $Glslc -or -not (Test-Path $Glslc)) {
    throw "glslc not found. Pass -Glslc <path>, or set VULKAN_SDK."
}

$shaderDir = $PSScriptRoot
$outFile   = Join-Path (Split-Path $shaderDir -Parent) 'src\ShaderBinaries.h'

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

$body = ''

foreach ($stage in @(@{ file = 'geometry.vert'; name = 'kGeometryVertSpv' },
                     @{ file = 'geometry.frag'; name = 'kGeometryFragSpv' })) {

    $src = Join-Path $shaderDir $stage.file
    $spv = [System.IO.Path]::GetTempFileName() + '.spv'

    & $Glslc -O --target-env=vulkan1.0 $src -o $spv
    if ($LASTEXITCODE -ne 0) { throw "glslc failed on $($stage.file)" }

    $bytes = [System.IO.File]::ReadAllBytes($spv)
    Remove-Item $spv -Force

    if ($bytes.Length % 4 -ne 0) { throw "$($stage.file): SPIR-V length is not a multiple of 4" }

    $words = New-Object 'System.Collections.Generic.List[string]'
    for ($i = 0; $i -lt $bytes.Length; $i += 4) {
        $w = [System.BitConverter]::ToUInt32($bytes, $i)
        $words.Add(('0x{0:x8}u' -f $w))
    }

    $body += "`tinline constexpr uint32_t $($stage.name)[] = {`n"
    for ($i = 0; $i -lt $words.Count; $i += 8) {
        $slice = $words[$i..([Math]::Min($i + 7, $words.Count - 1))]
        $body += "`t`t" + ($slice -join ', ') + ",`n"
    }
    $body += "`t};`n`n"

    Write-Host ("{0,-16} {1,6} bytes, {2} words" -f $stage.file, $bytes.Length, $words.Count)
}

$footer = "}`n"

[System.IO.File]::WriteAllText($outFile, $header + "`n" + $body + $footer)
Write-Host "wrote $outFile"
