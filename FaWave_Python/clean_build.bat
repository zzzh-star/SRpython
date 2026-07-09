@echo off
echo Cleaning build directories...
rmdir /s /q build
rmdir /s /q dist
del /q *.spec
echo Clean complete.
pause
