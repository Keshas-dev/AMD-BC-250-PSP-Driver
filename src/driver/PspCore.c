#include <ntddk.h>
#include <wdm.h>
#include "PspIoctl.h"
#include "firmware_data.h"
#include "PspCore.h"

#define GPU_BAR5_SIZE          0x80000ULL

// BUS_DATA_TYPE for PCI config access via HAL
#define PCIConfiguration 0

// LKML-discovered PSP BAR0 (Mattia Tadini 2026-09-19): fe700000 [1MB]
#define PSP_LKML_BAR0          0xFE700000ULL
#define PSP_LKML_BAR0_SIZE     0x100000ULL
// Secondary BAR1: fe884000 [8KB]
#define PSP_LKML_BAR1          0xFE884000ULL
#define PSP_LKML_BAR1_SIZE     0x2000ULL

extern PVOID g_Bar5Mapping;
extern SIZE_T g_Bar5Size;
extern KSPIN_LOCK g_Bar5MappingLock;
extern BOOLEAN g_GpuProxyAvailable;
extern HANDLE g_GpuDriverHandle;

/* GPU driver uses raw IOCTL values 0x900/0x901 in its switch statement,
 * NOT CTL_CODE values. We must match exactly.
 * NOTE: Named IOCTL_AMDBC250_BAR5_READ_PROXY_RAW to avoid collision with
 * GPU driver's CTL_CODE version (0x80000BCC) which is a different value. */
#define IOCTL_AMDBC250_BAR5_READ_PROXY_RAW  0x900
#define IOCTL_AMDBC250_BAR5_WRITE_PROXY_RAW 0x901

static BOOLEAN g_GpuProxyInitialized = FALSE;

NTSTATUS PspOpenGpuDriver(void)
{
    UNICODE_STRING devName;
    OBJECT_ATTRIBUTES objAttr;
    IO_STATUS_BLOCK ioStatus;
    LARGE_INTEGER allocSize;
    NTSTATUS status;
    
    RtlInitUnicodeString(&devName, L"\\Device\\AMDBC250DreamV43");
    InitializeObjectAttributes(&objAttr, &devName, OBJ_CASE_INSENSITIVE | OBJ_KERNEL_HANDLE, NULL, NULL);
    allocSize.QuadPart = 0;
    
    /* FIX 2026-09-23: FILE_SYNCHRONOUS_IO_NONALERT as CreateOptions (9th arg)
     * — ShareAccess is 7th. Without it ZwDeviceIoControlFile(NULL event)
     * can return STATUS_PENDING and leave the proxy read hanging. */
    status = ZwCreateFile(&g_GpuDriverHandle,
                          GENERIC_READ | GENERIC_WRITE,
                          &objAttr,
                          &ioStatus,
                          &allocSize,
                          FILE_ATTRIBUTE_NORMAL,
                          0,                              /* ShareAccess: exclusive */
                          FILE_OPEN,                      /* CreateDisposition */
                          FILE_SYNCHRONOUS_IO_NONALERT,   /* CreateOptions */
                          NULL,
                          0);
    
    if (NT_SUCCESS(status)) {
        KdPrint(("PSP_GPU_PROXY: Opened GPU driver handle=%p\n", g_GpuDriverHandle));
    } else {
        KdPrint(("PSP_GPU_PROXY: Failed to open GPU driver: 0x%08X\n", status));
    }
    
    return status;
}

