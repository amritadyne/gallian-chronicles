@echo off
setlocal EnableExtensions
title VC3 frame census (trace build) - keep this window open
rem Frame census with function attribution (host\vc3_census.cpp). Uses its own
rem build, out\vc3-trace: the optimised build with the runtime's call observers
rem left in (PSPRECOMP_AOT_PRODUCTION_FASTPATHS=OFF), so the census can name the
rem game functions that change each value. The first run compiles every
rem generated unit (as long as a clean VC3 build); later runs only update it.
rem Safe to close and run again: the build resumes.
rem
rem Plays the English ReTranslation with the launcher's English files and saves,
rem at 165 FPS. In a battle, get an enemy vehicle firing at your unit and press
rem F10 while it fires: the game slows for a few seconds (120 frames compared),
rem then for two frames while changes are credited to functions. Do it two or
rem three times, then quit.
rem Reports: logs\census\census-N.txt and census-N-functions.txt
rem Logs: logs\census-build.txt (build), logs\census-log.txt (game)

for %%I in ("%~dp0.") do set "ROOT=%%~fI"
for %%I in ("%ROOT%\..") do set "WORKSPACE=%%~fI"
set "BUILD=%ROOT%\out\vc3-trace"
set "LOGS=%ROOT%\logs"
set "LOG=%LOGS%\census-build.txt"
set "JOBS=12"
if defined VC3_BUILD_JOBS set "JOBS=%VC3_BUILD_JOBS%"
if not exist "%LOGS%" mkdir "%LOGS%"

set "MINGW="
if defined GC_MINGW set "MINGW=%GC_MINGW%"
if not defined MINGW for /f "delims=" %%G in ('where g++.exe 2^>nul') do if not defined MINGW set "MINGW=%%~dpG"
if not defined MINGW set "MINGW=%LOCALAPPDATA%\Microsoft\WinGet\Packages\BrechtSanders.WinLibs.POSIX.UCRT_Microsoft.Winget.Source_8wekyb3d8bbwe\mingw64\bin"
if "%MINGW:~-1%"=="\" set "MINGW=%MINGW:~0,-1%"
set "VULKAN_SDK=%WORKSPACE%\work\vulkan-sdk\1.4.363.1"
set "SDLROOT=%WORKSPACE%\psprecomp-upstream\third_party\SDL3-3.4.18-mingw\SDL3-3.4.18\x86_64-w64-mingw32"
set "FFMPEG_CACHE=%WORKSPACE%\psprecomp-upstream\out\vc2-portablekit-mingw\_deps\downloads"
set "PATH=%MINGW%;%VULKAN_SDK%\Bin;%PATH%"
rem CMake reads backslashes in -D paths as escapes: give it forward slashes.
set "MINGW_CM=%MINGW:\=/%"
set "SDL_CM=%SDLROOT:\=/%"
set "VULKAN_CM=%VULKAN_SDK:\=/%"
set "FFMPEG_CM=%FFMPEG_CACHE:\=/%"
rem A first configure that failed leaves a cache behind: start that one afresh.
if exist "%BUILD%" if not exist "%BUILD%\build.ninja" rmdir /s /q "%BUILD%"

echo.
echo  Building the VC3 trace build. The first time compiles everything: it is
echo  working as long as the [N/M] counter keeps going up.
echo.
echo [vc3-census-build] configure %DATE% %TIME% > "%LOG%"
cmake -S "%ROOT%" -B "%BUILD%" -G Ninja ^
  -DCMAKE_CXX_COMPILER="%MINGW_CM%/g++.exe" ^
  -DCMAKE_RC_COMPILER="%MINGW_CM%/windres.exe" ^
  -DCMAKE_BUILD_TYPE=Release -DPORTABLEKIT_RENDERER=ON ^
  -DPORTABLEKIT_FFMPEG=bundled -DPORTABLEKIT_FFMPEG_DOWNLOAD_DIR="%FFMPEG_CM%" ^
  -DPSPRECOMP_BUILD_TESTS=OFF -DPSPRECOMP_GENERATED_OPT_LEVEL=2 -DPSPRECOMP_GENERATED_JOBS=%JOBS% ^
  -DPSPRECOMP_AOT_PRODUCTION_FASTPATHS=OFF ^
  -DSDL3_DIR="%SDL_CM%/lib/cmake/SDL3" -DVulkan_ROOT="%VULKAN_CM%" >> "%LOG%" 2>&1
if errorlevel 1 (
  type "%LOG%"
  echo [vc3-census-build] CONFIGURE FAILED >> "%LOG%"
  goto failed
)
echo [vc3-census-build] build start %DATE% %TIME% >> "%LOG%"
powershell -NoProfile -ExecutionPolicy Bypass -Command "& cmake --build '%BUILD%' --target VC3Native --parallel %JOBS% 2>&1 | ForEach-Object { $l = \"$_\"; Write-Host $l; Add-Content -LiteralPath '%LOG%' -Value $l }; exit $LASTEXITCODE"
if errorlevel 1 (
  echo [vc3-census-build] BUILD FAILED %DATE% %TIME% >> "%LOG%"
  goto failed
)
copy /y "%SDLROOT%\bin\SDL3.dll" "%BUILD%\bin\" >nul
echo [vc3-census-build] BUILD OK %DATE% %TIME% >> "%LOG%"

rem The English edition as the launcher runs it (same disc image, executable and saves).
set "LAUNCHER_EN=%WORKSPACE%\psprecomp-upstream\out\launcher-native\editions\english"
set "VC3_DISC_IMAGE=%ROOT%\editions\english\Valkyria Chronicles 3 E2 - English ReTranslation.iso"
set "VC3_EXECUTABLE=%LAUNCHER_EN%\EBOOT.ELF"
set "VC3_MEMORY_STICK=%LAUNCHER_EN%\ms0"
if not exist "%VC3_EXECUTABLE%" set "VC3_EXECUTABLE=%ROOT%\editions\english\EBOOT.ELF"
if not exist "%VC3_MEMORY_STICK%" set "VC3_MEMORY_STICK=%ROOT%\editions\english\ms0"
set "PSPRECOMP_NO_INTERPRETER=1"
rem The frame rate: the first argument if given (0 = the game's own rate), else 165.
set "VC3_TICK_HZ=165"
if not "%~1"=="" set "VC3_TICK_HZ=%~1"
rem F11 swap hunts last 45 s (time to mark several bursts with F12).
set "VC3_CENSUS_HUNT_SECONDS=45"
if not exist "%LOGS%\census" mkdir "%LOGS%\census"
set "VC3_CENSUS=%LOGS%\census"

echo.
echo  Census on (frame rate %VC3_TICK_HZ%, 0 = the game's own). F10 frame census; F11 swap hunt (45 s),
echo  then F12 each time the enemy vehicle starts a burst.
echo  Reports: logs\census. Game log: logs\census-log.txt
"%BUILD%\bin\Valkyria Chronicles 3.exe" > "%LOGS%\census-log.txt" 2>&1
echo.
echo  The game has exited (code %ERRORLEVEL%). Tell Claude it is done.
pause
exit /b 0

:failed
echo.
echo  FAILED - send logs\census-build.txt to Claude.
pause
exit /b 1
