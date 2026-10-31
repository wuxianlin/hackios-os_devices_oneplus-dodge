#!/bin/bash
# Assemble the release zip: the boot image, the kernel, the payload builder and
# the Linux/Windows launcher. The CI runs this script in place, at devices/dodge/
# of the hackios tree; the build's pieces are found from there (or from beside
# this script, when it is run out of a checkout of this repository alone).
set -e
cd "$(dirname "$(readlink -f "$0")")"
TOP=../..
[ -f "$TOP/xnuloader/mkpayload.py" ] || TOP=.                       # a lone checkout
VER=$(git describe --tags --always 2>/dev/null || echo dev)
DIR=dist/hackios-dodge-$VER
rm -rf dist && mkdir -p "$DIR"

for f in boot.img kernel.bin; do [ -f "$TOP/$f" ] && cp "$TOP/$f" "$DIR/"; done
[ -f "$TOP/xnuloader/loader.efi" ] && cp "$TOP/xnuloader/loader.efi" "$DIR/"
[ -f "$TOP/userland/root.apfs" ] && cp "$TOP/userland/root.apfs" "$DIR/"   # the boot RAM disk
[ -f "$TOP/userland/system.apfs" ] && cp "$TOP/userland/system.apfs" "$DIR/"   # the system image (Apple's, built by the system-image step)
[ -f root.apfs ] && cp root.apfs "$DIR/"
cp "$TOP/xnuloader/mkpayload.py" "$DIR/"                   # builds the per-boot kernel payload
if [ -f "$TOP/tools/kernel-version.py" ]; then                 # the kernel.ver mkpayload would ask a full tree for
	HAKIOS_VERSION_SUFFIX="-gdodge-$(git rev-parse --short HEAD 2>/dev/null || echo dev)" \
		python3 "$TOP/tools/kernel-version.py" --config "$TOP/.config" > "$DIR/kernel.ver" 2>/dev/null || true
fi
cp launcher/hackios.py launcher/run.sh launcher/run.bat "$DIR/"
# the device's kernel arguments (the GPU set of configs/dodge.config, for the launcher to merge)
python3 - "$DIR/device-args" <<'PYEOF'
import sys
bootargs = []
for line in open('configs/dodge.config'):
    if line.startswith('CONFIG_BOOTARGS='):
        bootargs = line.split('=', 1)[1].strip().strip('"').split()
open(sys.argv[1], 'w').write(' '.join(bootargs) + '\n')
PYEOF
chmod +x "$DIR/run.sh"

cat > "$DIR/README.txt" <<'EOF'
HakiOS for the OnePlus 13 (dodge) -- iOS 16.5 booted over fastboot, nothing flashed.

Needs on the computer: python3 and platform-tools (fastboot + adb) in PATH or in a
platform-tools/ folder next to these files:
  https://developer.android.com/studio/releases/platform-tools
The phone: unlocked bootloader, in Android with USB debugging or in fastboot.

  1. Everything needed is in this zip: system.apfs (the system, from Apple's IPSW),
     root.apfs (the small boot disk), boot.img, kernel.bin and the loader.  (once)
  2. Linux: ./run.sh        Windows: run.bat                     (every boot)
     Or by hand:
       python3 hackios.py boot
       python3 hackios.py ramdisk system.apfs   (once per power-on, ~1 min)
       python3 hackios.py run                  (SECS=900 CMDLINE="-v")

iOS boots in about two minutes; the touch screen works. The session ends after SECS
seconds (the screen then freezes -- that is the session ending, not a hang; python3
hackios.py stop ends it early, fastboot reboot leaves). The kernel and iOS are never
installed to the phone.
EOF

( cd dist && zip -qr hackios-dodge-$VER.zip hackios-dodge-$VER )
echo "package.sh: dist/hackios-dodge-$VER.zip"
unzip -l dist/hackios-dodge-$VER.zip | tail -6
