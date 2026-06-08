#define _CRT_SECURE_NO_WARNINGS
#include <windows.h>
#include <stdio.h>
#include <stdarg.h>
#include <stdlib.h>
#include <string.h>

// Include shared IOCTL definitions
#include "PspIoctl.h"

// Forward declarations
BOOL RingAutoloadRlc(HANDLE hDevice);

static FILE *g_log = NULL;

void Log(const char *fmt, ...) {
    va_list a;
    va_start(a, fmt);
    vfprintf(stdout, fmt, a);
    va_end(a);
    if (g_log) {
        va_start(a, fmt);
        vfprintf(g_log, fmt, a);
        va_end(a);
        fflush(g_log);
    }
}

HANDLE OpenPspDriver() {
    HANDLE h = CreateFileW(
        PSP_DEVICE_NAME,
        GENERIC_READ | GENERIC_WRITE,
        0,
        NULL,
        OPEN_EXISTING,
        FILE_ATTRIBUTE_NORMAL,
        NULL
    );
    return h;
}

BOOL ReadRegister(HANDLE hDevice, ULONG offset, PULONG pValue)
{
    PSP_READ_REG_REQUEST req = { offset, 0 };
    PSP_READ_REG_RESPONSE resp = { 0, 0 };
    DWORD returned = 0;
    
    BOOL ok = DeviceIoControl(
        hDevice,
        IOCTL_PSP_READ_REG,
        &req, sizeof(req),
        &resp, sizeof(resp),
        &returned,
        NULL
    );
    
    if (ok && pValue) {
        *pValue = resp.Value;
    }
    return ok;
}

BOOL WriteRegister(HANDLE hDevice, ULONG offset, ULONG value)
{
    PSP_WRITE_REG_REQUEST req = { offset, value };
    PSP_WRITE_REG_RESPONSE resp = { 0, 0 };
    DWORD returned = 0;
    
    BOOL ok = DeviceIoControl(
        hDevice,
        IOCTL_PSP_WRITE_REG,
        &req, sizeof(req),
        &resp, sizeof(resp),
        &returned,
        NULL
    );
    
    return ok;
}

BOOL UnlockNbio(HANDLE hDevice)
{
    ULONG resp[3] = {0};
    DWORD returned = 0;

    Log("NBIO_UNLOCK...\n");

    BOOL ok = DeviceIoControl(
        hDevice,
        IOCTL_PSP_NBIO_UNLOCK,
        NULL, 0,
        &resp, sizeof(resp),
        &returned,
        NULL
    );

    if (ok) {
        Log("NBIO unlock: SIG1=0x%08X SIG2=0x%08X MMHUB=0x%08X\n", resp[0], resp[1], resp[2]);
        if (returned >= sizeof(ULONG) * 3) {
            Log("(write values returned, not verification - try GRBM_STATUS check)\n");
        }
    } else {
        Log("NBIO unlock FAILED (err=%lu)\n", GetLastError());
    }
    return ok;
}

BOOL CreateRing(HANDLE hDevice)
{
    ULONG resp[3] = {0};
    DWORD returned = 0;

    Log("CREATE_RING...\n");
    BOOL ok = DeviceIoControl(hDevice, IOCTL_PSP_CREATE_RING,
        NULL, 0, &resp, sizeof(resp), &returned, NULL);

    if (ok && returned >= sizeof(ULONG) * 3) {
        Log("Ring: PA=0x%08X C64pre=0x%08X C64post=0x%08X\n",
            resp[0], resp[1], resp[2]);
    } else if (ok && returned >= sizeof(ULONG) * 2) {
        Log("Ring: PA=0x%08X C2PMSG_64=0x%08X\n", resp[0], resp[1]);
    } else if (ok) {
        Log("Ring created (partial response)\n");
    } else {
        Log("CREATE_RING FAILED (err=%lu)\n", GetLastError());
    }
    return ok;
}

