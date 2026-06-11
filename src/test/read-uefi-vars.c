#include <windows.h>
#include <stdio.h>
#include <string.h>

int main() {
    const char *names[] = {
        "Setup", "AmdSetup", "CbsSetup", "SaSetup", "PchSetup",
        "CpuSetup", "CustomSetup", "AmiSetup", NULL
    };
    const char *guids[] = {
        "{EC87D643-EBA4-4BB5-A1E5-3F3E36B20DA9}",
        "{8BE4DF61-93CA-11D2-AA0D-00E098032B8C}",
        "{A04A27F4-DEF6-4B41-9F1B-89A5D476E34C}",
        "{899407D7-99FE-42D2-BC22-1223429DC98E}",
        NULL
    };

    BYTE buf[65536];
    DWORD size;
    int found = 0;

    for (int g = 0; guids[g]; g++) {
        for (int n = 0; names[n]; n++) {
            size = GetFirmwareEnvironmentVariableA(names[n], guids[g], buf, sizeof(buf));
            if (size == 0) {
                if (GetLastError() != ERROR_INVALID_FUNCTION &&
                    GetLastError() != ERROR_PRIVILEGE_NOT_HELD &&
                    GetLastError() != ERROR_NOT_FOUND &&
                    GetLastError() != 0x3EB) {
                    // Silent skip
                }
                continue;
            }
            found = 1;
            printf("FOUND: Name='%s' GUID=%s Size=%lu bytes\n", names[n], guids[g], size);
            for (DWORD i = 0; i < size; i += 16) {
                printf("  [0x%06lX] ", i);
                for (int j = 0; j < 16 && (i + j) < size; j++)
                    printf("%02X ", buf[i + j]);
                printf(" ");
                for (int j = 0; j < 16 && (i + j) < size; j++)
                    printf("%c", buf[i + j] >= 32 && buf[i + j] <= 126 ? buf[i + j] : '.');
                printf("\n");
            }
            printf("\n");
        }
    }
    if (!found)
        printf("No UEFI setup variables found.\nRun as Administrator!\n");
    return 0;
}
