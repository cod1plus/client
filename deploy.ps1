# Deploie le build frais dans le dossier de jeu.
# Usage : .\deploy.ps1   (ferme le jeu avant)
param(
    [string]$Game = "C:\Users\bitpo\OneDrive\Bureau\Call of Duty - R 1.5"
)

$build = Join-Path $PSScriptRoot "build\mss32.dll"
if (-not (Test-Path $build)) { Write-Host "build\mss32.dll introuvable - build d'abord."; exit 1 }

if (Get-Process CoDMP -ErrorAction SilentlyContinue) {
    Write-Host "CoDMP.exe tourne -> ferme le jeu, puis relance ce script."
    exit 1
}

Copy-Item $build (Join-Path $Game "mss32.dll") -Force
$g = Get-Item (Join-Path $Game "mss32.dll")
Write-Host ("OK -> mss32.dll deploye ({0} octets, {1})" -f $g.Length, $g.LastWriteTime)
