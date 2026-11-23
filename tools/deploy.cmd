@echo off
rem Copy the newest DisplayMOD .deb to the CardputerZero and install it.
rem   tools\deploy.cmd 192.168.137.203 [user]
rem Asks for the SSH password, then the sudo password. Closes when done.
setlocal
if "%~1"=="" (echo usage: tools\deploy.cmd IP [user] & exit /b 1)
set IP=%~1
set U=%~2
if "%U%"=="" set U=wade-cpz
cd /d "%~dp0.."
for /f "delims=" %%f in ('dir /b /o:d dist\displaymod_*_arm64.deb') do set DEB=%%f
if "%DEB%"=="" (echo no .deb in dist & exit /b 1)
echo Installing %DEB% on %U%@%IP%
scp "dist\%DEB%" %U%@%IP%:/tmp/ || exit /b 1
ssh -t %U%@%IP% "sudo apt install -y /tmp/%DEB%; rm -f /tmp/%DEB%"
