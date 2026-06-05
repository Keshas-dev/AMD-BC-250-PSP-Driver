#include <ntddk.h>
#include "PspIoctl.h"

// PSP Mailbox register offsets (relative to BAR0 base)
#define PSP_C2PMSG_35_OFFSET  0x1056C
#define PSP_C2PMSG_36_OFFSET  0x10570
#define PSP_C2PMSG_81_OFFSET  0x10614

// Device context stored in device extension
typedef struct _DEVICE_EXTENSION {
    PVOID       MmioBase;
    ULONG       MmioSize;
} DEVICE_EXTENSION, *PDEVICE_EXTENSION;

#define PSP_DEVICE_NAME        L"\\Device\\AmdBcPsp"
#define PSP_SYMBOLIC_LINK_NAME L"\\DosDevices\\AmdBcPsp"

// Function prototypes
DRIVER_INITIALIZE DriverEntry;
DRIVER_UNLOAD DriverUnload;
DRIVER_DISPATCH PspCreateClose;
DRIVER_DISPATCH PspDeviceControl;

NTSTATUS PspQueryDeviceResources(PDEVICE_EXTENSION devExt);

NTSTATUS DriverEntry(_In_ PDRIVER_OBJECT DriverObject, _In_ PUNICODE_STRING RegistryPath)
{
    NTSTATUS status;
    PDEVICE_OBJECT deviceObject = NULL;
    UNICODE_STRING deviceName;
    UNICODE_STRING symLinkName;

    UNREFERENCED_PARAMETER(RegistryPath);

    KdPrint(("=== AMD BC-250 PSP Driver: DriverEntry ===\n"));

    // Set dispatch functions
    DriverObject->MajorFunction[IRP_MJ_CREATE] = PspCreateClose;
    DriverObject->MajorFunction[IRP_MJ_CLOSE] = PspCreateClose;
    DriverObject->MajorFunction[IRP_MJ_DEVICE_CONTROL] = PspDeviceControl;
    DriverObject->DriverUnload = DriverUnload;

    // Create device
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

    // Create symbolic link for user-mode access
    RtlInitUnicodeString(&symLinkName, PSP_SYMBOLIC_LINK_NAME);
    status = IoCreateSymbolicLink(&symLinkName, &deviceName);
    if (!NT_SUCCESS(status)) {
        KdPrint(("IoCreateSymbolicLink failed: 0x%08X\n", status));
        IoDeleteDevice(deviceObject);
        return status;
    }

    // Set device flags - DO_BUFFERED_IO for METHOD_BUFFERED
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

    // Release MMIO mapping if active
    if (devExt->MmioBase != NULL) {
        MmUnmapIoSpace(devExt->MmioBase, devExt->MmioSize);
        devExt->MmioBase = NULL;
        KdPrint(("BAR0 resources released\n"));
    }

    // Remove symbolic link and device
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

    // Check MMIO state for all commands except INIT_HW
    if (ioctlCode != IOCTL_PSP_INIT_HW && devExt->MmioBase == NULL) {
        KdPrint(("MMIO not initialized - send IOCTL_PSP_INIT_HW first\n"));
        Irp->IoStatus.Status = STATUS_DEVICE_NOT_READY;
        Irp->IoStatus.Information = 0;
        IoCompleteRequest(Irp, IO_NO_INCREMENT);
        return STATUS_DEVICE_NOT_READY;
    }

    switch (ioctlCode) {
        case IOCTL_PSP_INIT_HW:
        {
            if (inputLength < sizeof(ULONG) * 2) {
                status = STATUS_INVALID_PARAMETER;
                break;
            }

            if (devExt->MmioBase != NULL) {
                // Already initialized - release first
                MmUnmapIoSpace(devExt->MmioBase, devExt->MmioSize);
                devExt->MmioBase = NULL;
                devExt->MmioSize = 0;
            }

            PULONG params = (PULONG)inputBuffer;
            PHYSICAL_ADDRESS physAddr;
            physAddr.QuadPart = params[0];  // Physical address from user-mode
            ULONG size = params[1];         // Size to map

            devExt->MmioBase = MmMapIoSpace(physAddr, size, MmNonCached);
            if (devExt->MmioBase == NULL) {
                status = STATUS_INSUFFICIENT_RESOURCES;
                KdPrint(("MmMapIoSpace failed for PA=0x%llX size=%u\n", physAddr.QuadPart, size));
                break;
            }

            devExt->MmioSize = size;
            KdPrint(("MMIO mapped: PA=0x%llX VA=%p size=%u\n", physAddr.QuadPart, devExt->MmioBase, size));
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

            if (offset >= devExt->MmioSize) {
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

            if (offset >= devExt->MmioSize) {
                status = STATUS_ARRAY_BOUNDS_EXCEEDED;
                break;
            }

            WRITE_REGISTER_ULONG((PULONG)((PUCHAR)devExt->MmioBase + offset), value);
            bytesReturned = sizeof(ULONG);
            break;
        }

        case IOCTL_PSP_LOAD_FW:
        {
            PVOID fwBuffer = NULL;
            size_t fwSize = 0;
            PVOID contiguousBuffer = NULL;
            PHYSICAL_ADDRESS physAddr;
            ULONG timeout = 0;

            if (inputLength == 0) {
                status = STATUS_INVALID_PARAMETER;
                break;
            }

            fwBuffer = inputBuffer;
            fwSize = inputLength;

            KdPrint(("IOCTL_PSP_LOAD_FW: Received firmware buffer, size=%u bytes\n", (ULONG)fwSize));

            PHYSICAL_ADDRESS highAddr;
            highAddr.QuadPart = 0xFFFFFFFF;

            contiguousBuffer = MmAllocateContiguousMemory(fwSize, highAddr);
            if (contiguousBuffer == NULL) {
                status = STATUS_INSUFFICIENT_RESOURCES;
                KdPrint(("IOCTL_PSP_LOAD_FW: Failed to allocate contiguous memory\n"));
                break;
            }

            RtlCopyMemory(contiguousBuffer, fwBuffer, fwSize);
            physAddr = MmGetPhysicalAddress(contiguousBuffer);
            KdPrint(("IOCTL_PSP_LOAD_FW: Firmware at PA=0x%llX, size=%u\n",
                physAddr.QuadPart, (ULONG)fwSize));

            if (PSP_C2PMSG_36_OFFSET < devExt->MmioSize) {
                WRITE_REGISTER_ULONG(
                    (PULONG)((PUCHAR)devExt->MmioBase + PSP_C2PMSG_36_OFFSET),
                    (ULONG)(physAddr.QuadPart & 0xFFFFFFFF)
                );
                KdPrint(("IOCTL_PSP_LOAD_FW: Wrote PA=0x%08X to C2PMSG_36\n",
                    (ULONG)(physAddr.QuadPart & 0xFFFFFFFF)));
            } else {
                status = STATUS_DEVICE_CONFIGURATION_ERROR;
                MmFreeContiguousMemory(contiguousBuffer);
                break;
            }

            if (PSP_C2PMSG_35_OFFSET < devExt->MmioSize) {
                WRITE_REGISTER_ULONG(
                    (PULONG)((PUCHAR)devExt->MmioBase + PSP_C2PMSG_35_OFFSET),
                    0x1
                );
                KdPrint(("IOCTL_PSP_LOAD_FW: Wrote command 0x1 to C2PMSG_35\n"));
            } else {
                status = STATUS_DEVICE_CONFIGURATION_ERROR;
                MmFreeContiguousMemory(contiguousBuffer);
                break;
            }

            if (PSP_C2PMSG_81_OFFSET < devExt->MmioSize) {
                ULONG statusReg = 0;
                ULONG initialStatus = 0;
                BOOLEAN success = FALSE;

                // Save initial status before sending command
                initialStatus = READ_REGISTER_ULONG(
                    (PULONG)((PUCHAR)devExt->MmioBase + PSP_C2PMSG_81_OFFSET)
                );
                KdPrint(("IOCTL_PSP_LOAD_FW: Initial C2PMSG_81=0x%08X\n", initialStatus));

                for (timeout = 0; timeout < 10000; timeout++) {
                    KeStallExecutionProcessor(1000);
                    statusReg = READ_REGISTER_ULONG(
                        (PULONG)((PUCHAR)devExt->MmioBase + PSP_C2PMSG_81_OFFSET)
                    );
                    if (statusReg != initialStatus) {
                        success = TRUE;
                        KdPrint(("IOCTL_PSP_LOAD_FW: C2PMSG_81 changed: 0x%08X -> 0x%08X\n",
                            initialStatus, statusReg));
                        break;
                    }
                }

                if (success) {
                    KdPrint(("IOCTL_PSP_LOAD_FW: SUCCESS! Status=0x%08X after %u ms\n",
                        statusReg, timeout));
                    status = STATUS_SUCCESS;
                    if (outputLength >= sizeof(ULONG)) {
                        ((PULONG)outputBuffer)[0] = statusReg;
                        bytesReturned = sizeof(ULONG);
                    }
                } else {
                    KdPrint(("IOCTL_PSP_LOAD_FW: TIMEOUT waiting for C2PMSG_81\n"));
                    status = STATUS_TIMEOUT;
                }
            } else {
                status = STATUS_DEVICE_CONFIGURATION_ERROR;
            }

            MmFreeContiguousMemory(contiguousBuffer);
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

NTSTATUS PspQueryDeviceResources(PDEVICE_EXTENSION devExt)
{
    return STATUS_NOT_SUPPORTED;
}
