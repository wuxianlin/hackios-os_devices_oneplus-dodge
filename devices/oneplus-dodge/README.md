# Device: OnePlus 13 (dodge)

Everything in HakiOS that belongs to this phone is here; the kernel, loader and user space above
know nothing about it (see the main README and `devices/poco-x5-pro` for the layout this follows).
A port of [HakiOS](https://github.com/oosexgang/hackios-xnu-kernel), started from the POCO X5 Pro.

| What | Where |
|---|---|
| SoC | Qualcomm Snapdragon 8 Elite (SM8750) |
| SoC addresses | `soc.args` (from the phone's device tree, `tools/soc-args.py`, via `pull-soc.sh`) |
| UEFI for the phone | `uefi/`: a port of Mu-Silicium (upstream carries the `dodgePkg` platform; this adds the boot chain: a patch, one inside `Mu_Basecore`, and the XnuLoader; `apply.sh` clones and applies) |
| The port's story | [PORTING-DODGE.md](PORTING-DODGE.md) — the touch controller alone took SPMI, RPMh, GPIO bit-bang SPI and the TouchComm protocol; every step is written down |

What works: boot to SpringBoard, the display, battery, all eight CPU cores, the **touchscreen**
(Synaptics S3910 — tap, drag, multitouch, edge gestures, 125 Hz) and the **power and volume-up
keys** (the PON block; volume-down is a PMIC GPIO whose pull-up setup would need a PMIC register
write that kills this kernel — parked). What does not (yet): GPU acceleration, camera, cellular,
Wi-Fi (WCN7850 is ath12k; the tree has ath11k only) and sound — the audio DSP path is blocked
deeper down: this fastboot VM's stage-2 maps only the first 512 KB of the shared memory at
0x81d00000 (the APPS↔ADSP partition lies beyond; Android's own SMEM at 0x80900000 is not mapped
for this VM at all), see PORTING-DODGE.md.

The port needs three small changes to files the device directory cannot hold; they are kept as a
patch in the port's build repository (`patches/hakios-os.patch` there): the touch driver and its
two lines of wiring in the kernel (`xnu/hakios.patch`, `hakios.input.[ch]`, `hakios.dodge_touch*`),
the `platform_dodge` entry and the GIC-stride patch in `xnuloader/loader.c`, and the environment
overrides in `mkxnuimg.py`.

## Running

From the top of the repository, as for the POCO X5 Pro (the main README): `./xl enter`,
`./xl ramdisk`, `./xl run` — plus `synaptics=1` among the kernel arguments for the touch screen,
and the image addresses for `enter`:

```sh
UEFI_FD=Mu-Silicium/Build/dodgePkg/RELEASE_CLANGPDB/FV/SILICIUM_UEFI.fd \
DTB=Mu-Silicium/Resources/DTBs/dodge.dtb \
FD_BASE=0xA7100000 FD_SIZE=0x300000 PAYLOAD_BASE=0xF0000000 BOOT_HEADER=4 \
	./xl enter
XL_CMDLINE="cpus=8 synaptics=1" ./xl run
```

(The port's build repository packages a launcher that does exactly this, for Linux and Windows.)
