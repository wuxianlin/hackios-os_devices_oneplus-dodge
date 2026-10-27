#!/bin/sh
# One-shot: boot the system (upload the ramdisk the first time only).
# Put this script next to boot.img, kernel.bin, system.apfs.
cd "$(dirname "$0")"
command -v python3 >/dev/null || { echo 'python3 not found'; exit 1; }
if [ ! -f system.apfs ]; then
	echo "Put your system.apfs in this folder (or set RAMDISK=none), then:"
	echo "  python3 hackios.py ramdisk system.apfs   once per power-on"
	echo "  ./run.sh                                every time"
	exec python3 hackios.py boot
# also: screenshot, log, stop, reboot (see: python3 hackios.py)
# device-args beside this script carries the touch/button kernel arguments
fi
python3 hackios.py boot &&
python3 hackios.py ramdisk system.apfs &&
python3 hackios.py run
