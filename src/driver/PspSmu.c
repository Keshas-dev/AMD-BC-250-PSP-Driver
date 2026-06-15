// PspSmu.c - SMU (System Management Unit) communication
// Uses SMU C2PMSG registers (MP1_BASE on BC-250 BAR5)
#include <ntddk.h>
#include <wdm.h>
#include "PspIoctl.h"
#include "PspCore.h"
#include "PspSmu.h"

#define SMU_RESPONSE_TIMEOUT_MS   3000

NTSTATUS PspSmuWake(PDEVICE_EXTENSION devExt, ULONG message, ULONG argument, PULONG pResponse)
{
    PVOID mboxBase;
    ULONG timeout;

    if (!devExt->MmioBase && !devExt->Bar0Base) return STATUS_DEVICE_NOT_READY;
    mboxBase = devExt->Bar0Base ? devExt->Bar0Base : devExt->MmioBase;

    PUCHAR smuBase = (PUCHAR)mboxBase + MP1_BASE;

    ULONG beforeMsg = READ_REGISTER_ULONG((PULONG)(smuBase + SMU_C2PMSG_66_OFFSET));
    ULONG beforeArg = READ_REGISTER_ULONG((PULONG)(smuBase + SMU_C2PMSG_82_OFFSET));
    KdPrint(("SMU: msg=0x%08X arg=0x%08X before[66]=0x%08X [82]=0x%08X\n",
        message, argument, beforeMsg, beforeArg));

    WRITE_REGISTER_ULONG((PULONG)(smuBase + SMU_C2PMSG_82_OFFSET), argument);
    KeStallExecutionProcessor(10);
    WRITE_REGISTER_ULONG((PULONG)(smuBase + SMU_C2PMSG_66_OFFSET), message);
    KeStallExecutionProcessor(10);
    WRITE_REGISTER_ULONG((PULONG)(smuBase + 0x00A6C), 1);
    KdPrint(("SMU: written msg/arg/trigger, polling [90]...\n"));

    *pResponse = 0;
    for (timeout = 0; timeout < SMU_RESPONSE_TIMEOUT_MS; timeout++) {
        KeStallExecutionProcessor(1000);
        ULONG resp = READ_REGISTER_ULONG((PULONG)(smuBase + SMU_C2PMSG_90_OFFSET));
        if (resp != 0 && resp != 0xFFFFFFFF) {
            *pResponse = resp;
            KdPrint(("SMU: response=0x%08X after %u ms\n", resp, timeout));
            return STATUS_SUCCESS;
        }
    }

    ULONG afterMsg = READ_REGISTER_ULONG((PULONG)(smuBase + SMU_C2PMSG_66_OFFSET));
    ULONG afterArg = READ_REGISTER_ULONG((PULONG)(smuBase + SMU_C2PMSG_82_OFFSET));
    KdPrint(("SMU: TIMEOUT [66]=0x%08X [82]=0x%08X\n", afterMsg, afterArg));
    return STATUS_TIMEOUT;
}
