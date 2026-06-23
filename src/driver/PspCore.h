#ifndef __PSP_CORE_H__
#define __PSP_CORE_H__

#include <wdm.h>
#include "PspIoctl.h"

extern PVOID g_Bar5Mapping;
extern SIZE_T g_Bar5Size;
extern KSPIN_LOCK g_Bar5MappingLock;
extern HANDLE g_GpuDriverHandle;
extern BOOLEAN g_GpuProxyAvailable;

NTSTATUS PspGpuProxyInit(PDEVICE_EXTENSION devExt);
ULONG PspGpuProxyReadRegister(ULONG offset);
BOOLEAN PspGpuProxyWriteRegister(ULONG offset, ULONG value);

NTSTATUS PspSendMailboxCommand(PDEVICE_EXTENSION devExt, ULONG command);
NTSTATUS PspLoadIpFwViaMailbox(PDEVICE_EXTENSION devExt, ULONG FwType, ULONG FwSize, PUCHAR FwData);
NTSTATUS PspInitTmr(PDEVICE_EXTENSION devExt);
VOID PspFreeTmr(VOID);
NTSTATUS PspAutoInitialize(PDEVICE_EXTENSION devExt);
BOOLEAN PspValidateFirmware(PUCHAR FirmwareData, ULONG FirmwareSize);
VOID PspFreeFirmware(PDEVICE_EXTENSION devExt);

extern BOOLEAN g_TmrInitialized;
extern PVOID g_TmrBuffer;
extern PHYSICAL_ADDRESS g_TmrPhysical;
extern ULONG g_TmrSize;

extern PVOID g_KiqRingVa;
extern PHYSICAL_ADDRESS g_KiqRingPa;
extern ULONG g_KiqRingSize;
extern ULONG g_KiqRingWptr;
extern BOOLEAN g_KiqRingInitialized;
extern KSPIN_LOCK g_KiqRingLock;

#endif
