// test-psp-driver.c
// PSP Test Tool — uses GPU driver IOCTLs (no separate PSP kernel driver)

#define _CRT_SECURE_NO_WARNINGS
#include <windows.h>
#include <stdio.h>
#include <stdarg.h>
#include <stdlib.h>
#include <string.h>

#define FILE_DEVICE_AMDBC250 0x8000
#include "PspIoctl.h"

static FILE *g_log = NULL;
static char g_exeDir[MAX_PATH] = {0};

// Find a firmware file by trying several paths relative to the EXE
static const char* FindFirmwareFile(const char *name) {
    static char buf[MAX_PATH*2];
    // 1. Direct path (current directory)
    FILE *f = fopen(name, "rb");
    if (f) { fclose(f); return name; }
    // 2. EXE directory
    if (g_exeDir[0]) {
        snprintf(buf, sizeof(buf), "%s\\%s", g_exeDir, name);
        f = fopen(buf, "rb");
        if (f) { fclose(f); return buf; }
        // 3. firmware\ subdir relative to EXE
        snprintf(buf, sizeof(buf), "%s\\firmware\\%s", g_exeDir, name);
        f = fopen(buf, "rb");
        if (f) { fclose(f); return buf; }
        // 4. ..\firmware\ relative to EXE
        snprintf(buf, sizeof(buf), "%s\\..\\firmware\\%s", g_exeDir, name);
        f = fopen(buf, "rb");
        if (f) { fclose(f); return buf; }
    }
    return name; // fallback to original name (will fail with clear error)
}

void Log(const char *fmt, ...) {
    va_list a; va_start(a, fmt);
    vfprintf(stdout, fmt, a); va_end(a);
    if (g_log) {
        va_list b; va_start(b, fmt);
        vfprintf(g_log, fmt, b); va_end(b); fflush(g_log);
    }
}

