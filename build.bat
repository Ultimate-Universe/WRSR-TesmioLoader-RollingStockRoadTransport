@echo off
setlocal

rem Build from a Visual Studio x64 Native Tools command prompt.
rem Conventional uncompressed x64 Windows DLL with a normal DllMain entry point.
set ROOT=%~dp0
set SRC=%ROOT%src\RollingStockRoadTransport.cpp
set INC=%ROOT%include
set OUT=%ROOT%build

if not exist "%OUT%" mkdir "%OUT%"

cl /nologo /c /O2 /GS- /GR- /EHsc- /Zl /W3 /I"%INC%" ^
   /Fo"%OUT%\RollingStockRoadTransport.obj" "%SRC%"
if errorlevel 1 exit /b 1

link /nologo /dll /entry:DllMain /nodefaultlib /machine:x64 /subsystem:windows ^
     /opt:ref /opt:icf ^
     /out:"%OUT%\RollingStockRoadTransport.dll" ^
     /implib:"%OUT%\RollingStockRoadTransport.lib" ^
     "%OUT%\RollingStockRoadTransport.obj" ^
     "%ROOT%tooling\imports\kernel32.lib"
if errorlevel 1 exit /b 1

copy /y "%ROOT%src\RollingStockRoadTransport.ini" "%OUT%\RollingStockRoadTransport.ini" >nul

echo Built %OUT%\RollingStockRoadTransport.dll
endlocal