NTSTATUS PspGpuProxyInit(PDEVICE_EXTENSION devExt)
{
    ULONG testValue;
    KIRQL irql;
    HANDLE localHandle = NULL;
    HANDLE staleHandle = NULL;
    NTSTATUS status;

    /* FIX 2026-09-23: serialize proxy open/test under CommandLock (KMUTEX is
     * recursive — safe if caller already holds it). Concurrent GET_STATUS /
     * auto-init raced: double ZwCreateFile overwrote g_GpuDriverHandle and
     * left ZwDeviceIoControlFile(0x900) hanging without FILE_SYNCHRONOUS.
     * ZwClose ONLY at PASSIVE_LEVEL outside spinlock (IRQL violation otherwise). */
    (void)KeWaitForSingleObject(&devExt->CommandLock, Executive, KernelMode, FALSE, NULL);

    KeAcquireSpinLock(&g_Bar5MappingLock, &irql);

    if (g_GpuProxyAvailable && g_GpuDriverHandle != NULL) {
        KeReleaseSpinLock(&g_Bar5MappingLock, irql);
        KeReleaseMutex(&devExt->CommandLock, FALSE);
        return STATUS_SUCCESS;
    }

    if (g_GpuDriverHandle == NULL) {
        KeReleaseSpinLock(&g_Bar5MappingLock, irql);
        status = PspOpenGpuDriver();
        if (!NT_SUCCESS(status)) {
            KdPrint(("PSP_GPU_PROXY: Failed to open GPU driver: 0x%08X\n", status));
            KeReleaseMutex(&devExt->CommandLock, FALSE);
            return status;
        }
        KeAcquireSpinLock(&g_Bar5MappingLock, &irql);
        localHandle = g_GpuDriverHandle;
        /* Check if another thread already initialized the proxy */
        if (g_GpuProxyAvailable) {
            staleHandle = localHandle;
            g_GpuDriverHandle = NULL;
            KeReleaseSpinLock(&g_Bar5MappingLock, irql);
            if (staleHandle != NULL) {
                ZwClose(staleHandle); /* PASSIVE — outside spinlock */
            }
            KeReleaseMutex(&devExt->CommandLock, FALSE);
            return STATUS_SUCCESS;
        }
    }

    KeReleaseSpinLock(&g_Bar5MappingLock, irql);

    testValue = PspGpuProxyReadRegister(0);
    KdPrint(("PSP_GPU_PROXY: Test read from offset 0: 0x%08X\n", testValue));

    if (testValue == 0xFFFFFFFF) {
        KdPrint(("PSP_GPU_PROXY: GPU driver proxy not responding correctly\n"));
        KeAcquireSpinLock(&g_Bar5MappingLock, &irql);
        /* Close pre-existing stale handle too (localHandle may be NULL) */
        if (g_GpuDriverHandle != NULL &&
            (localHandle == NULL || g_GpuDriverHandle == localHandle)) {
            staleHandle = g_GpuDriverHandle;
            g_GpuDriverHandle = NULL;
        } else {
            staleHandle = NULL;
        }
        KeReleaseSpinLock(&g_Bar5MappingLock, irql);
        if (staleHandle != NULL) {
            ZwClose(staleHandle); /* PASSIVE — outside spinlock */
        }
        KeReleaseMutex(&devExt->CommandLock, FALSE);
        return STATUS_DEVICE_NOT_READY;
    }

    KeAcquireSpinLock(&g_Bar5MappingLock, &irql);
    g_GpuProxyAvailable = TRUE;
    g_GpuProxyInitialized = TRUE;
    KeReleaseSpinLock(&g_Bar5MappingLock, irql);
    KeReleaseMutex(&devExt->CommandLock, FALSE);
    return STATUS_SUCCESS;
}

ULONG PspGpuProxyReadRegister(ULONG offset)
{
    KIRQL irql;
    HANDLE localHandle;
    BOOLEAN useProxy = FALSE;

    KeAcquireSpinLock(&g_Bar5MappingLock, &irql);
    /* 2026-09-23: never direct-map-read GPU BAR5 — atikmdag owns that window.
     * Direct g_Bar5Mapping access raced the GPU driver (0x1E). Always proxy IOCTL. */
    if (g_GpuDriverHandle != NULL) {
        localHandle = g_GpuDriverHandle;
        useProxy = TRUE;
    }
    KeReleaseSpinLock(&g_Bar5MappingLock, irql);
    
    if (useProxy) {
        ULONG inputOffset = offset;
        ULONG outputValue = 0;
        IO_STATUS_BLOCK ioStatus;
        NTSTATUS status;
        
        status = ZwDeviceIoControlFile(localHandle, NULL, NULL, NULL, &ioStatus,
                                        IOCTL_AMDBC250_BAR5_READ_PROXY_RAW,
                                        &inputOffset, sizeof(inputOffset), 
                                        &outputValue, sizeof(outputValue));
        if (NT_SUCCESS(status)) {
            return outputValue;
        }
        KdPrint(("PSP_GPU_PROXY: Read failed: status=0x%08X, ioStatus=0x%08X\n", status, ioStatus.Status));
    }
    
    return 0xFFFFFFFF;
}

