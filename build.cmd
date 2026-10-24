@echo off
rem Local build. Run from a "x64 Native Tools Command Prompt for VS 2022".
rem Requires the SDK sources in deps\foobar2000 and deps\pfc (see README.md),
rem or pass your own SDK root:  build.cmd C:\path\to\sdk\
setlocal
set SDK=%~1
if "%SDK%"=="" set SDK=%CD%\deps\
msbuild foo_vtabs64.vcxproj /m /p:Configuration=Release /p:Platform=x64 /p:PlatformToolset=v143 /p:SolutionDir=%CD%\build\ /p:SdkRoot=%SDK%
if errorlevel 1 exit /b 1
echo.
echo Built: build\out\x64\foo_vtabs64.dll
