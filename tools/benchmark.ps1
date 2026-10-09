# Benchmarks scvk against the game's own DirectX 7 driver: the same city, the same camera
# scenes with every frame timed, and the load times, over several runs of each.
#
#   pwsh tools/benchmark.ps1 -Region "Blackfall" -City "Ostton" -UserDir "S:\SimCity4Data"
#   pwsh tools/benchmark.ps1 -Region "Blackfall" -City "Ostton" -Runs 3 -SceneSeconds 30
#
# Every launch goes through Steam and needs its confirmation click; the rest runs by
# itself. scvk opens the region and the city, pauses it, plays the scenes, writes
# scvk-benchmark.csv and quits the game.
#
# Startup is timed to the first region on screen, before any switch to the benchmark's, so
# it compares like with like as long as every run opens in the same region.
#
# DirectX runs keep scvk loaded with no driver claimed, so both renderers get the same
# frame pacing changes and only the drawing differs. The order alternates, DirectX first
# then scvk first, so anything that drifts over a session, a warming disk cache or a
# heating card, falls on both alike.
#
# The results go to logs/benchmark-<timestamp>, with the report tools/benchmark-report.py
# makes from them. The run switches on the [Benchmark] section of the live scvk.ini and
# sets the driver in SC4GraphicsOptions.ini; both files are put back afterwards.

param(
	# The city's name, or the name of its save file.
	[Parameter(Mandatory = $true)]
	[string]$City,

	# The region the city is in, by the name the region view shows. Empty keeps the region
	# the game opens with.
	[string]$Region = "",

	# Runs of each renderer.
	[int]$Runs = 2,

	# Which renderers to run.
	[ValidateSet("Both", "scvk", "DirectX")]
	[string]$Renderer = "Both",

	# How long each scene is timed.
	[int]$SceneSeconds = 20,

	[ValidateSet("Debug", "Release")]
	[string]$Configuration = "Release",

	# Where scvk.dll is copied and the game's own plugin settings live.
	[string]$PluginsDir = $null,

	# The game's user dir, which holds the region and the city.
	[string]$UserDir = $null,

	# Seconds a run may take before it is stopped, counted from the game appearing.
	[int]$RunTimeout = 900,

	# Seconds to wait for the game to appear, which includes the Steam confirmation.
	[int]$LaunchTimeout = 300,

	# Skip copying the freshly built DLL into the Plugins folder.
	[switch]$NoDeploy
)

$ErrorActionPreference = "Stop"

#// Constants

$repositoryRoot = Split-Path $PSScriptRoot -Parent
$runScript      = Join-Path $PSScriptRoot "run-sc4.ps1"
$reportScript   = Join-Path $PSScriptRoot "benchmark-report.py"

if (-not $PluginsDir) {
	$PluginsDir = if ($env:SCVK_SC4_PLUGINS) { $env:SCVK_SC4_PLUGINS } else { "S:\SteamLibrary\steamapps\common\SimCity 4 Deluxe\Plugins" }
}

$liveSettings    = Join-Path $PluginsDir "scvk.ini"
$graphicsOptions = Join-Path $PluginsDir "SC4GraphicsOptions.ini"
$resultsName     = "scvk-benchmark.csv"
$sessionName     = "benchmark-" + (Get-Date -Format "yyyyMMdd-HHmmss")
$logDirectory    = Join-Path $repositoryRoot "logs"
$sessionPath     = Join-Path $logDirectory $sessionName

# The driver SC4GraphicsOptions asks the game for. scvk answers the OpenGL request, and the
# game's own DirectX driver the DirectX one once scvk claims nothing.
$requestedDrivers = @{ "scvk" = "OpenGL"; "DirectX" = "DirectX" }

#// Private Functions

# Settings text with one key of one section set, keeping the comments, the other keys and
# the line endings. A missing key goes at the top of its section, and a missing section at
# the end, so an older scvk.ini without the benchmark section still works.
function Set-IniKey {
	param([string]$Text, [string]$Section, [string]$Key, [string]$Value)

	# PowerShell names ignore case, so the match must not be called $section, or it would
	# replace the parameter
	$newline      = if ($Text -match "`r`n") { "`r`n" } else { "`n" }
	$line         = "$Key=$Value"
	$sectionMatch = [regex]::Match($Text, "(?ms)^\[$([regex]::Escape($Section))\][^\[]*")

	if (-not $sectionMatch.Success) {
		$trimmed = $Text.TrimEnd()
		return $(if ($trimmed) { $trimmed + $newline + $newline } else { "" }) + "[$Section]$newline$line$newline"
	}

	# Change the key within the section only
	#
	# The line is spliced in rather than handed to a regex replacement, which would read a
	# dollar sign in the value as a reference.
	$body     = $sectionMatch.Value
	$keyMatch = [regex]::Match($body, "(?m)^$([regex]::Escape($Key))=[^\r\n]*")

	if ($keyMatch.Success) {
		$body = $body.Substring(0, $keyMatch.Index) + $line + $body.Substring($keyMatch.Index + $keyMatch.Length)
	} else {
		$headerEnd = $body.IndexOf("`n") + 1
		$body = if ($headerEnd -gt 0) { $body.Insert($headerEnd, $line + $newline) } else { $body + $newline + $line + $newline }
	}

	return $Text.Substring(0, $sectionMatch.Index) + $body + $Text.Substring($sectionMatch.Index + $sectionMatch.Length)
}

