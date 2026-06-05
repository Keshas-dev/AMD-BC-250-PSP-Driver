# AMD BC-250 PSP Windows Driver

Windows kernel-mode driver for AMD BC-250 Platform Security Processor (PSP) interface.

## Overview

This driver provides low-level access to the AMD BC-250 PSP hardware via:
- **BAR0 MMIO register mapping** - Direct hardware register access
- **Mailbox interface** (C2PMSG) - Communication with PSP firmware
- **IOCTL interface** - User-mode applications can read/write registers and load firmware

## Hardware

- **Device**: PCI\VEN_1022&DEV_143E (AMD PSP)
- **Platform**: AMD BC-250 (PS5 Oberon variant)
- **Architecture**: KMDF (Kernel-Mode Driver Framework)

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

# Two-stage PSP boot sequence (from sibling project PSP v11 code)
output\test-psp-driver.exe -C 0x00000004      # SYSDRV command
output\test-psp-driver.exe -C 0x00000008      # SOS command

# Check GRBM_STATUS after firmware load
output\test-psp-driver.exe -r 0x2004

# Read any register
output\test-psp-driver.exe -r 0xC100           # NBIO signature reg 1
output\test-psp-driver.exe -r 0x50D0           # MMHUB register
output\test-psp-driver.exe -w 0x1056C 0x1      # Write to register

# Quick help
output\test-psp-driver.exe
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

- [AMD BC-250 Windows GPU Driver](../AMD-BC-250-Windows-Driver-main) - Main GPU driver project

## License

Educational purposes. Use at your own risk.
