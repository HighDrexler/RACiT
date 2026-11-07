@echo off
setlocal

rem ============================================================
rem  LINGUA: cambia solo il valore dopo "LINGUA="
rem    it = italiano     en = inglese     fr = francese
rem    es = spagnolo     de = tedesco     nl = olandese
rem  GIOCO: la cartella che contiene PS3_GAME
rem ============================================================
set "LINGUA=it"
if not defined GIOCO set "GIOCO=%~dp0input\BCES00511\game"


rem Da qui in poi non serve modificare nulla.
rem Avvia la build senza launcher. I salvataggi vanno in input\BCES00511\run.
set "ROOT=%~dp0"
set "ROOT=%ROOT:\=/%"
set "GIOCO=%GIOCO:\=/%"
set "ESEGUIBILE=%~dp0build\BCES00511\RaCACiT_BCES00511.exe"
set "DATI=%~dp0input\BCES00511\run"

if not exist "%ESEGUIBILE%" (
    echo Gioco non trovato: %ESEGUIBILE%
    echo Va prima compilato con: bash scripts/build_version.sh BCES00511
    pause
    exit /b 1
)
if not exist "%GIOCO%\PS3_GAME\PARAM.SFO" (
    echo Dump non trovato in: %GIOCO%
    pause
    exit /b 1
)

if not exist "%DATI%\hdd0" mkdir "%DATI%\hdd0"
if not exist "%DATI%\hdd1" mkdir "%DATI%\hdd1"

set "PS3_VFS_ROOT=%GIOCO%"
set "PS3_HDD0_ROOT=%ROOT%input/BCES00511/run/hdd0"
set "PS3_HDD1_ROOT=%ROOT%input/BCES00511/run/hdd1"
set "RACACIT_LOG_FILE=%ROOT%input/BCES00511/run/game.log"
set "RACACIT_LANGUAGE=%LINGUA%"

cd /d "%DATI%"
"%ESEGUIBILE%" "%ROOT%input/BCES00511/EBOOT.ELF"

if errorlevel 1 (
    echo.
    echo Il gioco si e' chiuso con un errore. Log: input\BCES00511\run\game.log
    pause
)
