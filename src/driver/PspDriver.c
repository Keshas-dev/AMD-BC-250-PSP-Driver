// PspDriver.c - AMD BC-250 PSP Kernel-Mode Driver (WDM)
// Pure dispatcher; core/KIQ/SMU logic lives in separate modules
#include <ntddk.h>
#include <wdm.h>

#include "PspIoctl.h"
#include "firmware_data.h"
#include "PspCore.h"
#include "PspKiq.h"
#include "PspSmu.h"

#pragma data_seg(".Shared")
PVOID g_Bar5Mapping = NULL;
SIZE_T g_Bar5Size = 0;
KSPIN_LOCK g_Bar5MappingLock;
BOOLEAN g_GpuProxyAvailable = FALSE;
HANDLE g_GpuDriverHandle = NULL;
#pragma data_seg()

#define GPU_BAR5_PHYSICAL      0xFE800000ULL
#define GPU_BAR5_SIZE          0x80000ULL

#define PSP_GPU_DRIVER_NT_NAME    L"\\Device\\AMDBC250DreamV43"
#define PSP_GPU_DRIVER_SYM_NAME   L"\\DosDevices\\AMDBC250DreamV43"
#define PSP_IOCTL_READ_REG_PROXY  CTL_CODE(FILE_DEVICE_UNKNOWN, 0x900, METHOD_BUFFERED, FILE_ANY_ACCESS)

/* PCIConfiguration comes from ntddk.h BUS_DATA_TYPE enum (= 4).
 * FIX 2026-09-23: old `#define PCIConfiguration 0` overrode it with 0 (Cmos),
 * so IOCTL_PSP_PCI_READ/WRITE hit CMOS bus data, never PCI config space. */

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

/* Mailbox lives on GPU BAR5 MP0 — always via GPU proxy (0x900/0x901), never dual-map. */
#define PSP_READ_MAILBOX(offset)    PspGpuProxyReadRegister((offset))
#define PSP_WRITE_MAILBOX(offset, value) \
    (void)PspGpuProxyWriteRegister((offset), (value))

// Device context (defined in PspIoctl.h, shared with all driver files)

DRIVER_INITIALIZE DriverEntry;
DRIVER_UNLOAD DriverUnload;
DRIVER_DISPATCH PspCreateClose;
DRIVER_DISPATCH PspDeviceControl;

NTSTATUS PspDoBootSequence(PDEVICE_EXTENSION devExt);

// Device names (also defined in PspIoctl.h as wide strings)
#define PSP_NT_DEVICE_NAME    L"\\Device\\AmdBcPsp"
#define PSP_SYMBOLIC_LINK_NAME L"\\DosDevices\\AmdBcPsp"

NTSTATUS PspDoBootSequence(PDEVICE_EXTENSION devExt)
{
    NTSTATUS stepStatus;
    PHYSICAL_ADDRESS highAddr;
    PUCHAR fileData = NULL;
    ULONG fileSize = 0;
    highAddr.QuadPart = 0x10000000000ULL;

    /* FIX 2026-08-21: with deferred BAR mapping, MmioBase/GpuMmioBase are
     * NULL at DriverEntry. Mailbox writes to a NULL base would bugcheck.
     * Skip the boot sequence entirely — SOS is already alive (loaded by
     * VBIOS) and INIT_HW maps the real BARs for later use.
     * 2026-09-23: also skip when only Bar0Base is up — requires proxy. */
    if (devExt->MmioBase == NULL && devExt->Bar0Base == NULL && !g_GpuProxyAvailable) {
        KdPrint(("BOOT_SEQ: skipped (no MMIO mapped yet; SOS handled by VBIOS)\n"));
        return STATUS_SUCCESS;
    }

    /* --- Load SYSDRV firmware --- */
    /* Try file first (\SystemRoot\System32\drivers\bc-250\Sysdrv.bin) */
    fileData = NULL; fileSize = 0;
    stepStatus = PspLoadFirmwareFromFile(
        L"\\SystemRoot\\System32\\drivers\\bc-250\\Sysdrv.bin",
        &fileData, &fileSize);
    if (NT_SUCCESS(stepStatus)) {
        KdPrint(("BOOT_SEQ: SYSDRV from file (%u bytes)\n", fileSize));
    } else if (g_SysdrvFirmwareSize > PSP_MAX_FW_TOTAL) {
        KdPrint(("BOOT_SEQ: SYSDRV FW too large\n"));
        if (fileData != NULL) ExFreePoolWithTag(fileData, 'fw');
        return STATUS_INVALID_PARAMETER;
    } else {
        fileSize = g_SysdrvFirmwareSize;
        KdPrint(("BOOT_SEQ: SYSDRV from embedded (%u bytes)\n", fileSize));
    }

    PspFreeFirmware(devExt);
    devExt->FwBuffer = MmAllocateContiguousMemory(fileSize, highAddr);
    if (devExt->FwBuffer == NULL) {
        highAddr.QuadPart = 0xFFFFFFFF;
        devExt->FwBuffer = MmAllocateContiguousMemory(fileSize, highAddr);
    }
    if (devExt->FwBuffer == NULL) {
        KdPrint(("BOOT_SEQ: SYSDRV alloc failed\n"));
        if (fileData != NULL) ExFreePoolWithTag(fileData, 'fw');
        return STATUS_INSUFFICIENT_RESOURCES;
    }

    if (fileData != NULL) {
        RtlCopyMemory(devExt->FwBuffer, fileData, fileSize);
        ExFreePoolWithTag(fileData, 'fw'); fileData = NULL;
    } else {
        RtlCopyMemory(devExt->FwBuffer, (PVOID)g_SysdrvFirmwareData, fileSize);
    }
    devExt->FwSize = fileSize;
    devExt->FwPhysical = MmGetPhysicalAddress(devExt->FwBuffer);
    devExt->FwPaShifted = (ULONG)(devExt->FwPhysical.QuadPart >> 20);
    KdPrint(("BOOT_SEQ: SYSDRV PA>>20=0x%08X\n", devExt->FwPaShifted));

    stepStatus = PspSendMailboxCommand(devExt, 0x00000004);
    if (!NT_SUCCESS(stepStatus)) {
        KdPrint(("BOOT_SEQ: SYSDRV failed 0x%08X\n", stepStatus));
        PspFreeFirmware(devExt);
        return stepStatus;
    }

    /* --- Load SOS firmware --- */
    fileData = NULL; fileSize = 0;
    stepStatus = PspLoadFirmwareFromFile(
        L"\\SystemRoot\\System32\\drivers\\bc-250\\Sos.bin",
        &fileData, &fileSize);
    if (NT_SUCCESS(stepStatus)) {
        KdPrint(("BOOT_SEQ: SOS from file (%u bytes)\n", fileSize));
    } else if (g_SosFirmwareSize > PSP_MAX_FW_TOTAL) {
        KdPrint(("BOOT_SEQ: SOS FW too large\n"));
        if (fileData != NULL) ExFreePoolWithTag(fileData, 'fw');
        PspFreeFirmware(devExt);
        return STATUS_INVALID_PARAMETER;
    } else {
        fileSize = g_SosFirmwareSize;
        KdPrint(("BOOT_SEQ: SOS from embedded (%u bytes)\n", fileSize));
    }

    PspFreeFirmware(devExt);
    devExt->FwBuffer = MmAllocateContiguousMemory(fileSize, highAddr);
    if (devExt->FwBuffer == NULL) {
        highAddr.QuadPart = 0xFFFFFFFF;
        devExt->FwBuffer = MmAllocateContiguousMemory(fileSize, highAddr);
    }
    if (devExt->FwBuffer == NULL) {
        KdPrint(("BOOT_SEQ: SOS alloc failed\n"));
        if (fileData != NULL) ExFreePoolWithTag(fileData, 'fw');
        return STATUS_INSUFFICIENT_RESOURCES;
    }
    RtlZeroMemory(devExt->FwBuffer, fileSize);
    devExt->FwSize = fileSize;

    if (fileData != NULL) {
        RtlCopyMemory(devExt->FwBuffer, fileData, fileSize);
        ExFreePoolWithTag(fileData, 'fw'); fileData = NULL;
    } else {
        RtlCopyMemory(devExt->FwBuffer, (PVOID)g_SosFirmwareData, g_SosFirmwareSize);
    }
    devExt->FwPhysical = MmGetPhysicalAddress(devExt->FwBuffer);
    devExt->FwPaShifted = (ULONG)(devExt->FwPhysical.QuadPart >> 20);
    KdPrint(("BOOT_SEQ: SOS PA>>20=0x%08X\n", devExt->FwPaShifted));

    stepStatus = PspSendMailboxCommand(devExt, 0x00000008);
    if (!NT_SUCCESS(stepStatus)) {
        KdPrint(("BOOT_SEQ: SOS failed 0x%08X\n", stepStatus));
        PspFreeFirmware(devExt);
        return stepStatus;
    }

    /* --- NBIO unlock --- */
    (void)PspGpuProxyWriteRegister(NBIO_SIG1_OFFSET, NBIO_SIG1_VALUE);
    (void)PspGpuProxyWriteRegister(NBIO_SIG2_OFFSET, NBIO_SIG2_VALUE);
    KeStallExecutionProcessor(1000);
    KdPrint(("BOOT_SEQ: NBIO unlock via GPU proxy written\n"));

    ULONG grbm = PspGpuProxyReadRegister(AMDBC250_GC_BASE + 0x2000);
    KdPrint(("BOOT_SEQ: GRBM_STATUS=0x%08X (GPU proxy)\n", grbm));
    return STATUS_SUCCESS;
}