BOOL NbioViaRing(HANDLE hDevice)
{
    ULONG resp[4] = {0};
    DWORD returned = 0;

    Log("NBIO_VIA_RING...\n");
    BOOL ok = DeviceIoControl(hDevice, IOCTL_PSP_NBIO_VIA_RING,
        NULL, 0, &resp, sizeof(resp), &returned, NULL);

    if (ok && returned >= sizeof(ULONG) * 4) {
        Log("cmd=0x%08X C64post=0x%08X MMHUB=0x%08X GRBM=0x%08X%s\n",
            resp[0], resp[1], resp[2], resp[3],
            (resp[3] != 0xFFFFFFFF) ? " *** GRBM UNLOCKED ***" : " (BLOCKED)");
    } else if (ok && returned >= sizeof(ULONG) * 3) {
        Log("cmd=0x%08X C2PMSG_64=0x%08X MMHUB=0x%08X\n", resp[0], resp[1], resp[2]);
    } else if (ok) {
        Log("NBIO via ring (partial response)\n");
    } else {
        Log("NBIO_VIA_RING FAILED (err=%lu)\n", GetLastError());
    }
    return ok;
}

BOOL GetPspStatus(HANDLE hDevice)
{
    PSP_STATUS_INFO info = {0};
    DWORD returned = 0;

    BOOL ok = DeviceIoControl(hDevice, IOCTL_PSP_GET_STATUS,
        NULL, 0, &info, sizeof(info), &returned, NULL);

    if (ok && returned >= sizeof(PSP_STATUS_INFO)) {
        Log("=== PSP STATUS ===\n");
        Log("  C2PMSG_35=0x%08X  C2PMSG_36=0x%08X  C2PMSG_37=0x%08X\n", info.C2PMSG_35, info.C2PMSG_36, info.C2PMSG_37);
        Log("  C2PMSG_64=0x%08X  C2PMSG_81=0x%08X\n", info.C2PMSG_64, info.C2PMSG_81);
        Log("  PSP Alive: %s\n", info.PspAlive ? "YES" : "NO");
        Log("  FW Loaded: %s (%u bytes, PA>>20=0x%08X)\n", info.FwLoaded ? "YES" : "NO", info.FwSize, info.FwPaShifted);
        Log("  NBIO SIG1=0x%08X SIG2=0x%08X\n", info.NbioSig1, info.NbioSig2);
        Log("  GRBM_STATUS=0x%08X%s\n", info.GrbmStatus,
            (info.GrbmStatus != 0xFFFFFFFF) ? " *** UNLOCKED ***" : " (BLOCKED)");
        Log("  GC (0x3000)=0x%08X  HDP (0x05A0)=0x%08X  MMHUB (0x50D0)=0x%08X\n",
            info.GcCheck, info.HdpCheck, info.MmhubCheck);
        Log("  MMIO VA=0x%08X  Size=%u  Ring Created: %s\n", info.MmioVA, info.MmioSize,
            info.RingCreated ? "YES" : "NO");
    } else {
        Log("GET_STATUS FAILED (err=%lu)\n", GetLastError());
    }
    return ok;
}

BOOL PciRead(HANDLE hDevice, ULONG bus, ULONG devFn, ULONG offset)
{
    ULONG args[3] = { bus, devFn, offset };
    ULONG resp = 0;
    DWORD returned = 0;
    BOOL ok = DeviceIoControl(hDevice, IOCTL_PSP_PCI_READ,
        args, sizeof(args), &resp, sizeof(resp), &returned, NULL);
    if (ok) {
        Log("PCI_READ B%d.D%d.F%d[0x%X] = 0x%08X\n", bus, (devFn>>3)&0x1F, devFn&7, offset, resp);
    } else {
        Log("PCI_READ FAILED (err=%lu)\n", GetLastError());
    }
    return ok;
}

BOOL PciWrite(HANDLE hDevice, ULONG bus, ULONG devFn, ULONG offset, ULONG value)
{
    ULONG args[4] = { bus, devFn, offset, value };
    DWORD returned = 0;
    BOOL ok = DeviceIoControl(hDevice, IOCTL_PSP_PCI_WRITE,
        args, sizeof(args), NULL, 0, &returned, NULL);
    if (ok) {
        Log("PCI_WRITE B%d.D%d.F%d[0x%X] = 0x%08X\n", bus, (devFn>>3)&0x1F, devFn&7, offset, value);
    } else {
        Log("PCI_WRITE FAILED (err=%lu)\n", GetLastError());
    }
    return ok;
}

