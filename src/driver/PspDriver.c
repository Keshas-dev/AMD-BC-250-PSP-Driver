#include <ntddk.h>
#include <wdf.h>
#include "PspIoctl.h"

// PSP Mailbox register offsets (relative to BAR0 base)
#define PSP_C2PMSG_35_OFFSET  0x1056C   // Command register
#define PSP_C2PMSG_36_OFFSET  0x10570   // Data register
#define PSP_C2PMSG_81_OFFSET  0x10614   // Status register

// Driver context structure
typedef struct _DEVICE_CONTEXT {
    PVOID       MmioBase;
    ULONG       MmioSize;
} DEVICE_CONTEXT, *PDEVICE_CONTEXT;

WDF_DECLARE_CONTEXT_TYPE_WITH_NAME(DEVICE_CONTEXT, DeviceGetContext)

// Function prototypes
EVT_WDF_DRIVER_DEVICE_ADD         EvtDeviceAdd;
EVT_WDF_DEVICE_PREPARE_HARDWARE   EvtDevicePrepareHardware;
EVT_WDF_DEVICE_RELEASE_HARDWARE   EvtDeviceReleaseHardware;
EVT_WDF_IO_QUEUE_IO_DEVICE_CONTROL EvtIoDeviceControl;

// Driver entry point
NTSTATUS DriverEntry(_In_ PDRIVER_OBJECT DriverObject, _In_ PUNICODE_STRING RegistryPath)
{
    WDF_DRIVER_CONFIG config;
    NTSTATUS status;

    KdPrint(("=== AMD BC-250 PSP Driver: DriverEntry ===\n"));

    WDF_DRIVER_CONFIG_INIT(&config, EvtDeviceAdd);
    status = WdfDriverCreate(DriverObject, RegistryPath, WDF_NO_OBJECT_ATTRIBUTES, &config, WDF_NO_HANDLE);

    return status;
}

// Called when Windows finds the PnP device (DEV_143E)
NTSTATUS EvtDeviceAdd(_In_ WDFDRIVER Driver, _In_ PWDFDEVICE_INIT DeviceInit)
{
    UNREFERENCED_PARAMETER(Driver);
    NTSTATUS status;
    WDF_OBJECT_ATTRIBUTES attributes;
    WDFDEVICE device;
    WDF_PNPPOWER_EVENT_CALLBACKS pnpCallbacks;
    WDF_IO_QUEUE_CONFIG queueConfig;
    DECLARE_CONST_UNICODE_STRING(userModeName, L"\\Device\\AmdBcPsp");
    DECLARE_CONST_UNICODE_STRING(symLinkName, L"\\DosDevices\\AmdBcPsp");

    KdPrint(("=== EvtDeviceAdd: Device detected ===\n"));

    // Setup PnP callbacks for hardware allocation
    WDF_PNPPOWER_EVENT_CALLBACKS_INIT(&pnpCallbacks);
    pnpCallbacks.EvtDevicePrepareHardware = EvtDevicePrepareHardware;
    pnpCallbacks.EvtDeviceReleaseHardware = EvtDeviceReleaseHardware;
    WdfDeviceInitSetPnpPowerEventCallbacks(DeviceInit, &pnpCallbacks);

    // Create device context
    WDF_OBJECT_ATTRIBUTES_INIT_CONTEXT_TYPE(&attributes, DEVICE_CONTEXT);
    status = WdfDeviceCreate(&DeviceInit, &attributes, &device);
    if (!NT_SUCCESS(status)) {
        KdPrint(("WdfDeviceCreate failed: 0x%08X\n", status));
        return status;
    }

    // Create symbolic link so user-mode can access the driver
    status = WdfDeviceCreateSymbolicLink(device, &symLinkName);
    if (!NT_SUCCESS(status)) {
        KdPrint(("WdfDeviceCreateSymbolicLink failed: 0x%08X\n", status));
        return status;
    }

    // Create default I/O queue for IOCTL requests
    WDF_IO_QUEUE_CONFIG_INIT_DEFAULT_QUEUE(&queueConfig, WdfIoQueueDispatchSequential);
    queueConfig.EvtIoDeviceControl = EvtIoDeviceControl;
    status = WdfIoQueueCreate(device, &queueConfig, WDF_NO_OBJECT_ATTRIBUTES, WDF_NO_HANDLE);
    if (!NT_SUCCESS(status)) {
        KdPrint(("WdfIoQueueCreate failed: 0x%08X\n", status));
        return status;
    }

    KdPrint(("=== EvtDeviceAdd: Success ===\n"));
    return status;
}

