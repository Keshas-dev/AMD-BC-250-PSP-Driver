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
0. Clear C2PMSG_81 (stale error may block)
0. Poll C2PMSG_64 bit 31 (MBOX_TOS_READY_FLAG) until set  ← **CRITICAL**
1. WRITE C2PMSG_69 = ring_pa_lo
2. WRITE C2PMSG_70 = ring_pa_hi
3. WRITE C2PMSG_71 = ring_size
4. WRITE C2PMSG_64 = 0x00020000  (GPCOM/KM ring = ring_type << 16)
5. Poll C2PMSG_64 bit 31 (MBOX_TOS_RESP_FLAG) until set
```

Timeout: 60s for step 0 (TOS_READY), 3s for step 5 (ring response). Both poll at 1ms intervals.
**Current HW status: step 0 always times out — C2PMSG_64 bit 31 never sets, even after SOS boot + 60s wait.**

### C2PMSG_81 save/restore
- `PspSendMailboxCmd()` saves original C2PMSG_81 before each command
- After command, if original was non-zero (e.g. 0xF0000010) AND success: restores original
- This preserves SOS state across mailbox commands

### PSP error codes (confirmed by testing)
| C2PMSG_81 | Meaning |
|-----------|---------|
| `0x00000000` | Idle / success |
| `0xF0000010` | Firmware validation failed (wrong type, bad signature) |
| `0xF0000020` | Unknown command / timeout |

**Note**: `0xF0000010` was previously thought to be "SOS alive". Testing shows this is actually a **stale error** from the BIOS bootloader's failed SOS attempt. After a successful `-B` (CMD 0x4+0x8), C2PMSG_81 = 0x00000000. No C2PMSG value indicates "SOS alive" — SOS aliveness is detected through C2PMSG_64 bit 31 (which never sets on this HW).

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
test-psp-driver.exe -R                         # Create PSP ring (wait for TOS_READY + C2PMSG_69/70/71/64)
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
| `src/driver/PspDriver.c` | Single-file WDM driver (~1250 lines). All IOCTL dispatch here. |
| `src/test/test-psp-driver.c` | User-mode test tool (~570 lines). All IOCTL wrappers + CLI args. |
| `inc/PspIoctl.h` | Shared IOCTL codes + struct definitions. |
| `inc/firmware_data.h` | 32KB embedded firmware arrays (binary, do not edit by hand). Generated by `scripts/gen-firmware-h.ps1`. |
| `inf/PspDriver.inf` | Device installation. Matches `PCI\VEN_1022&DEV_143E&SUBSYS_00001022&REV_00`. |

## BIOS v3 vs v5 differences

**Correction (2026-06-10)**: `Robin5.00.bin` is actually **v5.00**, NOT v3. The real v3 BIOS is `BC250_3.00.ROM` (extracted from the board's original SPI flash). The $PSP table structure is **identical** between v3 and v5:

| Feature | v3 (`BC250_3.00.ROM`) | v5 (`BC250_5.00_clv.bin`) |
|---------|----------------------|---------------------------|
| SOS (type 1) offset | 0x8E0400 | 0x8E0400 |
| SOS (type 1) size | 43008 (42KB) | 43008 (42KB) |
| SYSDRV (type 8) offset | 0x8FF000 | 0x8FF000 |
| SYSDRV (type 8) size | 262656 (256KB) | 262656 (256KB) |
| Extra PSP entries | type=18/32/40/49/51/68/79/81 | type=18/32/40/49/51/68/79/81 |
| **SOS binary (42KB)** | **Different** | **40530/43008 bytes differ (94%)** |
| **SYSDRV binary (256KB)** | **Different** | **159320/262656 bytes differ (61%)** |

v3 `BC250_3.00_CHIPSETMENU.ROM` variant (patched for Linux chipset menu): **PSP firmware is identical** to original v3 — only UEFI/BIOS differs.

**Hopeful**: Since SOS is fundamentally DIFFERENT between v3 and v5, the v3 SOS may support GPCOM ring protocol where v5 did not. This is the primary motivation for testing with v3 BIOS.

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

## SomnacinDumper-CPUCoreMod findings (bc250-collective)

Repo: https://github.com/bc250-collective/SomnacinDumper-CPUCoreMod
- **BC-250 codename: "ROBIN"** (`#ifdef ROBIN` in all firmware)
- **SMU firmware SPI address**: `0x8FEE00` (matches our BIOS SYSDRV type 8!)
  - Loader: offset `0x1B340` → absolute `0x8FF240`
  - C-payload: offset `0x3AE00` → absolute `0x8F8D00`
