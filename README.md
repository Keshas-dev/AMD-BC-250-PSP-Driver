# AMD BC-250 PSP Windows Driver

> "If you need a tool and nobody has built it yet, then build it yourself."

Windows kernel-mode (WDM) diagnostic driver for the AMD BC-250 Platform Security Processor (PSP).
Companion to the [AMD BC-250 GPU Driver](https://github.com/Keshas-dev/AMD-BC-250-Windows-Driver).

This driver provides low-level PSP access required by the GPU driver for NBIO firewall management,
firmware loading, and hardware register diagnostics on the ASRock BC-250 platform (AMD Oberon SoC).
Designed to coexist alongside the GPU driver — both use the same certificate and signing infrastructure.

## Overview

This driver provides low-level access to the AMD BC-250 PSP hardware via:
- **BAR0 MMIO register mapping** - Direct hardware register access
- **Mailbox interface** (C2PMSG) - Communication with PSP firmware
- **IOCTL interface** - User-mode applications can read/write registers and load firmware

## Hardware

- **Device**: PCI\VEN_1022&DEV_143E (AMD PSP)
- **Platform**: AMD BC-250 (PS5 Oberon variant)
- **BIOS**: Version 5.00 (`BC250_5.00_clv.bin`)
- **Architecture**: WDM (native NT)

## Repository Structure

```
├── src/
│   ├── driver/          # Kernel driver source
│   │   └── PspDriver.c
│   └── test/            # User-mode test tools
│       └── test-psp-driver.c
├── inc/                 # Shared headers
│   └── PspIoctl.h
├── inf/                 # Device installation files
│   └── PspDriver.inf
├── scripts/             # Build + setup scripts
│   ├── build.bat        # Driver build + sign
│   ├── compile-test.bat # Test tool compiler
│   ├── enable-testsigning.cmd  # Enable Windows Test Mode
│   ├── install-driver.cmd      # Install driver via pnputil
│   └── uninstall-driver.cmd    # Remove driver
├── docs/                # Documentation
│   ├── tikslas.txt      # Design specification (LT)
│   └── AGENTS.md        # Developer notes
├── README.md
└── .gitignore
```

## Prerequisites

- Visual Studio 2022 (with "Desktop development with C++" workload)
- Windows 11 SDK + Windows Driver Kit (WDK)
- Test signing enabled (see [Signing Requirements](#signing-requirements))

### Signing Requirements

Windows 10/11 requires drivers to be **digitally signed** to load. This project uses test certificates for development.

**Step 1: Enable Test Mode**
```cmd
scripts\enable-testsigning.cmd
```
Or manually (as Administrator):
```cmd
bcdedit /set testsigning on
```
**REBOOT required.** After reboot, you should see "Test Mode" watermark.

**Step 2: Build + Sign**
```cmd
cd scripts
build.bat
```
The build script automatically creates a test certificate and signs the driver. If signing fails, the driver can still be loaded with Test Mode enabled.

**Production**: For production use, drivers must be WHQL-signed or have an EV certificate from Microsoft-approved CA.

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

**Option 1: Automated (pnputil)**
```cmd
scripts\install-driver.cmd
```

**Option 2: Manual via Device Manager**
1. Build the driver (see above)
2. Open Device Manager
3. Find "AMD BC-250 PSP" device (or unknown device with PCI\VEN_1022&DEV_143E)
4. Update Driver → Browse → `output\`
5. Reboot if prompted

**Uninstall:**
```cmd
scripts\uninstall-driver.cmd
```

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

# Read any register
output\test-psp-driver.exe -r 0xC100

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
| `PSP_CREATE_RING` | 0x806 | Create PSP ring buffer (GPCOM/KM, psp_v11_0_8) |
| `PSP_NBIO_VIA_RING` | 0x807 | NBIO unlock + sigs via C2PMSG_64 |
| `PSP_GET_STATUS` | 0x808 | Full PSP status snapshot |
| `PSP_LOAD_EMBEDDED_FW` | 0x809 | Load compiled-in SYSDRV+SOS firmware |
| `PSP_BOOT_SEQUENCE` | 0x810 | Automated boot: FW alloc + CMD 0x4 + CMD 0x8 + GRBM |
| `PSP_RING_LOAD_IP_FW` | 0x814 | Load GPU IP firmware via PSP ring buffer |

## Architecture

```
User Mode                    Kernel Mode (WDM)
-----------                  -----------------
test-psp-driver.exe  ---->   PspDriver.sys
DeviceIoControl              ├─ DriverEntry (IoCreateDevice)
                             ├─ IOCTL dispatch
                             │   ├─ INIT_HW (MmMapIoSpace BAR5 0xFE800000)
                             │   ├─ READ_REG / WRITE_REG
                             │   ├─ LOAD_FW (persistent contiguous buffer)
                             │   ├─ SEND_CMD (PSP mailbox C2PMSG_35/36)
                             │   └─ NBIO_UNLOCK
                             └─ DriverUnload (free buffer + MMIO)
```

## Related Projects

- [AMD BC-250 Windows GPU Driver](https://github.com/Keshas-dev/AMD-BC-250-Windows-Driver) - Main GPU driver project

## License

Educational purposes. Use at your own risk.

## "If you need a tool and nobody has built it yet, then build it yourself."
