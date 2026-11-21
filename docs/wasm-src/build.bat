@echo off
rem Builds lsfgprep with Emscripten. build.bat node  -> build-node (byte comparison)   build.bat web -> build-web + copy to wasm\
setlocal
set I=C:\Users\emi\Documents\Codex\2026-10-27\referenced-chatgpt-conversation-this-is-an\integrations\python-env
set EMSDK=C:/Users/emi/emsdk
set EMSDK_PYTHON=C:\Users\emi\emsdk\python\3.13.3_64bit\python.exe
set PATH=%I%\Lib\site-packages\cmake\data\bin;%I%\Scripts;C:\Users\emi\emsdk\upstream\emscripten;C:\Users\emi\emsdk\node\24.19.0_64bit;%PATH%
cd /d "%~dp0.."
set NODEOPT=OFF
if "%1"=="node" set NODEOPT=ON
cmake -S wasm-src -B build-%1 -G Ninja -DCMAKE_BUILD_TYPE=Release -DLSFGPREP_NODE=%NODEOPT% -DCMAKE_TOOLCHAIN_FILE=C:/Users/emi/emsdk/upstream/emscripten/cmake/Modules/Platform/Emscripten.cmake || exit /b 1
cmake --build build-%1 -j 3 || exit /b 1
if "%1"=="web" node wasm-src\make-bundle.js build-web wasm\lsfgprep-bundle.js
