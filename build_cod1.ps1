param(
    [ValidateSet("Release", "Debug")]
    [string]$Configuration = "Release",
    [switch]$Clean
)

$ErrorActionPreference = "Stop"

$repoRoot = Split-Path -Parent $MyInvocation.MyCommand.Path
$buildDir = Join-Path $repoRoot "build"

function Find-Tool {
    param(
        [string]$Name,
        [string[]]$Candidates
    )

    foreach ($candidate in $Candidates) {
        if ($candidate -and (Test-Path $candidate)) {
            return (Resolve-Path $candidate).Path
        }
    }

    return $null
}

$binCandidates = @(
    (Join-Path $repoRoot "tools\mingw\bin"),
    (Join-Path $repoRoot "cod2x\tools\mingw\bin"),
    "C:\msys64\mingw32\bin",
    "C:\mingw32\bin"
)

$cmakeCandidates = @(
    (Join-Path $repoRoot "tools\mingw\bin\cmake.exe"),
    (Join-Path $repoRoot "cod2x\tools\mingw\bin\cmake.exe"),
    "C:\msys64\mingw32\bin\cmake.exe",
    "C:\Program Files\CMake\bin\cmake.exe",
    "C:\Program Files (x86)\CMake\bin\cmake.exe"
)

$makeCandidates = @(
    (Join-Path $repoRoot "tools\mingw\bin\mingw32-make.exe"),
    (Join-Path $repoRoot "cod2x\tools\mingw\bin\mingw32-make.exe"),
    "C:\msys64\mingw32\bin\mingw32-make.exe",
    "C:\mingw32\bin\mingw32-make.exe"
)

$mingwBin = Find-Tool -Name "MinGW bin" -Candidates $binCandidates
$cmakeExe = Find-Tool -Name "cmake.exe" -Candidates $cmakeCandidates
$makeExe = Find-Tool -Name "mingw32-make.exe" -Candidates $makeCandidates

if (-not $mingwBin) {
    throw @"
Impossible de trouver un toolchain MinGW 32-bit.

Emplacements cherches :
 - .\tools\mingw\bin
 - .\cod2x\tools\mingw\bin
 - C:\msys64\mingw32\bin
 - C:\mingw32\bin

Installe un MinGW-w64 i686 (WinLibs ou MSYS2) puis relance ce script.
"@
}

if (-not $cmakeExe) {
    throw @"
Impossible de trouver cmake.exe.

Installe CMake, ou utilise un package MSYS2/WinLibs qui fournit cmake.exe.
"@
}

if (-not $makeExe) {
    throw @"
Impossible de trouver mingw32-make.exe.

Installe le package make de MinGW 32-bit, puis relance ce script.
"@
}

$gccExe = Join-Path $mingwBin "i686-w64-mingw32-gcc.exe"
$gxxExe = Join-Path $mingwBin "i686-w64-mingw32-g++.exe"

if (-not (Test-Path $gccExe)) {
    $gccExe = Join-Path $mingwBin "gcc.exe"
}

if (-not (Test-Path $gxxExe)) {
    $gxxExe = Join-Path $mingwBin "g++.exe"
}

if (-not (Test-Path $gccExe) -or -not (Test-Path $gxxExe)) {
    throw "Impossible de trouver un compilateur C/C++ 32-bit dans '$mingwBin'."
}

$env:PATH = "$mingwBin;$env:PATH"

if ($Clean -and (Test-Path $buildDir)) {
    Remove-Item -Recurse -Force $buildDir
}

& $cmakeExe -S $repoRoot -B $buildDir `
    -G "MinGW Makefiles" `
    -DCMAKE_BUILD_TYPE=$Configuration `
    -DCMAKE_MAKE_PROGRAM="$makeExe" `
    -DCMAKE_C_COMPILER="$gccExe" `
    -DCMAKE_CXX_COMPILER="$gxxExe"

if ($LASTEXITCODE -ne 0) {
    throw "La configuration CMake a echoue (code $LASTEXITCODE)."
}

& $cmakeExe --build $buildDir --config $Configuration

if ($LASTEXITCODE -ne 0) {
    throw "La compilation a echoue (code $LASTEXITCODE)."
}

$dllPath = Join-Path $buildDir "cod1reloaded.dll"
$proxyPath = Join-Path $buildDir "mss32.dll"

if (-not (Test-Path $dllPath)) {
    throw "Build terminee, mais '$dllPath' est introuvable."
}

Write-Host ""
Write-Host "Build OK:" $dllPath -ForegroundColor Green
if (Test-Path $proxyPath) {
    Write-Host "Proxy OK:" $proxyPath -ForegroundColor Green
}
