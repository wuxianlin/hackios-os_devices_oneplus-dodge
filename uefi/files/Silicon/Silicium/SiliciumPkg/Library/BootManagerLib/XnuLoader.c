/**
  XNU loader for Silicium devices (dodge bring-up), stage 0: diagnostics and payload start.

  Runs when BDS finds nothing to boot. Collects what the real loader needs (exception level,
  CPU feature registers, framebuffer, memory map, payload), writes it as text to a raw GPT
  partition (XNU_LOG_PARTITION) so it can be read back from Android, neutralizes the Qualcomm
  virtual watchdog and starts "loader.efi" out of the XNUP payload when there is one.

  SPDX-License-Identifier: BSD-2-Clause-Patent
**/

#include <Uefi.h>

#include <Library/BaseLib.h>
#include <Library/BaseMemoryLib.h>
#include <Library/MemoryAllocationLib.h>
#include <Library/PrintLib.h>
#include <Library/UefiBootServicesTableLib.h>
#include <Library/UefiRuntimeServicesTableLib.h>

#include <Protocol/BlockIo.h>
#include <Protocol/GraphicsOutput.h>
#include <Protocol/PartitionInfo.h>

#include "XnuLoader.h"

#define LOG_SIZE  SIZE_256KB

STATIC VOID FlushLog (VOID);

STATIC CHAR8  *mLog;
STATIC UINTN   mLogLen;

VOID
EFIAPI
XnuLog (
  IN CONST CHAR8 *Format,
  ...)
{
  VA_LIST Marker;

  if (mLog == NULL || mLogLen >= LOG_SIZE - 512) {
    return;
  }

  VA_START (Marker, Format);
  mLogLen += AsciiVSPrint (mLog + mLogLen, LOG_SIZE - mLogLen, Format, Marker);
  VA_END (Marker);
}

#define READ_SYSREG(Name) ({ UINT64 _Value; __asm__ volatile ("mrs %0, " #Name : "=r" (_Value)); _Value; })

// A translation check (AT instruction) first: the payload address is only mapped when
// the device's memory map lists it; reading it unmapped would take the firmware down.
STATIC
BOOLEAN
Mapped (
  IN UINT64 Address)
{
  UINT64 El, Par;

  __asm__ volatile ("mrs %0, CurrentEL" : "=r" (El));
  if (El == 4) {
    __asm__ volatile ("at s1e1r, %1; isb; mrs %0, par_el1" : "=r" (Par) : "r" (Address) : "memory");
  } else {
    __asm__ volatile ("at s1e2r, %1; isb; mrs %0, par_el1" : "=r" (Par) : "r" (Address) : "memory");
  }

  return (Par & BIT0) == 0;
}

// Qualcomm hypervisor virtual watchdog (hh_virt_wdt) SMCs: XBL leaves it running and nothing
// in this firmware pets it, so the phone resets ~30 s in. Turn it off and stretch the time;
// the XNU loader re-arms and pets it once it runs ("watchdog: ..." in its log).
STATIC
UINT64
SmcCall (
  IN UINT64 X0,
  IN UINT64 X1,
  IN UINT64 X2)
{
  register UINT64 x0 __asm__ ("x0") = X0;
  register UINT64 x1 __asm__ ("x1") = X1;
  register UINT64 x2 __asm__ ("x2") = X2;

  __asm__ volatile ("smc #0" : "+r" (x0), "+r" (x1), "+r" (x2)
                    : : "x3", "x4", "x5", "x6", "x7", "x8", "x9", "x10", "x11", "x12", "x13", "x14", "x15", "x16", "x17");
  return x0;
}

#define VIRT_WDT_CONTROL  0x86000005ULL
#define VIRT_WDT_STATUS   0x86000006ULL
#define VIRT_WDT_PET      0x86000007ULL
#define VIRT_WDT_SETTIME  0x86000008ULL

EFI_STATUS
XnuLogInit ()
{
  if (mLog == NULL) {
    mLog = AllocateZeroPool (LOG_SIZE);
  }

  return mLog != NULL ? EFI_SUCCESS : EFI_OUT_OF_RESOURCES;
}

