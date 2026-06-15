#include <ntddk.h>
#include <wdm.h>

// Must define NT device name BEFORE including PspIoctl.h (which defines the user-mode symlink name)
#define PSP_NT_DEVICE_NAME     L"\\Device\\AmdBcPsp"
#define PSP_SYMBOLIC_LINK_NAME L"\\DosDevices\\AmdBcPsp"

#include "PspIoctl.h"
#include "firmware_data.h"

// BUS_DATA_TYPE for PCI config access via HAL
#define PCIConfiguration 0

// NBIO signature registers for firewall unlock
#define NBIO_SIG1_OFFSET       0xC100
#define NBIO_SIG2_OFFSET       0xC180
#define NBIO_SIG1_VALUE        0xFEDCBAEF
#define NBIO_SIG2_VALUE        0xFEDCBADF
#define MMHUB_CHECK_OFFSET     0x50D0

// GC register base offset on BC-250 — all GC registers shifted by 0x1260
#define AMDBC250_GC_BASE        0x1260

#define PSP_FW_WAIT_MS         500
#define PSP_TOS_READY_TIMEOUT  60000  // 60 second timeout for TOS_READY (Linux shows ~5.5s on BC-250)
#define PSP_BOOT_CMD_0xC100    0xC100
#define PSP_BOOT_CMD_0xC180    0xC180
#define PSP_BAR0_PHYSICAL      0xFD600000ULL

#define PSP_BAR0_PHYSICAL      0xFD600000ULL
#define PSP_BAR0_SIZE          0x40000

#define PSP_MAILBOX_BASE(devExt) (devExt->Bar0Base ? devExt->Bar0Base : devExt->MmioBase)
#define PSP_READ_MAILBOX(devExt, offset) \
    READ_REGISTER_ULONG((PULONG)((PUCHAR)PSP_MAILBOX_BASE(devExt) + (offset)))
#define PSP_WRITE_MAILBOX(devExt, offset, value) \
    WRITE_REGISTER_ULONG((PULONG)((PUCHAR)PSP_MAILBOX_BASE(devExt) + (offset)), (value))

typedef struct _DEVICE_EXTENSION {
    PVOID       MmioBase;
    ULONG       MmioSize;
    PVOID       Bar0Base;
    ULONG       Bar0Size;
    PVOID       FwBuffer;
    PHYSICAL_ADDRESS FwPhysical;
    ULONG       FwSize;
    ULONG       FwPaShifted;
    PVOID       RingBuffer;
    PHYSICAL_ADDRESS RingPhysical;
    ULONG       RingSize;
    BOOLEAN     RingCreated;
    KSPIN_LOCK  CommandLock;
    PVOID       PciCfgBase;
    ULONG       PciCfgSize;
} DEVICE_EXTENSION, *PDEVICE_EXTENSION;

DRIVER_INITIALIZE DriverEntry;
DRIVER_UNLOAD DriverUnload;
DRIVER_DISPATCH PspCreateClose;
DRIVER_DISPATCH PspDeviceControl;

/* Static 4KB ring buffer - one physical page, always contiguous */
static UCHAR g_RingBuffer[0x1000];
static BOOLEAN g_RingBufferInitialized = FALSE;
static PHYSICAL_ADDRESS g_RingBufferPhysical;
static ULONG ringWriteOffset = 0;
static ULONG g_RingFwCount = 0;              // Number of GPU FW loaded via ring
static PVOID g_TmrBuffer = NULL;             // TMR (Trusted Memory Region) buffer
static PHYSICAL_ADDRESS g_TmrPhysical;        // Physical address of TMR buffer
static ULONG g_TmrSize = 0;                  // TMR size in bytes
static BOOLEAN g_TmrInitialized = FALSE;     // Whether TMR has been initialized

// KIQ ring for GPU command submission
static PVOID g_KiqRingVa = NULL;
static PHYSICAL_ADDRESS g_KiqRingPa = {0};
static ULONG g_KiqRingSize = 0;
static ULONG g_KiqRingWptr = 0;
static BOOLEAN g_KiqRingInitialized = FALSE;
static KSPIN_LOCK g_KiqRingLock;

// KIQ register offsets (BAR5-relative, BC-250)
#define KIQ_BASE_LO       0xE060
#define KIQ_BASE_HI       0xE064
#define KIQ_CNTL          0xE068
#define KIQ_RPTR          0xE06C
#define KIQ_WPTR          0xE078
#define GRBM_GFX_INDEX    0x34D0
#define KIQ_GRBM_VAL      0x00010000  // ME=1, PIPE=0, QUEUE=0
#define KIQ_RING_SIZE     0x2000      // 8KB ring buffer

// SMU v11.8 protocol: send message via C2PMSG_66, read response from C2PMSG_82
// Response valid when C2PMSG_90 != 0 (0=busy, 1=OK, 0xFF=error)
static NTSTATUS PspSmuWake(PDEVICE_EXTENSION devExt, ULONG message, ULONG argument, PULONG pResponse)
{
    ULONG timeout;
    ULONG resp = 0;

    // Check if already initialized
    if (devExt->MmioBase == NULL) {
        return STATUS_DEVICE_NOT_READY;
    }

    // Write 0 to C2PMSG_90 to clear any previous response
    WRITE_REGISTER_ULONG((PULONG)((PUCHAR)devExt->MmioBase + MP1_BASE + SMU_C2PMSG_90_OFFSET), 0);

    // Write argument to C2PMSG_82
    WRITE_REGISTER_ULONG((PULONG)((PUCHAR)devExt->MmioBase + MP1_BASE + SMU_C2PMSG_82_OFFSET), argument);

    // Write message to C2PMSG_66 (triggers SMU)
    WRITE_REGISTER_ULONG((PULONG)((PUCHAR)devExt->MmioBase + MP1_BASE + SMU_C2PMSG_66_OFFSET), message);

    KdPrint(("SMU_WAKE: msg=0x%08X arg=0x%08X sent\n", message, argument));

    // Poll C2PMSG_90 until response ready (up to ~1s)
    for (timeout = 0; timeout < 1000; timeout++) {
        KeStallExecutionProcessor(1000);
        resp = READ_REGISTER_ULONG((PULONG)((PUCHAR)devExt->MmioBase + MP1_BASE + SMU_C2PMSG_90_OFFSET));
        if (resp != 0) {
            break;
        }
    }

    // Read response from C2PMSG_82
    ULONG response = READ_REGISTER_ULONG((PULONG)((PUCHAR)devExt->MmioBase + MP1_BASE + SMU_C2PMSG_82_OFFSET));

    KdPrint(("SMU_WAKE: C2PMSG_90=0x%08X response from C2PMSG_82=0x%08X timeout=%ums\n",
        resp, response, timeout));

    if (timeout >= 1000) {
        KdPrint(("SMU_WAKE: TIMEOUT - SMU not responding (power-gated or no firmware)\n"));
        return STATUS_TIMEOUT;
    }

    if (resp == 0xFFFFFFFF) {
        KdPrint(("SMU_WAKE: ERROR - invalid register reads\n"));
        return STATUS_UNSUCCESSFUL;
    }

    *pResponse = response;
    return (resp == 1) ? STATUS_SUCCESS : STATUS_UNSUCCESSFUL;
}

static NTSTATUS PspKiqInit(PDEVICE_EXTENSION devExt)
{
    PHYSICAL_ADDRESS highAddr;
    
    if (g_KiqRingInitialized) {
        return STATUS_SUCCESS;
    }

    // Allocate physically contiguous, page-aligned ring buffer
    highAddr.QuadPart = 0x100000000ULL;  // Prefer below 4GB
    g_KiqRingVa = MmAllocateContiguousMemory(KIQ_RING_SIZE, highAddr);
    if (g_KiqRingVa == NULL) {
        highAddr.QuadPart = 0xFFFFFFFF;
        g_KiqRingVa = MmAllocateContiguousMemory(KIQ_RING_SIZE, highAddr);
    }
    if (g_KiqRingVa == NULL) {
        KdPrint(("KIQ: Failed to allocate ring buffer\n"));
        return STATUS_INSUFFICIENT_RESOURCES;
    }
    
    RtlZeroMemory(g_KiqRingVa, KIQ_RING_SIZE);
    g_KiqRingPa = MmGetPhysicalAddress(g_KiqRingVa);
    g_KiqRingSize = KIQ_RING_SIZE;
    g_KiqRingWptr = 0;
    KeInitializeSpinLock(&g_KiqRingLock);

    if (devExt->MmioBase == NULL) {
        MmFreeContiguousMemory(g_KiqRingVa);
        g_KiqRingVa = NULL;
        return STATUS_DEVICE_NOT_READY;
    }

    // Set GRBM to select KIQ (ME=1)
    WRITE_REGISTER_ULONG((PULONG)((PUCHAR)devExt->MmioBase + GRBM_GFX_INDEX), KIQ_GRBM_VAL);
    KeStallExecutionProcessor(1);

    // Write KIQ_BASE_LO/HI with ring buffer physical address
    WRITE_REGISTER_ULONG((PULONG)((PUCHAR)devExt->MmioBase + KIQ_BASE_LO), g_KiqRingPa.LowPart);
    WRITE_REGISTER_ULONG((PULONG)((PUCHAR)devExt->MmioBase + KIQ_BASE_HI), g_KiqRingPa.HighPart);

    // Initialize KIQ_WPTR to 0
    WRITE_REGISTER_ULONG((PULONG)((PUCHAR)devExt->MmioBase + KIQ_WPTR), 0);

    // Restore GRBM to broadcast
    WRITE_REGISTER_ULONG((PULONG)((PUCHAR)devExt->MmioBase + GRBM_GFX_INDEX), 0);
    
    g_KiqRingInitialized = TRUE;

    KdPrint(("KIQ: Ring initialized PA=0x%llX VA=%p size=%u\n",
        g_KiqRingPa.QuadPart, g_KiqRingVa, g_KiqRingSize));

    return STATUS_SUCCESS;
}