NTSTATUS PspSendSmcBoot(PDEVICE_EXTENSION devExt)
{
    NTSTATUS status;
    PVOID cmdBufVa = NULL, tocBufVa = NULL;
    PHYSICAL_ADDRESS cmdBufPa = {0}, tocBufPa = {0}, highAddr;
    ULONG timeout, cmdReg, tocSize, fwOffset, totalSize;

    highAddr.QuadPart = 0x10000000000ULL;

    if (g_SmuFirmwareSize == 0 || g_SmuFirmwareSize > 0x100000) {
        KdPrint(("SMC_BOOT: Invalid SMU firmware size=%u\n", g_SmuFirmwareSize));
        return STATUS_INVALID_PARAMETER;
    }
    if (!g_GpuProxyAvailable) {
        NTSTATUS proxyStatus = PspGpuProxyInit(devExt);
        if (!NT_SUCCESS(proxyStatus)) {
            KdPrint(("SMC_BOOT: GPU proxy init failed 0x%08X\n", proxyStatus));
            return STATUS_DEVICE_NOT_READY;
        }
    }

    /* Allocate contiguous TOC buffer: toc_header(12) + toc_entry(16) + firmware */
    tocSize = 28;
    fwOffset = (tocSize + 0x7F) & ~0x7F;
    totalSize = fwOffset + g_SmuFirmwareSize;

    tocBufVa = MmAllocateContiguousMemory(totalSize, highAddr);
    if (tocBufVa == NULL) {
        highAddr.QuadPart = 0xFFFFFFFF;
        tocBufVa = MmAllocateContiguousMemory(totalSize, highAddr);
    }
    if (tocBufVa == NULL) {
        KdPrint(("SMC_BOOT: TOC alloc %u failed\n", totalSize));
        return STATUS_INSUFFICIENT_RESOURCES;
    }
    RtlZeroMemory(tocBufVa, totalSize);
    tocBufPa = MmGetPhysicalAddress(tocBufVa);

    /* Build TOC structure (psp_gfx_toc_header + psp_gfx_toc_entry format) */
    PUCHAR toc = (PUCHAR)tocBufVa;
    PHYSICAL_ADDRESS fwPa;
    fwPa.QuadPart = tocBufPa.QuadPart + fwOffset;

    /* psp_gfx_toc_header: id(4) + entry_count(4) + total_size(4) */
    *((PULONG)(toc + 0)) = 0;      /* toc_id (always 0) */
    *((PULONG)(toc + 4)) = 1;      /* entry_count */
    *((PULONG)(toc + 8)) = totalSize;  /* total_size */

    /* psp_gfx_toc_entry: fw_type(4) + fw_size(4) + fw_pa(8) + reserved(0) */
    *((PULONG)(toc + 12)) = 7;                    /* fw_type = PSP_GFX_FW_TYPE_PSP_SMC */
    *((PULONG)(toc + 16)) = g_SmuFirmwareSize;
    *((PULONG)(toc + 20)) = (ULONG)(fwPa.QuadPart & 0xFFFFFFFF);
    *((PULONG)(toc + 24)) = (ULONG)(fwPa.QuadPart >> 32);

    /* Copy SMU firmware */
    RtlCopyMemory(toc + fwOffset, (PVOID)g_SmuFirmwareData, g_SmuFirmwareSize);

    KdPrint(("SMC_BOOT: TOC PA=0x%llX fwPA=0x%llX total=%u\n",
             tocBufPa.QuadPart, fwPa.QuadPart, totalSize));

    /* Allocate GFX ring command buffer (matching PSP_CMD_BUFFER format) */
    cmdBufVa = MmAllocateContiguousMemory(64, highAddr);
    if (cmdBufVa == NULL) {
        highAddr.QuadPart = 0xFFFFFFFF;
        cmdBufVa = MmAllocateContiguousMemory(64, highAddr);
    }
    if (cmdBufVa == NULL) {
        MmFreeContiguousMemory(tocBufVa);
        KdPrint(("SMC_BOOT: CMD alloc failed\n"));
        return STATUS_INSUFFICIENT_RESOURCES;
    }
    RtlZeroMemory(cmdBufVa, 64);
    cmdBufPa = MmGetPhysicalAddress(cmdBufVa);

    /* Build GFX ring command buffer (matching PspLoadIpFwViaMailbox format):
     * Offset  0: buf_size
     * Offset  4: body_size (dwords of params after reserved header)
     * Offset  8: cmd_id
     * Offset 12-24: reserved (4 dwords)
     * Offset 28: param 0 (toc_pa_lo)
     * Offset 32: param 1 (toc_pa_hi)
     * Offset 36: param 2 (toc_size)  */
    PULONG cmd = (PULONG)cmdBufVa;
    cmd[0] = 64;                            /* total buf_size */
    cmd[1] = 3;                             /* body_size = 3 (toc_pa_lo, toc_pa_hi, toc_size) */
    cmd[2] = 0x20;                          /* cmd_id = GFX_CMD_ID_LOAD_TOC */
    cmd[3] = 0;                             /* reserved */
    cmd[4] = 0;                             /* reserved */
    cmd[5] = 0;                             /* reserved */
    cmd[6] = 0;                             /* reserved */
    cmd[7] = (ULONG)(tocBufPa.QuadPart & 0xFFFFFFFF);   /* toc_pa_lo */
    cmd[8] = (ULONG)(tocBufPa.QuadPart >> 32);           /* toc_pa_hi */
    cmd[9] = totalSize;                     /* toc_size */

    KdPrint(("SMC_BOOT: CMD PA=0x%llX\n", cmdBufPa.QuadPart));

    /* Init proxy BEFORE lock — ZwCreateFile must not run under CommandLock */
    if (!g_GpuProxyAvailable) {
        (void)PspGpuProxyInit(devExt);
    }
    if (!g_GpuProxyAvailable) {
        KdPrint(("SMC_BOOT: GPU proxy unavailable — cannot send TOC cmd\n"));
        MmFreeContiguousMemory(cmdBufVa);
        MmFreeContiguousMemory(tocBufVa);
        return STATUS_DEVICE_NOT_READY;
    }

    /* KMUTEX: proxy Zw needs PASSIVE — not spinlock */
    (void)KeWaitForSingleObject(&devExt->CommandLock, Executive, KernelMode, FALSE, NULL);
    (void)PspGpuProxyWriteRegister(PSP_C2PMSG_36_OFFSET,
        (ULONG)(cmdBufPa.QuadPart & 0xFFFFFFFF));
    (void)PspGpuProxyWriteRegister(PSP_C2PMSG_37_OFFSET,
        (ULONG)(cmdBufPa.QuadPart >> 32));
    (void)PspGpuProxyWriteRegister(PSP_C2PMSG_35_OFFSET, 0x20);
    KeReleaseMutex(&devExt->CommandLock, FALSE);

    /* Poll for completion via proxy */
    for (timeout = 0; timeout < 3000; timeout++) {
        KeStallExecutionProcessor(1000);
        cmdReg = PspGpuProxyReadRegister(PSP_C2PMSG_35_OFFSET);
        if (cmdReg == 0) break;
    }

    /* Check results — SMU register reads must use SMN via NBIO bridge (BAR5+0x38/0x3C) */
    ULONG c2p81 = 0;
    ULONG smu66 = 0, smu82 = 0, smu90 = 0;
    if (g_GpuProxyAvailable) {
        c2p81 = PspGpuProxyReadRegister(PSP_C2PMSG_81_OFFSET);
        (void)PspGpuProxyWriteRegister(0x38, 0x03B10A08);
        KeStallExecutionProcessor(1);
        smu66 = PspGpuProxyReadRegister(0x3C);
        (void)PspGpuProxyWriteRegister(0x38, 0x03B10A48);
        KeStallExecutionProcessor(1);
        smu82 = PspGpuProxyReadRegister(0x3C);
        (void)PspGpuProxyWriteRegister(0x38, 0x03B10A68);
        KeStallExecutionProcessor(1);
        smu90 = PspGpuProxyReadRegister(0x3C);
    }

    KdPrint(("SMC_BOOT: done timeout=%u C2PMSG_35=0x%08X C2PMSG_81=0x%08X\n", timeout, cmdReg, c2p81));
    KdPrint(("SMC_BOOT: SMU [66]=0x%08X (SMN) [82]=0x%08X [90]=0x%08X\n", smu66, smu82, smu90));

    MmFreeContiguousMemory(cmdBufVa);
    MmFreeContiguousMemory(tocBufVa);

    if (timeout >= 3000) {
        KdPrint(("SMC_BOOT: TIMEOUT\n"));
        return STATUS_TIMEOUT;
    }

    if (smu66 != 0 || smu82 != 0 || smu90 != 0) {
        KdPrint(("SMC_BOOT: SMU ALIVE!\n"));
        return STATUS_SUCCESS;
    }

    KdPrint(("SMC_BOOT: SMU still dead\n"));
    return STATUS_DEVICE_NOT_READY;
}

