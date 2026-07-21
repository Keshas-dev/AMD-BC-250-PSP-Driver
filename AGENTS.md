# AMD BC-250 PSP Test Tool — Agent Notes

## Status (2026-07-21): CONVERTED to non-PnP user-mode test tool

The PSP kernel driver (WDM PnP) has been **removed**. The separate `PspDriver.sys`
kernel driver is no longer needed because:

1. The GPU driver (`atikmdag.sys`) now has **direct PSP mailbox IOCTLs**:
   - `IOCTL_AMDBC250_PSP_LOAD_IP_FW` — load GPU firmware via C2PMSG mailbox
   - `IOCTL_AMDBC250_PSP_SMU_MSG` — send SMU messages via SMN (BAR5+0x38/0x3C)
2. The GPU driver already maps BAR5 (`MmMapIoSpace(0xFE800000)`) and has direct
   register access via `IOCTL_AMDBC250_BAR5_READ_PROXY` (0x900) / `WRITE_PROXY` (0x901).
3. The old PSP driver targeted `DEV_143E` (CPU PSP) — **wrong device** — and wrote
   mailbox commands to PSP BAR0 instead of GPU BAR5.

## What remains

- `src/test/test-psp-driver.c` — user-mode test tool, opens `\\.\AMDBC250DreamV43`
  and sends GPU driver IOCTLs for PSP mailbox / SMU / register access.
- `inc/PspIoctl.h` — shared IOCTL struct definitions (cleaned of kernel-only types).
- `Firmware/` — firmware `.bin` files (cyan_skillfish2_*).
- `extract-firmware.py` — firmware extraction from VBIOS/Linux.
- `docs/` — documentation.

## Build

`build.bat` compiles `output\psp-test-tool.exe` (user-mode EXE, no kernel build).

## Usage

```
psp-test-tool.exe -i 0xFE800000 0x80000   # Init HW (map BAR5 via GPU driver)
psp-test-tool.exe -t                       # Quick connectivity test
psp-test-tool.exe -L 8 cyan_skillfish2_rlc.bin  # Load RLC firmware
psp-test-tool.exe -S 0x02 0                # Get SMU version
psp-test-tool.exe -A                       # Load ALL firmware
psp-test-tool.exe -r 0x10614              # Read C2PMSG_81 register
psp-test-tool.exe -w 0x34D0 0xE0000000    # Write GRBM_GFX_INDEX (broadcast)
```