BOOL BootSequence(HANDLE hDevice)
{
    ULONG results[4] = {0};
    DWORD returned = 0;

    Log("BOOT_SEQUENCE (embedded FW + SYSDRV + SOS)...\n");
    BOOL ok = DeviceIoControl(hDevice, IOCTL_PSP_BOOT_SEQUENCE,
        NULL, 0, results, sizeof(results), &returned, NULL);

    if (ok && returned >= sizeof(results)) {
        Log("  FW PA>>20      = 0x%08X\n", results[0]);
        Log("  SYSDRV (0x4)   = %s\n", results[1] ? "SENT" : "FAIL");
        Log("  SOS    (0x8)   = %s\n", results[2] ? "SENT" : "FAIL");
        Log("  GRBM_STATUS    = 0x%08X%s\n", results[3],
            (results[3] != 0xFFFFFFFF) ? " *** NBIO UNLOCKED ***" : " (BLOCKED)");
    } else {
        Log("BOOT_SEQUENCE FAILED (err=%lu)\n", GetLastError());
    }
    return ok;
}

BOOL LoadEmbeddedFirmware(HANDLE hDevice)
{
    DWORD returned = 0;
    ULONG resp = 0;

    Log("LOAD_EMBEDDED_FW...\n");

    BOOL ok = DeviceIoControl(hDevice, IOCTL_PSP_LOAD_EMBEDDED_FW,
        NULL, 0, &resp, sizeof(resp), &returned, NULL);

    if (ok) {
        Log("Embedded FW loaded: PA>>20=0x%08X\n", resp);
    } else {
        Log("LOAD_EMBEDDED_FW FAILED (err=%lu)\n", GetLastError());
    }
    return ok;
}

BOOL ComprehensiveProbe(HANDLE hDevice)
{
    PSP_PROBE_INFO probe = {0};
    DWORD returned = 0;

    Log("=== COMPREHENSIVE HW PROBE ===\n");

    BOOL ok = DeviceIoControl(hDevice, IOCTL_PSP_PROBE,
        NULL, 0, &probe, sizeof(probe), &returned, NULL);

    if (!ok || returned < sizeof(probe)) {
        Log("PROBE FAILED (err=%lu)\n", GetLastError());
        return FALSE;
    }

    Log("--- Mailbox State ---\n");
    Log("  C2PMSG_35 (cmd)  = 0x%08X\n", probe.C2PMSG_35);
    Log("  C2PMSG_36 (data) = 0x%08X\n", probe.C2PMSG_36);
    Log("  C2PMSG_37 (dataH)= 0x%08X\n", probe.C2PMSG_37);
    Log("  C2PMSG_64 (ring) = 0x%08X\n", probe.C2PMSG_64);
    Log("  C2PMSG_81 (stat) = 0x%08X", probe.C2PMSG_81);
    if (probe.C2PMSG_81 == 0) Log(" (IDLE)");
    else if (probe.C2PMSG_81 == 0xFFFFFFFF) Log(" (INVALID)");
    else if (probe.C2PMSG_81 & 0x80000000) Log(" (PSP_RESPONSE)");
    Log("\n");

    Log("--- NBIO Region Probe ---\n");
    Log("  SIG1 (0xC100)    = 0x%08X%s\n", probe.NbioSig1,
        (probe.NbioSig1 == 0xFEDCBAEF) ? " (UNLOCKED)" : "");
    Log("  SIG2 (0xC180)    = 0x%08X%s\n", probe.NbioSig2,
        (probe.NbioSig2 == 0xFEDCBADF) ? " (UNLOCKED)" : "");
    Log("  MMHUB (0x50D0)   = 0x%08X%s\n", probe.MmhubCheck,
        (probe.MmhubCheck != 0 && probe.MmhubCheck != 0xFFFFFFFF) ? " (ACCESSIBLE)" : "");
    Log("  GRBM (0x2004)    = 0x%08X%s\n", probe.GrbmStatus,
        (probe.GrbmStatus != 0xFFFFFFFF) ? " *** ACCESSIBLE ***" : " (BLOCKED)");
    Log("  GC (0x3000)      = 0x%08X%s\n", probe.GcCheck,
        (probe.GcCheck != 0 && probe.GcCheck != 0xFFFFFFFF) ? " (ACCESSIBLE)" : "");
    Log("  HDP (0x05A0)     = 0x%08X%s\n", probe.HdpCheck,
        (probe.HdpCheck != 0 && probe.HdpCheck != 0xFFFFFFFF) ? " (ACCESSIBLE)" : "");

    Log("--- Operations ---\n");
    Log("  NBIO sig write   = %s\n", probe.SigWriteOk ? "OK" : "FAIL");
    Log("  Ring register    = %s", probe.RingProgOk ? "OK" : "FAIL");
    if (probe.RingCreated) Log(" (was created)");
    Log("\n");
    Log("  NBIO via ring    = %s", probe.NbioViaRingOk ? "OK" : "FAIL");
    if (probe.NbioViaRingOk) Log(" *** GRBM UNLOCKED! ***");
    Log("\n");

    Log("--- Ring Buffer State ---\n");
    Log("  AddrLow=0x%08X AddrHigh=0x%08X Size=%u\n",
        probe.RingAddrLow, probe.RingAddrHigh, probe.RingSize);

    return TRUE;
}

