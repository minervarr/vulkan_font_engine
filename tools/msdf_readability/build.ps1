# build.ps1 — configure + build the host msdf_readability tool with MSVC + Ninja.
#
# Usage:  pwsh build.ps1
# Then:   .\build\msdf_readability.exe <font.otf> [em=40] [threshold=0.08] [minSize=6] [maxSize=32] [outDir=out]
#
# Needs the Visual Studio Build Tools (cl) — we shell through vcvars64.bat — plus
# cmake and ninja on PATH.
$ErrorActionPreference = "Stop"
$here = Split-Path -Parent $MyInvocation.MyCommand.Path
$build = Join-Path $here "build"

$vcvars = "C:\Program Files (x86)\Microsoft Visual Studio\18\BuildTools\VC\Auxiliary\Build\vcvars64.bat"
if (-not (Test-Path $vcvars)) { throw "vcvars64.bat not found at $vcvars — edit this script to point at your VS install." }

$cfg = "cmake -S `"$here`" -B `"$build`" -G Ninja -DCMAKE_BUILD_TYPE=Release"
$bld = "cmake --build `"$build`""
cmd /c "call `"$vcvars`" >nul && $cfg && $bld"
if ($LASTEXITCODE -ne 0) { throw "build failed ($LASTEXITCODE)" }

Write-Host "`nmsdf_readability built: $(Join-Path $build 'msdf_readability.exe')"