// Extracts physical BAR addresses and maps them into kernel memory
NTSTATUS EvtDevicePrepareHardware(_In_ WDFDEVICE Device, _In_ WDFCMRESLIST ResourcesRaw, _In_ WDFCMRESLIST ResourcesTranslated)
{
    PDEVICE_CONTEXT ctx = DeviceGetContext(Device);
    BOOLEAN foundMmio = FALSE;

    UNREFERENCED_PARAMETER(ResourcesRaw);

    for (ULONG i = 0; i < WdfCmResourceListGetCount(ResourcesTranslated); i++) {
        PCM_PARTIAL_RESOURCE_DESCRIPTOR desc = WdfCmResourceListGetDescriptor(ResourcesTranslated, i);
        
        if (desc->Type == CmResourceTypeMemory) {
            ctx->MmioSize = desc->u.Memory.Length;
            ctx->MmioBase = MmMapIoSpace(desc->u.Memory.Start, ctx->MmioSize, MmNonCached);
            
            if (ctx->MmioBase != NULL) {
                foundMmio = TRUE;
                KdPrint(("=== BAR0 Mapped OK! Base: %p, Size: %X ===\n", ctx->MmioBase, ctx->MmioSize));
                break;
            }
        }
    }

    if (!foundMmio) {
        KdPrint(("ERROR: No MMIO resource found!\n"));
    }

    return foundMmio ? STATUS_SUCCESS : STATUS_DEVICE_CONFIGURATION_ERROR;
}

// Release resources when device stops
NTSTATUS EvtDeviceReleaseHardware(_In_ WDFDEVICE Device, _In_ WDFCMRESLIST ResourcesTranslated)
{
    PDEVICE_CONTEXT ctx = DeviceGetContext(Device);
    UNREFERENCED_PARAMETER(ResourcesTranslated);

    if (ctx->MmioBase != NULL) {
        MmUnmapIoSpace(ctx->MmioBase, ctx->MmioSize);
        ctx->MmioBase = NULL;
        KdPrint(("=== BAR0 resources released ===\n"));
    }
    return STATUS_SUCCESS;
}

