@echo off
rem ============================================================
rem  QDeskWidget - MSVC x64 Release packaging script
rem
rem  Output: dist\QDeskWidget\  -- a portable folder you can copy to other PCs (~34MB)
rem  Layout: QDeskWidget.exe + all DLLs + plugins\ live in the ROOT
rem          (no bin\ subfolder); qt.conf (Prefix = .) tells Qt where the plugins are.
rem          No translations\ -- the app has no QTranslator, see the TRIM block below.
rem
rem  Usage:
rem    package-msvc.bat           build package
rem    package-msvc.bat clean      delete build/MSVC_64bit_Release first (full rebuild)
rem
rem  NOTE: dev builds are Debug; releases MUST be Release. The Debug build depends
rem        on non-redistributable debug CRT and triggers more security prompts.
rem ============================================================
setlocal
set "SRC=%~dp0"
set "BUILD=%SRC%build\MSVC_64bit_Release"
set "DIST=%SRC%dist\QDeskWidget"

set "VSROOT=D:\Microsoft Visual Studio\18\Community"
set "MSVC=%VSROOT%\VC\Tools\MSVC\14.51.36231"
set "SDK=C:\Program Files (x86)\Windows Kits\10"
set "SDKVER=10.0.26100.0"
set "QT=D:\Qt\6.11.0\msvc2022_64"
set "JOMDIR=D:\Qt\Tools\QtCreator\bin\jom"
set "CMAKEEXE=D:\Qt\Tools\CMake_64\bin\cmake.exe"
if not exist "%CMAKEEXE%" set "CMAKEEXE=cmake"

set "INCLUDE=%MSVC%\include;%SDK%\Include\%SDKVER%\ucrt;%SDK%\Include\%SDKVER%\um;%SDK%\Include\%SDKVER%\shared;%SDK%\Include\%SDKVER%\winrt;%SDK%\Include\%SDKVER%\cppwinrt"
set "LIB=%MSVC%\lib\x64;%SDK%\Lib\%SDKVER%\ucrt\x64;%SDK%\Lib\%SDKVER%\um\x64"
set "PATH=%MSVC%\bin\HostX64\x64;%SDK%\bin\%SDKVER%\x64;%QT%\bin;%JOMDIR%;%PATH%"

if /i "%~1"=="clean" (
    echo === removing %BUILD% ===
    if exist "%BUILD%" rmdir /s /q "%BUILD%"
)

if not exist "%BUILD%\Makefile" (
    echo === configure (Release) ===
    "%CMAKEEXE%" -S "%SRC%." -B "%BUILD%" -G "NMake Makefiles JOM" -DCMAKE_BUILD_TYPE=Release -DCMAKE_PREFIX_PATH=%QT% -DCMAKE_C_COMPILER="%MSVC%\bin\HostX64\x64\cl.exe" -DCMAKE_CXX_COMPILER="%MSVC%\bin\HostX64\x64\cl.exe"
    if errorlevel 1 goto fail
)

echo === compile (Release) ===
cd /d "%BUILD%"
jom -f Makefile /nologo -j8
if errorlevel 1 goto fail

echo === install + windeployqt (collect Qt deps) ===
if exist "%DIST%" rmdir /s /q "%DIST%"
"%CMAKEEXE%" --install "%BUILD%" --prefix "%DIST%"
if errorlevel 1 goto fail

rem windeployqt only collects Qt's own DLLs; bundle VC/UCRT runtime ourselves.
rem Everything is flattened into the package ROOT (no bin\ subfolder).
echo === bundle VC / UCRT runtime (self-contained, no vc_redist needed) ===
set "BIN=%DIST%"

rem VC CRT: locate the VC14x.CRT folder under our toolset version
set "CRTDIR="
for /d %%C in ("%VSROOT%\VC\Redist\MSVC\14.51.36231\x64\Microsoft.VC14*.CRT") do set "CRTDIR=%%C"
if not "%CRTDIR%"=="" (
    copy /y "%CRTDIR%\*.dll" "%BIN%\" >nul
)