BOOLEAN PspGpuProxyWriteRegister(ULONG offset, ULONG value)
{
    KIRQL irql;
    HANDLE localHandle;
    BOOLEAN useProxy = FALSE;

    KeAcquireSpinLock(&g_Bar5MappingLock, &irql);
    /* 2026-09-23: never direct-map-write GPU BAR5 — proxy only (see ReadRegister). */
    if (g_GpuDriverHandle != NULL) {
        localHandle = g_GpuDriverHandle;
        useProxy = TRUE;
    }
    KeReleaseSpinLock(&g_Bar5MappingLock, irql);
    
    if (useProxy) {
        ULONG params[2] = {offset, value};
        IO_STATUS_BLOCK ioStatus;
        NTSTATUS status;
        
        status = ZwDeviceIoControlFile(localHandle, NULL, NULL, NULL, &ioStatus,
                                        IOCTL_AMDBC250_BAR5_WRITE_PROXY_RAW,
                                        params, sizeof(params), NULL, 0);
        return NT_SUCCESS(status);
    }
    
    return FALSE;
}

PVOID g_TmrBuffer = NULL;
PHYSICAL_ADDRESS g_TmrPhysical = {0};
ULONG g_TmrSize = 0;
BOOLEAN g_TmrInitialized = FALSE;

PVOID g_KiqRingVa = NULL;
PHYSICAL_ADDRESS g_KiqRingPa = {0};
ULONG g_KiqRingSize = 0;
ULONG g_KiqRingWptr = 0;
BOOLEAN g_KiqRingInitialized = FALSE;
KMUTEX g_KiqRingLock;

BOOLEAN PspValidateFirmware(PUCHAR FirmwareData, ULONG FirmwareSize)
{
    if (FirmwareData == NULL || FirmwareSize < 256)
        return FALSE;

    if (FirmwareSize < 1024 || FirmwareSize > PSP_MAX_FW_TOTAL)
        return FALSE;

    ULONG sampleStart = *(volatile ULONG*)FirmwareData;
    ULONG sampleMid = *(volatile PULONG)(FirmwareData + FirmwareSize / 2);
    if (sampleStart == 0 && sampleMid == 0)
        return FALSE;
    if (sampleStart == 0xFFFFFFFF && sampleMid == 0xFFFFFFFF)
        return FALSE;

    KdPrint(("FW validation: size=%u first=0x%08X mid=0x%08X -> OK\n",
        FirmwareSize, sampleStart, sampleMid));
    return TRUE;
}

VOID PspFreeFirmware(PDEVICE_EXTENSION devExt)
{
    if (devExt->FwBuffer) {
        MmFreeContiguousMemory(devExt->FwBuffer);
        devExt->FwBuffer = NULL;
        devExt->FwSize = 0;
        devExt->FwPhysical.QuadPart = 0;
        devExt->FwPaShifted = 0;
        KdPrint(("Firmware buffer freed\n"));
    }
}

