# AMD BC-250 PSP Windows Driver — Agent Guide

## Build commands (from repo root, in VS2022 x64 Native Tools prompt)

```cmd
scripts\build.bat           # -> output\PspDriver.sys + .inf + .cat + sign
scripts\compile-test.bat    # -> output\test-psp-driver.exe
```

Manual alternative:
```
cl /c /kernel /W3 /Zi /Od /DAMD64 /I"<WDK>\km" /I"<WDK>\km\crt" /I"<WDK>\shared" /Iinc src\driver\PspDriver.c
link /DRIVER /SUBSYSTEM:NATIVE /ENTRY:DriverEntry /OUT:output\PspDriver.sys PspDriver.obj ntoskrnl.lib wdm.lib hal.lib /LIBPATH:"<WDK>\km\x64"
cl /W3 /Zi /O2 /D_AMD64_ /Iinc src\test\test-psp-driver.c /Fe:output\test-psp-driver.exe
```

## Key architecture

- **WDM** (native NT), NOT KMDF (KMDF caused 0x7e). No WDF libs in link step.
- Target: PCI `VEN_1022&DEV_143E` (AMD PSP on BC-250 / PS5 Oberon)
- **BIOS**: Version 5.00 from `BC250_5.00_clv.bin` (all FW extracted from this version)
- MMIO maps **Graphics BAR5** (`0xFE800000`), not PSP BAR0. C2PMSG mailbox lives in BAR5.
- IOCTL: `METHOD_BUFFERED`, device `\Device\AmdBcPsp`, symlink `\DosDevices\AmdBcPsp`
- User-mode: `test-psp-driver.exe` talks via `DeviceIoControl` to `\\.\AmdBcPsp`

## Critical driver behavior

- **FW buffer is persistent**: After `IOCTL_PSP_LOAD_FW`, buffer stays allocated for subsequent `SEND_CMD` calls. Only freed on unload or re-load.
- **INIT_HW maps arbitrary PA**: The user specifies BAR5 physical address + size. The driver also probes for PCI ECAM (`0xE0000000`, `0xF0000000`, `0xC0000000`, `0xE000000000`) and initializes a global 4KB ring buffer PA on first call.
- **Spinlock on SEND_CMD**: `CommandLock` protects mailbox command sequence (C2PMSG_36 write → C2PMSG_35 write → poll C2PMSG_81).
- **GRBM_STATUS = 0xFFFFFFFF**: Hardware-level PS5 NBIO restriction on GRBM/CP range (0x2000-0x2FFF). Not a driver bug.
- **NBIO_VIA_RING (IOCTL 0x807)**: Writes `0x00020000` to C2PMSG_64 (bypasses ring entirely), writes NBIO sigs directly to BAR5, returns `[cmd, c64resp, mmhub]`. Does NOT check `RingCreated`.

## C2PMSG Mailbox Protocol

Critical protocol flow for firmware loading on BC-250 PSP:

```
1. WRITE_REGISTER_ULONG(C2PMSG_81, 0)     # Clear previous response (CRITICAL!)
2. WRITE_REGISTER_ULONG(C2PMSG_36, paLo)  # Full physical address (low 32 bits)
3. WRITE_REGISTER_ULONG(C2PMSG_35, cmd)   # Command (0x4 = SYSDRV, 0x8 = SOS)
4. Poll C2PMSG_81 until != initial value  # Wait for PSP response
5. Decode response: bit 31 = done, bits[27:0] = status (0 = success)
6. WRITE_REGISTER_ULONG(C2PMSG_35, 0)     # ACK (do NOT clear C2PMSG_81!)
7. IF original C2PMSG_81 was SOS alive (0xF0000010) AND status === success:
   RESTORE original value to C2PMSG_81   # Preserve SOS alive flag for GPU driver
```

### PSP error codes
| C2PMSG_81 | Meaning |
|-----------|---------|
| `0x00000000` | Idle / success |
| `0xF0000010` | Firmware validation failed (wrong type, bad signature) |
| `0xF0000020` | Unknown command / timeout |

If C2PMSG_81 is stuck at `0xF0000010`, the PSP ignores new commands until cleared.

### NBIO unlock state (verified on HW)
| Range | Offset example | Value | Status |
|-------|---------------|-------|--------|
| GC | 0x3000 | `0x009A0C00` | Unlocked |
| MMHUB | 0x5000 | `0x80840000` | Unlocked |
| HDP | 0x05A0 | `0x00070000` | Unlocked |
| NBIO SIG1 | 0xC100 | `0xFEDCBAEF` | Written |
| NBIO SIG2 | 0xC180 | `0xFEDCBADF` | Written |
| GRBM/CP | 0x2000 | `0xFFFFFFFF` | HW blocked (PS5 Oberon) |