rem UCRT: from the Windows SDK redist (path uses the SDK version).
rem The app requires Win10+, so ucrtbase.dll and the api-ms-win-* apiset stubs are
rem resolved by the OS. Copying the whole SDK folder would drop 45 api-ms-win-*.dll
rem plus a 1.4MB ucrtbase.dll into the package for nothing.
set "UCRTDIR=%SDK%\Redist\%SDKVER%\ucrt\DLLs\x64"
if exist "%UCRTDIR%" (
    copy /y "%UCRTDIR%\*.dll" "%BIN%\" >nul
    del /q "%BIN%\api-ms-win-*.dll" 2>nul
    del /q "%BIN%\ucrtbase.dll" 2>nul
)

rem Flatten: if CMake installed the exe into a bin\ subfolder, move everything
rem up to the package root so the whole app sits in a single directory.
if exist "%DIST%\bin" (
    echo === flatten bin\ into package root ===
    move /y "%DIST%\bin\*" "%DIST%\" >nul
    rmdir /q "%DIST%\bin" >nul 2>&1
)

rem ---------------------------------------------------------------------------
rem TRIM: drop Qt payload this app never loads -- 81MB -> ~34MB.
rem
rem Verified by deleting exactly these from a copy of the package and running
rem   QDeskWidget.exe --grab <dir>   three times
rem (music box with JPEG cover art / hello-clock plugin / weather: SVG icons + HTTPS),
rem all rendering correctly.
rem
rem   opengl32sw.dll, dxcompiler.dll, dxil.dll, D3Dcompiler_47.dll
rem       Only used by QOpenGL and the Qt Quick RHI backends. This app paints with
rem       QPainter (raster engine) and links no QtOpenGL/QtQuick module at all.
rem   translations\
rem       No QTranslator is ever installed; every string is a hard-coded zh_CN literal.
rem   plugins\generic\qtuiotouchplugin.dll
rem       Windows touch injection; the UI has no touch handling.
rem   plugins\tls\qcertonlybackend.dll
rem       qschannelbackend is the active backend on Windows; cert-only is unused.
rem   plugins\imageformats\{qtiff,qtga,qwbmp,qicns}.dll
rem       Assets are png/jpg/ico/svg only. Keep qjpeg -- NetEase cover art is JPEG.
rem
rem Done here instead of relying solely on the windeployqt flags set in
rem CMakeLists.txt: the deploy script is generated at configure time, so a
rem pre-existing build directory would keep running the stale one.
rem ---------------------------------------------------------------------------
echo === trim unused Qt payload ===
del /q "%DIST%\opengl32sw.dll" "%DIST%\dxcompiler.dll" "%DIST%\dxil.dll" "%DIST%\D3Dcompiler_47.dll" 2>nul
if exist "%DIST%\translations" rmdir /s /q "%DIST%\translations"
del /q "%DIST%\plugins\generic\qtuiotouchplugin.dll" 2>nul
if exist "%DIST%\plugins\generic" rmdir /q "%DIST%\plugins\generic" 2>nul
del /q "%DIST%\plugins\tls\qcertonlybackend.dll" 2>nul
del /q "%DIST%\plugins\imageformats\qtiff.dll" "%DIST%\plugins\imageformats\qtga.dll" "%DIST%\plugins\imageformats\qwbmp.dll" "%DIST%\plugins\imageformats\qicns.dll" 2>nul

rem Ensure qt.conf points Qt's plugin search to the package root (Prefix = .).
echo === fix qt.conf (Prefix = .) ===
> "%DIST%\qt.conf" echo [Paths]
>> "%DIST%\qt.conf" echo Prefix = .

rem Ship the built user/example plugins into the APP-DATA plugins folder
rem (dataDir() = <exe dir>/QDeskWidget, so they go to dist\QDeskWidget\QDeskWidget\plugins).
rem This is deliberately separate from dist\QDeskWidget\plugins, which holds Qt's own plugins.
if exist "%BUILD%\QDeskWidget\plugins" (
    echo === copy example plugins into app data dir ===
    if not exist "%DIST%\QDeskWidget\plugins" mkdir "%DIST%\QDeskWidget\plugins" >nul 2>&1
    copy /y "%BUILD%\QDeskWidget\plugins\*.dll" "%DIST%\QDeskWidget\plugins\" >nul
)

echo.
echo === OK: %DIST%\QDeskWidget.exe ===
echo     Package done. Send the whole dist\QDeskWidget folder to others.
echo     Reminder: an unsigned exe is still blocked by SmartScreen; sign it before release.
exit /b 0

:fail
echo.
echo *** PACKAGE FAILED ***
exit /b 1
