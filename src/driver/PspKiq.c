// PspKiq.c - KIQ (Queue) Ring functionality for AMD BC-250 PSP
#include <ntddk.h>
#include <wdm.h>
#include "PspIoctl.h"
#include "PspCore.h"
#include "PspKiq.h"

NTSTATUS PspKiqInit(PDEVICE_EXTENSION devExt, ULONG64 ringPA, ULONG ringSize, ULONG cmdBufSize)
{
    UNREFERENCED_PARAMETER(devExt);
    UNREFERENCED_PARAMETER(ringPA);
    UNREFERENCED_PARAMETER(ringSize);
    UNREFERENCED_PARAMETER(cmdBufSize);
    KdPrint(("PspKiqInit: SOS on this firmware does not support KIQ ring\n"));
    return STATUS_NOT_SUPPORTED;
}

NTSTATUS PspKiqSubmit(PDEVICE_EXTENSION devExt, PPSP_KIQ_SUBMIT_REQUEST req)
{
    UNREFERENCED_PARAMETER(devExt);
    UNREFERENCED_PARAMETER(req);
    KdPrint(("PspKiqSubmit: KIQ ring not initialized (unsupported by this SOS)\n"));
    return STATUS_DEVICE_NOT_READY;
}

VOID PspKiqCleanup(VOID)
{
    if (g_KiqRingVa) {
        MmFreeContiguousMemory(g_KiqRingVa);
        g_KiqRingVa = NULL;
        g_KiqRingPa.QuadPart = 0;
        g_KiqRingSize = 0;
        g_KiqRingInitialized = FALSE;
        KdPrint(("PspKiqCleanup: ring buffer freed\n"));
    }
}
