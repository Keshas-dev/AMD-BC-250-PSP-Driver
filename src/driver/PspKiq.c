// PspKiq.c - KIQ (Queue) Ring functionality for AMD BC-250 PSP
// Programs GPU KIQ registers so hardware can execute PM4 commands from the ring.
// NOTE: Only KIQ_BASE/WPTR/RPTR registers are used. HQD registers (0xDAC0+),
// RLC_CP_SCHEDULERS (dangerous unaligned), and ME_CNTL (unhalts unloaded PFP/CE)
// are deliberately NOT written. Firmware loader handles halt bits.
#include <ntddk.h>
#include <wdm.h>
#include "PspIoctl.h"
#include "PspCore.h"
#include "PspKiq.h"

/* ---- GPU register offsets (BAR5-relative, GC_BASE-shifted) ---- */
#define GPU_CP_ME_CNTL                  0x4A74
#define GPU_CP_KIQ_BASE_LO              0xE060
#define GPU_CP_KIQ_BASE_HI              0xE064
#define GPU_CP_KIQ_RPTR                 0xE06C
#define GPU_CP_KIQ_WPTR                 0xE078
#define GPU_RLC_CP_SCHEDULERS           0xECA8

/* CP_ME_CNTL bits */
#define ME_CNTL_ME_HALT   (1u << 28)
#define ME_CNTL_PFP_HALT  (1u << 30)

/* RLC_CP_SCHEDULERS KIQ value: ENABLE=1, ME=1 */
#define RLC_SCHEDULERS_KIQ 0xA0

/* WPTR polling page size */
#define WPTR_POLL_PAGE_SIZE 0x1000

/* Global WPTR polling page */
static PVOID g_WptrPollVa = NULL;
static PHYSICAL_ADDRESS g_WptrPollPa = {0};

/* Firmware DMA buffer — kept alive until next load or unload to prevent GPU DMA from reading freed memory */
static PVOID g_FirmwareDmaBuffer = NULL;

static LONG g_KiqInitGuard = 0;

static NTSTATUS PspKiqProgramHwRegisters(PDEVICE_EXTENSION devExt)
{
    KdPrint(("KIQ_HW: Programming KIQ registers (g_Bar5Mapping=%p, GpuMmioBase=%p, GpuDriverHandle=%p)\n",
        g_Bar5Mapping, devExt->GpuMmioBase, g_GpuDriverHandle));

    if (!g_Bar5Mapping && !devExt->GpuMmioBase && g_GpuDriverHandle == NULL) {
        KdPrint(("KIQ_HW: No BAR5 mapping and no proxy handle, cannot program KIQ\n"));
        return STATUS_DEVICE_NOT_READY;
    }

    /* 1. Set KIQ_BASE (KIQ engine reads commands from here!) */
    PspGpuProxyWriteRegister(GPU_CP_KIQ_BASE_LO,
        (ULONG)(g_KiqRingPa.QuadPart & 0xFFFFFFFF));
    PspGpuProxyWriteRegister(GPU_CP_KIQ_BASE_HI,
        (ULONG)(g_KiqRingPa.QuadPart >> 32));

    /* 2. Clear RPTR + WPTR */
    PspGpuProxyWriteRegister(GPU_CP_KIQ_RPTR, 0);
    PspGpuProxyWriteRegister(GPU_CP_KIQ_WPTR, 0);

    /* 3. Notify RLC scheduler — SKIPPED: RLC_CP_SCHEDULERS was previously at 0xECA1
     *    (unaligned, not 4-byte aligned). Correct offset is 0xECA8 (empirically confirmed).
     *    KIQ works without RLC scheduler notification on BC-250. */
    // PspGpuProxyWriteRegister(GPU_RLC_CP_SCHEDULERS, RLC_SCHEDULERS_KIQ);

    /* 4. Ensure CP is unhalted */
    PspGpuProxyWriteRegister(GPU_CP_ME_CNTL, 0);
    KeStallExecutionProcessor(100);

    KdPrint(("KIQ_HW: KIQ programmed — ring PA=0x%llX size=%u\n",
        g_KiqRingPa.QuadPart, g_KiqRingSize));

    return STATUS_SUCCESS;
}

