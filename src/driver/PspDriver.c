#include <ntddk.h>
#include "PspIoctl.h"
#include "firmware_data.h"

// PSP Mailbox register offsets (BAR5-relative)
#define PSP_C2PMSG_35_OFFSET  0x1056C
#define PSP_C2PMSG_36_OFFSET  0x10570
#define PSP_C2PMSG_81_OFFSET  0x10614

// NBIO signature registers for firewall unlock
#define NBIO_SIG1_OFFSET       0xC100
#define NBIO_SIG2_OFFSET       0xC180
#define NBIO_SIG1_VALUE        0xFEDCBAEF
#define NBIO_SIG2_VALUE        0xFEDCBADF
#define MMHUB_CHECK_OFFSET     0x50D0

#define PSP_FW_WAIT_MS         10000

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
} DEVICE_EXTENSION, *PDEVICE_EXTENSION;

#define PSP_DEVICE_NAME        L"\\Device\\AmdBcPsp"
#define PSP_SYMBOLIC_LINK_NAME L"\\DosDevices\\AmdBcPsp"

DRIVER_INITIALIZE DriverEntry;
DRIVER_UNLOAD DriverUnload;
DRIVER_DISPATCH PspCreateClose;
DRIVER_DISPATCH PspDeviceControl;

/* Static 4KB ring buffer - one physical page, always contiguous */
static UCHAR g_RingBuffer[0x1000];
static BOOLEAN g_RingBufferInitialized = FALSE;
static PHYSICAL_ADDRESS g_RingBufferPhysical;

