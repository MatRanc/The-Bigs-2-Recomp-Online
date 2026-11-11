@echo off
rem Two instances on one Windows PC: User1 hosts, User2 joins (the Windows twin of tools/run_pair.sh).
rem Put this next to thebigs2.exe and double-click it.
rem   run_pair.bat                 you drive both windows
rem   run_pair.bat auto            keys are injected (TB2_AUTOKEY) up to the first at-bat, like tools/to_match.sh
rem   run_pair.bat [auto] <ip>     use this LAN IP instead of the detected one
rem The joiner must target this PC's LAN IP, not 127.0.0.1: only then do both ends see the same IP
rem and split the lockstep link onto UDP 1795/1796.
setlocal
cd /d "%~dp0"
set "AUTO="
if /i "%~1"=="auto" (set AUTO=1 & shift)
set "IP=%~1"
if not defined IP for /f "usebackq delims=" %%i in (`powershell -NoProfile -Command "(Get-NetIPConfiguration | Where-Object IPv4DefaultGateway | Select-Object -First 1).IPv4Address.IPAddress"`) do set "IP=%%i"
if not defined IP (echo Could not find the LAN IP; run: run_pair.bat 192.168.x.y & pause & exit /b 1)
echo Using LAN IP %IP%
set "K=%TEMP%\tb2keys"
if defined AUTO (
  taskkill /im thebigs2.exe /f >nul 2>&1
  if not exist "%K%" mkdir "%K%"
  type nul > "%K%\host.keys"
  type nul > "%K%\guest.keys"
)

set TB2_FAKE_LIVE=1
set TB2_FAKE_ACCOUNT=1
set TB2_SKIP_ROSTER=1
set TB2_SESSION_TICK=1
set TB2_NET_LOG=1
set TB2_ONLINE_DEBUG=1

set TB2_ACCOUNT_NAME=User1
set TB2_FAKE_HOST=1
set TB2_HOST_PORT=51000
if defined AUTO set "TB2_AUTOKEY=%K%\host.keys"
start "TB2 host" thebigs2.exe

rem Keys: space=A, l=X, BackSpace=B, shift+arrows=d-pad. The menu wraps: Up once from the top is ONLINE.
if defined AUTO (
  timeout /t 13 /nobreak >nul
  call :send host "untilmenu space" "wait 3000" "tap shift+Up" "wait 800" "tap space" "wait 3500" "tap space" "wait 3500" "tap space" "wait 7000" "tap l"
)
echo Waiting for the host to listen on 51000 (create the game in the host window)...
:wait
timeout /t 1 /nobreak >nul
netstat -an | findstr /r /c:":51000 .*LISTENING" >nul || goto wait

set TB2_FAKE_HOST=
set TB2_HOST_PORT=
set TB2_ACCOUNT_NAME=User2
set TB2_FAKE_SEARCH=1
set TB2_PEER_IP=%IP%
set TB2_PEER_PORT=51000
if defined AUTO (
  timeout /t 4 /nobreak >nul
  set "TB2_AUTOKEY=%K%\guest.keys"
)
start "TB2 joiner" thebigs2.exe
if not defined AUTO (echo Joiner started. In its window go to online play and press Search. & exit /b 0)

rem Same timings as tools/auto_online.sh and tools/to_match.sh.
timeout /t 13 /nobreak >nul
call :send guest "untilmenu space" "wait 3000" "tap shift+Up" "wait 800" "tap space" "wait 3500" "tap space" "wait 3500" "tap space"
timeout /t 24 /nobreak >nul
echo Game Room: both Ready
call :send guest "tap space"
timeout /t 4 /nobreak >nul
call :send host "tap space"
timeout /t 16 /nobreak >nul
echo Team Setup: pads to sides, team, uniform, pitcher, lineup
call :send host "tap shift+Left"
call :send guest "tap shift+Right"
timeout /t 3 /nobreak >nul
call :both "tap space" "wait 1500" "tap space"
timeout /t 8 /nobreak >nul
call :both "tap space"
timeout /t 6 /nobreak >nul
call :both "tap space"
timeout /t 6 /nobreak >nul
echo Host picks the stadium; the match loads
call :send host "tap space"
timeout /t 40 /nobreak >nul
echo Done: the first at-bat should be up. Logs are in the game's logs folder.
exit /b 0

rem :send host|guest "line" ...  writes the lines in one go (the game empties the file after each read).
:send
set "F=%K%\%~1.keys"
set "T=%K%\%~1.tmp"
type nul > "%T%"
:sendloop
shift
if "%~1"=="" (type "%T%" >> "%F%" & exit /b 0)
>> "%T%" echo %~1
goto sendloop

:both
call :send host %*
call :send guest %*
exit /b 0