VOID
XnuLogFlush ()
{
  FlushLog ();
}

VOID
WdtNeutralize ()
{
  UINT64 SetTime = SmcCall (VIRT_WDT_SETTIME, 0xfff0, 0xfff0); /* 16 bits of ms: ~65 s */
  UINT64 Control = SmcCall (VIRT_WDT_CONTROL, 0, 0);           /* 0: both timers off */
  UINT64 Pet     = SmcCall (VIRT_WDT_PET, 0, 0);
  UINT64 Status  = SmcCall (VIRT_WDT_STATUS, 0, 0);

  XnuLog ("[vwdt] status=%lx set_time=%lx control=%lx pet=%lx\n", Status, SetTime, Control, Pet);
}

STATIC
EFI_BLOCK_IO_PROTOCOL *
FindPartition (
  IN CONST CHAR16 *Name)
{
  EFI_STATUS  Status;
  EFI_HANDLE *Handles;
  UINTN       Count;

  Status = gBS->LocateHandleBuffer (ByProtocol, &gEfiPartitionInfoProtocolGuid, NULL, &Count, &Handles);
  if (EFI_ERROR (Status)) {
    return NULL;
  }

  for (UINTN i = 0; i < Count; i++) {
    EFI_PARTITION_INFO_PROTOCOL *Info;
    EFI_BLOCK_IO_PROTOCOL       *BlockIo;

    Status = gBS->HandleProtocol (Handles[i], &gEfiPartitionInfoProtocolGuid, (VOID **)&Info);
    if (EFI_ERROR (Status) || Info->Type != PARTITION_TYPE_GPT) {
      continue;
    }

    if (StrnCmp (Info->Info.Gpt.PartitionName, Name, 36) != 0) {
      continue;
    }

    Status = gBS->HandleProtocol (Handles[i], &gEfiBlockIoProtocolGuid, (VOID **)&BlockIo);
    if (!EFI_ERROR (Status)) {
      return BlockIo;
    }
  }

  return NULL;
}

STATIC
VOID
FlushLog ()
{
  EFI_STATUS             Status;
  EFI_BLOCK_IO_PROTOCOL *BlockIo;
  UINTN                  BlockSize;
  UINTN                  WriteSize;

  if (mLog == NULL || mLogLen == 0) {
    return;
  }

  BlockIo = FindPartition (XNU_LOG_PARTITION);
  if (BlockIo == NULL) {
    return;
  }

  BlockSize = BlockIo->Media->BlockSize;
  WriteSize = ALIGN_VALUE (mLogLen + 1, BlockSize);

  Status = BlockIo->WriteBlocks (BlockIo, BlockIo->Media->MediaId, 0, WriteSize, mLog);
  if (!EFI_ERROR (Status)) {
    BlockIo->FlushBlocks (BlockIo);
  }
}

STATIC
VOID
LogCpu ()
{
  XnuLog ("[cpu]\n");
  XnuLog ("CurrentEL=%lx\n", READ_SYSREG (CurrentEL) >> 2);
  XnuLog ("MIDR_EL1=%016lx\n", READ_SYSREG (MIDR_EL1));
  XnuLog ("MPIDR_EL1=%016lx\n", READ_SYSREG (MPIDR_EL1));
  XnuLog ("ID_AA64MMFR0_EL1=%016lx\n", READ_SYSREG (ID_AA64MMFR0_EL1));
  XnuLog ("ID_AA64MMFR1_EL1=%016lx\n", READ_SYSREG (ID_AA64MMFR1_EL1));
  XnuLog ("ID_AA64MMFR2_EL1=%016lx\n", READ_SYSREG (ID_AA64MMFR2_EL1));
  XnuLog ("ID_AA64PFR0_EL1=%016lx\n", READ_SYSREG (ID_AA64PFR0_EL1));
  XnuLog ("ID_AA64PFR1_EL1=%016lx\n", READ_SYSREG (ID_AA64PFR1_EL1));
  XnuLog ("ID_AA64ISAR0_EL1=%016lx\n", READ_SYSREG (ID_AA64ISAR0_EL1));
  XnuLog ("ID_AA64ISAR1_EL1=%016lx\n", READ_SYSREG (ID_AA64ISAR1_EL1));
  XnuLog ("ID_AA64DFR0_EL1=%016lx\n", READ_SYSREG (ID_AA64DFR0_EL1));
  XnuLog ("SCTLR_EL1=%016lx\n", READ_SYSREG (SCTLR_EL1));
  XnuLog ("TCR_EL1=%016lx\n", READ_SYSREG (TCR_EL1));
  XnuLog ("TTBR0_EL1=%016lx\n", READ_SYSREG (TTBR0_EL1));
  XnuLog ("VBAR_EL1=%016lx\n", READ_SYSREG (VBAR_EL1));
  XnuLog ("CNTFRQ_EL0=%ld\n", READ_SYSREG (CNTFRQ_EL0));
  XnuLog ("DAIF=%lx\n", READ_SYSREG (DAIF));
}

