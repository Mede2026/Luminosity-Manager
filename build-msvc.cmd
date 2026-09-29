@echo off
rem Compile LuminosityManager.exe avec Visual Studio (MSVC), sur Windows (utilise par GitHub Actions).
rem Les .exe MSVC sont moins souvent pris pour des virus par Securite Windows que ceux de MinGW.
rem La version vient du fichier VERSION. Sous Linux : ./build.sh (MinGW).
setlocal
cd /d "%~dp0"
set "VSWHERE=%ProgramFiles(x86)%\Microsoft Visual Studio\Installer\vswhere.exe"
for /f "usebackq delims=" %%i in (`"%VSWHERE%" -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath`) do set "VS=%%i"
if not defined VS (echo Visual Studio introuvable & exit /b 1)
call "%VS%\VC\Auxiliary\Build\vcvars64.bat" >nul || exit /b 1
set /p VERSION=<VERSION
python src\ui\bundle.py src\ui.bundle.html || exit /b 1
rc /nologo /fo src\app.res src\app.rc || exit /b 1
rem /O1 = le plus petit, /MT = tout dans le .exe (aucune DLL a installer), /utf-8 = accents dans les textes
cl /nologo /utf-8 /O1 /MT /Gy /Gw /GR- /W3 /DUNICODE /D_UNICODE /D_CRT_SECURE_NO_WARNINGS ^
  /DAPP_VERSION_STR="\"%VERSION%\"" ^
  src\main.cpp src\light.cpp src\brightness.cpp src\net.cpp src\json.cpp src\core.cpp src\stats.cpp ^
  src\backup.cpp src\install.cpp src\webview.cpp src\ui.cpp src\app.res ^
  /Fo:src\ /Fe:LuminosityManager.exe ^
  /link /SUBSYSTEM:WINDOWS /MANIFEST:NO /OPT:REF /OPT:ICF ^
  kernel32.lib user32.lib gdi32.lib advapi32.lib shell32.lib ole32.lib oleaut32.lib uuid.lib wbemuuid.lib ^
  dxva2.lib mfplat.lib mf.lib mfreadwrite.lib mfuuid.lib strmiids.lib propsys.lib comctl32.lib ^
  winhttp.lib dwmapi.lib bcrypt.lib comdlg32.lib crypt32.lib wtsapi32.lib powrprof.lib || exit /b 1
del /q src\*.obj src\app.res src\ui.bundle.html 2>nul
dir LuminosityManager.exe | findstr LuminosityManager
