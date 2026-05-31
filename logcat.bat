@echo off
setlocal enabledelayedexpansion

:: Logcat for vk_font_android (io.nava.vkfont)
:: Shows everything from the app process + Vulkan validation output.

set ADB=adb
set PKG=io.nava.vkfont
set TARGET=

:: -------------------------------------------------------------------
:: DEVICE SELECTION LOGIC
:: -------------------------------------------------------------------
if not "%1"=="" (
    :: If a serial was passed directly via command line argument, use it
    set TARGET=-s %1
    goto run_logcat
)

echo Detecting connected Android devices...
set count=0

:: Parse active devices from 'adb devices' output
for /f "tokens=1,2" %%i in ('%ADB% devices 2^>nul') do (
    if "%%j"=="device" (
        set /a count+=1
        set "dev!count!=%%i"
    )
)

if %count%==0 (
    echo [ERROR] No Android devices found. Make sure USB debugging is on.
    exit /b 1
)

if %count%==1 (
    :: Only one device connected? Skip the menu and use it automatically.
    echo Only one device found. Auto-selecting !dev1!.
    set TARGET=-s !dev1!
    goto run_logcat
)

:: Interactive Menu for Multiple Devices
echo.
echo Multiple devices detected. Please select one:
for /l %%x in (1, 1, %count%) do (
    echo   [%%x] !dev%%x!
)
echo.

:ask_choice
set /p "choice=Enter device number (1-%count%): "
if not defined dev%choice% (
    echo Invalid choice. Try again.
    goto ask_choice
)

:: Assign chosen serial number to the target string
set "chosen_device=!dev%choice%!"
set TARGET=-s %chosen_device%
echo Selected device: %chosen_device%

:: -------------------------------------------------------------------
:: CORE LOGCAT RUNNER
:: -------------------------------------------------------------------
:run_logcat
echo.
echo Clearing logcat buffer...
%ADB% %TARGET% logcat -c

echo Waiting for %PKG% to appear in process list...
:waitpid
set PID=
for /f "tokens=1" %%p in ('%ADB% %TARGET% shell pidof %PKG% 2^>nul') do set PID=%%p
if "%PID%"=="" (
    timeout /t 1 /nobreak >nul
    goto waitpid
)

echo Found PID %PID% — streaming all logs (Ctrl+C to stop)
echo ========================================================
%ADB% %TARGET% logcat --pid=%PID% -v time *:V