// IOCTL handler
VOID EvtIoDeviceControl(_In_ WDFQUEUE Queue, _In_ WDFREQUEST Request, _In_ size_t OutputBufferLength, _In_ size_t InputBufferLength, _In_ ULONG IoControlCode)
{
    PDEVICE_CONTEXT ctx = DeviceGetContext(WdfIoQueueGetDevice(Queue));
    NTSTATUS status = STATUS_SUCCESS;
    size_t bytesReturned = 0;

    UNREFERENCED_PARAMETER(OutputBufferLength);
    UNREFERENCED_PARAMETER(InputBufferLength);

    if (ctx->MmioBase == NULL) {
        KdPrint(("MMIO not mapped!\n"));
        WdfRequestCompleteWithInformation(Request, STATUS_DEVICE_NOT_READY, 0);
        return;
    }

    switch (IoControlCode) {
        case IOCTL_PSP_READ_REG: {
            PULONG buffer = NULL;
            status = WdfRequestRetrieveInputBuffer(Request, sizeof(ULONG) * 2, (PVOID*)&buffer, NULL);
            if (NT_SUCCESS(status)) {
                ULONG offset = buffer[0];
                if (offset < ctx->MmioSize) {
                    ULONG regVal = READ_REGISTER_ULONG((PULONG)((PUCHAR)ctx->MmioBase + offset));
                    status = WdfRequestRetrieveOutputBuffer(Request, sizeof(ULONG), (PVOID*)&buffer, NULL);
                    if (NT_SUCCESS(status)) {
                        buffer[0] = regVal;
                        bytesReturned = sizeof(ULONG);
                    }
                } else {
                    status = STATUS_ARRAY_BOUNDS_EXCEEDED;
                }
            } else {
                KdPrint(("IOCTL_PSP_READ_REG: WdfRequestRetrieveInputBuffer failed: 0x%08X\n", status));
            }
            break;
        }

        case IOCTL_PSP_WRITE_REG: {
            PULONG buffer = NULL;
            status = WdfRequestRetrieveInputBuffer(Request, sizeof(ULONG) * 2, (PVOID*)&buffer, NULL);
            if (NT_SUCCESS(status)) {
                ULONG offset = buffer[0];
                if (offset < ctx->MmioSize) {
                    WRITE_REGISTER_ULONG((PULONG)((PUCHAR)ctx->MmioBase + offset), buffer[1]);
                    bytesReturned = sizeof(ULONG);
                } else {
                    status = STATUS_ARRAY_BOUNDS_EXCEEDED;
                }
            } else {
                KdPrint(("IOCTL_PSP_WRITE_REG: WdfRequestRetrieveInputBuffer failed: 0x%08X\n", status));
            }
            break;
        }

        case IOCTL_PSP_LOAD_FW: {
            PVOID fwBuffer = NULL;
            size_t fwSize = 0;
            PVOID contiguousBuffer = NULL;
            PHYSICAL_ADDRESS physAddr;
            ULONG timeout = 0;

            // 1. Get firmware buffer from user-mode
            status = WdfRequestRetrieveInputBuffer(Request, 1, (PVOID*)&fwBuffer, &fwSize);
            if (!NT_SUCCESS(status)) {
                KdPrint(("IOCTL_PSP_LOAD_FW: Failed to retrieve input buffer: 0x%08X\n", status));
                break;
            }

            if (fwSize == 0) {
                status = STATUS_INVALID_PARAMETER;
                KdPrint(("IOCTL_PSP_LOAD_FW: Empty firmware buffer\n"));
                break;
            }

            KdPrint(("IOCTL_PSP_LOAD_FW: Received firmware buffer, size=%u bytes\n", (ULONG)fwSize));

            // 2. Allocate contiguous physical memory (below 4GB for PSP)
            PHYSICAL_ADDRESS highAddr;
            highAddr.QuadPart = 0xFFFFFFFF;

            contiguousBuffer = MmAllocateContiguousMemory(fwSize, highAddr);
            if (contiguousBuffer == NULL) {
                status = STATUS_INSUFFICIENT_RESOURCES;
                KdPrint(("IOCTL_PSP_LOAD_FW: Failed to allocate contiguous memory\n"));
                break;
            }

            // 3. Copy firmware blob to contiguous memory
            RtlCopyMemory(contiguousBuffer, fwBuffer, fwSize);

            // 4. Get physical address
            physAddr = MmGetPhysicalAddress(contiguousBuffer);
            KdPrint(("IOCTL_PSP_LOAD_FW: Firmware copied to PA=0x%llX, size=%u\n",
                physAddr.QuadPart, (ULONG)fwSize));

            // 5. Write physical address to C2PMSG_36 (data register)
            if (PSP_C2PMSG_36_OFFSET < ctx->MmioSize) {
                WRITE_REGISTER_ULONG(
                    (PULONG)((PUCHAR)ctx->MmioBase + PSP_C2PMSG_36_OFFSET),
                    (ULONG)(physAddr.QuadPart & 0xFFFFFFFF)
                );
                KdPrint(("IOCTL_PSP_LOAD_FW: Wrote PA=0x%08X to C2PMSG_36\n",
                    (ULONG)(physAddr.QuadPart & 0xFFFFFFFF)));
            } else {
                status = STATUS_DEVICE_CONFIGURATION_ERROR;
                KdPrint(("IOCTL_PSP_LOAD_FW: C2PMSG_36 offset out of bounds\n"));
                MmFreeContiguousMemory(contiguousBuffer);
                break;
            }

            // 6. Write boot command to C2PMSG_35 (command register)
            if (PSP_C2PMSG_35_OFFSET < ctx->MmioSize) {
                WRITE_REGISTER_ULONG(
                    (PULONG)((PUCHAR)ctx->MmioBase + PSP_C2PMSG_35_OFFSET),
                    0x1  // Boot command
                );
                KdPrint(("IOCTL_PSP_LOAD_FW: Wrote command 0x1 to C2PMSG_35\n"));
            } else {
                status = STATUS_DEVICE_CONFIGURATION_ERROR;
                KdPrint(("IOCTL_PSP_LOAD_FW: C2PMSG_35 offset out of bounds\n"));
                MmFreeContiguousMemory(contiguousBuffer);
                break;
            }

            // 7. Wait for status change in C2PMSG_81
            if (PSP_C2PMSG_81_OFFSET < ctx->MmioSize) {
                ULONG statusReg = 0;
                BOOLEAN success = FALSE;

                for (timeout = 0; timeout < 10000; timeout++) { // 10 seconds max
                    KeStallExecutionProcessor(1000); // 1ms per iteration

                    statusReg = READ_REGISTER_ULONG(
                        (PULONG)((PUCHAR)ctx->MmioBase + PSP_C2PMSG_81_OFFSET)
                    );

                    if (statusReg != 0) {
                        success = TRUE;
                        break;
                    }
                }

                if (success) {
                    KdPrint(("IOCTL_PSP_LOAD_FW: SUCCESS! Status=0x%08X after %u ms\n",
                        statusReg, timeout));
                    status = STATUS_SUCCESS;
                    
                    // Return status in output buffer if available
                    if (OutputBufferLength >= sizeof(ULONG)) {
                        PULONG outBuf;
                        NTSTATUS outStatus = WdfRequestRetrieveOutputBuffer(
                            Request, sizeof(ULONG), (PVOID*)&outBuf, NULL);
                        if (NT_SUCCESS(outStatus)) {
                            outBuf[0] = statusReg;
                            bytesReturned = sizeof(ULONG);
                        }
                    }
                } else {
                    KdPrint(("IOCTL_PSP_LOAD_FW: TIMEOUT waiting for C2PMSG_81\n"));
                    status = STATUS_TIMEOUT;
                }
            } else {
                status = STATUS_DEVICE_CONFIGURATION_ERROR;
                KdPrint(("IOCTL_PSP_LOAD_FW: C2PMSG_81 offset out of bounds\n"));
            }

            // Cleanup contiguous memory
            MmFreeContiguousMemory(contiguousBuffer);
            break;
        }

        default:
            status = STATUS_INVALID_DEVICE_REQUEST;
            break;
    }

    WdfRequestCompleteWithInformation(Request, status, bytesReturned);
}
