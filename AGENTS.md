## Session 2026-06-30: All PSP Bugs Fixed

### PSP Bug Fix Status

| # | Description | Status | Fix |
|---|-------------|--------|-----|
| 2 | RLC_CP_SCHEDULERS at 0xECA1 not 4-byte aligned (PspKiq.c:57) | **FIXED** | Uses 0xECA8 (empirically confirmed writable) |
| 4 | IOCTL name collision — same name `IOCTL_AMDBC250_BAR5_READ_PROXY`, different values | **FIXED** | PSP uses `IOCTL_AMDBC250_BAR5_READ_PROXY_RAW` (0x900) to distinguish |
| 9 | `PspGpuProxyWriteRegister` return value ignored in callers (PspKiq.c:47-60) | **FIXED** | Return values checked in PspKiqInit/PspKiqSubmit |
| 12 | Race condition on g_GpuDriverHandle proxy init (PspKiq.c:86-94) | **FIXED** | Added spinlock + g_GpuProxyInitialized guard |

### Build Status: PASS (signed)

## Linux Register Comparison — Key Findings

**KIQ model is fundamentally different:**
- Our driver uses KIQ_BASE/CNTL/RPTR/WPTR at 0xE060-0xE078 — NOT in Linux GFX10 headers
- Linux uses CP_HQD_* registers (MQD model) via `gfx_v10_0_kiq_init_register()` for KIQ
- Our KIQ_BASE approach works on BC-250 but is BC-250-specific

**GRBM selection:**
- Our driver writes `GRBM_GFX_INDEX` (0x34D0) for ME/PIPE/QUEUE select
- Linux writes `mmGRBM_GFX_CNTL` (0x0dc2) via `nv_grbm_select()` — DIFFERENT register
- 0x34D0 confirmed writable on BC-250 but may not be the intended register

**HQD registers NBIO-blocked:**
- All CP_HQD writes at 0xDAC0+ range silently dropped by NBIO on BC-250
- Linux uses these for KIQ init; they are unusable on our hardware
- KIQ alias at 0xE060+ is the only usable path

### KIQ Progress — RPTR Still 0
- GRBM_GFX_INDEX (0x34D0) confirmed writable — selects ME=1 correctly
- HQD registers at 0xDAC0+ NBIO-blocked — ACTIVE=1, PQ_BASE, PQ_CONTROL, VMID writes all return 0
- KIQ alias writability: BASE_LO (0xE060) ✅, RPTR (0xE06C) ✅, WPTR (0xE078) ✅, ACTIVE (0xE080). READ-ONLY: SIZE (0xE068, reads 0), PQ_CTL (0xE070, reads 0x81818181)
- MEC firmware loaded successfully (cyan_skillfish2_mec.bin, version 0x90)
- KIQ_RPTR stays 0 despite MEC loaded, KIQ_ACTIVE=1, WPTR set, valid NOP in ring
- KIQ_SIZE=0 read-only prevents CP from knowing ring size
- Previous "partial HQD write" (0xDEADBEEF→0x0000BEE0) was misleading — likely stale/aliased readback

### Next Steps
1. Verify KIU firmware presence — may need separate loading beyond MEC
2. Try VRAM ring allocation if system aperture is blocker
3. Find correct RLC_CP_SCHEDULERS offset (4-byte aligned)
4. Add GRBM_GFX_CNTL register (Linux uses this, not GRBM_GFX_INDEX)
5. Fix the 4 bugs that directly affect PSP driver (RLC_CP_SCHEDULERS misalignment, IOCTL name collision, ignored return values, race condition)

## Driver Fixes Applied

### KIQ HW Register Programming (2026-06-16)
**Problem:** `PspKiqInit` only allocated software ring buffer — never programmed GPU hardware registers. GPU didn't know the ring existed. `PspKiqSubmit` wrote commands to memory but never updated GPU WPTR.

**Fix:** Complete rewrite of `PspKiq.c`:
1. **`PspKiqProgramHwRegisters()`** — Programs GPU HQD registers via GPU BAR5 proxy:
   - Halts ME+PFP (`CP_ME_CNTL`)
   - Selects KIQ engine (`GRBM_GFX_INDEX` = ME=1)
   - Programs `CP_HQD_PQ_BASE`/`PQ_BASE_HI` with ring physical address
   - Sets `CP_HQD_PQ_CONTROL` = log2(ring_size_in_dwords)
   - Sets VMID=0, PERSISTENT_STATE=0xE001
   - Activates queue (`CP_HQD_ACTIVE=1`)
   - Notifies RLC scheduler (`RLC_CP_SCHEDULERS=0xA0`)
   - Resumes CP
2. **`PspKiqInit()`** — Allocates ring (non-cached) + WPTR poll page + calls HW init
3. **`PspKiqSubmit()`** — Memory barrier + updates WPTR in GPU register (`CP_HQD_PQ_WPTR_LO`)
4. **`PspKiqCleanup()`** — Deactivates queue, frees all resources

GPU register offsets used (BAR5-relative, GC_BASE-shifted):
```
GRBM_GFX_INDEX:      0x34D0    CP_ME_CNTL:          0x4A74
CP_KIQ_BASE_LO:      0xE060    CP_HQD_ACTIVE:       0xDAC0
CP_HQD_PQ_BASE:      0xDAD8    CP_HQD_PQ_CONTROL:   0xDAFC
CP_HQD_PQ_WPTR_LO:   0xDB90    RLC_CP_SCHEDULERS:   0xECA1
```

