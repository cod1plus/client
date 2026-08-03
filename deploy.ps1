# Deploie le build frais dans le dossier de jeu.
# Usage : .\deploy.ps1            -> dossier de DEV (defaut)
#         .\deploy.ps1 -Live      -> dossier de jeu normal
#         .\deploy.ps1 -Game "C:\..."  -> un dossier precis
# Ferme le jeu avant.
param(
    [string]$Game,
    [switch]$Live,
    [switch]$Ini      # copie aussi cod1reloaded.ini (ecrase les reglages locaux)
)

$DevFolder  = "C:\Users\bitpo\OneDrive\Bureau\Call of Duty - R 1.6 - dev"
$LiveFolder = "C:\Users\bitpo\OneDrive\Bureau\Call of Duty - R 1.6"

if (-not $Game) { $Game = if ($Live) { $LiveFolder } else { $DevFolder } }

if (-not (Test-Path $Game)) { Write-Host "Dossier de jeu introuvable : $Game"; exit 1 }

$build = Join-Path $PSScriptRoot "build\mss32.dll"
if (-not (Test-Path $build)) { Write-Host "build\mss32.dll introuvable - build d'abord."; exit 1 }

if (Get-Process CoDMP -ErrorAction SilentlyContinue) {
    Write-Host "CoDMP.exe tourne -> ferme le jeu, puis relance ce script."
    exit 1
}

# Le proxy forwarde ses 728 exports vers mss32_original.dll : sans ce fichier le jeu
# ne demarre pas ("mss32.dll introuvable"), et l'erreur ne dit pas lequel manque.
if (-not (Test-Path (Join-Path $Game "mss32_original.dll"))) {
    Write-Host "ATTENTION : mss32_original.dll absent de $Game - le jeu ne demarrera pas."
}

Copy-Item $build (Join-Path $Game "mss32.dll") -Force
$g = Get-Item (Join-Path $Game "mss32.dll")
Write-Host ("OK -> {0}" -f $Game)
Write-Host ("     mss32.dll ({0} octets, {1})" -f $g.Length, $g.LastWriteTime)

if ($Ini) {
    Copy-Item (Join-Path $PSScriptRoot "cod1reloaded.ini") (Join-Path $Game "cod1reloaded.ini") -Force
    Write-Host "     cod1reloaded.ini copie"
}