- **SMU/PSP Xtensa MMIO registers** (accessible from PSP via BAR5):
  - `0x115A81C` — CPU core config (0x13=8 cores BC-250, 0x12=6 cores 4700S)
  - `0x115A870` — CPU core mask (0xFF=8, 0x77=6)
  - `0x3220000` — TLB entries (custom TLB for core init)
  - `0x0154002C` — SMU NBIF/IOMMU page table config
  - `0x0154001C` — SMU NBIF control
  - `0x015400F0` — SMU command register
  - `0x015400F4` — SMU argument register
  - `0x015400F8` — SMU trigger register (write 1 to execute)
  - `0x0170xxxx–0x0186xxxx` — CCX (CPU Complex) core init per core
  - `0x0145A820` — Final init trigger after all cores configured
- **SPI flash layout**: ABL0 at `0x962D00` (size 0x440), SecureOS at `0x99E200`
- **x86 BIOS patches**: at `0xE02000+0x83B00` and `0xE02000+0x1FD270`
- **Hardware**: Pi Pico 2 (PIO state machines) between mainboard and SPI flash
- **BC-250 upcore FAILS**: BIOS stuck at x86 core init (only 1 unit tested)
- **SMU command interface at `0x015400F0/F4/F8`** can be used from our PSP driver for SMU commands

## Linux psp_v11_0_8.c analysis

Source: https://raw.githubusercontent.com/torvalds/linux/master/drivers/gpu/drm/amd/amdgpu/psp_v11_0_8.c

Key differences from our Windows driver:

### 1. No microcode/firmware loading at all
- `psp_v11_0_8_funcs` struct only has: `ring_create`, `ring_stop`, `ring_destroy`, `ring_get_wptr`, `ring_set_wptr`
- No `init_microcode`, `bootloader_load_sysdrv`, `bootloader_load_sos`, `cmd_submit`, `tmr_init`
- SOS is pre-loaded by BIOS bootloader from SPI flash (type 1 in $PSP table)
- Our mailbox CMD 0x4/0x8 approach is WRONG for this device

### 2. Ring create waits for MBOX_TOS_READY_FLAG first
```c
/* Wait for sOS ready for ring creation */
ret = psp_wait_for(psp, C2PMSG_64, MBOX_TOS_READY_FLAG, MBOX_TOS_READY_MASK, 0);
if (ret) {
    DRM_ERROR("Failed to wait for trust OS ready for ring creation\n");
    return ret;
}
/* Then: */
WREG32(C2PMSG_69, ring_addr_lo);
WREG32(C2PMSG_70, ring_addr_hi);
WREG32(C2PMSG_71, ring_size);
WREG32(C2PMSG_64, ring_type << 16);
/* Wait for response bit 31 */
psp_wait_for(C2PMSG_64, MBOX_TOS_RESP_FLAG, ...);
```
- `MBOX_TOS_READY_FLAG/MASK` = `0x80000000` (bit 31)
- `MBOX_TOS_RESP_FLAG/MASK` = `0x80000000` (bit 31)
- Uses `psp_wait_for` with timeout = `adev->usec_timeout` (likely ~3 seconds)

### 3. SOS readiness (C2PMSG_64 bit 31) must come BEFORE ring create
- Linux dmesg shows PSP init success: TMR reserved at 0xF40F800000, SMU init OK
- On Linux, C2PMSG_64 has bit 31 set by the time amdgpu loads (~5.5s after boot)
- On our hardware, C2PMSG_64 = 0 when we test (possibly too early, or init interferes)

### 4. psp_v11_0.c (full devices) vs psp_v11_0_8.c (cyan_skillfish2)
| Feature | psp_v11_0.c (Navi10 etc.) | psp_v11_0_8.c (BC-250) |
|---------|--------------------------|------------------------|
| Load SOS firmware | Yes (from filesystem) | **No** (pre-loaded by BIOS) |
| Load SYSDRV | Yes (mailbox CMD 0x4) | **No** |
| Wait for bootloader | C2PMSG_35 bit 31 | **Not used** |
| SOS alive check | C2PMSG_81 != 0 | **Not used** |
| Ring create before TMR | Yes | Yes |
| C2PMSG_64 ready wait | Yes (MBOX_TOS_READY) | Yes (same) |
| APU codename | - | **CYAN_SKILLFISH2** (`AMD_APU_IS_CYAN_SKILLFISH2`) |

