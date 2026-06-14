# AMD BC-250 PSP Windows Driver

> "If you need a tool and nobody has built it yet, then build it yourself."

Windows kernel-mode (WDM) diagnostic driver for the AMD BC-250 Platform Security Processor (PSP).
Companion to the [AMD BC-250 GPU Driver](https://github.com/Keshas-dev/AMD-BC-250-Windows-Driver).

This driver provides low-level PSP access for register diagnostics, firmware analysis,
and hardware exploration on the AMD BC-250 (Cyan Skillfish) platform. Designed to coexist
alongside the GPU driver — both use the same certificate and signing infrastructure.

## GitHub Repository

This project is part of the [AMD BC-250 Windows Driver collection](https://github.com/Keshas-dev/AMD-BC-250-Windows-Driver) — GPU driver and PSP driver repositories work together.

The PSP driver repository is located at:
- **GPU Driver**: https://github.com/Keshas-dev/AMD-BC-250-Windows-Driver
- **PSP Driver**: https://github.com/Keshas-dev/AMD-BC-250-PSP-Windows-Driver

## Overview

This driver provides low-level access to the AMD BC-250 PSP hardware via:
- **BAR5 MMIO register mapping** — Direct hardware register access through the PSP's view of GPU registers
- **Mailbox interface** (C2PMSG) — Communication with PSP firmware for boot and control
- **IOCTL interface** — User-mode applications can read/write registers, load firmware, and probe hardware
- **PSP proxy bridge** — Supplies register access to the GPU driver when direct GPU MMIO is unavailable

## Hardware

- **Device**: `PCI\VEN_1022&DEV_143E` (AMD PSP)
- **Platform**: AMD BC-250 (Cyan Skillfish, codename "ROBIN")
- **GPU**: AMD BC-250 [1002:13FE] — non-standard register map vs Navi10
- **BIOS**: Version 5.00 (`BC250_5.00_clv.bin`) / P3.00 (`BC250_3.00.ROM`)
- **Architecture**: WDM (native NT kernel driver)

## Companion Project: SMU v11.8 Discoveries

The GPU driver repo contains the full SMU v11.8 analysis. Key findings:
- **MP1_BASE** = 0x16000 (byte offset in BAR5) — SMU mailbox registers
- **C2PMSG_66/82/90** at 0x16A08/0x16A48/0x16A68 — corrected offsets (was Navi10 0x16104+)
- **THM_BASE** = 0x16600 — thermal sensor registers (was 0x8000)
- **Protocol**: clear C2PMSG_90 → arg to C2PMSG_82 → msg to C2PMSG_66 → poll C2PMSG_90
- **BC-250 SMU v11.8 is minimal**: no PowerUpGfx, no EnableDpmFeature, no SetFanSpeedPercent
- **Use RequestActiveWgp (0x18)** to power up WGPs, SetCoreEnableMask (0x2C) for CU enable

See [GPU driver README](https://github.com/Keshas-dev/AMD-BC-250-Windows-Driver) for details.

The PSP driver repository can also be found at: https://github.com/Keshas-dev/AMD-BC-250-PSP-Windows-Driver

## Critical Discovery: BC-250 Register Map

**BC-250 (Cyan Skillfish) does NOT use the standard Navi10 register layout.**

All hardware registers in the 0x2000-0x2FFF range on BC-250 are shifted by `GC_BASE = 0x1260` bytes.
Standard Navi10 register offsets must be adjusted:

```
BAR5_offset = 0x1260 + Navi10_offset
```

This means:
- `CC_GC_SHADER_ARRAY_CONFIG` at Navi10 offset 0x2004 → BC-250 offset **0x3264**
- `GRBM_STATUS` at Navi10 offset 0x2000 → BC-250 offset **0x3260**
- `SPI_PG_ENABLE_STATIC_WGP_MASK` at Navi10 offset 0x229C → BC-250 offset **0x34FC**

### Corrected Register Offsets

| Register | Navi10 Offset | BC-250 BAR5 Offset |
|----------|--------------|-------------------|
| GRBM_STATUS | 0x2000 | **0x3260** |
| CC_GC_SHADER_ARRAY_CONFIG | 0x2004 | **0x3264** |
| GRBM_STATUS2 | 0x2008 | **0x3268** |
| GRBM_SOFT_RESET | 0x200C | **0x326C** |
| SPI_PG_ENABLE_STATIC_WGP_MASK | 0x229C | **0x34FC** |
| RLC_PG_ALWAYS_ON_WGP_MASK | 0x2B04 | **0x3D64** |
| CP scratch registers | 0x2074+ | **0x32D4+** |
| SDMA registers | 0x2600+ | **0x3860+** |

### Impact

All previous `0xFFFFFFFF` reads at offsets 0x2000-0x2FFF were caused by addressing unmapped
BAR5 space — **NOT by NBIO firewall blocking**. The NBIO on BC-250 does NOT block GC/GRBM/SDMA
registers at the corrected offsets. This was confirmed via Linux devmem reads on the actual hardware.

Register reads at the corrected offsets return valid values with no system instability.

## Repository Structure

```
├── src/
│   ├── driver/          # Kernel driver source
│   │   └── PspDriver.c
│   └── test/            # User-mode test tools
│       └── test-psp-driver.c
├── inc/                 # Shared headers
│   ├── PspIoctl.h
│   └── firmware_data.h
├── inf/                 # Device installation files
│   └── PspDriver.inf
├── scripts/             # Build + setup scripts
│   ├── build.bat        # Driver build + sign
│   ├── compile-test.bat # Test tool compiler
│   ├── enable-testsigning.cmd
│   ├── install-driver.cmd
│   └── uninstall-driver.cmd
├── docs/
│   ├── tikslas.txt
│   └── AGENTS.md
├── README.md
└── .gitignore
```

## Prerequisites

- **Visual Studio 2022** (Pro or Community) with "Desktop development with C++" workload
- **Windows Driver Kit (WDK) 10.0.26100.0** (Windows 11 24H2 SDK)
- **Windows 11 SDK** (included with VS2022)
- **E: drive** required — build scripts hardcode paths to `E:\VS2022\...` and `E:\Win11_WDK\...`
- Test signing enabled (see below)

> **Note**: If your VS/WDK is installed on C:, edit `scripts\build.bat` to update the paths before building.

### Signing Requirements

Windows 10/11 requires drivers to be **digitally signed** to load. This project uses test certificates
for development. Secure Boot must be **OFF** in BIOS.

**Step 1: Enable Test Mode**
```cmd
bcdedit /set testsigning on
```
Or use the helper script:
```cmd
scripts\enable-testsigning.cmd
```
**REBOOT required.** After reboot, you should see "Test Mode" watermark.

**Step 2: Build + Sign**
```cmd
cd scripts
build.bat
```
The build script creates a test certificate (`AMD-BC250-Signer`) and signs the driver.
Install the certificate to `LocalMachine\Root` and `LocalMachine\TrustedPublisher` if prompted.

**Production**: For production use, drivers must be WHQL-signed or have an EV certificate.

## Building

### Driver
```cmd
cd scripts
build.bat
```
Output: `output\PspDriver.sys`, `PspDriver.inf`, `PspDriver.cat`

### Test Tool
```cmd
cd scripts
compile-test.bat
```
Output: `output\test-psp-driver.exe`

## Installation

**Important**: Uninstall any previous version first, then reboot before installing.

**Option 1: Automated (pnputil)**
```cmd
scripts\install-driver.cmd
```

**Option 2: Manual via Device Manager**
1. Build the driver
2. Open Device Manager
3. Find "AMD BC-250 PSP" device (or unknown device with `PCI\VEN_1022&DEV_143E`)
4. Update Driver → Browse → `output\`
5. Reboot

**Uninstall:**
```cmd
scripts\uninstall-driver.cmd
```
Then reboot.

## Testing

```cmd
# Hardware init (map BAR5 MMIO at 0xFE800000, 2MB)
output\test-psp-driver.exe -i 0xFE800000 0x200000

# Connectivity test (reads C2PMSG mailbox registers)
output\test-psp-driver.exe -t

# Load firmware (persistent buffer, not freed after load)
output\test-psp-driver.exe -f cyan_skillfish2_sos_extracted.bin

# Two-stage PSP boot (embedded FW + SYSDRV + SOS)
output\test-psp-driver.exe -B

# Create PSP GPCOM ring (psp_v11_0_8 protocol)
output\test-psp-driver.exe -R

# Load GPU firmware via PSP ring (8 files)
output\test-psp-driver.exe -A

# Or load single GPU firmware
output\test-psp-driver.exe -L 9 cyan_skillfish2_sdma.bin

# NBIO unlock + signature registers
output\test-psp-driver.exe -U

# Full status snapshot
output\test-psp-driver.exe -s

# Read any register (use corrected BC-250 offsets)
output\test-psp-driver.exe -r 0x3264

# Quick help
output\test-psp-driver.exe
```

### Full GPU bring-up sequence

```cmd
cd output
test-psp-driver.exe -i 0xFE800000 0x200000   # Init BAR5
test-psp-driver.exe -B                        # Load SYSDRV + SOS
test-psp-driver.exe -R                        # Create GPCOM ring
test-psp-driver.exe -A                        # Load all GPU FW (CE, PFP, ME, MEC, MEC2, RLC, SDMA0, SDMA1)
```

## IOCTL Interface

See `inc/PspIoctl.h` for full definitions:

| IOCTL | Code | Description |
|-------|------|-------------|
| `PSP_INIT_HW` | 0x803 | Map BAR5 MMIO (physical address + size) |
| `PSP_READ_REG` | 0x800 | Read register at offset |
| `PSP_WRITE_REG` | 0x801 | Write value to register |
| `PSP_LOAD_FW` | 0x802 | Load firmware blob (persistent buffer) |
| `PSP_SEND_CMD` | 0x805 | Send mailbox command (0x4=SYSDRV, 0x8=SOS) |
| `PSP_NBIO_UNLOCK` | 0x804 | Write NBIO signature registers |
| `PSP_CREATE_RING` | 0x806 | Create PSP ring buffer (GPCOM/KM) |
| `PSP_NBIO_VIA_RING` | 0x807 | NBIO unlock via C2PMSG_64 |
| `PSP_GET_STATUS` | 0x808 | Full PSP status snapshot |
| `PSP_LOAD_EMBEDDED_FW` | 0x809 | Load compiled-in SYSDRV+SOS firmware |
| `PSP_BOOT_SEQUENCE` | 0x810 | Automated boot: FW alloc + CMD 0x4 + CMD 0x8 |
| `PSP_PCI_READ` | 0x811 | PCI config read |
| `PSP_PCI_WRITE` | 0x812 | PCI config write |
| `PSP_PROBE` | 0x813 | Comprehensive HW probe |
| `PSP_RING_LOAD_IP_FW` | 0x814 | Load GPU IP firmware via PSP ring buffer |
| `PSP_GET_GPU_INFO` | 0x815 | Bridge info for GPU driver |
| `PSP_REG_PROG` | 0x816 | Program register via ring |
| `PSP_AUTOLOAD_RLC` | 0x817 | Trigger RLC autoload |
| `PSP_KIQ_SUBMIT` | 0x818 | KIQ ring submit (TODO) |
| `PSP_INIT_TMR` | 0x819 | Init Trusted Memory Region |

## Architecture

```
User Mode                    Kernel Mode (WDM)
-----------                  -----------------
test-psp-driver.exe  ---->   PspDriver.sys
DeviceIoControl              ├─ DriverEntry (IoCreateDevice)
                             ├─ IOCTL dispatch
                             │   ├─ INIT_HW (MmMapIoSpace BAR5 0xFE800000)
                             │   ├─ READ_REG / WRITE_REG (direct BAR5 MMIO)
                             │   ├─ Mailbox C2PMSG (SYSDRV/SOS boot)
                             │   ├─ NBIO unlock signature registers
                             │   └─ Ring protocol (GPCOM, RBI)
                             ├─ PSP proxy bridge (for GPU driver)
                             │   ├─ GET_GPU_INFO → bridge info
                             │   └─ REG_PROG via ring or mailbox
                             └─ DriverUnload (free buffer + MMIO)

GPU Driver (atikmdag.sys)
  └─ Amdbc250PspProxy  ────> \\.\AmdBcPsp
      ├─ PSP_READ_REG       Direct BAR5 MMIO reads via PSP driver
      ├─ PSP_WRITE_REG      Direct BAR5 MMIO writes via PSP driver
      ├─ PSP_GET_GPU_INFO   Ring buffer PA, SOS status
      └─ PSP_REG_PROG       Register programming (via ring or mailbox)
```

## Current Status

### What Works
- **PSP driver loads and boots SOS** — SOS pre-loaded by BIOS, mailbox commands work
- **Register reads/writes** at all offsets via BAR5 MMIO (GC, MMHUB, HDP, NBIO, DF)
- **NBIO unlock** — signature registers written successfully
- **Mailbox commands** — CMD 0x4 (SYSDRV) and CMD 0x8 (SOS) work
- **PSP proxy bridge** — GPU driver can read/write registers through PSP driver
- **GC registers at corrected offsets** — 0x3260, 0x3264, 0x34FC all return valid values

### What Doesn't Work
- **GPCOM ring creation** — SOS firmware does not support TOS ring protocol (C2PMSG_64 bit 31 never sets)
- **GPU firmware loading via ring** — blocked by ring protocol not being supported
- **TMR init** — requires ring protocol
- **Mailbox-based PROG_REG** — PSP accepts command but write is silently ignored by SOS

### Register Access (BC-250 Corrected Offsets)
| Block | BAR5 Offset | Access | Notes |
|-------|-------------|--------|-------|
| GPU_ID | 0x0000 | Read | Returns 0x9FFF9700 |
| HDP | 0x05A0+ | Read/Write | Memory coherency |
| GC | 0x3260-0x3FFF | Read/Write | Shifted by GC_BASE=0x1260 |
| MMHUB | 0x5000+ | Read/Write | Memory management |
| NBIO | 0xC100+ | Read/Write | Control registers |
| DF | 0x1A000+ | Read | Data Fabric |
| PSP mailbox | 0x1056C+ | Read/Write | C2PMSG registers (PSP driver only) |

> **NBIO does NOT block GC/GRBM/SDMA registers at corrected offsets.** This was confirmed
> via Linux devmem tests. All previous 0xFFFFFFFF reads were caused by using wrong offsets.

## Related Projects

- [AMD BC-250 Windows GPU Driver](https://github.com/Keshas-dev/AMD-BC-250-Windows-Driver) — Main GPU driver project
- [AMD BC-250 PSP Windows Driver](https://github.com/Keshas-dev/AMD-BC-250-PSP-Windows-Driver) — Companion PSP driver repository

## License

Educational purposes. Use at your own risk.

## "If you need a tool and nobody has built it yet, then build it yourself."
