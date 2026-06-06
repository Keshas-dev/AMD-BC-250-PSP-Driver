// firmware_data.h
// Embedded firmware data for AMD BC-250 PSP Driver
// Contains SOS (Security Operating System) and SYSDRV firmware blobs

#ifndef __FIRMWARE_DATA_H__
#define __FIRMWARE_DATA_H__

#ifdef __cplusplus
extern "C" {
#endif

// Maximum firmware sizes (safety limits)
#define PSP_MAX_SOS_SIZE       (256 * 1024)   // 256 KB max for SOS
#define PSP_MAX_SYSDRV_SIZE    (256 * 1024)   // 256 KB max for SYSDRV
#define PSP_MAX_FW_TOTAL       (512 * 1024)   // 512 KB absolute max

// ============================================================================
// SOS FIRMWARE (Security Operating System)
// Type: 1 (PSP firmware type)
// Size: ~42 KB (padded to alignment)
// Usage: Load with command 0x08
// ============================================================================

extern UCHAR g_SosFirmwareData[];
extern ULONG g_SosFirmwareSize;

// ============================================================================
// SYSDRV FIRMWARE (System Driver)
// Type: 8 (System driver type)
// Size: ~256 KB
// Usage: Load with command 0x04
// ============================================================================

extern UCHAR g_SysdrvFirmwareData[];
extern ULONG g_SysdrvFirmwareSize;

// ============================================================================
// FIRMWARE VALIDATION HELPERS
// ============================================================================

static inline BOOLEAN FirmwareIsValid(PUCHAR FwData, ULONG FwSize)
{
    // Basic validation: non-null, minimum size, not all zeros/FFs
    if (FwData == NULL || FwSize < 256)
        return FALSE;
    
    if (FwSize < 1024 || FwSize > PSP_MAX_FW_TOTAL)
        return FALSE;
    
    // Check not all zeros
    ULONG sample1 = *(volatile ULONG*)FwData;
    ULONG sample2 = *(volatile ULONG*)(FwData + FwSize / 2);
    
    if (sample1 == 0 && sample2 == 0)
        return FALSE;
    
    // Check not all 0xFF
    if (sample1 == 0xFFFFFFFF && sample2 == 0xFFFFFFFF)
        return FALSE;
    
    return TRUE;
}

#ifdef __cplusplus
}
#endif

#endif // __FIRMWARE_DATA_H__
