// PspDriver.c - AMD BC-250 PSP Kernel-Mode Driver (WDM)
// Pure dispatcher; core/KIQ/SMU logic lives in separate modules
#include <ntddk.h>
#include <wdm.h>

#include "PspIoctl.h"
#include "firmware_data.h"
#include "PspCore.h"
#include "PspKiq.h"
#include "PspSmu.h"

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

#define PSP_BAR0_PHYSICAL      0xFD600000ULL
#define PSP_BAR0_SIZE          0x40000

// Mailbox base macro (fallback BAR5 when BAR0 unavailable)
#define PSP_MAILBOX_BASE(devExt) (devExt->Bar0Base ? devExt->Bar0Base : devExt->MmioBase)
#define PSP_READ_MAILBOX(offset) \
    READ_REGISTER_ULONG((PULONG)((PUCHAR)PSP_MAILBOX_BASE(devExt) + (offset)))
#define PSP_WRITE_MAILBOX(offset, value) \
    WRITE_REGISTER_ULONG((PULONG)((PUCHAR)PSP_MAILBOX_BASE(devExt) + (offset)), (value))

// Device context (defined in PspIoctl.h, shared with all driver files)

DRIVER_INITIALIZE DriverEntry;
DRIVER_UNLOAD DriverUnload;
DRIVER_DISPATCH PspCreateClose;
DRIVER_DISPATCH PspDeviceControl;

