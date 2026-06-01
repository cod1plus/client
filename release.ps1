# Build + packaging d'une release cod1reloaded prete a uploader sur GitHub.
#
# Usage :
#   .\release.ps1                 (version lue depuis src/updater.h)
#   .\release.ps1 -Version 1.0.1  (override)
#
# Produit dans dist/ :
#   - mss32.dll                  (a uploader sur la Release)
#   - manifest.json              (a uploader sur la Release)
#   - cod1reloaded-<ver>.zip     (paquet joueur : dll + ini + LISEZMOI)
param([string]$Version = "")

$root = $PSScriptRoot

# --- Version : source de verite = src/updater.h ---
if (-not $Version) {
    $h = Get-Content (Join-Path $root "src\updater.h") -Raw
    if ($h -match 'COD1RELOADED_VERSION\s*=\s*"([0-9.]+)"') { $Version = $Matches[1] }
    else { Write-Error "Version introuvable dans src/updater.h"; exit 1 }
}
Write-Host "=== Release cod1reloaded v$Version ===" -ForegroundColor Cyan

# --- Toolchain MinGW 32-bit ---
$cands = @("$root\tools\mingw\bin", "C:\msys64\mingw32\bin", "C:\mingw32\bin")
$mingw = $cands | Where-Object { Test-Path (Join-Path $_ "i686-w64-mingw32-g++.exe") } | Select-Object -First 1
if (-not $mingw) { Write-Error "MinGW 32-bit introuvable"; exit 1 }
$cmake = Join-Path $mingw "cmake.exe"; if (-not (Test-Path $cmake)) { $cmake = "cmake.exe" }
$env:PATH = "$mingw;$env:PATH"

$build = Join-Path $root "build"

# --- Configure si besoin ---
if (-not (Test-Path (Join-Path $build "CMakeCache.txt"))) {
    Write-Host "Configuration CMake..."
    $a = @("-S","$root","-B","$build","-G","MinGW Makefiles","-DCMAKE_BUILD_TYPE=Release",
           "-DCMAKE_MAKE_PROGRAM=$mingw\mingw32-make.exe",
           "-DCMAKE_C_COMPILER=$mingw\i686-w64-mingw32-gcc.exe",
           "-DCMAKE_CXX_COMPILER=$mingw\i686-w64-mingw32-g++.exe")
    Start-Process $cmake -ArgumentList $a -NoNewWindow -Wait | Out-Null
}

# --- Build Release ---
Write-Host "Compilation..."
$p = Start-Process $cmake -ArgumentList "--build","$build","--config","Release" -NoNewWindow -PassThru -Wait
$dll = Join-Path $build "mss32.dll"
if (-not (Test-Path $dll)) { Write-Error "Build echoue : build\mss32.dll introuvable (exit $($p.ExitCode))"; exit 1 }

# --- dist/ ---
$dist = Join-Path $root "dist"
Remove-Item -Recurse -Force $dist -ErrorAction SilentlyContinue
New-Item -ItemType Directory -Force $dist | Out-Null
Copy-Item $dll (Join-Path $dist "mss32.dll")
Copy-Item (Join-Path $root "cod1reloaded.ini") (Join-Path $dist "cod1reloaded.ini")

$readme = @"
cod1reloaded $Version

Installation :
1. Copier mss32.dll et cod1reloaded.ini dans le dossier de Call of Duty
   (a cote de CoDMP.exe). Sauvegarder le mss32.dll d'origine au besoin.
2. Lancer le jeu. Les mises a jour s'installent automatiquement.

Desinstallation : supprimer mss32.dll et cod1reloaded.ini.
"@
Set-Content (Join-Path $dist "LISEZMOI.txt") $readme -Encoding ASCII

# --- manifest.json (dist + racine repo) ---
$manifest = @"
{
  "version": "$Version",
  "download_url": "https://github.com/cod1plus/client/releases/latest/download/mss32.dll",
  "notes": "cod1reloaded $Version"
}
"@
Set-Content (Join-Path $dist "manifest.json") $manifest -Encoding ASCII
Set-Content (Join-Path $root "manifest.json") $manifest -Encoding ASCII

# --- zip joueur ---
$zip = Join-Path $dist "cod1reloaded-$Version.zip"
Compress-Archive -Path (Join-Path $dist "mss32.dll"),(Join-Path $dist "cod1reloaded.ini"),(Join-Path $dist "LISEZMOI.txt") -DestinationPath $zip -Force

Write-Host ""
Write-Host "OK -> $dist" -ForegroundColor Green
Write-Host "Cree la Release GitHub 'v$Version' et glisse :" -ForegroundColor Yellow
Write-Host "  - mss32.dll"
Write-Host "  - manifest.json"
Write-Host "  - cod1reloaded-$Version.zip"
