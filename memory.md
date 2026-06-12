# BC-250 PSP Windows Driver — Memory & Discoveries

## Project Status (2026-06-12)

### What Works
- **Driver**: Compiled, linked, signed (test cert), installs successfully
- **INIT_HW** (BAR5 mapping): Maps `0xFE800000` (Graphics BAR5, not PSP BAR0)
- **Mailbox commands**: CMD 0x4 (SYSDRV) and CMD 0x8 (SOS) work
- **NBIO unlock**: Signature registers at 0xC100/0xC180 write OK
- **Register reads**: GC (0x3000), MMHUB (0x5000), HDP (0x05A0) return valid values
- **Boot sequence** (`-B`): FW load + SYSDRV + SOS all succeed
- **C2PMSG_81**: Reports 0xF0000010 (stale bootloader error) before boot, 0x00000000 after

### What Does NOT Work
- **Ring protocol**: C2PMSG_64 bit 31 (MBOX_TOS_READY) never sets. SOS firmware (both v3 and v5) does NOT support rings
- **TMR init**: Requires ring → fails
- **GPU FW loading via ring**: Blocked by missing ring support
- **GRBM/CP at 0x2004**: Returns 0xFFFFFFFF (BLOCKED / wrong offset)

### Critical: GC_BASE Offset Not Applied
The **single most important discovery**: BC-250 uses a non-standard register map where GC registers are shifted by **GC_BASE = 0x1260** bytes in BAR5 space. This comes from Linux `cyan_skillfish_ip_offset.h`:

```
GC_BASE__INST0_SEG0 = 0x00001260
```

The driver code **still reads GRBM at 0x2004** (Navi10 offset). It should read at `0x1260 + 0x2004 = 0x3264` (or `0x1260 + 0x2000 = 0x3260` for GRBM_STATUS).

**This has NOT been verified and NOT been implemented in code.** The AGENTS.md documents the theory but the code was never updated.

### Suspected Corrected Register Map

| Register | Navi10 Offset | BC-250 Offset (theory) |
|----------|--------------|----------------------|
| GRBM_STATUS | 0x2000 | 0x3260 |
| CC_GC_SHADER_ARRAY_CONFIG | 0x2004 | 0x3264 |
| SPI_PG_ENABLE_STATIC_WGP_MASK | 0x229C | 0x34FC |
| RLC_PG_ALWAYS_ON_WGP_MASK | 0x2B04 | 0x3D64 |
| CP scratch | 0x2074 | 0x32D4 |
| SDMA | 0x2600 | 0x3860 |

**Must test:**
```cmd
test-psp-driver.exe -r 0x3260
test-psp-driver.exe -r 0x3264
test-psp-driver.exe -r 0x34FC
```

### UEFI Variables (BIOS Setup)

From `setup-menu-5-clv.sct.0.0.en-US.uefi.ifr.txt` (extracted IFR):

**Setup variable**: GUID `{EC87D643-EBA4-4BB5-A1E5-3F3E36B20DA9}`, size 0x1D3 (467 bytes)

| Setting | Offset | Values |
|---------|--------|--------|
| IOMMU | 0xD2 (210) | 0=Disabled, 1=Enabled |
| SVM Mode (Virtualization) | 0xB0 (176) | 0=Disabled, 1=Enabled |
| Above 4G Decoding | 0xDA (218) | 0=Disabled, 1=Enabled |
| SR-IOV Support | 0xDB (219) | 0=Disabled, 1=Enabled |
| Memory Clock | 0xD7 (215) | 0=Auto, 1-5=speeds |
| Bank Interleaving | 0xD5 (213) | 0=Disabled, 1=Enabled |
| Channel Interleaving | 0xD6 (214) | 0=Disabled, 1=Enabled |
| Memory Clear | 0xD8 (216) | 0=Disabled, 1=Enabled |
| GFX Fix Voltage | 0x1CD (461) | 0=Disabled, 1=Enabled |
| GFX Loadline | 0x1D0 (464) | 0=Disabled, 1-5=Ohm |
| VCORE Fix Voltage | 0x1C9 (457) | 0=Disabled, 1=Enabled |
| VCORE Loadline | 0x1CC (460) | 0-5 |

**Access from Windows**: These variables are `EFI_VARIABLE_BOOTSERVICE_ACCESS` — NOT readable from Windows via `GetFirmwareEnvironmentVariable`. Must use RU.efi at UEFI shell.

**RU.efi workflow**: Boot from USB → RU.efi → Alt+= → UEFI Variables → find `Setup` (EC87D643...) → edit hex at offset 0xD2 → change IOMMU from 01 to 00 → save → reboot.

### GFX Configuration (from SCT)
- `setup-menu-5-clv.sct` has GFX Configuration form at FormId 0x285B
- **The form is EMPTY** — contains only subtitles, no actual settings. GFX/NBIO advanced options are hidden/suppressed
- `setup-menu-modifikuotas.sct` (modified version) has VDDC_GFX, GFX Fix Voltage, GFX Loadline in main menu
- North Bridge form (0x285C) is also empty (no NBIO unlock toggle exists as a BIOS option)