// Device names (also defined in PspIoctl.h as wide strings)
#define PSP_NT_DEVICE_NAME    L"\\Device\\AmdBcPsp"
#define PSP_SYMBOLIC_LINK_NAME L"\\DosDevices\\AmdBcPsp"

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
    PVOID inputBuffer = Irp->AssociatedIrp.SystemBuffer;
    PVOID outputBuffer = Irp->AssociatedIrp.SystemBuffer;
    ULONG inputLength = irpStack->Parameters.DeviceIoControl.InputBufferLength;
    ULONG outputLength = irpStack->Parameters.DeviceIoControl.OutputBufferLength;
    ULONG ioctlCode = irpStack->Parameters.DeviceIoControl.IoControlCode;

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
        KdPrint(("IOCTL_PSP_INIT_HW (0x%08X)\n", ioctlCode));
        if (inputLength < sizeof(PSP_INIT_HW_REQUEST)) {
            status = STATUS_INVALID_PARAMETER;
            break;
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

        if (devExt->PciCfgBase == NULL) {
            ULONGLONG ecamCandidates[] = {
                0xE0000000ULL, 0xF0000000ULL, 0xC0000000ULL, 0xE000000000ULL
            };
            for (int e = 0; e < 4 && devExt->PciCfgBase == NULL; e++) {
                PHYSICAL_ADDRESS ecamPhys;
                ecamPhys.QuadPart = ecamCandidates[e];
                PVOID mapped = MmMapIoSpace(ecamPhys, 0x100000, MmNonCached);
                if (mapped) {
                    ULONG cfg0 = READ_REGISTER_ULONG((PULONG)mapped);
                    if (cfg0 != 0 && cfg0 != 0xFFFFFFFF) {
                        devExt->PciCfgBase = mapped;
                        devExt->PciCfgSize = 0x100000;
                        KdPrint(("PCI ECAM found at 0x%llX (Vendor=0x%08X)\n", ecamCandidates[e], cfg0));
                    } else {
                        MmUnmapIoSpace(mapped, 0x100000);
                    }
                }
            }
        }

        bytesReturned = sizeof(ULONG);
        if (outputLength >= sizeof(ULONG)) {
            ((PULONG)outputBuffer)[0] = (ULONG)(ULONG_PTR)devExt->MmioBase;
        }
        status = STATUS_SUCCESS;
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
        if (offset >= devExt->MmioSize || (offset + sizeof(ULONG) > devExt->MmioSize) || (offset & 0x3)) {
            KdPrint(("READ_REG: Offset 0x%X out of bounds\n", offset));
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
        status = STATUS_SUCCESS;
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
        if (offset >= devExt->MmioSize || (offset + sizeof(ULONG) > devExt->MmioSize) || (offset & 0x3)) {
            KdPrint(("WRITE_REG: Offset 0x%X out of bounds\n", offset));
            status = STATUS_ARRAY_BOUNDS_EXCEEDED;
            break;
        }
        WRITE_REGISTER_ULONG((PULONG)((PUCHAR)devExt->MmioBase + offset), value);
        bytesReturned = sizeof(ULONG);
        status = STATUS_SUCCESS;
        break;
    }

    case IOCTL_PSP_LOAD_FW:
    {
        if (inputLength == 0) {
            status = STATUS_INVALID_PARAMETER;
            break;
        }
        if (inputLength > PSP_MAX_FW_TOTAL) {
            KdPrint(("IOCTL_PSP_LOAD_FW: Firmware too large (%u > %u)\n", inputLength, PSP_MAX_FW_TOTAL));
            status = STATUS_INVALID_PARAMETER;
            break;
        }
        PspFreeFirmware(devExt);
        PHYSICAL_ADDRESS highAddr;
        highAddr.QuadPart = 0x10000000000ULL;
        devExt->FwBuffer = MmAllocateContiguousMemory(inputLength, highAddr);
        if (devExt->FwBuffer == NULL) {
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
        KdPrint(("IOCTL_PSP_LOAD_FW: Firmware loaded PA=0x%llX PA>>20=0x%08X size=%u\n",
            devExt->FwPhysical.QuadPart, devExt->FwPaShifted, devExt->FwSize));
        if (outputLength >= sizeof(ULONG)) {
            ((PULONG)outputBuffer)[0] = devExt->FwPaShifted;
            bytesReturned = sizeof(ULONG);
        }
        status = STATUS_SUCCESS;
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
        if (devExt->PciCfgBase != NULL) {
            ULONG addr = (bus * 0x100000) + (devFn * 0x1000) + (off & ~3);
            if (addr + sizeof(ULONG) <= devExt->PciCfgSize) {
                value = READ_REGISTER_ULONG((PULONG)((PUCHAR)devExt->PciCfgBase + addr));
            }
        }
        if (value == 0xFFFFFFFF) {
            ULONG slot = (devFn >> 3) & 0x1F;
            ULONG func = devFn & 7;
            HalGetBusDataByOffset(PCIConfiguration, bus, (slot << 3) | func, &value, off & ~3, sizeof(ULONG));
        }
        KdPrint(("PCI_READ: B%d.D%d.F%d off=0x%X => 0x%08X\n", bus, (devFn>>3)&0x1F, devFn&7, off, value));
        if (outputLength >= sizeof(ULONG)) {
            ((PULONG)outputBuffer)[0] = value;
            bytesReturned = sizeof(ULONG);
        }
        status = STATUS_SUCCESS;
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
        status = STATUS_SUCCESS;
        break;
    }

    case IOCTL_PSP_NBIO_UNLOCK:
    {
        ULONG beforeSig1 = READ_REGISTER_ULONG((PULONG)((PUCHAR)devExt->MmioBase + NBIO_SIG1_OFFSET));
        ULONG beforeSig2 = READ_REGISTER_ULONG((PULONG)((PUCHAR)devExt->MmioBase + NBIO_SIG2_OFFSET));
        ULONG mmhubBefore = READ_REGISTER_ULONG((PULONG)((PUCHAR)devExt->MmioBase + MMHUB_CHECK_OFFSET));
        KdPrint(("NBIO unlock: SIG1=0x%08X SIG2=0x%08X MMHUB=0x%08X\n", beforeSig1, beforeSig2, mmhubBefore));
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
        status = STATUS_NOT_IMPLEMENTED;
        break;
    }

    case IOCTL_PSP_NBIO_VIA_RING:
    {
        status = STATUS_NOT_IMPLEMENTED;
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
        info->C2PMSG_81 = PSP_READ_MAILBOX(PSP_C2PMSG_81_OFFSET);
        info->C2PMSG_35 = PSP_READ_MAILBOX(PSP_C2PMSG_35_OFFSET);
        info->C2PMSG_36 = PSP_READ_MAILBOX(PSP_C2PMSG_36_OFFSET);
        info->C2PMSG_37 = PSP_READ_MAILBOX(PSP_C2PMSG_37_OFFSET);
        info->C2PMSG_64 = PSP_READ_MAILBOX(PSP_C2PMSG_64_OFFSET);
        info->PspAlive = (info->C2PMSG_81 != 0 && info->C2PMSG_81 != 0xFFFFFFFF) ? 1 : 0;
        info->FwLoaded = (devExt->FwBuffer != NULL) ? 1 : 0;
        info->FwSize = devExt->FwSize;
        info->FwPaShifted = devExt->FwPaShifted;
        info->NbioSig1 = READ_REGISTER_ULONG((PULONG)((PUCHAR)devExt->MmioBase + NBIO_SIG1_OFFSET));
        info->NbioSig2 = READ_REGISTER_ULONG((PULONG)((PUCHAR)devExt->MmioBase + NBIO_SIG2_OFFSET));
        info->GrbmStatus = READ_REGISTER_ULONG((PULONG)((PUCHAR)devExt->MmioBase + (AMDBC250_GC_BASE + 0x2004)));
        info->MmhubCheck = READ_REGISTER_ULONG((PULONG)((PUCHAR)devExt->MmioBase + MMHUB_CHECK_OFFSET));
        info->GcCheck = READ_REGISTER_ULONG((PULONG)((PUCHAR)devExt->MmioBase + (AMDBC250_GC_BASE + 0x3000)));
        info->HdpCheck = READ_REGISTER_ULONG((PULONG)((PUCHAR)devExt->MmioBase + 0x05A0));
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
        if (g_SosFirmwareSize > PSP_MAX_FW_TOTAL) {
            KdPrint(("IOCTL_PSP_LOAD_EMBEDDED_FW: Embedded FW too large (%u > %u)\n",
                g_SosFirmwareSize, PSP_MAX_FW_TOTAL));
            status = STATUS_INVALID_PARAMETER;
            break;
        }
        PHYSICAL_ADDRESS highAddr;
        highAddr.QuadPart = 0x10000000000ULL;
        devExt->FwBuffer = MmAllocateContiguousMemory(g_SosFirmwareSize, highAddr);
        if (devExt->FwBuffer == NULL) {
            highAddr.QuadPart = 0xFFFFFFFF;
            devExt->FwBuffer = MmAllocateContiguousMemory(g_SosFirmwareSize, highAddr);
        }
        if (devExt->FwBuffer == NULL) {
            status = STATUS_INSUFFICIENT_RESOURCES;
            KdPrint(("IOCTL_PSP_LOAD_EMBEDDED_FW: allocation failed\n"));
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
        if (outputLength >= sizeof(ULONG)) {
            ((PULONG)outputBuffer)[0] = devExt->FwPaShifted;
            bytesReturned = sizeof(ULONG);
        }
        status = STATUS_SUCCESS;
        break;
    }

    case IOCTL_PSP_BOOT_SEQUENCE:
    {
        NTSTATUS stepStatus;
        ULONG results[4] = {0};
        PHYSICAL_ADDRESS highAddr;
        highAddr.QuadPart = 0x10000000000ULL;

        if (g_SysdrvFirmwareSize > PSP_MAX_FW_TOTAL) {
            KdPrint(("BOOT_SEQ: SYSDRV FW too large (%u > %u)\n", g_SysdrvFirmwareSize, PSP_MAX_FW_TOTAL));
            status = STATUS_INVALID_PARAMETER;
            break;
        }
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

        stepStatus = PspSendMailboxCommand(devExt, 0x00000004);
        results[1] = NT_SUCCESS(stepStatus) ? 1 : 0;
        KdPrint(("BOOT_SEQ: SYSDRV cmd=0x4 => %s\n", results[1] ? "SENT" : "FAIL"));
        if (!NT_SUCCESS(stepStatus)) {
            KdPrint(("BOOT_SEQ: SYSDRV failed with 0x%08X, skipping SOS\n", stepStatus));
            status = stepStatus;
            if (outputLength >= sizeof(results)) {
                RtlCopyMemory(outputBuffer, results, sizeof(results));
                bytesReturned = sizeof(results);
            }
            break;
        }

        if (g_SosFirmwareSize > PSP_MAX_FW_TOTAL) {
            KdPrint(("BOOT_SEQ: SOS FW too large (%u > %u)\n", g_SosFirmwareSize, PSP_MAX_FW_TOTAL));
            status = STATUS_INVALID_PARAMETER;
            break;
        }
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

        stepStatus = PspSendMailboxCommand(devExt, 0x00000008);
        results[2] = NT_SUCCESS(stepStatus) ? 1 : 0;
        KdPrint(("BOOT_SEQ: SOS cmd=0x8 => %s\n", results[2] ? "SENT" : "FAIL"));
        results[3] = READ_REGISTER_ULONG((PULONG)((PUCHAR)devExt->MmioBase + (AMDBC250_GC_BASE + 0x2004)));
        KdPrint(("BOOT_SEQ: SYSDRV=%d SOS=%d GRBM=0x%08X\n", results[1], results[2], results[3]));
        if (outputLength >= sizeof(results)) {
            RtlCopyMemory(outputBuffer, results, sizeof(results));
            bytesReturned = sizeof(results);
        }
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
        probe->C2PMSG_35 = PSP_READ_MAILBOX(PSP_C2PMSG_35_OFFSET);
        probe->C2PMSG_36 = PSP_READ_MAILBOX(PSP_C2PMSG_36_OFFSET);
        probe->C2PMSG_37 = PSP_READ_MAILBOX(PSP_C2PMSG_37_OFFSET);
        probe->C2PMSG_64 = PSP_READ_MAILBOX(PSP_C2PMSG_64_OFFSET);
        probe->C2PMSG_81 = PSP_READ_MAILBOX(PSP_C2PMSG_81_OFFSET);
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
        probe->RingProgOk = 0;
        probe->RingCreated = devExt->RingCreated ? 1 : 0;
        probe->NbioViaRingOk = 0;
        bytesReturned = sizeof(PSP_PROBE_INFO);
        status = STATUS_SUCCESS;
        break;
    }

    case IOCTL_PSP_RING_LOAD_IP_FW:
    {
        status = STATUS_NOT_IMPLEMENTED;
        break;
    }

    case IOCTL_PSP_GET_GPU_INFO:
    {
        if (outputLength < sizeof(PSP_GPU_INFO)) {
            status = STATUS_BUFFER_TOO_SMALL;
            break;
        }
        PSP_GPU_INFO* info = (PSP_GPU_INFO*)outputBuffer;
        RtlZeroMemory(info, sizeof(PSP_GPU_INFO));
        info->RingBufferPA = 0;
        info->FwLoaded = devExt->FwBuffer ? 1 : 0;
        info->FwCount = 0;
        info->TMRBase = g_TmrInitialized ? g_TmrPhysical.QuadPart : 0;
        info->TMSSize = g_TmrSize;
        info->GfxVersion = 10;
        info->C2pmsg64 = PSP_READ_MAILBOX(PSP_C2PMSG_64_OFFSET);
        info->C2pmsg81 = PSP_READ_MAILBOX(PSP_C2PMSG_81_OFFSET);
        info->TmrInitialized = g_TmrInitialized ? 1 : 0;
        bytesReturned = sizeof(PSP_GPU_INFO);
        status = STATUS_SUCCESS;
        break;
    }

    case IOCTL_PSP_REG_PROG:
    {
        if (inputLength < sizeof(PSP_REG_PROG_REQUEST)) {
            status = STATUS_INVALID_PARAMETER;
            break;
        }
        PSP_REG_PROG_REQUEST* req = (PSP_REG_PROG_REQUEST*)inputBuffer;
        ULONG regId = req->RegId;
        ULONG regVal = req->RegValue;
        if (outputLength < sizeof(ULONG)) {
            status = STATUS_BUFFER_TOO_SMALL;
            break;
        }
        WRITE_REGISTER_ULONG((PULONG)((PUCHAR)devExt->MmioBase + regId), regVal);
        ((PULONG)outputBuffer)[0] = regVal;
        bytesReturned = sizeof(ULONG);
        status = STATUS_SUCCESS;
        break;
    }

    case IOCTL_PSP_AUTOLOAD_RLC:
    {
        status = STATUS_NOT_IMPLEMENTED;
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
        status = STATUS_NOT_IMPLEMENTED;
        break;
    }

    case IOCTL_PSP_KIQ_LOAD_FW:
    {
        status = STATUS_NOT_IMPLEMENTED;
        break;
    }

    case IOCTL_PSP_KIQ_GET_STATUS:
    {
        if (outputLength < sizeof(PSP_KIQ_STATUS)) {
            status = STATUS_BUFFER_TOO_SMALL;
            break;
        }
        PSP_KIQ_STATUS* ks = (PSP_KIQ_STATUS*)outputBuffer;
        ks->RingInitialized = g_KiqRingInitialized ? 1 : 0;
        ks->RingWptr = g_KiqRingWptr;
        ks->RingSize = g_KiqRingSize;
        ks->RingPA = g_KiqRingPa.LowPart;
        bytesReturned = sizeof(PSP_KIQ_STATUS);
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