GPU driver conflict: The main GPU driver shares BAR5 with PSP. Running MMIO tests with the GPU driver active may cause black screen. Unload GPU driver or use Microsoft Basic Display Driver.

## C2PMSG Mailbox register offsets (BAR5-relative)

| Register | Offset | Purpose |
|----------|--------|---------|
| C2PMSG_35 | 0x1056C | Command register |
| C2PMSG_36 | 0x10570 | Data register (PA low 32b) |
| C2PMSG_37 | 0x10574 | Data register (PA high 32b) |
| C2PMSG_64 | 0x105E0 | Ring cmd/resp (psp_gfx_ctrl.cmd_resp) |
| C2PMSG_65 | 0x105E4 | RBI ring wptr (rbi_wptr) |
| C2PMSG_66 | 0x105E8 | RBI ring rptr (rbi_rptr) |
| C2PMSG_67 | 0x105EC | GPCOM ring wptr (gpcom_wptr) |
| C2PMSG_68 | 0x105F0 | GPCOM ring rptr (gpcom_rptr) |
| C2PMSG_69 | 0x105F4 | Ring buffer addr lo |
| C2PMSG_70 | 0x105F8 | Ring buffer addr hi |
| C2PMSG_71 | 0x105FC | Ring buffer size |
| C2PMSG_81 | 0x10614 | Status/response register |

## PSP Ring Buffer Protocol (psp_v11_0_8 for cyan_skillfish)

Ring creation (from Linux `psp_v11_0_8_ring_create`):
```
1. WRITE C2PMSG_69 = ring_pa_lo
2. WRITE C2PMSG_70 = ring_pa_hi
3. WRITE C2PMSG_71 = ring_size
4. WRITE C2PMSG_64 = 0x00020000  (GPCOM/KM ring = ring_type << 16)
5. Poll C2PMSG_64 bit 31 (GFX_FLAG_RESPONSE) until set
```

Ring frame structure (64 bytes, `psp_gfx_rb_frame`):
```
+0:  cmd_buf_addr_lo   (GPU address of command buffer)
+4:  cmd_buf_addr_hi
+8:  cmd_buf_size       (1024 bytes for cmd buffer)
+12: fence_addr_lo       (optional, 0 if unused)
+16: fence_addr_hi       (optional, 0 if unused)
+20: fence_value         (optional, 0 if unused)
```

Command buffer structure (1024 bytes, `psp_gfx_cmd_resp`):
```
+0:  buf_size           (1024)
+4:  buf_version         (1)
+8:  cmd_id             (GFX_CMD_ID_LOAD_IP_FW = 0x06)
+28: cmd_load_ip_fw: fw_addr_lo, fw_addr_hi, fw_size, fw_type
+864: resp.status       (PSP writes response here)
```

Ring submission:
```
1. Build command buffer with firmware params
2. Build ring frame pointing to command buffer
3. Write frame to ring buffer at wptr offset
4. WRITE C2PMSG_67 = new wptr value         (gpcom_wptr)
5. Poll C2PMSG_64 bit 31 (response ready)
6. Check cmd buffer +864 for resp.status (0 = success)
```

GPU FW type codes (from Linux `psp_gfx_if.h`):
```
FW type           Value
CP_ME             1
CP_PFP            2
CP_CE             3
CP_MEC            4
CP_MEC1  (MEC2)   5
RLC_G             8
SDMA0             9
SDMA1             10
```

Ring commands (from Linux `psp_gfx_if.h`):
```
GFX_CMD_ID_LOAD_IP_FW    0x06  (load GPU IP firmware)
GFX_CMD_ID_LOAD_TOC      0x20  (load TOC + get TMR size)
GFX_CMD_ID_AUTOLOAD_RLC  0x21  (start RLC autoload after all FW loaded)
```

## Test sequence (after driver install + reboot)

```
test-psp-driver.exe -i 0xFE800000 0x200000    # Init BAR5 MMIO (required first)
test-psp-driver.exe -t                         # Connectivity test (C2PMSG_35/36/81)
test-psp-driver.exe -f cyan_skillfish2_sos_extracted.bin   # Load FW
test-psp-driver.exe -C 0x4                     # SYSDRV command
test-psp-driver.exe -C 0x8                     # SOS command
test-psp-driver.exe -R                         # Create PSP ring (C2PMSG_69/70/71/64)
test-psp-driver.exe -U                         # NBIO unlock via C2PMSG_64 + sigs
test-psp-driver.exe -s                         # Full status snapshot
test-psp-driver.exe -B                         # Boot seq: embedded FW + SYSDRV + SOS + GRBM
test-psp-driver.exe -A                         # Load ALL GPU FW via PSP ring (cyan_skillfish2, 8 files)
test-psp-driver.exe -L <type> <file.bin>       # Load single GPU FW via PSP ring
```

