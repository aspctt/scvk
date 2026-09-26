# Deploys scvk, runs SimCity 4 for a fixed time, stops it, and collects the log.
#
#   pwsh tools/run-sc4.ps1 -Seconds 30
#   pwsh tools/run-sc4.ps1 -Seconds 120 -Configuration Release -Label city-load
#
# The game has to be started through steam://run. Launching the executable directly does
# not work: it is wrapped in Steam's DRM, so it hands off to Steam and exits within a few
# seconds without loading a single plugin. Going through Steam costs one confirmation
# click per run, after which the whole session is unattended however long it lasts.
#
# The Plugins folder defaults to this machine's layout and can be overridden with the
# SCVK_SC4_PLUGINS environment variable or the parameter below.

param(
	[int]$Seconds = 30,

	[ValidateSet("Debug", "Release")]
	[string]$Configuration = "Debug",

	# Appended to the captured log's filename, so runs can be told apart.
	[string]$Label = "",

	# Skip copying the freshly built DLL into the Plugins folder.
	[switch]$NoDeploy,

	# Seconds to wait for the game to appear after Steam is asked to launch it. Generous,
	# because confirming the Steam prompt is a manual step.
	[int]$LaunchTimeout = 120,

	[int]$AppId = 24780,

	[string]$PluginsDir = $null
)

$ErrorActionPreference = "Stop"

#// Constants

$repositoryRoot = Split-Path $PSScriptRoot -Parent

if (-not $PluginsDir) {
	$PluginsDir = if ($env:SCVK_SC4_PLUGINS) { $env:SCVK_SC4_PLUGINS } else { "S:\SimCity4Data\_ModdingData\Plugins" }
}

$userDirectory = Split-Path $PluginsDir -Parent
$builtDll      = Join-Path $repositoryRoot "$Configuration\scvk.dll"
$liveDll       = Join-Path $PluginsDir "scvk.dll"
$liveLog       = Join-Path $PluginsDir "scvk.log"
$logDirectory  = Join-Path $repositoryRoot "logs"

# The game's own arguments, matching the shortcut this replaces. The trailing separator on
# UserDir matters to the game's parser.
$gameArguments = @(
	"-BackgroundLoader:on",
	"-ExceptionHandling:off",
	"-UserDir:$userDirectory\"
)

$gameImageName = "SimCity 4.exe"

#// Private Functions

# Every running process whose image is SimCity 4 itself.
function Get-SimCity {
	return Get-Process -ErrorAction SilentlyContinue | Where-Object { $_.Path -and $_.Path.EndsWith($gameImageName) }
}

# Asks the game to close, then insists.
function Stop-SimCity {
	# The polite close is expected to fail while the renderer is incomplete, because the
	# game's quit confirmation is drawn through scvk and may not be legible enough to
	# answer, but it costs a few seconds to try and it will start working as rendering
	# improves.
	param([System.Diagnostics.Process]$Process)

	if ($null -eq $Process -or $Process.HasExited) { return }

	Write-Host "  asking pid $($Process.Id) to close"
	try { $Process.CloseMainWindow() | Out-Null } catch { }

	if ($Process.WaitForExit(5000)) {
		Write-Host "  closed cleanly"
		return
	}

	Write-Host "  did not close, stopping it"
	try { $Process.Kill() } catch { }
	try { $Process.WaitForExit(10000) | Out-Null } catch { }
}

#// Entry Point

if (-not (Test-Path $PluginsDir)) { throw "Not found: $PluginsDir" }

# Stop a game left over from an earlier run
#
# That is the most likely reason a deploy fails, since the DLL is locked while the game
# holds it open.
$staleProcesses = Get-SimCity

if ($staleProcesses) {
	Write-Host "A previous SimCity 4 is still running (pid $($staleProcesses.Id -join ", ")). Stopping it."
	foreach ($process in $staleProcesses) { Stop-SimCity -Process $process }
	Start-Sleep -Seconds 1
}

# Deploy the build and clear the old log
if (-not $NoDeploy) {
	if (-not (Test-Path $builtDll)) { throw "No $Configuration build at $builtDll" }
	Copy-Item $builtDll $liveDll -Force
	Write-Host "deployed $Configuration scvk.dll ($((Get-Item $liveDll).Length) bytes)"
}

if (Test-Path $liveLog) { [System.IO.File]::Delete($liveLog) }

# Launch the game through Steam
#
# Running the executable itself does not work: it is wrapped in Steam's DRM, so it hands
# off to Steam and exits within a few seconds without ever loading a plugin. Going through
# the steam:// URL is the only route that actually starts the game, at the cost of one
# confirmation click per run.
$steamUrl = "steam://run/$AppId//" + ($gameArguments -join " ") + "/"

