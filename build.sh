#!/bin/sh
# Compile LuminosityManager.exe (MinGW-w64, depuis Linux ou MSYS2).
# La version vient du fichier VERSION.
set -e
cd "$(dirname "$0")"
VERSION=$(tr -d '[:space:]' < VERSION)
x86_64-w64-mingw32-windres src/app.rc -O coff -o src/app.res
x86_64-w64-mingw32-g++ -Os -s -municode -mwindows -fno-exceptions -fno-rtti \
  -ffunction-sections -fdata-sections -Wl,--gc-sections -static -Wall \
  -DAPP_VERSION_STR="\"$VERSION\"" \
  src/main.cpp src/light.cpp src/brightness.cpp src/net.cpp src/ui.cpp src/app.res \
  -o LuminosityManager.exe \
  -lole32 -loleaut32 -luuid -lwbemuuid -ldxva2 -lmfplat -lmf -lmfreadwrite -lmfuuid -lstrmiids \
  -lpropsys -lcomctl32 -lwinhttp -lgdiplus
rm -f src/app.res
ls -l LuminosityManager.exe
