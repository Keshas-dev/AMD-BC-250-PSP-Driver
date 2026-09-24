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
        Log("  GC (0x4260)=0x%08X  HDP (0x05A0)=0x%08X  MMHUB (0x50D0)=0x%08X\n",
            info.GcCheck, info.HdpCheck, info.MmhubCheck);
        Log("  ME_CNTL=0x%08X  (ME_HALT=%d PFP_HALT=%d)\n", info.MeCntl,
            (info.MeCntl & 0x10000000) ? 1 : 0, (info.MeCntl & 0x40000000) ? 1 : 0);
        Log("  GRBM_GFX_INDEX=0x%08X\n", info.GrbmGfxIndex);
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
    Log("  GRBM (0x3264)    = 0x%08X%s\n", probe.GrbmStatus,
        (probe.GrbmStatus != 0xFFFFFFFF) ? " *** ACCESSIBLE ***" : " (BLOCKED)");
    Log("  GC (0x4260)      = 0x%08X%s\n", probe.GcCheck,
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
    
    PSP_LOAD_FW_RESPONSE resp;
    DWORD returned = 0;
    RtlZeroMemory(&resp, sizeof(resp));
    
    BOOL ok = DeviceIoControl(
        hDevice,
        IOCTL_PSP_LOAD_FW,
        fwBuffer, (DWORD)fwSize,
        &resp, sizeof(resp),
        &returned,
        NULL
    );
    
    free(fwBuffer);
    
    if (ok && returned == sizeof(resp)) {
        Log("Firmware load: SUCCESS (PA>>20=0x%08X C2PMSG_81=0x%08X)\n",
            resp.Status == 0 ? resp.MailboxStatus : 0, resp.MailboxStatus);
    } else if (ok) {
        Log("Firmware load: partial response (%u bytes, expected %u)\n", returned, sizeof(resp));
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
        case 1: return "ME";    case 2: return "PFP";
        case 3: return "CE";   case 4: return "MEC";
        case 5: return "MEC2"; case 8: return "RLC";
        case 9: return "SDMA"; case 10: return "SDMA1";
        default: return "?";
    }
}

