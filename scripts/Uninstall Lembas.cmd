@echo off
title Remove Lembas
if exist "%LOCALAPPDATA%\Lembas\Tools\Uninstall-Lembas.ps1" (
  powershell.exe -NoProfile -ExecutionPolicy Bypass -File "%LOCALAPPDATA%\Lembas\Tools\Uninstall-Lembas.ps1"
) else (
  powershell.exe -NoProfile -ExecutionPolicy Bypass -File "%~dp0payload\Tools\Uninstall-Lembas.ps1"
)
