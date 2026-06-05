# Agent Notes: AMD BC-250 PSP Windows Driver

## What This Repo Is

This is a **new/separate project** for a PSP (Platform Security Processor) driver for AMD BC-250. It is NOT the main GPU driver project. The mature GPU driver lives in the sibling directory `../AMD-BC-250-Windows-Driver-main`.

This directory contains:
- `tikslas.txt` — design specification (in Lithuanian), the authoritative design document
- `src/driver/PspDriver.c` — KMDF driver source
- `inf/PspDriver.inf` — device installation file
- `inc/PspIoctl.h` — shared IOCTL definitions (driver + user-mode tools)
- `src/test/test-psp-driver.c` — user-mode test tool
- `scripts/build.bat` — compile + test-sign script
- `scripts/compile-test.bat` — compile test tool script
- `scripts/enable-testsigning.cmd` — enable Windows Test Mode
- `scripts/install-driver.cmd` — driver installer (pnputil)
- `scripts/uninstall-driver.cmd` — driver remover

## Architecture

- **Target hardware**: PCI device `VEN_1022&DEV_143E` (AMD PSP on BC-250)
- **Driver type**: KMDF (Kernel-Mode Driver Framework), NOT WDM
- **Interface**: IOCTL-based communication with user-mode test tools
- **MMIO**: Maps BAR0 for register access, uses Mailbox registers (C2PMSG) for firmware loading
- **Key source files**:
  - `PspDriver.c` — KMDF driver (DriverEntry, EvtDeviceAdd, IOCTL dispatch)
  - `PspDriver.inf` — Device installation file
  - `PspIoctl.h` — Shared IOCTL codes and structures
  - `test-psp-driver.c` — User-mode test tool
  - `build.bat` — Compile + test-sign script

## Build Requirements

- Visual Studio 2022 with "Desktop development with C++"
- Windows 11 SDK + Windows Driver Kit (WDK) with `ntddk.h` and WDF libraries
- Test signing enabled (see below)
- Build produces `PspDriver.sys`, `PspDriver.inf`, `PspDriver.cat`

## Digital Signature (CRITICAL)

Windows 10/11 **requires** kernel drivers to be digitally signed. This project uses test signing for development.

### Setup Steps (in order):

1. **Enable Test Mode** (run as Administrator, then reboot):
   ```cmd
   scripts\enable-testsigning.cmd
   ```
   Or manually: `bcdedit /set testsigning on`

2. **Build + Sign** (auto-generates test certificate):
   ```cmd
   cd scripts
   build.bat
   ```

3. **Install**:
   ```cmd
   scripts\install-driver.cmd
   ```

### Signing Details

- `build.bat` automatically creates a test certificate via PowerShell or makecert
- If auto-signing fails, driver can still load with Test Mode enabled
- Production drivers require WHQL certification or EV certificate from Microsoft-approved CA
- **Never disable Secure Boot** if you need it for other security features

## Key Technical Details

- **IOCTL codes** (defined in `PspIoctl.h`):
  - `IOCTL_PSP_READ_REG` (0x800) — read BAR0 register
  - `IOCTL_PSP_WRITE_REG` (0x801) — write BAR0 register
  - `IOCTL_PSP_LOAD_FW` (0x802) — firmware load via Mailbox (fully implemented)
- **Mailbox registers**: C2PMSG_35 (command, ~0x1056C), C2PMSG_36 (data, ~0x10570), C2PMSG_81 (status)
- **Device symlink**: `\\DosDevices\\AmdBcPsp`
- **User-mode access**: `\\Device\\AmdBcPsp`

### Firmware Loading Flow (IOCTL_PSP_LOAD_FW)

1. User-mode sends firmware blob via `DeviceIoControl`
2. Driver allocates contiguous physical memory (`MmAllocateContiguousMemory`, <4GB)
3. Copies firmware blob to contiguous buffer
4. Gets physical address (`MmGetPhysicalAddress`)
5. Writes PA to C2PMSG_36 (data register)
6. Writes command `0x1` to C2PMSG_35 (command register)
7. Polls C2PMSG_81 status register for 10 seconds (1ms intervals)
8. Returns status code to user-mode
9. Cleans up allocated memory (`MmFreeContiguousMemory`)

## Reference Sibling Project

For working build.bat patterns, signing scripts, IOCTL conventions, and hardware register access patterns, see:
- `../AMD-BC-250-Windows-Driver-main/build.bat`
- `../AMD-BC-250-Windows-Driver-main/src/kmd/`
- `../AMD-BC-250-Windows-Driver-main/inc/`

## Source of Truth

`tikslas.txt` contains the complete specification (in Lithuanian) including the driver C code skeleton, INF file template, and build.bat template. When implementing, treat it as the authoritative design document.
