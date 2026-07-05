@echo off
REM Extract firmware arrays from firmware_data.h to .bin files
setlocal enabledelayedexpansion

set FW_HEADER=C:\AMD-BC-250\AMD-BC-250-PSP-Windows-Driver\inc\firmware_data.h
set OUT_DIR=C:\AMD-BC-250\AMD-BC-250-PSP-Windows-Driver\output\firmware

if not exist "%OUT_DIR%" mkdir "%OUT_DIR%"

REM Use PowerShell to parse and extract each firmware array
powershell -NoProfile -Command ^
"$content = Get-Content '%FW_HEADER%' -Raw; " ^
"$pattern = 'static const UCHAR g_(\w+)FirmwareData\[\] = \{([^}]+)\};'; " ^
"$matches = [regex]::Matches($content, $pattern); " ^
"foreach ($m in $matches) { " ^
"    $name = $m.Groups[1].Value; " ^
"    $hexStr = $m.Groups[2].Value; " ^
"    $bytes = $hexStr -split ',' | ForEach-Object { $_.Trim() } | Where-Object { $_ -ne '' } | ForEach-Object { [byte]('0x' + $_.Trim()) }; " ^
"    $outPath = '%OUT_DIR%\' + $name + '.bin'; " ^
"    [System.IO.File]::WriteAllBytes($outPath, $bytes); " ^
"    Write-Host ('  -> ' + $outPath + ' (' + $bytes.Length + ' bytes)'); " ^
"} " ^
"Write-Host 'Done.'"

echo.
echo Firmware files extracted to %OUT_DIR%
