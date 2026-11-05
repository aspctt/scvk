# Benchmarks scvk against the game's own DirectX 7 driver: the same city, the same camera
# scenes with every frame timed, and the load times, over several runs of each.
#
#   pwsh tools/benchmark.ps1 -Region "Blackfall" -City "Ostton" -UserDir "S:\SimCity4Data"
#   pwsh tools/benchmark.ps1 -Region "Blackfall" -City "Ostton" -Runs 3 -SceneSeconds 30
#
# -GameArguments has to be called from inside PowerShell. Through a child pwsh the switch
# loses its quotes and reads as a parameter name:
#
#   & tools/benchmark.ps1 -Region "Blackfall" -City "Ostton" -GameArguments "-VSync:off"
#
# Every launch goes through Steam and needs its confirmation click; the rest runs by
# itself. scvk opens the region and the city, pauses it, plays the scenes, writes
# scvk-benchmark.csv and quits the game.
#
# Startup is timed to the first region on screen, before any switch to the benchmark's, so
# it compares like with like as long as every run opens in the same region.
#
# Runs of the other renderers keep scvk loaded with no driver claimed, so every renderer
# gets the same frame pacing changes and only the drawing differs. The order alternates,
# the renderers as listed on odd passes and reversed on even ones, so anything that drifts
# over a session, a warming disk cache or a heating card, falls on all of them alike.
#
# The other renderers:
#
# - DirectX is the game's own DirectX 7 driver, and needs Apps free of dgVoodoo's DLLs.
# - dgVoodoo is the same driver through dgVoodoo 2, whose DDraw.dll and D3DImm.dll have to be
#   in Apps beside SimCity 4.exe. The script only checks for them, so DirectX and dgVoodoo
#   go in separate sessions with the DLLs swapped by hand between them.
# - scd3d11 is caspervg's Direct3D 11 driver, which takes the DirectX driver's class. Its
#   DLL is copied into Plugins for its runs only (-Scd3d11Path), since it would draw the
#   DirectX runs too.
#
#   & tools/benchmark.ps1 -City "Ostton" -Renderers DirectX, scvk, scd3d11 -Scd3d11Path <dll>
#
# -Variant names runs that differ by a launch option, such as "cull off", so the report
# keeps them apart from the plain ones.
#
# The results go to logs/benchmark-<timestamp>, with the report tools/benchmark-report.py
# makes from them. The run switches on the [Benchmark] section of the live scvk.ini and
# sets the driver and a 1920x1080 window in SC4GraphicsOptions.ini; both files are put back
# afterwards.

