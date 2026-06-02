# One-shot build script. Configures CMake (Release, x64), compiles, then
# stages everything that needs to ship in dist\.
#
#   PS> .\scripts\build.ps1
#
# Output:
#   dist\version.dll            the proxy DLL (drops next to forzahorizon6.exe)
#   dist\fh6-radio\ui\          dashboard (mounted at http://localhost:<port>)
#   dist\fh6-radio\config.toml  seeded from config.example.toml

$ErrorActionPreference = "Stop"
$root  = Split-Path -Parent $PSScriptRoot
$build = Join-Path $root "build"
$dist  = Join-Path $root "dist"

# Locate cmake.exe. Prefer the one on PATH; otherwise look inside any VS
# install (which always ships CMake when the C++ workload is selected),
# then fall back to the standalone CMake installer's default location.
function Find-CMake {
    $cmd = Get-Command cmake -ErrorAction SilentlyContinue
    if ($cmd) { return $cmd.Source }

    $vswhere = "${env:ProgramFiles(x86)}\Microsoft Visual Studio\Installer\vswhere.exe"
    if (Test-Path $vswhere) {
        $vsRoots = & $vswhere -all -products * -property installationPath
        foreach ($vs in $vsRoots) {
            $p = Join-Path $vs "Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin\cmake.exe"
            if (Test-Path $p) { return $p }
        }
    }
    foreach ($p in @(
        "${env:ProgramFiles}\CMake\bin\cmake.exe",
        "${env:ProgramFiles(x86)}\CMake\bin\cmake.exe"
    )) { if (Test-Path $p) { return $p } }

    throw @"
cmake.exe not found. Either:
  - install Visual Studio 2022/2026 with the "Desktop development with C++"
    workload (CMake is bundled), or
  - install CMake from https://cmake.org/download/ (tick "Add CMake to PATH").
"@
}

$cmake = Find-CMake
Write-Host "Using cmake: $cmake" -ForegroundColor DarkGray

if (-not (Test-Path (Join-Path $root "third_party\nlohmann\nlohmann\json.hpp"))) {
    Write-Host "third_party/ is empty -- running get-deps.ps1 first." -ForegroundColor Yellow
    & (Join-Path $PSScriptRoot "get-deps.ps1")
}

Write-Host "-> cmake configure" -ForegroundColor Cyan
& $cmake -S $root -B $build -A x64 | Out-Host
if ($LASTEXITCODE -ne 0) { throw "cmake configure failed" }

Write-Host "-> cmake build (Release)" -ForegroundColor Cyan
& $cmake --build $build --config Release | Out-Host
if ($LASTEXITCODE -ne 0) { throw "cmake build failed" }

if (Test-Path $dist) { Remove-Item -Recurse -Force $dist }
New-Item -ItemType Directory -Force -Path $dist | Out-Null
New-Item -ItemType Directory -Force -Path (Join-Path $dist "fh6-radio") | Out-Null

Copy-Item (Join-Path $build "Release\version.dll") $dist
Copy-Item -Recurse (Join-Path $root "ui\dist") (Join-Path $dist "fh6-radio\ui")
# Overlay assets (logo PNGs etc.) into the dashboard directory so they're
# served alongside index.html. The assets/ folder is the canonical home for
# these files; copies in ui/dist/ are just build artefacts.
$assetsDir = Join-Path $root "assets"
if (Test-Path $assetsDir) {
    Copy-Item -Force (Join-Path $assetsDir "*.png") (Join-Path $dist "fh6-radio\ui") -ErrorAction SilentlyContinue
    Write-Host "-> assets overlayed from assets\" -ForegroundColor DarkGray
}
Copy-Item (Join-Path $root "config.example.toml") (Join-Path $dist "fh6-radio\config.toml")

Copy-Item (Join-Path $PSScriptRoot "dist-readme.txt") (Join-Path $dist "README.txt")

$mediaDir = Join-Path $root "media"
if (Test-Path $mediaDir) {
    Copy-Item -Recurse -Force $mediaDir (Join-Path $dist "media")
    Write-Host "-> media overlay bundled from repo media\" -ForegroundColor DarkGray
} else {
    Write-Host "-> media\ not found in repo; run fetch-media.ps1 separately if needed." -ForegroundColor Yellow
}

# Desktop shell app (optional): bundle it if it has been built. Build it with
# scripts\build-app.ps1.
$appExe = Join-Path $root "app\build\Release\HorizonCloudBeats.exe"
if (Test-Path $appExe) {
    Copy-Item $appExe (Join-Path $dist "HorizonCloudBeats.exe") -Force
    Write-Host "-> desktop app bundled (HorizonCloudBeats.exe)" -ForegroundColor DarkGray
} else {
    Write-Host "-> desktop app not built; run scripts\build-app.ps1 to include it." -ForegroundColor Yellow
}

Write-Host "`nBuilt + staged in $dist" -ForegroundColor Green
Get-ChildItem -Recurse -File $dist | ForEach-Object {
    "  $($_.FullName.Substring($dist.Length + 1))"
}