Full GPU bring-up sequence (from output/ dir):
```
test-psp-driver.exe -i 0xFE800000 0x200000     # Init HW
test-psp-driver.exe -B                          # Boot: SYSDRV + SOS
test-psp-driver.exe -R                          # Create GPCOM ring
test-psp-driver.exe -A                          # Load all 8 GPU FW components
```

Error 21 (`ERROR_NOT_READY`) from any IOCTL means `INIT_HW` not called yet.

## Source files

| File | Purpose |
|------|---------|
| `src/driver/PspDriver.c` | Single-file WDM driver (~830 lines). All IOCTL dispatch here. |
| `src/test/test-psp-driver.c` | User-mode test tool (~570 lines). All IOCTL wrappers + CLI args. |
| `inc/PspIoctl.h` | Shared IOCTL codes + struct definitions. |
| `inc/firmware_data.h` | 32KB embedded firmware arrays (binary, do not edit by hand). Generated by `scripts/gen-firmware-h.ps1`. |
| `inf/PspDriver.inf` | Device installation. Matches `PCI\VEN_1022&DEV_143E&SUBSYS_00001022&REV_00`. |

## CRITICAL: firmware_data.h blob naming

The arrays in `firmware_data.h` are correctly named for this BIOS (5.00) $PSP table:

| Array name | File source | PSP type | Correct command |
|------------|-------------|----------|-----------------|
| `g_SosFirmwareData` | From BIOS 0x8E0400 | **Type 1 (SOS)** | **CMD 0x8** |
| `g_SysdrvFirmwareData` | From BIOS 0x8FEE00 | **Type 8 (SYSDRV)** | **CMD 0x4** |

Verified against the user's BIOS dump. If regenerating `firmware_data.h` from a different BIOS, verify the $PSP table type assignments before generating.

## GPU Driver Integration API

### Initialization sequence
```
1. PSP_INIT_HW     → {0xFE800000, 0x200000}     // Map BAR5
2. PSP_BOOT_SEQ    → NULL                        // Load FW + CMD 0x4 + CMD 0x8
3. PSP_NBIO_UNLOCK → NULL                        // Write NBIO sigs
4. PSP_GET_GPU_INFO → PSP_GPU_INFO               // Check C2PMSG_81, get TMR/ring info
5. Verify info.C2pmsg81 == 0xF0000010            // SOS alive
```

### Accessible MMIO ranges
| Range | Example offset | Status |
|-------|---------------|--------|
| GC | 0x3000 | Unlocked (READ_REG/WRITE_REG ok) |
| MMHUB | 0x5000-0x50D0 | Unlocked |
| HDP | 0x05A0 | Unlocked |
| NBIO SIG | 0xC100, 0xC180 | Writable |
| **GRBM/CP** | **0x2000-0x2FFF** | **HW BLOCKED (0xFFFFFFFF)** |

### Limitations
- GRBM/CP registers return 0xFFFFFFFF — PS5 Oberon hardware block
- PSP GPCOM ring not supported by BIOS SOS (C2PMSG_64 always 0)
- Sos alive indicator: C2PMSG_81 = 0xF0000010

### Useful IOCTLs for GPU driver
| IOCTL | Code | Use |
|-------|------|-----|
| PSP_INIT_HW | 0x803 | One-time MMIO mapping |
| PSP_BOOT_SEQUENCE | 0x810 | Automated FW boot |
| PSP_NBIO_UNLOCK | 0x804 | NBIO sig registers |
| PSP_GET_GPU_INFO | 0x815 | Bridge status snapshot |
| PSP_READ_REG | 0x800 | Read unlocked registers |
| PSP_WRITE_REG | 0x801 | Write unlocked registers |
| PSP_SEND_CMD | 0x805 | Raw mailbox command |

- **Secure Boot must be OFF** in BIOS. Test signing is blocked if Secure Boot is on.
- **Driver Code 0x7e**: Linking with WDF libraries (KMDF) instead of WDM. Do not add `wdf` libs.
- **Code 52**: Unsigned driver. Sign with `AMD-BC250-Signer` cert (from sibling project) or enable Test Mode.
- **No unit tests**: Only manual hardware-in-the-loop testing. No CI.
- **bcdedit /set testsigning on + reboot** required before driver loads.