NTSTATUS PspSendMailboxCommand(PDEVICE_EXTENSION devExt, ULONG command)
{
    ULONG timeout;
    ULONG cmdReg;

    if (devExt->FwBuffer == NULL) {
        KdPrint(("No firmware loaded\n"));
        return STATUS_NO_MEMORY;
    }

    /* C2PMSG lives on GPU BAR5 MP0 — always GPU proxy (never dual-map). */
    if (!g_GpuProxyAvailable) {
        NTSTATUS proxyStatus = PspGpuProxyInit(devExt);
        if (!NT_SUCCESS(proxyStatus)) {
            KdPrint(("Mailbox: GPU proxy unavailable 0x%08X\n", proxyStatus));
            return STATUS_DEVICE_NOT_READY;
        }
    }

    /* KMUTEX: covers proxy Zw (PASSIVE) — never spinlock across Zw */
    (void)KeWaitForSingleObject(&devExt->CommandLock, Executive, KernelMode, FALSE, NULL);

    if (!PspGpuProxyWriteRegister(PSP_C2PMSG_36_OFFSET, (ULONG)(devExt->FwPhysical.QuadPart & 0xFFFFFFFF)) ||
        !PspGpuProxyWriteRegister(PSP_C2PMSG_37_OFFSET, (ULONG)(devExt->FwPhysical.QuadPart >> 32)) ||
        !PspGpuProxyWriteRegister(PSP_C2PMSG_35_OFFSET, command)) {
        KeReleaseMutex(&devExt->CommandLock, FALSE);
        KdPrint(("Mailbox: GPU proxy write failed\n"));
        return STATUS_DEVICE_NOT_READY;
    }
    KdPrint(("Mailbox: PA=0x%llX cmd=0x%08X written via GPU proxy\n",
        devExt->FwPhysical.QuadPart, command));

    KeReleaseMutex(&devExt->CommandLock, FALSE);

    for (timeout = 0; timeout < PSP_FW_WAIT_MS; timeout++) {
        KeStallExecutionProcessor(1000);
        cmdReg = PspGpuProxyReadRegister(PSP_C2PMSG_35_OFFSET);
        if (cmdReg == 0) {
            KdPrint(("Mailbox: C2PMSG_35 cleared after %u ms (cmd 0x%08X)\n",
                timeout, command));
            break;
        }
    }

    if (timeout >= PSP_FW_WAIT_MS) {
        KdPrint(("Mailbox: TIMEOUT C2PMSG_35 stuck at 0x%08X (cmd 0x%08X)\n",
            cmdReg, command));
        return STATUS_TIMEOUT;
    }

    return STATUS_SUCCESS;
}