BOOL SendCommand(HANDLE hDevice, ULONG command)
{
    DWORD returned = 0;
    ULONG resp = 0;

    Log("SEND_CMD(0x%08X)...\n", command);

    BOOL ok = DeviceIoControl(
        hDevice,
        IOCTL_PSP_SEND_CMD,
        &command, sizeof(command),
        &resp, sizeof(resp),
        &returned,
        NULL
    );

    if (ok) {
        Log("Command 0x%08X sent OK\n", resp);
    } else {
        Log("Command 0x%08X FAILED (err=%lu)\n", command, GetLastError());
    }

    return ok;
}

BOOL InitHardware(HANDLE hDevice, ULONG64 physAddr, ULONG size)
{
    PSP_INIT_HW_REQUEST req = { physAddr, size };
    ULONG resp = 0;
    DWORD returned = 0;

    // FIX #8: Use correct format specifier for 64-bit physical address
    Log("INIT_HW(PA=0x%016llX, size=%u)...\n", physAddr, size);

    BOOL ok = DeviceIoControl(
        hDevice,
        IOCTL_PSP_INIT_HW,
        &req, sizeof(req),
        &resp, sizeof(resp),
        &returned,
        NULL
    );

    if (ok) {
        Log("Hardware initialized OK (VA=0x%08X)\n", resp);
    } else {
        Log("Hardware init FAILED (err=%lu)\n", GetLastError());
    }

    return ok;
}

BOOL LoadFirmware(HANDLE hDevice, const char* filename)
{
    FILE* f = fopen(filename, "rb");
    if (!f) {
        Log("Failed to open firmware file: %s\n", filename);
        return FALSE;
    }
    
    fseek(f, 0, SEEK_END);
    long fwSize = ftell(f);
    fseek(f, 0, SEEK_SET);
    
    if (fwSize <= 0 || fwSize > PSP_MAX_FW_SIZE) {
        Log("Invalid firmware size: %ld (max %d bytes)\n", fwSize, PSP_MAX_FW_SIZE);
        fclose(f);
        return FALSE;
    }
    
    BYTE* fwBuffer = (BYTE*)malloc(fwSize);
    if (!fwBuffer) {
        Log("Failed to allocate firmware buffer\n");
        fclose(f);
        return FALSE;
    }
    
    size_t read = fread(fwBuffer, 1, fwSize, f);
    fclose(f);
    
    if ((long)read != fwSize) {
        Log("Failed to read firmware file (read %zu of %ld)\n", read, fwSize);
        free(fwBuffer);
        return FALSE;
    }
    
    Log("Loading firmware: %s (%ld bytes)...\n", filename, fwSize);
    
    PSP_LOAD_FW_RESPONSE resp = {0};
    DWORD returned = 0;
    
    BOOL ok = DeviceIoControl(
        hDevice,
        IOCTL_PSP_LOAD_FW,
        fwBuffer, (DWORD)fwSize,
        &resp, sizeof(resp),
        &returned,
        NULL
    );
    
    free(fwBuffer);
    
    if (ok) {
        Log("Firmware load: SUCCESS (status=0x%08X, mailbox=0x%08X)\n", 
            resp.Status, resp.MailboxStatus);
    } else {
        Log("Firmware load: FAILED (err=%lu)\n", GetLastError());
    }
    
    return ok;
}

