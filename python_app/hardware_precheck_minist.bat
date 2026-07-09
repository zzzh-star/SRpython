@echo off
setlocal
cd /d "%~dp0\.."
"E:\anaconda\envs\minist\python.exe" python_app\hardware_precheck.py
pause
endlocal
