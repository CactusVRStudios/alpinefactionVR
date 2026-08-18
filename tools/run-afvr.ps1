[CmdletBinding()]
param(
    [string]$GameDir = $(if ($env:AFVR_GAME_DIR) { $env:AFVR_GAME_DIR } else { "D:\Red Faction" }),
    [string]$BuildDir = $(Join-Path (Split-Path $PSScriptRoot -Parent) "build-afvr-on\bin\Release"),
    [string]$Level = "L1S1.rfl"
)

$ErrorActionPreference = "Stop"

$gameExe = Join-Path $GameDir "RF_120na.exe"
$launcherExe = Join-Path $BuildDir "AlpineFactionLauncher.exe"
$patchDll = Join-Path $BuildDir "AlpineFaction.dll"

$requiredPaths = @($gameExe, $launcherExe, $patchDll)
$missingPaths = @($requiredPaths | Where-Object { -not (Test-Path -LiteralPath $_ -PathType Leaf) })
if ($missingPaths.Count -gt 0) {
    Write-Error ("AFVR validation prerequisites are missing:`n  " + ($missingPaths -join "`n  "))
}

$activeRuntime = $null
$runtimeKeys = @(
    "HKLM:\SOFTWARE\Khronos\OpenXR\1",
    "HKLM:\SOFTWARE\WOW6432Node\Khronos\OpenXR\1"
)
foreach ($runtimeKey in $runtimeKeys) {
    if (-not $activeRuntime) {
        $activeRuntime = (Get-ItemProperty -LiteralPath $runtimeKey -Name ActiveRuntime -ErrorAction SilentlyContinue).ActiveRuntime
    }
}

$dllHash = (Get-FileHash -LiteralPath $patchDll -Algorithm SHA256).Hash
$launcherHash = (Get-FileHash -LiteralPath $launcherExe -Algorithm SHA256).Hash
$launchArguments = @("-game", "-exe-path", "`"$gameExe`"", "-vr", "-level", $Level)

Write-Host "AFVR developer validation passed."
Write-Host "  Game:         $gameExe"
Write-Host "  Launcher:     $launcherExe"
Write-Host "  Patch:        $patchDll"
Write-Host "  Runtime:      $(if ($activeRuntime) { $activeRuntime } else { '<not found in registry; live VR will fall back to flat mode>' })"
Write-Host "  Level:        $Level"
Write-Host "  Launcher SHA: $launcherHash"
Write-Host "  Patch SHA:    $dllHash"
Write-Host "Would launch: $launcherExe $($launchArguments -join ' ')"
Write-Host "Validation only: this script never starts the launcher or game."
