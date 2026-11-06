@echo off
rem ultdaytonausa clean-up (from 0.0.22)
rem
rem Removes what the ultdaytonausa installers 0.0.14 to 0.0.21 left outside the game folder:
rem   - the Windows installed apps entry  HKCU\Software\Microsoft\Windows\CurrentVersion\Uninstall\ultdaytonausa
rem   - the uninstaller folder            %LOCALAPPDATA%\ultdaytonausa
rem   - the Start menu and Desktop shortcuts "Daytona USA.lnk" (only if they open DAYTONA.EXE)
rem   - a leftover uninstaller copy       %TEMP%\ultdaytonausa-uninstall.exe
rem The game folder, your settings and records are not touched. From 0.0.22 the installer is portable and
rem writes nothing outside the game folder. For this Windows user only; no admin rights needed.
setlocal EnableExtensions EnableDelayedExpansion
set "KEY=HKCU\Software\Microsoft\Windows\CurrentVersion\Uninstall\ultdaytonausa"
set "APPDIR=%LOCALAPPDATA%\ultdaytonausa"
set "TMPCOPY=%TEMP%\ultdaytonausa-uninstall.exe"
set "STARTLNK=%APPDATA%\Microsoft\Windows\Start Menu\Programs\Daytona USA.lnk"
set "DESKTOP="
for /f "usebackq delims=" %%D in (`powershell -NoProfile -Command "[Environment]::GetFolderPath('Desktop')" ^<nul 2^>nul`) do set "DESKTOP=%%D"
if not defined DESKTOP set "DESKTOP=%USERPROFILE%\Desktop"
set "DESKLNK=%DESKTOP%\Daytona USA.lnk"

echo ultdaytonausa clean-up
echo.
echo Looking for what earlier ultdaytonausa installers (0.0.14 to 0.0.21) left outside the game folder...
echo.
set FOUND=0
set HASKEY=0
reg query "%KEY%" <nul >nul 2>&1
if not errorlevel 1 set HASKEY=1
if %HASKEY%==1 (set /a FOUND+=1 & echo   Installed apps entry: %KEY%)
if exist "%APPDIR%\" (set /a FOUND+=1 & echo   Uninstaller folder:   %APPDIR%)
if exist "%TMPCOPY%" (set /a FOUND+=1 & echo   Uninstaller copy:     %TMPCOPY%)
call :checklnk "%STARTLNK%" START
if "!START!"=="1" (set /a FOUND+=1 & echo   Start menu shortcut:  %STARTLNK%)
call :checklnk "%DESKLNK%" DESK
if "!DESK!"=="1" (set /a FOUND+=1 & echo   Desktop shortcut:     %DESKLNK%)

if %FOUND%==0 (
    echo   Nothing found. There is nothing to clean up.
    echo.
    pause
    exit /b 0
)
echo.
echo Your game folder, settings and records are not touched.
set "ANS="
set /p "ANS=Remove the %FOUND% item(s) listed above? Type Y and press Enter (anything else keeps them): "
if /i not "%ANS%"=="Y" (
    echo Nothing was removed.
    echo.
    pause
    exit /b 1
)
echo.
set REMOVED=0
set FAILED=0
if %HASKEY%==1 (
    reg delete "%KEY%" /f <nul >nul 2>&1
    reg query "%KEY%" <nul >nul 2>&1
    if errorlevel 1 (set /a REMOVED+=1 & echo   Removed the installed apps entry) else (set /a FAILED+=1 & echo   Could not remove the installed apps entry)
)
if exist "%APPDIR%\" (
    rd /s /q "%APPDIR%" 2>nul
    if exist "%APPDIR%\" (set /a FAILED+=1 & echo   Could not remove %APPDIR% ^(is the uninstaller running?^)) else (set /a REMOVED+=1 & echo   Removed %APPDIR%)
)
if exist "%TMPCOPY%" (
    del /f /q "%TMPCOPY%" 2>nul
    if exist "%TMPCOPY%" (set /a FAILED+=1 & echo   Could not remove %TMPCOPY%) else (set /a REMOVED+=1 & echo   Removed %TMPCOPY%)
)
if "!START!"=="1" (
    del /f /q "%STARTLNK%" 2>nul
    if exist "%STARTLNK%" (set /a FAILED+=1 & echo   Could not remove the Start menu shortcut) else (set /a REMOVED+=1 & echo   Removed the Start menu shortcut)
)
if "!DESK!"=="1" (
    del /f /q "%DESKLNK%" 2>nul
    if exist "%DESKLNK%" (set /a FAILED+=1 & echo   Could not remove the Desktop shortcut) else (set /a REMOVED+=1 & echo   Removed the Desktop shortcut)
)
echo.
echo Done: %REMOVED% removed, %FAILED% could not be removed.
echo.
pause
exit /b %FAILED%

rem Sets %2 to 1 when %1 is a shortcut that opens DAYTONA.EXE (the ones the installers made), else 0.
:checklnk
set "%~2=0"
if not exist "%~1" exit /b 0
set "TARGET="
for /f "usebackq delims=" %%T in (`powershell -NoProfile -Command "(New-Object -ComObject WScript.Shell).CreateShortcut('%~1').TargetPath" ^<nul 2^>nul`) do set "TARGET=%%T"
if not defined TARGET (
    rem PowerShell not available: the installers' shortcut name is specific enough
    set "%~2=1"
    exit /b 0
)
echo "!TARGET!" | findstr /i /c:"DAYTONA.EXE" >nul && set "%~2=1"
exit /b 0
