# AMD BC-250 PSP Windows Driver

Windows kernel-mode (WDM) diagnostic driver for the AMD BC-250 Platform Security Processor (PSP).
Companion to the [AMD BC-250 GPU Driver](https://github.com/Keshas-dev/AMD-BC-250-Windows-Driver).

Provides low-level PSP/SMU access for register diagnostics, firmware loading, and hardware
exploration on the AMD BC-250 (Cyan Skillfish). Uses the same `AMD-BC250-Signer` test cert
as the GPU driver — both coexist on the same system.

## GitHub

- **PSP Driver**: https://github.com/Keshas-dev/AMD-BC-250-PSP-Windows-Driver
- **GPU Driver**: https://github.com/Keshas-dev/AMD-BC-250-Windows-Driver

## Capabilities

- **BAR5 MMIO mapping** via `MmMapIoSpace` (or GPU proxy fallback on Win11 26100)
- **SMU v88.6.0 mailbox** via SMN (NBIO 0x38/0x3C path) — frequency control, feature enable/disable
- **PSP C2PMSG mailbox** — SOS boot, firmware loading (RLC, MEC, SDMA, etc.)
- **GC/MMHUB/HDP/NBIO/DF register access** at corrected BC-250 offsets
- **IOCTL interface** — register R/W, firmware load, SMU messages, KIQ submit

## Latest Fix: Driver Signing (2026-07-08)

**Root cause**: `build.bat` searched only `x64\` for Inf2Cat, but WDK 10.0.26100.0 installs it in `x86\`:
```
E:\Program Files (x86)\Windows Kits\10\bin\10.0.26100.0\x86\Inf2Cat.exe
```
Without Inf2Cat, build fell back to `makecat.exe` which generates an incomplete catalog (1565 bytes vs
Inf2Cat's 4439 bytes). Windows rejected the System-class driver with "not digitally signed".

**Fix** (`build.bat`):
1. Search both `x86\` and `x64\` paths for Inf2Cat
2. Sign `.sys` FIRST, then generate `.cat` with Inf2Cat, then sign `.cat`
3. Fixed OS parameter from invalid `11_X64` to valid `10_X64`
4. Updated INF `DriverVer` to `07/08/2026,3.0.0.4`

## SMU v88.6.0 via SMN

SMU mailbox registers are NOT mapped into BAR5 on BC-250 (reads 0). Access via SMN using
NBIO's PCIE index/data registers at BAR5+0x38/0x3C:

| Register | SMN Address | Purpose |
|----------|-------------|---------|
| Queue 0 CMD | 0x03B10A08 | Freq/voltage control |
| Queue 0 RSP | 0x03B10A68 | Response |
| Queue 0 ARG | 0x03B10A48 | Parameter |
| Queue 2 CMD | 0x03B10528 | Feature enable/disable |
| Queue 2 ARG | 0x03B10998 | Feature mask |
| Queue 3 CMD | 0x03B10A20 | Temp, perf profile |
| Queue 3 ARG | 0x03B10A88 | Parameter |

**Key SMU messages (proven safe):**
- Q3 0x01 — TestMessage
- Q0 0x02 — GetSmuVersion (returns 0x00580600 = 88.6.0)
- Q0 0x3D — GetEnabledSmuFeatures (returns 0xDD602C7D)
- Q0 0x39 — ForceGfxFreq (MHz, requires voltage+profile set first)
- Q0 0x3B — ForceGfxVid
- Q2 0x06 — DisableSmuFeatures (mask: bit2=GFXOFF, bit3=CG, bit4=PG)

## Repository Structure

```
├── build.bat              # Build + sign driver (run from repo root)
├── inc/
│   ├── PspIoctl.h         # IOCTL definitions
│   └── firmware_data.h
├── inf/
│   └── PspDriver.inf      # Device installation
├── src/driver/
│   ├── PspDriver.c        # DriverEntry, IOCTL dispatch
│   ├── PspCore.c          # Mailbox, proxy bridge, firmware loading
│   ├── PspKiq.c           # KIQ ring management
│   └── PspSmu.c           # SMU v11.8 communication
├── scripts/
│   └── PspDriver.cdf      # makecat CDF (fallback if Inf2Cat unavailable)
├── docs/
│   └── AGENTS.md
└── README.md
```

## Prerequisites

- **Visual Studio 2022** + **WDK 10.0.26100.0** (auto-detected on C:, D:, or E: drive)
- Test signing: `bcdedit /set testsigning on`, Secure Boot OFF

## Building

### Driver
```cmd
build.bat
```
Output: `output\PspDriver.sys`, `output\PspDriver.inf`, `output\PspDriver.cat`, `output\firmware\*.bin`

Build signs .sys → Inf2Cat generates .cat → signs .cat, matching the GPU driver's build process.

## Installation

**Important**: Uninstall previous versions first (Device Manager → Uninstall with "Delete driver"), reboot.

**Option 1: Manual via Device Manager**
1. Build the driver
2. Device Manager → AMD BC-250 PSP → Update Driver → Browse → `output\`
3. Reboot

**Option 2: Automated**
```cmd
reinstall-psp-fix.bat
```

**Uninstall:**
Device Manager → AMD BC-250 PSP → Uninstall device (check "Delete driver software") → reboot.

## Testing (run from GPU repo)

The GPU driver repo contains all test tools. Build them there, then copy `output\*.exe` here or run from GPU repo:

```cmd
# PSP driver status + BAR5 mapping
cd C:\AMD-BC-250\AMD-BC-250-Windows-Driver-main
output\psp-status-test.exe

# SMU mailbox via SMN
output\bar5-smn-test.exe

# SMU telemetry monitor
output\smu-monitor.exe

# SMU frequency control (governor sequence)
output\governor-sequence.exe

# DCN display probe
output\dcn-init-test.exe
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
| `PSP_KIQ_SUBMIT` | 0x818 | KIQ ring submit |
| `PSP_INIT_TMR` | 0x819 | Init Trusted Memory Region |

### GPU Driver Proxy IOCTLs (Windows 11 26100)

When BAR5 mapping fails, the PSP driver uses these GPU driver proxy IOCTLs:

| IOCTL | Code | Description |
|-------|------|-------------|
| `IOCTL_AMDBC250_BAR5_READ_PROXY` | 0x900 | Read BAR5 register via GPU driver |
| `IOCTL_AMDBC250_BAR5_WRITE_PROXY` | 0x901 | Write BAR5 register via GPU driver |

## Architecture

```
User Mode                    Kernel Mode (WDM)
-----------                  -----------------
test-psp-driver.exe  ---->   PspDriver.sys
DeviceIoControl              ├─ DriverEntry (IoCreateDevice)
                             ├─ IOCTL dispatch
                             │   ├─ INIT_HW (MmMapIoSpace BAR5 0xFE800000)
                             │   │   └─ Falls back to GPU proxy if mapping fails
                             │   ├─ READ_REG / WRITE_REG (direct BAR5 MMIO or GPU proxy)
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

Windows 11 26100 GPU Proxy Fallback:
test-psp-driver.exe  ---->   PspDriver.sys  ---->   GPU Driver (atikmdag.sys)
                           (ZwCreateFile)      (BAR5 proxy IOCTLs)
                           (IOCTL_AMDBC250_*)    (MmioVirtualBase access)
```

## Current Status

### What Works
- ✅ **PSP driver loads, BAR5 maps**, SOS alive (C2PMSG_81=0xF0000010)
- ✅ **SMU v88.6.0 mailbox via SMN** — TestMessage, GetSmuVersion, GetEnabledSmuFeatures, ForceGfxFreq
- ✅ **Governor sequence safe** — Q3 temp → Q0 unforce → Q3 profile → Q0 force VID → Q0 force freq
- ✅ **Frequency control** (1500→1166 MHz) — SMU accepts freq/voltage changes
- ✅ **Feature enable/disable** via SMU Q2 (GFXOFF, CG, PG — all disableable)
- ✅ **GC/MMHUB/HDP/NBIO/DF register access** at corrected BC-250 offsets
- ✅ **PSP mailbox firmware loading** — RLC, MEC, ME, PFP, CE, SDMA all load OK
- ✅ **IRP_MJ_DEVICE_CONTROL** — all 30+ IOCTL handlers operational
- ✅ **GPU driver proxy bridge** for Win11 26100 fallback
- ✅ **Both drivers digitally signed** — Inf2Cat .cat generation fixed (x86 path)
- ✅ **All code review bugs fixed** — IP FW load, ring size cap, proxy return checks, spinlock races, SMU protocol

### What Doesn't Work
- ❌ **Compute/GFX execution** — WGPs permanently fused off (SPI_PG_MASK=RO 0)
- ❌ **GPCOM/TOS ring protocol** — SOS doesn't support ring-based commands
- ❌ **KIQ ring processing** — KIQ_BASE/KIQ_SIZE hardwired to 0
- ❌ **DCN display output** — timing registers read-only (DMCUB FW not loaded)
- ❌ **Mailbox-based PROG_REG** — PSP accepts command, write silently ignored

### Register Access Ranges
| Block | BAR5 Offset | Access | Notes |
|-------|-------------|--------|-------|
| GPU_ID | 0x0000 | Read | 0x9FFF9700 |
| HDP | 0x05A0+ | R/W | Memory coherency |
| GC | 0x3260-0x3FFF | R/W | GC_BASE=0x1260 shifted |
| MMHUB | 0x5000+ | R/W | Memory management |
| NBIO | 0xC100+ | R/W | PCIe config |
| DF | 0x1A000+ | Read | Data Fabric |
| PSP | 0x1056C+ | R/W | C2PMSG mailbox |

## Related Projects

- [AMD BC-250 Windows GPU Driver](https://github.com/Keshas-dev/AMD-BC-250-Windows-Driver) — Main GPU driver project
- [AMD BC-250 PSP Windows Driver](https://github.com/Keshas-dev/AMD-BC-250-PSP-Windows-Driver) — Companion PSP driver repository

## License

Educational purposes. Use at your own risk.

## "If you need a tool and nobody has built it yet, then build it yourself."