#// Entry Point

if (-not (Test-Path $PluginsDir)) { throw "Not found: $PluginsDir" }
if (-not (Test-Path $graphicsOptions)) { throw "Not found: $graphicsOptions. The benchmark picks the renderer through SC4GraphicsOptions." }

# Plan the runs
#
# Odd runs start with DirectX and even runs with scvk.
$plan = @()
for ($run = 1; $run -le $Runs; $run++) {
	$pair = if ($run % 2 -eq 1) { @("DirectX", "scvk") } else { @("scvk", "DirectX") }

	foreach ($name in $pair) {
		if ($Renderer -eq "Both" -or $Renderer -eq $name) {
			$plan += [pscustomobject]@{ Renderer = $name; Run = $run }
		}
	}
}

Write-Host "benchmark of '$City'$(if ($Region) { " in $Region" }): $($plan.Count) runs, $SceneSeconds s a scene"

# Deploy the build once
if (-not $NoDeploy) {
	$builtDll = Join-Path $repositoryRoot "$Configuration\scvk.dll"
	if (-not (Test-Path $builtDll)) { throw "No $Configuration build at $builtDll" }

	Copy-Item $builtDll (Join-Path $PluginsDir "scvk.dll") -Force
	Write-Host "deployed $Configuration scvk.dll"
}

New-Item -ItemType Directory -Path $sessionPath -Force | Out-Null

$originalSettings        = if (Test-Path $liveSettings) { [System.IO.File]::ReadAllText($liveSettings) } else { "" }
$originalGraphicsOptions = [System.IO.File]::ReadAllText($graphicsOptions)

try {
	$runNumber = 0

	foreach ($entry in $plan) {
		$runNumber++
		$label = "$sessionName-$($entry.Renderer.ToLowerInvariant())-$($entry.Run)"

		Write-Host ""
		Write-Host "run $runNumber of $($plan.Count): $($entry.Renderer), pass $($entry.Run)"

		# Clear results left from an earlier run
		foreach ($stale in @($resultsName, "$resultsName.partial")) {
			$stalePath = Join-Path $PluginsDir $stale
			if (Test-Path $stalePath) { [System.IO.File]::Delete($stalePath) }
		}

		# Ask for this run
		$benchmarkValues = [ordered]@{
			"Enabled"      = "true"
			"Renderer"     = $entry.Renderer
			"Region"       = $Region
			"City"         = $City
			"SceneSeconds" = "$SceneSeconds"
			"QuitWhenDone" = "true"
		}

		$runSettings = $originalSettings
		foreach ($key in $benchmarkValues.Keys) {
			$runSettings = Set-IniKey -Text $runSettings -Section "Benchmark" -Key $key -Value $benchmarkValues[$key]
		}

		[System.IO.File]::WriteAllText($liveSettings, $runSettings)
		[System.IO.File]::WriteAllText($graphicsOptions, (Set-IniKey -Text $originalGraphicsOptions -Section "GraphicsOptions" -Key "Driver" -Value $requestedDrivers[$entry.Renderer]))

		# Run it
		#
		# Info logging only: the debug level's statistics and captures would cost scvk time
		# that DirectX never spends.
		& $runScript -Seconds $RunTimeout -LaunchTimeout $LaunchTimeout -Configuration $Configuration -Label $label -NoDeploy -LogLevel info -UntilFile $resultsName -PluginsDir $PluginsDir -UserDir $UserDir

		# Gather what it wrote
		$results = Get-ChildItem $logDirectory -Filter "scvk-benchmark-*-$label.csv" -ErrorAction SilentlyContinue | Select-Object -First 1
		$log     = Get-ChildItem $logDirectory -Filter "scvk-*-$label.log" -ErrorAction SilentlyContinue | Select-Object -First 1

		if ($log) { Copy-Item $log.FullName (Join-Path $sessionPath "$($entry.Renderer)-$($entry.Run).log") }

		if (-not $results) {
			Write-Warning "Run $runNumber wrote no results; see its log."
			continue
		}

		Move-Item $results.FullName (Join-Path $sessionPath "$($entry.Renderer)-$($entry.Run).csv") -Force
	}
} finally {
	[System.IO.File]::WriteAllText($liveSettings, $originalSettings)
	[System.IO.File]::WriteAllText($graphicsOptions, $originalGraphicsOptions)
	Write-Host ""
	Write-Host "settings restored"
}

# Report
Write-Host ""
python $reportScript $sessionPath