STATIC
VOID
LogDisplay ()
{
  EFI_STATUS                    Status;
  EFI_GRAPHICS_OUTPUT_PROTOCOL *Gop;

  XnuLog ("[display]\n");

  Status = gBS->LocateProtocol (&gEfiGraphicsOutputProtocolGuid, NULL, (VOID **)&Gop);
  if (EFI_ERROR (Status)) {
    XnuLog ("gop=missing status=%r\n", Status);
    return;
  }

  XnuLog ("fb_base=%lx fb_size=%lx\n", Gop->Mode->FrameBufferBase, (UINT64)Gop->Mode->FrameBufferSize);
  XnuLog ("width=%d height=%d stride=%d format=%d\n",
       Gop->Mode->Info->HorizontalResolution, Gop->Mode->Info->VerticalResolution,
       Gop->Mode->Info->PixelsPerScanLine, Gop->Mode->Info->PixelFormat);
}

STATIC
VOID
LogMemoryMap ()
{
  EFI_STATUS             Status;
  EFI_MEMORY_DESCRIPTOR *Map;
  UINTN                  MapSize = 0;
  UINTN                  MapKey;
  UINTN                  DescSize;
  UINT32                 DescVersion;

  XnuLog ("[memmap]\n");

  Status = gBS->GetMemoryMap (&MapSize, NULL, &MapKey, &DescSize, &DescVersion);
  if (Status != EFI_BUFFER_TOO_SMALL) {
    XnuLog ("getmemorymap status=%r\n", Status);
    return;
  }

  MapSize += SIZE_4KB;
  Map      = AllocatePool (MapSize);
  if (Map == NULL) {
    return;
  }

  Status = gBS->GetMemoryMap (&MapSize, Map, &MapKey, &DescSize, &DescVersion);
  if (EFI_ERROR (Status)) {
    XnuLog ("getmemorymap status=%r\n", Status);
    return;
  }

  for (UINTN Offset = 0; Offset < MapSize; Offset += DescSize) {
    EFI_MEMORY_DESCRIPTOR *Desc = (EFI_MEMORY_DESCRIPTOR *)((UINT8 *)Map + Offset);

    XnuLog ("%016lx %016lx type=%d attr=%lx\n", Desc->PhysicalStart,
         Desc->PhysicalStart + EFI_PAGES_TO_SIZE (Desc->NumberOfPages), Desc->Type, Desc->Attribute);
  }

  FreePool (Map);
}

STATIC
VOID
LogPayload (
  IN BOOLEAN PayloadMapped)
{
  XnuLog ("[payload]\n");
  XnuLog ("base=%lx mapped=%d", (UINT64)XNU_PAYLOAD_BASE, PayloadMapped);

  if (PayloadMapped) {
    XNU_PAYLOAD_HEADER *Header = (XNU_PAYLOAD_HEADER *)XNU_PAYLOAD_BASE;

    XnuLog (" magic=%08x size=%x\n", Header->Magic, Header->Size);
  } else {
    XnuLog ("\n");
  }
}

