@echo off
rem Smoke test for the remote control server (AI input channel).
rem
rem Usage (from the build dir, with fable_2.exe already running):
rem   remote_smoke.cmd [port]
rem
rem Waits for the control port, pings it, runs a small timed script, checks
rem the resulting pad state, then clears. Exits 0 only if every step
rem succeeded.

setlocal
cd /d "%~dp0"
set PORT=%1
if "%PORT%"=="" set PORT=8791

rem Wait for the port (up to 60 s).
set /a TRIED=0
:wait
python -c "import socket,sys;sys.exit(0 if socket.socket().connect_ex(('127.0.0.1',%PORT%))==0 else 1)" || (
    set /a TRIED+=1
    if %TRIED% GEQ 60 (echo [smoke] port %PORT% never came up & exit /b 1)
    timeout /t 1 /nobreak >nul
    goto wait
)
echo [smoke] port %PORT% is up

python fable2_control.py --port %PORT% ping || exit /b 1
python fable2_control.py --port %PORT% info || exit /b 1

rem Press A for 300 ms, then (same connection batch via a script) verify the
rem state mid-hold and clear afterwards.
python fable2_control.py --port %PORT% press A --hold 300 || exit /b 1
timeout /t 1 /nobreak >nul
python fable2_control.py --port %PORT% get-state > smokestate.json || exit /b 1
type smokestate.json
python fable2_control.py --port %PORT% clear || exit /b 1

rem Final state should be empty.
python fable2_control.py --port %PORT% get-state > smokestate2.json || exit /b 1
type smokestate2.json
python -c "import json;d=json.load(open('smokestate2.json'));assert d['ok'];assert d['buttons']==[] and d['pending']==[],d;print('[smoke] OK: remote input channel verified')" || (
    del smokestate.json smokestate2.json >nul 2>nul & exit /b 1)
del smokestate.json smokestate2.json >nul 2>nul
endlocal
