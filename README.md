# AMD BC-250 PSP Windows Driver

Windows kernel-mode (WDM) diagnostic driver for the AMD BC-250 Platform Security Processor (PSP).
Companion to the [AMD BC-250 GPU Driver](https://github.com/Keshas-dev/AMD-BC-250-Windows-Driver).

Provides low-level PSP/SMU access for register diagnostics, firmware loading, and hardware
exploration on the AMD BC-250 (Cyan Skillfish). Uses the same `AMD-BC250-Signer` test cert
as the GPU driver — both coexist on the same system.

**Current status (2026-09-23):** **BAR2 auto-init fix built** (`pspdriver.sys` SHA256 `D8DC9423…01FE5`) — reinstall via Device Manager. Previous proxy-only build was hardware-verified alongside GPU `4.3.0.11` (no 0x1E, all init/ring/SMU PASS).

### BAR2 auto-init fix (2026-09-23) — pending reinstall
On this unit **PCI BAR0 = 0**; the pa_v1 1MB window is **BAR2 @ 0x18 = `0xFE700000`** (Linux binds BAR2 too). Old driver:
1. `#define PCIConfiguration 0` → `HalGetBusDataByOffset` read **CMOS**, not PCI (never found `1022:143E`)
2. Wrong slot encoding `(dev<<5)|func` → missed **B1.D0.F2**
3. BAR0=0 → mapped dead **`0xFD600000`** → auto-init reads all `0xFF`

**Now:** real `PCIConfiguration` + `(func<<5)|device` + BAR2 fallback + no `0xFD600000`. Also: no NBIO raw Bar0 write (proxy/ready only), alias-aware unload, INIT_HW fail-path clears stale maps. Code Reviewer PASS.

**After install:** `output\test-psp-driver.exe -s` once — expect live BAR0/`0xFE700000` map, pa_v1 regs readable (bootloader `0x001C0102`).

## GitHub

- **PSP Driver**: https://github.com/Keshas-dev/AMD-BC-250-PSP-Windows-Driver
- **GPU Driver**: https://github.com/Keshas-dev/AMD-BC-250-Windows-Driver

## Coexistence verification (2026-09-23)

| Layer | Result |
|-------|--------|
| GPU INIT NBIO_MAP | `INIT OK`, GPU_ID=`0x9FFF9700`, process exits |
| GPU INIT full Flags=0 | `SUCCESS — no TDR`, **deadlock fixed** (GPU repo) |
| PSP status (`-s`) | Alive YES, C2PMSG_64=`0x80000000`, C2PMSG_81=`0x002C7A89` |
| PSP GPU bridge | Own BAR0 `MmioVA=0xF8934000`, **no dual-map of 0xFE800000** |
| NBIO SIGs (boot) | SIG1=`0xFEDCBAEF` SIG2=`0xFEDCBADF` present |
| SPI_PG | **0 (gated)** — SOS-locked on Windows (expected) |
| SMU | 88.6.0, 1500MHz, Features `0xDD602C7D`, **16/16 messages OK** |
| PSP GPCOM ring | RING_INIT Result=1, GET_FW_ATTESTATION SUCCESS, WPTR advances |
| 0x1E BSOD | **None** this session |

## Capabilities

- **PSP BAR0 MMIO** `0xFE700000` via `MmMapIoSpace` + `PspEnablePciMemory()` (Command bit1 on 1022:143E)
- **GPU BAR5 access via proxy only** (`0x900`/`0x901`) — never dual-maps `0xFE800000`
- **SMU v88.6.0 mailbox** via SMN (NBIO 0x38/0x3C path) — frequency control, feature enable/disable
- **PSP C2PMSG mailbox** (0x58000 base) via proxy — firmware load paths
- **GC/MMHUB/HDP/NBIO/DF register access** at corrected BC-250 offsets (through proxy)
- **IOCTL interface** — register R/W, firmware load, SMU messages, KIQ submit

### Latest Fix: GPU BAR5 dual-map removed (2026-09-23) — VERIFIED

**Root cause of 0x1E BSOD:** `IOCTL_PSP_INIT_HW` with PA `0xFE800000` created a second `MmMapIoSpace` of GPU BAR5 (`g_Bar5Mapping`), bypassing the GPU driver's `DeviceMutex`. A/B test: PSP installed → 4× 0x1E; PSP removed → clean FurMark/`spi-pg-nbio-test`.

**Fix:**
1. PSP never maps GPU BAR5; all GPU MMIO = proxy `0x900`/`0x901`
2. GPU driver wraps proxy cases in `ExAcquireFastMutex(DeviceMutex)`
3. Legacy dual-map released under `g_Bar5MappingLock` on auto-init
4. Mailbox macros, NBIO unlock, BOOT_SEQ, GET_STATUS, GET_GPU_INFO, REG_PROG, LOAD_TOC, READ/WRITE_REG all proxy-routed

**Also fixed (GPU side):** INIT_HARDWARE full-init deadlock (mutex held across PSP `GET_GPU_INFO` → `0x900` re-acquire) — see GPU `README.md` / `AGENTS.md` (2026-09-23).

**PSP still owns:** PCI enable on `1022:143E`, its own BAR0, future **pa_v1 platform mailbox (C2PMSG_28..30)** unlock path (Linux `pspv_bc250`; TEE ring not required).

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

### Installed build changes (09/22/2026, PspDriver.sys 1 622 376 B)

| Change | Was → Now |
|--------|-----------|
| Deferred BAR mapping | DriverEntry mapped hardcoded `0xFD600000` (BIOS moves BAR each boot → random memory); now `Bar0Base=NULL`, real BAR mapped in `INIT_HW` |
| PCI memory enable | New `PspEnablePciMemory()`: scan `1022:143E`, Command bit1 ON before `MmMapIoSpace` (else windows read `0xFFFFFFFF`); reads real BAR0 = `0xFE700000` |
| Boot sequence skip | `PspDoBootSequence()` skips if MMIO not up (SOS already alive from VBIOS). Was: 5s timeouts every boot at DriverEntry |
| Blind ECAM removed | Was mapping `0xE0000000`/`0xC0000000`/… — `0xC0000000` is GPU VRAM aperture → garbage PCI_READ |
| PCI slot encoding | `(slot<<3)\|func` → `(slot<<5)\|func` (correct HAL encoding) |
| C2PMSG offsets | `0x1056C` family → **`0x58000` base** (verified by psp-ring tests) |
| NULL guards | NBIO unlock / GET_STATUS no longer write through NULL `MmioBase` |
| INF | `StartType=0` (BOOT), `ErrorControl=0`, `LoadOrderGroup=Cryptography`, class SecurityDevices, `PnpLockdown=1` |


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

- **Visual Studio 2022** + **WDK 10.0.26100.0** (auto-detected; **F:** on this host)
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

### GPU Driver Proxy IOCTLs (required for all GPU BAR5 access)

PSP **must not** map GPU BAR5 itself. Use these GPU driver proxy IOCTLs:

| IOCTL | Code | Description |
|-------|------|-------------|
| `IOCTL_AMDBC250_BAR5_READ_PROXY` | 0x900 | Read BAR5 register via GPU driver |
| `IOCTL_AMDBC250_BAR5_WRITE_PROXY` | 0x901 | Write BAR5 register via GPU driver |

Windows 11 26100: install **GPU driver first** (maps BAR5), then PSP (`ZwCreateFile` → `DeviceIoControl`).

## Architecture (2026-09-23): two devices, no BAR5 dual-map

**Critical fix:** the PSP driver must **never** `MmMapIoSpace(0xFE800000)`. GPU BAR5 belongs to `atikmdag.sys`. Dual-mapping raced the GPU driver and caused **bugcheck 0x1E** (A/B confirmed: 4 crashes with PSP installed, clean after uninstall).

```
PCI 1002:13FE  GPU BAR5  0xFE800000   ← GPU driver ONLY (DeviceMutex)
PCI 1022:143E  PSP BAR0  0xFE700000   ← PspDriver (own window, pa_v1 future unlock)
                     │
PSP GPU reg access ──┴──► GPU proxy raw IOCTLs 0x900 / 0x901
                          on \Device\AMDBC250DreamV43
                          (serialized by GPU DeviceMutex)
```

| Role | Owner | Notes |
|------|-------|-------|
| GPU registers / rings / display | **GPU driver** | Own BAR5 map + `DeviceMutex` |
| PSP BAR0 + platform mailbox | **PSP driver** | Linux `pspv_bc250` / pa_v1 C2PMSG_28..30 |
| C2PMSG on GPU BAR5 (0x58000 base) | via **proxy only** | Never direct map |
| NBIO sigs 0xC100/0xC180 | via **proxy** when GPU up | Dual-write was a conflict source |

Install order: **GPU first**, then PSP. GPU proxy IOCTLs (raw, not `CTL_CODE`):

| IOCTL | Code | Description |
|-------|------|-------------|
| `IOCTL_AMDBC250_BAR5_READ_PROXY` | `0x900` | Read GPU BAR5 register |
| `IOCTL_AMDBC250_BAR5_WRITE_PROXY` | `0x901` | Write GPU BAR5 register |

```
User Mode                    Kernel Mode (WDM)
-----------                  -----------------
test-psp-driver.exe  ---->   PspDriver.sys
DeviceIoControl              ├─ DriverEntry (IoCreateDevice)
                             ├─ IOCTL dispatch
                             │   ├─ INIT_HW → PSP BAR0 only + PspGpuProxyInit
                             │   │   └─ NEVER maps 0xFE800000
                             │   ├─ READ_REG / WRITE_REG / mailbox / NBIO / ring
                             │   │   └─ GPU offsets → PspGpuProxy*(0x900/0x901)
                             │   └─ Legacy GpuMmioBase unmapped if present
                             └─ DriverUnload

GPU Driver (atikmdag.sys)
  ├─ Own BAR5 MmioVirtualBase + DeviceMutex
  └─ cases 0x900/0x901 ── under DeviceMutex ──► BAR5 MMIO
```

## Current Status

### What Works (verified 2026-09-23)
- ✅ **Both drivers coexist** — GPU 4.3.0.11 + PSP 3.0.0.4, **no 0x1E BSOD**
- ✅ **PSP driver loads, PSP BAR0 maps**, SOS status via proxy (C2PMSG_81 via `0x900`)
- ✅ **No GPU BAR5 dual-map** — proxy-only path implemented and tested
- ✅ **GPU INIT deadlock fixed** — full-init Flags=0 returns SUCCESS, process exits (GPU repo fix)
- ✅ **SMU v88.6.0 mailbox via SMN** — TestMessage, GetSmuVersion, GetEnabledSmuFeatures, ForceGfxFreq, **16/16 whitelist**
- ✅ **Governor sequence safe** — Q3 temp → Q0 unforce → Q3 profile → Q0 force VID → Q0 force freq
- ✅ **Frequency control** (1500→1166 MHz) — SMU accepts freq/voltage changes
- ✅ **Feature enable/disable** via SMU Q2 (GFXOFF, CG, PG — all disableable)
- ✅ **GC/MMHUB/HDP/NBIO/DF register access** at corrected BC-250 offsets (via proxy)
- ✅ **PSP mailbox firmware loading** — RLC, MEC, ME, PFP, CE, SDMA all load OK
- ✅ **PSP GPCOM ring** — RING_INIT + GET_FW_ATTESTATION SUCCESS + WPTR advances
- ✅ **IRP_MJ_DEVICE_CONTROL** — all 30+ IOCTL handlers operational
- ✅ **GPU driver proxy bridge** — raw `0x900`/`0x901`, GPU `DeviceMutex`-serialized
- ✅ **Both drivers digitally signed** — Inf2Cat .cat generation fixed (x86 path)
- ✅ **All code review bugs fixed** — IP FW load, ring size cap, proxy return checks, spinlock races, SMU protocol

### What Doesn't Work
- ❌ **Compute/GFX execution** — WGPs SOS-locked (SPI_PG=0 on Windows init order)
- ❌ **GPCOM/TOS ring protocol** — SOS doesn't support ring-based commands (GPU-side ring works for attestation/TMR/IP-FW SMU)
- ❌ **KIQ ring processing** — KIQ_SIZE/KIQ_BASE hardwired to 0
- ❌ **DCN display output** — timing registers read-only (DMCUB FW not loaded)
- ❌ **Mailbox-based PROG_REG** — PSP accepts command, write silently ignored
- ❌ **NBIO re-unlock after boot** — gle=31 (SIGs already present from boot; re-issue fails, expected)

### Register Access Ranges
| Block | BAR5 Offset | Access | Notes |
|-------|-------------|--------|-------|
| GPU_ID | 0x0000 | Read | 0x9FFF9700 |
| HDP | 0x05A0+ | R/W | Memory coherency |
| GC | 0x3260-0x3FFF | R/W | GC_BASE=0x1260 shifted |
| MMHUB | 0x5000+ | R/W | Memory management |
| NBIO | 0xC100+ | R/W | PCIe config |
| DF | 0x1A000+ | Read | Data Fabric |
| PSP | BAR5 `0x58000+` (via proxy) | R/W | C2PMSG mailbox (0x58000 base) |

## Related Projects

- [AMD BC-250 Windows GPU Driver](https://github.com/Keshas-dev/AMD-BC-250-Windows-Driver) — Main GPU driver project
- [AMD BC-250 PSP Windows Driver](https://github.com/Keshas-dev/AMD-BC-250-PSP-Windows-Driver) — Companion PSP driver repository

## License

Educational purposes. Use at your own risk.

## "If you need a tool and nobody has built it yet, then build it yourself."
