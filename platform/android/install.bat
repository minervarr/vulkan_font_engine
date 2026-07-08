@echo off
setlocal EnableDelayedExpansion

:: APK output root, relative to this script
set "APK_BASE=%~dp0app\build\outputs\apk"

:: ── 1. Verify ADB ────────────────────────────────────────────────────────────
where adb >nul 2>&1
if errorlevel 1 (
    echo ERROR: adb not found in PATH. Add Android SDK platform-tools to PATH.
    pause & exit /b 1
)

:: ── 2. Collect connected devices (only state == "device") ────────────────────
set "DC=0"
for /f "skip=1 tokens=1,2" %%A in ('adb devices 2^>nul') do (
    if "%%B"=="device" (
        set /a DC+=1
        set "DEV_!DC!=%%A"
    )
)
if !DC!==0 (
    echo ERROR: No devices or emulators connected.
    pause & exit /b 1
)

:: ── 3. Select device ─────────────────────────────────────────────────────────
if !DC!==1 (
    set "DEVICE=!DEV_1!"
) else (
    echo.
    echo Connected devices:
    for /l %%I in (1,1,!DC!) do echo   [%%I]  !DEV_%%I!
    echo.
    set /p "CHO=Select device number: "
    set /a "CHO=!CHO!"
    if !CHO! LSS 1 goto :err_input
    if !CHO! GTR !DC! goto :err_input
    call set "DEVICE=%%DEV_!CHO!%%"
)
echo Device:  !DEVICE!

:: ── 4. Auto-detect and normalise device ABI ──────────────────────────────────
for /f "tokens=*" %%A in ('adb -s !DEVICE! shell getprop ro.product.cpu.abi 2^>nul') do set "_RAW=%%A"
set "DEV_ABI="
for %%T in (arm64-v8a x86_64 armeabi-v7a x86) do (
    echo.!_RAW! | findstr /i "%%T" >nul 2>&1 && if "!DEV_ABI!"=="" set "DEV_ABI=%%T"
)
if "!DEV_ABI!"=="" set "DEV_ABI=!_RAW!"
echo ABI:     !DEV_ABI!

:: ── 5. Select build type ─────────────────────────────────────────────────────
echo.
echo   [1]  Debug    (default)
echo   [2]  Release
echo.
set /p "BC=Build type [1]: "
if "!BC!"=="" set "BC=1"

set "BTYPE="
set "BCMD="
if "!BC!"=="1" (
    set "BTYPE=debug"
    set "BCMD=assembleDebug"
)
if "!BC!"=="2" (
    set "BTYPE=release"
    set "BCMD=assembleRelease"
)
if not defined BTYPE goto :err_input

:: ── 6. Scan APK directory ────────────────────────────────────────────────────
set "APK_DIR=!APK_BASE!\!BTYPE!"
if not exist "!APK_DIR!" (
    echo.
    echo No output found at: !APK_DIR!
    echo Build first:  gradlew !BCMD!
    pause & exit /b 1
)

set "AC=0"
set "BEST=0"
for %%F in ("!APK_DIR!\*.apk") do (
    set /a AC+=1
    set "APK_!AC!=%%~fF"
    set "APKN_!AC!=%%~nxF"
    echo.%%~nF | findstr /i "!DEV_ABI!" >nul 2>&1 && set "BEST=!AC!"
)
if !AC!==0 (
    echo No .apk files in !APK_DIR!
    echo Build first:  gradlew !BCMD!
    pause & exit /b 1
)

:: ── 7. Select APK ────────────────────────────────────────────────────────────
if !AC!==1 (
    set "APK=!APK_1!"
    set "APKN=!APKN_1!"
) else (
    echo.
    echo Available APKs  (device ABI: !DEV_ABI!):
    for /l %%I in (1,1,!AC!) do (
        if %%I==!BEST! (
            echo   [%%I]  !APKN_%%I!  ^<-- matches device
        ) else (
            echo   [%%I]  !APKN_%%I!
        )
    )
    echo.
    if !BEST! GTR 0 (
        set /p "AC_CHO=Select APK [!BEST!]: "
        if "!AC_CHO!"=="" set "AC_CHO=!BEST!"
    ) else (
        set /p "AC_CHO=Select APK: "
    )
    set /a "AC_CHO=!AC_CHO!"
    if !AC_CHO! LSS 1 goto :err_input
    if !AC_CHO! GTR !AC! goto :err_input
    call set "APK=%%APK_!AC_CHO!%%"
    call set "APKN=%%APKN_!AC_CHO!%%"
)

:: ── 8. Install ───────────────────────────────────────────────────────────────
echo.
echo Installing: !APKN!
echo On:         !DEVICE!
echo.
adb -s !DEVICE! install -r "!APK!"
if errorlevel 1 (
    echo.
    echo FAILED.
    pause & exit /b 1
)
echo.
echo Done.
pause
exit /b 0

:err_input
echo Invalid selection.
pause
exit /b 1
