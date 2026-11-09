<#
.SYNOPSIS
    Registers the tubane driver package with SteamVR (vrpathreg adddriver).

.DESCRIPTION
    Points SteamVR at <BuildDir>\package\tubane so every rebuild is picked up
    on the next SteamVR start without copying files.

    With -ForceDriver the script also edits Steam\config\steamvr.vrsettings:
        steamvr.forcedDriver           = "tubane"
        steamvr.activateMultipleDrivers = true
    so SteamVR uses the tubane HMD even when a real headset (or another
    driver) is present. A timestamped backup of steamvr.vrsettings is made
    first. Close SteamVR before running with -ForceDriver.

.EXAMPLE
    powershell -ExecutionPolicy Bypass -File tools\register_driver.ps1
.EXAMPLE
    powershell -ExecutionPolicy Bypass -File tools\register_driver.ps1 -BuildDir build\vs2022 -ForceDriver
#>
param(
    [string]$BuildDir = "build\vs2026",
    [switch]$ForceDriver
)

$ErrorActionPreference = "Stop"
. "$PSScriptRoot\steamvr_common.ps1"

$repoRoot   = Split-Path -Parent $PSScriptRoot
$packageDir = Join-Path (Join-Path $repoRoot $BuildDir) "package\tubane"
$dll        = Join-Path $packageDir "bin\win64\driver_tubane.dll"

if (-not (Test-Path $dll)) {
    throw "Driver DLL not found: $dll`nBuild first: cmake --preset vs2026 ; cmake --build --preset vs2026-release"
}
$packageDir = (Resolve-Path $packageDir).Path

$vrpathreg = Get-VrPathReg
Write-Host "vrpathreg : $vrpathreg"
Write-Host "package   : $packageDir"

& $vrpathreg adddriver $packageDir
if ($LASTEXITCODE -ne 0) { throw "vrpathreg adddriver failed (exit $LASTEXITCODE)" }

if ($ForceDriver) {
    Assert-SteamVRNotRunning
    $settingsPath = Get-SteamVRSettingsPath
    Backup-File $settingsPath

    $settings = Read-JsonObject $settingsPath
    if (-not $settings.PSObject.Properties["steamvr"]) {
        $settings | Add-Member -NotePropertyName steamvr -NotePropertyValue ([pscustomobject]@{})
    }
    Set-JsonProperty $settings.steamvr "forcedDriver" "tubane"
    Set-JsonProperty $settings.steamvr "activateMultipleDrivers" $true
    Write-JsonObject $settings $settingsPath
    Write-Host "steamvr.vrsettings updated: forcedDriver=tubane, activateMultipleDrivers=true"
}

Write-Host ""
Write-Host "Registered drivers:"
& $vrpathreg show
Write-Host ""
Write-Host "Done. Start SteamVR; the log is at <Steam>\logs\vrserver.txt (search for [tubane])."
