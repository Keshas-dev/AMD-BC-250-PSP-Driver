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
#define GPU_CP_KIQ_ACTIVE               0xE080
#define GPU_RLC_CP_SCHEDULERS           0xECA8
#define GPU_GRBM_GFX_INDEX              0x34D0
#define GPU_CP_HQD_PQ_CONTROL           0x90F0
#define GPU_CP_HQD_ACTIVE               0x910C

/* CP_ME_CNTL bits */
#define ME_CNTL_ME_HALT   (1u << 28)
#define ME_CNTL_PFP_HALT  (1u << 30)

/* RLC_CP_SCHEDULERS KIQ value: ENABLE=1, ME=1 */
#define RLC_SCHEDULERS_KIQ 0xA0

/* GRBM_GFX_INDEX: ME=1, PIPE=0, QUEUE=0 (selects KIQ engine) */
#define GRBM_GFX_INDEX_KIQ         (1u << 16)
/* GRBM_GFX_INDEX: SE_BROADCAST | QUEUE_BROADCAST | PIPE_BROADCAST | INSTANCE_BROADCAST */
#define GRBM_GFX_INDEX_BROADCAST   (0xE0000000UL)

/* CP_HQD_PQ_CONTROL: ring size = 4 DWORDs (only bit 0 stickable on BC-250) */
#define CP_HQD_PQ_CONTROL_MIN_SIZE 0x00000001

/* WPTR polling page size */
#define WPTR_POLL_PAGE_SIZE 0x1000