param(
	# The city's name, or the name of its save file.
	[Parameter(Mandatory = $true)]
	[string]$City,

	# The region the city is in, by the name the region view shows. Empty keeps the region
	# the game opens with.
	[string]$Region = "",

	# Runs of each renderer.
	[int]$Runs = 2,

	# Which renderers to run, in the order of the odd passes.
	[ValidateSet("DirectX", "scvk", "scd3d11", "dgVoodoo")]
	[string[]]$Renderers = @("DirectX", "scvk"),

	# Added to each renderer's name in the results, for runs that differ by a launch option.
	[string]$Variant = "",

	# scd3d11's DLL, kept outside Plugins, for its runs.
	[string]$Scd3d11Path = "",

	# How long each scene is timed.
	[int]$SceneSeconds = 20,

	# The window every run uses, for both renderers alike. The game's DirectX 7 driver
	# can't go above 2048x2048 without a wrapper, and borderless on a 2560x1440 screen made
	# it quit within seconds, so the default is a 1920x1080 window, as in the 0.2.0 runs.
	[int]$WindowWidth = 1920,
	[int]$WindowHeight = 1080,

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
	[switch]$NoDeploy,

	# More of the game's own launch options, for every run. With VSync on, scvk's lighter
	# scenes sit at the monitor's refresh rate, so -VSync:off shows the cost of a change
	# above it. The game's DirectX driver ignores that switch.
	[string[]]$GameArguments = @()
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

# The driver SC4GraphicsOptions asks the game for. scvk answers the OpenGL request. Once
# scvk claims nothing, the DirectX one goes to scd3d11 when it is installed, which takes
# the class at a higher version, and to the game's own driver otherwise.
$requestedDrivers = @{ "scvk" = "OpenGL"; "DirectX" = "DirectX"; "scd3d11" = "DirectX"; "dgVoodoo" = "DirectX" }

# What scvk's benchmark is told it measures: itself, or whatever driver the game picks.
$benchmarkRenderers = @{ "scvk" = "scvk"; "DirectX" = "DirectX"; "scd3d11" = "DirectX"; "dgVoodoo" = "DirectX" }

# dgVoodoo's DLLs, which make the game's DirectX 7 driver draw through Direct3D 11.
$appsDir       = Join-Path (Split-Path $PluginsDir -Parent) "Apps"
$dgVoodooFiles = @("DDraw.dll", "D3DImm.dll")

$scd3d11Name = "SCD3D11.dll"
$scd3d11Log  = Join-Path (Split-Path $PluginsDir -Parent) "Logs\SCD3D11.log"

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

# Check that each renderer's files are where its runs need them
$dgVoodooPresent = @($dgVoodooFiles | Where-Object { Test-Path (Join-Path $appsDir $_) })

if (($Renderers -contains "DirectX") -and ($Renderers -contains "dgVoodoo")) {
	throw "DirectX and dgVoodoo need Apps with and without dgVoodoo's DLLs, so they go in separate sessions."
}

if (($Renderers -contains "DirectX") -and $dgVoodooPresent.Count -gt 0) {
	throw "Apps holds dgVoodoo's $($dgVoodooPresent -join ', '), which would draw the DirectX runs. Move them out first."
}

if (($Renderers -contains "dgVoodoo") -and $dgVoodooPresent.Count -lt $dgVoodooFiles.Count) {
	throw "dgVoodoo's runs need $($dgVoodooFiles -join ' and ') in $appsDir."
}

# scd3d11 in either Plugins folder would take the DirectX runs too
$scd3d11Folders = @($PluginsDir) + $(if ($UserDir) { @(Join-Path $UserDir "Plugins") } else { @() })
foreach ($folder in $scd3d11Folders) {
	$installed = Join-Path $folder $scd3d11Name
	if (Test-Path $installed) { throw "$installed is installed. Move it out; the benchmark copies it in for scd3d11's runs only." }
}

if (($Renderers -contains "scd3d11") -and -not ($Scd3d11Path -and (Test-Path $Scd3d11Path))) {
	throw "scd3d11's runs need -Scd3d11Path, the DLL kept outside Plugins."
}

# Plan the runs
#
# Odd passes take the renderers in the order given and even passes in reverse.
$plan = @()
for ($run = 1; $run -le $Runs; $run++) {
	$order = @($Renderers)
	if ($run % 2 -eq 0) { [array]::Reverse($order) }

	foreach ($name in $order) {
		$plan += [pscustomobject]@{ Renderer = $name; Run = $run }
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

		# Name the run, with the variant when there is one, as the report and the files show it
		$displayName = if ($Variant) { "$($entry.Renderer) $Variant" } else { $entry.Renderer }
		$fileStem    = ($displayName -replace "[^A-Za-z0-9]+", "-").Trim("-")
		$label       = "$sessionName-$($fileStem.ToLowerInvariant())-$($entry.Run)"

		Write-Host ""
		Write-Host "run $runNumber of $($plan.Count): $displayName, pass $($entry.Run)"

		# Clear results left from an earlier run
		foreach ($stale in @($resultsName, "$resultsName.partial")) {
			$stalePath = Join-Path $PluginsDir $stale
			if (Test-Path $stalePath) { [System.IO.File]::Delete($stalePath) }
		}

		# Ask for this run
		$benchmarkValues = [ordered]@{
			"Enabled"      = "true"
			"Renderer"     = $benchmarkRenderers[$entry.Renderer]
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

		# Pick the driver and the window
		#
		# SC4GraphicsOptions decides the size and mode whenever it is installed, scvk's
		# borderless default included, so a window set here holds for every renderer.
		$graphicsValues = [ordered]@{
			"Driver"       = $requestedDrivers[$entry.Renderer]
			"WindowMode"   = "Windowed"
			"WindowWidth"  = "$WindowWidth"
			"WindowHeight" = "$WindowHeight"
		}

		$runGraphicsOptions = $originalGraphicsOptions
		foreach ($key in $graphicsValues.Keys) {
			$runGraphicsOptions = Set-IniKey -Text $runGraphicsOptions -Section "GraphicsOptions" -Key $key -Value $graphicsValues[$key]
		}

		[System.IO.File]::WriteAllText($graphicsOptions, $runGraphicsOptions)

		# Install scd3d11 for its own runs
		$isScd3d11Run = $entry.Renderer -eq "scd3d11"
		$runStarted   = Get-Date
		if ($isScd3d11Run) {
			Copy-Item $Scd3d11Path (Join-Path $PluginsDir $scd3d11Name) -Force
		}

		# Run it
		#
		# Info logging only: the debug level's statistics and captures would cost scvk time
		# that the other renderers never spend.
		try {
			& $runScript -Seconds $RunTimeout -LaunchTimeout $LaunchTimeout -Configuration $Configuration -Label $label -NoDeploy -LogLevel info -UntilFile $resultsName -PluginsDir $PluginsDir -UserDir $UserDir -GameArguments $GameArguments
		} finally {
			if ($isScd3d11Run) { Remove-Item (Join-Path $PluginsDir $scd3d11Name) -Force -ErrorAction SilentlyContinue }
		}

		# Gather what it wrote
		$results = Get-ChildItem $logDirectory -Filter "scvk-benchmark-*-$label.csv" -ErrorAction SilentlyContinue | Select-Object -First 1
		$log     = Get-ChildItem $logDirectory -Filter "scvk-*-$label.log" -ErrorAction SilentlyContinue | Select-Object -First 1

		if ($log) { Copy-Item $log.FullName (Join-Path $sessionPath "$fileStem-$($entry.Run).log") }
		# scd3d11's own log, which says which settings it ran with, unless it is one left from
		# an earlier session
		if ($isScd3d11Run -and (Test-Path $scd3d11Log) -and (Get-Item $scd3d11Log).LastWriteTime -ge $runStarted) {
			Copy-Item $scd3d11Log (Join-Path $sessionPath "$fileStem-$($entry.Run).scd3d11.log")
		}

		if (-not $results) {
			Write-Warning "Run $runNumber wrote no results; see its log."
			continue
		}

		# Name the renderer in the results
		#
		# scvk only knows it measured some other driver, so its results say DirectX for all
		# of them. The report tells the runs apart by this line.
		$resultLines = [System.IO.File]::ReadAllLines($results.FullName) | ForEach-Object { if ($_ -like "renderer,*") { "renderer,$displayName" } else { $_ } }
		[System.IO.File]::WriteAllText((Join-Path $sessionPath "$fileStem-$($entry.Run).csv"), ($resultLines -join "`n") + "`n")
		Remove-Item $results.FullName
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
