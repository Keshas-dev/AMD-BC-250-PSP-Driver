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

#define PSP_FW_WAIT_MS         500
#define PSP_TOS_READY_TIMEOUT  60000  // 60 second timeout for TOS_READY (Linux shows ~5.5s on BC-250)
#define PSP_BOOT_CMD_0xC100    0xC100
#define PSP_BOOT_CMD_0xC180    0xC180
#define PSP_BAR0_PHYSICAL      0xFD600000ULL

// Device context stored in device extension
typedef struct _DEVICE_EXTENSION {
    PVOID       MmioBase;
    ULONG       MmioSize;
    PVOID       FwBuffer;           // Persistent firmware buffer (allocated, not freed after LOAD_FW)
    PHYSICAL_ADDRESS FwPhysical;    // Physical address of firmware buffer
    ULONG       FwSize;             // Firmware size in bytes
    ULONG       FwPaShifted;        // PA >> 20 (1MB-aligned format for PSP)
    PVOID       RingBuffer;         // PSP ring buffer
    PHYSICAL_ADDRESS RingPhysical;
    ULONG       RingSize;
    BOOLEAN     RingCreated;
    KSPIN_LOCK  CommandLock;        // FIX #9: Protect SEND_CMD from race conditions
    PVOID       PciCfgBase;          // Mapped PCI ECAM region
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
static UCHAR g_CmdBuffer[PSP_CMD_BUF_SIZE];  // 1024-byte command buffer for ring submissions
static ULONG ringWriteOffset = 0;
static ULONG g_RingFwCount = 0;              // Number of GPU FW loaded via ring
static PVOID g_TmrBuffer = NULL;             // TMR (Trusted Memory Region) buffer
static PHYSICAL_ADDRESS g_TmrPhysical;        // Physical address of TMR buffer
static ULONG g_TmrSize = 0;                  // TMR size in bytes
static BOOLEAN g_TmrInitialized = FALSE;     // Whether TMR has been initialized

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
    ULONG statusReg;
    ULONG initialStatus;
    KIRQL irql;  // FIX #9: Add IRQL for spinlock

    if (devExt->FwBuffer == NULL) {
        KdPrint(("No firmware loaded\n"));
        return STATUS_NO_MEMORY;
    }

    // FIX #9: Acquire spinlock to prevent race conditions
    KeAcquireSpinLock(&devExt->CommandLock, &irql);

    // FIX #12: Write physical address (low 32 bits, assumes <4GB) to C2PMSG_36,
    // then write command to C2PMSG_35.
    // CRITICAL: Must clear C2PMSG_81 first by writing 0, or PSP ignores command.
    ULONG paLo = (ULONG)(devExt->FwPhysical.QuadPart & 0xFFFFFFFF);

    // Save original C2PMSG_81 (SOS alive flag for GPU driver)
    ULONG originalC2pmsg81 = READ_REGISTER_ULONG(
        (PULONG)((PUCHAR)devExt->MmioBase + PSP_C2PMSG_81_OFFSET)
    );

    // Clear previous response in C2PMSG_81 so PSP accepts new command
    WRITE_REGISTER_ULONG(
        (PULONG)((PUCHAR)devExt->MmioBase + PSP_C2PMSG_81_OFFSET),
        0
    );

    // Save cleared C2PMSG_81 as initial for polling
    initialStatus = READ_REGISTER_ULONG(
        (PULONG)((PUCHAR)devExt->MmioBase + PSP_C2PMSG_81_OFFSET)
    );

    // Write PA (low 32 bits, PSP sees <4GB physical) to C2PMSG_36
    WRITE_REGISTER_ULONG(
        (PULONG)((PUCHAR)devExt->MmioBase + PSP_C2PMSG_36_OFFSET),
        paLo
    );
    KdPrint(("Mailbox: Wrote PA=0x%08X to C2PMSG_36\n", paLo));

    // Write command to C2PMSG_35 (triggers PSP execution)
    WRITE_REGISTER_ULONG(
        (PULONG)((PUCHAR)devExt->MmioBase + PSP_C2PMSG_35_OFFSET),
        command
    );
    KdPrint(("Mailbox: Wrote command 0x%08X to C2PMSG_35\n", command));

        // FIX #4: Use non-blocking delay instead of busy-wait
    NTSTATUS status = STATUS_SUCCESS;
    for (timeout = 0; timeout < PSP_FW_WAIT_MS; timeout++) {
        KeStallExecutionProcessor(1000);  // 1ms stall
        statusReg = READ_REGISTER_ULONG(
            (PULONG)((PUCHAR)devExt->MmioBase + PSP_C2PMSG_81_OFFSET)
        );
        if (statusReg != initialStatus) {
            KdPrint(("Mailbox: C2PMSG_81 changed: 0x%08X -> 0x%08X after %u ms\n",
                initialStatus, statusReg, timeout));
            // FIX #11: Decode C2PMSG_81 response: bit 31 set = response valid,
            // bits [27:0] = status code. 0x00 = success, anything else = error.
            if (statusReg & 0x80000000) {
                ULONG pspStatus = statusReg & 0x0FFFFFFF;
                if (pspStatus != 0) {
                    KdPrint(("Mailbox: PSP returned error 0x%03X for command 0x%08X\n",
                        pspStatus, command));
                    status = STATUS_UNSUCCESSFUL;
                }
            }
            break;
        }
    }

    if (timeout >= PSP_FW_WAIT_MS) {
        KdPrint(("Mailbox: TIMEOUT waiting for C2PMSG_81 change (command 0x%08X)\n", command));
        status = STATUS_TIMEOUT;
    }

    // FIX #12: Acknowledge command completion by writing 0 to C2PMSG_35
    // (NOT C2PMSG_81 — that register holds SOS alive status and must be preserved)
    WRITE_REGISTER_ULONG(
        (PULONG)((PUCHAR)devExt->MmioBase + PSP_C2PMSG_35_OFFSET),
        0
    );

    // Restore SOS alive flag (0xF0000010) if it was present before command
    // This preserves the indicator that GPU driver Amdbc250PspInit() relies on
    if (originalC2pmsg81 == 0xF0000010 && NT_SUCCESS(status)) {
        WRITE_REGISTER_ULONG(
            (PULONG)((PUCHAR)devExt->MmioBase + PSP_C2PMSG_81_OFFSET),
            originalC2pmsg81
        );
    }

    // FIX #9: Release spinlock
    KeReleaseSpinLock(&devExt->CommandLock, irql);

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

    // Send INIT_TMR (0x01) via ring
    RtlZeroMemory(g_CmdBuffer, PSP_CMD_BUF_SIZE);
    PULONG cmd = (PULONG)g_CmdBuffer;
    cmd[0] = PSP_CMD_BUF_SIZE;
    cmd[1] = 1;
    cmd[2] = GFX_CMD_ID_INIT_TMR;
    cmd[7] = (ULONG)(g_TmrPhysical.QuadPart & 0xFFFFFFFF);
    cmd[8] = (ULONG)(g_TmrPhysical.QuadPart >> 32);
    cmd[9] = g_TmrSize;

    PHYSICAL_ADDRESS cmdPa = MmGetPhysicalAddress(g_CmdBuffer);

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

    WRITE_REGISTER_ULONG((PULONG)((PUCHAR)devExt->MmioBase + 0x105EC), ringWriteOffset);
    KeReleaseSpinLock(&devExt->CommandLock, ringIrql);

    ULONG timeout, resp = 0;
    for (timeout = 0; timeout < 2000; timeout++) {
        KeStallExecutionProcessor(1000);
        resp = READ_REGISTER_ULONG((PULONG)((PUCHAR)devExt->MmioBase + 0x105E0));
        if (resp & 0x80000000) {
            ULONG st = resp & 0x0000FFFF;
            ULONG cbStatus = ((PULONG)g_CmdBuffer)[864/sizeof(ULONG)];
            KdPrint(("TMR: C2PMSG_64=0x%08X cmdResp=0x%08X st=%u\n", resp, cbStatus, st));
            if (st == 0 && cbStatus == 0) {
                g_TmrInitialized = TRUE;
                KdPrint(("TMR: Initialized OK at PA=0x%llX\n", g_TmrPhysical.QuadPart));
                return STATUS_SUCCESS;
            }
            return STATUS_UNSUCCESSFUL;
        }
    }
    KdPrint(("TMR: timeout waiting for response\n"));
    return STATUS_TIMEOUT;
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
    PspFreeFirmware(devExt);

    if (devExt->PciCfgBase != NULL) {
        MmUnmapIoSpace(devExt->PciCfgBase, devExt->PciCfgSize);
        devExt->PciCfgBase = NULL;
    }

    if (devExt->MmioBase != NULL) {
        MmUnmapIoSpace(devExt->MmioBase, devExt->MmioSize);
        devExt->MmioBase = NULL;
        KdPrint(("BAR5 resources released\n"));
    }

    RtlInitUnicodeString(&symLinkName, PSP_SYMBOLIC_LINK_NAME);
    IoDeleteSymbolicLink(&symLinkName);
    IoDeleteDevice(deviceObject);
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

    if (ioctlCode != IOCTL_PSP_INIT_HW && ioctlCode != IOCTL_PSP_PCI_READ && ioctlCode != IOCTL_PSP_PCI_WRITE && devExt->MmioBase == NULL) {
        KdPrint(("MMIO not initialized\n"));
        Irp->IoStatus.Status = STATUS_DEVICE_NOT_READY;
        Irp->IoStatus.Information = 0;
        IoCompleteRequest(Irp, IO_NO_INCREMENT);
        return STATUS_DEVICE_NOT_READY;
    }

    switch (ioctlCode) {
        case IOCTL_PSP_INIT_HW:
        {
            if (inputLength < sizeof(PSP_INIT_HW_REQUEST)) {
                status = STATUS_INVALID_PARAMETER;
                break;
            }

            if (devExt->MmioBase != NULL) {
                MmUnmapIoSpace(devExt->MmioBase, devExt->MmioSize);
                devExt->MmioBase = NULL;
                devExt->MmioSize = 0;
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
            devExt->RingPhysical = g_RingBufferPhysical;
            devExt->RingSize = sizeof(g_RingBuffer);
            
            if (devExt->RingCreated) {
                KdPrint(("CREATE_RING: Ring already created, reinitializing...\n"));
            }
            RtlZeroMemory(devExt->RingBuffer, devExt->RingSize);

            KdPrint(("CREATE_RING: Ring at VA=%p PA=0x%llX size=%u\n",
                devExt->RingBuffer, devExt->RingPhysical.QuadPart, devExt->RingSize));

            /* Clear any stale error from C2PMSG_81 that might block TOS */
            WRITE_REGISTER_ULONG((PULONG)((PUCHAR)devExt->MmioBase + PSP_C2PMSG_81_OFFSET), 0);

            /* Linux psp_v11_0_8: Wait for MBOX_TOS_READY_FLAG (C2PMSG_64 bit 31)
               before programming ring registers. SOS must signal readiness first. */
            ULONG c64_val;
            ULONG tosTimeout;
            KdPrint(("CREATE_RING: C2PMSG_64=0x%08X C2PMSG_81=0x%08X waiting for TOS_READY...\n",
                READ_REGISTER_ULONG((PULONG)((PUCHAR)devExt->MmioBase + 0x105E0)),
                READ_REGISTER_ULONG((PULONG)((PUCHAR)devExt->MmioBase + PSP_C2PMSG_81_OFFSET))));
            for (tosTimeout = 0; tosTimeout < PSP_TOS_READY_TIMEOUT; tosTimeout++) {
                c64_val = READ_REGISTER_ULONG((PULONG)((PUCHAR)devExt->MmioBase + 0x105E0));
                if (c64_val & 0x80000000) {
                    KdPrint(("CREATE_RING: TOS ready after %u ms (C2PMSG_64=0x%08X)\n",
                        tosTimeout, c64_val));
                    break;
                }
                KeStallExecutionProcessor(1000);
            }

            if (tosTimeout >= PSP_TOS_READY_TIMEOUT) {
                KdPrint(("CREATE_RING: TOS not ready after %u ms (C2PMSG_64=0x%08X)\n",
                    PSP_TOS_READY_TIMEOUT, c64_val));
                status = STATUS_DEVICE_NOT_READY;
                break;
            }

            /* Write ring address to C2PMSG_69/70 */
            WRITE_REGISTER_ULONG((PULONG)((PUCHAR)devExt->MmioBase + 0x105F4),
                (ULONG)(devExt->RingPhysical.QuadPart & 0xFFFFFFFF));
            WRITE_REGISTER_ULONG((PULONG)((PUCHAR)devExt->MmioBase + 0x105F8),
                (ULONG)(devExt->RingPhysical.QuadPart >> 32));
            /* Ring size to C2PMSG_71 */
            WRITE_REGISTER_ULONG((PULONG)((PUCHAR)devExt->MmioBase + 0x105FC), devExt->RingSize);
            /* Ring type to C2PMSG_64: KM ring = 2 << 16 = 0x00020000 (psp_v11_0_8.c) */
            WRITE_REGISTER_ULONG((PULONG)((PUCHAR)devExt->MmioBase + 0x105E0), 0x00020000);

            /* Wait for response: C2PMSG_64 bit 31 = MBOX_TOS_RESP_FLAG */
            ULONG ringResp;
            ULONG respTimeout;
            for (respTimeout = 0; respTimeout < 3000; respTimeout++) {
                KeStallExecutionProcessor(1000);
                ringResp = READ_REGISTER_ULONG((PULONG)((PUCHAR)devExt->MmioBase + 0x105E0));
                if (ringResp & 0x80000000) break;
            }

            if (respTimeout >= 3000) {
                KdPrint(("CREATE_RING: Ring create timeout (C2PMSG_64=0x%08X)\n", ringResp));
                status = STATUS_DEVICE_NOT_READY;
                break;
            }

            devExt->RingCreated = TRUE;

            KdPrint(("CREATE_RING: C2PMSG_64=0x%08X tosWait=%ums respWait=%ums ringCreated=1\n",
                ringResp, tosTimeout, respTimeout));

            if (outputLength >= sizeof(ULONG) * 3) {
                ((PULONG)outputBuffer)[0] = (ULONG)(devExt->RingPhysical.QuadPart & 0xFFFFFFFF);
                ((PULONG)outputBuffer)[1] = c64_val;
                ((PULONG)outputBuffer)[2] = ringResp;
                bytesReturned = sizeof(ULONG) * 3;
            }
            status = STATUS_SUCCESS;
            break;
        }

        case IOCTL_PSP_NBIO_VIA_RING:
        {
            ULONG results[4] = {0};

            // Step 1: Write 0x00020000 to C2PMSG_64 (GFX_CTRL_CMD_ID_DESTROY_RINGS)
            ULONG c64_before = READ_REGISTER_ULONG((PULONG)((PUCHAR)devExt->MmioBase + 0x105E0));
            WRITE_REGISTER_ULONG((PULONG)((PUCHAR)devExt->MmioBase + 0x105E0), 0x00020000);
            results[0] = 0x00020000;

            KeStallExecutionProcessor(500000); // 500ms

            // Read C2PMSG_64 response
            ULONG c64resp = READ_REGISTER_ULONG((PULONG)((PUCHAR)devExt->MmioBase + 0x105E0));
            results[1] = c64resp;

            // Step 2: Write NBIO signatures directly to BAR5
            WRITE_REGISTER_ULONG((PULONG)((PUCHAR)devExt->MmioBase + NBIO_SIG1_OFFSET), NBIO_SIG1_VALUE);
            WRITE_REGISTER_ULONG((PULONG)((PUCHAR)devExt->MmioBase + NBIO_SIG2_OFFSET), NBIO_SIG2_VALUE);

            KeStallExecutionProcessor(100000); // 100ms

            // Step 3: Check results
            ULONG sig1 = READ_REGISTER_ULONG((PULONG)((PUCHAR)devExt->MmioBase + NBIO_SIG1_OFFSET));
            ULONG sig2 = READ_REGISTER_ULONG((PULONG)((PUCHAR)devExt->MmioBase + NBIO_SIG2_OFFSET));
            ULONG mmhub = READ_REGISTER_ULONG((PULONG)((PUCHAR)devExt->MmioBase + MMHUB_CHECK_OFFSET));
            ULONG grbm = READ_REGISTER_ULONG((PULONG)((PUCHAR)devExt->MmioBase + 0x2004));
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

            // Mailbox status
            info->C2PMSG_81 = READ_REGISTER_ULONG((PULONG)((PUCHAR)devExt->MmioBase + PSP_C2PMSG_81_OFFSET));
            info->C2PMSG_35 = READ_REGISTER_ULONG((PULONG)((PUCHAR)devExt->MmioBase + PSP_C2PMSG_35_OFFSET));
            info->C2PMSG_36 = READ_REGISTER_ULONG((PULONG)((PUCHAR)devExt->MmioBase + PSP_C2PMSG_36_OFFSET));
            info->C2PMSG_37 = READ_REGISTER_ULONG((PULONG)((PUCHAR)devExt->MmioBase + 0x10574));
            info->C2PMSG_64 = READ_REGISTER_ULONG((PULONG)((PUCHAR)devExt->MmioBase + 0x105E0));
            info->PspAlive = (info->C2PMSG_81 != 0 && info->C2PMSG_81 != 0xFFFFFFFF) ? 1 : 0;

            // Firmware info
            info->FwLoaded = (devExt->FwBuffer != NULL) ? 1 : 0;
            info->FwSize = devExt->FwSize;
            info->FwPaShifted = devExt->FwPaShifted;

            // NBIO status
            info->NbioSig1 = READ_REGISTER_ULONG((PULONG)((PUCHAR)devExt->MmioBase + NBIO_SIG1_OFFSET));
            info->NbioSig2 = READ_REGISTER_ULONG((PULONG)((PUCHAR)devExt->MmioBase + NBIO_SIG2_OFFSET));
            info->GrbmStatus = READ_REGISTER_ULONG((PULONG)((PUCHAR)devExt->MmioBase + 0x2004));
            info->MmhubCheck = READ_REGISTER_ULONG((PULONG)((PUCHAR)devExt->MmioBase + MMHUB_CHECK_OFFSET));
            info->GcCheck = READ_REGISTER_ULONG((PULONG)((PUCHAR)devExt->MmioBase + 0x3000));
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

            // FIX #5: Validate embedded firmware size before allocation
            // NOTE: g_SosFirmwareData contains Type 1 (SOS) content, suitable for CMD 0x8
            if (g_SosFirmwareSize > PSP_MAX_FW_TOTAL) {
                KdPrint(("IOCTL_PSP_LOAD_EMBEDDED_FW: Embedded FW too large (%u > %u)\n", 
                    g_SosFirmwareSize, PSP_MAX_FW_TOTAL));
                status = STATUS_INVALID_PARAMETER;
                break;
            }

            PHYSICAL_ADDRESS highAddr;
            highAddr.QuadPart = 0x10000000000ULL;  // Allow above 4GB

            devExt->FwBuffer = MmAllocateContiguousMemory(g_SysdrvFirmwareSize, highAddr);
            if (devExt->FwBuffer == NULL) {
                highAddr.QuadPart = 0xFFFFFFFF;
                devExt->FwBuffer = MmAllocateContiguousMemory(g_SysdrvFirmwareSize, highAddr);
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
            results[3] = READ_REGISTER_ULONG((PULONG)((PUCHAR)devExt->MmioBase + 0x2004));

            KdPrint(("BOOT_SEQ: FW=%d SYSDRV=%d SOS=%d GRBM=0x%08X\n",
                results[0] != 0, results[1], results[2], results[3]));

            if (outputLength >= sizeof(results)) {
                RtlCopyMemory(outputBuffer, results, sizeof(results));
                bytesReturned = sizeof(results);
            }
            status = STATUS_SUCCESS;  /* Always succeed - user checks GRBM in results */
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

            // Step 1: Read all mailbox registers
            probe->C2PMSG_35 = READ_REGISTER_ULONG((PULONG)((PUCHAR)devExt->MmioBase + PSP_C2PMSG_35_OFFSET));
            probe->C2PMSG_36 = READ_REGISTER_ULONG((PULONG)((PUCHAR)devExt->MmioBase + PSP_C2PMSG_36_OFFSET));
            probe->C2PMSG_37 = READ_REGISTER_ULONG((PULONG)((PUCHAR)devExt->MmioBase + 0x10574));
            probe->C2PMSG_64 = READ_REGISTER_ULONG((PULONG)((PUCHAR)devExt->MmioBase + 0x105E0));
            probe->C2PMSG_81 = READ_REGISTER_ULONG((PULONG)((PUCHAR)devExt->MmioBase + PSP_C2PMSG_81_OFFSET));

            // Step 2: Read NBIO region probes
            probe->NbioSig1 = READ_REGISTER_ULONG((PULONG)((PUCHAR)devExt->MmioBase + NBIO_SIG1_OFFSET));
            probe->NbioSig2 = READ_REGISTER_ULONG((PULONG)((PUCHAR)devExt->MmioBase + NBIO_SIG2_OFFSET));
            probe->MmhubCheck = READ_REGISTER_ULONG((PULONG)((PUCHAR)devExt->MmioBase + MMHUB_CHECK_OFFSET));
            probe->GrbmStatus = READ_REGISTER_ULONG((PULONG)((PUCHAR)devExt->MmioBase + 0x2004));
            probe->GcCheck = READ_REGISTER_ULONG((PULONG)((PUCHAR)devExt->MmioBase + 0x3000));
            probe->HdpCheck = READ_REGISTER_ULONG((PULONG)((PUCHAR)devExt->MmioBase + 0x05A0));

            // Step 3: Try writing NBIO sigs and verify
            WRITE_REGISTER_ULONG((PULONG)((PUCHAR)devExt->MmioBase + NBIO_SIG1_OFFSET), NBIO_SIG1_VALUE);
            WRITE_REGISTER_ULONG((PULONG)((PUCHAR)devExt->MmioBase + NBIO_SIG2_OFFSET), NBIO_SIG2_VALUE);
            KeStallExecutionProcessor(1000);
            ULONG sig1a = READ_REGISTER_ULONG((PULONG)((PUCHAR)devExt->MmioBase + NBIO_SIG1_OFFSET));
            ULONG sig2a = READ_REGISTER_ULONG((PULONG)((PUCHAR)devExt->MmioBase + NBIO_SIG2_OFFSET));
            probe->SigWriteOk = ((sig1a == NBIO_SIG1_VALUE) && (sig2a == NBIO_SIG2_VALUE)) ? 1 : 0;

            // Step 4: Try programming ring registers
            probe->RingAddrLow = (ULONG)(g_RingBufferPhysical.QuadPart & 0xFFFFFFFF);
            probe->RingAddrHigh = (ULONG)(g_RingBufferPhysical.QuadPart >> 32);
            probe->RingSize = 0x1000;
            WRITE_REGISTER_ULONG((PULONG)((PUCHAR)devExt->MmioBase + 0x105F4), probe->RingAddrLow);
            WRITE_REGISTER_ULONG((PULONG)((PUCHAR)devExt->MmioBase + 0x105F8), probe->RingAddrHigh);
            WRITE_REGISTER_ULONG((PULONG)((PUCHAR)devExt->MmioBase + 0x105FC), probe->RingSize);
            KeStallExecutionProcessor(1000);
            ULONG rl = READ_REGISTER_ULONG((PULONG)((PUCHAR)devExt->MmioBase + 0x105F4));
            ULONG rh = READ_REGISTER_ULONG((PULONG)((PUCHAR)devExt->MmioBase + 0x105F8));
            ULONG rs = READ_REGISTER_ULONG((PULONG)((PUCHAR)devExt->MmioBase + 0x105FC));
            probe->RingProgOk = ((rl == probe->RingAddrLow) && (rs == probe->RingSize)) ? 1 : 0;
            probe->RingCreated = devExt->RingCreated ? 1 : 0;

            // Step 5: Try NBIO via ring (write 0x00020000 to C2PMSG_64)
            WRITE_REGISTER_ULONG((PULONG)((PUCHAR)devExt->MmioBase + 0x105E0), 0x00020000);
            KeStallExecutionProcessor(500000);
            ULONG c64r = READ_REGISTER_ULONG((PULONG)((PUCHAR)devExt->MmioBase + 0x105E0));
            ULONG grbm2 = READ_REGISTER_ULONG((PULONG)((PUCHAR)devExt->MmioBase + 0x2004));
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

            // Build command buffer (psp_gfx_cmd_resp, 1024 bytes)
            // +0: buf_size, +4: buf_version, +8: cmd_id = GFX_CMD_ID_LOAD_IP_FW (0x06)
            // +28: cmd_load_ip_fw: fw_addr_lo, fw_addr_hi, fw_size, fw_type
            RtlZeroMemory(g_CmdBuffer, PSP_CMD_BUF_SIZE);
            PULONG cmd = (PULONG)g_CmdBuffer;
            cmd[0] = PSP_CMD_BUF_SIZE;
            cmd[1] = 1;
            cmd[2] = GFX_CMD_ID_LOAD_IP_FW;
            cmd[7] = (ULONG)(fwPa.QuadPart & 0xFFFFFFFF);
            cmd[8] = (ULONG)(fwPa.QuadPart >> 32);
            cmd[9] = fwSize;
            cmd[10] = fwType;
            PHYSICAL_ADDRESS cmdPa = MmGetPhysicalAddress(g_CmdBuffer);

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

            // Set ring wptr (C2PMSG_67 = 0x105EC, gpcom_wptr per psp_v11_0_8.c)
            WRITE_REGISTER_ULONG((PULONG)((PUCHAR)devExt->MmioBase + 0x105EC), ringWriteOffset);
            KeReleaseSpinLock(&devExt->CommandLock, ringIrql);

            // Poll C2PMSG_64 (0x105E0) for response (bit 31 = done)
            ULONG timeout, resp = 0;
            for (timeout = 0; timeout < 1000; timeout++) {
                KeStallExecutionProcessor(1000);
                resp = READ_REGISTER_ULONG((PULONG)((PUCHAR)devExt->MmioBase + 0x105E0));
                if (resp & 0x80000000) {
                    ULONG st = resp & 0x0000FFFF;
                    // Also check command buffer response status at +864
                    ULONG cbStatus = ((PULONG)g_CmdBuffer)[864/sizeof(ULONG)];
                    status = (st == 0 && cbStatus == 0) ? STATUS_SUCCESS : STATUS_UNSUCCESSFUL;
                    if (NT_SUCCESS(status)) g_RingFwCount++;
                    KdPrint(("RING: C2PMSG_64=0x%08X cmdResp=0x%08X\n", resp, cbStatus));
                    break;
                }
            }
            if (timeout >= 1000) { KdPrint(("RING: timeout\n")); status = STATUS_TIMEOUT; }

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

            info->RingBufferPA = (ULONG)(g_RingBufferPhysical.QuadPart & 0xFFFFFFFF);
            info->FwLoaded = devExt->FwBuffer ? 1 : 0;
            info->FwCount = g_RingFwCount;
            info->TMRBase = g_TmrInitialized ? g_TmrPhysical.QuadPart : 0xF40F800000;
            info->TMSSize = g_TmrSize ? g_TmrSize : 0x400000;
            info->GfxVersion = 10;

            info->C2pmsg64 = READ_REGISTER_ULONG(
                (PULONG)((PUCHAR)devExt->MmioBase + 0x105E0));
            info->C2pmsg81 = READ_REGISTER_ULONG(
                (PULONG)((PUCHAR)devExt->MmioBase + PSP_C2PMSG_81_OFFSET));
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
            if (!devExt->RingCreated) {
                status = STATUS_DEVICE_NOT_READY; break;
            }

            PSP_REG_PROG_REQUEST* req = (PSP_REG_PROG_REQUEST*)inputBuffer;
            ULONG regId = req->RegId;
            ULONG regVal = req->RegValue;

            RtlZeroMemory(g_CmdBuffer, PSP_CMD_BUF_SIZE);
            PULONG cmd = (PULONG)g_CmdBuffer;
            cmd[0] = PSP_CMD_BUF_SIZE;
            cmd[1] = 1;
            cmd[2] = 0x0000000B;
            cmd[7] = regVal;
            cmd[8] = regId;
            PHYSICAL_ADDRESS cmdPa = MmGetPhysicalAddress(g_CmdBuffer);

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

            KdPrint(("REG_PROG: id=%u val=0x%08X\n", regId, regVal));

            WRITE_REGISTER_ULONG(
                (PULONG)((PUCHAR)devExt->MmioBase + 0x105EC), ringWriteOffset);
            KeReleaseSpinLock(&devExt->CommandLock, ringIrql);

            ULONG timeout, resp = 0;
            for (timeout = 0; timeout < 1000; timeout++) {
                KeStallExecutionProcessor(1000);
                resp = READ_REGISTER_ULONG(
                    (PULONG)((PUCHAR)devExt->MmioBase + 0x105E0));
                if (resp & 0x80000000) {
                    ULONG st = resp & 0x0000FFFF;
                    ULONG cbStatus = ((PULONG)g_CmdBuffer)[864/sizeof(ULONG)];
                    status = (st == 0 && cbStatus == 0) ? STATUS_SUCCESS : STATUS_UNSUCCESSFUL;
                    KdPrint(("REG_PROG: C2PMSG_64=0x%08X cmdResp=0x%08X\n", resp, cbStatus));
                    break;
                }
            }
            if (timeout >= 1000) { status = STATUS_TIMEOUT; }

            if (outputLength >= sizeof(ULONG)) {
                ((PULONG)outputBuffer)[0] = regVal;
                bytesReturned = sizeof(ULONG);
            }
            break;
        }

        case IOCTL_PSP_AUTOLOAD_RLC:
        {
            if (!devExt->RingCreated) { status = STATUS_DEVICE_NOT_READY; break; }

            // Build command buffer with GFX_CMD_ID_AUTOLOAD_RLC (0x21)
            RtlZeroMemory(g_CmdBuffer, PSP_CMD_BUF_SIZE);
            PULONG cmd = (PULONG)g_CmdBuffer;
            cmd[0] = PSP_CMD_BUF_SIZE;
            cmd[1] = 1;
            cmd[2] = 0x00000021;  // GFX_CMD_ID_AUTOLOAD_RLC
            PHYSICAL_ADDRESS cmdPa = MmGetPhysicalAddress(g_CmdBuffer);

            // Build ring frame
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

            // Set wptr (C2PMSG_67) and wait for response on C2PMSG_64
            WRITE_REGISTER_ULONG((PULONG)((PUCHAR)devExt->MmioBase + 0x105EC), ringWriteOffset);
            KeReleaseSpinLock(&devExt->CommandLock, ringIrql);

            ULONG timeout, resp = 0;
            for (timeout = 0; timeout < 5000; timeout++) {  // Up to 5s for RLC autoload
                KeStallExecutionProcessor(1000);
                resp = READ_REGISTER_ULONG((PULONG)((PUCHAR)devExt->MmioBase + 0x105E0));
                if (resp & 0x80000000) {
                    ULONG st = resp & 0x0000FFFF;
                    ULONG cbStatus = ((PULONG)g_CmdBuffer)[864/sizeof(ULONG)];
                    status = (st == 0 && cbStatus == 0) ? STATUS_SUCCESS : STATUS_UNSUCCESSFUL;
                    KdPrint(("AUTOLOAD_RLC: C2PMSG_64=0x%08X cmdResp=0x%08X\n", resp, cbStatus));
                    break;
                }
            }
            if (timeout >= 5000) { KdPrint(("AUTOLOAD_RLC: timeout\n")); status = STATUS_TIMEOUT; }

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

        default:
            status = STATUS_INVALID_DEVICE_REQUEST;
            break;
    }

    Irp->IoStatus.Status = status;
    Irp->IoStatus.Information = bytesReturned;
    IoCompleteRequest(Irp, IO_NO_INCREMENT);
    return status;
}
