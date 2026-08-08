# Deploys scvk, runs SimCity 4 for a fixed time, stops it, and collects the log.
#
#   pwsh tools/run-sc4.ps1 -Seconds 30
#   pwsh tools/run-sc4.ps1 -Seconds 120 -Configuration Release -Label citiy-load
#
# The game has to be started through steam://run. Launching the executable
# directly does not work: it is wrapped in Steam's DRM, so it hands off to
# Steam and exits within a few seconds without loading a single plugin. Going
# through Steam costs one confirmation click per run, after which the whole
# session is unattended however long it lasts.
#
# Paths default to this machine's layout and can be overridden with the
# SCVK_SC4_PLUGINS environment variable or the parameters below.

param(
    [int]$Seconds = 30,

    [ValidateSet('Debug', 'Release')]
    [string]$Configuration = 'Debug',

    # Appended to the captured log's filename, so runs can be told apart.
    [string]$Label = '',

    # Skip copying the freshly built DLL into the Plugins folder.
    [switch]$NoDeploy,

    # Skip grabbing a screenshot just before the game is stopped.
    [switch]$NoScreenshot,

    # Seconds to wait for the game to appear after Steam is asked to launch it.
    # Generous, because confirming the Steam prompt is a manual step.
    [int]$LaunchTimeout = 120,

    [int]$AppId = 24780,

    [string]$GameExe = $null,
    [string]$PluginsDir = $null
)

$ErrorActionPreference = 'Stop'

$repoRoot = Split-Path $PSScriptRoot -Parent

if (-not $GameExe) {
    $GameExe = if ($env:SCVK_SC4_EXE) { $env:SCVK_SC4_EXE }
               else { 'S:\SteamLibrary\steamapps\common\SimCity 4 Deluxe\Apps\SimCity 4.exe' }
}

if (-not $PluginsDir) {
    $PluginsDir = if ($env:SCVK_SC4_PLUGINS) { $env:SCVK_SC4_PLUGINS }
                  else { 'S:\SimCity4Data\_ModdingData\Plugins' }
}

$userDir  = Split-Path $PluginsDir -Parent
$builtDll = Join-Path $repoRoot "$Configuration\scvk.dll"
$liveDll  = Join-Path $PluginsDir 'scvk.dll'
$liveLog  = Join-Path $PluginsDir 'scvk.log'
$logDir   = Join-Path $repoRoot 'logs'

if (-not (Test-Path $PluginsDir)) { throw "Not found: $PluginsDir" }

# The game's own arguments, matching the shortcut this replaces. The trailing
# separator on UserDir matters to the game's parser.
$gameArgs = @(
    '-BackgroundLoader:on',
    '-ExceptionHandling:off',
    "-UserDir:$userDir\"
)

