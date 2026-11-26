@echo off
setlocal
if not defined DANZI_REFERENCE (echo Set DANZI_REFERENCE to the reference checkout. & exit /b 1)
set CONFIG=%1
if "%CONFIG%"=="" set CONFIG=release
set ROOT=%~dp0..
set VS=C:\Program Files\Microsoft Visual Studio\18\Community
call "%VS%\VC\Auxiliary\Build\vcvars64.bat" >nul || exit /b 1
set PATH=%VS%\Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin;%VS%\Common7\IDE\CommonExtensions\Microsoft\CMake\Ninja;%PATH%

pushd "%ROOT%\oracle" || exit /b 1
set CARGO_TARGET_DIR=%ROOT%\build\oracle
cargo build --release --offline || (popd & exit /b 1)
popd


python -I "%ROOT%\tools\snapshot_reference.py" "%DANZI_REFERENCE%" "%ROOT%\build\m4-reference" || exit /b 1
pushd "%ROOT%\build\m4-reference" || exit /b 1
set DANZILOADER_VERSION=oracle
cargo build --release --offline -p victory-ref-backend -p shader-oracle -p m6-oracle --target-dir "%ROOT%\build\m4-target" || (popd & exit /b 1)
popd

pushd "%ROOT%" || exit /b 1
cmake --preset msvc-%CONFIG% || (popd & exit /b 1)
cmake --build --preset msvc-%CONFIG% || (popd & exit /b 1)
if /I "%2"=="notest" (popd & exit /b 0)
ctest --preset msvc-%CONFIG% || (popd & exit /b 1)
popd