### 5. amdgpu_psp.c psp_early_init for 11.0.8
```c
case IP_VERSION(11, 0, 8):
    if (adev->apu_flags & AMD_APU_IS_CYAN_SKILLFISH2)
        psp_v11_0_8_set_psp_funcs(psp);
    psp->autoload_supported = false;
    psp->boot_time_tmr = false;
    break;
```

### 6. Ring create fixed (applied to PspDriver.c)
- `PspCreateRing()` now waits for C2PMSG_64 bit 31 (MBOX_TOS_READY_FLAG) before writing ring registers
- Before polling, clears C2PMSG_81 (stale 0xF0000010 blocks ring)
- Timeout: 60 seconds (increased from 10s), poll every 1ms. On timeout returns `STATUS_DEVICE_NOT_READY` (STATUS_TIMEOUT has success severity → DeviceIoControl returned TRUE with 0 bytes)
- After writing ring regs, waits for response bit 31 with 3s timeout
- Mailbox CMD 0x4/0x8 path kept for backward compatibility
- **C2PMSG_64 = 0 even after 60s wait on our hardware — TOS_READY never sets**

### 7. C2PMSG_81 save/restore applied
- `PspSendMailboxCmd()` saves original C2PMSG_81 before each command
- After command, if original was non-zero (e.g. 0xF0000010) AND success: restores original
- This preserves SOS state across mailbox commands

### 8. Code review fixes applied this session
- **Duplicate `-T` flag** in test-psp-driver.c (ComprehensiveProbe was unreachable behind TriggerAutoloadRlc) → changed to `-H`
- **Race condition** on `ringWriteOffset` in all 4 ring paths (IOCTL_RING_LOAD_FW, IOCTL_REG_PROG, IOCTL_AUTOLOAD_RLC, PspInitTmr) → protected with `devExt->CommandLock` spinlock
- **Physical address validation** in INIT_HW → reject PA=0, size=0, size>4MB
- **STATUS_TIMEOUT → STATUS_DEVICE_NOT_READY** in ring create (STATUS_TIMEOUT = 0x00000102 = success severity → `NT_SUCCESS(status)` = TRUE)

