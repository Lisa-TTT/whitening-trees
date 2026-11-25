@echo off
setlocal
set "ROOT=%~dp0"
if exist "%ROOT%.venv\Scripts\pythonw.exe" (
    start "" "%ROOT%.venv\Scripts\pythonw.exe" "%ROOT%desktop\jog_window.py"
) else (
    py -3 "%ROOT%desktop\jog_window.py"
)\n