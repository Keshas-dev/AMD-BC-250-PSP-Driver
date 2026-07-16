#include <ntddk.h>
#include <wdm.h>
#include "PspIoctl.h"
#include "firmware_data.h"
#include "PspCore.h"

#define GPU_BAR5_SIZE          0x80000ULL

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
    
    status = ZwCreateFile(&g_GpuDriverHandle, 
                          GENERIC_READ | GENERIC_WRITE, 
                          &objAttr, 
                          &ioStatus, 
                          &allocSize, 
                          FILE_ATTRIBUTE_NORMAL,
                          0,
                          FILE_OPEN,
                          0,
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
    NTSTATUS status;
    
    KeAcquireSpinLock(&g_Bar5MappingLock, &irql);
    
    if (g_GpuProxyAvailable && g_GpuDriverHandle != NULL) {
        KeReleaseSpinLock(&g_Bar5MappingLock, irql);
        return STATUS_SUCCESS;
    }
    
    if (g_GpuDriverHandle == NULL) {
        KeReleaseSpinLock(&g_Bar5MappingLock, irql);
        status = PspOpenGpuDriver();
        if (!NT_SUCCESS(status)) {
            KdPrint(("PSP_GPU_PROXY: Failed to open GPU driver: 0x%08X\n", status));
            return status;
        }
        KeAcquireSpinLock(&g_Bar5MappingLock, &irql);
        localHandle = g_GpuDriverHandle;
        /* Check if another thread already initialized the proxy */
        if (g_GpuProxyAvailable) {
            ZwClose(localHandle);
            g_GpuDriverHandle = NULL;
            KeReleaseSpinLock(&g_Bar5MappingLock, irql);
            return STATUS_SUCCESS;
        }
    }
    
    KeReleaseSpinLock(&g_Bar5MappingLock, irql);
    
    testValue = PspGpuProxyReadRegister(0);
    KdPrint(("PSP_GPU_PROXY: Test read from offset 0: 0x%08X\n", testValue));
    
    if (testValue == 0xFFFFFFFF) {
        KdPrint(("PSP_GPU_PROXY: GPU driver proxy not responding correctly\n"));
        KeAcquireSpinLock(&g_Bar5MappingLock, &irql);
        if (localHandle != NULL && g_GpuDriverHandle == localHandle) {
            ZwClose(g_GpuDriverHandle);
            g_GpuDriverHandle = NULL;
        }
        KeReleaseSpinLock(&g_Bar5MappingLock, irql);
        return STATUS_DEVICE_NOT_READY;
    }
    
    KeAcquireSpinLock(&g_Bar5MappingLock, &irql);
    g_GpuProxyAvailable = TRUE;
    g_GpuProxyInitialized = TRUE;
    KeReleaseSpinLock(&g_Bar5MappingLock, irql);
    return STATUS_SUCCESS;
}

