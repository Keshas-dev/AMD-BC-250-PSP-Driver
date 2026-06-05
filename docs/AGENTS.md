# Agent Notes: AMD BC-250 PSP Windows Driver

## Repo Overview

Separate PSP (Platform Security Processor) driver for AMD BC-250. NOT the main GPU driver — that lives in `../AMD-BC-250-Windows-Driver-main/`.

## Architecture

- **Driver type**: **WDM** (native NT). NOT KMDF — KMDF caused Code 0x7e.
- **Target**: PCI `VEN_1022&DEV_143E` (AMD PSP on BC-250)
- **MMIO**: Maps **Graphics BAR5** (`0xFE800000`), NOT PSP BAR0. C2PMSG registers live in BAR5.
- **IOCTL**: METHOD_BUFFERED, user-mode via `\\Device\\AmdBcPsp`
- **Device symlink**: `\\DosDevices\\AmdBcPsp`

## Building

### Prerequisites
- VS2022 (Community/Professional/Enterprise) + WDK 10.0.26100+ on any of C-H drives
- `bcdedit /set testsigning on` + **Reboot** (must see "Test Mode" watermark)
- **Secure Boot must be OFF** in BIOS. Test signing is blocked if Secure Boot is ON.
- **Critical**: Use `AMD-BC250-Signer` certificate (from sibling project). It is pre-installed in Trusted Root store. Do NOT generate a new cert.

### Build steps
```cmd
# Set VS environment
call "E:\Program Files\Microsoft Visual Studio\2022\Community\VC\Auxiliary\Build\vcvars64.bat"

# Compile driver
cl.exe /c /kernel /W3 /Zi /Od /DAMD64 /I"<WDK>\km" /I"<WDK>\km\crt" /I"<WDK>\shared" /Iinc /Fo:output\PspDriver.obj src\driver\PspDriver.c

# Link (no WDF libs — WDM driver)
link.exe /DRIVER /SUBSYSTEM:NATIVE /ENTRY:DriverEntry /OUT:output\PspDriver.sys output\PspDriver.obj ntoskrnl.lib wdm.lib hal.lib ntstrsafe.lib BufferOverflowK.lib /LIBPATH:"<WDK>\km\x64"

# Compile test tool
cl.exe /W3 /Zi /O2 /DAMD64 /I"<SDK>\um" /I"<SDK>\shared" /I"<SDK>\ucrt" /Iinc src\test\test-psp-driver.c /Fe:output\test-psp-driver.exe /link /LIBPATH:"<SDK>\um\x64" /LIBPATH:"<SDK>\ucrt\x64"

# Sign
signtool sign /fd SHA256 /a /s My /n "AMD-BC250-Signer" output\PspDriver.sys

# Generate INF catalog
Inf2Cat /driver:output /os:10_x64
signtool sign /fd SHA256 /a /s My /n "AMD-BC250-Signer" output\pspdriver.cat
```

## Installation & Testing

### Uninstall old driver first (CRITICAL)
Device Manager → find device → **Uninstall** → check "Delete the driver software for this device" → **Reboot**

### Install
Device Manager → Scan → unknown device → Update Driver → Browse → `output\`

### Test sequence
```cmd
cd output
test-psp-driver.exe -i 0xFE800000 0x200000   # Init BAR5 MMIO mapping
test-psp-driver.exe -t                        # Connectivity test
test-psp-driver.exe -f cyan_skillfish2_sos_extracted.bin  # FW load
```

## Known Hardware Facts (Verified)

| Address | Register | Value | Meaning |
|---------|----------|-------|---------|
| MMIO `0xFE800000` | BAR5 base | — | Graphics BAR, C2PMSG mailbox lives here |
| BAR5+`0x1056C` | **C2PMSG_35** | `0x00000000` | Command register (idle) |
| BAR5+`0x10570` | **C2PMSG_36** | `0x00000000` | Data register (PA) |
| BAR5+`0x10614` | **C2PMSG_81** | `0xF0000010` | PSP SOS status (alive and ready) |
| BAR5+`0x2004` | **GRBM_STATUS** | `0xFFFFFFFF` | BLOCKED by NBIO firewall |
| PSP BAR0 | B0:D8:F0 | `0xFD600000` | Native PSP BAR (not used by this driver) |

## Firmware Loading (IOCTL_PSP_LOAD_FW)

**Flow:**
1. User sends firmware blob via DeviceIoControl
2. Driver allocates contiguous memory (`MmAllocateContiguousMemory`, <4GB)
3. Copies firmware blob, gets PA (`MmGetPhysicalAddress`)
4. Writes PA to C2PMSG_36, command `0x1` to C2PMSG_35
5. **Waits for C2PMSG_81 to CHANGE** (not just become non-zero — it starts at `0xF0000010`)
6. Times out after 10s if unchanged

**Critical bug fixed:** Original code checked `statusReg != 0` but C2PMSG_81 is already `0xF0000010` when SOS is alive, so the wait loop exited immediately. Fixed to check `statusReg != initialStatus`.

**Current limitation:** GRBM_STATUS still returns `0xFFFFFFFF` after FW load. NBIO unlock needs intact SOS firmware with valid signature/CSUM, not a truncated file.

## Signing

- Use `AMD-BC250-Signer` cert already in Trusted Root store (from sibling project)
- PFX at `../AMD-BC-250-Windows-Driver-main/testcert.pfx`
- Do NOT create new certs — they won't be trusted by Windows
- Self-signed certs must be added to `Cert:\CurrentUser\Root` store to pass `signtool verify /pa`

## Common Errors

| Code | Error | Fix |
|------|-------|-----|
| **Code 52** | Unsigned driver | Sign with AMD-BC250-Signer OR disable Secure Boot |
| **0x7e** | KMDF init failed | Use WDM, not KMDF (no WDF libs in link) |
| **Error 2** | Device not found | Driver not installed or not loaded |
| **`0xFFFFFFFF` on GRBM** | NBIO firewall | Need proper SOS firmware to unlock |

## Sibling Project Reference

`../AMD-BC-250-Windows-Driver-main/` has working examples of:
- Manual PCI config access patterns
- NBIO register map (blocked/allowed regions)
- IOCTL dispatch with WDM driver
- PSP v11 initialization flow

## Source of Truth

`docs/tikslas.txt` — original spec in Lithuanian with driver skeleton code.