NTSTATUS PspLoadIpFwViaMailbox(PDEVICE_EXTENSION devExt, ULONG FwType, ULONG FwSize, PUCHAR FwData)
{
    NTSTATUS status;
    PVOID cmdBufVa = NULL;
    PHYSICAL_ADDRESS cmdBufPa = {0};
    PVOID fwBufVa = NULL;
    PHYSICAL_ADDRESS fwBufPa = {0};
    ULONG timeout;
    ULONG cmdReg = 0;
    volatile PULONG cmdDwords;

    if (FwSize == 0 || FwData == NULL || FwSize > 0x100000) {
        return STATUS_INVALID_PARAMETER;
    }

    /* C2PMSG mailbox = GPU proxy only (never dual-map BAR5). */
    if (!g_GpuProxyAvailable) {
        NTSTATUS proxyStatus = PspGpuProxyInit(devExt);
        if (!NT_SUCCESS(proxyStatus)) {
            KdPrint(("LOAD_IP_FW_MAILBOX: GPU proxy init failed: 0x%08X\n", proxyStatus));
            return STATUS_DEVICE_NOT_READY;
        }
    }

    /* Allocate 1024-byte command buffer (non-cached, below 4GB) */
    {
        PHYSICAL_ADDRESS low = {0}, high = {0}, boundary = {0};
        high.QuadPart = 0xFFFFFFFFULL;
        cmdBufVa = MmAllocateContiguousMemorySpecifyCache(
            1024, low, high, boundary, MmNonCached);
    }
    if (!cmdBufVa) {
        return STATUS_INSUFFICIENT_RESOURCES;
    }
    RtlZeroMemory(cmdBufVa, 1024);
    cmdBufPa = MmGetPhysicalAddress(cmdBufVa);

    /* Allocate non-cached buffer for firmware data */
    {
        PHYSICAL_ADDRESS low = {0}, high = {0}, boundary = {0};
        high.QuadPart = 0xFFFFFFFFULL;
        fwBufVa = MmAllocateContiguousMemorySpecifyCache(
            FwSize, low, high, boundary, MmNonCached);
    }
    if (!fwBufVa) {
        MmFreeContiguousMemory(cmdBufVa);
        return STATUS_INSUFFICIENT_RESOURCES;
    }
    RtlCopyMemory(fwBufVa, FwData, FwSize);
    fwBufPa = MmGetPhysicalAddress(fwBufVa);

    /* Fill command buffer in PSP ring frame format:
     * Offset  0: buf_size  = 1024
     * Offset  4: body_size = 4 (dwords of params)
     * Offset  8: cmd_id    = 0x06 (LOAD_IP_FW)
     * Offset 12-24: reserved
     * Offset 28: fw_pa_lo
     * Offset 32: fw_pa_hi
     * Offset 36: fw_size
     * Offset 40: fw_type   */
    cmdDwords = (volatile PULONG)cmdBufVa;
    cmdDwords[0]  = 1024;                       /* buf_size */
    cmdDwords[1]  = 4;                          /* body_size (dwords) */
    cmdDwords[2]  = GFX_CMD_ID_LOAD_IP_FW;     /* cmd_id = 0x06 */
    cmdDwords[3]  = 0;                          /* reserved */
    cmdDwords[4]  = 0;                          /* reserved */
    cmdDwords[5]  = 0;                          /* reserved */
    cmdDwords[6]  = 0;                          /* reserved */
    cmdDwords[7]  = (ULONG)(fwBufPa.QuadPart & 0xFFFFFFFF);  /* fw_pa_lo */
    cmdDwords[8]  = (ULONG)(fwBufPa.QuadPart >> 32);         /* fw_pa_hi */
    cmdDwords[9]  = FwSize;                     /* fw_size */
    cmdDwords[10] = FwType;                     /* fw_type */

    KdPrint(("LOAD_IP_FW_MAILBOX: type=%u size=%u cmdBufPA=0x%llX fwPA=0x%llX\n",
             FwType, FwSize, cmdBufPa.QuadPart, fwBufPa.QuadPart));

    /* Send via mailbox: C2PMSG_36/37 = command buffer PA, C2PMSG_35 = 0x06 — KMUTEX for Zw */
    if (!g_GpuProxyAvailable) {
        KdPrint(("LOAD_IP_FW_MAILBOX: no GPU proxy\n"));
        MmFreeContiguousMemory(cmdBufVa);
        MmFreeContiguousMemory(fwBufVa);
        return STATUS_DEVICE_NOT_READY;
    }
    (void)KeWaitForSingleObject(&devExt->CommandLock, Executive, KernelMode, FALSE, NULL);

    if (!PspGpuProxyWriteRegister(PSP_C2PMSG_36_OFFSET, (ULONG)(cmdBufPa.QuadPart & 0xFFFFFFFF)) ||
        !PspGpuProxyWriteRegister(PSP_C2PMSG_37_OFFSET, (ULONG)(cmdBufPa.QuadPart >> 32)) ||
        !PspGpuProxyWriteRegister(PSP_C2PMSG_35_OFFSET, GFX_CMD_ID_LOAD_IP_FW)) {
        KeReleaseMutex(&devExt->CommandLock, FALSE);
        KdPrint(("LOAD_IP_FW_MAILBOX: GPU proxy write failed\n"));
        MmFreeContiguousMemory(cmdBufVa);
        MmFreeContiguousMemory(fwBufVa);
        return STATUS_DEVICE_NOT_READY;
    }

    KeReleaseMutex(&devExt->CommandLock, FALSE);

    /* Wait for completion (C2PMSG_35 clears to 0) — proxy only */
    for (timeout = 0; timeout < PSP_FW_WAIT_MS; timeout++) {
        KeStallExecutionProcessor(1000);
        cmdReg = PspGpuProxyReadRegister(PSP_C2PMSG_35_OFFSET);
        if (cmdReg == 0) {
            KdPrint(("LOAD_IP_FW_MAILBOX: Completed after %u ms\n", timeout));
            break;
        }
    }

    /* Read C2PMSG_81 for status — proxy only */
    {
        ULONG c2pmsg81 = PspGpuProxyReadRegister(PSP_C2PMSG_81_OFFSET);
        KdPrint(("LOAD_IP_FW_MAILBOX: C2PMSG_81=0x%08X timeout=%u cmdReg=0x%08X\n",
                 c2pmsg81, timeout, cmdReg));

        if (timeout >= PSP_FW_WAIT_MS) {
            KdPrint(("LOAD_IP_FW_MAILBOX: TIMEOUT C2PMSG_35=0x%08X\n", cmdReg));
            status = STATUS_TIMEOUT;
        } else if (c2pmsg81 == 0xF0000010 || c2pmsg81 == 0) {
            /* C2PMSG_81 = 0xF0000010 means SOS is alive / command accepted.
             * 0 means SOS cleared the status after a successful load.
             * Both indicate success. */
            status = STATUS_SUCCESS;
        } else {
            KdPrint(("LOAD_IP_FW_MAILBOX: PSP error C2PMSG_81=0x%08X\n", c2pmsg81));
            status = STATUS_UNSUCCESSFUL;
        }
    }

    /* Cleanup */
    MmFreeContiguousMemory(cmdBufVa);
    MmFreeContiguousMemory(fwBufVa);
    return status;
}

