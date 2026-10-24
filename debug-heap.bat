@echo off
rem ---------------------------------------------------------------------------
rem  Heap-corruption diagnosis helper
rem  Run this, reproduce the crash, then send me debug-out.txt
rem  (It launches the Debug build under cdb so the stack is captured instead of
rem   a modal "HEAP CORRUPTION DETECTED" box.)
rem ---------------------------------------------------------------------------
setlocal
set PATH=D:\Qt\6.11.0\msvc2022_64\bin;%PATH%
cd /d %~dp0

set EXE=build\MSVC_64bit_Debug\QDeskWidget.exe
if not exist "%EXE%" (
    echo [x] %EXE% not found - build the project in Qt Creator first.
    pause
    exit /b 1
)

set CDB=C:\Program Files (x86)\Windows Kits\10\Debuggers\x64\cdb.exe
if not exist "%CDB%" (
    echo [x] cdb.exe not found.
    pause
    exit /b 1
)

echo Running under the debugger. Reproduce the crash now...
"%CDB%" -g -G -o -c "sxe -c \"\" av; g; .echo ===CRASH===; kb 40; q" "%EXE%" --crt-debug > "%~dp0debug-out.txt" 2>&1

echo.
echo Done. Opening debug-out.txt ...
start notepad "%~dp0debug-out.txt"