### PSP Architecture
- **BAR5 (Graphics BAR, 0xFE800000)**: Main MMIO interface to PSP
- **C2PMSG registers at 0x1056C-0x10614**: Mailbox protocol
- **No ring protocol support** in SOS firmware → can't use GPCOM/RBI command rings
- **PSP CSHA (hardware crypto)**: From Linux psp_v11_0_8, possible alternative for TMR setup
- **SMN access via PCI config**: B0D0F0 + 0xB8 (addr) / 0xBC (data) — read-only from host

### BIOS Firmware Analysis
- **BC-250**: Zen 2 (Family 17h) based SOC, PCI Host Bridge 0x1022:0x13E0
- **Codename**: "ROBIN" (from firmware ifdefs)
- **PSP table at 0x8E0000**: Format = `$PSP` magic + type(4)+size(4)+baseOffset(4)+reserved(4)
- **v5 SOS size**: **46592 bytes** (0xB600) — NOT 43008 as initially assumed
- **v3 SOS**: 43008 bytes, **differs from v5** by ~94%
- **SYSDRV**: Same size (262656 bytes) in both, ~61% different

### Build System
- **build.bat**: WDM (not KMDF!), targets PCI `VEN_1022&DEV_143E`
- **Toolchain**: VS2022 Community on `E:` drive, WDK 10.0.26100.0
- **Linking**: `/DRIVER /SUBSYSTEM:NATIVE /ENTRY:DriverEntry`, `/NODEFAULTLIB`, `/GS-`
- **Signing**: Test certificate created via PowerShell `New-SelfSignedCertificate`
- **Test mode required**: `bcdedit /set testsigning on` + reboot

### Known Driver Bugs (ALL FIXED)
1. C2PMSG_81 save/restore → remove save/restore, poll C2PMSG_35 instead
2. Spinlock held during polling → release before poll, re-acquire for writes
3. Missing C2PMSG_37 write (high 32 bits) → write high PA to C2PMSG_37
4. Shared g_CmdBuffer race → per-call ExAllocatePool2
5. LOAD_EMBEDDED_FW wrong size → use g_SosFirmwareSize not g_SysdrvFirmwareSize
6. BOOT_SEQUENCE always STATUS_SUCCESS → return real NTSTATUS
7. Test tool LoadFirmware reads wrong struct → read single ULONG
8. REG_PROG write-only → add IsRead flag for direct MMIO read

### Test IOCTLs
| Flag | IOCTL | Use |
|------|-------|-----|
| `-i <pa> <size>` | `IOCTL_PSP_INIT_HW` (0x803) | Map BAR5 (required first) |
| `-t` | Connectivity test | Read C2PMSG_35/36/81 |
| `-B` | `IOCTL_PSP_BOOT_SEQUENCE` (0x810) | Full autoboot |
| `-s` | `IOCTL_PSP_STATUS` (0x809) | Status snapshot |
| `-H` | `IOCTL_PSP_HW_PROBE` (0x814) | Comprehensive probe |
| `-r <off>` | `IOCTL_PSP_READ_REG` (0x800) | Read MMIO register |
| `-w <off> <val>` | `IOCTL_PSP_WRITE_REG` (0x801) | Write MMIO register |
| `-R` | Ring create (FAILS — HW) |
| `-M` | TMR init (FAILS — needs ring) |

### Next Steps (Suggested)
1. **Verify GC_BASE offsets**: Test 0x3260, 0x3264, 0x34FC via `-r`
2. **If confirmed**: Add `PSP_GC_BASE 0x1260` to `PspIoctl.h`, update all GRBM reads
3. **BIOS reconfig via RU.efi**: Disable IOMMU (offset 0xD2 → 00), enable Above 4G (0xDA → 01)
4. **Test GRBM after reconfig**: `-r 0x3260` should return valid status bits
5. **Try CSHA commands** as ring alternative for TMR
6. **Extend driver**: If NBIO/GRBM unlock confirmed, add 40 CU unlock (write 0xFFE00000 to 0x3264, 0x1F to 0x34FC)
7. **v3 BIOS test**: Flash v3 if v5 SOS continues to block features

### Files
| File | Location |
|------|----------|
| Driver source | `src/driver/PspDriver.c` (~1305 lines) |
| Test tool | `src/test/test-psp-driver.c` (~813 lines) |
| IOCTL definitions | `inc/PspIoctl.h` |
| FW blob data | `inc/firmware_data.h` (generated, do not hand-edit) |
| INF | `inf/PspDriver.inf` |
| Build script | `scripts/build.bat` |
| BIOS v5 | `Firmware/BIOS/BC250_5.00_clv.bin` (16MB) |
| BIOS v3 original | `Firmware/BIOS/BC250_3.00.ROM` (16MB) |
| BIOS v3 chipset menu | `Firmware/BIOS/BC250_3.00_CHIPSETMENU.ROM` (16MB) |
| IFR extracted | `Firmware/BIOS/setup-menu-5-clv.sct.0.0.en-US.uefi.ifr.txt` |
| SCT files | `Firmware/BIOS/*.sct` |
| RU.efi + RU.exe | `Firmware/BIOS/` |
| GUI tool | `output/BC250UefiGui.exe` (C# WinForms) |
| Memory log | `memory.md` (this file) |
