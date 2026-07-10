# build.ps1 — configure + build + run the coverage_test host tool with MSVC + Ninja.
#
# Usage:  pwsh build.ps1
# Then:   (this script also runs it) — build\coverage_test.exe exits 0 on all-pass.
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

$cfg = "cmake -S `"$here`" -B `"$build`" -G Ninja -DCMAKE_BUILD_TYPE=Release"
$bld = "cmake --build `"$build`""
$run = Join-Path $build "coverage_test.exe"
cmd /c "call `"$vcvars`" >nul && $cfg && $bld && `"$run`""
$exitCode = $LASTEXITCODE
if ($exitCode -ne 0) { throw "coverage_test failed or reported a test failure (exit $exitCode)" }

Write-Host "`ncoverage_test: all pass"