//
// The payload is a list of named blobs. If it carries "loader.efi", that is
// the real XNU loader: start it straight from memory, so the loader can be
// iterated on without rebuilding this firmware.
//
STATIC
VOID
StartPayloadLoader ()
{
  XNU_PAYLOAD_HEADER *Header = (XNU_PAYLOAD_HEADER *)XNU_PAYLOAD_BASE;
  UINT8              *Cursor;
  UINT8              *End;

  if (!Mapped ((UINT64)XNU_PAYLOAD_BASE) ||
      Header->Magic != XNU_PAYLOAD_MAGIC || Header->Size > XNU_PAYLOAD_MAX_SIZE) {
    return;
  }

  Cursor = (UINT8 *)(Header + 1);
  End    = Cursor + Header->Size;

  while (Cursor + sizeof (XNU_PAYLOAD_ENTRY) <= End) {
    XNU_PAYLOAD_ENTRY *Entry = (XNU_PAYLOAD_ENTRY *)Cursor;

    if (Entry->Name[0] == 0) {
      break;
    }

    if (AsciiStrnCmp (Entry->Name, "loader.efi", sizeof (Entry->Name)) == 0) {
      EFI_HANDLE Handle;
      EFI_STATUS Status = gBS->LoadImage (FALSE, gImageHandle, NULL, Entry + 1, Entry->Size, &Handle);

      XnuLog ("[loader] LoadImage(size=%x)=%r\n", Entry->Size, Status);
      FlushLog ();
      if (!EFI_ERROR (Status)) {
        Status = gBS->StartImage (Handle, NULL, NULL);
        XnuLog ("[loader] StartImage=%r\n", Status);
        FlushLog ();
      }

      return;
    }

    Cursor += sizeof (XNU_PAYLOAD_ENTRY) + ALIGN_VALUE (Entry->Size, 16);
  }
}

