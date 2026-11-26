@echo off
setlocal
set CONFIG=%2
if "%CONFIG%"=="" set CONFIG=release
set ROOT=%~dp0..
set VS=C:\Program Files\Microsoft Visual Studio\18\Community
call "%VS%\VC\Auxiliary\Build\vcvars64.bat" >nul || exit /b 1
set PATH=%VS%\Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin;%VS%\Common7\IDE\CommonExtensions\Microsoft\CMake\Ninja;%PATH%
pushd "%ROOT%" || exit /b 1
cmake --preset msvc-%CONFIG% >nul || (popd & exit /b 1)
cmake --build --preset msvc-%CONFIG% --target %1 || (popd & exit /b 1)
popd
