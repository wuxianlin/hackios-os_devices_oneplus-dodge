# Device: OnePlus 13 (dodge)

Qualcomm SM8750 ("sun", Snapdragon 8 Elite). Made by `tools/new-device.py` on 2026-10-29 from the
phone's own device tree (`live.dtb`, pulled from its Android), then edited as the port went; this
repository carries what is true of this phone and of its SoC family and of no other
([docs/porting/repositories.md](https://github.com/oosexgang/hakios/blob/main/docs/porting/repositories.md)).

| What | Value |
|---|---|
| Board (the tree's `model`) | Qualcomm Technologies, Inc. Sun MTP,dodge |
| Compatible | `qcom,sun-mtp qcom,sun qcom,sunp-mtp qcom,sunp qcom,mtp` |
| SoC layer | `soc/` (sun) |
| Drivers of its own | `drivers/input/hakios.dodge_touch.c` (Synaptics S3910), `drivers/wifi/hakios.wcn.c` (WCN7850 PCIe bring-up) |

## What works (seen on the phone, October 2026)

A run reaches SpringBoard on the GPU (Adreno 830, the 23-stage bring-up and the RedwoodGPU
service, Mesa rendering), with the touch screen, all three buttons; the audio DSP's PAS
bring-up steps and the WCN7850's read-only PCIe probe also run. What is not brought up yet:
sound out, Wi-Fi as a network, UFS storage, modem, cameras, haptic, thermal. The full story of
the touch panel alone (SPMI writes, RPMh votes through the RSC, GPIO bit-bang SPI, the
TouchCom protocol) is in the head of [drivers/input/hakios.dodge_touch.c](drivers/input/hakios.dodge_touch.c).

`configs/dodge.config` is the record: what is built in, and every boot-argument of the working
runs. Four cores (`cpus=4`) is the proven boot; all eight have been booted (`cpus=8`) too.
The GPU needs its whole argument set together (`touchscreen=0 wfi=0 wfe_mode=0` are required
alongside the `adreno8` set by the ARC clock votes of the kernel's adreno8_rpmh.c), a cold
Android settle before the run, the GPU firmware of `fw-gpu/`, and the GL system and root
images (Mesa) -- the plain root stops at init's self checks.

**The SCM waitqueue protocol is reverted in this port's patch** (patches/xnu-dodge.patch carries the
adsp driver without it): the polling GET_WQ_CTX/WAITQ_RESUME loop that the audio DSP's long calls
want deadlocks the adreno8 secure handover -- every run that passed GPU step 5 hung the whole
kernel. Until it is implemented bounded, the audio DSP's bring-up arguments stay staged and
the adsp driver out of the boot.

## The SoC addresses (`soc.args`)

Read out of the tree by `tools/soc-args.py` and **hand-corrected**: the tool was written for the
SC7280's tree, and on this one it misses six values. What it missed, and where each really is in
the tree (all verified against `live.dtb`):

| Argument | Value | Where in the tree |
|---|---|---|
| `soc_aoss` | `0xc300000` | `/soc/power-controller@c300000`, compatible `qcom,aoss-qmp` -- no SoC name in it, which the tool's matcher requires |
| `soc_cmd_db` | `0x81c60000` | `/reserved-memory/aop_cmd_db_region@81c60000` -- the node is named `aop_cmd_db_region`, not the `cmd_db` the tool looks for |
| `soc_mpss_mem` | `0x8ba00000` | `/reserved-memory/mpss_region@8ba00000` (`mpss_region`, not `mpss`) |
| `modem_ipa_fw_at` | `0x9b090000` | `/reserved-memory/ipa_gsi_region@9b090000` (`ipa_gsi_region`, not `ipa_gsi`) |
| `soc_smem` | `0x81d00000` | `/reserved-memory/smem_region@81d00000`: this tree has no `/soc/qcom,smem` node whose `memory-region` the tool follows |
| `soc_pas_mpss` | -- | **not derivable**: `/soc/remoteproc-mss@04080000` (`qcom,sun-modem-pas`) carries no `qcom,pas-id`; find the PAS id in the vendor kernel before the modem is brought up |

`pull-soc.sh` regenerates `soc.args` from the phone on adb (and would drop the hand corrections:
re-apply them, or teach `tools/soc-args.py` the names above). The run scripts add the lines that
differ from the kernel's built-in defaults to the command line (`tools/soc-only-changes.sh`).

The audio DSP's own addresses (shared memory, DSP memory, command database, sequencer, IPCC) are
NOT in `soc.args` -- the modem's arguments are -- but in `configs/dodge.config` as the staged
`adsp_*` boot-arguments of the proven bring-up: `adsp_smem=0x81d00000 adsp_smem_size=0x200000
adsp_mem=0x9ee00000 adsp_size=0x5080000 adsp_cmddb=0x81c60000 adsp_rsc=0x16520000
adsp_ipcc=0x406000`, all of them straight out of the same tree (`smem_region`,
`adspslpi_region@9ee00000` 0x5080000 bytes, `aop_cmd_db_region`, `rsc@16500000` drv-2,
`qcom,ipcc@406000`).

## Firmware

`pull-firmware.sh` pulls the audio DSP's signed images off the phone (with root: they are
unreadable by the shell user): `fw-audio/` = `adsp.mdt` + `adsp.b00..b36`, and the SM8750's
`adsp_dtb.*` (the DSP's own device tree, its own PAS image, which TrustZone starts first), plus
`cdsp.*`/`cdsp_dtb.*` as the cross-check of the release (`adsp_fw=cdsp`). Never committed.

## Running it on the phone

From the common repository (the UEFI pieces come from the Mu-Silicium tree beside it, symlinked in;
every piece of the boot image is overridable -- the env below is this phone's):

```sh
export UEFI_FD=$PWD/Mu-Silicium/Build/dodgePkg/RELEASE_CLANGPDB/FV/SILICIUM_UEFI.fd
export DTB=$PWD/Mu-Silicium/Resources/DTBs/dodge.dtb
export FD_BASE=0xA7100000 FD_SIZE=0x300000 PAYLOAD_BASE=0xF0000000 BOOT_HEADER=4
make dodge_defconfig && make config && make kernel -j8 && make loader
SOC_ARGS= make phone SECS=600
```

`make phone` takes this device's arguments from the .config (`tools/hakios-config.py args`; the
proven set: `cpus=4 buttons=1 buttons_pin=0x18d buttons_swap=1 synaptics=1 touchscreen=0
wfi=0 wfe_mode=0 adreno8=23 adreno8_keep=1 adreno8_return=0 adreno8_gmuclk=2
adreno8_secure_preflight=4`) and builds `xnu-usb.img` with the env above. **`SOC_ARGS=` matters**:
the modem's `soc_*` arguments of `soc.args` are dormant in this configuration (MODEM is left
out) and with them the kernel's command line overflows and loses its tail; drop them until the
modem is brought up, when they are what `soc.args` is for.

## The three changes this phone needed outside this repository

Kept as commits on the fork of the common repository (see its `LOCAL-CHANGES.md`):

1. `drivers/input/hakios.buttons.c` -- the version-7 PMIC arbiter layout of the SM8750 (its own
   core/table/observer addresses, up to 1024 APIDs) and the pin-setup writes for the volume keys;
2. `drivers/audio/hakios.adsp.c` -- the staged bring-up arguments (`adsp_*`), the RSC's
   version-3 command layout, the DSP's second signed image, the page-at-a-time shared-memory
   access, and the interrupt-storm avoidances;
3. `xnuloader/loader.c` -- `platform_dodge`: the SM8750's core map (no uniform MPIDR shift) and
   the twice-as-wide GIC redistributor stride, patched into the kernel while it loads.

## Check

`tools/hakios-device.py check dodge` (from the common repository).
