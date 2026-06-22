// PspKiq.c - KIQ (Queue) Ring functionality for AMD BC-250 PSP
// Programs GPU HQD registers so hardware can execute PM4 commands from the ring.
#include <ntddk.h>
#include <wdm.h>
#include "PspIoctl.h"
#include "PspCore.h"
#include "PspKiq.h"

/* ---- GPU register offsets (BAR5-relative, GC_BASE-shifted) ----
 * These match the GPU driver's amdbc250_dream_hw.h offsets exactly.
 * All offsets are byte offsets from GPU BAR5 base (0xFE800000). */
#define GPU_GRBM_GFX_INDEX              0x34D0
#define GPU_CP_ME_CNTL                  0x4A74
#define GPU_CP_KIQ_BASE_LO              0xE060
#define GPU_CP_KIQ_BASE_HI              0xE064
#define GPU_CP_KIQ_RPTR                 0xE06C
#define GPU_CP_KIQ_WPTR                 0xE078
#define GPU_CP_HQD_ACTIVE               0xDAC0
#define GPU_CP_HQD_VMID                 0xDAC4
#define GPU_CP_HQD_PERSISTENT_STATE     0xDAC8
#define GPU_CP_HQD_PQ_BASE              0xDAD8
#define GPU_CP_HQD_PQ_BASE_HI           0xDADC
#define GPU_CP_HQD_PQ_RPTR              0xDAE0
#define GPU_CP_HQD_PQ_RPTR_REPORT_ADDR  0xDAE4
#define GPU_CP_HQD_PQ_RPTR_REPORT_ADDR_HI 0xDAE8
#define GPU_CP_HQD_PQ_WPTR_POLL_ADDR    0xDAEC
#define GPU_CP_HQD_PQ_WPTR_POLL_ADDR_HI 0xDAF0
#define GPU_CP_HQD_PQ_DOORBELL_CONTROL  0xDAF4
#define GPU_CP_HQD_PQ_CONTROL           0xDAFC
#define GPU_CP_HQD_PQ_WPTR_POLL_CNTL    0xDB00
#define GPU_CP_HQD_EOP_BASE_ADDR        0xDB4C
#define GPU_CP_HQD_EOP_BASE_ADDR_HI     0xDB50
#define GPU_CP_HQD_EOP_CONTROL          0xDB54
#define GPU_CP_HQD_PQ_WPTR_LO           0xDB90
#define GPU_CP_HQD_PQ_WPTR_HI           0xDB94
#define GPU_RLC_CP_SCHEDULERS           0xECA1

/* GRBM_GFX_INDEX values */
#define GRBM_GFX_INDEX_KIQ     0x00010000  /* ME=1, selects KIQ engine */
#define GRBM_GFX_INDEX_BROADCAST 0xE0000000

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