BOOL RingLoadIpFw(HANDLE hDevice, ULONG fwType, const char* filename)
{
    FILE* f = fopen(filename, "rb");
    if (!f) {
        Log("Failed to open FW file: %s\n", filename);
        return FALSE;
    }
    fseek(f, 0, SEEK_END);
    long fwSize = ftell(f);
    fseek(f, 0, SEEK_SET);
    if (fwSize <= 0 || fwSize > 1024*1024) {
        Log("Invalid FW size: %ld\n", fwSize);
        fclose(f); return FALSE;
    }

    ULONG inputSize = sizeof(PSP_RING_FW_REQUEST) + fwSize;
    BYTE* inputBuf = (BYTE*)malloc(inputSize);
    if (!inputBuf) { fclose(f); return FALSE; }

    PSP_RING_FW_REQUEST* req = (PSP_RING_FW_REQUEST*)inputBuf;
    req->FwType = fwType;
    req->FwSize = fwSize;
    size_t read = fread(inputBuf + sizeof(PSP_RING_FW_REQUEST), 1, fwSize, f);
    fclose(f);
    if ((long)read != fwSize) { free(inputBuf); return FALSE; }

    Log("RING_LOAD_IP_FW(type=%u, %s, %ld bytes)...\n", fwType, filename, fwSize);

    ULONG resp = 0;
    DWORD returned = 0;
    BOOL ok = DeviceIoControl(hDevice, IOCTL_PSP_RING_LOAD_IP_FW,
        inputBuf, inputSize, &resp, sizeof(resp), &returned, NULL);

    free(inputBuf);

    if (ok) { Log("  OK (PA>>20=0x%08X)\n", resp); }
    else { Log("  FAILED (err=%lu)\n", GetLastError()); }
    return ok;
}

static const char* FwTypeName(ULONG t) {
    switch (t) {
        case 1: return "CE";   case 2: return "PFP";
        case 3: return "ME";   case 4: return "MEC";
        case 5: return "MEC2"; case 6: return "RLC";
        case 7: return "SDMA"; case 8: return "SDMA1";
        default: return "?";
    }
}

BOOL RingLoadAllFw(HANDLE hDevice)
{
    // NOTE: Decompressed .bin files must be in the working directory (e.g., `output\`)
    static const struct { ULONG type; const char* name; } fw_list[] = {
        {1, "cyan_skillfish2_ce.bin"},
        {2, "cyan_skillfish2_pfp.bin"},
        {3, "cyan_skillfish2_me.bin"},
        {4, "cyan_skillfish2_mec.bin"},
        {5, "cyan_skillfish2_mec2.bin"},
        {6, "cyan_skillfish2_rlc.bin"},
        {7, "cyan_skillfish2_sdma.bin"},
        {8, "cyan_skillfish2_sdma1.bin"},
    };
    int count = sizeof(fw_list)/sizeof(fw_list[0]);
    int ok = 0;
    Log("=== Loading ALL GPU FW via PSP ring ===\n");
    for (int i = 0; i < count; i++) {
        Log("[%d/%d] %s: ", i+1, count, FwTypeName(fw_list[i].type));
        if (RingLoadIpFw(hDevice, fw_list[i].type, fw_list[i].name)) ok++;
    }
    Log("=== All FW loaded, triggering RLC autoload... ===\n");
    if (ok == count) {
        BOOL rlcOk = RingAutoloadRlc(hDevice);
        Log("=== Done: %d/%d loaded, RLC %s ===\n", ok, count, rlcOk ? "OK" : "FAIL");
        return rlcOk;
    } else {
        Log("=== Done: %d/%d loaded (skipping RLC) ===\n", ok, count);
        return FALSE;
    }
}

BOOL RingAutoloadRlc(HANDLE hDevice)
{
    ULONG resp = 0;
    DWORD returned = 0;
    Log("AUTOLOAD_RLC (trigger GPU FW execution)...\n");
    BOOL ok = DeviceIoControl(hDevice, IOCTL_PSP_AUTOLOAD_RLC,
        NULL, 0, &resp, sizeof(resp), &returned, NULL);
    if (ok) { Log("  OK (C2PMSG_64=0x%08X)\n", resp); }
    else { Log("  FAILED (err=%lu)\n", GetLastError()); }
    return ok;
}

BOOL InitTmr(HANDLE hDevice)
{
    ULONG resp = 0;
    DWORD returned = 0;
    Log("INIT_TMR (allocate 4MB TMR, send via ring)...\n");
    BOOL ok = DeviceIoControl(hDevice, IOCTL_PSP_INIT_TMR,
        NULL, 0, &resp, sizeof(resp), &returned, NULL);
    if (ok) { Log("  TMR Init: %s\n", resp ? "OK" : "FAIL"); }
    else { Log("  TMR Init FAILED (err=%lu)\n", GetLastError()); }
    return ok;
}

