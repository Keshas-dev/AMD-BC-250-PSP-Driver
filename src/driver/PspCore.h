#ifndef __PSP_CORE_H__
#define __PSP_CORE_H__

#include <wdm.h>
#include "PspIoctl.h"

// DEVICE_EXTENSION is defined in PspIoctl.h (shared with all driver files)

NTSTATUS PspSendMailboxCommand(PDEVICE_EXTENSION devExt, ULONG command);
NTSTATUS PspInitTmr(PDEVICE_EXTENSION devExt);
VOID PspFreeTmr(VOID);
NTSTATUS PspAutoInitialize(PDEVICE_EXTENSION devExt);
BOOLEAN PspValidateFirmware(PUCHAR FirmwareData, ULONG FirmwareSize);
VOID PspFreeFirmware(PDEVICE_EXTENSION devExt);

// Shared TMR globals
extern BOOLEAN g_TmrInitialized;
extern PVOID g_TmrBuffer;
extern PHYSICAL_ADDRESS g_TmrPhysical;
extern ULONG g_TmrSize;

// Shared KIQ globals
extern PVOID g_KiqRingVa;
extern PHYSICAL_ADDRESS g_KiqRingPa;
extern ULONG g_KiqRingSize;
extern ULONG g_KiqRingWptr;
extern BOOLEAN g_KiqRingInitialized;

#endif