NTSTATUS PspInitTmr(PDEVICE_EXTENSION devExt)
{
    UNREFERENCED_PARAMETER(devExt);
    KdPrint(("TMR: Init requested (ring protocol not supported on this SOS)\n"));
    if (g_KiqRingInitialized) {
        g_TmrInitialized = TRUE;
        return STATUS_SUCCESS;
    }
    return STATUS_DEVICE_NOT_READY;
}

VOID PspFreeTmr(VOID)
{
    if (g_TmrBuffer) {
        MmFreeContiguousMemory(g_TmrBuffer);
        g_TmrBuffer = NULL;
        g_TmrPhysical.QuadPart = 0;
        g_TmrSize = 0;
        g_TmrInitialized = FALSE;
        KdPrint(("TMR buffer freed\n"));
    }
}

/* LKML fix (Mattia Tadini 2026-09-19): PSP 1022:143E has DISABLED memory
 * windows at fe700000 [1MB] + fe884000 [8KB]. PCI Command Register bit 1
 * (Memory Space Enable) must be set BEFORE MmMapIoSpace, otherwise the
 * windows are deaf (reads return 0xFFFFFFFF / no response).
 * Returns: physical address of PSP BAR0, or 0 if not found. */
static ULONGLONG PspEnablePciMemory(VOID)
{
    /* Scan bus 0-1 for VEN_1022 DEV_143E (CPU PSP) */
    for (ULONG bus = 0; bus < 2; bus++) {
        for (ULONG slot = 0; slot < 32; slot++) {
            for (ULONG func = 0; func < 8; func++) {
                ULONG devFn = (slot << 5) | func;
                ULONG idReg = 0;
                HalGetBusDataByOffset(PCIConfiguration, bus, devFn, &idReg, 0, sizeof(ULONG));
                if (idReg == 0xFFFFFFFF || idReg == 0) continue;
                /* VEN_1022 = low 16 bits, DEV_143E = high 16 bits */
                if ((idReg & 0xFFFF) == 0x1022 && ((idReg >> 16) & 0xFFFF) == 0x143E) {
                    /* Read current Command register (offset 0x04) */
                    ULONG cmd = 0;
                    HalGetBusDataByOffset(PCIConfiguration, bus, devFn, &cmd, 0x04, sizeof(ULONG));
                    KdPrint(("PSP_PCI: Found 1022:143E at B%u.D%u.F%u cmd=0x%08X\n", bus, slot, func, cmd));
                    /* Enable IO Space (bit0) + Memory Space (bit1) + Bus Master (bit2) */
                    ULONG newCmd = cmd | 0x7;
                    if (newCmd != cmd) {
                        HalSetBusDataByOffset(PCIConfiguration, bus, devFn, &newCmd, 0x04, sizeof(ULONG));
                        KeStallExecutionProcessor(100);
                        HalGetBusDataByOffset(PCIConfiguration, bus, devFn, &cmd, 0x04, sizeof(ULONG));
                        KdPrint(("PSP_PCI: Enabled memory, cmd now=0x%08X\n", cmd));
                    }
                    /* Read BAR0 (offset 0x10) for real PSP MMIO base */
                    ULONG bar0 = 0;
                    HalGetBusDataByOffset(PCIConfiguration, bus, devFn, &bar0, 0x10, sizeof(ULONG));
                    /* Mask out type bits [2:0] (0=32-bit, 1=64-bit, 2=64-bit prefetch) */
                    ULONGLONG bar0Phys = (ULONGLONG)(bar0 & ~0xFU);
                    KdPrint(("PSP_PCI: BAR0 raw=0x%08X phys=0x%llX\n", bar0, bar0Phys));
                    return bar0Phys;
                }
            }
        }
    }
    KdPrint(("PSP_PCI: 1022:143E not found on bus 0-1\n"));
    return 0;
}

