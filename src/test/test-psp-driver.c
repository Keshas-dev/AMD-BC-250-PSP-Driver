#define _CRT_SECURE_NO_WARNINGS
#include <windows.h>
#include <stdio.h>
#include <stdarg.h>
#include <stdlib.h>
#include <string.h>

// Include shared IOCTL definitions
#include "PspIoctl.h"

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

void PrintUsage(const char *prog)
{
    printf("Usage: %s [options]\n", prog);
    printf("Options:\n");
    printf("  -r <offset>        Read register at offset (hex)\n");
    printf("  -w <offset> <val>  Write value to register at offset (hex)\n");
    printf("  -f <file>          Load firmware file via Mailbox (C2PMSG_35/36/81)\n");
    printf("  -m                 Read mailbox status (C2PMSG_81)\n");
    printf("  -t                 Run basic connectivity test\n");
    printf("  -l <logfile>       Write log to file\n");
    printf("\nExamples:\n");
    printf("  %s -r 0x1056C     Read C2PMSG_35 (command register)\n", prog);
    printf("  %s -m             Read C2PMSG_81 (status register)\n", prog);
    printf("  %s -f fw.bin      Load firmware file\n", prog);
    printf("  %s -t             Run connectivity test\n", prog);
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
    Log("Opening driver: %S\n", PSP_DEVICE_NAME);

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
        else if (strcmp(argv[i], "-f") == 0 && i + 1 < argc) {
            const char* filename = argv[++i];
            Log("LOAD_FIRMWARE: %s\n", filename);
            ok = LoadFirmware(h, filename);
            if (!ok) {
                ret = 1;
            }
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
