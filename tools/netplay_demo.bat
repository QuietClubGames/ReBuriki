@echo off
rem Online play demo on one PC: two copies of the game, linked as host (player 1) and joiner (player 2).
rem Click a window to give it the keyboard: each copy's player uses the Player 1 keys.
rem The second copy is silent so the sound isn't doubled. Close either window to end.
cd /d "%~dp0..\recomp"
if not exist buriki_play.exe (
  echo buriki_play.exe was not found in the recomp folder.
  pause
  exit /b 1
)
start "" buriki_play.exe --scale 2 --netplay-host 7600
ping -n 3 127.0.0.1 >nul
start "" buriki_play.exe --scale 2 --no-sound --netplay-join 127.0.0.1:7600