NTSTATUS PspAutoInitialize(PDEVICE_EXTENSION devExt)
{
    if (devExt->Bar0Base == NULL) {
        /* LKML fix: enable PCI device memory BEFORE mapping */
        ULONGLONG pspBar0 = PspEnablePciMemory();

        PHYSICAL_ADDRESS physAddr;
        if (pspBar0 != 0) {
            /* Use the discovered BAR0 (fe700000) */
            physAddr.QuadPart = (LONGLONG)pspBar0;
            KdPrint(("PSP: mapping discovered BAR0 at 0x%llX\n", physAddr.QuadPart));
        } else {
            /* Fallback to stale hardcoded address */
            physAddr.QuadPart = PSP_BAR0_PHYSICAL;
            KdPrint(("PSP: 1022:143E not found, fallback to 0x%llX\n", physAddr.QuadPart));
        }
        devExt->Bar0Base = MmMapIoSpace(physAddr, PSP_BAR0_SIZE, MmNonCached);
        if (devExt->Bar0Base == NULL) {
            KdPrint(("PSP: BAR0 map failed at 0x%llX\n", physAddr.QuadPart));
        } else {
            devExt->Bar0Size = PSP_BAR0_SIZE;
            devExt->MmioBase = devExt->Bar0Base;
            devExt->MmioSize = PSP_BAR0_SIZE;
            KdPrint(("PSP: BAR0 mapped: PA=0x%llX VA=%p size=%u\n",
                physAddr.QuadPart, devExt->Bar0Base, devExt->Bar0Size));
        }
    }

    /* 2026-09-23: NEVER MmMapIoSpace GPU BAR5 (0xFE800000) — atikmdag owns it.
     * A second map races with the GPU driver (0x1E BSOD). GPU access = proxy only. */
    if (devExt->GpuMmioBase != NULL) {
        KIRQL irql;
        KeAcquireSpinLock(&g_Bar5MappingLock, &irql);
        g_Bar5Mapping = NULL;
        g_Bar5Size = 0;
        KeReleaseSpinLock(&g_Bar5MappingLock, irql);
        MmUnmapIoSpace(devExt->GpuMmioBase, devExt->GpuMmioSize);
        devExt->GpuMmioBase = NULL;
        devExt->GpuMmioSize = 0;
        KdPrint(("PSP: released legacy GPU BAR5 dual-map (now proxy-only)\n"));
    }

    if (devExt->Bar0Base == NULL) {
        KdPrint(("PSP: Auto-init FAILED — no PSP BAR0 mapping\n"));
        return STATUS_DEVICE_NOT_READY;
    }

    /* Best-effort GPU proxy; PSP BAR0 alone is enough for device to stay up. */
    {
        NTSTATUS proxySt = PspGpuProxyInit(devExt);
        KdPrint(("PSP: Auto-init GPU proxy=0x%08X handle=%p avail=%d\n",
            proxySt, g_GpuDriverHandle, (int)g_GpuProxyAvailable));
    }
    return STATUS_SUCCESS;
}

