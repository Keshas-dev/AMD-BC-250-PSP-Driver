// PspSmu.c - SMU (System Management Unit) communication via SMN
// BC-250: MP1 (SMU) is NOT mapped into BAR5. SMU registers are accessed via
// the SMN bus through NBIO PCI config space bridge:
//   Write SMN address to NBIO_ADDR (BAR5+0x38)
//   Write/read data from NBIO_DATA (BAR5+0x3C)
// SMN addresses: C2PMSG_66=0x03B10A08, C2PMSG_82=0x03B10A48, C2PMSG_90=0x03B10A68
#include <ntddk.h>
#include <wdm.h>
#include "PspIoctl.h"
#include "PspCore.h"
#include "PspSmu.h"

#define SMU_RESPONSE_TIMEOUT_MS   3000

/* SMN bridge registers via NBIO (raw BAR5 offsets, no GC_BASE shift) */
#define NBIO_SMN_ADDR  0x38
#define NBIO_SMN_DATA  0x3C

/* SMU v11.8 SMN addresses (verified working on BC-250 via bar5-smn-test.c) */
#define SMU_SMN_C2PMSG_66  0x03B10A08   /* Message register */
#define SMU_SMN_C2PMSG_82  0x03B10A48   /* Argument/result register */
#define SMU_SMN_C2PMSG_90  0x03B10A68   /* Response register (0=busy, 1=OK) */

static VOID SmnWrite(PDEVICE_EXTENSION devExt, ULONG smnAddr, ULONG data)
{
    PspGpuProxyWriteRegister(NBIO_SMN_ADDR, smnAddr);
    KeStallExecutionProcessor(1);
    PspGpuProxyWriteRegister(NBIO_SMN_DATA, data);
    KeStallExecutionProcessor(1);
}

static ULONG SmnRead(PDEVICE_EXTENSION devExt, ULONG smnAddr)
{
    PspGpuProxyWriteRegister(NBIO_SMN_ADDR, smnAddr);
    KeStallExecutionProcessor(1);
    return PspGpuProxyReadRegister(NBIO_SMN_DATA);
}

NTSTATUS PspSmuWake(PDEVICE_EXTENSION devExt, ULONG message, ULONG argument, PULONG pResponse)
{
    ULONG timeout;
    ULONG before66, before82, before90;

    KdPrint(("PSP_SMU: PspSmuWake msg=0x%08X arg=0x%08X\n", message, argument));

    if (!g_Bar5Mapping && !devExt->GpuMmioBase && !g_GpuProxyAvailable) {
        KdPrint(("PSP_SMU: No GPU BAR5 access available\n"));
        return STATUS_NOT_SUPPORTED;
    }

    before66 = SmnRead(devExt, SMU_SMN_C2PMSG_66);
    before82 = SmnRead(devExt, SMU_SMN_C2PMSG_82);
    before90 = SmnRead(devExt, SMU_SMN_C2PMSG_90);
    KdPrint(("PSP_SMU: before [66]=0x%08X [82]=0x%08X [90]=0x%08X\n",
        before66, before82, before90));

    /* SMU mailbox protocol: write arg, write msg, poll response */
    SmnWrite(devExt, SMU_SMN_C2PMSG_82, argument);
    KeStallExecutionProcessor(10);
    SmnWrite(devExt, SMU_SMN_C2PMSG_66, message);
    KeStallExecutionProcessor(10);

    KdPrint(("PSP_SMU: msg/param written via SMN, polling [90]...\n"));

    *pResponse = 0;
    for (timeout = 0; timeout < SMU_RESPONSE_TIMEOUT_MS; timeout++) {
        KeStallExecutionProcessor(1000);
        ULONG resp = SmnRead(devExt, SMU_SMN_C2PMSG_90);
        if (resp != 0 && resp != 0xFFFFFFFF) {
            *pResponse = resp;
            KdPrint(("PSP_SMU: response=0x%08X after %u ms\n", resp, timeout));
            return STATUS_SUCCESS;
        }
    }

    ULONG after66 = SmnRead(devExt, SMU_SMN_C2PMSG_66);
    ULONG after82 = SmnRead(devExt, SMU_SMN_C2PMSG_82);
    ULONG after90 = SmnRead(devExt, SMU_SMN_C2PMSG_90);
    KdPrint(("PSP_SMU: TIMEOUT [66]=0x%08X [82]=0x%08X [90]=0x%08X\n",
        after66, after82, after90));
    return STATUS_TIMEOUT;
}