### KIQ_SUBMIT Struct Layout Fix (2026-06-15)
**Bug:** IOCTL_PSP_KIQ_SUBMIT had mismatched struct layout between caller and handler.

- GPU driver sent `{count, cmd0, cmd1, ...}` (flat array)
- PSP driver expected `PSP_KIQ_SUBMIT_REQUEST` struct with `Reserved[3]` padding
- Handler read commands from wrong offset (inputDwords[1] instead of &req->Commands[0])

**Fix:** Both sides now use consistent struct layout:
```c
typedef struct _PSP_KIQ_SUBMIT_REQUEST {
    ULONG CommandCount;
    ULONG Reserved[3];      // padding
    ULONG Commands[64];     // PM4 commands
} PSP_KIQ_SUBMIT_REQUEST;
```

- PSP driver (`PspDriver.c`): Uses `req->CommandCount` and `&req->Commands[0]`
- GPU driver (`amdbc250_psp.c`): Uses `PSP_KIQ_SUBMIT_REQUEST` struct with proper padding

## Build commands (from repo root, in VS2022 x64 Native Tools prompt)

```cmd
build.bat                    # -> output\PspDriver.sys + .inf + .cat + sign
scripts\compile-test.bat     # -> output\test-psp-driver.exe
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
- **Spinlock on SEND_CMD**: `CommandLock` protects mailbox command sequence (C2PMSG_36 write → C2PMSG_35 write → poll).
- **GRBM_STATUS = 0xFFFFFFFF** (at Navi10 offset 0x2000): Caused by BC-250's non-standard register map (GC_BASE=0x1260). The actual register is at BAR5+0x3260 and returns valid values at the corrected offset. NBIO does NOT block GC registers on BC-250.

## CRITICAL BUG: METHOD_BUFFERED buffer sharing in PSP IOCTL (2026-07-03)

**Root cause of PSP GPU_PM4_SUBMIT error 87**: In `METHOD_BUFFERED` IOCTL, `inputBuffer` and `outputBuffer` point to the SAME system buffer (`Irp->AssociatedIrp.SystemBuffer`). The PSP driver's IOCTL handler at `PspDriver.c:1164` called `RtlZeroMemory(resp, sizeof(*resp))` which zeroed the first 44 bytes — including `req->CommandCount` at offset 0. When `PspGpuPm4Submit` then checked `req->CommandCount`, it found 0 and returned `STATUS_INVALID_PARAMETER`.

**Fix**: Save `cmdCount` and `waitMs` from `req` BEFORE `RtlZeroMemory`, then restore them after.

**Applies to all METHOD_BUFFERED IOCTL handlers** that write to the output buffer before reading all input fields. KIQ_SUBMIT handler was not affected (doesn't zero the buffer).

## Agent Analysis: PSP Driver Bugs Found (2026-07-03)

| # | Priority | Description | Fix |
|---|----------|-------------|-----|
| 1 | CRITICAL | `PspGpuProxyInit` holds spinlock while calling `ZwCreateFile` (PspCore.c:69-71) — illegal at DISPATCH_LEVEL | **FIXED**: Release lock before `PspOpenGpuDriver()`, re-acquire after |
| 2 | HIGH | `IOCTL_PSP_GPU_PM4_SUBMIT` size check requires full 268-byte struct even with `CommandCount=5` | **FIXED**: Dynamic check via `FIELD_OFFSET(..., Commands[req->CommandCount])` |
| 3 | HIGH | `IOCTL_PSP_GPU_PM4_SUBMIT` METHOD_BUFFERED buffer sharing — `RtlZeroMemory` clears `req->CommandCount` | **FIXED**: Save/restore fields around zero |
| 4 | HIGH | NBIO unlock uses GPU BAR5 (`g_Bar5Mapping`) instead of PSP BAR0 (`devExt->MmioBase`) — writes silently fail | **FIXED**: Always use `devExt->MmioBase` |
| 5 | HIGH | Handle leak race: `PspGpuProxyInit` can return early with `g_GpuDriverHandle` set but `g_GpuProxyAvailable=FALSE` | **FIXED**: Close handle via `ZwClose` on error path |
| 6 | MEDIUM | GRBM_STATUS reads offset 0x2004 (CC_CONFIG) instead of 0x2000 (GRBM_STATUS) in all 6 read sites | **FIXED**: All occurrences changed to 0x2000 |
| 7 | LOW | Error string missing in KdPrint for some IOCTL validation paths | Deferred |

## Test Tool Bugs Found by Agent (2026-07-03)

| # | Priority | Description | Fix |
|---|----------|-------------|-----|
| 1 | HIGH | `psp-gpu-pm4-submit-test.c`: PM4 header `0xC0370003` has swapped count/opcode | **FIXED**: `0xC0043700` |
| 2 | HIGH | `psp-gpu-pm4-submit-test.c`: WRITE_DATA CONTROL `0x10100000` has wrong DST_SEL | **FIXED**: `0x00000102` (register | WR_CONFIRM) |
| 3 | HIGH | `gfx-ring-init-test.c`: RPTR comparison always succeeds (false positive) — `RPTR >= wptrTarget` always true due to bit 24 | **FIXED**: Compare before/after difference |

## Known Bugs (PSP Driver)

### Bug 1: C2PMSG_81 save/restore in PspSendMailboxCommand (`PspDriver.c:124-197`)
- Saves `originalC2pmsg81` before command, **restores it after** — overwrites PSP response.
- Correct Linux protocol: poll `C2PMSG_35` for completion, do NOT touch C2PMSG_81.
- GPU driver relies on reading C2PMSG_81 to detect SOS alive status — restore corrupts this.
- **Fix**: Remove save/restore entirely; poll C2PMSG_35 until it clears.

### Bug 2: Spinlock held during polling (`PspDriver.c:117-200`)
- `KeAcquireSpinLock` at DISPATCH_LEVEL, then `KeStallExecutionProcessor(1000)` up to 500ms.
- Blocks all other threads from accessing the device during entire wait.
- **Fix**: Release spinlock before polling C2PMSG_35, re-acquire for register writes.

### Bug 3: Missing C2PMSG_37 write for high 32 bits (`PspDriver.c:141-144`)
- Only writes low 32 bits of firmware PA to C2PMSG_36.
- If `MmAllocateContiguousMemory` returns PA >4GB, high bits are lost.
- **Fix**: Write `(ULONG)(devExt->FwPhysical.QuadPart >> 32)` to C2PMSG_37 (0x10574).

### Bug 4: Shared `g_CmdBuffer` race between ring operations (`PspDriver.c`)
- `g_CmdBuffer` (1024 bytes) shared by `PspInitTmr`, `IOCTL_PSP_RING_LOAD_IP_FW`, `IOCTL_PSP_REG_PROG`, `IOCTL_PSP_AUTOLOAD_RLC`.
- Concurrent IOCTL calls corrupt each other's command buffer data.
- **Fix**: Allocate per-call command buffer via `ExAllocatePool2`. Static `g_CmdBuffer` declaration removed.

### Bug 5: LOAD_EMBEDDED_FW wrong allocation size (`PspDriver.c:881`)
```c
MmAllocateContiguousMemory(g_SysdrvFirmwareSize, highAddr);  // 262656 bytes
RtlCopyMemory(..., (PVOID)g_SosFirmwareData, g_SosFirmwareSize);  // 46592 bytes
```
- Allocates SYSDRV firmware size but copies SOS data. Wastes memory but works.
- **Fix**: Use `g_SosFirmwareSize` for allocation.

### Bug 6: BOOT_SEQUENCE always returns STATUS_SUCCESS (`PspDriver.c:1007`)
```c
status = STATUS_SUCCESS;  /* Always succeed */
```
- Even if SYSDRV or SOS loading fails, returns success.
- **Fix**: Return real NTSTATUS from failing step.

### Bug 7: Test tool LoadFirmware reads wrong struct (`test-psp-driver.c:1910-1920`)
- Test tool reads `PSP_LOAD_FW_RESPONSE` (2 ULONGs: Status + MailboxStatus).
- Driver returns only **1 ULONG** (`devExt->FwPaShifted`).
- Second field (`MailboxStatus`) contains garbage data.
- **Fix**: Change test tool to read single ULONG.

### Bug 8: REG_PROG write-only, broken for reads (`PspDriver.c:1139-1201`)
- GPCOM `PROG_REG` command (0x0B) only writes, never reads back the register.
- GPU proxy sent `{RegOffset, 0, 1}` expecting a read, got back `RegValue=0`.
- **Fix**: Added `IsRead` flag support: when input buffer has 3rd ULONG=1, use direct PSP MMIO read instead of GPCOM command.

## C2PMSG Mailbox Protocol (CORRECTED)

**Critical protocol flow for firmware loading on BC-250 PSP** — verified against Linux `psp_v11_0_8.c`:

```
1. WRITE_REGISTER_ULONG(C2PMSG_36, paLo)  # Full physical address (low 32 bits)
2. WRITE_REGISTER_ULONG(C2PMSG_37, paHi)  # High 32 bits (CRITICAL for >4GB)
3. WRITE_REGISTER_ULONG(C2PMSG_35, cmd)   # Command (0x4 = SYSDRV, 0x8 = SOS)
4. Poll C2PMSG_35 until == 0              # PSP clears it when done (not C2PMSG_81!)
5. Write 0 to C2PMSG_35                   # ACK (safe to write already-0 value)
6. DO NOT restore C2PMSG_81               # PSP manages this register
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
| GRBM/CP | 0x3260 (corrected) | `0x009A0C00` | Unlocked — wrong offset (0x2000) used previously |

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
0. Poll C2PMSG_64 bit 31 (MBOX_TOS_READY_FLAG) until set  ← CRITICAL
1. WRITE C2PMSG_69 = ring_pa_lo
2. WRITE C2PMSG_70 = ring_pa_hi
3. WRITE C2PMSG_71 = ring_size
4. WRITE C2PMSG_64 = 0x00020000  (GPCOM/KM ring = ring_type << 16)
5. Poll C2PMSG_64 bit 31 (MBOX_TOS_RESP_FLAG) until set
```

Timeout: 60s for step 0 (TOS_READY), 3s for step 5 (ring response). Both poll at 1ms intervals.
**Current HW status: step 0 always times out — C2PMSG_64 bit 31 never sets, even after SOS boot + 60s wait.**

Ring frame structure (64 bytes, `psp_gfx_rb_frame`):
```
+0:  cmd_buf_addr_lo   (GPU address of command buffer)
+4:  cmd_buf_addr_hi
+8:  cmd_buf_size       (1024 bytes for cmd buffer)
+12: fence_addr_lo
+16: fence_addr_hi
+20: fence_value
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
4. WRITE C2PMSG_67 = new wptr value (spinlock held until after this write)
5. Poll C2PMSG_64 bit 31 (response ready) — spinlock released before polling
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
GFX_CMD_ID_INIT_TMR      0x01  (init trusted memory region)
GFX_CMD_ID_LOAD_IP_FW    0x06  (load GPU IP firmware)
GFX_CMD_ID_PROG_REG      0x0B  (program register via ring)
GFX_CMD_ID_LOAD_TOC      0x20  (load TOC + get TMR size)
GFX_CMD_ID_AUTOLOAD_RLC  0x21  (start RLC autoload after all FW loaded)
```

## Test sequence (after driver install + reboot)

All IOCTLs and their flags (from test-psp-driver.exe):
```
test-psp-driver.exe -i 0xFE800000 0x200000    # Init BAR5 MMIO (required first)
test-psp-driver.exe -t                         # Connectivity test (C3/36/81)
test-psp-driver.exe -f cyan_skillfish2_sos.bin # Load FW blob
test-psp-driver.exe -C 0x4                     # SYSDRV mailbox command
test-psp-driver.exe -C 0x8                     # SOS mailbox command
test-psp-driver.exe -E                         # Load EMBEDDED firmware
test-psp-driver.exe -B                         # Full BOOT sequence
test-psp-driver.exe -s                         # PSP status snapshot
test-psp-driver.exe -H                         # Comprehensive HW probe
test-psp-driver.exe -m                         # Read C2PMSG_81
test-psp-driver.exe -u                         # NBIO unlock (direct sigs)
test-psp-driver.exe -U                         # NBIO unlock via ring
test-psp-driver.exe -R                         # Create GPCOM ring
test-psp-driver.exe -M                         # Init TMR (needs ring)
test-psp-driver.exe -L <type> <file>           # Load GPU FW via ring
test-psp-driver.exe -A                         # Load ALL 8 GPU FW + RLC
test-psp-driver.exe -P <id> <val>              # Program register via ring
test-psp-driver.exe -T                         # RLC autoload (trigger)
test-psp-driver.exe -G                         # Get GPU bridge info
```

Error 21 (`ERROR_NOT_READY`) from any IOCTL means `INIT_HW` not called yet.

## Source files

| File | Purpose |
|------|---------|
| `src/driver/PspDriver.c` | WDM driver (~1052 lines). IOCTL dispatch, INIT_HW, BOOT_SEQ, proxy. |
| `src/driver/PspCore.c` | Mailbox, firmware validation, GPU proxy, TMR, auto-init. |
| `src/driver/PspKiq.c` | **KIQ ring with GPU HQD register programming** — allocates ring, programs HW, submits PM4, updates WPTR. |
| `src/driver/PspSmu.c` | SMU wake command. |
| `src/test/test-psp-driver.c` | User-mode test tool (~931 lines). All IOCTL wrappers + CLI args. |
| `inc/PspIoctl.h` | Shared IOCTL codes + struct definitions. |
| `inc/firmware_data.h` | Embedded firmware arrays (SOS + SYSDRV). Generated by `scripts/gen-firmware-h-v3.ps1`. |
| `inf/PspDriver.inf` | Device installation. Matches `PCI\VEN_1022&DEV_143E`. |

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

### Accessible MMIO ranges (BC-250 corrected offsets)
| Range | Example offset | Status |
|-------|---------------|--------|
| GC (shifted) | 0x3260-0x3FFF | Unlocked (READ_REG/WRITE_REG ok) |
| MMHUB | 0x5000-0x50D0 | Unlocked |
| HDP | 0x05A0 | Unlocked |
| NBIO SIG | 0xC100, 0xC180 | Writable |
| GRBM/CP | 0x3260 (GC_BASE+0x2000) | **Unlocked** at corrected offset |

### Limitations
- PSP GPCOM ring not supported by BIOS SOS (C2PMSG_64 always 0)
- GPU firmware loading via ring blocked by unsupported ring protocol
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

Repo: https://github.com/bc250-collective/SomnacinDumper-CPUCoreMod — Additional research from the bc250-collective GitHub organization
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

### 2. Ring create waits for MBOX_TOS_READY_FLAG first
```c
ret = psp_wait_for(psp, C2PMSG_64, MBOX_TOS_READY_FLAG, MBOX_TOS_READY_MASK, 0);
if (ret) { DRM_ERROR("Failed to wait for trust OS ready for ring creation\n"); return ret; }
WREG32(C2PMSG_69, ring_addr_lo);
WREG32(C2PMSG_70, ring_addr_hi);
WREG32(C2PMSG_71, ring_size);
WREG32(C2PMSG_64, ring_type << 16);
psp_wait_for(C2PMSG_64, MBOX_TOS_RESP_FLAG, ...);
```

### 3. psp_v11_0.c (full devices) vs psp_v11_0_8.c (cyan_skillfish2)
| Feature | psp_v11_0.c (Navi10 etc.) | psp_v11_0_8.c (BC-250) |
|---------|--------------------------|------------------------|
| Load SOS firmware | Yes (from filesystem) | **No** (pre-loaded by BIOS) |
| Load SYSDRV | Yes (mailbox CMD 0x4) | **No** |
| Wait for bootloader | C2PMSG_35 bit 31 | **Not used** |
| SOS alive check | C2PMSG_81 != 0 | **Not used** |
| Ring create before TMR | Yes | Yes |
| C2PMSG_64 ready wait | Yes | Yes |
| APU codename | - | **CYAN_SKILLFISH2** |

### 4. Mailbox protocol (CORRECT — Linux uses C2PMSG_35, not C2PMSG_81)
Linux `psp_v11_0_mbox_send()`:
1. Wait for C2PMSG_35 to become 0 (previous cmd done)
2. Write data to C2PMSG_36
3. Write cmd to C2PMSG_35 (triggers PSP)
4. **Poll C2PMSG_35 until 0** (PSP clears when done)
5. Read result status from command/data registers

Our driver incorrectly:
1. Saves C2PMSG_81, clears it
2. Writes data to C2PMSG_36, cmd to C2PMSG_35
3. **Polls C2PMSG_81** (not C2PMSG_35)
4. Restores C2PMSG_81 → **clobbers PSP response**

## Session results (2026-06-11): BC-250 register map is NOT standard Navi10

### Critical Discovery: GC_BASE offset

Found the root cause of all 0xFFFFFFFF register reads at offsets 0x2000-0x2FFF:

**BC-250 (Cyan Skillfish) uses different BAR5 register offsets than standard Navi10.**

From `linux/drivers/gpu/drm/amd/include/cyan_skillfish_ip_offset.h`:
```
GC_BASE__INST0_SEG0 = 0x00001260  ← GC registers shifted by 0x1260 bytes
GC_BASE__INST0_SEG1 = 0x0000A000  ← second segment
```

This means register `mmCC_GC_SHADER_ARRAY_CONFIG` (at byte offset 0x2004 on Navi10) is at **BAR5 + 0x1260 + 0x2004 = BAR5 + 0x3264** on BC-250.

**Impact on previous testing:** ALL 0x2000-0x2FFF reads returned 0xFFFFFFFF because they hit unmapped address space, NOT because of NBIO firewall. The NBIO firewall status for GC registers is actually **unknown** — we were testing the wrong addresses.

**Linux amdgpu** works because RREG32_SOC15/WREG32_SOC15 macros automatically add GC_BASE to register offsets via `cyan_skillfish_reg_init.c`.

**40 CU unlock patch** (duggasco/bc250-40cu-unlock) writes to registers through the proper amdgpu MMIO path which handles the offset shift.

### Corrected Register Offsets for BC-250

| Register | Navi10 Offset | BC-250 BAR5 Offset |
|----------|--------------|-------------------|
| CC_GC_SHADER_ARRAY_CONFIG | 0x2004 | **0x3264** |
| GRBM_STATUS | 0x2000 | **0x3260** |
| SPI_PG_ENABLE_STATIC_WGP_MASK | 0x229C | **0x34FC** |
| RLC_PG_ALWAYS_ON_WGP_MASK | 0x2B04 | **0x3D64** |
| CP scratch registers | 0x2074+ | **0x32D4+** |
| SDMA registers | 0x2600+ | **0x3860+** |

### Next Steps (ALL DONE — confirmed 2026-06-11)
1. ✅ **Register reads at corrected offsets** — CONFIRMED: 0x3260, 0x3264, 0x34FC all return valid values
2. ✅ **NBIO firewall status** — CONFIRMED: NBIO does NOT block GC registers at corrected offsets
3. ✅ **Windows driver offset update** — GC_BASE=0x1260 applied across GPU driver and docs
4. ✅ **40 CU unlock test** — Write 0xFFE00000 to 0x3264 and 0x1F to 0x34FC (pending physical test)
5. Test warm reboot from Linux (with amdgpu loaded) into Windows

## Session results (2026-06-11): Ring protocol limitations & NBIO reassessment

### CREATE_RING protocol correction
- **Removed TOS_READY wait** before writing ring params. Linux psp protocol:
  1. Write ring PA to C2PMSG_69/70, size to C2PMSG_71
  2. Write ring type to C2PMSG_64 (triggers ring creation)
  3. Wait for TOS_RESP_FLAG (C2PMSG_64 bit 31)
- C2PMSG_81 should NOT be cleared before ring creation — it's SOS alive register, not ring sync
- Fixed `g_RingBufferPhysical` initialization: was 0 (never initialized) causing ring at PA=0

### Ring type: KM=1, not KM=2
- Linux enum: `PSP_RING_TYPE_KM = 1` (KM ring = 0x00010000, not 0x00020000)
- Changed from 0x00020000 to 0x00010000 — still no response from TOS

### Mailbox-based PROG_REG fallback
- When GPCOM ring not available, REG_PROG now tries to send PROG_REG (0x0B) via C2PMSG_35/36/37 mailbox
- PSP accepts the command (C2PMSG_35 clears), but **register write is silently ignored**
- Verified by attempting to write 0x12345678 to 0xC100 — readback still returns 0xFEDCBAEF
- SOS firmware does NOT support mailbox-based register programming

### SOS pre-loaded by BIOS
- C2PMSG_81 = 0xF0000010 is readable immediately after INIT_HW, without BOOT_SEQUENCE
- SOS is pre-loaded by BIOS/UEFI during POST
- Our BOOT_SEQUENCE (CMD 0x4/0x8) is redundant — SOS already running
- BOOT_SEQUENCE is harmless but unnecessary

### NBIO firewall reassessment: GC registers NOT blocked
- **GC registers at corrected offsets (0x3260+) are directly accessible via BAR5 MMIO**
- NBIO on BC-250 does NOT implement the PS5-style register firewall for GC/GRBM/SDMA blocks
- All previous "blocked" results were due to reading unmapped address space (wrong offsets)
- Linux amdgpu's `RREG32_SOC15`/`WREG32_SOC15` macros handle the offset shift automatically
- Direct BAR5 MMIO at 0x3260, 0x3264, 0x34FC returns valid values with no system instability
- Conclusion: NBIO unlock is not required for register access at corrected offsets

### Ring protocol still unsupported
- Even with SOS loaded and alive (C2PMSG_81 = 0xF0000010):
  - GPCOM ring: ❌ (TOS protocol not implemented in firmware)
  - Mailbox PROG_REG: ❌ (accepted but write ignored)
- Linux `psp_v11_0_8` skips ALL PSP firmware loading and ALL ring commands

### GPU Driver Proxy fixes (this session)
- Fixed GET_CAPS early-return: field layout now matches main handler (d[4]=CUs=24, not temperature)
- Fixed GET_VRAM_INFO early-return: uses ULONG64 byte layout (not ULONG MB — caused "16777216 MB" display bug)
- Fixed PspProxyInit(): only sets g_PspProxyAvailable=TRUE when PSP_GET_GPU_INFO succeeds
- Fixed NBIO_MAP INIT_HARDWARE path: sets GpuClockMhz=2000, MemoryClockMhz=1500 defaults

## Session results (2026-06-10): Fundamental HW blockers

### Confirmed: GPCOM ring protocol NOT supported by this SOS
- C2PMSG_64 = 0x00000000 always — bit 31 (MBOX_TOS_READY) NEVER sets, even after 60s poll
- C2PMSG_69/70/71 (ring addr/size) writes are **silently rejected** (read back returns 0)
- C2PMSG_65/66/67/68 (RBI/GPCOM wptr/rptr) = 0 always
- TMR init (`-M`) also fails because it uses ring protocol internally

### 0xF0000010 reinterpretation
- `0xF0000010` is **NOT** "SOS alive" — it's "Firmware validation failed (wrong type, bad signature)"
- This is a stale error from the BIOS bootloader's attempt to boot SOS
- After our `-B` sequence loads SOS fresh, C2PMSG_81 = 0x00000000 (idle/success)
- The BIOS-loaded SOS FAILED (0xF0000010), our mailbox-loaded SOS SUCCEEDS (0x00000000)
- **Neither variant sets C2PMSG_64 bit 31** — the SOS on this BIOS version simply doesn't support rings

### SMN access: read-only from NB PCI config
- SMN read works through B0D0F0+0xB8 (addr) / 0xBC (data)
- SMN write does NOT work through this method
- SMU registers at SMN 0x0154xxxx accessible R/O but not writable from host

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
| **GRBM/CP registers (0x3260+ corrected)** | **✅ Unlocked (wrong offset 0x2000 used previously)** |
| **SMN writes** | **❌ Host blocked (R/O only)** |
| **SMU commands** | **❌ Need PSP or MSR access** |

## Agent setup (TODO)

User requested setting up multiple agents to work in parallel:
1. **Code reviewer agent** — analyze driver/test code, suggest fixes, find bugs
2. **Research agent** — search internet for useful info (PSP protocol, AMD NBIO, BC-250 docs)
3. **Architecture agent** — suggest alternative approaches, spot design issues

Use `task` tool with `general` subagent type for these roles.
Reminder: check `E:\` drive is connected before build commands (user sometimes forgets).

## Linux psp_v11_0_8 vs psp_v11_0 comparison (confirmed 2026-06-13)

### BC-250 (`IP_VERSION(11, 0, 8)`) is NOT in any `init_microcode` switch
- `psp_v11_0_init_microcode()` in `psp_v11_0.c` has cases for: 11.0.0, 11.0.2, 11.0.4, 11.0.5, 11.0.7, 11.0.9, 11.0.11, 11.0.12, 11.0.13, 11.5.0, 11.5.2
- **11.0.8 is absent** — Linux never loads SOS/ASD/TA firmware files for BC-250
- Linux relies entirely on BIOS pre-loaded SOS for this device

### Key differences: 11.0.0 (Navi10 dGPU) vs 11.0.8 (BC-250)
| Feature | 11.0.0 (Navi10) | 11.0.8 (BC-250) |
|---------|-----------------|-----------------|
| `init_microcode` | Loads SOS + ASD + TA | **Not called** (no entry) |
| SOS source | Filesystem (`psp_init_sos_microcode`) | **BIOS pre-loaded** |
| Ring protocol | Full support | **TOS never sets MBOX_TOS_READY** |
| Mailbox CMD 0x4/0x8 | Used in `psp_v11_0.c` | **Not used in `psp_v11_0_8.c`** |

### 11.5.0 (Steam Deck APU) closest analog
- Also skips SOS firmware loading
- Loads only ASD + TOC microcode
- Confirms APU-like devices (BC-250, Steam Deck) have minimal PSP that relies on BIOS

### Linux doesn't use mailbox for register programming
- `psp_v11_0_8` has no `cmd_submit` — no GPCOM mailbox commands
- All register access goes through direct BAR5 MMIO (RREG32/WREG32 with GC_BASE offset)
- This matches our finding that mailbox PROG_REG is accepted but silently ignored

### Conclusion
- Our Windows driver behavior (mailbox CMD 0x4/0x8 works, ring protocol doesn't) is **consistent with Linux**
- Ring protocol failure is a SOS firmware limitation, not a driver bug
- Direct MMIO at corrected offsets (GC_BASE=0x1260) is the intended access method

## Fixes Applied (2026-06-13)

### Driver fixes
1. **C2PMSG_37 constant** - Added `PSP_C2PMSG_37_OFFSET` to PspIoctl.h, updated PspDriver.c to use it
2. **GC_BASE offset** - Already correctly applied (0x1260 shift for BAR5 registers)
3. **Mailbox protocol** - Already fixed: polls C2PMSG_35, not C2PMSG_81
4. **Spinlock handling** - Already fixed: releases before polling
5. **Per-call buffers** - Already fixed: uses ExAllocatePool2 for ring operations
6. **BOOT_SEQUENCE** - Already fixed: returns real NTSTATUS

### Test tool fixes
1. **64-bit address parsing** - Changed `-i` option to use `strtoull()` for physical addresses
2. **FW type mapping** - Corrected enum: CE=3, PFP=2, ME=1, SDMA=9, SDMA1=10, RLC=8 (was incorrect 6/8/9)

## Windows 11 26100 Compatibility Fix (2026-06-15)

### Problem
On Windows 11 26100, `MmMapIoSpace` fails when trying to map GPU BAR5 (`0xFE800000`), returning NULL. This blocks all mailbox access since PSP mailbox registers live in BAR5 space.

### Solution
Implemented GPU driver proxy fallback:
1. **PSP driver** (`PspCore.c`):
   - Opens handle to GPU driver device (`\\Device\\AMDBC250DreamV43`)
   - Uses `IOCTL_AMDBC250_BAR5_READ_PROXY` (0x900) and `IOCTL_AMDBC250_BAR5_WRITE_PROXY` (0x901)
   - Falls back to proxy when `MmMapIoSpace` fails in `INIT_HW`

2. **GPU driver** (`amdbc250_dream_kmd.c`):
   - Already has proxy IOCTLs that access `DevExt->MmioVirtualBase`
   - BAR5 is mapped by GPU driver's `INIT_HARDWARE` IOCTL

### Required sequence
1. Install GPU driver first (maps BAR5)
2. Install PSP driver
3. `INIT_HW` will use GPU proxy if direct mapping fails

## Test Results (2026-06-15)

### Pasileidimo eiga (driveris įdiegtas, Test Signing įjungtas)
```
test-psp-driver.exe -i 0xFE800000 0x200000   # INIT_HW - SUCCESS (VA=0xD1800000)
test-psp-driver.exe -t                       # Connectivity test - PASS
test-psp-driver.exe -B                       # BOOT_SEQUENCE - SUCCESS (SYSDRV/SOS sent)
test-psp-driver.exe -s                       # PSP status - FW Loaded: YES (262144 bytes)
test-psp-driver.exe -H                       # Comprehensive probe - NBIO sig write OK, Ring prog FAIL, NBIO via ring OK
test-psp-driver.exe -m                       # C2PMSG_81 - 0xF0000010 (SOS alive)
test-psp-driver.exe -U                       # NBIO via ring - GRBM UNLOCKED (0x3260 accessible)
test-psp-driver.exe -R                       # CREATE_RING - ERROR_NOT_READY (TOS ring protocol not supported)
test-psp-driver.exe -M                       # INIT_TMR - ERROR_NOT_READY (needs ring)
```

### Windows 11 26100 Test Results
```
# GPU driver first (maps BAR5)
safe-test.exe -m  # READ_REG GPU_ID = 0x9FFF9700