static NTSTATUS PspKiqProgramHwRegisters(PDEVICE_EXTENSION devExt)
{
    ULONG ringSizeDwords;
    ULONG pqControl;

    KdPrint(("KIQ_HW: Programming GPU HQD registers (g_Bar5Mapping=%p, GpuMmioBase=%p)\n",
        g_Bar5Mapping, devExt->GpuMmioBase));

    if (!g_Bar5Mapping && !devExt->GpuMmioBase) {
        KdPrint(("KIQ_HW: No BAR5 mapping available, cannot program HQD\n"));
        return STATUS_DEVICE_NOT_READY;
    }

    /* 1. Halt ME + PFP before programming */
    if (!PspGpuProxyWriteRegister(GPU_CP_ME_CNTL, ME_CNTL_ME_HALT | ME_CNTL_PFP_HALT)) {
        KdPrint(("KIQ_HW: Failed to halt ME+PFP\n"));
        return STATUS_DEVICE_NOT_READY;
    }
    KeStallExecutionProcessor(10);

    /* 2. Select KIQ engine: GRBM_GFX_INDEX = ME=1 */
    PspGpuProxyWriteRegister(GPU_GRBM_GFX_INDEX, GRBM_GFX_INDEX_KIQ);
    KeStallExecutionProcessor(1);

    /* 3. Deactivate queue first */
    PspGpuProxyWriteRegister(GPU_CP_HQD_ACTIVE, 0);
    KeStallExecutionProcessor(1);

    /* 4. Disable WPTR polling and doorbell */
    PspGpuProxyWriteRegister(GPU_CP_HQD_PQ_WPTR_POLL_CNTL, 0);
    PspGpuProxyWriteRegister(GPU_CP_HQD_PQ_DOORBELL_CONTROL, 0);

    /* 5. Clear EOP (not using interrupt fence for now) */
    PspGpuProxyWriteRegister(GPU_CP_HQD_EOP_BASE_ADDR, 0);
    PspGpuProxyWriteRegister(GPU_CP_HQD_EOP_BASE_ADDR_HI, 0);
    PspGpuProxyWriteRegister(GPU_CP_HQD_EOP_CONTROL, 0x08000000);

    /* 6. Clear RPTR report and WPTR poll addresses */
    PspGpuProxyWriteRegister(GPU_CP_HQD_PQ_RPTR_REPORT_ADDR, 0);
    PspGpuProxyWriteRegister(GPU_CP_HQD_PQ_RPTR_REPORT_ADDR_HI, 0);
    PspGpuProxyWriteRegister(GPU_CP_HQD_PQ_WPTR_POLL_ADDR, 0);
    PspGpuProxyWriteRegister(GPU_CP_HQD_PQ_WPTR_POLL_ADDR_HI, 0);

    /* 7. Set PQ base (ring buffer physical address, 256-byte aligned) */
    PspGpuProxyWriteRegister(GPU_CP_HQD_PQ_BASE,
        (ULONG)(g_KiqRingPa.QuadPart & 0xFFFFFF00));
    PspGpuProxyWriteRegister(GPU_CP_HQD_PQ_BASE_HI,
        (ULONG)(g_KiqRingPa.QuadPart >> 32));

    /* 8. Set PQ control = log2(ring_size_in_dwords) */
    ringSizeDwords = g_KiqRingSize / sizeof(ULONG);
    pqControl = 0;
    { ULONG tmp = ringSizeDwords; while (tmp > 1) { tmp >>= 1; pqControl++; } }
    PspGpuProxyWriteRegister(GPU_CP_HQD_PQ_CONTROL, pqControl);

    /* 9. Set VMID = 0 */
    PspGpuProxyWriteRegister(GPU_CP_HQD_VMID, 0);

    /* 10. Set persistent state */
    PspGpuProxyWriteRegister(GPU_CP_HQD_PERSISTENT_STATE, 0xE001);

    /* 11. Clear RPTR and WPTR */
    PspGpuProxyWriteRegister(GPU_CP_HQD_PQ_RPTR, 0);
    PspGpuProxyWriteRegister(GPU_CP_HQD_PQ_WPTR_LO, 0);
    PspGpuProxyWriteRegister(GPU_CP_HQD_PQ_WPTR_HI, 0);

    /* 12. Set KIQ_BASE (KIQ engine reads commands from here!) */
    PspGpuProxyWriteRegister(GPU_CP_KIQ_BASE_LO,
        (ULONG)(g_KiqRingPa.QuadPart & 0xFFFFFFFF));
    PspGpuProxyWriteRegister(GPU_CP_KIQ_BASE_HI,
        (ULONG)(g_KiqRingPa.QuadPart >> 32));
    PspGpuProxyWriteRegister(GPU_CP_KIQ_RPTR, 0);
    PspGpuProxyWriteRegister(GPU_CP_KIQ_WPTR, 0);

    /* 13. Re-select KIQ engine before activate (ME=1) — must be selected for HQD_ACTIVE */
    PspGpuProxyWriteRegister(GPU_GRBM_GFX_INDEX, GRBM_GFX_INDEX_KIQ);
    KeStallExecutionProcessor(1);

    /* 14. Activate queue */
    PspGpuProxyWriteRegister(GPU_CP_HQD_ACTIVE, 1);
    KeStallExecutionProcessor(1);

    /* 15. Notify RLC scheduler */
    PspGpuProxyWriteRegister(GPU_RLC_CP_SCHEDULERS, RLC_SCHEDULERS_KIQ);

    /* 16. Resume CP (clear ME_HALT + PFP_HALT) */
    PspGpuProxyWriteRegister(GPU_CP_ME_CNTL, 0);
    KeStallExecutionProcessor(100);

    KdPrint(("KIQ_HW: HQD programmed — ring PA=0x%llX size=%u log2(dwords)=%u\n",
        g_KiqRingPa.QuadPart, g_KiqRingSize, pqControl));

    return STATUS_SUCCESS;
}

