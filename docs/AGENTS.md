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
test-psp-driver.exe -i 0xFE800000 0x200000                     # Init BAR5 MMIO (also inits ring buffer PA)
test-psp-driver.exe -t                                          # Connectivity test
test-psp-driver.exe -f cyan_skillfish2_sos_extracted.bin        # Load FW (persistent, no command sent)
test-psp-driver.exe -C 0x00000004                                # SYSDRV mailbox command
test-psp-driver.exe -C 0x00000008                                # SOS mailbox command
test-psp-driver.exe -R                                           # Create PSP ring (C2PMSG_69/70/71/64)
test-psp-driver.exe -U                                           # NBIO unlock via ring + signature regs
test-psp-driver.exe -t                                           # Check C2PMSG_81
test-psp-driver.exe -r 0x2004                                    # GRBM_STATUS
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

## Firmware Loading (IOCTL_PSP_LOAD_FW + IOCTL_PSP_SEND_CMD)

The PSP boot on BC-250 requires TWO commands: `0x4` (SYSDRV) then `0x8` (SOS).

### Key design decisions from sibling project (`amdbc250_psp_v11.c`):
1. **PA >> 20 format** — C2PMSG_36 receives `PhysicalAddress >> 20` (1MB-aligned), NOT full PA
2. **Persistent buffer** — firmware memory is NOT freed after LOAD_FW IOCTL. It stays allocated until DriverUnload, so subsequent `-C` commands reuse the same physical address
3. **C2PMSG_36 re-written before each command** — PSP clears it after processing, so `-C` re-writes PA>>20 before writing command to C2PMSG_35
4. **Wait for CHANGE** — polls C2PMSG_81 until value differs from initial (was `statusReg != 0` bug — C2PMSG_81 starts at `0xF0000010`)

### PSP mailbox protocol (from Linux amdgpu analysis):
```
-f <fw.bin>:        Allocates → copies → stores PA>>20 → C2PMSG_36=PA>>20 → C2PMSG_35=0x1 → wait
-C 0x00000004:      C2PMSG_36=PA>>20 → C2PMSG_35=0x4 (SYSDRV) → wait
-C 0x00000008:      C2PMSG_36=PA>>20 → C2PMSG_35=0x8 (SOS) → wait
```

## PSP Ring Buffer (+ IOCTL_PSP_CREATE_RING + IOCTL_PSP_NBIO_VIA_RING)

Following the sibling project's PSP v11 init flow, the driver can create a PSP communication ring:

1. **CREATE_RING** (`-R`): Programs PSP C2PMSG_69/70 (ring PA), C2PMSG_71 (ring size 4KB), C2PMSG_64 (trigger). Uses a static 4KB buffer, no allocation overhead.

2. **NBIO_VIA_RING** (`-U`): Writes command `0x00020000` (GFX_CTRL_CMD_ID_DESTROY_RINGS / NBIO unlock) to ring, updates ring write pointer (C2PMSG_67), then also writes NBIO signature registers. Checks MMHUB and GRBM_STATUS in response.

### NBIO unlock status (verified on real HW):
- NBIO signature regs `0xC100/0xC180` = `0xFEDCBAEF`/`0xFEDCBADF` (already set)
- MMHUB (0x5000+), GC (0x3000+), HDP (0x05A0+), DF, NBIO blocks = **readable/writable**
- **GRBM/CP (0x2000-0x2FFF) = `0xFFFFFFFF`** — hardware-level PS5 NBIO restriction. NOT a driver bug.

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
| **Error 31** | NBIO unlock failed | MMHUB unchanged after sig write (already unlocked) |
| **`0xFFFFFFFF` on GRBM** | NBIO firewall | Hardware-level PS5 restriction (GRBM/CP blocked) |

## Sibling Project Reference

`../AMD-BC-250-Windows-Driver-main/` has working examples of:
- Manual PCI config access patterns
- NBIO register map (blocked/allowed regions)
- IOCTL dispatch with WDM driver
- PSP v11 initialization flow

## Source of Truth

`docs/tikslas.txt` — original spec in Lithuanian with driver skeleton code.
