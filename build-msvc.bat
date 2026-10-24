@echo off
rem ============================================================
rem  QDeskWidget - MSVC x64 Debug build script
rem
rem  usage:
rem    build-msvc.bat            incremental build
rem    build-msvc.bat clean      full rebuild (use after header changes)
rem
rem  why not vcvars64.bat: reg.exe is restricted on this machine, vcvars
rem  cannot resolve the full SDK path and linking fails to find rc.exe.
rem  so INCLUDE / LIB / PATH are set by hand below.
rem ============================================================
setlocal

set "SRC=%~dp0"
set "BUILD=%SRC%build\MSVC_64bit_Debug"
set "MSVC=D:\Microsoft Visual Studio\18\Community\VC\Tools\MSVC\14.51.36231"
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
    echo === full rebuild, removing %BUILD% ===
    if exist "%BUILD%" rmdir /s /q "%BUILD%"
)

if not exist "%BUILD%\Makefile" (
    echo === configure ===
    "%CMAKEEXE%" -S "%SRC%." -B "%BUILD%" -G "NMake Makefiles JOM" -DCMAKE_BUILD_TYPE=Debug -DCMAKE_PREFIX_PATH=%QT% -DCMAKE_C_COMPILER="%MSVC%\bin\HostX64\x64\cl.exe" -DCMAKE_CXX_COMPILER="%MSVC%\bin\HostX64\x64\cl.exe"
    if errorlevel 1 goto fail
)

echo === compile ===
cd /d "%BUILD%"
jom -f Makefile /nologo -j8
if errorlevel 1 goto fail

echo.
echo === OK: %BUILD%\QDeskWidget.exe ===
exit /b 0

:fail
echo.
echo *** BUILD FAILED ***
exit /b 1