static NTSTATUS PspKiqSubmit(PDEVICE_EXTENSION devExt, PULONG Pm4Commands, ULONG DwordCount)
{
    KIRQL irql;
    ULONG wptr;
    ULONG i;

    if (!g_KiqRingInitialized) {
        KdPrint(("KIQ: Ring not initialized\n"));
        return STATUS_DEVICE_NOT_READY;
    }
    if (Pm4Commands == NULL || DwordCount == 0 || DwordCount > 64) {
        return STATUS_INVALID_PARAMETER;
    }

    KeAcquireSpinLock(&g_KiqRingLock, &irql);

    // Write PM4 commands to ring buffer with wrap-around
    wptr = g_KiqRingWptr;
    for (i = 0; i < DwordCount; i++) {
        if (wptr >= g_KiqRingSize / sizeof(ULONG)) {
            wptr = 0;
        }
        ((volatile PULONG)g_KiqRingVa)[wptr] = Pm4Commands[i];
        wptr++;
    }
    g_KiqRingWptr = wptr;

    // Ring doorbell: write KIQ_WPTR to trigger GPU execution
    // KIQ_WPTR (0xE078) is accessible via BAR5 MMIO
    WRITE_REGISTER_ULONG((PULONG)((PUCHAR)devExt->MmioBase + KIQ_WPTR), g_KiqRingWptr);

    KeReleaseSpinLock(&g_KiqRingLock, irql);

    KdPrint(("KIQ: Submitted %u DWORDs, WPTR=%u\n", DwordCount, g_KiqRingWptr));
    return STATUS_SUCCESS;
}

static VOID PspKiqCleanup(VOID)
{
    g_KiqRingInitialized = FALSE;
    g_KiqRingWptr = 0;
    g_KiqRingSize = 0;
    if (g_KiqRingVa) {
        MmFreeContiguousMemory(g_KiqRingVa);
        g_KiqRingVa = NULL;
    }
    g_KiqRingPa.QuadPart = 0;
    KdPrint(("KIQ: Ring cleaned up\n"));
}

static BOOLEAN PspValidateFirmware(PUCHAR FirmwareData, ULONG FirmwareSize)
{
    if (FirmwareData == NULL || FirmwareSize < 256)
        return FALSE;

    // FIX #5: Check firmware is non-empty and within reasonable size range
    if (FirmwareSize < 1024 || FirmwareSize > PSP_MAX_FW_TOTAL)
        return FALSE;

    // Check firmware is not all zeros or all FFs
    ULONG sampleStart = *(volatile ULONG*)FirmwareData;
    ULONG sampleMid = *(volatile ULONG*)(FirmwareData + FirmwareSize / 2);
    if (sampleStart == 0 && sampleMid == 0)
        return FALSE;
    if (sampleStart == 0xFFFFFFFF && sampleMid == 0xFFFFFFFF)
        return FALSE;

    KdPrint(("FW validation: size=%u first=0x%08X mid=0x%08X -> OK\n",
        FirmwareSize, sampleStart, sampleMid));
    return TRUE;
}

static VOID PspFreeFirmware(PDEVICE_EXTENSION devExt)
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

static NTSTATUS PspSendMailboxCommand(PDEVICE_EXTENSION devExt, ULONG command)
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

    NTSTATUS status = STATUS_SUCCESS;
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
        status = STATUS_TIMEOUT;
    }

    return status;
}

static NTSTATUS PspInitTmr(PDEVICE_EXTENSION devExt)
{
    if (!devExt->RingCreated) {
        KdPrint(("TMR: Ring not created\n"));
        return STATUS_DEVICE_NOT_READY;
    }
    if (g_TmrInitialized) {
        KdPrint(("TMR: Already initialized\n"));
        return STATUS_SUCCESS;
    }

    // Allocate 4MB TMR buffer (matching Linux: reserve 0x400000 for PSP TMR)
    g_TmrSize = 0x400000;
    if (g_TmrBuffer == NULL) {
        PHYSICAL_ADDRESS highAddr;
        highAddr.QuadPart = 0x10000000000ULL;
        g_TmrBuffer = MmAllocateContiguousMemory(g_TmrSize, highAddr);
        if (g_TmrBuffer == NULL) {
            highAddr.QuadPart = 0xFFFFFFFF;
            g_TmrBuffer = MmAllocateContiguousMemory(g_TmrSize, highAddr);
        }
        if (g_TmrBuffer == NULL) {
            KdPrint(("TMR: Failed to allocate 4MB buffer\n"));
            return STATUS_INSUFFICIENT_RESOURCES;
        }
        RtlZeroMemory(g_TmrBuffer, g_TmrSize);
        g_TmrPhysical = MmGetPhysicalAddress(g_TmrBuffer);
        KdPrint(("TMR: Allocated 4MB at PA=0x%llX\n", g_TmrPhysical.QuadPart));
    }

    // Send INIT_TMR (0x01) via ring — use per-call buffer (no race)
    PVOID cmdBuffer = ExAllocatePool2(POOL_FLAG_NON_PAGED, PSP_CMD_BUF_SIZE, 'mCSP');
    if (!cmdBuffer) return STATUS_INSUFFICIENT_RESOURCES;
    RtlZeroMemory(cmdBuffer, PSP_CMD_BUF_SIZE);
    PULONG cmd = (PULONG)cmdBuffer;
    cmd[0] = PSP_CMD_BUF_SIZE;
    cmd[1] = 1;
    cmd[2] = GFX_CMD_ID_INIT_TMR;
    cmd[7] = (ULONG)(g_TmrPhysical.QuadPart & 0xFFFFFFFF);
    cmd[8] = (ULONG)(g_TmrPhysical.QuadPart >> 32);
    cmd[9] = g_TmrSize;

    PHYSICAL_ADDRESS cmdPa = MmGetPhysicalAddress(cmdBuffer);

    KIRQL ringIrql;
    KeAcquireSpinLock(&devExt->CommandLock, &ringIrql);
    if (ringWriteOffset + PSP_RING_FRAME_SIZE > sizeof(g_RingBuffer))
        ringWriteOffset = 0;
    PULONG frame = (PULONG)(g_RingBuffer + ringWriteOffset);
    RtlZeroMemory(frame, PSP_RING_FRAME_SIZE);
    frame[0] = (ULONG)(cmdPa.QuadPart & 0xFFFFFFFF);
    frame[1] = (ULONG)(cmdPa.QuadPart >> 32);
    frame[2] = PSP_CMD_BUF_SIZE;
    ringWriteOffset += PSP_RING_FRAME_SIZE;

    KdPrint(("TMR: Submitting INIT_TMR via ring\n"));

    PVOID mboxBase = devExt->Bar0Base ? devExt->Bar0Base : devExt->MmioBase;
    WRITE_REGISTER_ULONG((PULONG)((PUCHAR)mboxBase + PSP_C2PMSG_67_OFFSET), ringWriteOffset);
    KeReleaseSpinLock(&devExt->CommandLock, ringIrql);

    ULONG timeout, resp = 0;
    NTSTATUS tmrStatus = STATUS_TIMEOUT;
    for (timeout = 0; timeout < 2000; timeout++) {
        KeStallExecutionProcessor(1000);
        resp = READ_REGISTER_ULONG((PULONG)((PUCHAR)mboxBase + PSP_C2PMSG_64_OFFSET));
        if (resp & 0x80000000) {
            ULONG st = resp & 0x0000FFFF;
            ULONG cbStatus = ((PULONG)cmdBuffer)[864/sizeof(ULONG)];
            KdPrint(("TMR: C2PMSG_64=0x%08X cmdResp=0x%08X st=%u\n", resp, cbStatus, st));
            if (st == 0 && cbStatus == 0) {
                g_TmrInitialized = TRUE;
                tmrStatus = STATUS_SUCCESS;
            } else {
                tmrStatus = STATUS_UNSUCCESSFUL;
            }
            break;
        }
    }
    ExFreePool(cmdBuffer);
    if (NT_SUCCESS(tmrStatus)) {
        KdPrint(("TMR: Initialized OK at PA=0x%llX\n", g_TmrPhysical.QuadPart));
    } else if (timeout >= 2000) {
        KdPrint(("TMR: timeout waiting for response\n"));
    }
    return tmrStatus;
}

// Free TMR buffer on driver unload
static VOID PspFreeTmr(VOID)
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

NTSTATUS DriverEntry(_In_ PDRIVER_OBJECT DriverObject, _In_ PUNICODE_STRING RegistryPath)
{
    NTSTATUS status;
    PDEVICE_OBJECT deviceObject = NULL;
    UNICODE_STRING deviceName;
    UNICODE_STRING symLinkName;

    UNREFERENCED_PARAMETER(RegistryPath);

    KdPrint(("=== AMD BC-250 PSP Driver: DriverEntry ===\n"));

    DriverObject->MajorFunction[IRP_MJ_CREATE] = PspCreateClose;
    DriverObject->MajorFunction[IRP_MJ_CLOSE] = PspCreateClose;
    DriverObject->MajorFunction[IRP_MJ_DEVICE_CONTROL] = PspDeviceControl;
    DriverObject->DriverUnload = DriverUnload;

    RtlInitUnicodeString(&deviceName, PSP_NT_DEVICE_NAME);
    status = IoCreateDevice(
        DriverObject,
        sizeof(DEVICE_EXTENSION),
        &deviceName,
        FILE_DEVICE_UNKNOWN,
        0,
        FALSE,
        &deviceObject
    );
    if (!NT_SUCCESS(status)) {
        KdPrint(("IoCreateDevice failed: 0x%08X\n", status));
        return status;
    }

    // FIX #9: Initialize spinlock
    PDEVICE_EXTENSION devExt = (PDEVICE_EXTENSION)deviceObject->DeviceExtension;
    KeInitializeSpinLock(&devExt->CommandLock);

    RtlInitUnicodeString(&symLinkName, PSP_SYMBOLIC_LINK_NAME);
    status = IoCreateSymbolicLink(&symLinkName, &deviceName);
    if (!NT_SUCCESS(status)) {
        KdPrint(("IoCreateSymbolicLink failed: 0x%08X\n", status));
        IoDeleteDevice(deviceObject);
        return status;
    }

    deviceObject->Flags |= DO_BUFFERED_IO;
    deviceObject->Flags &= ~DO_DEVICE_INITIALIZING;

    KdPrint(("=== AMD BC-250 PSP Driver: Initialized ===\n"));
    return STATUS_SUCCESS;
}

VOID DriverUnload(_In_ PDRIVER_OBJECT DriverObject)
{
    UNICODE_STRING symLinkName;
    PDEVICE_OBJECT deviceObject = DriverObject->DeviceObject;
    PDEVICE_EXTENSION devExt = (PDEVICE_EXTENSION)deviceObject->DeviceExtension;

    KdPrint(("=== AMD BC-250 PSP Driver: Unload ===\n"));

    PspFreeTmr();
    PspKiqCleanup();
    PspFreeFirmware(devExt);

    if (devExt->PciCfgBase != NULL) {
        MmUnmapIoSpace(devExt->PciCfgBase, devExt->PciCfgSize);
        devExt->PciCfgBase = NULL;
    }

    if (devExt->Bar0Base != NULL) {
        MmUnmapIoSpace(devExt->Bar0Base, devExt->Bar0Size);
        devExt->Bar0Base = NULL;
    }

    if (devExt->MmioBase != NULL && devExt->MmioBase != devExt->Bar0Base) {
        MmUnmapIoSpace(devExt->MmioBase, devExt->MmioSize);
        devExt->MmioBase = NULL;
    }
    KdPrint(("BAR5 resources released\n"));

    RtlInitUnicodeString(&symLinkName, PSP_SYMBOLIC_LINK_NAME);
    IoDeleteSymbolicLink(&symLinkName);
    IoDeleteDevice(deviceObject);
}

