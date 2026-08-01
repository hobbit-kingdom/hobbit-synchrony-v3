@echo off
setlocal

rem Builds MultiplayerSetup.exe with the C# compiler that ships with Windows.
rem No SDK, no NuGet, no project files - output is a ~15 KB standalone exe that
rem runs on any Windows with .NET Framework 4.x (i.e. Windows 7 and newer).

set CSC=%WINDIR%\Microsoft.NET\Framework64\v4.0.30319\csc.exe
if not exist "%CSC%" set CSC=%WINDIR%\Microsoft.NET\Framework\v4.0.30319\csc.exe

if not exist "%CSC%" (
    echo Could not find csc.exe under %WINDIR%\Microsoft.NET
    exit /b 1
)

"%CSC%" /nologo /target:exe /platform:anycpu /optimize+ ^
        /out:"%~dp0MultiplayerSetup.exe" "%~dp0Program.cs"

if errorlevel 1 (
    echo Build failed.
    exit /b 1
)

echo Built "%~dp0MultiplayerSetup.exe"
