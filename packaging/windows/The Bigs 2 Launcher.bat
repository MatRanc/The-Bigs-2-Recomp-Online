@echo off
rem Starts The Bigs 2 Launcher. Needs Python 3 from python.org (with "tcl/tk", the default).
cd /d "%~dp0"
where pyw >nul 2>nul && (start "" pyw thebigs2_gui.py & exit /b)
where pythonw >nul 2>nul && (start "" pythonw thebigs2_gui.py & exit /b)
echo Python 3 is not installed. Get it from https://www.python.org/downloads/ and run this again.
pause