function Stop-SimCity {
    # Asks first, then insists. The polite close is expected to fail while the
    # renderer is incomplete, because the game's quit confirmation is drawn
    # through scvk and may not be legible enough to answer, but it costs a few
    # seconds to try and it will start working as rendering improves.
    #
    # Only ever a process whose image is SimCity 4 itself.
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

# An earlier run left behind is the most likely reason a deploy fails, since
# the DLL is locked while the game holds it open.
$stale = Get-Process -ErrorAction SilentlyContinue |
    Where-Object { $_.Path -and $_.Path.EndsWith('SimCity 4.exe') }

if ($stale) {
    Write-Host "A previous SimCity 4 is still running (pid $($stale.Id -join ', ')). Stopping it."
    foreach ($p in $stale) { Stop-SimCity -Process $p }
    Start-Sleep -Seconds 1
}

if (-not $NoDeploy) {
    if (-not (Test-Path $builtDll)) { throw "No $Configuration build at $builtDll" }
    Copy-Item $builtDll $liveDll -Force
    Write-Host "deployed $Configuration scvk.dll ($((Get-Item $liveDll).Length) bytes)"
}

if (Test-Path $liveLog) { [System.IO.File]::Delete($liveLog) }

# Launched through Steam rather than directly.
#
# Running the executable itself does not work: it is wrapped in Steam's DRM,
# so it hands off to Steam and exits within a few seconds without ever loading
# a plugin. Going through the steam:// URL is the only route that actually
# starts the game, at the cost of one confirmation click per run.
$steamUrl = "steam://run/$AppId//" + ($gameArgs -join ' ') + '/'

Write-Host "launching through Steam"
Write-Host "  >>> confirm the launch in Steam if it asks <<<"
Start-Process $steamUrl | Out-Null

# Steam takes a while, and the confirmation is a human in the loop, so the
# timer only starts once the process actually exists.
$game = $null
$launchDeadline = (Get-Date).AddSeconds($LaunchTimeout)

while ((Get-Date) -lt $launchDeadline) {
    $game = Get-Process -ErrorAction SilentlyContinue |
        Where-Object { $_.Path -and $_.Path.EndsWith('SimCity 4.exe') } |
        Select-Object -First 1

    if ($game) { break }
    Start-Sleep -Milliseconds 500
}

if (-not $game) {
    throw "SimCity 4 did not start within $LaunchTimeout seconds. Was the Steam prompt confirmed?"
}

Write-Host "  started, pid $($game.Id); running for $Seconds seconds"

$deadline = (Get-Date).AddSeconds($Seconds)
$exitedEarly = $false

while ((Get-Date) -lt $deadline) {
    if ($game.HasExited) { $exitedEarly = $true; break }
    Start-Sleep -Milliseconds 500
}

if ($exitedEarly) {
    Write-Host "  the game exited on its own after $([int]((Get-Date) - $game.StartTime).TotalSeconds)s (code $($game.ExitCode))"
} else {
    Stop-SimCity -Process $game
}

# The log is flushed line by line, so it is complete the moment the process
# stops; no settling delay is needed.
if (-not (Test-Path $liveLog)) {
    Write-Warning "No log was produced at $liveLog. The DLL may not have been loaded at all."
    exit 1
}

if (-not (Test-Path $logDir)) { New-Item -ItemType Directory -Path $logDir | Out-Null }

$stamp    = Get-Date -Format 'yyyyMMdd-HHmmss'
$suffix   = if ($Label) { "-$Label" } else { '' }
$captured = Join-Path $logDir "scvk-$stamp$suffix.log"
Copy-Item $liveLog $captured

# The driver writes a BMP per dumped frame next to its log. Converted to PNG
# here purely because it is a more convenient format to look at.
Add-Type -AssemblyName System.Drawing
foreach ($bmp in Get-ChildItem $PluginsDir -Filter 'scvk-*.bmp' -ErrorAction SilentlyContinue) {
    $png = Join-Path $logDir (($bmp.BaseName) + "-$stamp$suffix.png")
    try {
        $img = [System.Drawing.Image]::FromFile($bmp.FullName)
        $img.Save($png, [System.Drawing.Imaging.ImageFormat]::Png)
        $img.Dispose()
        [System.IO.File]::Delete($bmp.FullName)
        Write-Host "  captured $png"
    } catch {
        Write-Host "  could not convert $($bmp.Name): $($_.Exception.Message)"
    }
}

$lines = [System.IO.File]::ReadAllLines($captured)

Write-Host ""
Write-Host "captured $captured  ($($lines.Count) lines)"
Write-Host ""

$device = $lines | Where-Object { $_ -match 'Vulkan: selected' } | Select-Object -First 1
if ($device) { Write-Host "  $($device.Trim())" }

$frames = $lines |
    Where-Object { $_ -match '(\d+) frames presented' } |
    ForEach-Object { [int][regex]::Match($_, '(\d+) frames presented').Groups[1].Value }

if ($frames) {
    Write-Host ("  frames presented : {0:N0}" -f ($frames | Measure-Object -Maximum).Maximum)
} else {
    Write-Host "  frames presented : none"
}

$errors = @($lines | Where-Object { $_ -match 'Vulkan ERROR|Vulkan warning' })
Write-Host "  validation       : $(if ($errors.Count) { "$($errors.Count) message(s)" } else { 'clean' })"

$dumps = @($lines | Where-Object { $_ -match 'dumping every draw of frame' })
Write-Host "  frame dumps      : $($dumps.Count)"

$notable = @($lines | Where-Object { $_ -match 'MISMATCH|LARGE DRAW|FATAL|not handled|could not|Falling back' })
if ($notable.Count) {
    Write-Host ""
    Write-Host "  notable:"
    $notable | Select-Object -First 8 | ForEach-Object { Write-Host "    $($_.Trim())" }
}

Write-Host ""
