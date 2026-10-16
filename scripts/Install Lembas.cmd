@echo off
title Lembas for Windows
powershell.exe -NoProfile -ExecutionPolicy Bypass -File "%~dp0payload\Tools\Install-Lembas.ps1" %*
