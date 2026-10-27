@echo off
rem One-shot: boot the system (upload the ramdisk the first time only).
rem Put this directory next to boot.img, kernel.bin, system.apfs.
cd /d "%~dp0"
where python >nul 2>nul || (echo python3 not found: https://www.python.org/downloads/ & exit /b 1)
if not exist system.apfs (
  echo Put your system.apfs in this folder (or set RAMDISK=none), then:
  echo   python hackios.py ramdisk system.apfs   once per power-on
  echo   run                                    every time
  python hackios.py boot
  exit /b 0
)
python hackios.py boot
python hackios.py ramdisk system.apfs
python hackios.py run
