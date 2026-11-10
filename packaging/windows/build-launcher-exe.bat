@echo off
rem Optional: turn the launcher into "The Bigs 2 Launcher.exe" (no Python needed afterwards). Needs Python 3.
cd /d "%~dp0"
py -m pip install --user pyinstaller || exit /b 1
py -m PyInstaller --onefile --windowed --name "The Bigs 2 Launcher" --distpath . thebigs2_gui.py
rmdir /s /q build 2>nul
del "The Bigs 2 Launcher.spec" 2>nul
pause