static BOOLEAN PspValidateFirmware(PUCHAR FirmwareData, ULONG FirmwareSize)
{
    if (FirmwareData == NULL || FirmwareSize < 256)
        return FALSE;

    // Validate firmware header: first 4 bytes should be the total size
    ULONG headerSize = *(volatile ULONG*)FirmwareData;
    if (headerSize == 0 || headerSize > FirmwareSize + 256)
        return FALSE;
    if (headerSize > 256 * 1024)
        return FALSE;

    // Verify size range
    if (FirmwareSize < 1024 || FirmwareSize > 512 * 1024)
        return FALSE;

    // Check signature bytes near the end of actual data
    ULONG checkOffset = headerSize > 256 ? headerSize - 256 : 0;
    if (checkOffset > 0 && checkOffset < FirmwareSize) {
        ULONG sig = *(volatile ULONG*)(FirmwareData + checkOffset);
        KdPrint(("FW validation: headerSize=%u sig@0x%X=0x%08X\n",
            headerSize, checkOffset, sig));
    }

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

    if (devExt->FwBuffer == NULL) {
        KdPrint(("No firmware loaded\n"));
        return STATUS_NO_MEMORY;
    }

    // Write PA >> 20 to C2PMSG_36 (PSP uses 1MB-aligned format)
    WRITE_REGISTER_ULONG(
        (PULONG)((PUCHAR)devExt->MmioBase + PSP_C2PMSG_36_OFFSET),
        devExt->FwPaShifted
    );
    KdPrint(("Mailbox: Wrote PA>>20=0x%08X to C2PMSG_36\n", devExt->FwPaShifted));

    // Save initial C2PMSG_81
    initialStatus = READ_REGISTER_ULONG(
        (PULONG)((PUCHAR)devExt->MmioBase + PSP_C2PMSG_81_OFFSET)
    );

    // Write command to C2PMSG_35
    WRITE_REGISTER_ULONG(
        (PULONG)((PUCHAR)devExt->MmioBase + PSP_C2PMSG_35_OFFSET),
        command
    );
    KdPrint(("Mailbox: Wrote command 0x%08X to C2PMSG_35\n", command));

    // Wait for C2PMSG_81 to change
    {
        LARGE_INTEGER delay;
        delay.QuadPart = -10000LL;
        for (timeout = 0; timeout < PSP_FW_WAIT_MS; timeout++) {
            KeDelayExecutionThread(KernelMode, FALSE, &delay);
            statusReg = READ_REGISTER_ULONG(
                (PULONG)((PUCHAR)devExt->MmioBase + PSP_C2PMSG_81_OFFSET)
            );
            if (statusReg != initialStatus) {
                KdPrint(("Mailbox: C2PMSG_81 changed: 0x%08X -> 0x%08X after %u ms\n",
                    initialStatus, statusReg, timeout));
                return STATUS_SUCCESS;
            }
        }
    }

    KdPrint(("Mailbox: TIMEOUT waiting for C2PMSG_81 change (command 0x%08X)\n", command));
    return STATUS_TIMEOUT;
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

    RtlInitUnicodeString(&deviceName, PSP_DEVICE_NAME);
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

    PspFreeFirmware(devExt);

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

    if (ioctlCode != IOCTL_PSP_INIT_HW && devExt->MmioBase == NULL) {
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

            devExt->MmioBase = MmMapIoSpace(physAddr, size, MmNonCached);
            if (devExt->MmioBase == NULL) {
                status = STATUS_INSUFFICIENT_RESOURCES;
                KdPrint(("MmMapIoSpace failed for PA=0x%llX size=%u\n", physAddr.QuadPart, size));
                break;
            }

            devExt->MmioSize = size;
            KdPrint(("MMIO mapped: PA=0x%llX VA=%p size=%u\n", physAddr.QuadPart, devExt->MmioBase, size));

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

            if (offset + sizeof(ULONG) > devExt->MmioSize || (offset & 0x3)) {
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

            if (offset + sizeof(ULONG) > devExt->MmioSize || (offset & 0x3)) {
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

            // Free previous firmware if any
            PspFreeFirmware(devExt);

            PHYSICAL_ADDRESS highAddr;
            highAddr.QuadPart = 0xFFFFFFFF;

            devExt->FwBuffer = MmAllocateContiguousMemory(inputLength, highAddr);
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
            RtlZeroMemory(devExt->RingBuffer, devExt->RingSize);

            KdPrint(("CREATE_RING: Ring at VA=%p PA=0x%llX size=%u\n",
                devExt->RingBuffer, devExt->RingPhysical.QuadPart, devExt->RingSize));

            /* Wait briefly for TOS_READY */
            ULONG c64;
            for (ULONG t = 0; t < 100; t++) {
                c64 = READ_REGISTER_ULONG((PULONG)((PUCHAR)devExt->MmioBase + 0x105E0));
                if (c64 & 0x80000000) break;
                KeStallExecutionProcessor(10000);
            }

            /* Write ring address to C2PMSG_69/70 */
            WRITE_REGISTER_ULONG((PULONG)((PUCHAR)devExt->MmioBase + 0x105F4),
                (ULONG)(devExt->RingPhysical.QuadPart & 0xFFFFFFFF));
            WRITE_REGISTER_ULONG((PULONG)((PUCHAR)devExt->MmioBase + 0x105F8),
                (ULONG)(devExt->RingPhysical.QuadPart >> 32));
            /* Ring size to C2PMSG_71 */
            WRITE_REGISTER_ULONG((PULONG)((PUCHAR)devExt->MmioBase + 0x105FC), devExt->RingSize);
            /* Trigger */
            WRITE_REGISTER_ULONG((PULONG)((PUCHAR)devExt->MmioBase + 0x105E0), 0);

            KeStallExecutionProcessor(50000);

            c64 = READ_REGISTER_ULONG((PULONG)((PUCHAR)devExt->MmioBase + 0x105E0));
            devExt->RingCreated = TRUE;  /* Ring registers were programmed successfully */

            KdPrint(("CREATE_RING: C2PMSG_64=0x%08X ringCreated=1\n", c64));

            if (outputLength >= sizeof(ULONG) * 2) {
                ((PULONG)outputBuffer)[0] = (ULONG)(devExt->RingPhysical.QuadPart & 0xFFFFFFFF);
                ((PULONG)outputBuffer)[1] = c64;
                bytesReturned = sizeof(ULONG) * 2;
            }
            status = STATUS_SUCCESS;
            break;
        }

        case IOCTL_PSP_NBIO_VIA_RING:
        {
            if (!devExt->RingCreated || devExt->RingBuffer == NULL) {
                status = STATUS_DEVICE_NOT_READY;
                break;
            }

            /* Format PSP command packet in ring buffer */
            /* Standard PSP command: dword[0]=cmd, dword[1]=arg */
            volatile PULONG ring = (PULONG)devExt->RingBuffer;
            ring[0] = 0x00020000;  /* NBIO unlock command */
            ring[1] = 0;
            ring[2] = 0;
            ring[3] = 0;
            KeMemoryBarrier();

            /* Update ring write pointer to signal PSP */
            WRITE_REGISTER_ULONG((PULONG)((PUCHAR)devExt->MmioBase + 0x105EC), 4 * sizeof(ULONG));

            KeStallExecutionProcessor(100000);  /* 100ms */

            /* Also write NBIO signatures */
            WRITE_REGISTER_ULONG((PULONG)((PUCHAR)devExt->MmioBase + NBIO_SIG1_OFFSET), NBIO_SIG1_VALUE);
            WRITE_REGISTER_ULONG((PULONG)((PUCHAR)devExt->MmioBase + NBIO_SIG2_OFFSET), NBIO_SIG2_VALUE);

            KeStallExecutionProcessor(10000);

            /* Check MMHUB */
            ULONG mmhub = READ_REGISTER_ULONG((PULONG)((PUCHAR)devExt->MmioBase + MMHUB_CHECK_OFFSET));
            ULONG grbm = READ_REGISTER_ULONG((PULONG)((PUCHAR)devExt->MmioBase + 0x2004));

            KdPrint(("NBIO_VIA_RING: MMHUB=0x%08X GRBM=0x%08X\n", mmhub, grbm));

            if (outputLength >= sizeof(ULONG) * 3) {
                ((PULONG)outputBuffer)[0] = mmhub;
                ((PULONG)outputBuffer)[1] = grbm;
                ((PULONG)outputBuffer)[2] = devExt->RingCreated ? 1 : 0;
                bytesReturned = sizeof(ULONG) * 3;
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

            PHYSICAL_ADDRESS highAddr;
            highAddr.QuadPart = 0xFFFFFFFF;

            devExt->FwBuffer = MmAllocateContiguousMemory(g_SosFirmwareSize, highAddr);
            if (devExt->FwBuffer == NULL) {
                status = STATUS_INSUFFICIENT_RESOURCES;
                KdPrint(("IOCTL_PSP_LOAD_EMBEDDED_FW: Failed to allocate\n"));
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

        default:
            status = STATUS_INVALID_DEVICE_REQUEST;
            break;
    }

    Irp->IoStatus.Status = status;
    Irp->IoStatus.Information = bytesReturned;
    IoCompleteRequest(Irp, IO_NO_INCREMENT);
    return status;
}
