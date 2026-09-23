#ifndef __PSP_SMU_H__
#define __PSP_SMU_H__

#include <wdm.h>
#include "PspIoctl.h"

NTSTATUS PspSmuWake(PDEVICE_EXTENSION devExt, ULONG message, ULONG argument, PULONG pResponse);

#endif