BOOL GetGpuInfo(HANDLE hDevice)
{
    PSP_GPU_INFO info = {0};
    DWORD returned = 0;
    BOOL ok = DeviceIoControl(hDevice, IOCTL_PSP_GET_GPU_INFO,
        NULL, 0, &info, sizeof(info), &returned, NULL);
    if (ok && returned >= sizeof(PSP_GPU_INFO)) {
        Log("=== GPU BRIDGE INFO ===\n");
        Log("  RingBuffer PA = 0x%08X\n", info.RingBufferPA);
        Log("  TMR Base      = 0x%016llX (size=%u)\n", info.TMRBase, info.TMSSize);
        Log("  GPU FW Loaded = %s (%u components)\n", info.FwLoaded ? "YES" : "NO", info.FwCount);
        Log("  GFX Version   = %u (cyan_skillfish2=gfx_v10_0_0)\n", info.GfxVersion);
        Log("  C2PMSG_64     = 0x%08X\n", info.C2pmsg64);
        Log("  C2PMSG_81     = 0x%08X  %s\n", info.C2pmsg81,
            (info.C2pmsg81 == 0xF0000010) ? "(SOS Alive)" : "");
        Log("  TMR Init      = %s\n", info.TmrInitialized ? "YES" : "NO");
    } else {
        Log("GET_GPU_INFO FAILED (err=%lu)\n", GetLastError());
    }
    return ok;
}

BOOL ProgReg(HANDLE hDevice, ULONG regId, ULONG value)
{
    PSP_REG_PROG_REQUEST req = { regId, value };
    ULONG resp = 0;
    DWORD returned = 0;
    Log("REG_PROG(id=%u, val=0x%08X)...\n", regId, value);
    BOOL ok = DeviceIoControl(hDevice, IOCTL_PSP_REG_PROG,
        &req, sizeof(req), &resp, sizeof(resp), &returned, NULL);
    if (ok) { Log("  OK (resp=0x%08X)\n", resp); }
    else { Log("  FAILED (err=%lu)\n", GetLastError()); }
    return ok;
}

void PrintUsage(const char *prog)
{
    printf("Usage: %s [options]\n", prog);
    printf("Options:\n");
    printf("  -r <offset>        Read register at offset (hex)\n");
    printf("  -w <offset> <val>  Write value to register at offset (hex)\n");
    printf("  -i <phys> <size>   Init hardware (map BAR5 physical address and size)\n");
    printf("  -f <file>          Load firmware file (persistent buffer, keeps allocated)\n");
    printf("  -C <cmd>           Send mailbox command to PSP (uses loaded firmware PA)\n");
    printf("  -u                 NBIO unlock (write signature registers)\n");
    printf("  -R                 Create PSP ring (allocates ring buffer, programs regs)\n");
    printf("  -U                 NBIO unlock via ring (uses ring to send command)\n");
    printf("  -s                 PSP status snapshot (comprehensive driver + HW state)\n");
    printf("  -E                 Load embedded firmware (compiled into driver)\n");
    printf("  -B                 Full boot sequence (embedded FW + SYSDRV 0x4 + SOS 0x8)\n");
    printf("  -pb <bus> <df> <off>  PCI config read (bus, device<<3|func, offset hex)\n");
    printf("  -pw <bus> <df> <off> <val>  PCI config write\n");
    printf("  -L <type> <file>   Load GPU FW via PSP ring (type: 1=CE 2=PFP 3=ME 4=MEC 5=MEC2 6=RLC 7=SDMA 8=SDMA1)\n");
    printf("  -A                 Load ALL GPU FW via PSP ring (cyan_skillfish2)\n");
    printf("  -G                 Get GPU bridge info (ring PA, TMR, FW status)\n");
    printf("  -P <id> <val>      Program register through PSP ring\n");
    printf("  -T                 Trigger RLC autoload (start GPU FW execution)\n");
    printf("  -m                 Read mailbox status (C2PMSG_81)\n");
    printf("  -t                 Run basic connectivity test\n");
    printf("  -M                 Initialize TMR (Trusted Memory Region, 4MB)\n");
    printf("  -T                 Run comprehensive HW probe (mailbox + NBIO + ring)\n");
    printf("  -l <logfile>       Write log to file\n");
    printf("\nExamples:\n");
    printf("  %s -i 0xFE800000 0x100000     Init HW with BAR5 at 0xFE800000\n", prog);
    printf("  %s -r 0x1056C                  Read C2PMSG_35\n", prog);
    printf("  %s -u                          NBIO unlock\n", prog);
    printf("  %s -m                          Read C2PMSG_81\n", prog);
    printf("  %s -t                          Run connectivity test\n", prog);
}