HANDLE OpenGpuDriver() {
    return CreateFileW(GPU_DEVICE_NAME,
        GENERIC_READ | GENERIC_WRITE, 0, NULL,
        OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
}

BOOL IoCtl(HANDLE h, ULONG code, const void *in, ULONG inSize, void *out, ULONG outSize) {
    DWORD ret = 0;
    return DeviceIoControl(h, code, (void*)in, inSize, out, outSize, &ret, NULL);
}

// --- GPU driver register access (raw BAR5 proxy 0x900/0x901) ---
BOOL GpuReadReg(HANDLE h, ULONG offset, PULONG pValue) {
    ULONG out = 0;
    BOOL ok = IoCtl(h, IOCTL_GPU_BAR5_READ_PROXY_RAW, &offset, sizeof(offset), &out, sizeof(out));
    if (ok && pValue) *pValue = out;
    return ok;
}

BOOL GpuWriteReg(HANDLE h, ULONG offset, ULONG value) {
    ULONG in[2] = {offset, value};
    return IoCtl(h, IOCTL_GPU_BAR5_WRITE_PROXY_RAW, in, sizeof(in), NULL, 0);
}

// --- GPU driver READ_REG / WRITE_REG (structured CTL_CODE) ---
BOOL GpuReadReg2(HANDLE h, ULONG offset, PULONG pValue) {
    AMDBC250_IOCTL_READ_REG req = {offset, 0, 0};
    BOOL ok = IoCtl(h, IOCTL_GPU_READ_REG, &req, sizeof(req), &req, sizeof(req));
    if (ok && pValue) *pValue = req.Value;
    return ok;
}

BOOL GpuWriteReg2(HANDLE h, ULONG offset, ULONG value) {
    AMDBC250_IOCTL_WRITE_REG req = {offset, value, 0};
    return IoCtl(h, IOCTL_GPU_WRITE_REG, &req, sizeof(req), &req, sizeof(req));
}

// --- Init GPU driver HW (map BAR5) ---
BOOL GpuInitHw(HANDLE h, ULONG64 physAddr, ULONG size, ULONG flags) {
    AMDBC250_IOCTL_INIT_HARDWARE ih;
    memset(&ih, 0, sizeof(ih));
    ih.MmioPhysicalBase = physAddr;
    ih.MmioSize = size;
    ih.Flags = flags;
    return IoCtl(h, IOCTL_GPU_INIT_HW, &ih, sizeof(ih), &ih, sizeof(ih));
}

// --- Direct PSP mailbox: load IP firmware ---
BOOL GpuLoadIpFw(HANDLE h, ULONG fwType, const char *filename) {
    const char *path = FindFirmwareFile(filename);
    FILE *fp = fopen(path, "rb");
    if (!fp) { Log("  FAILED: cannot open %s (searched: cwd, exe_dir, firmware\\, ..\\firmware\\)\n", filename); return FALSE; }
    fseek(fp, 0, SEEK_END);
    long fwSize = ftell(fp);
    fseek(fp, 0, SEEK_SET);
    if (fwSize <= 0 || fwSize > 1024*1024) {
        Log("  FAILED: invalid size %ld\n", fwSize); fclose(fp); return FALSE;
    }

    size_t bufSize = sizeof(AMDBC250_IOCTL_PSP_LOAD_IP_FW) + fwSize;
    BYTE *buf = (BYTE*)malloc(bufSize);
    if (!buf) { Log("  FAILED: alloc\n"); fclose(fp); return FALSE; }

    AMDBC250_IOCTL_PSP_LOAD_IP_FW *req = (AMDBC250_IOCTL_PSP_LOAD_IP_FW*)buf;
    req->FwType = fwType;
    req->FwSize = (ULONG)fwSize;
    req->Result = 0;
    req->C2Pmsg35After = 0;
    req->C2Pmsg81After = 0;
    fread(req + 1, 1, fwSize, fp);
    fclose(fp);

    AMDBC250_IOCTL_PSP_LOAD_IP_FW resp;
    DWORD ret = 0;
    BOOL ok = IoCtl(h, IOCTL_GPU_PSP_LOAD_IP_FW, buf, (DWORD)bufSize, &resp, sizeof(resp));

    if (ok) {
        Log("  Result=%u C2Pmsg35=0x%08X C2Pmsg81=0x%08X\n",
            resp.Result, resp.C2Pmsg35After, resp.C2Pmsg81After);
    } else {
        Log("  FAILED (err=%lu)\n", GetLastError());
    }
    free(buf);
    return ok;
}

// --- Direct SMU message via GPU driver ---
BOOL GpuSmuMsg(HANDLE h, ULONG msg, ULONG arg) {
    AMDBC250_IOCTL_PSP_SMU_MSG req;
    memset(&req, 0, sizeof(req));
    req.Message = msg;
    req.Argument = arg;
    BOOL ok = IoCtl(h, IOCTL_GPU_PSP_SMU_MSG, &req, sizeof(req), &req, sizeof(req));
    if (ok) {
        Log("SMU: msg=0x%X arg=0x%X => resp=0x%X status=%u result=%u\n",
            msg, arg, req.Response, req.ResponseStatus, req.Result);
    } else {
        Log("SMU FAILED (err=%lu)\n", GetLastError());
    }
    return ok;
}

static const char *FwTypeName(ULONG t) {
    switch (t) {
        case 1: return "ME";   case 2: return "PFP";  case 3: return "CE";
        case 4: return "MEC";  case 5: return "MEC2"; case 8: return "RLC";
        case 9: return "SDMA"; case 10: return "SDMA1"; default: return "?";
    }
}

int main(int argc, char *argv[]) {
    if (argc < 2) {
        printf("Usage: %s [options]\n"
               "  -i <phys> <size> [flags]  Init HW (e.g. 0xFE800000 0x80000 1)\n"
               "  -r <offset>               Read register via BAR5 proxy\n"
               "  -w <offset> <val>         Write register via BAR5 proxy\n"
               "  -m                        Read C2PMSG_81 (SOS alive)\n"
               "  -L <type> <file>          Load firmware via PSP mailbox\n"
               "  -A                        Load ALL GPU firmware\n"
               "  -S <msg> <arg>            Send SMU message\n"
               "  -G                        Get SMU version + features\n"
               "  -t                        Connectivity test\n"
               "  -l <logfile>              Log to file\n", argv[0]);
        return 1;
    }

    const char *logfile = NULL;
    for (int i = 1; i < argc; i++)
        if (strcmp(argv[i], "-l") == 0 && i + 1 < argc) logfile = argv[++i];
    if (logfile) g_log = fopen(logfile, "w");
    GetModuleFileNameA(NULL, g_exeDir, sizeof(g_exeDir));
    char *p = strrchr(g_exeDir, '\\');
    if (p) *p = '\0'; // strip EXE name, keep dir

    Log("=== AMD BC-250 PSP Test Tool (via GPU driver) ===\n");
    Log("Opening: %ls\n", GPU_DEVICE_NAME);
    HANDLE h = OpenGpuDriver();
    if (h == INVALID_HANDLE_VALUE) {
        Log("ERROR: cannot open GPU driver (err=%lu)\n", GetLastError());
        if (g_log) fclose(g_log); return 1;
    }
    Log("OK!\n");

    int ret = 0, i = 1;
    while (i < argc) {
        if (strcmp(argv[i], "-i") == 0 && i + 2 < argc) {
            ULONG64 phys = strtoull(argv[++i], NULL, 0);
            ULONG sz = (ULONG)strtoul(argv[++i], NULL, 0);
            ULONG flags = (i + 1 < argc && argv[i+1][0] != '-') ?
                (ULONG)strtoul(argv[++i], NULL, 0) : 1;
            Log("INIT_HW(0x%llX, %u, %u)... ", phys, sz, flags);
            BOOL ok = GpuInitHw(h, phys, sz, flags);
            Log("%s\n", ok ? "OK" : "FAIL");
            if (!ok) ret = 1;
        }
        else if (strcmp(argv[i], "-r") == 0 && i + 1 < argc) {
            ULONG off = (ULONG)strtoul(argv[++i], NULL, 0);
            ULONG v = 0;
            if (GpuReadReg(h, off, &v)) Log("REG[0x%X] = 0x%08X\n", off, v);
            else { Log("READ FAILED\n"); ret = 1; }
        }
        else if (strcmp(argv[i], "-w") == 0 && i + 2 < argc) {
            ULONG off = (ULONG)strtoul(argv[++i], NULL, 0);
            ULONG val = (ULONG)strtoul(argv[++i], NULL, 0);
            Log("WRITE[0x%X] = 0x%08X: %s\n", off, val,
                GpuWriteReg(h, off, val) ? "OK" : "FAIL");
        }
        else if (strcmp(argv[i], "-m") == 0) {
            ULONG v = 0;
            if (GpuReadReg(h, 0x10614, &v))
                Log("C2PMSG_81 = 0x%08X%s\n", v,
                    v == 0xF0000010 ? " (SOS ALIVE)" : (v & 0x80000000) ? " (SOS_RESP)" : "");
            else { Log("FAILED\n"); ret = 1; }
        }
        else if (strcmp(argv[i], "-L") == 0 && i + 2 < argc) {
            ULONG type = (ULONG)strtoul(argv[++i], NULL, 0);
            Log("LOAD_IP_FW[%s]: %s\n", FwTypeName(type), argv[i+1]);
            if (!GpuLoadIpFw(h, type, argv[++i])) ret = 1;
        }
        else if (strcmp(argv[i], "-A") == 0) {
            struct { ULONG t; const char *n; } fw[] = {
                {3,"cyan_skillfish2_ce.bin"},{2,"cyan_skillfish2_pfp.bin"},
                {1,"cyan_skillfish2_me.bin"},{4,"cyan_skillfish2_mec.bin"},
                {5,"cyan_skillfish2_mec2.bin"},{8,"cyan_skillfish2_rlc.bin"},
                {9,"cyan_skillfish2_sdma.bin"},{10,"cyan_skillfish2_sdma1.bin"},
            };
            int okCount = 0, n = sizeof(fw)/sizeof(fw[0]);
            Log("=== Loading ALL GPU FW ===\n");
            for (int j = 0; j < n; j++) {
                Log("[%d/%d] %s... ", j+1, n, FwTypeName(fw[j].t));
                if (GpuLoadIpFw(h, fw[j].t, fw[j].n)) okCount++;
            }
            Log("=== %d/%d loaded ===\n", okCount, n);
            if (okCount != n) ret = 1;
        }
        else if (strcmp(argv[i], "-S") == 0 && i + 2 < argc) {
            ULONG msg = (ULONG)strtoul(argv[++i], NULL, 0);
            ULONG arg = (ULONG)strtoul(argv[++i], NULL, 0);
            if (!GpuSmuMsg(h, msg, arg)) ret = 1;
        }
        else if (strcmp(argv[i], "-G") == 0) {
            if (GpuSmuMsg(h, 0x02, 0)) { GpuSmuMsg(h, 0x03, 0); GpuSmuMsg(h, 0x3D, 0); GpuSmuMsg(h, 0x37, 0); GpuSmuMsg(h, 0x1E, 0); }
        }
        else if (strcmp(argv[i], "-t") == 0) {
            ULONG v = 0;
            Log("=== Connectivity Test ===\n");
            Log("C2PMSG_35(0x1056C) = ");
            Log(GpuReadReg(h, 0x1056C, &v) ? "0x%08X\n" : "FAIL\n", v);
            Log("C2PMSG_81(0x10614) = ");
            Log(GpuReadReg(h, 0x10614, &v) ? "0x%08X\n" : "FAIL\n", v);
            Log("GPU_ID(0x0000)     = ");
            Log(GpuReadReg(h, 0x0000, &v) ? "0x%08X\n" : "FAIL\n", v);
            Log("GRBM_GFX_INDEX(0x34D0) = ");
            Log(GpuReadReg(h, 0x34D0, &v) ? "0x%08X\n" : "FAIL\n", v);
            Log("=== Done ===\n");
        }
        else if (strcmp(argv[i], "-l") == 0) { i++; }
        else { Log("Unknown: %s\n", argv[i]); ret = 1; }
        i++;
    }

    CloseHandle(h);
    if (g_log) fclose(g_log);
    return ret;
}
