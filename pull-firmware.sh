#!/bin/bash
# Pull this phone's own firmware out of its Android into the fw-*/ directories at the top of the
# repository (never committed: it is the vendor's). The phone is on adb, in Android.
#
# What this port loads today is the audio DSP's: its signed image (adsp.mdt and its b-files) and
# the SM8750's second signed image, the DSP's own device tree (adsp_dtb.*), which TrustZone starts
# before the DSP itself; the compute DSP's pair (cdsp.*, cdsp_dtb.*) comes too, as the cross-check
# of the audio DSP's release (boot-args adsp_fw=cdsp adsp_pas=18 adsp_dtb_pas=0x25).
#
# On this phone adsp.mdt is r--r----- system:system: adb cannot read it directly, so root copies
# the lot to /data/local/tmp first (the old port's pull-firmware.sh, proven).
set -e
cd "$(dirname "$0")/../.."

adb get-state >/dev/null 2>&1 || { echo "pull-firmware.sh: the phone is not on adb"; exit 1; }

src=""
for d in /vendor/firmware_mnt/image /vendor/firmware; do
	if adb shell "su -c 'test -f $d/adsp.mdt'" >/dev/null 2>&1; then src=$d; break; fi
done
[ -n "$src" ] || { echo "pull-firmware.sh: no adsp.mdt found on the phone (looked in /vendor/firmware_mnt/image and /vendor/firmware)"; exit 1; }

echo "pull-firmware.sh: copying $src/{adsp,adsp_dtb,cdsp,cdsp_dtb}.* out with root (unreadable by the shell user)"
adb shell "su -c 'mkdir -p /data/local/tmp/fw; cp $src/adsp.mdt $src/adsp.b* $src/adsp_dtb.* $src/cdsp.mdt $src/cdsp.b* $src/cdsp_dtb.* /data/local/tmp/fw/ 2>/dev/null; chmod 644 /data/local/tmp/fw/*'"
rm -rf fw-audio
mkdir -p fw-audio
for f in $(adb shell "su -c 'ls /data/local/tmp/fw/'" | tr -d '\r'); do
	adb pull "/data/local/tmp/fw/$f" fw-audio/ >/dev/null
done
adb shell "su -c 'rm -rf /data/local/tmp/fw'" || true
[ -f fw-audio/adsp.mdt ] || { echo "pull-firmware.sh: the copy did not come back"; exit 1; }
[ -f fw-audio/adsp_dtb.mdt ] || echo "pull-firmware.sh: no adsp_dtb.mdt -- on SM8750 the DSP's own tree is what TrustZone starts first; without it the DSP will not start"
echo "pull-firmware.sh: fw-audio: $(ls -1 fw-audio | wc -l | tr -d ' ') files ($(du -sh fw-audio | cut -f1))"
echo "pull-firmware.sh: done (ADSP=1 userland/build.sh puts fw-audio into the small image)"
