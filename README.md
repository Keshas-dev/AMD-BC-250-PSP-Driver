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
├── scripts/             # Build scripts
│   ├── build.bat        # Driver build + sign
│   └── compile-test.bat # Test tool compiler
├── docs/                # Documentation
│   ├── tikslas.txt      # Design specification (LT)
│   └── AGENTS.md        # Developer notes
├── README.md
└── .gitignore
```

## Prerequisites

- Visual Studio 2022 (with "Desktop development with C++" workload)
- Windows 11 SDK + Windows Driver Kit (WDK)
- Test signing enabled:
  ```cmd
  bcdedit /set testsigning on
  ```
  (Reboot required)

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

1. Build the driver (see above)
2. Open Device Manager
3. Find "AMD BC-250 PSP" device (or unknown device with PCI\VEN_1022&DEV_143E)
4. Update Driver → Browse → `output\`
5. Reboot if prompted

## Testing

```cmd
# Read mailbox command register
output\test-psp-driver.exe -r 0x1056C

# Read mailbox status register
output\test-psp-driver.exe -m

# Run connectivity test
output\test-psp-driver.exe -t

# Write value to register
output\test-psp-driver.exe -w 0x1056C 0x1
```

## IOCTL Interface

See `inc/PspIoctl.h` for full definitions:

- `IOCTL_PSP_READ_REG` (0x800) - Read BAR0 register
- `IOCTL_PSP_WRITE_REG` (0x801) - Write BAR0 register  
- `IOCTL_PSP_LOAD_FW` (0x802) - Load firmware via Mailbox

## Architecture

```
User Mode                    Kernel Mode
-----------                  -------------
test-psp-driver.exe  ---->   PspDriver.sys
DeviceIoControl              ├─ DriverEntry
                             ├─ EvtDeviceAdd (PnP)
                             ├─ EvtDevicePrepareHardware (BAR0 map)
                             ├─ IOCTL dispatch
                             │   ├─ READ_REG
                             │   ├─ WRITE_REG
                             │   └─ LOAD_FW (Mailbox C2PMSG)
                             └─ EvtDeviceReleaseHardware
```

## Related Projects

- [AMD BC-250 Windows GPU Driver](../AMD-BC-250-Windows-Driver-main) - Main GPU driver project

## License

Educational purposes. Use at your own risk.
