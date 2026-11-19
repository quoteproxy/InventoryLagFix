@echo off
rem Builds InventoryLagFix.dll with the MSVC x64 toolset (Visual Studio 2022 or Build Tools 2022).
setlocal
cd /d "%~dp0"
set "VSWHERE=%ProgramFiles(x86)%\Microsoft Visual Studio\Installer\vswhere.exe"
for /f "usebackq tokens=*" %%i in (`"%VSWHERE%" -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath`) do set "VSDIR=%%i"
if not defined VSDIR ( echo MSVC x64 toolset not found & exit /b 1 )
call "%VSDIR%\VC\Auxiliary\Build\vcvars64.bat" >nul || exit /b 1
if not exist build mkdir build
cl /nologo /LD /O2 /MT /EHsc /W4 /WX /permissive- /std:c++17 /Fobuild\ src\InventoryLagFix.cpp /link /NOLOGO shell32.lib ole32.lib /OUT:build\InventoryLagFix.dll /IMPLIB:build\InventoryLagFix.lib || exit /b 1
copy /y build\InventoryLagFix.dll package\SKSE\Plugins\InventoryLagFix.dll >nul || exit /b 1
echo built: build\InventoryLagFix.dll