/* Static helper function for auto-initialization */
static NTSTATUS PspAutoInitialize(PDEVICE_EXTENSION devExt)
{
    if (devExt->Bar0Base == NULL) {
        PHYSICAL_ADDRESS physAddr;
        physAddr.QuadPart = PSP_BAR0_PHYSICAL;
        
        devExt->Bar0Base = MmMapIoSpace(physAddr, PSP_BAR0_SIZE, MmNonCached);
        if (devExt->Bar0Base == NULL) {
            KdPrint(("PSP: BAR0 map failed at 0x%llX, using BAR5 for mailbox\n", physAddr.QuadPart));
        } else {
            devExt->Bar0Size = PSP_BAR0_SIZE;
            KdPrint(("PSP: BAR0 mapped: PA=0x%llX VA=%p size=%u\n", physAddr.QuadPart, devExt->Bar0Base, devExt->Bar0Size));
        }
    }
    
    if (devExt->MmioBase == NULL) {
        devExt->MmioBase = devExt->Bar0Base ? devExt->Bar0Base : devExt->MmioBase;
        devExt->MmioSize = devExt->Bar0Size;
    }
    
    return STATUS_SUCCESS;
}

NTSTATUS PspCreateClose(_In_ PDEVICE_OBJECT DeviceObject, _In_ PIRP Irp)
{
    UNREFERENCED_PARAMETER(DeviceObject);
    Irp->IoStatus.Status = STATUS_SUCCESS;
    Irp->IoStatus.Information = 0;
    IoCompleteRequest(Irp, IO_NO_INCREMENT);
    return STATUS_SUCCESS;
}

