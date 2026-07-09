@echo off
setlocal
cd /d "%~dp0\.."
"E:\anaconda\envs\minist\python.exe" -m PyInstaller ^
  --noconfirm ^
  --clean ^
  --windowed ^
  --name SR_Python_MainControl ^
  --add-data "python_app\config;python_app\config" ^
  --add-data "python_app\src\ui_theme.qss;python_app\src" ^
  python_app\main.py
endlocal
