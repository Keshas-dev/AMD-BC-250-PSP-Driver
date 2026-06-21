// PspSmu.c - SMU (System Management Unit) communication
// Uses SMU C2PMSG registers via GPU BAR5 proxy (MP1 at GPU_BAR5+0x16000)
// SMU C2PMSG offsets from mp_11_0_8_offset.h:
//   C2PMSG_66 (msg)   = MP1_BASE + 0xA08 = 0x16A08
//   C2PMSG_82 (param)  = MP1_BASE + 0xA48 = 0x16A48
//   C2PMSG_90 (resp)   = MP1_BASE + 0xA68 = 0x16A68
// Protocol: write param to C2PMSG_82, write msg to C2PMSG_66, poll C2PMSG_90
#include <ntddk.h>
#include <wdm.h>
#include "PspIoctl.h"
#include "PspCore.h"
#include "PspSmu.h"

#define SMU_RESPONSE_TIMEOUT_MS   3000

NTSTATUS PspSmuWake(PDEVICE_EXTENSION devExt, ULONG message, ULONG argument, PULONG pResponse)
{
    ULONG timeout;
    ULONG reg66, reg82, reg90;

    PAGED_CODE();
    KdPrint(("PSP_SMU: PspSmuWake msg=0x%08X arg=0x%08X\n", message, argument));

    if (!g_Bar5Mapping && !g_GpuProxyAvailable) {
        KdPrint(("PSP_SMU: No GPU BAR5 access available\n"));
        return STATUS_NOT_SUPPORTED;
    }

    reg66 = MP1_BASE + SMU_C2PMSG_66_OFFSET;
    reg82 = MP1_BASE + SMU_C2PMSG_82_OFFSET;
    reg90 = MP1_BASE + SMU_C2PMSG_90_OFFSET;

    ULONG before66 = PspGpuProxyReadRegister(reg66);
    ULONG before82 = PspGpuProxyReadRegister(reg82);
    ULONG before90 = PspGpuProxyReadRegister(reg90);
    KdPrint(("PSP_SMU: before [66]=0x%08X [82]=0x%08X [90]=0x%08X\n",
        before66, before82, before90));

    PspGpuProxyWriteRegister(reg82, argument);
    KeStallExecutionProcessor(10);
    PspGpuProxyWriteRegister(reg66, message);
    KeStallExecutionProcessor(10);

    KdPrint(("PSP_SMU: msg/param written, polling [90]...\n"));

    *pResponse = 0;
    for (timeout = 0; timeout < SMU_RESPONSE_TIMEOUT_MS; timeout++) {
        KeStallExecutionProcessor(1000);
        ULONG resp = PspGpuProxyReadRegister(reg90);
        if (resp != 0 && resp != 0xFFFFFFFF) {
            *pResponse = resp;
            KdPrint(("PSP_SMU: response=0x%08X after %u ms\n", resp, timeout));
            return STATUS_SUCCESS;
        }
    }

    ULONG after66 = PspGpuProxyReadRegister(reg66);
    ULONG after82 = PspGpuProxyReadRegister(reg82);
    ULONG after90 = PspGpuProxyReadRegister(reg90);
    KdPrint(("PSP_SMU: TIMEOUT [66]=0x%08X [82]=0x%08X [90]=0x%08X\n",
        after66, after82, after90));
    return STATUS_TIMEOUT;
}