NTSTATUS PspDeviceControl(_In_ PDEVICE_OBJECT DeviceObject, _In_ PIRP Irp)
{
    PDEVICE_EXTENSION devExt = (PDEVICE_EXTENSION)DeviceObject->DeviceExtension;
    PIO_STACK_LOCATION irpStack = IoGetCurrentIrpStackLocation(Irp);
    NTSTATUS status = STATUS_SUCCESS;
    ULONG bytesReturned = 0;
    PVOID inputBuffer = NULL;
    PVOID outputBuffer = NULL;
    ULONG inputLength = 0;
    ULONG outputLength = 0;
    ULONG ioctlCode = irpStack->Parameters.DeviceIoControl.IoControlCode;

    inputBuffer = Irp->AssociatedIrp.SystemBuffer;
    outputBuffer = Irp->AssociatedIrp.SystemBuffer;
    inputLength = irpStack->Parameters.DeviceIoControl.InputBufferLength;
    outputLength = irpStack->Parameters.DeviceIoControl.OutputBufferLength;

    if (devExt->MmioBase == NULL) {
        NTSTATUS autoStatus = PspAutoInitialize(devExt);
        if (!NT_SUCCESS(autoStatus)) {
            KdPrint(("MMIO not initialized (auto-init failed: 0x%08X)\n", autoStatus));
            Irp->IoStatus.Status = STATUS_DEVICE_NOT_READY;
            Irp->IoStatus.Information = 0;
            IoCompleteRequest(Irp, IO_NO_INCREMENT);
            return autoStatus;
        }
    }

    switch (ioctlCode) {
case IOCTL_PSP_INIT_HW:
        {
            if (inputLength < sizeof(PSP_INIT_HW_REQUEST)) {
                status = STATUS_INVALID_PARAMETER;
                break;
            }

            NTSTATUS bar0Status = PspAutoInitialize(devExt);
            if (bar0Status < 0) {
                KdPrint(("BAR0 init note: 0x%08X\n", bar0Status));
            }

            if (devExt->Bar0Base == NULL) {
                NTSTATUS autoStatus = PspAutoInitialize(devExt);
                if (autoStatus < 0) {
                    KdPrint(("BAR0 not initialized (auto-init failed: 0x%08X)\n", autoStatus));
                    Irp->IoStatus.Status = STATUS_DEVICE_NOT_READY;
                    Irp->IoStatus.Information = 0;
                    IoCompleteRequest(Irp, IO_NO_INCREMENT);
                    return autoStatus;
                }
            }

            PSP_INIT_HW_REQUEST* req = (PSP_INIT_HW_REQUEST*)inputBuffer;
            PHYSICAL_ADDRESS physAddr;
            physAddr.QuadPart = req->PhysicalAddress;
            ULONG size = req->Size;

            if (physAddr.QuadPart == 0 || size == 0 || size > 0x400000) {
                status = STATUS_INVALID_PARAMETER;
                KdPrint(("INIT_HW: invalid PA=0x%llX size=%u\n", physAddr.QuadPart, size));
                break;
            }

            if (devExt->MmioBase != devExt->Bar0Base && devExt->MmioBase != NULL) {
                MmUnmapIoSpace(devExt->MmioBase, devExt->MmioSize);
            }

            devExt->MmioBase = MmMapIoSpace(physAddr, size, MmNonCached);
            if (devExt->MmioBase == NULL) {
                status = STATUS_INSUFFICIENT_RESOURCES;
                KdPrint(("MmMapIoSpace failed for PA=0x%llX size=%u\n", physAddr.QuadPart, size));
                break;
            }

            devExt->MmioSize = size;
            KdPrint(("MMIO mapped: PA=0x%llX VA=%p size=%u\n", physAddr.QuadPart, devExt->MmioBase, size));

            // Map PCI ECAM region for config space (try multiple common addresses)
            if (devExt->PciCfgBase == NULL) {
                ULONGLONG ecamCandidates[] = {
                    0xE0000000ULL,  // AMD standard (256MB range from e820)
                    0xF0000000ULL,  // AMD fallback
                    0xC0000000ULL,  // VRAM base - unlikely but possible
                    0xE000000000ULL // Above 4GB
                };
                for (int e = 0; e < 4 && devExt->PciCfgBase == NULL; e++) {
                    PHYSICAL_ADDRESS ecamPhys;
                    ecamPhys.QuadPart = ecamCandidates[e];
                    PVOID mapped = MmMapIoSpace(ecamPhys, 0x100000, MmNonCached);
                    if (mapped) {
                        // Verify by reading Vendor/Device ID at B0D0F0 offset 0
                        ULONG cfg0 = READ_REGISTER_ULONG((PULONG)mapped);
                        if (cfg0 != 0 && cfg0 != 0xFFFFFFFF) {
                            devExt->PciCfgBase = mapped;
                            devExt->PciCfgSize = 0x100000;
                            KdPrint(("PCI ECAM found at 0x%llX (Vendor=0x%08X)\n", ecamCandidates[e], cfg0));
                        } else {
                            MmUnmapIoSpace(mapped, 0x100000);
                            KdPrint(("PCI ECAM candidate 0x%llX gave 0x%08X - not valid\n", ecamCandidates[e], cfg0));
                        }
                    }
                }
                if (devExt->PciCfgBase == NULL) {
                    KdPrint(("PCI ECAM not found - config reads will fail\n"));
                }
            }

            /* Initialize global ring buffer physical address (once) */
            if (!g_RingBufferInitialized) {
                g_RingBufferPhysical = MmGetPhysicalAddress(g_RingBuffer);
                g_RingBufferInitialized = TRUE;
                KdPrint(("Ring buffer at PA=0x%llX\n", g_RingBufferPhysical.QuadPart));
            }

            bytesReturned = sizeof(ULONG);
            if (outputLength >= sizeof(ULONG)) {
                ((PULONG)outputBuffer)[0] = (ULONG)(ULONG_PTR)devExt->MmioBase;
            }
            break;
        }

        case IOCTL_PSP_READ_REG:
        {
            if (inputLength < sizeof(ULONG) * 2) {
                status = STATUS_INVALID_PARAMETER;
                break;
            }

            PULONG params = (PULONG)inputBuffer;
            ULONG offset = params[0];

            // FIX #3: Enhanced buffer validation before register access
            if (offset >= devExt->MmioSize || (offset + sizeof(ULONG) > devExt->MmioSize) || (offset & 0x3)) {
                KdPrint(("READ_REG: Offset 0x%X out of bounds (MMIO size: %u)\n", offset, devExt->MmioSize));
                status = STATUS_ARRAY_BOUNDS_EXCEEDED;
                break;
            }

            if (outputLength < sizeof(ULONG)) {
                status = STATUS_BUFFER_TOO_SMALL;
                break;
            }

            ULONG value = READ_REGISTER_ULONG((PULONG)((PUCHAR)devExt->MmioBase + offset));
            ((PULONG)outputBuffer)[0] = value;
            bytesReturned = sizeof(ULONG);
            break;
        }

        case IOCTL_PSP_WRITE_REG:
        {
            if (inputLength < sizeof(ULONG) * 2) {
                status = STATUS_INVALID_PARAMETER;
                break;
            }

            PULONG params = (PULONG)inputBuffer;
            ULONG offset = params[0];
            ULONG value = params[1];

            // FIX #3: Enhanced buffer validation before register access
            if (offset >= devExt->MmioSize || (offset + sizeof(ULONG) > devExt->MmioSize) || (offset & 0x3)) {
                KdPrint(("WRITE_REG: Offset 0x%X out of bounds (MMIO size: %u)\n", offset, devExt->MmioSize));
                status = STATUS_ARRAY_BOUNDS_EXCEEDED;
                break;
            }

            WRITE_REGISTER_ULONG((PULONG)((PUCHAR)devExt->MmioBase + offset), value);
            bytesReturned = sizeof(ULONG);
            break;
        }

        case IOCTL_PSP_LOAD_FW:
        {
            if (inputLength == 0) {
                status = STATUS_INVALID_PARAMETER;
                break;
            }

            // FIX #5: Validate firmware size before allocation
            if (inputLength > PSP_MAX_FW_TOTAL) {
                KdPrint(("IOCTL_PSP_LOAD_FW: Firmware too large (%u > %u)\n", inputLength, PSP_MAX_FW_TOTAL));
                status = STATUS_INVALID_PARAMETER;
                break;
            }

            // Free previous firmware if any
            PspFreeFirmware(devExt);

            PHYSICAL_ADDRESS highAddr;
            highAddr.QuadPart = 0x10000000000ULL;  // Allow allocation above 4GB (system RAM up to 0x45FFFFFFF)

            devExt->FwBuffer = MmAllocateContiguousMemory(inputLength, highAddr);
            if (devExt->FwBuffer == NULL) {
                // Fallback to below 4GB
                highAddr.QuadPart = 0xFFFFFFFF;
                devExt->FwBuffer = MmAllocateContiguousMemory(inputLength, highAddr);
            }
            if (devExt->FwBuffer == NULL) {
                status = STATUS_INSUFFICIENT_RESOURCES;
                KdPrint(("IOCTL_PSP_LOAD_FW: Failed to allocate contiguous memory\n"));
                break;
            }

            devExt->FwSize = (ULONG)inputLength;
            RtlCopyMemory(devExt->FwBuffer, inputBuffer, inputLength);
            devExt->FwPhysical = MmGetPhysicalAddress(devExt->FwBuffer);
            devExt->FwPaShifted = (ULONG)(devExt->FwPhysical.QuadPart >> 20);

            KdPrint(("IOCTL_PSP_LOAD_FW: Firmware loaded PA=0x%llX PA>>20=0x%08X size=%u (PERSISTENT)\n",
                devExt->FwPhysical.QuadPart, devExt->FwPaShifted, devExt->FwSize));

            /* Firmware loaded, ready for SEND_CMD. Don't send commands here -
               the user will send them via -C option. */
            if (outputLength >= sizeof(ULONG)) {
                ((PULONG)outputBuffer)[0] = devExt->FwPaShifted;
                bytesReturned = sizeof(ULONG);
            }

            // Buffer stays allocated for subsequent SEND_CMD calls
            break;
        }

        case IOCTL_PSP_SEND_CMD:
        {
            if (inputLength < sizeof(ULONG)) {
                status = STATUS_INVALID_PARAMETER;
                break;
            }

            if (devExt->FwBuffer == NULL) {
                KdPrint(("IOCTL_PSP_SEND_CMD: No firmware loaded. Use IOCTL_PSP_LOAD_FW first.\n"));
                status = STATUS_NO_MEMORY;
                break;
            }

            ULONG command = ((PULONG)inputBuffer)[0];
            KdPrint(("IOCTL_PSP_SEND_CMD: Sending command 0x%08X\n", command));

            status = PspSendMailboxCommand(devExt, command);

            if (NT_SUCCESS(status) && outputLength >= sizeof(ULONG)) {
                ((PULONG)outputBuffer)[0] = command;
                bytesReturned = sizeof(ULONG);
            }
            break;
        }

        case IOCTL_PSP_PCI_READ:
        {
            if (inputLength < sizeof(ULONG) * 3) { status = STATUS_INVALID_PARAMETER; break; }
            PULONG params = (PULONG)inputBuffer;
            ULONG bus = params[0];
            ULONG devFn = params[1];
            ULONG off = params[2];
            ULONG value = 0xFFFFFFFF;

            // Try ECAM first, fall back to HAL
            if (devExt->PciCfgBase != NULL) {
                ULONG addr = (bus * 0x100000) + (devFn * 0x1000) + (off & ~3);
                if (addr + sizeof(ULONG) <= devExt->PciCfgSize) {
                    value = READ_REGISTER_ULONG((PULONG)((PUCHAR)devExt->PciCfgBase + addr));
                }
            }
            if (value == 0xFFFFFFFF) {
                // Try HAL
                ULONG slot = (devFn >> 3) & 0x1F;
                ULONG func = devFn & 7;
                HalGetBusDataByOffset(PCIConfiguration, bus, (slot << 3) | func, &value, off & ~3, sizeof(ULONG));
            }

            KdPrint(("PCI_READ: B%d.D%d.F%d off=0x%X => 0x%08X\n", bus, (devFn>>3)&0x1F, devFn&7, off, value));
            if (outputLength >= sizeof(ULONG)) {
                ((PULONG)outputBuffer)[0] = value;
                bytesReturned = sizeof(ULONG);
            }
            break;
        }

        case IOCTL_PSP_PCI_WRITE:
        {
            if (inputLength < sizeof(ULONG) * 4) { status = STATUS_INVALID_PARAMETER; break; }
            PULONG params = (PULONG)inputBuffer;
            ULONG bus = params[0];
            ULONG devFn = params[1];
            ULONG off = params[2];
            ULONG value = params[3];

            if (devExt->PciCfgBase != NULL) {
                ULONG addr = (bus * 0x100000) + (devFn * 0x1000) + (off & ~3);
                if (addr + sizeof(ULONG) <= devExt->PciCfgSize) {
                    WRITE_REGISTER_ULONG((PULONG)((PUCHAR)devExt->PciCfgBase + addr), value);
                }
            } else {
                ULONG slot = (devFn >> 3) & 0x1F;
                ULONG func = devFn & 7;
                HalSetBusDataByOffset(PCIConfiguration, bus, (slot << 3) | func, &value, off & ~3, sizeof(ULONG));
            }
            KdPrint(("PCI_WRITE: B%d.D%d.F%d off=0x%X <= 0x%08X\n", bus, (devFn>>3)&0x1F, devFn&7, off, value));
            break;
        }

        case IOCTL_PSP_NBIO_UNLOCK:
        {
            ULONG beforeSig1 = READ_REGISTER_ULONG((PULONG)((PUCHAR)devExt->MmioBase + NBIO_SIG1_OFFSET));
            ULONG beforeSig2 = READ_REGISTER_ULONG((PULONG)((PUCHAR)devExt->MmioBase + NBIO_SIG2_OFFSET));
            ULONG mmhubBefore = READ_REGISTER_ULONG((PULONG)((PUCHAR)devExt->MmioBase + MMHUB_CHECK_OFFSET));

            KdPrint(("NBIO unlock: SIG1=0x%08X SIG2=0x%08X MMHUB=0x%08X\n",
                beforeSig1, beforeSig2, mmhubBefore));

            WRITE_REGISTER_ULONG((PULONG)((PUCHAR)devExt->MmioBase + NBIO_SIG1_OFFSET), NBIO_SIG1_VALUE);
            WRITE_REGISTER_ULONG((PULONG)((PUCHAR)devExt->MmioBase + NBIO_SIG2_OFFSET), NBIO_SIG2_VALUE);

            KeStallExecutionProcessor(1000);

            ULONG mmhubAfter = READ_REGISTER_ULONG((PULONG)((PUCHAR)devExt->MmioBase + MMHUB_CHECK_OFFSET));

            KdPrint(("NBIO unlock: MMHUB after=0x%08X\n", mmhubAfter));

            if (outputLength >= sizeof(ULONG) * 3) {
                ((PULONG)outputBuffer)[0] = NBIO_SIG1_VALUE;
                ((PULONG)outputBuffer)[1] = NBIO_SIG2_VALUE;
                ((PULONG)outputBuffer)[2] = mmhubAfter;
                bytesReturned = sizeof(ULONG) * 3;
            }

            status = (mmhubAfter != mmhubBefore) ? STATUS_SUCCESS : STATUS_UNSUCCESSFUL;
            break;
        }

        case IOCTL_PSP_CREATE_RING:
        {
            /* Use global ring buffer (always available, no allocation needed) */
            devExt->RingBuffer = g_RingBuffer;
            /* Initialize physical address if not already done */
            if (!g_RingBufferInitialized || g_RingBufferPhysical.QuadPart == 0) {
                g_RingBufferPhysical = MmGetPhysicalAddress(g_RingBuffer);
                g_RingBufferInitialized = TRUE;
                KdPrint(("CREATE_RING: Ring buffer PA initialized to 0x%llX\n",
                    g_RingBufferPhysical.QuadPart));
            }
            devExt->RingPhysical = g_RingBufferPhysical;
            devExt->RingSize = sizeof(g_RingBuffer);
            
            if (devExt->RingCreated) {
                KdPrint(("CREATE_RING: Ring already created, reinitializing...\n"));
            }
            RtlZeroMemory(devExt->RingBuffer, devExt->RingSize);

            KdPrint(("CREATE_RING: Ring at VA=%p PA=0x%llX size=%u\n",
                devExt->RingBuffer, devExt->RingPhysical.QuadPart, devExt->RingSize));

            PVOID mboxBase = devExt->Bar0Base ? devExt->Bar0Base : devExt->MmioBase;
            ULONG c64_before = READ_REGISTER_ULONG((PULONG)((PUCHAR)mboxBase + PSP_C2PMSG_64_OFFSET));

            WRITE_REGISTER_ULONG((PULONG)((PUCHAR)mboxBase + PSP_C2PMSG_69_OFFSET),
                (ULONG)(devExt->RingPhysical.QuadPart & 0xFFFFFFFF));
            WRITE_REGISTER_ULONG((PULONG)((PUCHAR)mboxBase + PSP_C2PMSG_70_OFFSET),
                (ULONG)(devExt->RingPhysical.QuadPart >> 32));
            WRITE_REGISTER_ULONG((PULONG)((PUCHAR)mboxBase + PSP_C2PMSG_71_OFFSET), devExt->RingSize);
            WRITE_REGISTER_ULONG((PULONG)((PUCHAR)mboxBase + PSP_C2PMSG_64_OFFSET), 0x00010000);

            ULONG ringResp;
            ULONG respTimeout;
            for (respTimeout = 0; respTimeout < 3000; respTimeout++) {
                KeStallExecutionProcessor(1000);
                ringResp = READ_REGISTER_ULONG((PULONG)((PUCHAR)mboxBase + PSP_C2PMSG_64_OFFSET));
                if (ringResp & 0x80000000) break;
            }

            if (respTimeout >= 3000) {
                KdPrint(("CREATE_RING: Ring create timeout (C2PMSG_64=0x%08X)\n", ringResp));
                status = STATUS_DEVICE_NOT_READY;
                break;
            }

            devExt->RingCreated = TRUE;

            KdPrint(("CREATE_RING: C2PMSG_64 before=0x%08X after=0x%08X respWait=%ums ringCreated=1\n",
                c64_before, ringResp, respTimeout));

            if (outputLength >= sizeof(ULONG) * 3) {
                ((PULONG)outputBuffer)[0] = (ULONG)(devExt->RingPhysical.QuadPart & 0xFFFFFFFF);
                ((PULONG)outputBuffer)[1] = c64_before;
                ((PULONG)outputBuffer)[2] = ringResp;
                bytesReturned = sizeof(ULONG) * 3;
            }
            status = STATUS_SUCCESS;
            break;
        }

        case IOCTL_PSP_NBIO_VIA_RING:
        {
            ULONG results[4] = {0};
            PVOID mboxBase = devExt->Bar0Base ? devExt->Bar0Base : devExt->MmioBase;

            ULONG c64_before = READ_REGISTER_ULONG((PULONG)((PUCHAR)mboxBase + PSP_C2PMSG_64_OFFSET));
            WRITE_REGISTER_ULONG((PULONG)((PUCHAR)mboxBase + PSP_C2PMSG_64_OFFSET), 0x00020000);
            results[0] = 0x00020000;

            KeStallExecutionProcessor(500000);

            ULONG c64resp = READ_REGISTER_ULONG((PULONG)((PUCHAR)mboxBase + PSP_C2PMSG_64_OFFSET));
            results[1] = c64resp;

            WRITE_REGISTER_ULONG((PULONG)((PUCHAR)devExt->MmioBase + NBIO_SIG1_OFFSET), NBIO_SIG1_VALUE);
            WRITE_REGISTER_ULONG((PULONG)((PUCHAR)devExt->MmioBase + NBIO_SIG2_OFFSET), NBIO_SIG2_VALUE);

            KeStallExecutionProcessor(100000);

            ULONG sig1 = READ_REGISTER_ULONG((PULONG)((PUCHAR)devExt->MmioBase + NBIO_SIG1_OFFSET));
            ULONG sig2 = READ_REGISTER_ULONG((PULONG)((PUCHAR)devExt->MmioBase + NBIO_SIG2_OFFSET));
            ULONG mmhub = READ_REGISTER_ULONG((PULONG)((PUCHAR)devExt->MmioBase + MMHUB_CHECK_OFFSET));
            ULONG grbm = READ_REGISTER_ULONG((PULONG)((PUCHAR)devExt->MmioBase + (AMDBC250_GC_BASE + 0x2004)));
            results[2] = mmhub;
            results[3] = grbm;

            KdPrint(("NBIO_VIA_RING: C64pre=0x%08X cmd=0x%08X C64post=0x%08X "
                "SIG1=0x%08X SIG2=0x%08X MMHUB=0x%08X GRBM=0x%08X\n",
                c64_before, 0x00020000, c64resp, sig1, sig2, mmhub, grbm));

            if (outputLength >= sizeof(results)) {
                RtlCopyMemory(outputBuffer, results, sizeof(results));
                bytesReturned = sizeof(results);
            }
            status = (grbm != 0xFFFFFFFF) ? STATUS_SUCCESS : STATUS_UNSUCCESSFUL;
            break;
        }

        case IOCTL_PSP_GET_STATUS:
        {
            if (outputLength < sizeof(PSP_STATUS_INFO)) {
                status = STATUS_BUFFER_TOO_SMALL;
                break;
            }

PSP_STATUS_INFO* info = (PSP_STATUS_INFO*)outputBuffer;
            RtlZeroMemory(info, sizeof(PSP_STATUS_INFO));

            PVOID mboxBase = devExt->Bar0Base ? devExt->Bar0Base : devExt->MmioBase;
            info->C2PMSG_81 = READ_REGISTER_ULONG((PULONG)((PUCHAR)mboxBase + PSP_C2PMSG_81_OFFSET));
            info->C2PMSG_35 = READ_REGISTER_ULONG((PULONG)((PUCHAR)mboxBase + PSP_C2PMSG_35_OFFSET));
            info->C2PMSG_36 = READ_REGISTER_ULONG((PULONG)((PUCHAR)mboxBase + PSP_C2PMSG_36_OFFSET));
            info->C2PMSG_37 = READ_REGISTER_ULONG((PULONG)((PUCHAR)mboxBase + PSP_C2PMSG_37_OFFSET));
            info->C2PMSG_64 = READ_REGISTER_ULONG((PULONG)((PUCHAR)mboxBase + PSP_C2PMSG_64_OFFSET));
            info->PspAlive = (info->C2PMSG_81 != 0 && info->C2PMSG_81 != 0xFFFFFFFF) ? 1 : 0;

            // Firmware info
            info->FwLoaded = (devExt->FwBuffer != NULL) ? 1 : 0;
            info->FwSize = devExt->FwSize;
            info->FwPaShifted = devExt->FwPaShifted;

            // NBIO status
            info->NbioSig1 = READ_REGISTER_ULONG((PULONG)((PUCHAR)devExt->MmioBase + NBIO_SIG1_OFFSET));
            info->NbioSig2 = READ_REGISTER_ULONG((PULONG)((PUCHAR)devExt->MmioBase + NBIO_SIG2_OFFSET));
            info->GrbmStatus = READ_REGISTER_ULONG((PULONG)((PUCHAR)devExt->MmioBase + (AMDBC250_GC_BASE + 0x2004)));
            info->MmhubCheck = READ_REGISTER_ULONG((PULONG)((PUCHAR)devExt->MmioBase + MMHUB_CHECK_OFFSET));
            info->GcCheck = READ_REGISTER_ULONG((PULONG)((PUCHAR)devExt->MmioBase + (AMDBC250_GC_BASE + 0x3000)));
            info->HdpCheck = READ_REGISTER_ULONG((PULONG)((PUCHAR)devExt->MmioBase + 0x05A0));

            // MMIO and ring info
            info->MmioVA = (ULONG)(ULONG_PTR)devExt->MmioBase;
            info->MmioSize = devExt->MmioSize;
            info->RingCreated = devExt->RingCreated ? 1 : 0;

            bytesReturned = sizeof(PSP_STATUS_INFO);
            status = STATUS_SUCCESS;
            break;
        }

        case IOCTL_PSP_LOAD_EMBEDDED_FW:
        {
            if (outputLength < sizeof(ULONG)) {
                status = STATUS_BUFFER_TOO_SMALL;
                break;
            }

            PspFreeFirmware(devExt);

            // Validate embedded firmware size before allocation
            if (g_SosFirmwareSize > PSP_MAX_FW_TOTAL) {
                KdPrint(("IOCTL_PSP_LOAD_EMBEDDED_FW: Embedded FW too large (%u > %u)\n", 
                    g_SosFirmwareSize, PSP_MAX_FW_TOTAL));
                status = STATUS_INVALID_PARAMETER;
                break;
            }

            PHYSICAL_ADDRESS highAddr;
            highAddr.QuadPart = 0x10000000000ULL;  // Allow above 4GB

            devExt->FwBuffer = MmAllocateContiguousMemory(g_SosFirmwareSize, highAddr);
            if (devExt->FwBuffer == NULL) {
                highAddr.QuadPart = 0xFFFFFFFF;
                devExt->FwBuffer = MmAllocateContiguousMemory(g_SosFirmwareSize, highAddr);
            }
            if (devExt->FwBuffer == NULL) {
                break;
            }

            RtlCopyMemory(devExt->FwBuffer, (PVOID)g_SosFirmwareData, g_SosFirmwareSize);
            devExt->FwSize = g_SosFirmwareSize;
            devExt->FwPhysical = MmGetPhysicalAddress(devExt->FwBuffer);
            devExt->FwPaShifted = (ULONG)(devExt->FwPhysical.QuadPart >> 20);

            if (!PspValidateFirmware((PUCHAR)devExt->FwBuffer, devExt->FwSize)) {
                KdPrint(("IOCTL_PSP_LOAD_EMBEDDED_FW: Validation FAILED\n"));
                PspFreeFirmware(devExt);
                status = STATUS_IMAGE_CHECKSUM_MISMATCH;
                break;
            }

            KdPrint(("IOCTL_PSP_LOAD_EMBEDDED_FW: Embedded FW loaded PA=0x%llX PA>>20=0x%08X size=%u\n",
                devExt->FwPhysical.QuadPart, devExt->FwPaShifted, devExt->FwSize));

            ((PULONG)outputBuffer)[0] = devExt->FwPaShifted;
            bytesReturned = sizeof(ULONG);
            status = STATUS_SUCCESS;
            break;
        }

        case IOCTL_PSP_BOOT_SEQUENCE:
        {
            NTSTATUS stepStatus;
            ULONG results[4] = {0};
            PHYSICAL_ADDRESS highAddr;
            highAddr.QuadPart = 0x10000000000ULL;  // Allow above 4GB

            // FIX #5: Validate SYSDRV firmware size before allocation
            if (g_SysdrvFirmwareSize > PSP_MAX_FW_TOTAL) {
                KdPrint(("BOOT_SEQ: SYSDRV FW too large (%u > %u)\n", g_SysdrvFirmwareSize, PSP_MAX_FW_TOTAL));
                status = STATUS_INVALID_PARAMETER;
                break;
            }

            // Step 1: Load SYSDRV firmware (type 8, 256KB from BIOS 0x8FEE00) -> send command 0x4
            PspFreeFirmware(devExt);
            devExt->FwBuffer = MmAllocateContiguousMemory(g_SysdrvFirmwareSize, highAddr);
            if (devExt->FwBuffer == NULL) {
                highAddr.QuadPart = 0xFFFFFFFF;
                devExt->FwBuffer = MmAllocateContiguousMemory(g_SysdrvFirmwareSize, highAddr);
            }
            if (devExt->FwBuffer == NULL) {
                KdPrint(("BOOT_SEQ: SYSDRV alloc failed\n"));
                status = STATUS_INSUFFICIENT_RESOURCES;
                break;
            }
            RtlCopyMemory(devExt->FwBuffer, (PVOID)g_SysdrvFirmwareData, g_SysdrvFirmwareSize);
            devExt->FwSize = g_SysdrvFirmwareSize;
            devExt->FwPhysical = MmGetPhysicalAddress(devExt->FwBuffer);
            devExt->FwPaShifted = (ULONG)(devExt->FwPhysical.QuadPart >> 20);
            results[0] = devExt->FwPaShifted;

            KdPrint(("BOOT_SEQ: SYSDRV loaded PA=0x%llX PA>>20=0x%08X\n",
                devExt->FwPhysical.QuadPart, devExt->FwPaShifted));

            // Send SYSDRV command (0x4) to PSP
            stepStatus = PspSendMailboxCommand(devExt, 0x00000004);
            results[1] = NT_SUCCESS(stepStatus) ? 1 : 0;
            KdPrint(("BOOT_SEQ: SYSDRV cmd=0x4 => %s\n", results[1] ? "SENT" : "FAIL"));

            // FIX #10: Check SYSDRV status before proceeding to SOS
            if (!NT_SUCCESS(stepStatus)) {
                KdPrint(("BOOT_SEQ: SYSDRV failed with status 0x%08X, skipping SOS\n", stepStatus));
                status = stepStatus;
                results[2] = 0;
                results[3] = 0;
                if (outputLength >= sizeof(results)) {
                    RtlCopyMemory(outputBuffer, results, sizeof(results));
                    bytesReturned = sizeof(results);
                }
                break;
            }

            // Step 2: Load SOS firmware (type 1, 46KB, padded to 256KB) -> send command 0x8
            if (g_SosFirmwareSize > PSP_MAX_FW_TOTAL) {
                KdPrint(("BOOT_SEQ: SOS FW too large (%u > %u)\n", g_SosFirmwareSize, PSP_MAX_FW_TOTAL));
                status = STATUS_INVALID_PARAMETER;
                break;
            }

            // Free SYSDRV, allocate new buffer for SOS
            PspFreeFirmware(devExt);
            devExt->FwBuffer = MmAllocateContiguousMemory(262144, highAddr);
            if (devExt->FwBuffer == NULL) {
                highAddr.QuadPart = 0xFFFFFFFF;
                devExt->FwBuffer = MmAllocateContiguousMemory(262144, highAddr);
            }
            if (devExt->FwBuffer == NULL) {
                KdPrint(("BOOT_SEQ: SOS alloc failed\n"));
                status = STATUS_INSUFFICIENT_RESOURCES;
                break;
            }
            RtlZeroMemory(devExt->FwBuffer, 262144);
            RtlCopyMemory(devExt->FwBuffer, (PVOID)g_SosFirmwareData, g_SosFirmwareSize);
            devExt->FwSize = 262144;
            devExt->FwPhysical = MmGetPhysicalAddress(devExt->FwBuffer);
            devExt->FwPaShifted = (ULONG)(devExt->FwPhysical.QuadPart >> 20);

            KdPrint(("BOOT_SEQ: SOS loaded PA=0x%llX PA>>20=0x%08X\n",
                devExt->FwPhysical.QuadPart, devExt->FwPaShifted));

            // Send SOS command (0x8) to PSP
            stepStatus = PspSendMailboxCommand(devExt, 0x00000008);
            results[2] = NT_SUCCESS(stepStatus) ? 1 : 0;
            KdPrint(("BOOT_SEQ: SOS cmd=0x8 => %s\n", results[2] ? "SENT" : "FAIL"));

            // Step 4: Read GRBM
            results[3] = READ_REGISTER_ULONG((PULONG)((PUCHAR)devExt->MmioBase + (AMDBC250_GC_BASE + 0x2004)));

            KdPrint(("BOOT_SEQ: FW=%d SYSDRV=%d SOS=%d GRBM=0x%08X\n",
                results[0] != 0, results[1], results[2], results[3]));

            if (outputLength >= sizeof(results)) {
                RtlCopyMemory(outputBuffer, results, sizeof(results));
                bytesReturned = sizeof(results);
            }
            // Return real status from failing step (if any)
            status = stepStatus;
            break;
        }

        case IOCTL_PSP_PROBE:
        {
            if (outputLength < sizeof(PSP_PROBE_INFO)) {
                status = STATUS_BUFFER_TOO_SMALL;
                break;
            }

            PSP_PROBE_INFO* probe = (PSP_PROBE_INFO*)outputBuffer;
            RtlZeroMemory(probe, sizeof(PSP_PROBE_INFO));

            PVOID mboxBase = devExt->Bar0Base ? devExt->Bar0Base : devExt->MmioBase;
            probe->C2PMSG_35 = READ_REGISTER_ULONG((PULONG)((PUCHAR)mboxBase + PSP_C2PMSG_35_OFFSET));
            probe->C2PMSG_36 = READ_REGISTER_ULONG((PULONG)((PUCHAR)mboxBase + PSP_C2PMSG_36_OFFSET));
            probe->C2PMSG_37 = READ_REGISTER_ULONG((PULONG)((PUCHAR)mboxBase + PSP_C2PMSG_37_OFFSET));
            probe->C2PMSG_64 = READ_REGISTER_ULONG((PULONG)((PUCHAR)mboxBase + PSP_C2PMSG_64_OFFSET));
            probe->C2PMSG_81 = READ_REGISTER_ULONG((PULONG)((PUCHAR)mboxBase + PSP_C2PMSG_81_OFFSET));

            probe->NbioSig1 = READ_REGISTER_ULONG((PULONG)((PUCHAR)devExt->MmioBase + NBIO_SIG1_OFFSET));
            probe->NbioSig2 = READ_REGISTER_ULONG((PULONG)((PUCHAR)devExt->MmioBase + NBIO_SIG2_OFFSET));
            probe->MmhubCheck = READ_REGISTER_ULONG((PULONG)((PUCHAR)devExt->MmioBase + MMHUB_CHECK_OFFSET));
            probe->GrbmStatus = READ_REGISTER_ULONG((PULONG)((PUCHAR)devExt->MmioBase + (AMDBC250_GC_BASE + 0x2004)));
            probe->GcCheck = READ_REGISTER_ULONG((PULONG)((PUCHAR)devExt->MmioBase + (AMDBC250_GC_BASE + 0x3000)));
            probe->HdpCheck = READ_REGISTER_ULONG((PULONG)((PUCHAR)devExt->MmioBase + 0x05A0));

            WRITE_REGISTER_ULONG((PULONG)((PUCHAR)devExt->MmioBase + NBIO_SIG1_OFFSET), NBIO_SIG1_VALUE);
            WRITE_REGISTER_ULONG((PULONG)((PUCHAR)devExt->MmioBase + NBIO_SIG2_OFFSET), NBIO_SIG2_VALUE);
            KeStallExecutionProcessor(1000);
            ULONG sig1a = READ_REGISTER_ULONG((PULONG)((PUCHAR)devExt->MmioBase + NBIO_SIG1_OFFSET));
            ULONG sig2a = READ_REGISTER_ULONG((PULONG)((PUCHAR)devExt->MmioBase + NBIO_SIG2_OFFSET));
            probe->SigWriteOk = ((sig1a == NBIO_SIG1_VALUE) && (sig2a == NBIO_SIG2_VALUE)) ? 1 : 0;

            probe->RingAddrLow = (ULONG)(g_RingBufferPhysical.QuadPart & 0xFFFFFFFF);
            probe->RingAddrHigh = (ULONG)(g_RingBufferPhysical.QuadPart >> 32);
            probe->RingSize = 0x1000;
            WRITE_REGISTER_ULONG((PULONG)((PUCHAR)mboxBase + PSP_C2PMSG_69_OFFSET), probe->RingAddrLow);
            WRITE_REGISTER_ULONG((PULONG)((PUCHAR)mboxBase + PSP_C2PMSG_70_OFFSET), probe->RingAddrHigh);
            WRITE_REGISTER_ULONG((PULONG)((PUCHAR)mboxBase + PSP_C2PMSG_71_OFFSET), probe->RingSize);
            KeStallExecutionProcessor(1000);
            ULONG rl = READ_REGISTER_ULONG((PULONG)((PUCHAR)mboxBase + PSP_C2PMSG_69_OFFSET));
            ULONG rh = READ_REGISTER_ULONG((PULONG)((PUCHAR)mboxBase + PSP_C2PMSG_70_OFFSET));
            ULONG rs = READ_REGISTER_ULONG((PULONG)((PUCHAR)mboxBase + PSP_C2PMSG_71_OFFSET));
            probe->RingProgOk = ((rl == probe->RingAddrLow) && (rs == probe->RingSize)) ? 1 : 0;
            probe->RingCreated = devExt->RingCreated ? 1 : 0;

            WRITE_REGISTER_ULONG((PULONG)((PUCHAR)mboxBase + PSP_C2PMSG_64_OFFSET), 0x00020000);
            KeStallExecutionProcessor(500000);
            ULONG c64r = READ_REGISTER_ULONG((PULONG)((PUCHAR)mboxBase + PSP_C2PMSG_64_OFFSET));
            ULONG grbm2 = READ_REGISTER_ULONG((PULONG)((PUCHAR)devExt->MmioBase + (AMDBC250_GC_BASE + 0x2004)));
            probe->NbioViaRingOk = (grbm2 != 0xFFFFFFFF) ? 1 : 0;

            KdPrint(("PROBE: mailbox=0x%08X/0x%08X/0x%08X/0x%08X/0x%08X "
                "sig=0x%08X/0x%08X mmhub=0x%08X grbm=0x%08X gc=0x%08X hdp=0x%08X "
                "sigOk=%d ringProg=%d nbioVia=%d\n",
                probe->C2PMSG_35, probe->C2PMSG_36, probe->C2PMSG_37,
                probe->C2PMSG_64, probe->C2PMSG_81,
                probe->NbioSig1, probe->NbioSig2, probe->MmhubCheck,
                probe->GrbmStatus, probe->GcCheck, probe->HdpCheck,
                probe->SigWriteOk, probe->RingProgOk, probe->NbioViaRingOk));

            bytesReturned = sizeof(PSP_PROBE_INFO);
            status = STATUS_SUCCESS;
            break;
        }

        case IOCTL_PSP_RING_LOAD_IP_FW:
        {
            if (inputLength < sizeof(PSP_RING_FW_REQUEST)) { status = STATUS_INVALID_PARAMETER; break; }
            if (!devExt->RingCreated) { status = STATUS_DEVICE_NOT_READY; break; }
            PSP_RING_FW_REQUEST* req = (PSP_RING_FW_REQUEST*)inputBuffer;
            ULONG fwSize = req->FwSize;
            ULONG fwType = req->FwType;
            ULONG totalInput = sizeof(PSP_RING_FW_REQUEST) + fwSize;
            if (inputLength < totalInput || fwSize == 0 || fwSize > PSP_MAX_FW_TOTAL) { status = STATUS_INVALID_PARAMETER; break; }
            PUCHAR fwData = (PUCHAR)inputBuffer + sizeof(PSP_RING_FW_REQUEST);

            PHYSICAL_ADDRESS highAddr;
            highAddr.QuadPart = 0x10000000000ULL;
            PVOID fwMem = MmAllocateContiguousMemory(fwSize, highAddr);
            if (fwMem == NULL) { highAddr.QuadPart = 0xFFFFFFFF; fwMem = MmAllocateContiguousMemory(fwSize, highAddr); }
            if (fwMem == NULL) { status = STATUS_INSUFFICIENT_RESOURCES; break; }
            RtlCopyMemory(fwMem, fwData, fwSize);
            PHYSICAL_ADDRESS fwPa = MmGetPhysicalAddress(fwMem);

            // Build command buffer (per-call alloc — no race with other ring ops)
            PVOID cmdBuffer = ExAllocatePool2(POOL_FLAG_NON_PAGED, PSP_CMD_BUF_SIZE, 'mCSP');
            if (!cmdBuffer) { MmFreeContiguousMemory(fwMem); status = STATUS_INSUFFICIENT_RESOURCES; break; }
            RtlZeroMemory(cmdBuffer, PSP_CMD_BUF_SIZE);
            PULONG cmd = (PULONG)cmdBuffer;
            cmd[0] = PSP_CMD_BUF_SIZE;
            cmd[1] = 1;
            cmd[2] = GFX_CMD_ID_LOAD_IP_FW;
            cmd[7] = (ULONG)(fwPa.QuadPart & 0xFFFFFFFF);
            cmd[8] = (ULONG)(fwPa.QuadPart >> 32);
            cmd[9] = fwSize;
            cmd[10] = fwType;
            PHYSICAL_ADDRESS cmdPa = MmGetPhysicalAddress(cmdBuffer);

            KIRQL ringIrql;
            KeAcquireSpinLock(&devExt->CommandLock, &ringIrql);
            if (ringWriteOffset + PSP_RING_FRAME_SIZE > sizeof(g_RingBuffer)) ringWriteOffset = 0;
            PULONG frame = (PULONG)(g_RingBuffer + ringWriteOffset);
            RtlZeroMemory(frame, PSP_RING_FRAME_SIZE);
            frame[0] = (ULONG)(cmdPa.QuadPart & 0xFFFFFFFF);
            frame[1] = (ULONG)(cmdPa.QuadPart >> 32);
            frame[2] = PSP_CMD_BUF_SIZE;
            ringWriteOffset += PSP_RING_FRAME_SIZE;

            KdPrint(("RING: type=%u size=%u fwPA=0x%llX cmdPA=0x%llX\n", fwType, fwSize, fwPa.QuadPart, cmdPa.QuadPart));

            PVOID mboxBase = devExt->Bar0Base ? devExt->Bar0Base : devExt->MmioBase;
            WRITE_REGISTER_ULONG((PULONG)((PUCHAR)mboxBase + PSP_C2PMSG_67_OFFSET), ringWriteOffset);
            KeReleaseSpinLock(&devExt->CommandLock, ringIrql);

            ULONG timeout, resp = 0;
            for (timeout = 0; timeout < 1000; timeout++) {
                KeStallExecutionProcessor(1000);
                resp = READ_REGISTER_ULONG((PULONG)((PUCHAR)mboxBase + PSP_C2PMSG_64_OFFSET));
                if (resp & 0x80000000) {
                    ULONG st = resp & 0x0000FFFF;
                    ULONG cbStatus = ((PULONG)cmdBuffer)[864/sizeof(ULONG)];
                    status = (st == 0 && cbStatus == 0) ? STATUS_SUCCESS : STATUS_UNSUCCESSFUL;
                    if (NT_SUCCESS(status)) g_RingFwCount++;
                    KdPrint(("RING: C2PMSG_64=0x%08X cmdResp=0x%08X\n", resp, cbStatus));
                    break;
                }
            }
            if (timeout >= 1000) { KdPrint(("RING: timeout\n")); status = STATUS_TIMEOUT; }

            ExFreePool(cmdBuffer);
            MmFreeContiguousMemory(fwMem);

            if (outputLength >= sizeof(ULONG)) { ((PULONG)outputBuffer)[0] = (ULONG)(fwPa.QuadPart >> 20); bytesReturned = sizeof(ULONG); }
            break;
        }

        case IOCTL_PSP_GET_GPU_INFO:
        {
            if (outputLength < sizeof(PSP_GPU_INFO)) {
                status = STATUS_BUFFER_TOO_SMALL; break;
            }
            PSP_GPU_INFO* info = (PSP_GPU_INFO*)outputBuffer;
            RtlZeroMemory(info, sizeof(PSP_GPU_INFO));

            PVOID mboxBase = devExt->Bar0Base ? devExt->Bar0Base : devExt->MmioBase;
            info->RingBufferPA = (ULONG)(g_RingBufferPhysical.QuadPart & 0xFFFFFFFF);
            info->FwLoaded = devExt->FwBuffer ? 1 : 0;
            info->FwCount = g_RingFwCount;
            info->TMRBase = g_TmrInitialized ? g_TmrPhysical.QuadPart : 0xF40F800000;
            info->TMSSize = g_TmrSize ? g_TmrSize : 0x400000;
            info->GfxVersion = 10;

            info->C2pmsg64 = READ_REGISTER_ULONG(
                (PULONG)((PUCHAR)mboxBase + PSP_C2PMSG_64_OFFSET));
            info->C2pmsg81 = READ_REGISTER_ULONG(
                (PULONG)((PUCHAR)mboxBase + PSP_C2PMSG_81_OFFSET));
            info->TmrInitialized = g_TmrInitialized ? 1 : 0;

            bytesReturned = sizeof(PSP_GPU_INFO);
            status = STATUS_SUCCESS;
            break;
        }

        case IOCTL_PSP_REG_PROG:
        {
            if (inputLength < sizeof(PSP_REG_PROG_REQUEST)) {
                status = STATUS_INVALID_PARAMETER; break;
            }

            PSP_REG_PROG_REQUEST* req = (PSP_REG_PROG_REQUEST*)inputBuffer;
            ULONG regId = req->RegId;
            ULONG regVal = req->RegValue;

            /* Check for IsRead flag: when inputLength >= 12 and 3rd ULONG == 1 */
            BOOLEAN isRead = (inputLength >= 3 * sizeof(ULONG) &&
                             ((PULONG)inputBuffer)[2] == 1);

            if (isRead) {
                PVOID mboxBase = devExt->Bar0Base ? devExt->Bar0Base : devExt->MmioBase;
                if (regId >= devExt->MmioSize || (regId + sizeof(ULONG) > devExt->MmioSize) || (regId & 0x3)) {
                    if (regId >= devExt->Bar0Size || (regId + sizeof(ULONG) > devExt->Bar0Size) || (regId & 0x3)) {
                        status = STATUS_ARRAY_BOUNDS_EXCEEDED; break;
                    }
                    ULONG value = READ_REGISTER_ULONG((PULONG)((PUCHAR)mboxBase + regId));
                    if (outputLength >= sizeof(ULONG)) {
                        ((PULONG)outputBuffer)[0] = value;
                        bytesReturned = sizeof(ULONG);
                    }
                    KdPrint(("REG_PROG: read reg 0x%04X = 0x%08X\n", regId, value));
                    status = STATUS_SUCCESS;
                    break;
                }
                ULONG value = READ_REGISTER_ULONG((PULONG)((PUCHAR)devExt->MmioBase + regId));
                if (outputLength >= sizeof(ULONG)) {
                    ((PULONG)outputBuffer)[0] = value;
                    bytesReturned = sizeof(ULONG);
                }
                KdPrint(("REG_PROG: read reg 0x%04X = 0x%08X\n", regId, value));
                status = STATUS_SUCCESS;
                break;
            }

            if (!devExt->RingCreated) {
                PVOID mboxBase = devExt->Bar0Base ? devExt->Bar0Base : devExt->MmioBase;
                KdPrint(("REG_PROG: ring not available, trying mailbox PROG_REG (reg=0x%04X val=0x%08X)\n",
                    regId, regVal));
                KIRQL mbIrql;
                KeAcquireSpinLock(&devExt->CommandLock, &mbIrql);
                WRITE_REGISTER_ULONG((PULONG)((PUCHAR)mboxBase + PSP_C2PMSG_36_OFFSET), regId);
                WRITE_REGISTER_ULONG((PULONG)((PUCHAR)mboxBase + PSP_C2PMSG_37_OFFSET), regVal);
                WRITE_REGISTER_ULONG((PULONG)((PUCHAR)mboxBase + PSP_C2PMSG_35_OFFSET), 0x0000000B);
                KeReleaseSpinLock(&devExt->CommandLock, mbIrql);
                ULONG mbTimeout;
                for (mbTimeout = 0; mbTimeout < 500; mbTimeout++) {
                    KeStallExecutionProcessor(1000);
                    ULONG cmdReg = READ_REGISTER_ULONG((PULONG)((PUCHAR)mboxBase + PSP_C2PMSG_35_OFFSET));
                    if (cmdReg == 0) {
                        KdPrint(("REG_PROG: mailbox PROG_REG completed after %u ms\n", mbTimeout));
                        status = STATUS_SUCCESS;
                        break;
                    }
                }
                if (mbTimeout >= 500) {
                    KdPrint(("REG_PROG: mailbox PROG_REG timeout\n"));
                    status = STATUS_TIMEOUT;
                }
                if (outputLength >= sizeof(ULONG)) {
                    ((PULONG)outputBuffer)[0] = regVal;
                    bytesReturned = sizeof(ULONG);
                }
                break;
            }

            /* Write path: GPCOM PROG_REG command via ring (bypasses NBIO) */
            PVOID cmdBuffer = ExAllocatePool2(POOL_FLAG_NON_PAGED, PSP_CMD_BUF_SIZE, 'mCSP');
            if (!cmdBuffer) { status = STATUS_INSUFFICIENT_RESOURCES; break; }
            RtlZeroMemory(cmdBuffer, PSP_CMD_BUF_SIZE);
            PULONG cmd = (PULONG)cmdBuffer;
            cmd[0] = PSP_CMD_BUF_SIZE;
            cmd[1] = 1;
            cmd[2] = 0x0000000B;
            cmd[7] = regVal;
            cmd[8] = regId;
            PHYSICAL_ADDRESS cmdPa = MmGetPhysicalAddress(cmdBuffer);

            KIRQL ringIrql;
            KeAcquireSpinLock(&devExt->CommandLock, &ringIrql);
            if (ringWriteOffset + PSP_RING_FRAME_SIZE > sizeof(g_RingBuffer))
                ringWriteOffset = 0;
            PULONG frame = (PULONG)(g_RingBuffer + ringWriteOffset);
            RtlZeroMemory(frame, PSP_RING_FRAME_SIZE);
            frame[0] = (ULONG)(cmdPa.QuadPart & 0xFFFFFFFF);
            frame[1] = (ULONG)(cmdPa.QuadPart >> 32);
            frame[2] = PSP_CMD_BUF_SIZE;
            ringWriteOffset += PSP_RING_FRAME_SIZE;

            KdPrint(("REG_PROG: write id=%u val=0x%08X\n", regId, regVal));

            PVOID mboxBase = devExt->Bar0Base ? devExt->Bar0Base : devExt->MmioBase;
            WRITE_REGISTER_ULONG(
                (PULONG)((PUCHAR)mboxBase + PSP_C2PMSG_67_OFFSET), ringWriteOffset);
            KeReleaseSpinLock(&devExt->CommandLock, ringIrql);

            ULONG timeout, resp = 0;
            for (timeout = 0; timeout < 1000; timeout++) {
                KeStallExecutionProcessor(1000);
                resp = READ_REGISTER_ULONG(
                    (PULONG)((PUCHAR)mboxBase + PSP_C2PMSG_64_OFFSET));
                if (resp & 0x80000000) {
                    ULONG st = resp & 0x0000FFFF;
                    ULONG cbStatus = ((PULONG)cmdBuffer)[864/sizeof(ULONG)];
                    status = (st == 0 && cbStatus == 0) ? STATUS_SUCCESS : STATUS_UNSUCCESSFUL;
                    KdPrint(("REG_PROG: C2PMSG_64=0x%08X cmdResp=0x%08X\n", resp, cbStatus));
                    break;
                }
            }
            if (timeout >= 1000) { status = STATUS_TIMEOUT; }

            ExFreePool(cmdBuffer);
            if (outputLength >= sizeof(ULONG)) {
                ((PULONG)outputBuffer)[0] = regVal;
                bytesReturned = sizeof(ULONG);
            }
            break;
        }

        case IOCTL_PSP_AUTOLOAD_RLC:
        {
            if (!devExt->RingCreated) { status = STATUS_DEVICE_NOT_READY; break; }

            // Build command buffer (per-call alloc — no race)
            PVOID cmdBuffer = ExAllocatePool2(POOL_FLAG_NON_PAGED, PSP_CMD_BUF_SIZE, 'mCSP');
            if (!cmdBuffer) { status = STATUS_INSUFFICIENT_RESOURCES; break; }
            RtlZeroMemory(cmdBuffer, PSP_CMD_BUF_SIZE);
            PULONG cmd = (PULONG)cmdBuffer;
            cmd[0] = PSP_CMD_BUF_SIZE;
            cmd[1] = 1;
            cmd[2] = 0x00000021;
            PHYSICAL_ADDRESS cmdPa = MmGetPhysicalAddress(cmdBuffer);

            KIRQL ringIrql;
            KeAcquireSpinLock(&devExt->CommandLock, &ringIrql);
            if (ringWriteOffset + PSP_RING_FRAME_SIZE > sizeof(g_RingBuffer))
                ringWriteOffset = 0;
            PULONG frame = (PULONG)(g_RingBuffer + ringWriteOffset);
            RtlZeroMemory(frame, PSP_RING_FRAME_SIZE);
            frame[0] = (ULONG)(cmdPa.QuadPart & 0xFFFFFFFF);
            frame[1] = (ULONG)(cmdPa.QuadPart >> 32);
            frame[2] = PSP_CMD_BUF_SIZE;
            ringWriteOffset += PSP_RING_FRAME_SIZE;

            KdPrint(("AUTOLOAD_RLC: triggering GPU firmware execution\n"));

            PVOID mboxBase = devExt->Bar0Base ? devExt->Bar0Base : devExt->MmioBase;
            WRITE_REGISTER_ULONG((PULONG)((PUCHAR)mboxBase + PSP_C2PMSG_67_OFFSET), ringWriteOffset);
            KeReleaseSpinLock(&devExt->CommandLock, ringIrql);

            ULONG timeout, resp = 0;
            for (timeout = 0; timeout < 5000; timeout++) {
                KeStallExecutionProcessor(1000);
                resp = READ_REGISTER_ULONG((PULONG)((PUCHAR)mboxBase + PSP_C2PMSG_64_OFFSET));
                if (resp & 0x80000000) {
                    ULONG st = resp & 0x0000FFFF;
                    ULONG cbStatus = ((PULONG)cmdBuffer)[864/sizeof(ULONG)];
                    status = (st == 0 && cbStatus == 0) ? STATUS_SUCCESS : STATUS_UNSUCCESSFUL;
                    KdPrint(("AUTOLOAD_RLC: C2PMSG_64=0x%08X cmdResp=0x%08X\n", resp, cbStatus));
                    break;
                }
            }
            if (timeout >= 5000) { KdPrint(("AUTOLOAD_RLC: timeout\n")); status = STATUS_TIMEOUT; }

            ExFreePool(cmdBuffer);
            if (outputLength >= sizeof(ULONG)) { ((PULONG)outputBuffer)[0] = resp; bytesReturned = sizeof(ULONG); }
            break;
        }

        case IOCTL_PSP_INIT_TMR:
        {
            status = PspInitTmr(devExt);
            if (outputLength >= sizeof(ULONG)) {
                ((PULONG)outputBuffer)[0] = g_TmrInitialized ? 1 : 0;
                bytesReturned = sizeof(ULONG);
            }
            break;
        }

        case IOCTL_PSP_SMU_WAKE:
        {
            if (inputLength < sizeof(PSP_SMU_WAKE_REQUEST)) {
                status = STATUS_INVALID_PARAMETER;
                break;
            }

            PSP_SMU_WAKE_REQUEST* req = (PSP_SMU_WAKE_REQUEST*)inputBuffer;
            ULONG response = 0;

            status = PspSmuWake(devExt, req->Message, req->Argument, &response);

            if (outputLength >= sizeof(PSP_SMU_WAKE_RESPONSE)) {
                PSP_SMU_WAKE_RESPONSE* resp = (PSP_SMU_WAKE_RESPONSE*)outputBuffer;
                resp->Message = req->Message;
                resp->Argument = req->Argument;
                resp->Response = response;
                resp->Status = (status == STATUS_SUCCESS) ? 1 : (status == STATUS_TIMEOUT ? 0 : 0xFF);
                bytesReturned = sizeof(PSP_SMU_WAKE_RESPONSE);
            }
            break;
        }

        case IOCTL_PSP_LOAD_TOC:
        {
            status = STATUS_NOT_IMPLEMENTED;
            break;
        }

        case IOCTL_PSP_KIQ_SUBMIT:
        {
            PSP_KIQ_SUBMIT_REQUEST* req = (PSP_KIQ_SUBMIT_REQUEST*)inputBuffer;
            
            // Initialize KIQ ring on first use
            if (!g_KiqRingInitialized) {
                status = PspKiqInit(devExt);
                if (!NT_SUCCESS(status)) {
                    KdPrint(("KIQ: Init failed: 0x%08X\n", status));
                    break;
                }
            }

            // Parse input: use struct layout with CommandCount and Commands[]
            if (inputLength < sizeof(PSP_KIQ_SUBMIT_REQUEST)) {
                status = STATUS_BUFFER_TOO_SMALL;
                break;
            }
            ULONG cmdCount = req->CommandCount;
            if (cmdCount == 0 || cmdCount > 64) {
                status = STATUS_INVALID_PARAMETER;
                break;
            }

            status = PspKiqSubmit(devExt, &req->Commands[0], cmdCount);
            if (NT_SUCCESS(status) && outputLength >= sizeof(ULONG)) {
                ((PULONG)outputBuffer)[0] = g_KiqRingWptr;
                bytesReturned = sizeof(ULONG);
            }
            break;
        }

        case IOCTL_PSP_KIQ_LOAD_FW:
        {
            if (!g_KiqRingInitialized) {
                status = PspKiqInit(devExt);
                if (!NT_SUCCESS(status)) {
                    KdPrint(("KIQ_LOAD_FW: KIQ init failed: 0x%08X\n", status));
                    break;
                }
            }

            if (inputLength < sizeof(ULONG) * 4) {
                status = STATUS_INVALID_PARAMETER;
                break;
            }

            ULONG fwType = ((PULONG)inputBuffer)[0];
            ULONG fwSize = ((PULONG)inputBuffer)[1];
            ULONG fwPaLow = ((PULONG)inputBuffer)[2];
            ULONG fwPaHigh = ((PULONG)inputBuffer)[3];

            if (fwSize == 0 || fwSize > 0x100000) {
                KdPrint(("KIQ_LOAD_FW: Invalid FW size %u\n", fwSize));
                status = STATUS_INVALID_PARAMETER;
                break;
            }

            PVOID cmdBuffer = ExAllocatePool2(POOL_FLAG_NON_PAGED, PSP_CMD_BUF_SIZE, 'mCSP');
            if (!cmdBuffer) { status = STATUS_INSUFFICIENT_RESOURCES; break; }
            RtlZeroMemory(cmdBuffer, PSP_CMD_BUF_SIZE);
            PULONG cmd = (PULONG)cmdBuffer;
            cmd[0] = PSP_CMD_BUF_SIZE;
            cmd[1] = 1;
            cmd[2] = GFX_CMD_ID_LOAD_IP_FW;
            cmd[7] = fwPaLow;
            cmd[8] = fwPaHigh;
            cmd[9] = fwSize;
            cmd[10] = fwType;
            PHYSICAL_ADDRESS cmdPa = MmGetPhysicalAddress(cmdBuffer);

            KIRQL ringIrql;
            KeAcquireSpinLock(&devExt->CommandLock, &ringIrql);
            if (ringWriteOffset + PSP_RING_FRAME_SIZE > sizeof(g_RingBuffer))
                ringWriteOffset = 0;
            PULONG frame = (PULONG)(g_RingBuffer + ringWriteOffset);
            RtlZeroMemory(frame, PSP_RING_FRAME_SIZE);
            frame[0] = (ULONG)(cmdPa.QuadPart & 0xFFFFFFFF);
            frame[1] = (ULONG)(cmdPa.QuadPart >> 32);
            frame[2] = PSP_CMD_BUF_SIZE;
            ringWriteOffset += PSP_RING_FRAME_SIZE;
            KdPrint(("KIQ_LOAD_FW: type=%u size=%u PA=%08X:%08X\n", fwType, fwSize, fwPaHigh, fwPaLow));

            PVOID mboxBase = devExt->Bar0Base ? devExt->Bar0Base : devExt->MmioBase;
            WRITE_REGISTER_ULONG((PULONG)((PUCHAR)mboxBase + PSP_C2PMSG_67_OFFSET), ringWriteOffset);
            KeReleaseSpinLock(&devExt->CommandLock, ringIrql);

            ULONG timeout, resp = 0;
            for (timeout = 0; timeout < 5000; timeout++) {
                KeStallExecutionProcessor(1000);
                resp = READ_REGISTER_ULONG((PULONG)((PUCHAR)mboxBase + PSP_C2PMSG_64_OFFSET));
                if (resp & 0x80000000) {
                    ULONG st = resp & 0x0000FFFF;
                    ULONG cbStatus = ((PULONG)cmdBuffer)[864/sizeof(ULONG)];
                    status = (st == 0 && cbStatus == 0) ? STATUS_SUCCESS : STATUS_UNSUCCESSFUL;
                    KdPrint(("KIQ_LOAD_FW: C2PMSG_64=0x%08X cmdResp=0x%08X\n", resp, cbStatus));
                    break;
                }
            }
            if (timeout >= 5000) { KdPrint(("KIQ_LOAD_FW: timeout\n")); status = STATUS_TIMEOUT; }

            ExFreePool(cmdBuffer);
            if (outputLength >= sizeof(ULONG)) { ((PULONG)outputBuffer)[0] = resp; bytesReturned = sizeof(ULONG); }
            break;
        }

        default:
            status = STATUS_INVALID_DEVICE_REQUEST;
            break;
    }

    Irp->IoStatus.Status = status;
    Irp->IoStatus.Information = bytesReturned;
    IoCompleteRequest(Irp, IO_NO_INCREMENT);
    return status;
}