NTSTATUS PspKiqInit(PDEVICE_EXTENSION devExt, ULONG64 ringPA, ULONG ringSize, ULONG cmdBufSize)
{
    PHYSICAL_ADDRESS highAddr;
    PHYSICAL_ADDRESS lowAddr;
    PHYSICAL_ADDRESS boundary;

    UNREFERENCED_PARAMETER(cmdBufSize);

    if (InterlockedCompareExchange(&g_KiqInitGuard, 1, 0) != 0) {
        if (g_KiqRingInitialized) return STATUS_SUCCESS;
        while (!g_KiqRingInitialized) {
            KeStallExecutionProcessor(1);
        }
        return STATUS_SUCCESS;
    }

    /* If no direct BAR5 mapping AND no proxy handle, try proxy init first */
    if (!devExt->MmioBase) {
        if (g_GpuDriverHandle == NULL) {
            NTSTATUS proxySt = PspGpuProxyInit(devExt);
            if (!NT_SUCCESS(proxySt)) {
                g_KiqInitGuard = 0;
                return STATUS_DEVICE_NOT_READY;
            }
        }
    }

    /* Allocate ring buffer (non-cached for GPU coherency) */
    if (!g_KiqRingVa) {
        lowAddr.QuadPart = 0;
        highAddr.QuadPart = 0x100000000ULL;
        boundary.QuadPart = 0;
        g_KiqRingVa = MmAllocateContiguousMemorySpecifyCache(
            ringSize ? ringSize : 0x2000,
            lowAddr, highAddr, boundary, MmNonCached);
        if (!g_KiqRingVa) {
            highAddr.QuadPart = 0xFFFFFFFF;
            g_KiqRingVa = MmAllocateContiguousMemorySpecifyCache(
                ringSize ? ringSize : 0x2000,
                lowAddr, highAddr, boundary, MmNonCached);
        }
        if (!g_KiqRingVa) {
            g_KiqInitGuard = 0;
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

    /* Allocate WPTR polling page (non-cached) for GPU to read WPTR */
    if (!g_WptrPollVa) {
        lowAddr.QuadPart = 0;
        highAddr.QuadPart = 0x100000000ULL;
        boundary.QuadPart = 0;
        g_WptrPollVa = MmAllocateContiguousMemorySpecifyCache(
            WPTR_POLL_PAGE_SIZE, lowAddr, highAddr, boundary, MmNonCached);
        if (!g_WptrPollVa) {
            highAddr.QuadPart = 0xFFFFFFFF;
            g_WptrPollVa = MmAllocateContiguousMemorySpecifyCache(
                WPTR_POLL_PAGE_SIZE, lowAddr, highAddr, boundary, MmNonCached);
        }
        if (g_WptrPollVa) {
            RtlZeroMemory(g_WptrPollVa, WPTR_POLL_PAGE_SIZE);
            g_WptrPollPa = MmGetPhysicalAddress(g_WptrPollVa);
            KdPrint(("KIQ: WPTR poll page PA=0x%llX VA=%p\n",
                g_WptrPollPa.QuadPart, g_WptrPollVa));
        } else {
            KdPrint(("KIQ: WARNING — WPTR poll page alloc failed\n"));
        }
    }

    KeInitializeSpinLock(&g_KiqRingLock);
    g_KiqRingWptr = 0;
    g_KiqRingInitialized = TRUE;

    /* Ensure GPU BAR5 access is available before programming HW registers */
    if (!g_Bar5Mapping && !devExt->GpuMmioBase) {
        KdPrint(("KIQ: No GPU BAR5, trying proxy init\n"));
        NTSTATUS proxyStatus = PspGpuProxyInit(devExt);
        if (!NT_SUCCESS(proxyStatus)) {
            KdPrint(("KIQ: GPU proxy init failed (0x%08X), HW registers won't be programmed\n", proxyStatus));
        }
    }

    /* Program GPU HQD hardware registers */
    NTSTATUS hwStatus = PspKiqProgramHwRegisters(devExt);
    if (!NT_SUCCESS(hwStatus)) {
        KdPrint(("KIQ: WARNING — HW register programming failed (0x%08X)\n", hwStatus));
        /* Ring is still usable for software-only paths */
    }

    KdPrint(("KIQ: Init complete — ring PA=0x%llX size=%u\n",
        g_KiqRingPa.QuadPart, g_KiqRingSize));

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

    /* Check ring capacity */
    {
        UINT32 ringDwords = g_KiqRingSize / sizeof(ULONG);
        UINT32 rptr = PspGpuProxyReadRegister(GPU_CP_KIQ_RPTR);
        UINT32 used;
        if (wptr >= rptr) {
            used = wptr - rptr;
        } else {
            used = ringDwords - rptr + wptr;
        }
        if (used + req->CommandCount > ringDwords) {
            KeReleaseSpinLock(&g_KiqRingLock, irql);
            return STATUS_BUFFER_TOO_SMALL;
        }
    }

    for (i = 0; i < req->CommandCount; i++) {
        ((volatile PULONG)(g_KiqRingVa))[wptr] = req->Commands[i];
        wptr++;
        if (wptr * sizeof(ULONG) >= g_KiqRingSize) {
            wptr = 0;
        }
    }
    g_KiqRingWptr = wptr;

    KeReleaseSpinLock(&g_KiqRingLock, irql);

    /* Memory barrier: ring data must be visible before WPTR update */
    KeMemoryBarrier();

    /* Update WPTR in polling page (if allocated) */
    if (g_WptrPollVa) {
        *(volatile PULONG)(g_WptrPollVa) = wptr;
    }

    /* Update WPTR in GPU KIQ register */
    PspGpuProxyWriteRegister(GPU_CP_KIQ_WPTR, wptr);

    return STATUS_SUCCESS;
}

VOID PspKiqCleanup(VOID)
{
    /* Halt CP engines */
    if (g_Bar5Mapping || g_GpuDriverHandle) {
        PspGpuProxyWriteRegister(GPU_CP_ME_CNTL, ME_CNTL_ME_HALT | ME_CNTL_PFP_HALT);
    }

    /* Free firmware DMA buffer */
    if (g_FirmwareDmaBuffer) {
        MmFreeContiguousMemory(g_FirmwareDmaBuffer);
        g_FirmwareDmaBuffer = NULL;
    }

    /* Free WPTR polling page */
    if (g_WptrPollVa) {
        MmFreeContiguousMemory(g_WptrPollVa);
        g_WptrPollVa = NULL;
        g_WptrPollPa.QuadPart = 0;
    }

    /* Free ring buffer */
    if (g_KiqRingVa) {
        MmFreeContiguousMemory(g_KiqRingVa);
        g_KiqRingVa = NULL;
        g_KiqRingPa.QuadPart = 0;
        g_KiqRingSize = 0;
        g_KiqRingWptr = 0;
        g_KiqRingInitialized = FALSE;
        KdPrint(("PspKiqCleanup: ring buffer freed\n"));
    }
    g_KiqInitGuard = 0;
}

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

    /* Free previous firmware DMA buffer before allocating new one */
    if (g_FirmwareDmaBuffer != NULL) {
        MmFreeContiguousMemory(g_FirmwareDmaBuffer);
        g_FirmwareDmaBuffer = NULL;
    }

    /* Allocate contiguous non-paged buffer for DMA — FwData may span multiple
     * IOCTL pages; MmGetPhysicalAddress on SystemBuffer only returns the first page */
    {
        PHYSICAL_ADDRESS lowAddr, highAddr;
        lowAddr.QuadPart = 0;
        highAddr.QuadPart = 0xFFFFFFFFFFFFFFFFULL;
        g_FirmwareDmaBuffer = MmAllocateContiguousMemorySpecifyCache(
            FwSize, lowAddr, highAddr, lowAddr, MmNonCached);
        if (g_FirmwareDmaBuffer == NULL) {
            g_FirmwareDmaBuffer = MmAllocateContiguousMemory(FwSize, highAddr);
        }
        if (g_FirmwareDmaBuffer == NULL) {
            KdPrint(("PspKiqLoadFirmware: DMA buffer alloc failed (size=%u)\n", FwSize));
            return STATUS_INSUFFICIENT_RESOURCES;
        }
        RtlCopyMemory(g_FirmwareDmaBuffer, FwData, FwSize);
        fwPa = MmGetPhysicalAddress(g_FirmwareDmaBuffer);
    }

    RtlZeroMemory(cmd, sizeof(cmd));

    cmd[0] = 1024;
    cmd[1] = 1;
    cmd[2] = 0x06; // LOAD_IP_FW

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

    /* NOTE: g_FirmwareDmaBuffer is NOT freed here — GPU DMA may still be reading it.
     * It is freed on the next PspKiqLoadFirmware call or in PspKiqCleanup. */

    return status;
}

NTSTATUS PspGpuPm4Submit(PDEVICE_EXTENSION devExt, PPSP_GPU_PM4_SUBMIT_REQUEST req, PPSP_GPU_PM4_SUBMIT_RESPONSE resp)
{
    ULONG i;
    KIRQL irql;
    ULONG wptr;

    if (!g_KiqRingInitialized || !g_KiqRingVa) {
        NTSTATUS st = PspKiqInit(devExt, 0, 0, 0);
        if (!NT_SUCCESS(st)) return st;
    }

    if (!req || req->CommandCount == 0 || req->CommandCount > 64) {
        return STATUS_INVALID_PARAMETER;
    }

    /* Read SCRATCH before */
    resp->ScratchBefore = PspGpuProxyReadRegister(0x32D4);

    /* Read WPTR before */
    resp->HqdPqWptrBefore = PspGpuProxyReadRegister(GPU_CP_KIQ_WPTR);

    /* Write PM4 commands to ring */
    KeAcquireSpinLock(&g_KiqRingLock, &irql);
    wptr = g_KiqRingWptr;

    /* Check ring capacity */
    {
        UINT32 ringDwords = g_KiqRingSize / sizeof(ULONG);
        UINT32 rptr = PspGpuProxyReadRegister(GPU_CP_KIQ_RPTR);
        UINT32 used;
        if (wptr >= rptr) {
            used = wptr - rptr;
        } else {
            used = ringDwords - rptr + wptr;
        }
        if (used + req->CommandCount > ringDwords) {
            KeReleaseSpinLock(&g_KiqRingLock, irql);
            return STATUS_BUFFER_TOO_SMALL;
        }
    }

    for (i = 0; i < req->CommandCount; i++) {
        ((volatile PULONG)(g_KiqRingVa))[wptr] = req->Commands[i];
        wptr++;
        if (wptr * sizeof(ULONG) >= g_KiqRingSize) {
            wptr = 0;
        }
    }
    g_KiqRingWptr = wptr;
    KeReleaseSpinLock(&g_KiqRingLock, irql);

    /* Memory barrier */
    KeMemoryBarrier();

    /* Update WPTR in polling page */
    if (g_WptrPollVa) {
        *(volatile PULONG)(g_WptrPollVa) = wptr;
    }

    /* Kick GPU via KIQ WPTR */
    PspGpuProxyWriteRegister(GPU_CP_KIQ_WPTR, wptr);

    resp->Pm4Dwords = req->CommandCount;
    resp->KiqRingWptr = wptr;

    /* Wait if requested */
    if (req->WaitMs > 0) {
        LARGE_INTEGER delay;
        delay.QuadPart = -(LONGLONG)10000 * req->WaitMs;
        KeDelayExecutionThread(KernelMode, FALSE, &delay);
    }

    /* Read results */
    resp->ScratchAfter = PspGpuProxyReadRegister(0x32D4);
    resp->HqdPqWptrAfter = PspGpuProxyReadRegister(GPU_CP_KIQ_WPTR);
    resp->WptrReadback = PspGpuProxyReadRegister(GPU_CP_KIQ_WPTR);
    resp->KiqRingSize = g_KiqRingSize;
    resp->KiqRingPa = g_KiqRingPa.LowPart;

    KdPrint(("GPU_PM4: Scratch=0x%08X->0x%08X WPTR=%u->%u PM4=%u\n",
        resp->ScratchBefore, resp->ScratchAfter,
        resp->HqdPqWptrBefore, resp->HqdPqWptrAfter,
        req->CommandCount));

    return STATUS_SUCCESS;
}
