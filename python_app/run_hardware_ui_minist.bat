@echo off
setlocal
cd /d "%~dp0\.."
"E:\anaconda\envs\minist\python.exe" python_app\main.py --ui --config python_app\config\hardware_config.json
endlocal
