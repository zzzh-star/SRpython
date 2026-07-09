@echo off
setlocal
cd /d "%~dp0"

echo Building FaWave...

set "PYTHON_EXE=E:\anaconda\envs\minist\python.exe"
set "MINIST_ROOT=E:\anaconda\envs\minist"
if not exist "%PYTHON_EXE%" (
    set "PYTHON_EXE=python"
    set "MINIST_ROOT="
)

"%PYTHON_EXE%" -m PyInstaller --version >nul 2>nul
if %ERRORLEVEL% neq 0 (
    echo PyInstaller not found in the selected Python environment.
    echo Please install it with:
    echo   "%PYTHON_EXE%" -m pip install pyinstaller
    pause
    goto :eof
)

if not exist "assets\app_icon.ico" (
    echo Generating app_icon.ico...
    "%PYTHON_EXE%" tools\generate_app_icon.py
    if %ERRORLEVEL% neq 0 (
        echo Warning: Failed to generate icon. Building without icon...
        set "ICON_ARG="
    ) else (
        set "ICON_ARG=--icon=assets\app_icon.ico"
    )
) else (
    set "ICON_ARG=--icon=assets\app_icon.ico"
)

"%PYTHON_EXE%" -m PyInstaller --noconfirm --clean --windowed ^
  --name FaWave ^
  %ICON_ARG% ^
  --add-data "assets;assets" ^
  --add-data "config;config" ^
  --add-data "src\ui\themes;src\ui\themes" ^
  --add-binary "%MINIST_ROOT%\Library\bin\pyside6.cp310-win_amd64.dll;." ^
  --add-binary "%MINIST_ROOT%\Library\bin\pyside6qml.cp310-win_amd64.dll;." ^
  --add-binary "%MINIST_ROOT%\Library\bin\shiboken6.cp310-win_amd64.dll;." ^
  --collect-all pyqtgraph ^
  --collect-all OpenGL ^
  --collect-all trimesh ^
  --hidden-import pyqtgraph.opengl ^
  --hidden-import OpenGL.GL ^
  --hidden-import trimesh.exchange.gltf ^
  --hidden-import trimesh.exchange.obj ^
  --hidden-import trimesh.exchange.stl ^
  --hidden-import pandas ^
  --hidden-import openpyxl ^
  --hidden-import numpy ^
  main.py

if %ERRORLEVEL% neq 0 (
    echo Build failed.
    pause
    goto :eof
)

echo Build complete. Check the dist\FaWave\ folder.
pause