int main(int argc, char *argv[])
{
    if (argc < 2) {
        PrintUsage(argv[0]);
        return 1;
    }

    // Parse options
    const char *logfile = NULL;
    int i = 1;
    while (i < argc && argv[i][0] == '-') {
        if (strcmp(argv[i], "-l") == 0 && i + 1 < argc) {
            logfile = argv[++i];
        }
        i++;
    }

    if (logfile) {
        g_log = fopen(logfile, "w");
        if (!g_log) {
            printf("Warning: cannot open log file: %s\n", logfile);
        }
    }

    Log("=== AMD BC-250 PSP Driver Test Tool ===\n");
    Log("Opening driver: %ls\n", PSP_DEVICE_NAME);

    HANDLE h = OpenPspDriver();
    if (h == INVALID_HANDLE_VALUE) {
        DWORD err = GetLastError();
        Log("ERROR: Failed to open driver (error=%lu)\n", err);
        Log("Make sure the driver is installed and loaded.\n");
        Log("Run as Administrator if needed.\n");
        if (g_log) fclose(g_log);
        return 1;
    }

    Log("Driver opened successfully!\n\n");

    BOOL ok;
    ULONG value;
    int ret = 0;

    // Parse and execute commands
    i = 1;
    while (i < argc) {
        if (strcmp(argv[i], "-r") == 0 && i + 1 < argc) {
            ULONG offset = (ULONG)strtoul(argv[++i], NULL, 0);
            Log("READ_REG(0x%04X): ", offset);
            ok = ReadRegister(h, offset, &value);
            if (ok) {
                Log("0x%08X\n", value);
            } else {
                Log("FAILED (err=%lu)\n", GetLastError());
                ret = 1;
            }
        }
        else if (strcmp(argv[i], "-w") == 0 && i + 2 < argc) {
            ULONG offset = (ULONG)strtoul(argv[++i], NULL, 0);
            ULONG val = (ULONG)strtoul(argv[++i], NULL, 0);
            Log("WRITE_REG(0x%04X, 0x%08X): ", offset, val);
            ok = WriteRegister(h, offset, val);
            if (ok) {
                Log("OK\n");
            } else {
                Log("FAILED (err=%lu)\n", GetLastError());
                ret = 1;
            }
        }
        else if (strcmp(argv[i], "-i") == 0 && i + 2 < argc) {
            ULONG physAddr = (ULONG)strtoul(argv[++i], NULL, 0);
            ULONG size = (ULONG)strtoul(argv[++i], NULL, 0);
            ok = InitHardware(h, physAddr, size);
            if (!ok) {
                ret = 1;
            }
        }
        else if (strcmp(argv[i], "-C") == 0 && i + 1 < argc) {
            ULONG cmd = (ULONG)strtoul(argv[++i], NULL, 0);
            ok = SendCommand(h, cmd);
            if (!ok) {
                ret = 1;
            }
        }
        else if (strcmp(argv[i], "-u") == 0) {
            ok = UnlockNbio(h);
            if (!ok) { ret = 1; }
        }
        else if (strcmp(argv[i], "-R") == 0) {
            ok = CreateRing(h);
            if (!ok) { ret = 1; }
        }
        else if (strcmp(argv[i], "-U") == 0) {
            ok = NbioViaRing(h);
            if (!ok) { ret = 1; }
        }
        else if (strcmp(argv[i], "-s") == 0) {
            ok = GetPspStatus(h);
            if (!ok) { ret = 1; }
        }
        else if (strcmp(argv[i], "-B") == 0) {
            ok = BootSequence(h);
            if (!ok) { ret = 1; }
        }
        else if (strcmp(argv[i], "-pb") == 0 && i + 3 < argc) {
            ULONG bus = (ULONG)strtoul(argv[++i], NULL, 0);
            ULONG df = (ULONG)strtoul(argv[++i], NULL, 0);
            ULONG off = (ULONG)strtoul(argv[++i], NULL, 0);
            ok = PciRead(h, bus, df, off);
            if (!ok) { ret = 1; }
        }
        else if (strcmp(argv[i], "-pw") == 0 && i + 4 < argc) {
            ULONG bus = (ULONG)strtoul(argv[++i], NULL, 0);
            ULONG df = (ULONG)strtoul(argv[++i], NULL, 0);
            ULONG off = (ULONG)strtoul(argv[++i], NULL, 0);
            ULONG val = (ULONG)strtoul(argv[++i], NULL, 0);
            ok = PciWrite(h, bus, df, off, val);
            if (!ok) { ret = 1; }
        }
        else if (strcmp(argv[i], "-E") == 0) {
            ok = LoadEmbeddedFirmware(h);
            if (!ok) { ret = 1; }
        }
        else if (strcmp(argv[i], "-f") == 0 && i + 1 < argc) {
            const char* filename = argv[++i];
            Log("LOAD_FIRMWARE: %s\n", filename);
            ok = LoadFirmware(h, filename);
            if (!ok) {
                ret = 1;
            }
        }
        else if (strcmp(argv[i], "-L") == 0 && i + 2 < argc) {
            ULONG fwType = (ULONG)strtoul(argv[++i], NULL, 0);
            const char* filename = argv[++i];
            ok = RingLoadIpFw(h, fwType, filename);
            if (!ok) { ret = 1; }
        }
        else if (strcmp(argv[i], "-A") == 0) {
            ok = RingLoadAllFw(h);
            if (!ok) { ret = 1; }
        }
        else if (strcmp(argv[i], "-G") == 0) {
            ok = GetGpuInfo(h);
            if (!ok) { ret = 1; }
        }
        else if (strcmp(argv[i], "-P") == 0 && i + 2 < argc) {
            ULONG regId = (ULONG)strtoul(argv[++i], NULL, 0);
            ULONG val = (ULONG)strtoul(argv[++i], NULL, 0);
            ok = ProgReg(h, regId, val);
            if (!ok) { ret = 1; }
        }
        else if (strcmp(argv[i], "-T") == 0) {
            ok = RingAutoloadRlc(h);
            if (!ok) { ret = 1; }
        }
        else if (strcmp(argv[i], "-m") == 0) {
            Log("MAILBOX STATUS (C2PMSG_81): ");
            ok = ReadRegister(h, PSP_C2PMSG_81_OFFSET, &value);
            if (ok) {
                Log("0x%08X\n", value);
            } else {
                Log("FAILED (err=%lu)\n", GetLastError());
                ret = 1;
            }
        }
        else if (strcmp(argv[i], "-T") == 0) {
            ok = ComprehensiveProbe(h);
            if (!ok) { ret = 1; }
        }
        else if (strcmp(argv[i], "-M") == 0) {
            ok = InitTmr(h);
            if (!ok) { ret = 1; }
        }
        else if (strcmp(argv[i], "-t") == 0) {
            Log("=== Connectivity Test ===\n");
            
            // Test 1: Read C2PMSG_35 (command register)
            Log("Test 1 - Read C2PMSG_35 (0x%04X): ", PSP_C2PMSG_35_OFFSET);
            ok = ReadRegister(h, PSP_C2PMSG_35_OFFSET, &value);
            if (ok) {
                Log("PASS (value=0x%08X)\n", value);
            } else {
                Log("FAIL (err=%lu)\n", GetLastError());
                ret = 1;
            }

            // Test 2: Read C2PMSG_36 (data register)
            Log("Test 2 - Read C2PMSG_36 (0x%04X): ", PSP_C2PMSG_36_OFFSET);
            ok = ReadRegister(h, PSP_C2PMSG_36_OFFSET, &value);
            if (ok) {
                Log("PASS (value=0x%08X)\n", value);
            } else {
                Log("FAIL (err=%lu)\n", GetLastError());
                ret = 1;
            }

            // Test 3: Read C2PMSG_81 (status register)
            Log("Test 3 - Read C2PMSG_81 (0x%04X): ", PSP_C2PMSG_81_OFFSET);
            ok = ReadRegister(h, PSP_C2PMSG_81_OFFSET, &value);
            if (ok) {
                Log("PASS (value=0x%08X)\n", value);
            } else {
                Log("FAIL (err=%lu)\n", GetLastError());
                ret = 1;
            }

            Log("\n=== Test Complete ===\n");
        }
        else if (strcmp(argv[i], "-l") == 0) {
            i++; // Skip logfile argument (already handled)
        }
        else if (argv[i][0] == '-') {
            Log("Unknown option: %s\n", argv[i]);
            ret = 1;
        }
        i++;
    }

    CloseHandle(h);
    Log("\nDone.\n");
    if (g_log) fclose(g_log);
    return ret;
}
