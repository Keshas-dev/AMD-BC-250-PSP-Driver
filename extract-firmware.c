// Extract firmware arrays from firmware_data.h to .bin files
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <windows.h>

int main(void) {
    const char* inFile = "C:\\AMD-BC-250\\AMD-BC-250-PSP-Windows-Driver\\inc\\firmware_data.h";
    const char* outDir = "C:\\AMD-BC-250\\AMD-BC-250-PSP-Windows-Driver\\output\\firmware\\";
    FILE* f = fopen(inFile, "rb");
    if (!f) { printf("FAIL: Cannot open %s\n", inFile); return 1; }

    fseek(f, 0, SEEK_END);
    long fileSize = ftell(f);
    fseek(f, 0, SEEK_SET);
    char* buf = (char*)malloc(fileSize + 1);
    fread(buf, 1, fileSize, f);
    fclose(f);
    buf[fileSize] = 0;

    char* p = buf;
    while (*p) {
        // Find array declaration: static const UCHAR g_XXXXFirmwareData[] = {
        char* start = strstr(p, "static const UCHAR g_");
        if (!start) break;

        char* nameStart = start + 20; // after "static const UCHAR g_"
        char* nameEnd = strstr(nameStart, "FirmwareData");
        if (!nameEnd) { p = start + 1; continue; }

        // Extract name length
        int nameLen = (int)(nameEnd - nameStart);
        char fwName[256];
        strncpy_s(fwName, sizeof(fwName), nameStart, nameLen);
        fwName[nameLen] = 0;

        // Find opening brace
        char* brace = strchr(nameEnd, '{');
        if (!brace) { p = nameEnd + 1; continue; }

        // Find closing brace
        char* closeBrace = strchr(brace, '}');
        if (!closeBrace) { p = brace + 1; continue; }

        // Parse hex bytes between braces
        char* hex = brace + 1;
        char* end = closeBrace;

        // Count commas to estimate size
        int commaCount = 0;
        for (char* c = hex; c < end; c++) if (*c == ',') commaCount++;
        if (commaCount == 0) { p = end + 1; continue; }

        unsigned char* data = (unsigned char*)malloc(commaCount + 1);
        int dataLen = 0;

        char* token = hex;
        while (token < end) {
            // Skip whitespace
            while (*token == ' ' || *token == '\t' || *token == '\n' || *token == '\r') token++;
            if (token >= end) break;

            // Parse hex value like "0xAB" or "0XAB"
            if (token[0] == '0' && (token[1] == 'x' || token[1] == 'X')) {
                unsigned int val;
                sscanf_s(token + 2, "%x", &val);
                if (val <= 0xFF) {
                    data[dataLen++] = (unsigned char)val;
                }
            }

            // Move to next token (past comma)
            while (token < end && *token != ',') token++;
            if (*token == ',') token++;
        }

        printf("  %-6s: %d bytes\n", fwName, dataLen);

        // Write .bin file
        char outPath[512];
        sprintf_s(outPath, sizeof(outPath), "%s%s.bin", outDir, fwName);
        FILE* out = fopen(outPath, "wb");
        if (out) {
            fwrite(data, 1, dataLen, out);
            fclose(out);
            printf("         -> %s\n", outPath);
        }

        free(data);
        p = end + 1;
    }

    free(buf);
    printf("Done.\n");
    return 0;
}
