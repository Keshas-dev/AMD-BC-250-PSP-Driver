// PspIoctl.h
// Shared IOCTL definitions for AMD BC-250 PSP Driver
// Used by both kernel driver (PspDriver.c) and user-mode test tools

#ifndef __PSP_IOCTL_H__
#define __PSP_IOCTL_H__

#ifdef __cplusplus
extern "C" {
#endif

// Device name and symbolic link
#define PSP_DEVICE_NAME    L"\\\\.\\AmdBcPsp"

// IOCTL codes (must match driver definitions)
#define IOCTL_PSP_READ_REG    CTL_CODE(FILE_DEVICE_UNKNOWN, 0x800, METHOD_BUFFERED, FILE_ANY_ACCESS)
#define IOCTL_PSP_WRITE_REG   CTL_CODE(FILE_DEVICE_UNKNOWN, 0x801, METHOD_BUFFERED, FILE_ANY_ACCESS)
#define IOCTL_PSP_LOAD_FW     CTL_CODE(FILE_DEVICE_UNKNOWN, 0x802, METHOD_BUFFERED, FILE_ANY_ACCESS)
#define IOCTL_PSP_INIT_HW     CTL_CODE(FILE_DEVICE_UNKNOWN, 0x803, METHOD_BUFFERED, FILE_ANY_ACCESS)
#define IOCTL_PSP_NBIO_UNLOCK CTL_CODE(FILE_DEVICE_UNKNOWN, 0x804, METHOD_BUFFERED, FILE_ANY_ACCESS)
#define IOCTL_PSP_SEND_CMD    CTL_CODE(FILE_DEVICE_UNKNOWN, 0x805, METHOD_BUFFERED, FILE_ANY_ACCESS)
#define IOCTL_PSP_CREATE_RING      CTL_CODE(FILE_DEVICE_UNKNOWN, 0x806, METHOD_BUFFERED, FILE_ANY_ACCESS)
#define IOCTL_PSP_NBIO_VIA_RING     CTL_CODE(FILE_DEVICE_UNKNOWN, 0x807, METHOD_BUFFERED, FILE_ANY_ACCESS)
#define IOCTL_PSP_GET_STATUS        CTL_CODE(FILE_DEVICE_UNKNOWN, 0x808, METHOD_BUFFERED, FILE_ANY_ACCESS)
#define IOCTL_PSP_LOAD_EMBEDDED_FW  CTL_CODE(FILE_DEVICE_UNKNOWN, 0x809, METHOD_BUFFERED, FILE_ANY_ACCESS)
#define IOCTL_PSP_BOOT_SEQUENCE     CTL_CODE(FILE_DEVICE_UNKNOWN, 0x810, METHOD_BUFFERED, FILE_ANY_ACCESS)
#define IOCTL_PSP_PCI_READ           CTL_CODE(FILE_DEVICE_UNKNOWN, 0x811, METHOD_BUFFERED, FILE_ANY_ACCESS)
#define IOCTL_PSP_PCI_WRITE          CTL_CODE(FILE_DEVICE_UNKNOWN, 0x812, METHOD_BUFFERED, FILE_ANY_ACCESS)
#define IOCTL_PSP_PROBE              CTL_CODE(FILE_DEVICE_UNKNOWN, 0x813, METHOD_BUFFERED, FILE_ANY_ACCESS)
#define IOCTL_PSP_RING_LOAD_IP_FW    CTL_CODE(FILE_DEVICE_UNKNOWN, 0x814, METHOD_BUFFERED, FILE_ANY_ACCESS)
#define IOCTL_PSP_GET_GPU_INFO        CTL_CODE(FILE_DEVICE_UNKNOWN, 0x815, METHOD_BUFFERED, FILE_ANY_ACCESS)
#define IOCTL_PSP_REG_PROG            CTL_CODE(FILE_DEVICE_UNKNOWN, 0x816, METHOD_BUFFERED, FILE_ANY_ACCESS)
#define IOCTL_PSP_AUTOLOAD_RLC        CTL_CODE(FILE_DEVICE_UNKNOWN, 0x817, METHOD_BUFFERED, FILE_ANY_ACCESS)
#define IOCTL_PSP_KIQ_SUBMIT          CTL_CODE(FILE_DEVICE_UNKNOWN, 0x818, METHOD_BUFFERED, FILE_ANY_ACCESS)
#define IOCTL_PSP_INIT_TMR            CTL_CODE(FILE_DEVICE_UNKNOWN, 0x819, METHOD_BUFFERED, FILE_ANY_ACCESS)
#define IOCTL_PSP_LOAD_TOC            CTL_CODE(FILE_DEVICE_UNKNOWN, 0x820, METHOD_BUFFERED, FILE_ANY_ACCESS)
#define IOCTL_PSP_SMU_WAKE            CTL_CODE(FILE_DEVICE_UNKNOWN, 0x821, METHOD_BUFFERED, FILE_ANY_ACCESS)
#define IOCTL_PSP_KIQ_LOAD_FW         CTL_CODE(FILE_DEVICE_UNKNOWN, 0x822, METHOD_BUFFERED, FILE_ANY_ACCESS)
#define IOCTL_PSP_KIQ_GET_STATUS      CTL_CODE(FILE_DEVICE_UNKNOWN, 0x823, METHOD_BUFFERED, FILE_ANY_ACCESS)
#define IOCTL_PSP_LOAD_IP_FW_DIRECT   CTL_CODE(FILE_DEVICE_UNKNOWN, 0x824, METHOD_BUFFERED, FILE_ANY_ACCESS)
#define IOCTL_PSP_GPU_PM4_SUBMIT      CTL_CODE(FILE_DEVICE_UNKNOWN, 0x825, METHOD_BUFFERED, FILE_ANY_ACCESS)

#ifdef _NTDDK_
typedef struct _DEVICE_EXTENSION {
    PVOID       MmioBase;
    ULONG       MmioSize;
    PVOID       Bar0Base;
    ULONG       Bar0Size;
    PVOID       GpuMmioBase;
    ULONG       GpuMmioSize;
    PVOID       FwBuffer;
    PHYSICAL_ADDRESS FwPhysical;
    ULONG       FwSize;
    ULONG       FwPaShifted;
    PVOID       RingBuffer;
    PHYSICAL_ADDRESS RingBufferPA;
    ULONG       RingSize;
    BOOLEAN     RingCreated;
    KSPIN_LOCK  CommandLock;
    PVOID       PciCfgBase;
    ULONG       PciCfgSize;
} DEVICE_EXTENSION, *PDEVICE_EXTENSION;
#endif

// Timeout for PSP firmware commands (ms)
#define PSP_FW_WAIT_MS               5000

// PSP Mailbox register offsets (relative to BAR0 base)
// These match the hardware addresses documented in the spec
#define PSP_C2PMSG_35_OFFSET  0x1056C   // Command register
#define PSP_C2PMSG_36_OFFSET  0x10570   // Data register (PA low 32b)
#define PSP_C2PMSG_37_OFFSET  0x10574   // Data register (PA high 32b)
#define PSP_C2PMSG_64_OFFSET  0x105E0   // Ring control register
#define PSP_C2PMSG_65_OFFSET  0x105E4   // RBI wptr
#define PSP_C2PMSG_66_OFFSET  0x105E8   // RBI rptr
#define PSP_C2PMSG_67_OFFSET  0x105EC   // GPCOM wptr
#define PSP_C2PMSG_68_OFFSET  0x105F0   // GPCOM rptr
#define PSP_C2PMSG_69_OFFSET  0x105F4   // Ring buffer addr lo
#define PSP_C2PMSG_70_OFFSET  0x105F8   // Ring buffer addr hi
#define PSP_C2PMSG_71_OFFSET  0x105FC   // Ring buffer size
#define PSP_C2PMSG_81_OFFSET  0x10614   // Status register

// SMU v11.8 Mailbox offsets (MP1_BASE = 0x16000 on BC-250)
// Used for SMU communication: C2PMSG_66->90 at MP1_BASE offset
#define MP1_BASE             0x16000    // MP1 (SMU) base in BAR5 (same as MP0 on BC-250)
#define SMU_C2PMSG_66_OFFSET 0x00A08    // Message register (write msg → triggers SMU)
#define SMU_C2PMSG_82_OFFSET 0x00A48    // Argument register (write param, read result)
#define SMU_C2PMSG_83_OFFSET 0x00A4C    // Extended data
#define SMU_C2PMSG_90_OFFSET 0x00A68    // Response register (0=busy, 1=OK, FF=err)

// PSP BAR0 physical address and size (for auto-init fallback)
#define PSP_BAR0_PHYSICAL      0xFD600000ULL
#define PSP_BAR0_SIZE          0x40000

// GC register base offset on BC-250 (Cyan Skillfish)
// Navi10 has GC registers at BAR5+0x0000; BC-250 shifts them by 0x1260
// See: cyan_skillfish_ip_offset.h (GC_BASE__INST0_SEG0 = 0x00001260)
#define AMDBC250_GC_BASE                        0x1260

// GFX firmware type codes from Linux psp_gfx_if.h (used with IOCTL_PSP_RING_LOAD_IP_FW)
#define GFX_FW_TYPE_CP_ME      1
#define GFX_FW_TYPE_CP_PFP     2
#define GFX_FW_TYPE_CP_CE      3
#define GFX_FW_TYPE_CP_MEC     4
#define GFX_FW_TYPE_CP_MEC1    5
#define GFX_FW_TYPE_RLC_G      8
#define GFX_FW_TYPE_SDMA0      9
#define GFX_FW_TYPE_SDMA1      10

// PSP register program IDs (from Linux psp_reg_prog_id enum)
#define PSP_REG_IH_RB_CNTL         0
#define PSP_REG_IH_RB_CNTL_RING1   1
#define PSP_REG_IH_RB_CNTL_RING2   2
#define PSP_REG_MMHUB_L1_TLB_CNTL  25
#define PSP_REG_LAST               26

// Compat aliases
#define GFX_FW_TYPE_CE     GFX_FW_TYPE_CP_CE
#define GFX_FW_TYPE_PFP    GFX_FW_TYPE_CP_PFP
#define GFX_FW_TYPE_ME     GFX_FW_TYPE_CP_ME
#define GFX_FW_TYPE_MEC    GFX_FW_TYPE_CP_MEC
#define GFX_FW_TYPE_MEC2   GFX_FW_TYPE_CP_MEC1
#define GFX_FW_TYPE_RLC    GFX_FW_TYPE_RLC_G
#define GFX_FW_TYPE_SDMA   GFX_FW_TYPE_SDMA0

// PSP ring frame command IDs (from Linux psp_gfx_if.h)
#define GFX_CMD_ID_INIT_TMR     0x00000001
#define GFX_CMD_ID_LOAD_IP_FW   0x00000006
#define GFX_CMD_ID_PROG_REG     0x0000000B
#define GFX_CMD_ID_LOAD_TOC     0x00000020
#define GFX_CMD_ID_AUTOLOAD_RLC 0x00000021

// Ring frame: 64 bytes pointing to a 1024-byte command buffer
#define PSP_RING_FRAME_SIZE   64
#define PSP_CMD_BUF_SIZE      1024

// IOCTL input/output structures
#pragma pack(push, 1)

typedef struct _PSP_READ_REG_REQUEST {
    ULONG Offset;           // Register offset from BAR0 base
    ULONG Reserved;         // Padding
} PSP_READ_REG_REQUEST, *PPSP_READ_REG_REQUEST;

typedef struct _PSP_READ_REG_RESPONSE {
    ULONG Value;            // Register value read
    ULONG Status;           // NTSTATUS equivalent (0 = success)
} PSP_READ_REG_RESPONSE, *PPSP_READ_REG_RESPONSE;

typedef struct _PSP_WRITE_REG_REQUEST {
    ULONG Offset;           // Register offset from BAR0 base
    ULONG Value;            // Value to write
} PSP_WRITE_REG_REQUEST, *PPSP_WRITE_REG_REQUEST;

typedef struct _PSP_WRITE_REG_RESPONSE {
    ULONG Status;           // NTSTATUS equivalent (0 = success)
    ULONG Reserved;
} PSP_WRITE_REG_RESPONSE, *PPSP_WRITE_REG_RESPONSE;

// For IOCTL_PSP_GET_STATUS, output is comprehensive PSP snapshot
typedef struct _PSP_STATUS_INFO {
    ULONG C2PMSG_81;           // Raw C2PMSG_81 value
    ULONG C2PMSG_35;           // Raw C2PMSG_35 value
    ULONG C2PMSG_36;           // Raw C2PMSG_36 value
    ULONG PspAlive;            // 1 if C2PMSG_81 is valid (not 0, not 0xFFFFFFFF)
    ULONG FwLoaded;            // 1 if firmware buffer active
    ULONG FwSize;              // Firmware size in bytes
    ULONG FwPaShifted;         // PA>>20 of firmware buffer
    ULONG NbioSig1;            // NBIO signature register 0xC100
    ULONG NbioSig2;            // NBIO signature register 0xC180
    ULONG GrbmStatus;          // GRBM_STATUS (GC_BASE + 0x2000 = 0x3260)
    ULONG MmhubCheck;          // MMHUB check register (0x50D0)
    ULONG MmioVA;              // BAR5 virtual address
    ULONG MmioSize;            // BAR5 mapped size
    ULONG RingCreated;         // 1 if PSP ring is active
    ULONG C2PMSG_37;           // Raw C2PMSG_37 value
    ULONG C2PMSG_64;           // Raw C2PMSG_64 value (ring control)
    ULONG GcCheck;             // GC register (0x3000)
    ULONG HdpCheck;            // HDP register (0x05A0)
    ULONG MeCntl;              // CP_ME_CNTL (GC_BASE + 0x3814 = 0x4A74) — bit28=ME_HALT, bit30=PFP_HALT
    ULONG GrbmGfxIndex;        // GRBM_GFX_INDEX (GC_BASE + 0x2270 = 0x34D0) — selects ME/PIPE/QUEUE
} PSP_STATUS_INFO, *PPSP_STATUS_INFO;

// For IOCTL_PSP_INIT_HW, input is physical address + size
typedef struct _PSP_INIT_HW_REQUEST {
    ULONG64 PhysicalAddress;   // Physical address of BAR5
    ULONG Size;                // Size to map
} PSP_INIT_HW_REQUEST, *PPSP_INIT_HW_REQUEST;

// For IOCTL_PSP_LOAD_FW, input buffer contains the firmware blob
// Output buffer receives status
#define PSP_MAX_FW_SIZE       (1024 * 1024) // 1MB max firmware size
#define PSP_MAX_FW_TOTAL       (1024 * 1024) // 1MB total FW allocation limit

typedef struct _PSP_LOAD_FW_RESPONSE {
    ULONG Status;           // NTSTATUS equivalent
    ULONG MailboxStatus;    // C2PMSG_81 value after command
} PSP_LOAD_FW_RESPONSE, *PPSP_LOAD_FW_RESPONSE;

// Comprehensive HW probe output
typedef struct _PSP_PROBE_INFO {
    // Mailbox registers
    ULONG C2PMSG_35;
    ULONG C2PMSG_36;
    ULONG C2PMSG_37;
    ULONG C2PMSG_64;
    ULONG C2PMSG_81;
    // NBIO unlock results
    ULONG NbioSig1;
    ULONG NbioSig2;
    ULONG MmhubCheck;
    ULONG GrbmStatus;          // GRBM_STATUS (GC_BASE + 0x2000 = 0x3260)
    ULONG GcCheck;
    ULONG HdpCheck;
    // Ring state
    ULONG RingCreated;
    ULONG RingAddrLow;
    ULONG RingAddrHigh;
    ULONG RingSize;
    // Operation results
    ULONG SigWriteOk;
    ULONG RingProgOk;
    ULONG NbioViaRingOk;
} PSP_PROBE_INFO, *PPSP_PROBE_INFO;

// For IOCTL_PSP_RING_LOAD_IP_FW, input buffer contains this struct + firmware blob
typedef struct _PSP_RING_FW_REQUEST {
    ULONG FwType;       // GFX_FW_TYPE code (CE=1, PFP=2, ME=3, MEC=4, MEC2=5, RLC=6, SDMA=7, SDMA1=8)
    ULONG FwSize;       // Size of firmware blob that follows in the input buffer
    // Firmware data follows immediately in the input buffer after this struct
} PSP_RING_FW_REQUEST, *PPSP_RING_FW_REQUEST;

// For IOCTL_PSP_KIQ_LOAD_FW, input buffer contains this struct + firmware blob
typedef struct _PSP_KIQ_LOAD_FW_REQUEST {
    ULONG FwType;       // GFX_FW_TYPE code (CE=3, PFP=2, ME=1, etc.)
    ULONG FwSize;       // Size of firmware blob that follows in the input buffer
    // Firmware data follows immediately in the input buffer after this struct
} PSP_KIQ_LOAD_FW_REQUEST, *PPSP_KIQ_LOAD_FW_REQUEST;

// For IOCTL_PSP_LOAD_IP_FW_DIRECT — load GPU firmware via mailbox command
typedef struct _PSP_LOAD_IP_FW_REQUEST {
    ULONG FwType;       // GFX_FW_TYPE code (ME=1, PFP=2, CE=3, MEC=4, etc.)
    ULONG FwSize;       // Size of firmware blob that follows in the input buffer
    // Firmware data follows immediately in the input buffer after this struct
} PSP_LOAD_IP_FW_REQUEST, *PPSP_LOAD_IP_FW_REQUEST;

typedef struct _PSP_LOAD_IP_FW_RESPONSE {
    ULONG Status;           // NTSTATUS
    ULONG C2Pmsg35;         // Mailbox command register after
    ULONG C2Pmsg81;         // PSP status after command
    ULONG Reserved;
} PSP_LOAD_IP_FW_RESPONSE, *PPSP_LOAD_IP_FW_RESPONSE;

// Input for IOCTL_PSP_GPU_PM4_SUBMIT — submit GPU PM4 via PSP KIQ ring
typedef struct _PSP_GPU_PM4_SUBMIT_REQUEST {
    ULONG CommandCount;      // Number of PM4 DWORDs (max 64)
    ULONG Reserved;          // Alignment padding
    ULONG WaitMs;            // Wait time in ms after kick (0 = no wait)
    ULONG Commands[64];      // PM4 commands
} PSP_GPU_PM4_SUBMIT_REQUEST, *PPSP_GPU_PM4_SUBMIT_REQUEST;

// Output for IOCTL_PSP_GPU_PM4_SUBMIT
typedef struct _PSP_GPU_PM4_SUBMIT_RESPONSE {
    ULONG Status;            // NTSTATUS
    ULONG ScratchBefore;     // SCRATCH (0x32D4) before PM4
    ULONG ScratchAfter;      // SCRATCH (0x32D4) after wait
    ULONG WptrReadback;      // WPTR readback after kick
    ULONG KiqRingWptr;       // PSP KIQ ring WPTR
    ULONG KiqRingSize;       // PSP KIQ ring size
    ULONG KiqRingPa;         // PSP KIQ ring PA (low 32 bits)
    ULONG HqdPqWptrBefore;   // HQD_PQ_WPTR_LO before kick
    ULONG HqdPqWptrAfter;    // HQD_PQ_WPTR_LO after wait
    ULONG HqdActive;         // HQD_ACTIVE readback
    ULONG Pm4Dwords;         // Number of PM4 DWORDs written
} PSP_GPU_PM4_SUBMIT_RESPONSE, *PPSP_GPU_PM4_SUBMIT_RESPONSE;

// Output for IOCTL_PSP_KIQ_GET_STATUS
typedef struct _PSP_KIQ_STATUS {
    ULONG RingInitialized;    // 1 if KIQ ring is active
    ULONG RingWptr;           // Current write pointer
    ULONG RingSize;           // Ring buffer size in bytes
    ULONG RingPA;             // Physical address (low 32 bits)
} PSP_KIQ_STATUS, *PPSP_KIQ_STATUS;

// Output for IOCTL_PSP_GET_GPU_INFO — info GPU driver needs on init
typedef struct _PSP_GPU_INFO {
    ULONG RingBufferPA;       // Physical address of the PSP ring buffer (low 32 bits)
    ULONG FwLoaded;           // 1 if GPU FW loaded via ring
    ULONG FwCount;            // Number of GPU FW components loaded (max 8)
    ULONGLONG TMRBase;        // TMR base address (Linux uses 0xF40F800000)
    ULONG TMSSize;            // TMR size (4MB)
    ULONG GfxVersion;         // GPU IP version (cyan_skillfish2 = 10)
    ULONG C2pmsg64;           // Current C2PMSG_64 value
    ULONG C2pmsg81;           // Current C2PMSG_81 value (SOS alive = 0xF0000010)
    ULONG TmrInitialized;     // 1 if TMR has been initialized successfully
} PSP_GPU_INFO, *PPSP_GPU_INFO;

// Input for IOCTL_PSP_REG_PROG — program a register through PSP
// Uses GFX_CMD_ID_PROG_REG (0x0B) via the ring
typedef struct _PSP_REG_PROG_REQUEST {
    ULONG RegId;             // Register ID (see psp_reg_prog_id enum)
    ULONG RegValue;          // Value to program
} PSP_REG_PROG_REQUEST, *PPSP_REG_PROG_REQUEST;

// Input/output for IOCTL_PSP_KIQ_SUBMIT — submit PM4 commands to KIQ ring
typedef struct _PSP_KIQ_SUBMIT_REQUEST {
    ULONG CommandCount;      // Number of PM4 DWORDs (max 64)
    ULONG Reserved[3];       // Alignment padding
    ULONG Commands[64];      // PM4 commands
} PSP_KIQ_SUBMIT_REQUEST, *PPSP_KIQ_SUBMIT_REQUEST;

// Input for IOCTL_PSP_SMU_WAKE — send SMU command and read response
typedef struct _PSP_SMU_WAKE_REQUEST {
    ULONG Message;           // SMU message ID (e.g., 0x01=GetSmUInfo, 0x07=PowerGateIP)
    ULONG Argument;          // Argument to pass to SMU
    ULONG Reserved[2];
} PSP_SMU_WAKE_REQUEST, *PPSP_SMU_WAKE_REQUEST;

// Output for IOCTL_PSP_SMU_WAKE
typedef struct _PSP_SMU_WAKE_RESPONSE {
    ULONG Message;           // Echo of input message
    ULONG Argument;          // Argument passed in
    ULONG Response;          // Value read from C2PMSG_82 after command
    ULONG Status;            // C2PMSG_90 response (0=busy, 1=OK, 0xFF=error)
} PSP_SMU_WAKE_RESPONSE, *PPSP_SMU_WAKE_RESPONSE;

#pragma pack(pop)

#ifdef __cplusplus
}
#endif

#endif // __PSP_IOCTL_H__