NTSTATUS PspKiqInit(PDEVICE_EXTENSION devExt, ULONG64 ringPA, ULONG ringSize, ULONG cmdBufSize)
{
    PHYSICAL_ADDRESS highAddr;
    PHYSICAL_ADDRESS lowAddr;
    PHYSICAL_ADDRESS boundary;

    UNREFERENCED_PARAMETER(cmdBufSize);

    if (g_KiqRingInitialized) {
        return STATUS_SUCCESS;
    }

    if (!devExt->MmioBase) {
        return STATUS_DEVICE_NOT_READY;
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

    /* Update WPTR in GPU hardware registers — both HQD and KIQ paths */
    PspGpuProxyWriteRegister(GPU_CP_HQD_PQ_WPTR_LO, wptr);
    PspGpuProxyWriteRegister(GPU_CP_HQD_PQ_WPTR_HI, 0);
    PspGpuProxyWriteRegister(GPU_CP_KIQ_WPTR, wptr);

    return STATUS_SUCCESS;
}

VOID PspKiqCleanup(VOID)
{
    /* Deactivate GPU queue if possible */
    if (g_Bar5Mapping) {
        PspGpuProxyWriteRegister(GPU_CP_HQD_ACTIVE, 0);
        PspGpuProxyWriteRegister(GPU_CP_ME_CNTL, ME_CNTL_ME_HALT | ME_CNTL_PFP_HALT);
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

    /* Read HQD_ACTIVE to check queue state */
    resp->HqdActive = PspGpuProxyReadRegister(GPU_CP_HQD_ACTIVE);

    /* Read WPTR before */
    resp->HqdPqWptrBefore = PspGpuProxyReadRegister(GPU_CP_HQD_PQ_WPTR_LO);

    /* Write PM4 commands to ring */
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

    /* Memory barrier */
    KeMemoryBarrier();

    /* Update WPTR in polling page */
    if (g_WptrPollVa) {
        *(volatile PULONG)(g_WptrPollVa) = wptr;
    }

    /* Kick GPU WPTR — both HQD and KIQ paths */
    PspGpuProxyWriteRegister(GPU_CP_HQD_PQ_WPTR_LO, wptr);
    PspGpuProxyWriteRegister(GPU_CP_HQD_PQ_WPTR_HI, 0);
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
    resp->HqdPqWptrAfter = PspGpuProxyReadRegister(GPU_CP_HQD_PQ_WPTR_LO);
    resp->WptrReadback = PspGpuProxyReadRegister(GPU_CP_KIQ_WPTR);
    resp->KiqRingSize = g_KiqRingSize;
    resp->KiqRingPa = g_KiqRingPa.LowPart;

    KdPrint(("GPU_PM4: Scratch=0x%08X->0x%08X WPTR=%u->%u Active=0x%08X PM4=%u\n",
        resp->ScratchBefore, resp->ScratchAfter,
        resp->HqdPqWptrBefore, resp->HqdPqWptrAfter,
        resp->HqdActive, req->CommandCount));

    return STATUS_SUCCESS;
}
