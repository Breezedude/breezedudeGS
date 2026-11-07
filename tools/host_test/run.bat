@echo off
REM Host test for the night mode: radio/bd_night.cpp (sunrise, sunset) and the
REM ground station's changes to radio/fanet_sched.cpp. Compiles the firmware
REM sources natively against the stubs in stubs\. Needs Visual Studio's cl.exe.
REM
REM Usage:  run.bat            (from a normal cmd prompt, finds vcvars itself)
REM         run.bat nosetup    (from a Developer Command Prompt)

setlocal
set HERE=%~dp0
set SRC=%HERE%..\..\components\bdgs
set WORK=%HERE%build

if /I "%~1"=="nosetup" goto :build
for %%V in (
  "C:\Program Files\Microsoft Visual Studio\2022\Community\VC\Auxiliary\Build\vcvars64.bat"
  "C:\Program Files\Microsoft Visual Studio\2022\Professional\VC\Auxiliary\Build\vcvars64.bat"
  "C:\Program Files\Microsoft Visual Studio\2022\Enterprise\VC\Auxiliary\Build\vcvars64.bat"
  "C:\Program Files\Microsoft Visual Studio\2022\BuildTools\VC\Auxiliary\Build\vcvars64.bat"
) do if exist %%V ( call %%V >nul 2>&1 & goto :build )
echo Could not find vcvars64.bat - run this from a Developer Command Prompt
echo and pass "nosetup", or adjust the paths above.
exit /b 1

:build
if not exist "%WORK%" mkdir "%WORK%"
pushd "%WORK%"
REM stubs\ first: bd_state.h and logging.h resolve to the stubs.
cl /nologo /EHsc /W3 /std:c++17 /utf-8 /I"%HERE%stubs" /I"%SRC%\include" ^
  "%HERE%test_night.cpp" "%SRC%\radio\bd_night.cpp" "%SRC%\radio\fanet_sched.cpp" /Fe:test_night.exe >nul
if errorlevel 1 (
  echo COMPILE FAILED
  popd
  exit /b 1
)
.\test_night.exe
set RC=%ERRORLEVEL%
popd
exit /b %RC%
