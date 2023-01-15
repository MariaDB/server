@echo off
setlocal
SET srcdir="%1"
SET bindir="%1"
SET pcretest="%3"
if not [%CMAKE_CONFIG_TYPE%]==[] SET pcretest="%bindir%\%CMAKE_CONFIG_TYPE%\pcretest.exe"
call %srcdir%\RunTest.Bat
if errorlevel 1 exit /b 1
echo RunTest.bat tests successfully completed