VOID
XnuLoaderMain ()
{
  EFI_TIME Time;
  BOOLEAN  PayloadMapped;

  mLog = AllocateZeroPool (LOG_SIZE);
  if (mLog != NULL) {
    XnuLog ("== XNULOADER LOG v2 ==\n");

    if (!EFI_ERROR (gRT->GetTime (&Time, NULL))) {
      XnuLog ("time=%04d-%02d-%02d %02d:%02d:%02d\n", Time.Year, Time.Month, Time.Day, Time.Hour, Time.Minute, Time.Second);
    }
  }

  WdtNeutralize ();

/* Enable GENI SE4 serial clock via EFI Clock protocol (goes through SCMI,
 * bypassing Gunyah's GCC write filtering). Also enable the QUP wrapper AHB clock. */
  {
    STATIC EFI_GUID kClockGuid = { 0x241AFAE6, 0x885F, 0x4F6C, { 0xA7, 0xEA, 0xC2, 0x8E, 0xAB, 0x79, 0xC3, 0xE5 } };
    VOID *pv = NULL;

    if (!EFI_ERROR (gBS->LocateProtocol (&kClockGuid, NULL, &pv)) && pv != NULL) {
      /* protocol layout: Version@0, GetClockID@8, EnableClock@0x10, ..., SetFreqHz@0x30 */
      typedef EFI_STATUS (EFIAPI *CLK_GET_ID)(VOID *, CONST CHAR8 *, UINTN *);
      typedef EFI_STATUS (EFIAPI *CLK_ENABLE)(VOID *, UINTN);
      CLK_GET_ID GetId = *(CLK_GET_ID *)((VOID **)pv + 1);
      CLK_ENABLE Enable = *(CLK_ENABLE *)((VOID **)pv + 2);

      XnuLog ("[clk] protocol=%x getid=%x enable=%x\n",
              (UINT64)(UINTN)pv, (UINT64)(UINTN)GetId, (UINT64)(UINTN)Enable);
      FlushLog ();

      typedef EFI_STATUS (EFIAPI *CLK_SET_FREQ)(VOID *, UINTN, UINT64, UINT32, UINT64 *);
      CLK_SET_FREQ SetFreq = *(CLK_SET_FREQ *)((VOID **)pv + 6);  /* offset 0x30 */

      CONST CHAR8 *names[] = { "gcc_qupv3_wrap1_s4_clk", "gcc_qupv3_wrap_1_s_ahb_clk", "gcc_qupv3_wrap_1_m_ahb_clk" };
      for (UINTN i = 0; i < 3; i++) {
        UINTN cid = 0;
        EFI_STATUS st = GetId (pv, names[i], &cid);

        if (EFI_ERROR (st)) {
          XnuLog ("[clk] %a: GetId err %r\n", names[i], st);
        } else {
          st = Enable (pv, cid);
          if (i == 0) {
            /* SE4: set frequency to 19.2 MHz (closest match) */
            UINT64 actual = 0;
            EFI_STATUS fs = SetFreq (pv, cid, 19200000ULL, 2 /* CLOSEST */, &actual);
            XnuLog ("[clk] %a: id=%u enable=%r freq=%llu (want 19.2MHz, %r)\n",
                    names[i], cid, st, actual, fs);
          } else {
            XnuLog ("[clk] %a: id=%u enable=%r\n", names[i], cid, st);
          }
        }
        FlushLog ();
      }
    } else {
      XnuLog ("[clk] ClockDxe protocol not found\n");
      FlushLog ();
    }
  }

/* GENI SE4 test */
  {
    volatile UINT32 *se = (volatile UINT32 *)0xa90000;

    XnuLog ("[geni] BEFORE access\n");
    FlushLog ();

    /* Reset the SE4 clock domain to restart the GENI firmware sequencer.
     * EFI Clock protocol: ResetClock at offset 0x78 = func ptr 15 */
    {
      STATIC EFI_GUID kClockGuid2 = { 0x241AFAE6, 0x885F, 0x4F6C, { 0xA7, 0xEA, 0xC2, 0x8E, 0xAB, 0x79, 0xC3, 0xE5 } };
      VOID *cpv = NULL;

      if (!EFI_ERROR (gBS->LocateProtocol (&kClockGuid2, NULL, &cpv)) && cpv != NULL) {
        typedef EFI_STATUS (EFIAPI *CLK_GET_ID2)(VOID *, CONST CHAR8 *, UINTN *);
        typedef EFI_STATUS (EFIAPI *CLK_RESET)(VOID *, UINTN, UINTN);
        CLK_GET_ID2 GetId2 = *(CLK_GET_ID2 *)((VOID **)cpv + 1);
        CLK_RESET Reset = *(CLK_RESET *)((VOID **)cpv + 15);  /* 0x78 / 8 = 15 */
        UINTN cid = 0;

        if (!EFI_ERROR (GetId2 (cpv, "gcc_qupv3_wrap1_s4_clk", &cid))) {
          EFI_STATUS rs1 = Reset (cpv, cid, 1);  /* assert reset */
          /* small delay */
          for (volatile int d = 0; d < 100000; d++) {}
          EFI_STATUS rs2 = Reset (cpv, cid, 0);  /* deassert */
          XnuLog ("[geni] clock reset: assert=%r deassert=%r (id=%u)\n", rs1, rs2, cid);
          FlushLog ();
        }
      }
    }
    XnuLog ("[geni] status=%x fw=%x mode=%x\n", se[0x40 / 4], se[0x68 / 4], se[0x258 / 4]);
    FlushLog ();

    /* switch to FIFO mode if in GSI */
    if (se[0x258 / 4] & 1) {
      se[0xe18 / 4] = 0;
      se[0x258 / 4] &= ~1u;
      XnuLog ("[geni] switched to FIFO\n");
      FlushLog ();
    }

    /* configure SPI: mode 0, 8-bit words */
    se[0x224 / 4] = 0;   /* CPHA */
    se[0x230 / 4] = 0;   /* CPOL */
    se[0x268 / 4] = 4;   /* WORD_LEN: 8 bits */
    se[0x260 / 4] = 0x0007f8fe;
    se[0x264 / 4] = 0x000ffefe;
    se[0x284 / 4] = 0x0007f8fe;
    se[0x288 / 4] = 0x000ffefe;
    se[0x254 / 4] = 0;   /* BYTE_GRAN */

    /* clear pending IRQs */
    se[0x610 / 4] = 0xffffffff;

    /* try a 4-byte SPI transfer: send 0xFF 0x00 0x07 0x00 */
    se[0x26c / 4] = 4;   /* TX len */
    se[0x270 / 4] = 4;   /* RX len */
    se[0x700 / 4] = 0x0700FF;  /* TX FIFO: 0xFF, 0x00, 0x07, 0x00 packed */
    se[0x600 / 4] = 3u << 27;  /* SPI full-duplex trigger */

    /* wait for completion */
    for (volatile int t = 0; t < 500000; t++) {
      if (se[0x610 / 4] & 1) break;  /* done */
    }
    UINT32 irq = se[0x610 / 4];
    UINT32 rx0 = se[0x780 / 4];  /* RX FIFO */
    UINT32 sts = se[0x40 / 4];

    XnuLog ("[geni] xfer: irq=%x rx=%x status=%x fifo=%x\n", irq, rx0, sts, se[0x804 / 4]);
    FlushLog ();
  }

#if 0  /* SPMI disabled: kernel driver is sole accessor */

  {
    volatile UINT32 *core = (volatile UINT32 *)0x0c400000;
    volatile UINT32 *obs = (volatile UINT32 *)0x0c440000;
    unsigned apid_count = core[1] & 0x7ff;
    int apid = -1;

    XnuLog ("[spmi] ver=%x feat=%x\n", core[0], core[1]);
    FlushLog ();

    /* (SpmiBus protocol probe removed: crashes the UEFI) */

    /* dump cmd_db entries named "gpio*" (full 24B entry: name[8] aux[8] addr[4] pad[4]) */
    {
      volatile UINT32 *db = (volatile UINT32 *)0x81c60000;

      for (unsigned sl = 0; sl < 8; sl++) {
        unsigned char *sd = (unsigned char *)db + 8 + sl * 16;
        unsigned off = sd[2] | sd[3] << 8;
        unsigned n = sd[6] | sd[7] << 8;

        if (!off || !n || n > 512) continue;
        for (unsigned i = 0; i < n; i++) {
          unsigned char *e = (unsigned char *)db + 0x90 + off + i * 24;
          UINT32 w1 = *(volatile UINT32 *)e;

          if (w1 == 0x6f697067 /* "gpio" */) {
            XnuLog ("[cmddb] gpio entry %u: aux=%x %x addr=%x tail=%x\n", i,
              *(volatile UINT32 *)(e + 8), *(volatile UINT32 *)(e + 12),
              *(volatile UINT32 *)(e + 16), *(volatile UINT32 *)(e + 20));
            FlushLog ();
          }
        }
      }
    }

    int apid4 = -1;   /* gpio4 (ppid 0x98c): unused by Android - ground-truth write test */

    {
      volatile UINT32 *cnfg = (volatile UINT32 *)0x0c42d000;
      int found = 0;

      for (unsigned i = 0; i < apid_count && i < 900; i++) {
        UINT32 v = core[0x2000 / 4 + i];
        if (!v) continue;
        if (((v >> 8) & 0xfff) == 0x98c && apid4 < 0 && (cnfg[i] & 7) == 0) { apid4 = (int)i; }
        if (((v >> 8) & 0xfff) == 0x98b) {
          UINT32 owner = cnfg[i] & 7;
          int irq_owner = (v >> 24) & 1;
          XnuLog ("[spmi] apid %u: owner=%u irq=%d\n", i, owner, irq_owner);
          FlushLog ();
          found++;
          /* prefer the APID our EE (0, APPS) owns for writes */
          if (owner == 0) { apid = (int)i; }
          else if (apid < 0) { apid = (int)i; }
        }
      }
      XnuLog ("[spmi] gpio3 apid=%d (%d mappings, count %u)\n", apid, found, apid_count);
      FlushLog ();
    }

    if (apid >= 0) {
      volatile UINT32 *o = obs + (0x20 * apid) / 4;
      volatile UINT32 *rwt = (volatile UINT32 *)(0x0c500000 + 0x1000 * (UINT64)apid);
      UINT32 wtest;

      /* S2 write-filter test: WDATA (+0x10) and CMD (+0x00) readback */
      wtest = rwt[4];
      rwt[4] = 0xA5A55A5A;
      wtest = rwt[4];
      XnuLog ("[spmi] wfilter: wdata wrote A5A55A5A read back %x\n", wtest);
      /* CMD readback: the arbiter may auto-clear CMD after execution, but a fresh
       * write should be visible for at least one cycle. Read immediately. */
      {
        UINT32 cmd_before = rwt[0];
        UINT32 st_before = rwt[2];

        rwt[0] = 0x10000400;                     /* EXT_WRITE 0x40 */
        wtest = rwt[0];                          /* read back CMD immediately */
        XnuLog ("[spmi] cmd: before=%x wrote 10000400 readback=%x (st %x -> %x)\n",
                cmd_before, wtest, st_before, rwt[2]);
        FlushLog ();
        /* also try the observer channel CMD readback */
        {
          volatile UINT32 *ot = obs + (0x20 * apid) / 4;
          UINT32 ocmd = ot[0];

          ot[0] = 0x68000400;                    /* EXT_READ 0x40 */
          wtest = ot[0];
          XnuLog ("[spmi] ocmd: before=%x wrote 68000400 readback=%x (st %x)\n",
                  ocmd, wtest, ot[2]);
          FlushLog ();
        }
      }
      FlushLog ();

      UINT32 st = 0;

      o[0] = (13u << 27) | (0x04u << 4) | 0u;   /* EXT_READ peripheral type */
      for (volatile int w = 0; w < 100000; w++) { st = o[2]; if (st & 1) break; }
      XnuLog ("[spmi] rd type: st=%x dat=%x\n", st, o[6] & 0xff);
      FlushLog ();

      if ((st & 1) && !(st & 0xe)) {
        const struct { UINT8 off, val; } seq[] = {
          { 0x40, 0x01 }, { 0x41, 0x00 }, { 0x44, 0x80 }, { 0x46, 0x80 },
        };
        for (unsigned i = 0; i < sizeof (seq) / sizeof (seq[0]); i++) {
          o[4] = seq[i].val;
          o[0] = (2u << 27) | ((UINT32)seq[i].off << 4) | 0u;  /* EXT_WRITE */
          st = 0;
          for (volatile int w = 0; w < 100000; w++) { st = o[2]; if (st & 1) break; }
          XnuLog ("[spmi] wr %x=%x st=%x\n", seq[i].off, seq[i].val, st);
          FlushLog ();
          if (!(st & 1) || (st & 0xe)) break;
       }
       /* ACC_ENABLE: Linux's qpnpint_irq_domain_activate writes 1 here before
        * any peripheral writes work. v7: wr_base + 0x100 + 0x1000*apid. */
       {
         volatile UINT32 *acc = (volatile UINT32 *)(0x0c500000 + 0x100 + 0x1000 * (UINT64)apid);
         UINT32 acc_before = *acc;

         *acc = 1;
         __asm__ volatile ("dc cvac, %0; dsb sy" :: "r" (acc) : "memory");
         XnuLog ("[spmi] acc_enable %x: %x -> post-flush %x\n",
                 (UINT64)(UINTN)acc, acc_before, *acc);
         FlushLog ();
       }
       /* Android's live config (from /sys/kernel/debug/gpio, touch on):
        * out high vin-1 pull-down-10uA. Full sequence, driver order, EN last. */
       volatile UINT32 *rw = (volatile UINT32 *)(0x0c500000 + 0x1000 * (UINT64)apid);
        const struct { UINT8 off, val; } seq2[] = {
          { 0x40, 0x01 }, { 0x41, 0x01 }, { 0x42, 0x04 }, { 0x45, 0x01 },
          { 0x44, 0x80 }, { 0x46, 0x80 },
        };
        for (unsigned i = 0; i < sizeof (seq2) / sizeof (seq2[0]); i++) {
          rw[4] = seq2[i].val;                                /* WDATA0 +0x10 */
          rw[0] = (2u << 27) | ((UINT32)seq2[i].off << 4) | 0u; /* EXT_WRITE */
          st = 0;
          for (volatile int w = 0; w < 100000; w++) { st = rw[2]; if (st & 1) break; }
          XnuLog ("[spmi] rw wr %x=%x st=%x\n", seq2[i].off, seq2[i].val, st);
          FlushLog ();
          if (!(st & 1) || (st & 0xe)) break;
        }
        /* routing self-check on gpio4: write VIN=2 via RW, read back via observer */
        if (apid4 >= 0) {
          volatile UINT32 *r4 = (volatile UINT32 *)(0x0c500000 + 0x1000 * (UINT64)apid4);
          volatile UINT32 *o4 = obs + (0x20 * apid4) / 4;
          UINT32 st4 = 0, before = 0, after = 0;

          o4[0] = (13u << 27) | (0x41u << 4) | 0u;
          for (volatile int w = 0; w < 100000; w++) { st4 = o4[2]; if (st4 & 1) break; }
          before = o4[6] & 0xff;

          r4[4] = 0x02;                                          /* WDATA: VIN=2 */
          r4[0] = (2u << 27) | (0x41u << 4) | 0u;                /* EXT_WRITE */
          for (volatile int w = 0; w < 100000; w++) { st4 = r4[2]; if (st4 & 1) break; }

          o4[0] = (13u << 27) | (0x41u << 4) | 0u;
          for (volatile int w = 0; w < 100000; w++) { st4 = o4[2]; if (st4 & 1) break; }
          after = o4[6] & 0xff;
          XnuLog ("[spmi] g4 apid=%d vin %x -> %x (wr st=%x)\n", apid4, before, after, st4);
          FlushLog ();
        }
        /* read back through the RW channel itself and compare with the observer */
        {
          const UINT8 offs2[] = { 0x40, 0x44, 0x46 };
          for (unsigned i = 0; i < 3; i++) {
            rw[0] = (13u << 27) | ((UINT32)offs2[i] << 4) | 0u;   /* EXT_READ */
            st = 0;
            for (volatile int w = 0; w < 100000; w++) { st = rw[2]; if (st & 1) break; }
            XnuLog ("[spmi] rw rd %x=%x st=%x\n", offs2[i], rw[6] & 0xff, st);
            o[0] = (13u << 27) | ((UINT32)offs2[i] << 4) | 0u;
            st = 0;
            for (volatile int w = 0; w < 100000; w++) { st = o[2]; if (st & 1) break; }
            XnuLog ("[spmi] ob rd %x=%x st=%x\n", offs2[i], o[6] & 0xff, st);
            FlushLog ();
          }
        }
        /* full readback: subtype + all 8 GPIO3 config regs */
        {
          const UINT8 offs[] = { 0x04, 0x05, 0x40, 0x41, 0x42, 0x43, 0x44, 0x45, 0x46, 0x47 };
          for (unsigned i = 0; i < sizeof (offs) / sizeof (offs[0]); i++) {
            o[0] = (13u << 27) | ((UINT32)offs[i] << 4) | 0u;
            st = 0;
            for (volatile int w = 0; w < 100000; w++) { st = o[2]; if (st & 1) break; }
            XnuLog ("[spmi] rd %x=%x st=%x\n", offs[i], o[6] & 0xff, st);
          }
        }
        FlushLog ();
      }
    }
  }
  #endif

PayloadMapped = Mapped ((UINT64)XNU_PAYLOAD_BASE);

  if (mLog != NULL) {
    PayloadMapped = Mapped ((UINT64)XNU_PAYLOAD_BASE);

  LogPayload (PayloadMapped);
    FlushLog ();
    LogCpu ();
    FlushLog ();
    LogDisplay ();
    FlushLog ();
    LogMemoryMap ();
    FlushLog ();
    XnuLog ("== END ==\n");
    FlushLog ();
  }

  // Normally does not return
  StartPayloadLoader ();

  // Nothing started: wait where it is safe to leave the phone unattended, then back to Android.
  gBS->Stall (300 * 1000 * 1000);
  gRT->ResetSystem (EfiResetCold, EFI_SUCCESS, 0, NULL);
}
