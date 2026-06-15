#ifndef __PSP_KIQ_H__
#define __PSP_KIQ_H__

#include <wdm.h>
#include "PspIoctl.h"

NTSTATUS PspKiqInit(PDEVICE_EXTENSION devExt, ULONG64 ringPA, ULONG ringSize, ULONG cmdBufSize);
NTSTATUS PspKiqSubmit(PDEVICE_EXTENSION devExt, PPSP_KIQ_SUBMIT_REQUEST req);
VOID PspKiqCleanup(VOID);

// KIQ register offsets (BAR5-relative) — BC-250 cyan_skillfish
#define PSP_KIQ_OFFSET             0x0E060
#define PSP_KIQ_WPTR_OFFSET        0x0E0A4
#define PSP_KIQ_RPTR_OFFSET        0x0E0A8
#define PSP_KIQ_CMD_BUF_OFFSET     0x0E10C
#define PSP_KIQ_RING_BASE_LO       0x0E088
#define PSP_KIQ_RING_BASE_HI       0x0E08C
#define PSP_KIQ_RING_SIZE          0x0E090
#define PSP_KIQ_ACTIVE             0x0E200

#define KIQ_RING_TYPE_KM           1

#endif