Write-Host "launching through Steam"
Write-Host "  >>> confirm the launch in Steam if it asks <<<"
Start-Process $steamUrl | Out-Null

# Wait for it to appear
#
# Steam takes a while, and the confirmation is a human in the loop, so the timer only
# starts once the process actually exists.
$game = $null
$launchDeadline = (Get-Date).AddSeconds($LaunchTimeout)

while ((Get-Date) -lt $launchDeadline) {
	$game = Get-SimCity | Select-Object -First 1

	if ($game) { break }
	Start-Sleep -Milliseconds 500
}

if (-not $game) {
	throw "SimCity 4 did not start within $LaunchTimeout seconds. Was the Steam prompt confirmed?"
}

Write-Host "  started, pid $($game.Id); running for $Seconds seconds"

# Let it run, then stop it
$deadline = (Get-Date).AddSeconds($Seconds)
$hasExitedEarly = $false

while ((Get-Date) -lt $deadline) {
	if ($game.HasExited) { $hasExitedEarly = $true; break }
	Start-Sleep -Milliseconds 500
}

if ($hasExitedEarly) {
	Write-Host "  the game exited on its own after $([int]((Get-Date) - $game.StartTime).TotalSeconds)s (code $($game.ExitCode))"
} else {
	Stop-SimCity -Process $game
}

# Collect the log
#
# It is flushed line by line, so it is complete the moment the process stops; no settling
# delay is needed.
if (-not (Test-Path $liveLog)) {
	Write-Warning "No log was produced at $liveLog. The DLL may not have been loaded at all."
	exit 1
}

if (-not (Test-Path $logDirectory)) { New-Item -ItemType Directory -Path $logDirectory | Out-Null }

$timestamp   = Get-Date -Format "yyyyMMdd-HHmmss"
$suffix      = if ($Label) { "-$Label" } else { "" }
$capturedLog = Join-Path $logDirectory "scvk-$timestamp$suffix.log"
Copy-Item $liveLog $capturedLog

# Collect the captures
#
# The driver writes BMPs next to its log. They are converted to PNG here purely because
# it is a more convenient format to look at. Depth captures are raw floats, moved as they
# are.
Add-Type -AssemblyName System.Drawing
foreach ($bitmap in Get-ChildItem $PluginsDir -Filter "scvk-*.bmp" -ErrorAction SilentlyContinue) {
	$pngPath = Join-Path $logDirectory (($bitmap.BaseName) + "-$timestamp$suffix.png")
	try {
		$image = [System.Drawing.Image]::FromFile($bitmap.FullName)
		$image.Save($pngPath, [System.Drawing.Imaging.ImageFormat]::Png)
		$image.Dispose()
		[System.IO.File]::Delete($bitmap.FullName)
		Write-Host "  captured $pngPath"
	} catch {
		Write-Host "  could not convert $($bitmap.Name): $($_.Exception.Message)"
	}
}

foreach ($rawFile in Get-ChildItem $PluginsDir -Filter "scvk-*.raw" -ErrorAction SilentlyContinue) {
	$destination = Join-Path $logDirectory (($rawFile.BaseName) + "-$timestamp$suffix.raw")
	Move-Item $rawFile.FullName $destination -Force
	Write-Host "  captured $destination"
}

# Summarise the run
$lines = [System.IO.File]::ReadAllLines($capturedLog)

Write-Host ""
Write-Host "captured $capturedLog  ($($lines.Count) lines)"
Write-Host ""

$deviceLine = $lines | Where-Object { $_ -match "Vulkan: selected" } | Select-Object -First 1
if ($deviceLine) { Write-Host "  $($deviceLine.Trim())" }

$frameCounts = $lines | Where-Object { $_ -match "(\d+) frames presented" } | ForEach-Object { [int][regex]::Match($_, "(\d+) frames presented").Groups[1].Value }

if ($frameCounts) {
	Write-Host ("  frames presented : {0:N0}" -f ($frameCounts | Measure-Object -Maximum).Maximum)
} else {
	Write-Host "  frames presented : none"
}

$validationMessages = @($lines | Where-Object { $_ -match "Vulkan ERROR|Vulkan warning" })
Write-Host "  validation       : $(if ($validationMessages.Count) { "$($validationMessages.Count) message(s)" } else { "clean" })"

$frameDumps = @($lines | Where-Object { $_ -match "=== dumping " })
Write-Host "  frame dumps      : $($frameDumps.Count)"

$notableLines = @($lines | Where-Object { $_ -match "MISMATCH|LARGE DRAW|FATAL|not handled|could not|Falling back" })
if ($notableLines.Count) {
	Write-Host ""
	Write-Host "  notable:"
	$notableLines | Select-Object -First 8 | ForEach-Object { Write-Host "    $($_.Trim())" }
}

Write-Host ""
