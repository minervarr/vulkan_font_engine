# build.ps1 — configure + build the host atlas_gen tool with MSVC + Ninja.
#
# Usage:  pwsh build.ps1            # builds into .\build, prints the exe path
# Then:   .\build\atlas_gen.exe <roman.otf> <bold.otf> <italic.otf> <math.otf> out.msdf out.rgba
#
# Needs the Visual Studio Build Tools (cl) — we shell through vcvars64.bat — plus
# cmake and ninja on PATH.
$ErrorActionPreference = "Stop"
$here = Split-Path -Parent $MyInvocation.MyCommand.Path
$build = Join-Path $here "build"

# Locate vcvars64.bat via vswhere (any VS edition with the C++ desktop tools).
$vcvars = $null
$vswhere = "${env:ProgramFiles(x86)}\Microsoft Visual Studio\Installer\vswhere.exe"
if (Test-Path $vswhere) {
    $vsPath = & $vswhere -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath | Select-Object -First 1
    if ($vsPath) {
        $candidate = Join-Path $vsPath "VC\Auxiliary\Build\vcvars64.bat"
        if (Test-Path $candidate) { $vcvars = $candidate }
    }
}
if (-not $vcvars) { throw "vcvars64.bat not found via vswhere — install VS Build Tools with the C++ desktop workload." }

# Configure + build inside the MSVC environment (one cmd so the env persists).
$cfg = "cmake -S `"$here`" -B `"$build`" -G Ninja -DCMAKE_BUILD_TYPE=Release"
$bld = "cmake --build `"$build`""
cmd /c "call `"$vcvars`" >nul && $cfg && $bld"
if ($LASTEXITCODE -ne 0) { throw "build failed ($LASTEXITCODE)" }

Write-Host "`natlas_gen built: $(Join-Path $build 'atlas_gen.exe')"