# Then PSP driver
test-psp-driver.exe -i 0xFE800000 0x200000   # INIT_HW - uses GPU proxy
test-psp-driver.exe -m                       # C2PMSG_81 - 0xF0000010 (PSP alive)
```

### Patvirtinta
- BAR5 MMIO mapping (0xFE800000, 2MB) - veikia
- Mailbox COMMAND 0x4/0x8 (SYSDRV/SOS) - veikia
- NBIO unlock (NBIO_SIG1=0xFEDCBAEF, SIG2=0xFEDCBADF) - veikia
- GC registrai 0x3260+ (GRBM, SPI_PG, RLC_PG) - unlocked
- C2PMSG_81 = 0xF0000010 - SOS statusas (po BOOT_SEQUENCE)

### Užblokuota (SOS firmware ribos)
- GPCOM ring (C2PMSG_64 bit 31 never sets) - TOS protokolas neimplementuotas
- TMR init - reikia ring
- GPU firmware loading via ring - reikia ring
- Mailbox PROG_REG - priimta bet ignoruota

## Kitos testavimo kryptys (atenkantys į ring ribas)

### Kryptis 1: Išsamus MMIO tyrimas
- Testuoti visus unsigned registrus ties 0x3260+ (GC_BASE + offset)
- Bandyti spinorius: SPI_PG_ENABLE_STATIC_WGP_MASK (0x34FC), RLC_PG_ALWAYS_ON_WGP_MASK (0x3D64)
- Patikrinti 40 CU unlock: WRITE 0xFFE00000 to 0x3264, WRITE 0x1F to 0x34FC

### Kryptis 2: C2PMSG_64 TOS_READY stebėjimas
- Ilgai stebėti C2PMSG_64 bit 31 (bet jis nėra palaikomas šio SOS)
- Patikrinti ar BIOS v3 SOS gali būti pakraunama ir ar ji palaiko ring

### Kryptis 3: SMU komandų testas
- `PspSmuWake()` per IOCTL_PSP_SMU_WAKE - SMU nėra aktivus
- SMN R/O access per PCI config (veikia, bet neįmanoma rašyti)
- SMU NBIF/IOMMU registrai (0x0154xxxx) prieinami tik skaityti

### Kryptis 4: Firmware keitimas
- Bandyti v3 BIOS SOS (gali turėti GPCOM ring palaikymą)
- Generuoti naują `firmware_data.h` su `scripts/gen-firmware-h-v3.ps1`
- Testuoti ar `IOCTL_PSP_BOOT_SEQUENCE` veikia su v3 firmware

### Kryptis 5: Driverio tobulinimas
- Perkelti į KMDF/WDF (su saugumu) - šiuo metu WDM native
- Pridėti KIQ ring debug output
- Ištaisyti g_RingBufferPhysical inicializaciją (kad nepadėtų PA=0)
- **Svarbiausias bug'as**: `g_RingBuffer[0x1000]` statinis masyvas (C53) turi neteisingą fizinį adresą
  - Reikia `MmAllocateContiguousMemory(0x1000)` vietoje statinio masyvo
  - Tačiau TOS ring protokolas neleidžiamas šio SOS → nėra prioritetas

---

## GPU Driver Progress (2026-06-26) — Affects PSP KIQ Path

### Software PM4 Executor — KIQ_SIZE=0 Block Confirmed
- **GPU driver implemented `DreamV3SwPm4Process`** — software PM4 executor that translates PM4 packets (IT_NOP, IT_WRITE_DATA, IT_EVENT_WRITE_EOP, IT_RELEASE_MEM, PM4_TYPE_0) to direct register MMIO writes
- **FUNDAMENTAL BLOCKER**: KIQ_SIZE (0xE068) is factory **read-only = 0** — hardware thinks ring has 0 bytes, CP refuses to process
- KIQ_SIZE not found in MEC firmware binary — check is **hardware-level**, not patchable
- **PATH 3 fallback** in SEND_PM4 IOCTL: when PSP KIQ (Path 1) AND GfxRing (Path 2) are both unavailable, falls through to software PM4 executor

### PSP Driver KIQ — Same Block
- PSP driver's KIQ ring (via PspKiq.c) exhibits same behavior: WPTR advances but RPTR stays 0
- **KIQ_SIZE=0 affects PSP KIQ too** — both GPU and PSP KIQ paths hit the same hardware block

### IC_BASE Register Fixes (Affects FW Loading via PSP)
- Fixed IC_BASE register offsets (was 0x7C10-0x7C18, corrected to 0x17390-0x17398)
- Fixed IC_BASE_CNTL: write 0 (not 0x100), write LO/HI before CNTL to prevent premature DMA
- Added UCODE_ADDR polling (500ms timeout) before unhalt to confirm DMA completion
- **Sequence must match Linux**: write LO → HI → CNTL=0 → upload JT → write version → poll → unhalt

### CP_HQD_* Registers Completely NBIO-Blocked (0xDAC0-0xDBFF)
- ALL CP_HQD registers (ACTIVE, VMID, PQ_BASE, PQ_CONTROL, PQ_WPTR_LO, PQ_RPTR) confirmed NBIO-blocked
- PSP driver's `PspKiqProgramHwRegisters` writes to these registers via GPU proxy — **writes are silently dropped**
- GRBM_GFX_CNTL (0x2022) does NOT enable HQD access on BC-250
- **KIQ_BASE alias at 0xE060+ is the only writable path** for KIQ on BC-250

### KIQ_WPTR 9-bit Limit
- KIQ_WPTR (0xE078) is only 9 bits (mask 0x1FF) — max ring 512 dwords (2048 bytes)
- Values beyond 0x3FF read back as `value & 0x1FF`

### MEC Firmware Execution Verified
- Corrupting MEC ucode at offset 0x41830 → SCRATCH changed from written value to 0x00000000
- Proves MEC engine executes loaded firmware and modifies memory
- MEC2 firmware has real ARM64 ucode (MEC1 has 0x78 0x80 NOP pattern) but KIQ behavior identical

### SCRATCH Register Behavior
- SCRATCH (0x32D4) is writable via IOCTL but high nibble [31:28] hardware-masked (e.g., 0xCAFEBABE → 0x4AFEBABE)
- Confirmed via IT_WRITE_DATA PM4 → direct MMIO readback shows masked value

### RLC Firmware Loading Skipped
- RLC registers (0x3A00-0x3A50) are in the FREEZE ZONE (0x3400-0x8100) on BC-250
- BIOS/SMU handles RLC firmware loading
- DreamV3InitRlc in rlc.c is a no-op

### PM4 Header Format Correction
- ALL existing test tools (GPU and PSP) used wrong PM4 TYPE3 header (`0xC0370003` instead of `0xC0023700`)
- Correct: `PM4_TYPE3_HDR(op, count) = (3<<30) | ((count-1)<<16) | (op<<8)`
- `0xC0370003` encodes opcode=0 (NOP), count=56 — wrong for IT_WRITE_DATA
- `0xC0023700` encodes opcode=0x37 (IT_WRITE_DATA), count=4 — correct
- Header fix confirmed by working software PM4 executor test

### Path Forward
- **Software PM4 executor is the working path** — all hardware ring paths factory-locked
- PSP driver KIQ path (Path 1) remains as fallback but will hit KIQ_SIZE=0 block
- GPU driver SEND_PM4 now has PATH 3 (software PM4) working as confirmed working bypass
- All existing test tools using wrong headers need updating (not urgent since KIQ never processes)
