// PspCore.c - Core PSP functionality (mailbox, TMR, firmware, auto-init)
// Extracted from PspDriver.c for modularity
#include <ntddk.h>
#include <wdm.h>
#include "PspIoctl.h"
#include "firmware_data.h"
#include "PspCore.h"

// Shared TMR state (non-static for extern in PspCore.h)
PVOID g_TmrBuffer = NULL;
PHYSICAL_ADDRESS g_TmrPhysical = {0};
ULONG g_TmrSize = 0;
BOOLEAN g_TmrInitialized = FALSE;

// Shared KIQ state (defined in PspKiq.c)
PVOID g_KiqRingVa = NULL;
PHYSICAL_ADDRESS g_KiqRingPa = {0};
ULONG g_KiqRingSize = 0;
ULONG g_KiqRingWptr = 0;
BOOLEAN g_KiqRingInitialized = FALSE;

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
    PVOID mboxBase = devExt->Bar0Base ? devExt->Bar0Base : devExt->MmioBase;

    if (devExt->FwBuffer == NULL) {
        KdPrint(("No firmware loaded\n"));
        return STATUS_NO_MEMORY;
    }

    KeAcquireSpinLock(&devExt->CommandLock, &irql);

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

    KeReleaseSpinLock(&devExt->CommandLock, irql);

    for (timeout = 0; timeout < PSP_FW_WAIT_MS; timeout++) {
        KeStallExecutionProcessor(1000);
        cmdReg = READ_REGISTER_ULONG(
            (PULONG)((PUCHAR)mboxBase + PSP_C2PMSG_35_OFFSET)
        );
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
            KdPrint(("PSP: BAR0 map failed at 0x%llX, using BAR5 for mailbox\n", physAddr.QuadPart));
        } else {
            devExt->Bar0Size = PSP_BAR0_SIZE;
            KdPrint(("PSP: BAR0 mapped: PA=0x%llX VA=%p size=%u\n",
                physAddr.QuadPart, devExt->Bar0Base, devExt->Bar0Size));
        }
    }

    if (devExt->MmioBase == NULL) {
        devExt->MmioBase = devExt->Bar0Base ? devExt->Bar0Base : devExt->MmioBase;
        devExt->MmioSize = devExt->Bar0Size;
    }

    return STATUS_SUCCESS;
}
