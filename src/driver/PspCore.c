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

#define IOCTL_AMDBC250_BAR5_READ_PROXY  CTL_CODE(FILE_DEVICE_UNKNOWN, 0x900, METHOD_BUFFERED, FILE_ANY_ACCESS)
#define IOCTL_AMDBC250_BAR5_WRITE_PROXY CTL_CODE(FILE_DEVICE_UNKNOWN, 0x901, METHOD_BUFFERED, FILE_ANY_ACCESS)

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
    
    if (g_GpuProxyAvailable && g_GpuDriverHandle != NULL) {
        return STATUS_SUCCESS;
    }
    
    if (g_GpuDriverHandle == NULL) {
        NTSTATUS status = PspOpenGpuDriver();
        if (!NT_SUCCESS(status)) {
            KdPrint(("PSP_GPU_PROXY: Failed to open GPU driver: 0x%08X\n", status));
            return status;
        }
    }
    
    testValue = PspGpuProxyReadRegister(0);
    KdPrint(("PSP_GPU_PROXY: Test read from offset 0: 0x%08X\n", testValue));
    
    if (testValue == 0xFFFFFFFF) {
        KdPrint(("PSP_GPU_PROXY: GPU driver proxy not responding correctly\n"));
        return STATUS_DEVICE_NOT_READY;
    }
    
    g_GpuProxyAvailable = TRUE;
    g_GpuProxyInitialized = TRUE;
    return STATUS_SUCCESS;
}

ULONG PspGpuProxyReadRegister(ULONG offset)
{
    if (g_Bar5Mapping != NULL) {
        return READ_REGISTER_ULONG((PULONG)((PUCHAR)g_Bar5Mapping + offset));
    }
    
    if (g_GpuDriverHandle != NULL) {
        ULONG inputOffset = offset;
        ULONG outputValue = 0;
        IO_STATUS_BLOCK ioStatus;
        NTSTATUS status;
        
        status = ZwDeviceIoControlFile(g_GpuDriverHandle, NULL, NULL, NULL, &ioStatus,
                                        IOCTL_AMDBC250_BAR5_READ_PROXY,
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
    if (g_Bar5Mapping != NULL) {
        WRITE_REGISTER_ULONG((PULONG)((PUCHAR)g_Bar5Mapping + offset), value);
        return TRUE;
    }
    
    if (g_GpuDriverHandle != NULL) {
        ULONG params[2] = {offset, value};
        IO_STATUS_BLOCK ioStatus;
        NTSTATUS status;
        
        status = ZwDeviceIoControlFile(g_GpuDriverHandle, NULL, NULL, NULL, &ioStatus,
                                        IOCTL_AMDBC250_BAR5_WRITE_PROXY,
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
        PspGpuProxyWriteRegister(PSP_C2PMSG_36_OFFSET, (ULONG)(devExt->FwPhysical.QuadPart & 0xFFFFFFFF));
        PspGpuProxyWriteRegister(PSP_C2PMSG_37_OFFSET, (ULONG)(devExt->FwPhysical.QuadPart >> 32));
        PspGpuProxyWriteRegister(PSP_C2PMSG_35_OFFSET, command);
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

    return STATUS_SUCCESS;
}