### 9. GitHub code review by agent
- **Real bugs found**: #6 duplicate -T flag, #2 ringWriteOffset race, #10 PA validation (all fixed above)
- **False positives** (#1, #3, #4, #5, #7, #8, #9): already fixed in earlier commits or not applicable
- All false positives were correctly identified by looking at the codebase before reading latest source

## Session results (2026-06-10): Fundamental HW blockers identified

### Confirmed: GPCOM ring protocol NOT supported by this SOS
- C2PMSG_64 = 0x00000000 always — bit 31 (MBOX_TOS_READY) NEVER sets, even after 60s poll
- C2PMSG_69/70/71 (ring addr/size) writes are **silently rejected** (read back returns 0)
- C2PMSG_65/66/67/68 (RBI/GPCOM wptr/rptr) = 0 always
- TMR init (`-M`) also fails because it uses ring protocol internally
- **Linux cyan_skillfish2**: amdgpu sends NO mailbox commands, just calls `psp_ring_create()` directly → waits for C2PMSG_64 bit 31 with 20ms timeout. On our HW, this step would also fail.

### 0xF0000010 reinterpretation
- `0xF0000010` is **NOT** "SOS alive" — it's "Firmware validation failed (wrong type, bad signature)"
- This is a stale error from the BIOS bootloader's attempt to boot SOS
- After our `-B` sequence loads SOS fresh, C2PMSG_81 = 0x00000000 (idle/success), not 0xF0000010
- The BIOS-loaded SOS FAILED (0xF0000010), our mailbox-loaded SOS SUCCEEDS (0x00000000)
- **Neither variant sets C2PMSG_64 bit 31** — the SOS on this BIOS version simply doesn't support rings

### SMN access: read-only from NB PCI config
- SMN read works through B0D0F0+0xB8 (addr) / 0xBC (data)
- SMN write **does NOT work** through this method — writing to 0xBC has no effect
- SMU registers at SMN 0x0154xxxx (from SomnacinDumper) are accessible R/O but not writable from host
- To write SMN, need PSP firmware (PSP has internal SMN write access) or MSRs (0xC0011030/0xC0011031)
- **SMU commands cannot be sent directly from the x86 host driver**

### Interesting SOS memory (BAR5 registers 0x10630-0x10700)
| Offset | Value | Notes |
|--------|-------|-------|
| 0x10638 | 0x47474747 | "GGGG" — init pattern |
| 0x1063C | 0x00000008 | size/version |
| 0x10650-58 | 0x0001F000 | 0x1F000 = 126976 (~124KB) |
| 0x10674 | 0xFFAA5500 | magic/signature |
| 0x1067C | 0x45454545 | "EEEE" — init pattern |
| 0x10680 | 0x00000084 | 132 |
| 0x1068C | 0x00003C00 | 15360 |
| 0x106A4 | 0x426F306D | **"Bo0m"** — custom SOS signature |
| 0x106AC | 0x012C1B34 | address/version |
| 0x106B0 | 0x00000001 | 1 |
| 0x106BC | 0x01110111 | repeated pattern |
| 0x106C4-E8 | 0x01000000 | repeated |
| 0x106F0 | 0x00000200 | 512 |
| 0x106F4 | 0x00000100 | 256 |
| 0x10700 | 0x00700A70 | version/date? |

The "Bo0m" string at 0x106A4 is unique to this SOS — not standard AMD firmware.

### PCI config: Host bridge (B0D0F0) = 0x13E0
- Vendor/Device: `0x1022:0x13E0` — AMD Family 17h (Zen 2) Host bridge
- Matches PS5 Oberon / BC-250 SOC (Zen 2 based)
- Functions F1-F7 also present with shifted Vendor/Device IDs

### Known-working vs blocked
| Feature | Status |
|---------|--------|
| Mailbox CMD 0x4/0x8 (SYSDRV/SOS) | ✅ Works |
| NBIO unlock (sigs + GC/MMHUB/HDP) | ✅ Works |
| MMIO reads/writes unlocked range | ✅ Works |
| PCI config reads | ✅ Works |
| SMN reads (R/O via B0D0F0) | ✅ Works |
| **GPCOM ring (C2PMSG_64/67/69/70/71)** | **❌ HW not supported** |
| **RBI ring (C2PMSG_65/66)** | **❌ HW not supported** |
| **TMR init (needs ring)** | **❌ Blocked** |
| **GPU FW loading (needs ring)** | **❌ Blocked** |
| **GRBM/CP registers (0x2000-0x2FFF)** | **❌ HW blocked (0xFFFFFFFF)** |
| **SMN writes** | **❌ Host blocked (R/O only)** |
| **SMU commands** | **❌ Need PSP or MSR access** |

### Next steps (user-driven)
1. User has found real **v3 BIOS** (`BC250_3.00.ROM` + `BC250_3.00_CHIPSETMENU.ROM`). $PSP table is identical to v5 but SOS is 94% DIFFERENT — the v3 SOS may support GPCOM ring protocol where v5 SOS did not.
2. User is analyzing BIOS v3 firmware for differences (confirmed: SOS 40530/43008 bytes differ, SYSDRV 159320/262656 bytes differ)
3. Future: try BIOS v3 on hardware and test if C2PMSG_64 bit 31 (TOS_READY) ever sets with the v3 SOS
4. If v3 SOS supports rings: regenerate `firmware_data.h` from v3 blobs, re-test ring/TMR/GPU FW loading
5. If ring protocol still fails on v3: investigate alternative approaches (mailbox-based FW loading, PSP-side SMU commands)

## Agent setup (TODO)

User requested setting up multiple agents to work in parallel:
1. **Code reviewer agent** — analyze driver/test code, suggest fixes, find bugs
2. **Research agent** — search internet for useful info (PSP protocol, AMD NBIO, BC-250 docs)
3. **Architecture agent** — suggest alternative approaches, spot design issues

Use `task` tool with `general` subagent type for these roles.
Reminder: check `E:\` drive is connected before build commands (user sometimes forgets).
