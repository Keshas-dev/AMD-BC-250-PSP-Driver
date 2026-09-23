#ifndef __PSP_KIQ_H__
#define __PSP_KIQ_H__

#include <wdm.h>
#include "PspIoctl.h"

NTSTATUS PspKiqInit(PDEVICE_EXTENSION devExt, ULONG64 ringPA, ULONG ringSize, ULONG cmdBufSize);
NTSTATUS PspKiqSubmit(PDEVICE_EXTENSION devExt, PPSP_KIQ_SUBMIT_REQUEST req);
NTSTATUS PspKiqLoadFirmware(PDEVICE_EXTENSION devExt, ULONG FwType, ULONG FwSize, PUCHAR FwData);
NTSTATUS PspGpuPm4Submit(PDEVICE_EXTENSION devExt, PPSP_GPU_PM4_SUBMIT_REQUEST req, PPSP_GPU_PM4_SUBMIT_RESPONSE resp);
VOID PspKiqCleanup(VOID);

// KIQ register offsets (BAR5-relative, GC_BASE-shifted) — BC-250 cyan_skillfish
// Verified writable: KIQ_BASE_LO (0xE060), KIQ_BASE_HI (0xE064), KIQ_RPTR (0xE06C),
// KIQ_DOORBELL (0xE074), KIQ_WPTR (0xE078), KIQ_VMID (0xE07C), KIQ_ACTIVE (0xE080)
// Read-only on BC-250: KIQ_SIZE (0xE068 = 0), KIQ_PQ_CTL (0xE070 = 0x81818181)
#define PSP_KIQ_OFFSET             0x0E060  // KIQ_BASE_LO alias
#define PSP_KIQ_RING_BASE_LO       0x0E060  // KIQ ring base low  (WRITABLE)
#define PSP_KIQ_RING_BASE_HI       0x0E064  // KIQ ring base high (WRITABLE)
#define PSP_KIQ_SIZE               0x0E068  // KIQ ring size      (READ-ONLY = 0 on BC-250)
#define PSP_KIQ_RPTR               0x0E06C  // KIQ read pointer   (WRITABLE)
#define PSP_KIQ_PQ_CTL             0x0E070  // KIQ queue control  (READ-ONLY = 0x81818181)
#define PSP_KIQ_DOORBELL           0x0E074  // KIQ doorbell       (WRITABLE)
#define PSP_KIQ_WPTR               0x0E078  // KIQ write pointer  (WRITABLE)
#define PSP_KIQ_VMID               0x0E07C  // KIQ VMID           (WRITABLE)
#define PSP_KIQ_ACTIVE             0x0E080  // KIQ active         (WRITABLE)

#define KIQ_RING_TYPE_KM           1

// Deprecated aliases (remapped to correct offsets)
#define PSP_KIQ_WPTR_OFFSET        PSP_KIQ_WPTR
#define PSP_KIQ_RPTR_OFFSET        PSP_KIQ_RPTR
#define PSP_KIQ_RING_SIZE          PSP_KIQ_SIZE
#define PSP_KIQ_CMD_BUF_OFFSET     0x0E10C  // Not a standard KIQ register; kept for compat

#endif