NTSTATUS DriverEntry(_In_ PDRIVER_OBJECT DriverObject, _In_ PUNICODE_STRING RegistryPath)
{
    NTSTATUS status = STATUS_SUCCESS;
    PDEVICE_OBJECT deviceObject = NULL;
    UNICODE_STRING deviceName;
    UNICODE_STRING symLinkName;

    UNREFERENCED_PARAMETER(RegistryPath);

    KdPrint(("=== AMD BC-250 PSP Driver v2.0: DriverEntry ===\n"));

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
        KeInitializeMutex(&devExt->CommandLock, 0);
    KeInitializeSpinLock(&g_Bar5MappingLock);

    /* FIX 2026-08-21: do NOT auto-map the stale hardcoded 0xFD600000.
     * BIOS reassigns BARs every boot, so that VA pointed at random memory
     * (all reads FF / garbage). Bar0Base now starts NULL and is mapped via
     * INIT_HW with the REAL BAR address once discovered. */
    devExt->Bar0Base = NULL;
    devExt->Bar0Size = 0;
    devExt->MmioBase = NULL;
    devExt->MmioSize = 0;
    devExt->GpuMmioBase = NULL;
    devExt->GpuMmioSize = 0;
    devExt->PciCfgBase = NULL;
    devExt->PciCfgSize = 0;

    KdPrint(("PSP Driver: Bar0 deferred (map via INIT_HW with real BAR)\n"));

    status = PspDoBootSequence(devExt);
    if (!NT_SUCCESS(status)) {
        KdPrint(("PSP Driver: Boot sequence failed 0x%08X\n", status));
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

    PspFreeTmr();
    PspKiqCleanup();
    PspFreeFirmware(devExt);

    /* Clean up GPU proxy state */
    if (g_GpuDriverHandle != NULL) {
        ZwClose(g_GpuDriverHandle);
        g_GpuDriverHandle = NULL;
    }
    g_GpuProxyAvailable = FALSE;
    {
        KIRQL irql;
        KeAcquireSpinLock(&g_Bar5MappingLock, &irql);
        g_Bar5Mapping = NULL;
        g_Bar5Size = 0;
        KeReleaseSpinLock(&g_Bar5MappingLock, irql);
    }

    if (devExt->PciCfgBase != NULL) {
        MmUnmapIoSpace(devExt->PciCfgBase, devExt->PciCfgSize);
        devExt->PciCfgBase = NULL;
    }

    /* FIX Code Reviewer 2026-09-23: alias-aware unmap — after successful map
     * Bar0Base == MmioBase; nulling Bar0Base first broke the != guard and
     * double-unmapped the same VA on unload (sc stop / uninstall path). */
    {
        PVOID oldBar0 = devExt->Bar0Base;
        PVOID oldMmio = devExt->MmioBase;
        PVOID oldGpu = devExt->GpuMmioBase;
        if (oldBar0 != NULL) {
            if (oldMmio == oldBar0) { oldMmio = NULL; devExt->MmioBase = NULL; }
            if (oldGpu == oldBar0)  { oldGpu = NULL;  devExt->GpuMmioBase = NULL; }
            MmUnmapIoSpace(oldBar0, devExt->Bar0Size);
            devExt->Bar0Base = NULL;
            devExt->Bar0Size = 0;
        }
        if (oldMmio != NULL) {
            MmUnmapIoSpace(oldMmio, devExt->MmioSize);
            devExt->MmioBase = NULL;
            devExt->MmioSize = 0;
        }
        if (oldGpu != NULL) {
            MmUnmapIoSpace(oldGpu, devExt->GpuMmioSize);
            devExt->GpuMmioBase = NULL;
            devExt->GpuMmioSize = 0;
        }
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

    /* Gate on PSP BAR0 only — GpuMmioBase is intentionally never mapped (proxy-only).
     * FIX 2026-09-23: serialize auto-init — concurrent first IOCTLs raced in
     * PspAutoInitialize (double MmMapIoSpace / double proxy open → hang). */
    if (devExt->Bar0Base == NULL && devExt->MmioBase == NULL) {
        KdPrint(("PspDeviceControl: no PSP MMIO, calling auto-init\n"));
        (void)KeWaitForSingleObject(&devExt->CommandLock, Executive, KernelMode, FALSE, NULL);
        NTSTATUS autoStatus = STATUS_SUCCESS;
        if (devExt->Bar0Base == NULL && devExt->MmioBase == NULL) {
            autoStatus = PspAutoInitialize(devExt);
        }
        KeReleaseMutex(&devExt->CommandLock, FALSE);
        KdPrint(("PspDeviceControl: auto-init returned 0x%08X Bar0=%p Gpu=%p proxy=%d\n",
            autoStatus, devExt->Bar0Base, devExt->GpuMmioBase, (int)g_GpuProxyAvailable));
            if (!NT_SUCCESS(autoStatus)) {
                KdPrint(("MMIO not initialized (auto-init failed: 0x%08X)\n", autoStatus));
                Irp->IoStatus.Status = STATUS_DEVICE_NOT_READY;
                Irp->IoStatus.Information = 0;
                IoCompleteRequest(Irp, IO_NO_INCREMENT);
                return STATUS_DEVICE_NOT_READY;
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

        /* FIX 2026-09-23: serialize INIT_HW remap with auto-init gate —
         * concurrent INIT_HW + first-IOCTL auto-init could double-map Bar0
         * or unmap a VA another thread just published. KMUTEX recursive. */
        (void)KeWaitForSingleObject(&devExt->CommandLock, Executive, KernelMode, FALSE, NULL);

        /* FIX 2026-08-21: was `== PSP_BAR0_PHYSICAL` (hardcoded 0xFD600000,
         * stale — BIOS reassigns BARs per boot). Now ANY address that is not
         * the GPU BAR5 maps as PSP BAR0, so the real CPU-PSP BAR can be
         * passed at runtime once discovered. */
        /* FIX Code Reviewer 2026-09-23: on intentional remap, unmap old VA
         * (when it was distinct) then set BOTH Bar0Base and MmioBase together —
         * old path left MmioBase pointing at the previous PA after re-INIT. */
        if (physAddr.QuadPart != (LONGLONG)GPU_BAR5_PHYSICAL) {
            PVOID oldBar0 = devExt->Bar0Base;
            ULONG oldBar0Size = devExt->Bar0Size;
            PVOID oldMmio = devExt->MmioBase;
            ULONG oldMmioSize = devExt->MmioSize;
            if (oldBar0 != NULL) {
                MmUnmapIoSpace(oldBar0, oldBar0Size);
            }
            if (oldMmio != NULL && oldMmio != oldBar0) {
                MmUnmapIoSpace(oldMmio, oldMmioSize);
            }
            devExt->Bar0Base = MmMapIoSpace(physAddr, size, MmNonCached);
            devExt->Bar0Size = size;
            if (devExt->Bar0Base == NULL) {
                /* FIX Code Reviewer 2026-09-23: clear Mmio too — old maps
                 * already unmapped above; leave all-NULL for auto-init retry. */
                devExt->Bar0Size = 0;
                devExt->MmioBase = NULL;
                devExt->MmioSize = 0;
                status = STATUS_INSUFFICIENT_RESOURCES;
                KdPrint(("INIT_HW: PSP BAR0 map failed at 0x%llX\n", physAddr.QuadPart));
                KeReleaseMutex(&devExt->CommandLock, FALSE);
                break;
            }
            devExt->MmioBase = devExt->Bar0Base;
            devExt->MmioSize = size;
            KdPrint(("INIT_HW: PSP BAR0 mapped at 0x%llX VA=%p size=%u\n",
                physAddr.QuadPart, devExt->Bar0Base, size));
        } else {
            /* GPU BAR5 PA (0xFE800000): NEVER dual-map — atikmdag owns it (0x1E BSOD).
             * GPU register access = GPU proxy IOCTL 0x900/0x901 only. */
            if (devExt->GpuMmioBase != NULL) {
                KIRQL irql;
                KeAcquireSpinLock(&g_Bar5MappingLock, &irql);
                g_Bar5Mapping = NULL;
                g_Bar5Size = 0;
                KeReleaseSpinLock(&g_Bar5MappingLock, irql);
                MmUnmapIoSpace(devExt->GpuMmioBase, devExt->GpuMmioSize);
                devExt->GpuMmioBase = NULL;
                devExt->GpuMmioSize = 0;
            }
            if (devExt->MmioBase == NULL) {
                devExt->MmioBase = devExt->Bar0Base;
                devExt->MmioSize = devExt->Bar0Size;
            }
            {
                /* PspGpuProxyInit takes CommandLock recursively — OK */
                NTSTATUS proxyStatus = PspGpuProxyInit(devExt);
                KdPrint(("INIT_HW: GPU BAR5 dual-map forbidden, proxy=0x%08X handle=%p avail=%d\n",
                    proxyStatus, g_GpuDriverHandle, (int)g_GpuProxyAvailable));
                if (!NT_SUCCESS(proxyStatus)) {
                    status = proxyStatus;
                    KeReleaseMutex(&devExt->CommandLock, FALSE);
                    break;
                }
            }
        }

        if (devExt->PciCfgBase == NULL) {
            /* FIX 2026-08-21: removed blind ECAM candidate mapping
             * (0xE0000000/0xC0000000 etc). 0xC0000000 is the GPU's own BAR0
             * (VRAM aperture) — the old "validation" accepted framebuffer
             * garbage and every PCI_READ then returned junk, which also
             * blocked the HalGetBusDataByOffset fallback. With PciCfgBase
             * left NULL, PCI_READ uses HalGetBusDataByOffset (correct). */
            devExt->PciCfgBase = NULL;
        }

        if (outputLength >= sizeof(ULONG)) {
            /* Proxy-only: GpuMmioBase stays 0; 1 = proxy ready, 0 = not */
            ((PULONG)outputBuffer)[0] = g_GpuProxyAvailable ? 1U : 0U;
            bytesReturned = sizeof(ULONG);
        }
        status = STATUS_SUCCESS;
        KeReleaseMutex(&devExt->CommandLock, FALSE);
        break;
    }

    case IOCTL_PSP_READ_REG:
    {
        KdPrint(("READ_REG: proxy=%d GpuMmio=%p\n", (int)g_GpuProxyAvailable, devExt->GpuMmioBase));
        if (inputLength < sizeof(ULONG)) {
            status = STATUS_INVALID_PARAMETER;
            break;
        }
        ULONG offset = ((PULONG)inputBuffer)[0];

        /* CPU-PSP BAR0 native window (pa_v1 platform mailbox + CCP regs).
         * Offsets 0x10000-0x10FFF: C2PMSG_28/29/30 (0x10570/74/78), inten/intsts
         * (0x10690/64), bootloader (0x109ec), feature (0x109fc), doorbell
         * (0x10a24/a40). These LIVE on Bar0Base (1022:143E BAR0 @ fe700000),
         * NOT on GPU BAR5 — must not go through GPU proxy. */
        if (devExt->Bar0Base && offset >= 0x10000 && offset < 0x11000 &&
            (offset + 4) <= devExt->Bar0Size) {
            if (outputLength < sizeof(ULONG)) {
                status = STATUS_BUFFER_TOO_SMALL;
                break;
            }
            ULONG value = READ_REGISTER_ULONG((PULONG)((PUCHAR)devExt->Bar0Base + offset));
            ((PULONG)outputBuffer)[0] = value;
            bytesReturned = sizeof(ULONG);
            status = STATUS_SUCCESS;
            KdPrint(("READ_REG: BAR0 pa_v1 off=0x%X val=0x%08X\n", offset, value));
            break;
        }

        /* NBIO SIG window: proxy ONLY (BAR5 owner = GPU).
         * FIX Code Reviewer 2026-09-23: removed Bar0 fallback for 0xC000-0xC1FF —
         * those are GPU NBIO offsets; writing them into PSP BAR0 hits live PSP regs. */
        if (offset >= 0xC000 && offset < 0xC200) {
            if (outputLength < sizeof(ULONG)) {
                status = STATUS_BUFFER_TOO_SMALL;
                break;
            }
            if (!g_GpuProxyAvailable) {
                NTSTATUS proxyStatus = PspGpuProxyInit(devExt);
                if (!NT_SUCCESS(proxyStatus)) {
                    KdPrint(("READ_REG: NBIO proxy init failed: 0x%08X\n", proxyStatus));
                    status = STATUS_DEVICE_NOT_READY;
                    break;
                }
            }
            /* FIX Code Reviewer 2026-09-23: 0xFFFFFFFF = proxy-dead sentinel
             * (real SIGs are 0xFEDCBAEF/0xFEDCBADF). Treat as not-ready. */
            ULONG nbioValue = PspGpuProxyReadRegister(offset);
            if (nbioValue == 0xFFFFFFFF) {
                KdPrint(("READ_REG: NBIO proxy dead (0xFFFFFFFF)\n"));
                status = STATUS_DEVICE_NOT_READY;
                break;
            }
            ((PULONG)outputBuffer)[0] = nbioValue;
            bytesReturned = sizeof(ULONG);
            status = STATUS_SUCCESS;
            break;
        }

        /* All other GPU regs: proxy only (GpuMmioBase intentionally never mapped). */
        {
            if (!g_GpuProxyAvailable) {
                NTSTATUS proxyStatus = PspGpuProxyInit(devExt);
                if (!NT_SUCCESS(proxyStatus)) {
                    KdPrint(("READ_REG: GPU proxy init failed: 0x%08X\n", proxyStatus));
                    status = STATUS_DEVICE_NOT_READY;
                    break;
                }
            }
            ULONG value = PspGpuProxyReadRegister(offset);
            KdPrint(("READ_REG: proxy offset=0x%X value=0x%08X\n", offset, value));
            if (value == 0xFFFFFFFF) {
                status = STATUS_DEVICE_NOT_READY;
                break;
            }
            if (outputLength < sizeof(ULONG)) {
                status = STATUS_BUFFER_TOO_SMALL;
                break;
            }
            ((PULONG)outputBuffer)[0] = value;
            bytesReturned = sizeof(ULONG);
            status = STATUS_SUCCESS;
        }
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

        /* CPU-PSP BAR0 native window (pa_v1) — write via Bar0Base, never GPU proxy. */
        if (devExt->Bar0Base && offset >= 0x10000 && offset < 0x11000 &&
            (offset + 4) <= devExt->Bar0Size) {
            WRITE_REGISTER_ULONG((PULONG)((PUCHAR)devExt->Bar0Base + offset), value);
            KdPrint(("WRITE_REG: BAR0 pa_v1 off=0x%X val=0x%08X\n", offset, value));
            status = STATUS_SUCCESS;
            break;
        }

        /* NBIO SIG + all GPU regs: proxy only — NO Bar0 fallback.
         * FIX Code Reviewer 2026-09-23: old Bar0 write for 0xC000-0xC1FF put
         * GPU NBIO signature values into live PSP BAR0 registers (wrong window). */
        if (!g_GpuProxyAvailable) {
            NTSTATUS proxyStatus = PspGpuProxyInit(devExt);
            if (!NT_SUCCESS(proxyStatus)) {
                KdPrint(("WRITE_REG: GPU proxy init failed: 0x%08X\n", proxyStatus));
                status = STATUS_DEVICE_NOT_READY;
                break;
            }
        }
        if (!PspGpuProxyWriteRegister(offset, value)) {
            KdPrint(("WRITE_REG: GPU proxy write failed offset=0x%X\n", offset));
            status = STATUS_DEVICE_NOT_READY;
        } else {
            status = STATUS_SUCCESS;
        }
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
            ULONG64 addr64 = ((ULONG64)bus * 0x100000) + ((ULONG64)devFn * 0x1000) + (off & ~3);
            if (addr64 + sizeof(ULONG) <= devExt->PciCfgSize) {
                value = READ_REGISTER_ULONG((PULONG)((PUCHAR)devExt->PciCfgBase + (ULONG)addr64));
            }
        }
        if (value == 0xFFFFFFFF) {
            /* Input devFn = Linux-style (device << 3) | function.
             * HalGetBusData needs PCI_SLOT_NUMBER = (function << 5) | device. */
            ULONG device = (devFn >> 3) & 0x1F;
            ULONG func = devFn & 7;
            ULONG slotNumber = (func << 5) | device;
            HalGetBusDataByOffset(PCIConfiguration, bus, slotNumber, &value, off & ~3, sizeof(ULONG));
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
            /* Same encoding fix as PCI_READ: PCI_SLOT_NUMBER = (function << 5) | device */
            ULONG device = (devFn >> 3) & 0x1F;
            ULONG func = devFn & 7;
            ULONG slotNumber = (func << 5) | device;
            HalSetBusDataByOffset(PCIConfiguration, bus, slotNumber, &value, off & ~3, sizeof(ULONG));
        }
        KdPrint(("PCI_WRITE: B%d.D%d.F%d off=0x%X <= 0x%08X\n", bus, (devFn>>3)&0x1F, devFn&7, off, value));
        status = STATUS_SUCCESS;
        break;
    }

    case IOCTL_PSP_NBIO_UNLOCK:
      {
          /* NBIO SIG live on GPU BAR5 (0xC100/0xC180) — write via GPU proxy only.
           * Direct MmioBase writes hit PSP BAR0 (wrong window) or dual-map (0x1E). */
          if (!g_GpuProxyAvailable && g_GpuDriverHandle == NULL) {
              NTSTATUS proxySt = PspGpuProxyInit(devExt);
              if (!NT_SUCCESS(proxySt)) {
                  KdPrint(("NBIO unlock: GPU proxy unavailable 0x%08X\n", proxySt));
                  status = STATUS_DEVICE_NOT_READY;
                  break;
              }
          }
          ULONG beforeSig1 = PspGpuProxyReadRegister(NBIO_SIG1_OFFSET);
          ULONG beforeSig2 = PspGpuProxyReadRegister(NBIO_SIG2_OFFSET);
          ULONG mmhubBefore = PspGpuProxyReadRegister(MMHUB_CHECK_OFFSET);
          KdPrint(("NBIO unlock(proxy): SIG1=0x%08X SIG2=0x%08X MMHUB=0x%08X\n",
              beforeSig1, beforeSig2, mmhubBefore));
          if (!PspGpuProxyWriteRegister(NBIO_SIG1_OFFSET, NBIO_SIG1_VALUE) ||
              !PspGpuProxyWriteRegister(NBIO_SIG2_OFFSET, NBIO_SIG2_VALUE)) {
              status = STATUS_DEVICE_NOT_READY;
              break;
          }
          KeStallExecutionProcessor(1000);
          ULONG mmhubAfter = PspGpuProxyReadRegister(MMHUB_CHECK_OFFSET);
          KdPrint(("NBIO unlock(proxy): MMHUB after=0x%08X\n", mmhubAfter));
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
        KdPrint(("CREATE_RING: Allocating ring buffer...\n"));
        if (devExt->RingBufferPA.QuadPart != 0) {
            KdPrint(("CREATE_RING: Ring already created at PA=0x%llx\n", devExt->RingBufferPA.QuadPart));
            devExt->RingCreated = TRUE;
            if (outputLength >= sizeof(ULONG) * 3) {
                ULONG* resp = (ULONG*)outputBuffer;
                resp[0] = (ULONG)(devExt->RingBufferPA.QuadPart & 0xFFFFFFFF);
                resp[1] = (ULONG)(devExt->RingBufferPA.QuadPart >> 32);
                resp[2] = devExt->RingSize;
                bytesReturned = sizeof(ULONG) * 3;
            }
            status = STATUS_SUCCESS;
        } else {
            PHYSICAL_ADDRESS highAddr = {0};
            highAddr.QuadPart = 0x10000000000ULL;
            devExt->RingBuffer = MmAllocateContiguousMemory(0x10000, highAddr);
            if (devExt->RingBuffer == NULL) {
                highAddr.QuadPart = 0xFFFFFFFF;
                devExt->RingBuffer = MmAllocateContiguousMemory(0x10000, highAddr);
            }
            if (devExt->RingBuffer == NULL) {
                KdPrint(("CREATE_RING: Allocation failed\n"));
                status = STATUS_INSUFFICIENT_RESOURCES;
                break;
            }
            devExt->RingBufferPA = MmGetPhysicalAddress(devExt->RingBuffer);
            devExt->RingSize = 0x10000;
            devExt->RingCreated = TRUE;
            KdPrint(("CREATE_RING: PA=0x%llx size=%u\n", devExt->RingBufferPA.QuadPart, devExt->RingSize));
            if (outputLength >= sizeof(ULONG) * 3) {
                ULONG* resp = (ULONG*)outputBuffer;
                resp[0] = (ULONG)(devExt->RingBufferPA.QuadPart & 0xFFFFFFFF);
                resp[1] = (ULONG)(devExt->RingBufferPA.QuadPart >> 32);
                resp[2] = devExt->RingSize;
                bytesReturned = sizeof(ULONG) * 3;
            }
            status = STATUS_SUCCESS;
        }
        break;
    }

    case IOCTL_PSP_NBIO_VIA_RING:
    {
        /* GFX_CMD_ID_PROG_REG (0x0B) is NOT implemented on this SOS
         * (PspInitTmr prints "ring protocol not supported"). The old handler
         * returned STATUS_SUCCESS with a fake response — report it honestly. */
        KdPrint(("NBIO_VIA_RING: GFX_CMD_ID_PROG_REG (0x0B) not implemented on this SOS\n"));
        status = STATUS_NOT_SUPPORTED;
        break;
    }

    case IOCTL_PSP_GET_STATUS:
    {
        KdPrint(("IOCTL_PSP_GET_STATUS: g_GpuProxyAvailable=%d\n", g_GpuProxyAvailable));
        if (!g_GpuProxyAvailable) {
            NTSTATUS proxyStatus = PspGpuProxyInit(devExt);
            if (!NT_SUCCESS(proxyStatus)) {
                KdPrint(("IOCTL_PSP_GET_STATUS: GPU proxy init failed: 0x%08X\n", proxyStatus));
            } else {
                KdPrint(("IOCTL_PSP_GET_STATUS: GPU proxy initialized, g_GpuProxyAvailable=%d\n", g_GpuProxyAvailable));
            }
        }
        if (outputLength < sizeof(PSP_STATUS_INFO)) {
            status = STATUS_BUFFER_TOO_SMALL;
            break;
        }
        PSP_STATUS_INFO* info = (PSP_STATUS_INFO*)outputBuffer;
        RtlZeroMemory(info, sizeof(PSP_STATUS_INFO));
        
        ULONG mbox64, mbox35, mbox36, mbox37, mbox81;
        if (g_GpuProxyAvailable) {
            KdPrint(("IOCTL_PSP_GET_STATUS: using GPU proxy\n"));
            mbox64 = PspGpuProxyReadRegister(PSP_C2PMSG_64_OFFSET);
            mbox35 = PspGpuProxyReadRegister(PSP_C2PMSG_35_OFFSET);
            mbox36 = PspGpuProxyReadRegister(PSP_C2PMSG_36_OFFSET);
            mbox37 = PspGpuProxyReadRegister(PSP_C2PMSG_37_OFFSET);
            mbox81 = PspGpuProxyReadRegister(PSP_C2PMSG_81_OFFSET);
        } else {
            KdPrint(("IOCTL_PSP_GET_STATUS: no proxy\n"));
            mbox64 = mbox35 = mbox36 = mbox37 = mbox81 = 0xFFFFFFFF;
        }
        
        info->C2PMSG_81 = mbox81;
        info->C2PMSG_35 = mbox35;
        info->C2PMSG_36 = mbox36;
        info->C2PMSG_37 = mbox37;
        info->C2PMSG_64 = mbox64;
        info->PspAlive = (mbox81 != 0 && mbox81 != 0xFFFFFFFF) ? 1 : 0;
        info->FwLoaded = (devExt->FwBuffer != NULL) ? 1 : 0;
        info->FwSize = devExt->FwSize;
        info->FwPaShifted = devExt->FwPaShifted;
        info->NbioSig1 = PspGpuProxyReadRegister(NBIO_SIG1_OFFSET);
        info->NbioSig2 = PspGpuProxyReadRegister(NBIO_SIG2_OFFSET);

        /* GPU registers via proxy only — no dual BAR5 map */
        info->GrbmStatus = PspGpuProxyReadRegister(AMDBC250_GC_BASE + 0x2000);
        info->MmhubCheck = PspGpuProxyReadRegister(MMHUB_CHECK_OFFSET);
        info->GcCheck = PspGpuProxyReadRegister(AMDBC250_GC_BASE + 0x3000);
        info->HdpCheck = PspGpuProxyReadRegister(0x05A0);
        info->MeCntl = PspGpuProxyReadRegister(AMDBC250_GC_BASE + 0x3814);
        info->GrbmGfxIndex = PspGpuProxyReadRegister(AMDBC250_GC_BASE + 0x2270);
        if (info->GrbmStatus == 0xFFFFFFFF && info->MmhubCheck == 0xFFFFFFFF) {
            KdPrint(("IOCTL_PSP_GET_STATUS: GPU proxy unavailable\n"));
        }
        info->MmioVA = (ULONG)(ULONG_PTR)devExt->Bar0Base;
        info->MmioSize = devExt->Bar0Size;
        info->RingCreated = devExt->RingCreated ? 1 : 0;
        KdPrint(("IOCTL_PSP_GET_STATUS: mbox81=0x%08X\n", mbox81));
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

        /* 2026-09-23: NEVER dual-map GPU BAR5 — proxy only. */
        if (!g_GpuProxyAvailable) {
            NTSTATUS proxyStatus = PspGpuProxyInit(devExt);
            KdPrint(("BOOT_SEQ: GPU proxy init=0x%08X\n", proxyStatus));
        }

        /* Try file-based SYSDRV, fallback to embedded */
        {
            PUCHAR fwData = NULL;
            ULONG fwSize = 0;
            if (NT_SUCCESS(PspLoadFirmwareFromFile(
                    L"\\SystemRoot\\System32\\drivers\\bc-250\\Sysdrv.bin",
                    &fwData, &fwSize)) && fwSize <= PSP_MAX_FW_TOTAL) {
                PspFreeFirmware(devExt);
                devExt->FwBuffer = MmAllocateContiguousMemory(fwSize, highAddr);
                if (devExt->FwBuffer == NULL) {
                    highAddr.QuadPart = 0xFFFFFFFF;
                    devExt->FwBuffer = MmAllocateContiguousMemory(fwSize, highAddr);
                }
                if (devExt->FwBuffer) {
                    RtlCopyMemory(devExt->FwBuffer, fwData, fwSize);
                    devExt->FwSize = fwSize;
                    devExt->FwPhysical = MmGetPhysicalAddress(devExt->FwBuffer);
                    devExt->FwPaShifted = (ULONG)(devExt->FwPhysical.QuadPart >> 20);
                    KdPrint(("BOOT_SEQ: SYSDRV from file (%u bytes)\n", fwSize));
                }
                ExFreePoolWithTag(fwData, 'fw');
            }
        }
        if (devExt->FwBuffer == NULL) {
            if (g_SysdrvFirmwareSize > PSP_MAX_FW_TOTAL) {
                KdPrint(("BOOT_SEQ: SYSDRV FW too large (%u)\n", g_SysdrvFirmwareSize));
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
            KdPrint(("BOOT_SEQ: SYSDRV from embedded (%u bytes)\n", g_SysdrvFirmwareSize));
        }
        results[0] = devExt->FwPaShifted;

        stepStatus = PspSendMailboxCommand(devExt, 0x00000004);
        results[1] = NT_SUCCESS(stepStatus) ? 1 : 0;
        KdPrint(("BOOT_SEQ: SYSDRV cmd=0x4 => %s\n", results[1] ? "SENT" : "FAIL"));
        if (!NT_SUCCESS(stepStatus)) {
            KdPrint(("BOOT_SEQ: SYSDRV failed with 0x%08X, skipping SOS\n", stepStatus));
            status = stepStatus;
            PspFreeFirmware(devExt);
            if (outputLength >= sizeof(results)) {
                RtlCopyMemory(outputBuffer, results, sizeof(results));
                bytesReturned = sizeof(results);
            }
            break;
        }

        /* Try file-based SOS, fallback to embedded */
        PspFreeFirmware(devExt);
        {
            PUCHAR fwData = NULL;
            ULONG fwSize = 0;
            if (NT_SUCCESS(PspLoadFirmwareFromFile(
                    L"\\SystemRoot\\System32\\drivers\\bc-250\\Sos.bin",
                    &fwData, &fwSize)) && fwSize <= PSP_MAX_FW_TOTAL) {
                devExt->FwBuffer = MmAllocateContiguousMemory(fwSize, highAddr);
                if (devExt->FwBuffer == NULL) {
                    highAddr.QuadPart = 0xFFFFFFFF;
                    devExt->FwBuffer = MmAllocateContiguousMemory(fwSize, highAddr);
                }
                if (devExt->FwBuffer) {
                    RtlCopyMemory(devExt->FwBuffer, fwData, fwSize);
                    devExt->FwSize = fwSize;
                    devExt->FwPhysical = MmGetPhysicalAddress(devExt->FwBuffer);
                    devExt->FwPaShifted = (ULONG)(devExt->FwPhysical.QuadPart >> 20);
                    KdPrint(("BOOT_SEQ: SOS from file (%u bytes)\n", fwSize));
                }
                ExFreePoolWithTag(fwData, 'fw');
            }
        }
        if (devExt->FwBuffer == NULL) {
            if (g_SosFirmwareSize > PSP_MAX_FW_TOTAL) {
                KdPrint(("BOOT_SEQ: SOS FW too large (%u)\n", g_SosFirmwareSize));
                PspFreeFirmware(devExt);
                status = STATUS_INVALID_PARAMETER;
                break;
            }
            devExt->FwBuffer = MmAllocateContiguousMemory(g_SosFirmwareSize, highAddr);
            if (devExt->FwBuffer == NULL) {
                highAddr.QuadPart = 0xFFFFFFFF;
                devExt->FwBuffer = MmAllocateContiguousMemory(g_SosFirmwareSize, highAddr);
            }
            if (devExt->FwBuffer == NULL) {
                KdPrint(("BOOT_SEQ: SOS alloc failed\n"));
                PspFreeFirmware(devExt);
                status = STATUS_INSUFFICIENT_RESOURCES;
                break;
            }
            RtlZeroMemory(devExt->FwBuffer, g_SosFirmwareSize);
            RtlCopyMemory(devExt->FwBuffer, (PVOID)g_SosFirmwareData, g_SosFirmwareSize);
            devExt->FwSize = g_SosFirmwareSize;
            devExt->FwPhysical = MmGetPhysicalAddress(devExt->FwBuffer);
            devExt->FwPaShifted = (ULONG)(devExt->FwPhysical.QuadPart >> 20);
            KdPrint(("BOOT_SEQ: SOS from embedded (%u bytes)\n", g_SosFirmwareSize));
        }

        stepStatus = PspSendMailboxCommand(devExt, 0x00000008);
        results[2] = NT_SUCCESS(stepStatus) ? 1 : 0;
        KdPrint(("BOOT_SEQ: SOS cmd=0x8 => %s\n", results[2] ? "SENT" : "FAIL"));

        (void)PspGpuProxyWriteRegister(NBIO_SIG1_OFFSET, NBIO_SIG1_VALUE);
        (void)PspGpuProxyWriteRegister(NBIO_SIG2_OFFSET, NBIO_SIG2_VALUE);
        KeStallExecutionProcessor(1000);
        KdPrint(("BOOT_SEQ: NBIO unlock via GPU proxy written\n"));

        results[3] = PspGpuProxyReadRegister(AMDBC250_GC_BASE + 0x2000);
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
        probe->NbioSig1 = PspGpuProxyReadRegister(NBIO_SIG1_OFFSET);
        probe->NbioSig2 = PspGpuProxyReadRegister(NBIO_SIG2_OFFSET);
        probe->MmhubCheck = PspGpuProxyReadRegister(MMHUB_CHECK_OFFSET);
        probe->GrbmStatus = PspGpuProxyReadRegister(AMDBC250_GC_BASE + 0x2000);
        probe->GcCheck = PspGpuProxyReadRegister(AMDBC250_GC_BASE + 0x3000);
        probe->HdpCheck = PspGpuProxyReadRegister(0x05A0);
        (void)PspGpuProxyWriteRegister(NBIO_SIG1_OFFSET, NBIO_SIG1_VALUE);
        (void)PspGpuProxyWriteRegister(NBIO_SIG2_OFFSET, NBIO_SIG2_VALUE);
        KeStallExecutionProcessor(1000);
        ULONG sig1a = PspGpuProxyReadRegister(NBIO_SIG1_OFFSET);
        ULONG sig2a = PspGpuProxyReadRegister(NBIO_SIG2_OFFSET);
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
        KdPrint(("IOCTL_PSP_GET_GPU_INFO: proxy=%d\n", (int)g_GpuProxyAvailable));
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
        if (!g_GpuProxyAvailable) {
            NTSTATUS proxyStatus = PspGpuProxyInit(devExt);
            if (!NT_SUCCESS(proxyStatus)) {
                KdPrint(("GET_GPU_INFO: GPU proxy init failed: 0x%08X\n", proxyStatus));
            }
        }
        if (g_GpuProxyAvailable) {
            info->C2pmsg64 = PspGpuProxyReadRegister(PSP_C2PMSG_64_OFFSET);
            info->C2pmsg81 = PspGpuProxyReadRegister(PSP_C2PMSG_81_OFFSET);
        } else {
            info->C2pmsg64 = 0xFFFFFFFF;
            info->C2pmsg81 = 0xFFFFFFFF;
        }
        KdPrint(("IOCTL_PSP_GET_GPU_INFO: C2pmsg64=0x%08X C2pmsg81=0x%08X\n", info->C2pmsg64, info->C2pmsg81));
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
        if (!g_GpuProxyAvailable) {
            NTSTATUS proxyStatus = PspGpuProxyInit(devExt);
            if (!NT_SUCCESS(proxyStatus)) {
                KdPrint(("IOCTL_PSP_REG_PROG: GPU proxy init failed: 0x%08X\n", proxyStatus));
                status = STATUS_DEVICE_NOT_READY;
                break;
            }
        }
        if (!PspGpuProxyWriteRegister(regId, regVal)) {
            KdPrint(("IOCTL_PSP_REG_PROG: GPU proxy write failed offset=0x%X\n", regId));
            status = STATUS_DEVICE_NOT_READY;
            break;
        }
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
        KdPrint(("IOCTL_PSP_LOAD_TOC\n"));

        if (!g_GpuProxyAvailable) {
            NTSTATUS proxyStatus = PspGpuProxyInit(devExt);
            if (!NT_SUCCESS(proxyStatus)) {
                KdPrint(("LOAD_TOC: GPU proxy init failed: 0x%08X\n", proxyStatus));
                status = STATUS_DEVICE_NOT_READY;
                break;
            }
        }
        if (!g_GpuProxyAvailable) {
            KdPrint(("LOAD_TOC: no GPU proxy\n"));
            status = STATUS_DEVICE_NOT_READY;
            break;
        }

        /* Load ASD, TA, and SMU firmware from disk */
        PUCHAR asdData = NULL; ULONG asdSize = 0;
        PUCHAR taData = NULL;  ULONG taSize = 0;
        PUCHAR smuData = NULL; ULONG smuSize = 0;

        PspLoadFirmwareFromFile(L"\\SystemRoot\\System32\\drivers\\bc-250\\Asd.bin",
            &asdData, &asdSize);
        PspLoadFirmwareFromFile(L"\\SystemRoot\\System32\\drivers\\bc-250\\Ta.bin",
            &taData, &taSize);
        NTSTATUS fwStatus = PspLoadFirmwareFromFile(
            L"\\SystemRoot\\System32\\drivers\\bc-250\\Smu.bin",
            &smuData, &smuSize);
        if (!NT_SUCCESS(fwStatus)) {
            KdPrint(("LOAD_TOC: Smu.bin not found in bc-250, using embedded\n"));
            smuData = (PUCHAR)g_SmuFirmwareData;
            smuSize = g_SmuFirmwareSize;
        } else {
            KdPrint(("LOAD_TOC: Using SMU firmware from file (%u bytes)\n", smuSize));
        }

        if (smuSize == 0 || smuSize > 0x100000) {
            KdPrint(("LOAD_TOC: Invalid SMU firmware size=%u\n", smuSize));
            if (smuData != g_SmuFirmwareData && smuData != NULL)
                ExFreePoolWithTag(smuData, 'fw');
            if (asdData) ExFreePoolWithTag(asdData, 'fw');
            if (taData) ExFreePoolWithTag(taData, 'fw');
            status = STATUS_INVALID_PARAMETER;
            break;
        }

        PVOID cmdBufVa = NULL, tocBufVa = NULL;
        PHYSICAL_ADDRESS cmdBufPa = {0}, tocBufPa = {0}, highAddr = {0};
        ULONG fwOffsets[3], fwSizes[3], fwTypes[3];
        ULONG entryCount = 0, timeout = 0, cmdReg = 0;
        ULONG c2p81 = 0;

        highAddr.QuadPart = 0x10000000000ULL;

        /* Build TOC: 12-byte header + 16-byte entries (ASD=5, TA=6, SMC=7) */
        ULONG tocSize = 12;
        if (asdData) { fwTypes[entryCount] = 5; fwSizes[entryCount] = asdSize; entryCount++; }
        if (taData)  { fwTypes[entryCount] = 6; fwSizes[entryCount] = taSize;  entryCount++; }
        fwTypes[entryCount] = 7; fwSizes[entryCount] = smuSize; entryCount++;

        tocSize = 12 + entryCount * 16;
        ULONG fwOffset = (tocSize + 0x7F) & ~0x7F;
        ULONG totalSize = fwOffset;
        for (ULONG i = 0; i < entryCount; i++) {
            fwOffsets[i] = totalSize;
            totalSize += fwSizes[i];
        }

        tocBufVa = MmAllocateContiguousMemory(totalSize, highAddr);
        if (tocBufVa == NULL) {
            highAddr.QuadPart = 0xFFFFFFFF;
            tocBufVa = MmAllocateContiguousMemory(totalSize, highAddr);
        }
        if (tocBufVa == NULL) {
            KdPrint(("LOAD_TOC: alloc %u failed\n", totalSize));
            if (smuData != g_SmuFirmwareData && smuData) ExFreePoolWithTag(smuData, 'fw');
            if (asdData) ExFreePoolWithTag(asdData, 'fw');
            if (taData) ExFreePoolWithTag(taData, 'fw');
            status = STATUS_INSUFFICIENT_RESOURCES;
            break;
        }
        RtlZeroMemory(tocBufVa, totalSize);
        tocBufPa = MmGetPhysicalAddress(tocBufVa);

        PUCHAR toc = (PUCHAR)tocBufVa;
        *((PULONG)(toc + 0)) = 0;
        *((PULONG)(toc + 4)) = entryCount;
        *((PULONG)(toc + 8)) = totalSize;
        for (ULONG i = 0; i < entryCount; i++) {
            PHYSICAL_ADDRESS fwPa;
            fwPa.QuadPart = tocBufPa.QuadPart + fwOffsets[i];
            ULONG e = 12 + i * 16;
            *((PULONG)(toc + e + 0)) = fwTypes[i];
            *((PULONG)(toc + e + 4)) = fwSizes[i];
            *((PULONG)(toc + e + 8)) = (ULONG)(fwPa.QuadPart & 0xFFFFFFFF);
            *((PULONG)(toc + e + 12)) = (ULONG)(fwPa.QuadPart >> 32);
        }
        if (asdData) {
            RtlCopyMemory(toc + fwOffsets[0], asdData, asdSize);
            ExFreePoolWithTag(asdData, 'fw');
        }
        if (taData) {
            RtlCopyMemory(toc + fwOffsets[1], taData, taSize);
            ExFreePoolWithTag(taData, 'fw');
        }
        RtlCopyMemory(toc + fwOffsets[entryCount - 1], smuData, smuSize);
        if (smuData != g_SmuFirmwareData) ExFreePoolWithTag(smuData, 'fw');

        /* Command buffer */
        cmdBufVa = MmAllocateContiguousMemory(64, highAddr);
        if (cmdBufVa == NULL) {
            highAddr.QuadPart = 0xFFFFFFFF;
            cmdBufVa = MmAllocateContiguousMemory(64, highAddr);
        }
        if (cmdBufVa == NULL) {
            MmFreeContiguousMemory(tocBufVa);
            status = STATUS_INSUFFICIENT_RESOURCES;
            break;
        }
        RtlZeroMemory(cmdBufVa, 64);
        cmdBufPa = MmGetPhysicalAddress(cmdBufVa);

        PULONG cmd = (PULONG)cmdBufVa;
        cmd[0] = 64;  cmd[1] = 3;  cmd[2] = GFX_CMD_ID_LOAD_TOC;
        cmd[3] = 0;  cmd[4] = 0;  cmd[5] = 0;  cmd[6] = 0;
        cmd[7] = (ULONG)(tocBufPa.QuadPart & 0xFFFFFFFF);
        cmd[8] = (ULONG)(tocBufPa.QuadPart >> 32);
        cmd[9] = totalSize;

        KdPrint(("LOAD_TOC: cmdPA=0x%llX tocPA=0x%llX total=%u entries=%u\n",
                 cmdBufPa.QuadPart, tocBufPa.QuadPart, totalSize, entryCount));

        /* Send via mailbox — proxy must be up first (no Zw under lock) */
        if (!g_GpuProxyAvailable) {
            KdPrint(("LOAD_TOC: no GPU proxy\n"));
            MmFreeContiguousMemory(cmdBufVa);
            MmFreeContiguousMemory(tocBufVa);
            status = STATUS_DEVICE_NOT_READY;
            break;
        }
        {
            (void)KeWaitForSingleObject(&devExt->CommandLock, Executive, KernelMode, FALSE, NULL);
            if (!PspGpuProxyWriteRegister(PSP_C2PMSG_36_OFFSET,
                    (ULONG)(cmdBufPa.QuadPart & 0xFFFFFFFF)) ||
                !PspGpuProxyWriteRegister(PSP_C2PMSG_37_OFFSET,
                    (ULONG)(cmdBufPa.QuadPart >> 32)) ||
                !PspGpuProxyWriteRegister(PSP_C2PMSG_35_OFFSET,
                    GFX_CMD_ID_LOAD_TOC)) {
                KeReleaseMutex(&devExt->CommandLock, FALSE);
                KdPrint(("LOAD_TOC: GPU proxy write failed\n"));
                MmFreeContiguousMemory(cmdBufVa);
                MmFreeContiguousMemory(tocBufVa);
                status = STATUS_DEVICE_NOT_READY;
                break;
            }
            KeReleaseMutex(&devExt->CommandLock, FALSE);
        }

        /* Poll C2PMSG_35 for completion */
        {
            for (timeout = 0; timeout < 3000; timeout++) {
                KeStallExecutionProcessor(1000);
                cmdReg = PspGpuProxyReadRegister(PSP_C2PMSG_35_OFFSET);
                if (cmdReg == 0) break;
            }
        }

        c2p81 = PspGpuProxyReadRegister(PSP_C2PMSG_81_OFFSET);
        /* SMU registers via SMN (MP1 NOT mapped in BAR5 on BC-250) */
        PspGpuProxyWriteRegister(0x38, 0x03B10A08);
        KeStallExecutionProcessor(1);
        ULONG smu66 = PspGpuProxyReadRegister(0x3C);
        PspGpuProxyWriteRegister(0x38, 0x03B10A48);
        KeStallExecutionProcessor(1);
        ULONG smu82 = PspGpuProxyReadRegister(0x3C);
        PspGpuProxyWriteRegister(0x38, 0x03B10A68);
        KeStallExecutionProcessor(1);
        ULONG smu90 = PspGpuProxyReadRegister(0x3C);

        KdPrint(("LOAD_TOC: timeout=%u C2PMSG_35=0x%08X C2PMSG_81=0x%08X\n", timeout, cmdReg, c2p81));
        KdPrint(("LOAD_TOC: SMU [66]=0x%08X (SMN) [82]=0x%08X [90]=0x%08X\n", smu66, smu82, smu90));

        if (outputLength >= sizeof(ULONG) * 5) {
            PULONG resp = (PULONG)outputBuffer;
            resp[0] = timeout >= 3000 ? 0 : 1;
            resp[1] = c2p81;
            resp[2] = smu66;
            resp[3] = smu82;
            resp[4] = smu90;
            bytesReturned = sizeof(ULONG) * 5;
        }

        MmFreeContiguousMemory(cmdBufVa);
        MmFreeContiguousMemory(tocBufVa);

        status = timeout >= 3000 ? STATUS_TIMEOUT : STATUS_SUCCESS;
        break;
    }

    case IOCTL_PSP_KIQ_SUBMIT:
    {
        PSP_KIQ_SUBMIT_REQUEST* req;
        if (inputBuffer == NULL || inputLength < sizeof(ULONG)) {
            status = STATUS_INVALID_PARAMETER;
            break;
        }
        req = (PSP_KIQ_SUBMIT_REQUEST*)inputBuffer;
        if (req->CommandCount == 0 || req->CommandCount > 64) {
            status = STATUS_INVALID_PARAMETER;
            break;
        }
        if (inputLength < (SIZE_T)FIELD_OFFSET(PSP_KIQ_SUBMIT_REQUEST, Commands[req->CommandCount])) {
            status = STATUS_INVALID_PARAMETER;
            break;
        }
        if (!g_KiqRingInitialized) {
            status = PspKiqInit(devExt, 0, 0, 0);
            if (!NT_SUCCESS(status)) {
                break;
            }
        }
        status = PspKiqSubmit(devExt, req);
        if (NT_SUCCESS(status) && outputLength >= sizeof(ULONG)) {
            ((PULONG)outputBuffer)[0] = g_KiqRingWptr;
            bytesReturned = sizeof(ULONG);
        }
        break;
    }

    case IOCTL_PSP_KIQ_LOAD_FW:
    {
        KdPrint(("IOCTL_PSP_KIQ_LOAD_FW: inputLength=%u outputLength=%u\n", inputLength, outputLength));
        
        if (inputLength < sizeof(ULONG) * 2) {
            KdPrint(("KIQ_LOAD_FW: buffer too small\n"));
            status = STATUS_INVALID_PARAMETER;
            break;
        }

        ULONG fwType = *((PULONG)inputBuffer);
        ULONG fwSize = *((PULONG)inputBuffer + 1);
        KdPrint(("KIQ_LOAD_FW: fwType=%u fwSize=%u inputLength=%u\n", fwType, fwSize, inputLength));
        
        if (fwSize == 0 || fwSize > 0x100000 || inputLength < sizeof(ULONG) * 2 + fwSize) {
            KdPrint(("KIQ_LOAD_FW: invalid params\n"));
            status = STATUS_INVALID_PARAMETER;
            break;
        }
        
        PUCHAR fwData = (PUCHAR)inputBuffer + sizeof(ULONG) * 2;
        if (!fwData) {
            status = STATUS_INVALID_PARAMETER;
            break;
        }
        status = PspKiqLoadFirmware(devExt, fwType, fwSize, fwData);
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

    case IOCTL_PSP_LOAD_IP_FW_DIRECT:
    {
        if (inputLength < sizeof(PSP_LOAD_IP_FW_REQUEST)) {
            status = STATUS_INVALID_PARAMETER;
            break;
        }
        PSP_LOAD_IP_FW_REQUEST* req = (PSP_LOAD_IP_FW_REQUEST*)inputBuffer;
        /* Check upper bound to prevent integer overflow in addition below */
        if (req->FwSize == 0 || req->FwSize > 0x100000 ||
            inputLength < sizeof(PSP_LOAD_IP_FW_REQUEST) + req->FwSize) {
            status = STATUS_BUFFER_TOO_SMALL;
            break;
        }
        PUCHAR fwData = (PUCHAR)(req + 1);
        KdPrint(("IOCTL_PSP_LOAD_IP_FW_DIRECT: type=%u size=%u\n", req->FwType, req->FwSize));
        status = PspLoadIpFwViaMailbox(devExt, req->FwType, req->FwSize, fwData);
        if (outputLength >= sizeof(PSP_LOAD_IP_FW_RESPONSE)) {
            /* NOTE: METHOD_BUFFERED means inputBuffer == outputBuffer.
             * RtlZeroMemory(resp) will also zero req->FwType/FwSize/CommandCount.
             * This is safe here because all input fields have been consumed
             * before the zero. The PM4_SUBMIT handler (below) explicitly
             * saves/restores overlapping fields for the same reason. */
            PSP_LOAD_IP_FW_RESPONSE* resp = (PSP_LOAD_IP_FW_RESPONSE*)outputBuffer;
            RtlZeroMemory(resp, sizeof(*resp));
            resp->Status = (ULONG)status;
            if (g_GpuProxyAvailable) {
                resp->C2Pmsg35 = PspGpuProxyReadRegister(PSP_C2PMSG_35_OFFSET);
                resp->C2Pmsg81 = PspGpuProxyReadRegister(PSP_C2PMSG_81_OFFSET);
            }
            bytesReturned = sizeof(PSP_LOAD_IP_FW_RESPONSE);
        }
        break;
    }

    case IOCTL_PSP_GPU_PM4_SUBMIT:
    {
        KdPrint(("IOCTL_PSP_GPU_PM4_SUBMIT: inputLen=%u outputLen=%u\n", inputLength, outputLength));
        if (inputLength < (SIZE_T)FIELD_OFFSET(PSP_GPU_PM4_SUBMIT_REQUEST, Commands[0])) {
            status = STATUS_INVALID_PARAMETER;
            break;
        }
        PPSP_GPU_PM4_SUBMIT_REQUEST req = (PPSP_GPU_PM4_SUBMIT_REQUEST)inputBuffer;
        ULONG cmdCount = req->CommandCount;
        ULONG waitMs = req->WaitMs;
        /* Validate cmdCount BEFORE reading Commands[] (prevents OOB read) */
        if (cmdCount == 0 || cmdCount > 64 ||
            inputLength < (SIZE_T)FIELD_OFFSET(PSP_GPU_PM4_SUBMIT_REQUEST, Commands[cmdCount])) {
            status = STATUS_INVALID_PARAMETER;
            break;
        }
        /* Save Commands[0..7] that overlap response struct (METHOD_BUFFERED sharing) */
        ULONG savedCmds[8];
        ULONG saveCount = cmdCount < 8 ? cmdCount : 8;
        RtlCopyMemory(savedCmds, req->Commands, saveCount * sizeof(ULONG));
        if (outputLength < sizeof(PSP_GPU_PM4_SUBMIT_RESPONSE)) {
            status = STATUS_BUFFER_TOO_SMALL;
            break;
        }
        PPSP_GPU_PM4_SUBMIT_RESPONSE resp = (PPSP_GPU_PM4_SUBMIT_RESPONSE)outputBuffer;
        RtlZeroMemory(resp, sizeof(*resp));
        /* Restore fields that RtlZeroMemory cleared (METHOD_BUFFERED shares buffer) */
        req->CommandCount = cmdCount;
        req->WaitMs = waitMs;
        RtlCopyMemory(req->Commands, savedCmds, saveCount * sizeof(ULONG));
        status = PspGpuPm4Submit(devExt, req, resp);
        bytesReturned = sizeof(PSP_GPU_PM4_SUBMIT_RESPONSE);
        break;
    }

    default:
        KdPrint(("DEFAULT: unknown IOCTL 0x%08X\n", ioctlCode));
        status = STATUS_INVALID_DEVICE_REQUEST;
        break;
    }

    Irp->IoStatus.Status = status;
    Irp->IoStatus.Information = bytesReturned;
    IoCompleteRequest(Irp, IO_NO_INCREMENT);
    return status;
}
