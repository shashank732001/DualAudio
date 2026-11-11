@echo off
rem Builds DualAudio.exe (window + tray app) and dualaudio-cli.exe (test tool). No runtime needed.
call "C:\Program Files\Microsoft Visual Studio\2022\Community\VC\Auxiliary\Build\vcvars64.bat" >nul 2>nul || exit /b 1
cd /d "%~dp0"
set FLAGS=/nologo /utf-8 /std:c++17 /O1 /MT /EHsc /W3 /DUNICODE /D_UNICODE
cl %FLAGS% dualaudio_gui.cpp /Fe:DualAudio.exe /link /OPT:REF /OPT:ICF /SUBSYSTEM:WINDOWS || exit /b 1
cl %FLAGS% dualaudio_cli.cpp /Fe:dualaudio-cli.exe /link /OPT:REF /OPT:ICF /SUBSYSTEM:CONSOLE || exit /b 1
del /q *.obj 2>nul
echo Built DualAudio.exe and dualaudio-cli.exe