BOOL RingLoadAllFw(HANDLE hDevice)
{
    static const struct { ULONG type; const char* name; } fw_list[] = {
        {3, "cyan_skillfish2_ce.bin"},
        {2, "cyan_skillfish2_pfp.bin"},
        {1, "cyan_skillfish2_me.bin"},
        {4, "cyan_skillfish2_mec.bin"},
        {5, "cyan_skillfish2_mec2.bin"},
        {8, "cyan_skillfish2_rlc.bin"},
        {9, "cyan_skillfish2_sdma.bin"},
        {10, "cyan_skillfish2_sdma1.bin"},
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

BOOL KiqSubmit(HANDLE hDevice, PULONG commands, ULONG count)
{
    PSP_KIQ_SUBMIT_REQUEST req;
    req.CommandCount = count;
    memset(req.Reserved, 0, sizeof(req.Reserved));
    memset(req.Commands, 0, sizeof(req.Commands));
    memcpy(req.Commands, commands, count * sizeof(ULONG));
    ULONG wptr = 0;
    DWORD returned = 0;
    Log("KIQ_SUBMIT(%u dwords)...\n", count);
    BOOL ok = DeviceIoControl(hDevice, IOCTL_PSP_KIQ_SUBMIT,
        &req, sizeof(req), &wptr, sizeof(wptr), &returned, NULL);
    if (ok) { Log("  OK (wptr=0x%08X)\n", wptr); }
    else { Log("  FAILED (err=%lu)\n", GetLastError()); }
    return ok;
}

BOOL KiqLoadFw(HANDLE hDevice, ULONG fwType, const char* filename)
{
    FILE *fp = fopen(filename, "rb");
    if (!fp) { Log("  FAILED: cannot open %s\n", filename); return FALSE; }
    
    fseek(fp, 0, SEEK_END);
    long fwSize = ftell(fp);
    fseek(fp, 0, SEEK_SET);
    
    if (fwSize <= 0 || fwSize > 1024*1024) { Log("  FAILED: invalid firmware size %ld\n", fwSize); fclose(fp); return FALSE; }
    
    size_t bufSize = sizeof(PSP_KIQ_LOAD_FW_REQUEST) + fwSize;
    PSP_KIQ_LOAD_FW_REQUEST *req = (PSP_KIQ_LOAD_FW_REQUEST*)malloc(bufSize);
    if (!req) { Log("  FAILED: cannot allocate %zu bytes\n", bufSize); fclose(fp); return FALSE; }
    
    req->FwType = fwType;
    req->FwSize = (ULONG)fwSize;
    fread(req + 1, 1, fwSize, fp);
    fclose(fp);
    
    DWORD returned = 0;
    ULONG resp = 0;
    BOOL ok = DeviceIoControl(hDevice, IOCTL_PSP_KIQ_LOAD_FW,
        req, (DWORD)bufSize, &resp, sizeof(resp), &returned, NULL);
    
    if (ok) {
        Log("  KIQ_LOAD_FW: type=%u size=%lu status=0x%08X\n", fwType, fwSize, resp);
    } else {
        Log("  FAILED (err=%lu)\n", GetLastError());
    }
    
    free(req);
    return ok;
}

BOOL SmuWake(HANDLE hDevice, ULONG message, ULONG argument)
{
    PSP_SMU_WAKE_REQUEST req;
    PSP_SMU_WAKE_RESPONSE resp;
    DWORD returned = 0;
    RtlZeroMemory(&req, sizeof(req));
    req.Message = message;
    req.Argument = argument;
    BOOL ok = DeviceIoControl(hDevice, IOCTL_PSP_SMU_WAKE,
        &req, sizeof(req), &resp, sizeof(resp), &returned, NULL);
    if (ok && returned >= sizeof(PSP_SMU_WAKE_RESPONSE)) {
        Log("SMU_WAKE: msg=0x%08X arg=0x%08X => resp=0x%08X status=%s\n",
            resp.Message, resp.Argument, resp.Response,
            resp.Status ? "OK" : "FAIL");
    } else {
        Log("SMU_WAKE FAILED (err=%lu)\n", GetLastError());
    }
    return ok;
}

/* --- 8-core unlock via Host Bridge PCI config 0xB8/0xBC (GabriWar path) --- */

static BOOL PciCfgReadVal(HANDLE h, ULONG bus, ULONG devFn, ULONG off, ULONG *out)
{
    ULONG args[3] = { bus, devFn, off };
    ULONG resp = 0xFFFFFFFF;
    DWORD returned = 0;
    BOOL ok = DeviceIoControl(h, IOCTL_PSP_PCI_READ,
        args, sizeof(args), &resp, sizeof(resp), &returned, NULL);
    if (ok && returned >= sizeof(ULONG)) { *out = resp; return TRUE; }
    return FALSE;
}

static BOOL PciCfgWriteVal(HANDLE h, ULONG bus, ULONG devFn, ULONG off, ULONG val)
{
    ULONG args[4] = { bus, devFn, off, val };
    DWORD returned = 0;
    return DeviceIoControl(h, IOCTL_PSP_PCI_WRITE,
        args, sizeof(args), NULL, 0, &returned, NULL);
}

/* Find Host Bridge 1022:13E0. Returns TRUE + bus/devFn (Linux-style (dev<<3)|fn). */
static BOOL FindHostBridge(HANDLE h, ULONG *busOut, ULONG *dfOut)
{
    ULONG b, d, f;
    for (b = 0; b < 8; b++) {
        for (d = 0; d < 32; d++) {
            for (f = 0; f < 8; f++) {
                ULONG df = (d << 3) | f;
                ULONG id = 0;
                if (!PciCfgReadVal(h, b, df, 0, &id)) continue;
                if (id == 0xFFFFFFFF || id == 0) continue;
                if ((id & 0xFFFF) == 0x1022 && ((id >> 16) & 0xFFFF) == 0x13E0) {
                    *busOut = b; *dfOut = df;
                    return TRUE;
                }
            }
        }
    }
    return FALSE;
}

/* Read an SMN register via Host Bridge 0xB8/0xBC (save/select/read/restore). Read-only. */
static BOOL SmnReadViaPci(HANDLE h, ULONG smnAddr, ULONG *out)
{
    ULONG bus, df, saved = 0, val = 0;
    if (!FindHostBridge(h, &bus, &df)) {
        Log("  Host Bridge 1022:13E0 not found\n");
        return FALSE;
    }
    if (!PciCfgReadVal(h, bus, df, 0xB8, &saved)) return FALSE;
    if (!PciCfgWriteVal(h, bus, df, 0xB8, smnAddr)) return FALSE;
    if (!PciCfgReadVal(h, bus, df, 0xBC, &val)) return FALSE;
    PciCfgWriteVal(h, bus, df, 0xB8, saved); /* best-effort restore */
    *out = val;
    return TRUE;
}

BOOL EightCoreStatus(HANDLE hDevice)
{
    ULONG mask = 0;
    int i;
    Log("8CORE STATUS: reading SMN 0x0115A870 via Host Bridge PCI 0xB8/0xBC...\n");
    if (!SmnReadViaPci(hDevice, 0x0115A870, &mask)) {
        Log("  FAILED to read core mask\n");
        return FALSE;
    }
    mask &= 0xFF;
    Log("  SMN 0x0115A870 = 0x%02X  enabled=[", mask);
    for (i = 0; i < 8; i++) if ((mask >> i) & 1) Log("%d ", i);
    Log("] disabled=[");
    for (i = 0; i < 8; i++) if (!((mask >> i) & 1)) Log("%d ", i);
    Log("]\n");
    if (mask == 0xFF) Log("  state: UNLOCKED (all 8 cores)\n");
    else if (mask == 0x77) Log("  state: STOCK (6 cores) — run '-8core apply' to unlock\n");
    else Log("  state: UNKNOWN mask (refusing to guess)\n");
    {
        SYSTEM_INFO si;
        GetSystemInfo(&si);
        Log("  logical processors visible: %u%s\n", (unsigned)si.dwNumberOfProcessors,
            (mask == 0xFF && si.dwNumberOfProcessors < 16) ?
            "  (mask set but firmware has not re-enumerated — warm reboot needed)" : "");
    }
    return TRUE;
}

BOOL EightCoreApply(HANDLE hDevice)
{
    PSP_PCI_SMN_UNLOCK_REQUEST req;
    PSP_PCI_SMN_UNLOCK_RESPONSE resp;
    DWORD returned = 0;
    ULONG mask = 0;

    Log("8CORE APPLY: checking current mask first...\n");
    if (SmnReadViaPci(hDevice, 0x0115A870, &mask)) {
        mask &= 0xFF;
        Log("  before: 0x%02X\n", mask);
        if (mask == 0xFF) { Log("  already unlocked, nothing to do\n"); return TRUE; }
        if (mask != 0x77) {
            Log("  REFUSING: unexpected mask 0x%02X (expected 0x77)\n", mask);
            return FALSE;
        }
    } else {
        Log("  WARNING: could not read mask, proceeding to SMU command anyway\n");
    }

    RtlZeroMemory(&req, sizeof(req));
    req.SmnAddr = 0x0115A870;
    req.SmnValue = 0xFF;
    req.SmuMsg = 0x98;
    req.Reserved = 0;
    RtlZeroMemory(&resp, sizeof(resp));
    Log("8CORE APPLY: sending Q3 msg 0x98 via PCI config 0xB8/0xBC...\n");
    if (!DeviceIoControl(hDevice, IOCTL_PSP_PCI_SMN_UNLOCK,
        &req, sizeof(req), &resp, sizeof(resp), &returned, NULL)) {
        Log("  IOCTL FAILED (err=%lu)\n", GetLastError());
        return FALSE;
    }
    Log("  SMU response=0x%02X readback=0x%08X status=0x%08X\n",
        resp.SmuResponse, resp.SmnReadback, resp.Status);
    if ((resp.SmnReadback & 0xFF) == 0xFF) {
        Log("  UNLOCKED. Cores appear after a WARM reboot (cold boot reverts to 6).\n");
        return TRUE;
    }
    Log("  UNLOCK FAILED — mask unchanged, nothing broken\n");
    return FALSE;
}

/* --- end 8-core --- */

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
    printf("  -L <type> <file>   Load GPU FW via PSP ring (type: 1=ME 2=PFP 3=CE 4=MEC 5=MEC2 8=RLC 9=SDMA 10=SDMA1)\n");
    printf("  -A                 Load ALL GPU FW via PSP ring (cyan_skillfish2)\n");
    printf("  -G                 Get GPU bridge info (ring PA, TMR, FW status)\n");
    printf("  -P <id> <val>      Program register through PSP ring\n");
    printf("  -T                 Trigger RLC autoload (start GPU FW execution)\n");
    printf("  -m                 Read mailbox status (C2PMSG_81)\n");
    printf("  -t                 Run basic connectivity test\n");
    printf("  -M                 Initialize TMR (Trusted Memory Region, 4MB)\n");
    printf("  -H                 Run comprehensive HW probe (mailbox + NBIO + ring)\n");
    printf("  -k <dwords...>     Submit PM4 commands via KIQ ring (hex dwords, up to 64)\n");
    printf("  -Q <type> <file>   Load GPU FW via KIQ ring (type: 1=ME 2=PFP 3=CE 4=MEC 5=MEC2 8=RLC 9=SDMA 10=SDMA1)\n");
    printf("  -S <msg> <arg>     Send SMU wake command (hex msgID, hex arg)\n");
    printf("  -8core status|apply  8-core unlock via Host Bridge PCI 0xB8/0xBC (Q3 msg 0x98)\n");
    printf("  -pa-nonce            DBC GET_NONCE via pa_v1 mailbox (channel prove, expect mbox 0x4)\n");
    printf("  -pa-hsti             HSTI QUERY via pa_v1 mailbox (security bits, may be empty)\n");
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
            ULONG64 physAddr = strtoull(argv[++i], NULL, 0);
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
        else if (strcmp(argv[i], "-R") == 0 && i + 2 < argc) {
            ULONG regId = (ULONG)strtoul(argv[++i], NULL, 0);
            ULONG regVal = (ULONG)strtoul(argv[++i], NULL, 0);
            ok = ProgReg(h, regId, regVal);
            if (!ok) { ret = 1; }
        }
        else if (strcmp(argv[i], "-S") == 0 && i + 2 < argc) {
            ULONG msg = (ULONG)strtoul(argv[++i], NULL, 0);
            ULONG arg = (ULONG)strtoul(argv[++i], NULL, 0);
            ok = SmuWake(h, msg, arg);
            if (!ok) { ret = 1; }
        }
        else if (strcmp(argv[i], "-H") == 0) {
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
        else if (strcmp(argv[i], "-k") == 0) {
            // Collect variable number of PM4 DWORDs after -k
            ULONG cmds[64];
            ULONG count = 0;
            int j = i + 1;
            while (j < argc && argv[j][0] != '-' && count < 64) {
                cmds[count++] = (ULONG)strtoul(argv[j], NULL, 0);
                j++;
            }
            if (count > 0) {
                Log("KIQ_SUBMIT(%u dwords): ", count);
                for (ULONG d = 0; d < count; d++) Log("0x%08X ", cmds[d]);
                Log("\n");
                ok = KiqSubmit(h, cmds, count);
                if (!ok) { ret = 1; }
            } else {
                Log("ERROR: -k requires at least one PM4 DWORD\n");
                ret = 1;
            }
            i = j - 1; // Skip consumed args
        }
        else if (strcmp(argv[i], "-Q") == 0 && i + 2 < argc) {
            ULONG fwType = (ULONG)strtoul(argv[i + 1], NULL, 0);
            const char* filename = argv[i + 2];
            Log("KIQ_LOAD_FW: type=%u file=%s\n", fwType, filename);
            ok = KiqLoadFw(h, fwType, filename);
            if (!ok) { ret = 1; }
            i += 2;
        }
        else if (strcmp(argv[i], "-F") == 0 && i + 2 < argc) {
            ULONG fwType = (ULONG)strtoul(argv[i + 1], NULL, 0);
            const char* filename = argv[i + 2];
            Log("LOAD_IP_FW_DIRECT: type=%u file=%s\n", fwType, filename);

            FILE *fp = fopen(filename, "rb");
            if (!fp) { Log("  FAILED: cannot open %s\n", filename); ret = 1; }
            else {
                fseek(fp, 0, SEEK_END);
                long fwSize = ftell(fp);
                fseek(fp, 0, SEEK_SET);
                if (fwSize <= 0 || fwSize > 1024*1024) {
                    Log("  FAILED: invalid firmware size %ld\n", fwSize);
                    fclose(fp); ret = 1;
                } else {
                    size_t bufSize = sizeof(PSP_LOAD_IP_FW_REQUEST) + fwSize;
                    PSP_LOAD_IP_FW_REQUEST *req = (PSP_LOAD_IP_FW_REQUEST*)malloc(bufSize);
                    if (!req) { Log("  FAILED: cannot allocate %zu bytes\n", bufSize); fclose(fp); ret = 1; }
                    else {
                        req->FwType = fwType;
                        req->FwSize = (ULONG)fwSize;
                        fread(req + 1, 1, fwSize, fp);
                        fclose(fp);

                        PSP_LOAD_IP_FW_RESPONSE resp;
                        DWORD returned = 0;
                        BOOL ok2 = DeviceIoControl(h, IOCTL_PSP_LOAD_IP_FW_DIRECT,
                            req, (DWORD)bufSize, &resp, sizeof(resp), &returned, NULL);
                        if (ok2) {
                            Log("  Status=0x%08X C2Pmsg35=0x%08X C2Pmsg81=0x%08X\n",
                                resp.Status, resp.C2Pmsg35, resp.C2Pmsg81);
                        } else {
                            Log("  FAILED (err=%lu)\n", GetLastError());
                        }
                        free(req);
                    }
                }
            }
            i += 2;
        }
        else if (strcmp(argv[i], "-8core") == 0 && i + 1 < argc) {
            if (strcmp(argv[i + 1], "status") == 0) {
                ok = EightCoreStatus(h);
                if (!ok) { ret = 1; }
            } else if (strcmp(argv[i + 1], "apply") == 0) {
                ok = EightCoreApply(h);
                if (!ok) { ret = 1; }
            } else {
                Log("ERROR: -8core needs 'status' or 'apply'\n");
                ret = 1;
            }
            i += 1;
        }
        else if (strcmp(argv[i], "-pa-nonce") == 0) {
            PSP_PA_SEND_REQUEST preq;
            PSP_PA_SEND_RESPONSE presp;
            DWORD pret = 0;
            int n;
            RtlZeroMemory(&preq, sizeof(preq));
            preq.Msg = PSP_PA_MSG_DBC_GET_NONCE;
            preq.AuthNeeded = 0;
            RtlZeroMemory(&presp, sizeof(presp));
            Log("PA_SEND: DBC GET_NONCE via pa_v1 mailbox (expect mbox 0x4 on BC-250)...\n");
            ok = DeviceIoControl(h, IOCTL_PSP_PA_SEND,
                &preq, sizeof(preq), &presp, sizeof(presp), &pret, NULL);
            if (!ok) {
                Log("  IOCTL FAILED (err=%lu)\n", GetLastError());
                ret = 1;
            } else {
                Log("  Status=0x%08X MailboxStatus=0x%X Payload=%u CmdRaw=0x%08X\n",
                    presp.Status, presp.MailboxStatus, presp.PayloadSize, presp.CmdRespRaw);
                Log("  Nonce: ");
                for (n = 0; n < 16; n++) Log("%02X", presp.Nonce[n]);
                Log("\n");
                if (presp.MailboxStatus == 0x4)
                    Log("  = Linux behavior reproduced (rejected 0x4, channel LIVE)\n");
                else if (presp.MailboxStatus == 0)
                    Log("  = command ACCEPTED (unexpected on BC-250!)\n");
            }
        }
        else if (strcmp(argv[i], "-pa-hsti") == 0) {
            PSP_PA_SEND_REQUEST preq;
            PSP_PA_SEND_RESPONSE presp;
            DWORD pret = 0;
            RtlZeroMemory(&preq, sizeof(preq));
            preq.Msg = PSP_PA_MSG_HSTI_QUERY;
            preq.AuthNeeded = 0;
            RtlZeroMemory(&presp, sizeof(presp));
            Log("PA_SEND: HSTI QUERY via pa_v1 mailbox...\n");
            ok = DeviceIoControl(h, IOCTL_PSP_PA_SEND,
                &preq, sizeof(preq), &presp, sizeof(presp), &pret, NULL);
            if (!ok) {
                Log("  IOCTL FAILED (err=%lu)\n", GetLastError());
                ret = 1;
            } else {
                Log("  Status=0x%08X MailboxStatus=0x%X Payload=%u CmdRaw=0x%08X\n",
                    presp.Status, presp.MailboxStatus, presp.PayloadSize, presp.CmdRespRaw);
                Log("  HSTI=0x%08X\n", presp.Hsti);
                if (presp.MailboxStatus == 0) {
                    /* Verified layout (union psp_cap_register): hsti bit N ->
                     * capability bit N+8: b0=fused_part b1=boot_integrity
                     * b2=debug_lock_on b5=tsme b7=anti_rollback
                     * b8=rpmc_prod b9=rpmc_spi b10=tpm b11=rom_armor */
                    ULONG hsti = presp.Hsti;
                    Log("  fused_part=%u boot_integrity=%u debug_lock_on=%u\n",
                        (hsti >> 0) & 1, (hsti >> 1) & 1, (hsti >> 2) & 1);
                    Log("  tsme=%u anti_rollback=%u rpmc_prod=%u rpmc_spi=%u tpm=%u rom_armor=%u\n",
                        (hsti >> 5) & 1, (hsti >> 7) & 1, (hsti >> 8) & 1,
                        (hsti >> 9) & 1, (hsti >> 10) & 1, (hsti >> 11) & 1);
                }
                if (presp.MailboxStatus != 0) {
                    Log("  = HSTI rejected/absent (Tadini: HSTI empty on BC-250)\n");
                }
            }
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
