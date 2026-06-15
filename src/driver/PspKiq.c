// PspKiq.c - KIQ (Queue) Ring functionality for AMD BC-250 PSP
#include <ntddk.h>
#include <wdm.h>
#include "PspIoctl.h"
#include "PspCore.h"
#include "PspKiq.h"

NTSTATUS PspKiqLoadFirmware(PDEVICE_EXTENSION devExt, ULONG FwType, ULONG FwSize, PUCHAR FwData)
{
    NTSTATUS status;
    ULONG cmd[64];
    ULONG i;
    PHYSICAL_ADDRESS fwPa;

    UNREFERENCED_PARAMETER(devExt);

    if (!g_KiqRingInitialized) {
        status = PspKiqInit(devExt, 0, 0, 0);
        if (!NT_SUCCESS(status)) {
            return status;
        }
    }

    if (FwSize == 0 || FwData == NULL) {
        return STATUS_INVALID_PARAMETER;
    }

    if (FwSize > 0x100000) {
        return STATUS_INVALID_BUFFER_SIZE;
    }

    RtlZeroMemory(cmd, sizeof(cmd));

    cmd[0] = 1024;
    cmd[1] = 1;
    cmd[2] = 0x06; // LOAD_IP_FW

    fwPa = MmGetPhysicalAddress(FwData);
    cmd[7] = (ULONG)(fwPa.QuadPart & 0xFFFFFFFF);
    cmd[8] = (ULONG)(fwPa.QuadPart >> 32);
    cmd[9] = FwSize;
    cmd[10] = FwType;

    KdPrint(("PspKiqLoadFirmware: type=%u size=%u PA=0x%llX\n",
             FwType, FwSize, fwPa.QuadPart));

    PSP_KIQ_SUBMIT_REQUEST req;
    RtlZeroMemory(&req, sizeof(req));
    req.CommandCount = 64;
    for (i = 0; i < 64; i++) {
        req.Commands[i] = cmd[i];
    }

    status = PspKiqSubmit(devExt, &req);
    return status;
}

NTSTATUS PspKiqInit(PDEVICE_EXTENSION devExt, ULONG64 ringPA, ULONG ringSize, ULONG cmdBufSize)
{
    PVOID mboxBase;
    PHYSICAL_ADDRESS highAddr;

    UNREFERENCED_PARAMETER(cmdBufSize);

    if (g_KiqRingInitialized) {
        return STATUS_SUCCESS;
    }

    if (!devExt->MmioBase) {
        return STATUS_DEVICE_NOT_READY;
    }

    mboxBase = devExt->Bar0Base ? devExt->Bar0Base : devExt->MmioBase;

    if (!g_KiqRingVa) {
        highAddr.QuadPart = 0x100000000ULL;
        g_KiqRingVa = MmAllocateContiguousMemory(ringSize ? ringSize : 0x2000, highAddr);
        if (!g_KiqRingVa) {
            highAddr.QuadPart = 0xFFFFFFFF;
            g_KiqRingVa = MmAllocateContiguousMemory(ringSize ? ringSize : 0x2000, highAddr);
        }
        if (!g_KiqRingVa) {
            return STATUS_INSUFFICIENT_RESOURCES;
        }
        RtlZeroMemory(g_KiqRingVa, ringSize ? ringSize : 0x2000);
        g_KiqRingPa = MmGetPhysicalAddress(g_KiqRingVa);
        g_KiqRingSize = ringSize ? ringSize : 0x2000;
    }

    if (ringPA) {
        g_KiqRingPa.QuadPart = ringPA;
        g_KiqRingSize = ringSize ? ringSize : g_KiqRingSize;
    }

    KeInitializeSpinLock(&g_KiqRingLock);
    g_KiqRingWptr = 0;
    g_KiqRingInitialized = TRUE;

    return STATUS_SUCCESS;
}

NTSTATUS PspKiqSubmit(PDEVICE_EXTENSION devExt, PPSP_KIQ_SUBMIT_REQUEST req)
{
    KIRQL irql;
    ULONG i;
    ULONG wptr;

    UNREFERENCED_PARAMETER(devExt);

    if (!g_KiqRingInitialized || !g_KiqRingVa) {
        return STATUS_DEVICE_NOT_READY;
    }

    if (!req || req->CommandCount == 0 || req->CommandCount > 64) {
        return STATUS_INVALID_PARAMETER;
    }

    KeAcquireSpinLock(&g_KiqRingLock, &irql);

    wptr = g_KiqRingWptr;
    for (i = 0; i < req->CommandCount; i++) {
        ((volatile PULONG)(g_KiqRingVa))[wptr] = req->Commands[i];
        wptr++;
        if (wptr * sizeof(ULONG) >= g_KiqRingSize) {
            wptr = 0;
        }
    }
    g_KiqRingWptr = wptr;

    KeReleaseSpinLock(&g_KiqRingLock, irql);

    return STATUS_SUCCESS;
}

VOID PspKiqCleanup(VOID)
{
    if (g_KiqRingVa) {
        MmFreeContiguousMemory(g_KiqRingVa);
        g_KiqRingVa = NULL;
        g_KiqRingPa.QuadPart = 0;
        g_KiqRingSize = 0;
        g_KiqRingWptr = 0;
        g_KiqRingInitialized = FALSE;
        KdPrint(("PspKiqCleanup: ring buffer freed\n"));
    }
}