/* Load firmware file from disk into a non-paged pool buffer.
 * FileName format: L"\\SystemRoot\\System32\\drivers\\amdgpu\\navi10_smc.bin"
 * Caller must free *OutData with ExFreePoolWithTag. */
NTSTATUS PspLoadFirmwareFromFile(PCWSTR FileName, PUCHAR* OutData, PULONG OutSize)
{
    HANDLE hFile = NULL;
    OBJECT_ATTRIBUTES objAttr;
    IO_STATUS_BLOCK ioStatus;
    UNICODE_STRING uniPath;
    FILE_STANDARD_INFORMATION fileInfo;
    NTSTATUS status;
    PUCHAR buffer = NULL;

    if (FileName == NULL || OutData == NULL || OutSize == NULL) {
        return STATUS_INVALID_PARAMETER;
    }
    *OutData = NULL;
    *OutSize = 0;

    RtlInitUnicodeString(&uniPath, FileName);
    InitializeObjectAttributes(&objAttr, &uniPath, OBJ_CASE_INSENSITIVE | OBJ_KERNEL_HANDLE, NULL, NULL);

    status = ZwCreateFile(&hFile, GENERIC_READ, &objAttr, &ioStatus, NULL,
                          FILE_ATTRIBUTE_NORMAL, FILE_SHARE_READ, FILE_OPEN,
                          FILE_SYNCHRONOUS_IO_NONALERT, NULL, 0);
    if (!NT_SUCCESS(status)) {
        KdPrint(("FW_FILE: Failed to open %wZ (0x%08X)\n", &uniPath, status));
        return status;
    }

    status = ZwQueryInformationFile(hFile, &ioStatus, &fileInfo, sizeof(fileInfo), FileStandardInformation);
    if (!NT_SUCCESS(status)) {
        KdPrint(("FW_FILE: Query info failed (0x%08X)\n", status));
        ZwClose(hFile);
        return status;
    }

    ULONG fileSize = (ULONG)fileInfo.EndOfFile.QuadPart;
    if (fileSize == 0 || fileSize > PSP_MAX_FW_TOTAL) {
        KdPrint(("FW_FILE: Invalid file size=%u\n", fileSize));
        ZwClose(hFile);
        return STATUS_FILE_TOO_LARGE;
    }

    buffer = (PUCHAR)ExAllocatePoolWithTag(NonPagedPool, fileSize, 'fw');
    if (buffer == NULL) {
        KdPrint(("FW_FILE: Alloc %u failed\n", fileSize));
        ZwClose(hFile);
        return STATUS_INSUFFICIENT_RESOURCES;
    }

    status = ZwReadFile(hFile, NULL, NULL, NULL, &ioStatus, buffer, fileSize, NULL, NULL);
    ZwClose(hFile);

    if (!NT_SUCCESS(status)) {
        KdPrint(("FW_FILE: Read failed (0x%08X)\n", status));
        ExFreePoolWithTag(buffer, 'fw');
        return status;
    }

    KdPrint(("FW_FILE: Loaded %wZ (%u bytes)\n", &uniPath, fileSize));
    *OutData = buffer;
    *OutSize = fileSize;
    return STATUS_SUCCESS;
}