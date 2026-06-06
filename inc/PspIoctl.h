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

// PSP Mailbox register offsets (relative to BAR0 base)
// These match the hardware addresses documented in the spec
#define PSP_C2PMSG_35_OFFSET  0x1056C   // Command register
#define PSP_C2PMSG_36_OFFSET  0x10570   // Data register  
#define PSP_C2PMSG_81_OFFSET  0x10614   // Status register

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
    ULONG GrbmStatus;          // GRBM_STATUS (0x2004)
    ULONG MmhubCheck;          // MMHUB check register (0x50D0)
    ULONG MmioVA;              // BAR5 virtual address
    ULONG MmioSize;            // BAR5 mapped size
    ULONG RingCreated;         // 1 if PSP ring is active
} PSP_STATUS_INFO, *PPSP_STATUS_INFO;

// For IOCTL_PSP_INIT_HW, input is physical address + size
typedef struct _PSP_INIT_HW_REQUEST {
    ULONG64 PhysicalAddress;   // Physical address of BAR5
    ULONG Size;                // Size to map
} PSP_INIT_HW_REQUEST, *PPSP_INIT_HW_REQUEST;

// For IOCTL_PSP_LOAD_FW, input buffer contains the firmware blob
// Output buffer receives status
#define PSP_MAX_FW_SIZE       (512 * 1024)  // 512KB max firmware size

typedef struct _PSP_LOAD_FW_RESPONSE {
    ULONG Status;           // NTSTATUS equivalent
    ULONG MailboxStatus;    // C2PMSG_81 value after command
} PSP_LOAD_FW_RESPONSE, *PPSP_LOAD_FW_RESPONSE;

#pragma pack(pop)

#ifdef __cplusplus
}
#endif

#endif // __PSP_IOCTL_H__
