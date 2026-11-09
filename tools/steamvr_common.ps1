# Helpers shared by register_driver.ps1 / unregister_driver.ps1.

function Get-SteamPath {
    $key = Get-ItemProperty "HKCU:\Software\Valve\Steam" -ErrorAction SilentlyContinue
    if (-not $key -or -not $key.SteamPath) { throw "Steam installation not found (HKCU\Software\Valve\Steam\SteamPath)." }
    return ($key.SteamPath -replace "/", "\")
}

function Get-SteamVRDir {
    $candidates = @()

    # 1) Runtime currently registered with OpenVR (most reliable).
    $vrpath = Join-Path $env:LOCALAPPDATA "openvr\openvrpaths.vrpath"
    if (Test-Path $vrpath) {
        try { $candidates += @((Get-Content -Raw $vrpath | ConvertFrom-Json).runtime) } catch { }
    }

    # 2) Default Steam library.
    $steam = Get-SteamPath
    $candidates += (Join-Path $steam "steamapps\common\SteamVR")

    # Additional Steam libraries are listed in libraryfolders.vdf as  "path"  "D:\\SteamLibrary"
    $vdf = Join-Path $steam "steamapps\libraryfolders.vdf"
    if (Test-Path $vdf) {
        foreach ($m in (Select-String -Path $vdf -Pattern '"path"\s+"([^"]+)"' -AllMatches).Matches) {
            $lib = $m.Groups[1].Value -replace "\\\\", "\"
            $candidates += (Join-Path $lib "steamapps\common\SteamVR")
        }
    }
    foreach ($c in $candidates) {
        if (Test-Path (Join-Path $c "bin\win64\vrpathreg.exe")) { return $c }
    }
    throw "SteamVR not found. Install SteamVR from Steam first."
}

function Get-VrPathReg {
    return (Join-Path (Get-SteamVRDir) "bin\win64\vrpathreg.exe")
}

function Get-SteamVRSettingsPath {
    $path = Join-Path (Get-SteamPath) "config\steamvr.vrsettings"
    if (-not (Test-Path $path)) { Set-Content -Path $path -Value "{}" -Encoding ASCII }
    return $path
}

function Assert-SteamVRNotRunning {
    if (Get-Process -Name vrserver, vrmonitor -ErrorAction SilentlyContinue) {
        throw "SteamVR is running. Close it first (it rewrites steamvr.vrsettings on exit)."
    }
}

function Backup-File([string]$path) {
    $backup = "$path.tubane-backup-$(Get-Date -Format yyyyMMdd-HHmmss)"
    Copy-Item $path $backup
    Write-Host "backup    : $backup"
}

function Read-JsonObject([string]$path) {
    $text = Get-Content -Raw -Path $path
    if ([string]::IsNullOrWhiteSpace($text)) { return [pscustomobject]@{} }
    return ($text | ConvertFrom-Json)
}

function Write-JsonObject($object, [string]$path) {
    $json = $object | ConvertTo-Json -Depth 32
    # UTF-8 without BOM (SteamVR's JSON parser does not expect a BOM).
    [System.IO.File]::WriteAllText($path, $json, (New-Object System.Text.UTF8Encoding($false)))
}

function Set-JsonProperty($object, [string]$name, $value) {
    if ($object.PSObject.Properties[$name]) { $object.$name = $value }
    else { $object | Add-Member -NotePropertyName $name -NotePropertyValue $value }
}
