<#
.SYNOPSIS
    Unregisters the tubane driver from SteamVR and (optionally) undoes -ForceDriver.

.EXAMPLE
    powershell -ExecutionPolicy Bypass -File tools\unregister_driver.ps1
.EXAMPLE
    powershell -ExecutionPolicy Bypass -File tools\unregister_driver.ps1 -RemoveForcedDriver
#>
param(
    [string]$BuildDir = "build\vs2026",
    [switch]$RemoveForcedDriver
)

$ErrorActionPreference = "Stop"
. "$PSScriptRoot\steamvr_common.ps1"

$repoRoot   = Split-Path -Parent $PSScriptRoot
$packageDir = Join-Path (Join-Path $repoRoot $BuildDir) "package\tubane"

$vrpathreg = Get-VrPathReg
& $vrpathreg removedriver $packageDir
if ($LASTEXITCODE -ne 0) { Write-Warning "vrpathreg removedriver returned $LASTEXITCODE (driver may not have been registered)" }

if ($RemoveForcedDriver) {
    Assert-SteamVRNotRunning
    $settingsPath = Get-SteamVRSettingsPath
    $settings = Read-JsonObject $settingsPath
    if ($settings.PSObject.Properties["steamvr"] -and
        $settings.steamvr.PSObject.Properties["forcedDriver"] -and
        $settings.steamvr.forcedDriver -eq "tubane") {
        Backup-File $settingsPath
        $settings.steamvr.PSObject.Properties.Remove("forcedDriver")
        Write-JsonObject $settings $settingsPath
        Write-Host "Removed steamvr.forcedDriver (activateMultipleDrivers left unchanged)."
    } else {
        Write-Host "steamvr.forcedDriver is not 'tubane'; nothing to change."
    }
}

& $vrpathreg show
