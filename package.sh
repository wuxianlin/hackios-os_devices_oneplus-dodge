#!/bin/bash
# Assemble the release zip: the boot image, the kernel, the payload builder and
# the Linux/Windows launcher. Run from this repository's root after the build
# (the GitHub workflow does it; the pieces land beside this script).
set -e
cd "$(dirname "$0")"
VER=$(git describe --tags --always 2>/dev/null || echo dev)
DIR=dist/hackios-dodge-$VER
rm -rf dist && mkdir -p "$DIR"

cp boot.img kernel.bin "$DIR/"
if [ -f root.apfs ]; then cp root.apfs "$DIR/"; fi          # the boot RAM disk (~42 MB, /dev/md1)
cp hackios-os/xnuloader/mkpayload.py "$DIR/"                # builds the per-boot kernel payload
cp launcher/hackios.py launcher/run.sh launcher/run.bat "$DIR/"
grep -v '^#' devices/oneplus-dodge/args | grep -v '^$' > "$DIR/device-args"
chmod +x "$DIR/run.sh"
cp loader.efi "$DIR/"                                        # for rebuilding boot.img

cat > "$DIR/README.txt" <<'EOF'
HakiOS for the OnePlus 13 (dodge) -- iOS 16.5 booted over fastboot, nothing flashed.

Needs on the computer: python3 and platform-tools (fastboot + adb) in PATH or in a
platform-tools/ folder next to these files:
  https://developer.android.com/studio/releases/platform-tools
The phone: unlocked bootloader, in Android with USB debugging or in fastboot.

  1. Put your images in this folder: system.apfs (the system, from Apple's
     IPSW -- `hackios.py ramdisk system.apfs` uploads it once per power-on)
     and root.apfs (the small boot disk, in every run's payload).   (once)
  2. Linux: ./run.sh        Windows: run.bat                     (every boot)
     Or by hand:
       python3 hackios.py boot
       python3 hackios.py ramdisk system.apfs   (once per power-on, ~1 min)
       python3 hackios.py run                  (SECS=900 CMDLINE="-v")

iOS boots in about two minutes; touch works, including the edge gestures. The
session ends after SECS seconds (the screen then freezes -- that is the session
ending, not a hang; python3 hackios.py stop ends it early, fastboot reboot
leaves). The kernel and iOS are never installed to the phone.
EOF

( cd dist && zip -qr hackios-dodge-$VER.zip hackios-dodge-$VER )
echo "package.sh: dist/hackios-dodge-$VER.zip"
unzip -l dist/hackios-dodge-$VER.zip | tail -12