/* KIQ_WPTR is 9-bit only (mask 0x1FF) — max ring = 512 DWORDs = 2048 bytes */
#define KIQ_MAX_RING_SIZE   2048

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

    /* 1. Select KIQ engine via GRBM_GFX_INDEX (ME=1, PIPE=0, QUEUE=0) */
    PspGpuProxyWriteRegister(GPU_GRBM_GFX_INDEX, GRBM_GFX_INDEX_KIQ);
    KeStallExecutionProcessor(1);

    /* 2. Deactivate queue before programming */
    PspGpuProxyWriteRegister(GPU_CP_HQD_ACTIVE, 0);
    PspGpuProxyWriteRegister(GPU_CP_KIQ_ACTIVE, 0);
    KeStallExecutionProcessor(1);

    /* 3. Set ring buffer address via KIQ_BASE */
    {
        ULONG baseLo = (ULONG)(g_KiqRingPa.QuadPart & 0xFFFFFFFF);
        ULONG baseHi = (ULONG)(g_KiqRingPa.QuadPart >> 32);
        KdPrint(("KIQ_HW: Writing KIQ_BASE = 0x%08X_%08X (PA=0x%llX)\n",
            baseHi, baseLo, g_KiqRingPa.QuadPart));
        if (!PspGpuProxyWriteRegister(GPU_CP_KIQ_BASE_LO, baseLo)) {
            KdPrint(("KIQ_HW: Failed to write KIQ_BASE_LO\n"));
            PspGpuProxyWriteRegister(GPU_GRBM_GFX_INDEX, GRBM_GFX_INDEX_BROADCAST);
            return STATUS_DEVICE_NOT_READY;
        }
        KeStallExecutionProcessor(1);
        ULONG readback = PspGpuProxyReadRegister(GPU_CP_KIQ_BASE_LO);
        KdPrint(("KIQ_HW: KIQ_BASE_LO wrote=0x%08X readback=0x%08X\n", baseLo, readback));
        if (readback != baseLo) {
            KdPrint(("KIQ_HW: WARNING - KIQ_BASE_LO readback mismatch!\n"));
        }
        if (!PspGpuProxyWriteRegister(GPU_CP_KIQ_BASE_HI, baseHi)) {
            KdPrint(("KIQ_HW: Failed to write KIQ_BASE_HI\n"));
            PspGpuProxyWriteRegister(GPU_GRBM_GFX_INDEX, GRBM_GFX_INDEX_BROADCAST);
            return STATUS_DEVICE_NOT_READY;
        }
    }

    /* 4. Set ring size via CP_HQD_PQ_CONTROL (only bit 0 sticks on BC-250 = 4 DWORDs) */
    PspGpuProxyWriteRegister(GPU_CP_HQD_PQ_CONTROL, CP_HQD_PQ_CONTROL_MIN_SIZE);

    /* 5. Clear RPTR + WPTR */
    PspGpuProxyWriteRegister(GPU_CP_KIQ_RPTR, 0);
    PspGpuProxyWriteRegister(GPU_CP_KIQ_WPTR, 0);
    KeStallExecutionProcessor(1);

    /* 6. Ensure CP is unhalted */
    PspGpuProxyWriteRegister(GPU_CP_ME_CNTL, 0);
    KeStallExecutionProcessor(100);

    /* 7. Reactivate queue */
    PspGpuProxyWriteRegister(GPU_CP_HQD_ACTIVE, 1);
    PspGpuProxyWriteRegister(GPU_CP_KIQ_ACTIVE, 1);
    KeStallExecutionProcessor(1);

    /* 8. Notify RLC scheduler to enable KIQ scheduling */
    {
        ULONG sched = PspGpuProxyReadRegister(GPU_RLC_CP_SCHEDULERS);
        PspGpuProxyWriteRegister(GPU_RLC_CP_SCHEDULERS, sched | RLC_SCHEDULERS_KIQ);
    }

    /* 9. Restore GRBM_GFX_INDEX to broadcast */
    PspGpuProxyWriteRegister(GPU_GRBM_GFX_INDEX, GRBM_GFX_INDEX_BROADCAST);

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

    /* Cap ring size to KIQ_WPTR 9-bit limit (max 512 DWORDs = 2048 bytes) */
    if (ringSize == 0) {
        ringSize = KIQ_MAX_RING_SIZE;
    } else if (ringSize > KIQ_MAX_RING_SIZE) {
        KdPrint(("KIQ: ringSize %u capped to %u (9-bit WPTR limit)\n", ringSize, KIQ_MAX_RING_SIZE));
        ringSize = KIQ_MAX_RING_SIZE;
    }

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
        /* Try below 2GB first for guaranteed non-zero low 32 bits */
        lowAddr.QuadPart = 0x10000;  /* skip zero page */
        highAddr.QuadPart = 0x80000000ULL;  /* below 2GB */
        boundary.QuadPart = 0;
        g_KiqRingVa = MmAllocateContiguousMemorySpecifyCache(
            ringSize, lowAddr, highAddr, boundary, MmNonCached);
        /* Try below 4GB (above 2GB) */
        if (!g_KiqRingVa) {
            lowAddr.QuadPart = 0x80000000ULL;
            highAddr.QuadPart = 0x100000000ULL;
            g_KiqRingVa = MmAllocateContiguousMemorySpecifyCache(
                ringSize, lowAddr, highAddr, boundary, MmNonCached);
        }
        /* Last resort: try any address */
        if (!g_KiqRingVa) {
            lowAddr.QuadPart = 0;
            highAddr.QuadPart = 0xFFFFFFFFFFFFFFFFULL;
            g_KiqRingVa = MmAllocateContiguousMemorySpecifyCache(
                ringSize, lowAddr, highAddr, boundary, MmNonCached);
        }
        if (!g_KiqRingVa) {
            g_KiqInitGuard = 0;
            return STATUS_INSUFFICIENT_RESOURCES;
        }
        RtlZeroMemory(g_KiqRingVa, ringSize);
        g_KiqRingPa = MmGetPhysicalAddress(g_KiqRingVa);
        g_KiqRingSize = ringSize;
        KdPrint(("KIQ: ring allocated VA=%p PA=0x%llX size=%u\n",
            g_KiqRingVa, g_KiqRingPa.QuadPart, g_KiqRingSize));
        /* If PA low 32 bits is 0 (e.g. at 4GB boundary), try re-allocating */
        if (g_KiqRingPa.LowPart == 0 && g_KiqRingPa.QuadPart != 0) {
            KdPrint(("KIQ: PA LowPart=0 (0x%llX), re-trying allocation\n",
                g_KiqRingPa.QuadPart));
            MmFreeContiguousMemory(g_KiqRingVa);
            g_KiqRingVa = NULL;
            /* Force lower 2GB by trying a different approach */
            lowAddr.QuadPart = 0x10000;
            highAddr.QuadPart = 0x80000000ULL;
            g_KiqRingVa = MmAllocateContiguousMemorySpecifyCache(
                ringSize, lowAddr, highAddr, boundary, MmNonCached);
            if (g_KiqRingVa) {
                g_KiqRingPa = MmGetPhysicalAddress(g_KiqRingVa);
                g_KiqRingSize = ringSize;
            } else {
                /* If still failing, try with lower priority */
                g_KiqRingVa = MmAllocateContiguousMemory(ringSize, highAddr);
                if (g_KiqRingVa) {
                    g_KiqRingPa = MmGetPhysicalAddress(g_KiqRingVa);
                    g_KiqRingSize = ringSize;
                } else {
                    g_KiqInitGuard = 0;
                    return STATUS_INSUFFICIENT_RESOURCES;
                }
            }
        }
        KdPrint(("KIQ: ring allocated (attempt 2) VA=%p PA=0x%llX LowPart=0x%08X size=%u\n",
            g_KiqRingVa, g_KiqRingPa.QuadPart, g_KiqRingPa.LowPart, g_KiqRingSize));
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

    /* Select KIQ engine for all KIQ register accesses (RPTR read, WPTR write) */
    PspGpuProxyWriteRegister(GPU_GRBM_GFX_INDEX, GRBM_GFX_INDEX_KIQ);
    KeStallExecutionProcessor(1);

    KeAcquireSpinLock(&g_KiqRingLock, &irql);

    wptr = g_KiqRingWptr;

    /* Check ring capacity — mask RPTR to 9 bits (KIQ_WPTR is 9-bit, max 512 DWORDs) */
    {
        UINT32 ringDwords = g_KiqRingSize / sizeof(ULONG);
        UINT32 rptr = PspGpuProxyReadRegister(GPU_CP_KIQ_RPTR) & 0x000001FF;
        UINT32 used;
        if (wptr >= rptr) {
            used = wptr - rptr;
        } else {
            used = ringDwords - rptr + wptr;
        }
        if (used + req->CommandCount > ringDwords) {
            KeReleaseSpinLock(&g_KiqRingLock, irql);
            PspGpuProxyWriteRegister(GPU_GRBM_GFX_INDEX, GRBM_GFX_INDEX_BROADCAST);
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
    if (!PspGpuProxyWriteRegister(GPU_CP_KIQ_WPTR, wptr)) {
        KdPrint(("KIQ: WARNING — Write WPTR=%u via proxy failed\n", wptr));
    }

    /* Restore GRBM_GFX_INDEX to broadcast */
    PspGpuProxyWriteRegister(GPU_GRBM_GFX_INDEX, GRBM_GFX_INDEX_BROADCAST);

    return STATUS_SUCCESS;
}

VOID PspKiqCleanup(VOID)
{
    KIRQL irql;

    /* Acquire ring lock to prevent race with PspKiqSubmit/PspGpuPm4Submit */
    KeAcquireSpinLock(&g_KiqRingLock, &irql);
    g_KiqRingInitialized = FALSE;

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
        KdPrint(("PspKiqCleanup: ring buffer freed\n"));
    }
    g_KiqInitGuard = 0;

    KeReleaseSpinLock(&g_KiqRingLock, irql);
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
    cmd[1] = 4;  /* body_size = 4 DWORDs (fw_pa_lo/hi, fw_size, fw_type) */
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

    /* METHOD_BUFFERED: save fields BEFORE writing to response (same buffer!) */
    ULONG savedCmdCount = req ? req->CommandCount : 0;
    ULONG savedWaitMs = req ? req->WaitMs : 0;

    if (!g_KiqRingInitialized || !g_KiqRingVa) {
        NTSTATUS st = PspKiqInit(devExt, 0, 0, 0);
        if (!NT_SUCCESS(st)) {
            resp->Status = st;
            return st;
        }
    }

    if (!req || savedCmdCount == 0 || savedCmdCount > 64) {
        resp->Status = STATUS_INVALID_PARAMETER;
        return STATUS_INVALID_PARAMETER;
    }

    resp->Status = STATUS_SUCCESS;

    /* Select KIQ engine for all subsequent KIQ register accesses */
    PspGpuProxyWriteRegister(GPU_GRBM_GFX_INDEX, GRBM_GFX_INDEX_KIQ);
    KeStallExecutionProcessor(1);

    /* Read SCRATCH before */
    resp->ScratchBefore = PspGpuProxyReadRegister(0x32D4);

    /* Read WPTR before */
    resp->HqdPqWptrBefore = PspGpuProxyReadRegister(GPU_CP_KIQ_WPTR);

    /* Write PM4 commands to ring */
    KeAcquireSpinLock(&g_KiqRingLock, &irql);
    wptr = g_KiqRingWptr;

    /* Check ring capacity — mask RPTR to 9 bits (KIQ_WPTR is 9-bit, max 512 DWORDs) */
    {
        UINT32 ringDwords = g_KiqRingSize / sizeof(ULONG);
        UINT32 rptr = PspGpuProxyReadRegister(GPU_CP_KIQ_RPTR) & 0x000001FF;
        UINT32 used;
        if (wptr >= rptr) {
            used = wptr - rptr;
        } else {
            used = ringDwords - rptr + wptr;
        }
        if (used + savedCmdCount > ringDwords) {
            KeReleaseSpinLock(&g_KiqRingLock, irql);
            PspGpuProxyWriteRegister(GPU_GRBM_GFX_INDEX, GRBM_GFX_INDEX_BROADCAST);
            return STATUS_BUFFER_TOO_SMALL;
        }
    }

    for (i = 0; i < savedCmdCount; i++) {
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
    if (!PspGpuProxyWriteRegister(GPU_CP_KIQ_WPTR, wptr)) {
        KdPrint(("GPU_PM4: WARNING — Write WPTR=%u via proxy failed\n", wptr));
    }

    resp->Pm4Dwords = savedCmdCount;
    resp->KiqRingWptr = wptr;

    /* Wait if requested */
    if (savedWaitMs > 0) {
        LARGE_INTEGER delay;
        delay.QuadPart = -(LONGLONG)10000 * savedWaitMs;
        KeDelayExecutionThread(KernelMode, FALSE, &delay);
    }

    /* Read results */
    resp->ScratchAfter = PspGpuProxyReadRegister(0x32D4);
    resp->HqdPqWptrAfter = PspGpuProxyReadRegister(GPU_CP_KIQ_WPTR);
    resp->WptrReadback = PspGpuProxyReadRegister(GPU_CP_KIQ_WPTR);

    /* Restore GRBM_GFX_INDEX */
    PspGpuProxyWriteRegister(GPU_GRBM_GFX_INDEX, GRBM_GFX_INDEX_BROADCAST);
    resp->KiqRingSize = g_KiqRingSize;
    resp->KiqRingPa = g_KiqRingPa.LowPart;
    resp->HqdActive = PspGpuProxyReadRegister(GPU_CP_HQD_ACTIVE);

    KdPrint(("GPU_PM4: Scratch=0x%08X->0x%08X WPTR=%u->%u PM4=%u PA=0x%llX HqdActive=%u\n",
        resp->ScratchBefore, resp->ScratchAfter,
        resp->HqdPqWptrBefore, resp->HqdPqWptrAfter,
        savedCmdCount, g_KiqRingPa.QuadPart, resp->HqdActive));

    return STATUS_SUCCESS;
}
