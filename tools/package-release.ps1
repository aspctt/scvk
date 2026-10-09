# Builds scvk in Release and packs the download into Releases\scvk-<version>.zip.
#
#   pwsh tools/package-release.ps1
#
# The zip holds scvk.dll, which goes into the game's Plugins folder, beside the
# readme and the third party notices from package\ and the license. The version comes from
# src\version.h, so bump it there first. Release notes are written by hand beside the zip.

param(
	# Pack the Release DLL already built instead of building it again.
	[switch]$NoBuild
)

$ErrorActionPreference = "Stop"

#// Constants

$repositoryRoot   = Split-Path $PSScriptRoot -Parent
$releaseDirectory = Join-Path $repositoryRoot "Releases"
$stagingDirectory = Join-Path $repositoryRoot "obj\package"
$versionHeader    = Join-Path $repositoryRoot "src\version.h"

# Each file in the zip, by its name there, and where it comes from.
$packageFiles = [ordered]@{
	"LICENSE.txt"             = Join-Path $repositoryRoot "LICENSE"
	"README.htm"              = Join-Path $repositoryRoot "package\README.htm"
	"scvk.dll"                = Join-Path $repositoryRoot "Release\scvk.dll"
	"Third Party Notices.txt" = Join-Path $repositoryRoot "package\Third Party Notices.txt"
}

#// Entry Point

# Read the version
$versionMatch = Select-String -Path $versionHeader -Pattern '#define SCVK_VERSION_STRING "([^"]+)"'
if (-not $versionMatch) { throw "No SCVK_VERSION_STRING in $versionHeader" }

$version = $versionMatch.Matches[0].Groups[1].Value
$zipPath = Join-Path $releaseDirectory "scvk-$version.zip"

# Build Release
#
# vswhere ships with every Visual Studio installer and finds MSBuild wherever the edition
# put it.
if (-not $NoBuild) {
	$vswhere = Join-Path ${env:ProgramFiles(x86)} "Microsoft Visual Studio\Installer\vswhere.exe"
	$msbuild = & $vswhere -latest -requires Microsoft.Component.MSBuild -find "MSBuild\**\Bin\MSBuild.exe" | Select-Object -First 1
	if (-not $msbuild) { throw "MSBuild not found" }

	& $msbuild (Join-Path $repositoryRoot "scvk.sln") -p:Configuration=Release -p:Platform=Win32 -v:minimal -nologo
	if ($LASTEXITCODE -ne 0) { throw "The Release build failed" }
}

# Gather the files under their names in the zip
if (Test-Path $stagingDirectory) { Remove-Item -Recurse -Force $stagingDirectory }
New-Item -ItemType Directory -Force $stagingDirectory | Out-Null

foreach ($name in $packageFiles.Keys) {
	$source = $packageFiles[$name]
	if (-not (Test-Path $source)) { throw "Not found: $source" }

	Copy-Item $source (Join-Path $stagingDirectory $name)
}

# Pack them
New-Item -ItemType Directory -Force $releaseDirectory | Out-Null
Compress-Archive -Path (Join-Path $stagingDirectory "*") -DestinationPath $zipPath -Force

Write-Host "packed $zipPath"
foreach ($name in $packageFiles.Keys) {
	Write-Host "  $name"
}
