#!/bin/sh
# Compile LuminosityManager.exe (MinGW-w64, depuis Linux ou MSYS2)
set -e
cd "$(dirname "$0")"
x86_64-w64-mingw32-windres src/app.rc -O coff -o src/app.res
x86_64-w64-mingw32-g++ -Os -s -municode -mwindows -fno-exceptions -fno-rtti \
  -ffunction-sections -fdata-sections -Wl,--gc-sections -static -Wall \
  src/main.cpp src/app.res -o LuminosityManager.exe \
  -lole32 -loleaut32 -luuid -lwbemuuid -ldxva2 -lmfplat -lmf -lmfreadwrite -lmfuuid -lstrmiids -lpropsys
rm -f src/app.res
ls -l LuminosityManager.exe
