# Builds the Horizon CloudBeats desktop shell (app\) into a single .exe.
#
#   PS> .\scripts\build-app.ps1
#
# Output: app\build\Release\HorizonCloudBeats.exe
#
# Downloads the WebView2 SDK (NuGet package) on first run. The WebView2 runtime
# itself is preinstalled on Windows 10/11; nothing extra ships beside the .exe.

$ErrorActionPreference = "Stop"
$root = Split-Path -Parent $PSScriptRoot
$app  = Join-Path $root "app"
$wv2  = Join-Path $app  "third_party\webview2"

function Find-CMake {
    $cmd = Get-Command cmake -ErrorAction SilentlyContinue
    if ($cmd) { return $cmd.Source }
    foreach ($p in @(
        "${env:ProgramFiles}\CMake\bin\cmake.exe",
        "${env:ProgramFiles(x86)}\CMake\bin\cmake.exe")) {
        if (Test-Path $p) { return $p }
    }
    $vswhere = "${env:ProgramFiles(x86)}\Microsoft Visual Studio\Installer\vswhere.exe"
    if (Test-Path $vswhere) {
        foreach ($vs in (& $vswhere -all -products * -property installationPath)) {
            $p = Join-Path $vs "Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin\cmake.exe"
            if (Test-Path $p) { return $p }
        }
    }
    throw "cmake.exe not found. Install Visual Studio 2022 (Desktop C++) or CMake."
}

# --- WebView2 SDK -----------------------------------------------------------
if (-not (Test-Path (Join-Path $wv2 "include\WebView2.h"))) {
    Write-Host "-> downloading WebView2 SDK" -ForegroundColor Cyan
    $tmp = Join-Path $env:TEMP ("wv2-" + [guid]::NewGuid())
    New-Item -ItemType Directory -Force -Path $tmp | Out-Null
    $nupkg = Join-Path $tmp "webview2.zip"
    Invoke-WebRequest -Uri "https://www.nuget.org/api/v2/package/Microsoft.Web.WebView2" `
                      -OutFile $nupkg -UseBasicParsing
    Expand-Archive -Path $nupkg -DestinationPath $tmp -Force

    New-Item -ItemType Directory -Force -Path (Join-Path $wv2 "include") | Out-Null
    New-Item -ItemType Directory -Force -Path (Join-Path $wv2 "x64")     | Out-Null
    Copy-Item (Join-Path $tmp "build\native\include\*") (Join-Path $wv2 "include") -Recurse -Force
    Copy-Item (Join-Path $tmp "build\native\x64\WebView2LoaderStatic.lib") (Join-Path $wv2 "x64") -Force
    Remove-Item -Recurse -Force $tmp
    Write-Host "   WebView2 SDK staged in $wv2" -ForegroundColor DarkGray
}

# --- build ------------------------------------------------------------------
$cmake = Find-CMake
$build = Join-Path $app "build"
Write-Host "-> cmake configure" -ForegroundColor Cyan
& $cmake -S $app -B $build -A x64 | Out-Host
if ($LASTEXITCODE -ne 0) { throw "cmake configure failed" }
Write-Host "-> cmake build (Release)" -ForegroundColor Cyan
& $cmake --build $build --config Release | Out-Host
if ($LASTEXITCODE -ne 0) { throw "cmake build failed" }

$exe = Join-Path $build "Release\HorizonCloudBeats.exe"
Write-Host "`nBuilt: $exe" -ForegroundColor Green
