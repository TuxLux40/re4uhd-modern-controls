# RE4 Modern Controls installer for Windows. Start it by double-clicking install.bat.
#   install.bat              install or update, then choose the settings
#   install.bat -Defaults    install with the recommended settings, no questions
#   install.bat -Uninstall   remove the mod
#   install.bat -GameDir "D:\Games\Steam\steamapps\common\Resident Evil 4"
param(
    [switch]$Uninstall,
    [switch]$Defaults,
    [string]$GameDir = ""
)
$ErrorActionPreference = "Stop"

$AppId = 254700
$SupportedSha256 = "19AED4AF0AB06A748FF8744D45AC5580FCD6BE6B6B7E944B1AB8822A00C8EE4A"
$Here = Split-Path -Parent $MyInvocation.MyCommand.Path
$DllSrc = Join-Path $Here "files\xinput1_3.dll"
$IniTemplate = Join-Path $Here "files\default.ini"

function Fail($msg) { Write-Host ""; Write-Host "Error: $msg" -ForegroundColor Red; exit 1 }

# ------------------------------------------------------------------------------------------- find Steam and RE4
function Get-SteamLibraries {
    $roots = @()
    foreach ($key in "HKCU:\Software\Valve\Steam", "HKLM:\SOFTWARE\WOW6432Node\Valve\Steam", "HKLM:\SOFTWARE\Valve\Steam") {
        $p = Get-ItemProperty -Path $key -ErrorAction SilentlyContinue
        if ($p -and $p.SteamPath) { $roots += ($p.SteamPath -replace "/", "\") }
        if ($p -and $p.InstallPath) { $roots += $p.InstallPath }
    }
    $libs = @()
    foreach ($r in ($roots | Select-Object -Unique)) {
        $libs += $r
        $vdf = Join-Path $r "steamapps\libraryfolders.vdf"
        if (Test-Path $vdf) {
            foreach ($m in [regex]::Matches((Get-Content $vdf -Raw), '"path"\s+"([^"]+)"')) {
                $libs += ($m.Groups[1].Value -replace "\\\\", "\")
            }
        }
    }
    return $libs | Select-Object -Unique
}

if (-not $GameDir) {
    foreach ($lib in Get-SteamLibraries) {
        $acf = Join-Path $lib "steamapps\appmanifest_$AppId.acf"
        if (-not (Test-Path $acf)) { continue }
        $m = [regex]::Match((Get-Content $acf -Raw), '"installdir"\s+"([^"]+)"')
        if (-not $m.Success) { continue }
        $dir = Join-Path $lib ("steamapps\common\" + $m.Groups[1].Value)
        if (Test-Path (Join-Path $dir "Bin32\bio4.exe")) { $GameDir = $dir; break }
    }
}
if (-not $GameDir) {
    Fail "Resident Evil 4 (2005) was not found in your Steam libraries.`nRun it again as: install.bat -GameDir `"C:\path\to\Resident Evil 4`""
}
$Bin = Join-Path $GameDir "Bin32"
if (-not (Test-Path (Join-Path $Bin "bio4.exe"))) { Fail "$Bin\bio4.exe not found. Is this the Resident Evil 4 folder?" }
if (Get-Process -Name bio4 -ErrorAction SilentlyContinue) { Fail "Resident Evil 4 is running. Quit the game first." }

$Dll = Join-Path $Bin "xinput1_3.dll"
$Backup = Join-Path $Bin "xinput1_3.dll.re4mc-backup"
$Ini = Join-Path $Bin "RE4ModernControls.ini"

function Test-Ours($path) {
    if (-not (Test-Path $path)) { return $false }
    return [System.Text.Encoding]::ASCII.GetString([System.IO.File]::ReadAllBytes($path)).Contains("RE4 Modern Controls")
}

Write-Host "RE4 Modern Controls"
Write-Host "Game folder: $GameDir"

# ------------------------------------------------------------------------------------------- uninstall
if ($Uninstall) {
    if (Test-Ours $Dll) { Remove-Item $Dll; Write-Host "Removed the mod." } else { Write-Host "The mod is not installed." }
    if (Test-Path $Backup) { Move-Item $Backup $Dll; Write-Host "Restored your previous xinput1_3.dll." }
    Remove-Item (Join-Path $Bin "RE4ModernControls.ini"), (Join-Path $Bin "RE4ModernControls.log") -ErrorAction SilentlyContinue
    Write-Host "Done."
    exit 0
}

# ------------------------------------------------------------------------------------------- install
if (-not (Test-Path $DllSrc)) { Fail "files\xinput1_3.dll is missing next to this script. Extract the whole zip first." }
$sum = (Get-FileHash (Join-Path $Bin "bio4.exe") -Algorithm SHA256).Hash
if ($sum -ne $SupportedSha256) {
    Write-Host ""
    Write-Host "Warning: this bio4.exe is not the version the mod supports (Steam, update 1.1.0)." -ForegroundColor Yellow
    Write-Host "The mod will load but stay inactive, and the game plays as usual."
    if (-not $Defaults) {
        $a = Read-Host "Install anyway? [y/N]"
        if ($a -notmatch "^[Yy]") { exit 1 }
    }
}
if ((Test-Path $Dll) -and -not (Test-Ours $Dll)) {
    Move-Item $Dll $Backup -Force
    Write-Host "Kept your previous xinput1_3.dll as xinput1_3.dll.re4mc-backup."
}
Copy-Item $DllSrc $Dll -Force
Write-Host "Installed the mod."

# ------------------------------------------------------------------------------------------- settings
function Get-Current($key, $default) {
    if (Test-Path $Ini) {
        $m = [regex]::Match((Get-Content $Ini -Raw), "(?m)^$key=(.*?)\r?$")
        if ($m.Success) { return $m.Groups[1].Value.Trim() }
    }
    return $default
}
$run = Get-Current "Run" "Always"; $walk = Get-Current "Walk" "1"; $cross = Get-Current "Crosshair" "1"
$height = Get-Current "Height" "0"; $invy = Get-Current "InvertY" "0"

function Ask-Choice($question, $default, [string[]]$options) {
    Write-Host ""; Write-Host $question
    for ($i = 0; $i -lt $options.Count; $i++) { Write-Host ("  {0}) {1}" -f ($i + 1), $options[$i]) }
    $a = Read-Host "Choose [$default]"
    if ($a) { return $a } else { return "$default" }
}
function Ask-YesNo($question, $default) {
    $hint = if ($default -eq "0") { "y/N" } else { "Y/n" }
    $a = Read-Host "$question [$hint]"
    if ($a -match "^[Yy]") { return "1" }
    if ($a -match "^[Nn]") { return "0" }
    return $default
}

if (-not $Defaults) {
    Write-Host ""
    Write-Host "Settings (press Enter to keep the value in brackets; run the installer again to change them later)"
    $d = switch ($run) { "OuterRing" { 2 } "Button" { 3 } default { 1 } }
    $run = switch (Ask-Choice "How should Leon run?" $d @(
            "Always run, hold the run button to walk (recommended)",
            "Run when the stick is pushed all the way, walk below that",
            "Classic: hold the run button to run")) { "2" { "OuterRing" } "3" { "Button" } default { "Always" } }
    Write-Host ""
    $walk = Ask-YesNo "Walk with the left stick while aiming?" $walk
    $cross = Ask-YesNo "Show a crosshair where the laser points?" $cross
    $d = if ($height -eq "0") { 1 } else { 2 }
    $height = switch (Ask-Choice "Camera height" $d @(
            "Same height as the original game (recommended)",
            "A bit higher")) { "2" { "250" } default { "0" } }
    Write-Host ""
    $invy = Ask-YesNo "Invert camera up/down?" $invy
}

# Start from the template (every option commented = recommended default) and set only what differs.
$text = (Get-Content $IniTemplate -Raw) -replace "\r\n", "`n"
function Set-Key($key, $value) { $script:text = [regex]::Replace($script:text, "(?m)^;?$key=.*$", "$key=$value") }
if ($run -ne "Always") { Set-Key "Run" $run }
if ($walk -ne "1") { Set-Key "Walk" $walk }
if ($cross -ne "1") { Set-Key "Crosshair" $cross }
if ($height -ne "0") { Set-Key "Height" $height }
if ($invy -ne "0") { Set-Key "InvertY" $invy }
[System.IO.File]::WriteAllText($Ini, ($text -replace "`n", "`r`n"), (New-Object System.Text.UTF8Encoding $false))
Write-Host ""
Write-Host "Saved settings to $Ini"
Write-Host ""
Write-Host "Done. Start Resident Evil 4 from Steam as usual." -ForegroundColor Green