ULONG PspGpuProxyReadRegister(ULONG offset)
{
    KIRQL irql;
    HANDLE localHandle;
    BOOLEAN useProxy = FALSE;

    KeAcquireSpinLock(&g_Bar5MappingLock, &irql);
    if (g_Bar5Mapping != NULL) {
        ULONG val;
        __try {
            val = READ_REGISTER_ULONG((PULONG)((PUCHAR)g_Bar5Mapping + offset));
        } __except (EXCEPTION_EXECUTE_HANDLER) {
            KdPrint(("PSP_GPU_PROXY: Read at offset 0x%X caused exception, fallback\n", offset));
            KeReleaseSpinLock(&g_Bar5MappingLock, irql);
            return 0xFFFFFFFF;
        }
        KeReleaseSpinLock(&g_Bar5MappingLock, irql);
        return val;
    }
    /* Snapshot handle under lock — g_GpuDriverHandle is set once and never modified after init,
     * so the handle is stable once observed as non-NULL. ZwDeviceIoControlFile is called
     * outside the lock to avoid holding a spinlock during IOCTL dispatch. */
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
    if (g_Bar5Mapping != NULL) {
        __try {
            WRITE_REGISTER_ULONG((PULONG)((PUCHAR)g_Bar5Mapping + offset), value);
        } __except (EXCEPTION_EXECUTE_HANDLER) {
            KdPrint(("PSP_GPU_PROXY: Write at offset 0x%X caused exception\n", offset));
            KeReleaseSpinLock(&g_Bar5MappingLock, irql);
            return FALSE;
        }
        KeReleaseSpinLock(&g_Bar5MappingLock, irql);
        return TRUE;
    }
    /* Snapshot handle under lock — handle is stable once non-NULL (set once, never closed). */
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
KSPIN_LOCK g_KiqRingLock;

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
    KIRQL irql;
    PVOID mboxBase = devExt->GpuMmioBase ? devExt->GpuMmioBase : devExt->MmioBase;
    BOOLEAN useGpuProxy = FALSE;

    if (mboxBase == NULL && g_GpuDriverHandle != NULL) {
        NTSTATUS proxyStatus = PspGpuProxyInit(devExt);
        if (NT_SUCCESS(proxyStatus)) {
            mboxBase = g_Bar5Mapping;
            useGpuProxy = TRUE;
        }
    }

    if (mboxBase == NULL) {
        KdPrint(("Mailbox: No BAR mapped (call INIT_HW first)\n"));
        return STATUS_DEVICE_NOT_READY;
    }

    if (devExt->FwBuffer == NULL) {
        KdPrint(("No firmware loaded\n"));
        return STATUS_NO_MEMORY;
    }

    KeAcquireSpinLock(&devExt->CommandLock, &irql);

    mboxBase = (g_Bar5Mapping != NULL) ? g_Bar5Mapping : mboxBase;

    if (useGpuProxy && g_GpuProxyAvailable) {
        if (!PspGpuProxyWriteRegister(PSP_C2PMSG_36_OFFSET, (ULONG)(devExt->FwPhysical.QuadPart & 0xFFFFFFFF)) ||
            !PspGpuProxyWriteRegister(PSP_C2PMSG_37_OFFSET, (ULONG)(devExt->FwPhysical.QuadPart >> 32)) ||
            !PspGpuProxyWriteRegister(PSP_C2PMSG_35_OFFSET, command)) {
            KeReleaseSpinLock(&devExt->CommandLock, irql);
            KdPrint(("Mailbox: GPU proxy write failed\n"));
            return STATUS_DEVICE_NOT_READY;
        }
        KdPrint(("Mailbox: PA=0x%llX cmd=0x%08X written via GPU proxy\n",
            devExt->FwPhysical.QuadPart, command));
    } else {
        WRITE_REGISTER_ULONG(
            (PULONG)((PUCHAR)mboxBase + PSP_C2PMSG_36_OFFSET),
            (ULONG)(devExt->FwPhysical.QuadPart & 0xFFFFFFFF)
        );
        WRITE_REGISTER_ULONG(
            (PULONG)((PUCHAR)mboxBase + PSP_C2PMSG_37_OFFSET),
            (ULONG)(devExt->FwPhysical.QuadPart >> 32)
        );

        WRITE_REGISTER_ULONG(
            (PULONG)((PUCHAR)mboxBase + PSP_C2PMSG_35_OFFSET),
            command
        );
        KdPrint(("Mailbox: PA=0x%llX cmd=0x%08X written to C2PMSG_36/37/35\n",
            devExt->FwPhysical.QuadPart, command));
    }

    KeReleaseSpinLock(&devExt->CommandLock, irql);

    for (timeout = 0; timeout < PSP_FW_WAIT_MS; timeout++) {
        KeStallExecutionProcessor(1000);
        if (useGpuProxy && g_GpuProxyAvailable) {
            cmdReg = PspGpuProxyReadRegister(PSP_C2PMSG_35_OFFSET);
        } else {
            cmdReg = READ_REGISTER_ULONG(
                (PULONG)((PUCHAR)mboxBase + PSP_C2PMSG_35_OFFSET)
            );
        }
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
    PUCHAR mboxBase;
    KIRQL irql;
    volatile PULONG cmdDwords;

    if (FwSize == 0 || FwData == NULL || FwSize > 0x100000) {
        return STATUS_INVALID_PARAMETER;
    }

    /* We need GPU BAR5 for mailbox access (proxy path handles it below) */
    if (g_Bar5Mapping == NULL) {
        NTSTATUS proxyStatus = PspGpuProxyInit(devExt);
        if (!NT_SUCCESS(proxyStatus)) {
            KdPrint(("LOAD_IP_FW_MAILBOX: GPU proxy init failed: 0x%08X\n", proxyStatus));
            return STATUS_DEVICE_NOT_READY;
        }
    }
    if (!g_Bar5Mapping && !devExt->GpuMmioBase && !g_GpuProxyAvailable) {
        return STATUS_DEVICE_NOT_READY;
    }
    mboxBase = g_Bar5Mapping ? g_Bar5Mapping : devExt->GpuMmioBase;

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

    /* Send via mailbox: C2PMSG_36/37 = command buffer PA, C2PMSG_35 = 0x06 */
    KeAcquireSpinLock(&devExt->CommandLock, &irql);

    if (g_GpuProxyAvailable) {
        if (!PspGpuProxyWriteRegister(PSP_C2PMSG_36_OFFSET, (ULONG)(cmdBufPa.QuadPart & 0xFFFFFFFF)) ||
            !PspGpuProxyWriteRegister(PSP_C2PMSG_37_OFFSET, (ULONG)(cmdBufPa.QuadPart >> 32)) ||
            !PspGpuProxyWriteRegister(PSP_C2PMSG_35_OFFSET, GFX_CMD_ID_LOAD_IP_FW)) {
            KeReleaseSpinLock(&devExt->CommandLock, irql);
            KdPrint(("LOAD_IP_FW_MAILBOX: GPU proxy write failed\n"));
            MmFreeContiguousMemory(cmdBufVa);
            MmFreeContiguousMemory(fwBufVa);
            return STATUS_DEVICE_NOT_READY;
        }
    } else {
        mboxBase = (g_Bar5Mapping != NULL) ? g_Bar5Mapping : mboxBase;
        WRITE_REGISTER_ULONG((PULONG)((PUCHAR)mboxBase + PSP_C2PMSG_36_OFFSET),
            (ULONG)(cmdBufPa.QuadPart & 0xFFFFFFFF));
        WRITE_REGISTER_ULONG((PULONG)((PUCHAR)mboxBase + PSP_C2PMSG_37_OFFSET),
            (ULONG)(cmdBufPa.QuadPart >> 32));
        WRITE_REGISTER_ULONG((PULONG)((PUCHAR)mboxBase + PSP_C2PMSG_35_OFFSET),
            GFX_CMD_ID_LOAD_IP_FW);
    }

    KeReleaseSpinLock(&devExt->CommandLock, irql);

    /* Wait for completion (C2PMSG_35 clears to 0) */
    for (timeout = 0; timeout < PSP_FW_WAIT_MS; timeout++) {
        KeStallExecutionProcessor(1000);
        if (g_GpuProxyAvailable) {
            cmdReg = PspGpuProxyReadRegister(PSP_C2PMSG_35_OFFSET);
        } else {
            cmdReg = READ_REGISTER_ULONG(
                (PULONG)((PUCHAR)mboxBase + PSP_C2PMSG_35_OFFSET));
        }
        if (cmdReg == 0) {
            KdPrint(("LOAD_IP_FW_MAILBOX: Completed after %u ms\n", timeout));
            break;
        }
    }

    /* Read C2PMSG_81 for status */
    {
        ULONG c2pmsg81 = 0;
        if (g_GpuProxyAvailable) {
            c2pmsg81 = PspGpuProxyReadRegister(PSP_C2PMSG_81_OFFSET);
        } else {
            c2pmsg81 = READ_REGISTER_ULONG(
                (PULONG)((PUCHAR)mboxBase + PSP_C2PMSG_81_OFFSET));
        }
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

NTSTATUS PspAutoInitialize(PDEVICE_EXTENSION devExt)
{
    if (devExt->Bar0Base == NULL) {
        PHYSICAL_ADDRESS physAddr;
        physAddr.QuadPart = PSP_BAR0_PHYSICAL;
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

    if (devExt->GpuMmioBase == NULL) {
        PHYSICAL_ADDRESS physAddr;
        physAddr.QuadPart = 0xFE800000ULL;
        devExt->GpuMmioBase = MmMapIoSpace(physAddr, 0x80000, MmNonCached);
        if (devExt->GpuMmioBase == NULL) {
            KdPrint(("PSP: BAR5 map failed at 0x%llX (Windows 11 26100 blocks this, will use GPU proxy)\n", physAddr.QuadPart));
            devExt->GpuMmioSize = 0x80000;
        } else {
            devExt->GpuMmioSize = 0x80000;
            KdPrint(("PSP: BAR5 mapped: PA=0x%llX VA=%p size=%u\n",
                physAddr.QuadPart, devExt->GpuMmioBase, devExt->GpuMmioSize));
        }
    }

    if (devExt->Bar0Base == NULL && devExt->GpuMmioBase == NULL) {
        KdPrint(("PSP: Auto-init FAILED — no MMIO base available\n"));
        return STATUS_DEVICE_NOT_READY;
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