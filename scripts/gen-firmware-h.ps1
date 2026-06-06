$fwDir = 'C:\AMD-BC-250\AMD-BC-250-PSP-Windows-Driver\Firmware'

# Read the firmware files from the Firmware directory (extracted from BIOS 5.00)
$sosData = [System.IO.File]::ReadAllBytes("$fwDir\cyan_skillfish2_sos_42kb.bin")

# Pad SOS to 256KB if needed (the file from BIOS 5.00 may be different size)
$sosFinal = New-Object Byte[] 262144
$sosLen = [Math]::Min($sosData.Length, 262144)
[Array]::Copy($sosData, $sosFinal, $sosLen)

$sysdrvData = [System.IO.File]::ReadAllBytes("$fwDir\cyan_skillfish2_sysdrv.bin")
$sysdrvLen = [Math]::Min($sysdrvData.Length, 262144)

Write-Host ("SOS: {0} bytes (padded to 262144)" -f $sosData.Length)
Write-Host ("SYSDRV: {0} bytes" -f $sysdrvData.Length)

$sb = New-Object System.Text.StringBuilder
[void]$sb.AppendLine('#ifndef __FIRMWARE_DATA_H__')
[void]$sb.AppendLine('#define __FIRMWARE_DATA_H__')
[void]$sb.AppendLine('')
[void]$sb.AppendLine('// SOS firmware (type 1, 46KB) from BIOS 5.00 PSP table entry 0')
[void]$sb.AppendLine("static const ULONG g_SosFirmwareSize = $($sosData.Length);")
[void]$sb.AppendLine('static const UCHAR g_SosFirmwareData[] = {')
for ($i = 0; $i -lt $sosFinal.Length; $i++) {
    if ($i % 16 -eq 0) { [void]$sb.Append('    ') }
    [void]$sb.AppendFormat('0x{0:X2}', $sosFinal[$i])
    if ($i -lt $sosFinal.Length - 1) { [void]$sb.Append(', ') }
    if (($i + 1) % 16 -eq 0) { [void]$sb.AppendLine('') }
}
[void]$sb.AppendLine('')
[void]$sb.AppendLine('};')
[void]$sb.AppendLine('')
[void]$sb.AppendLine('// SYSDRV firmware (type 8, 256KB) from BIOS 5.00 PSP table entry 1')
[void]$sb.AppendLine("static const ULONG g_SysdrvFirmwareSize = $($sysdrvData.Length);")
[void]$sb.AppendLine('static const UCHAR g_SysdrvFirmwareData[] = {')
for ($i = 0; $i -lt $sysdrvData.Length; $i++) {
    if ($i % 16 -eq 0) { [void]$sb.Append('    ') }
    [void]$sb.AppendFormat('0x{0:X2}', $sysdrvData[$i])
    if ($i -lt $sysdrvData.Length - 1) { [void]$sb.Append(', ') }
    if (($i + 1) % 16 -eq 0) { [void]$sb.AppendLine('') }
}
[void]$sb.AppendLine('')
[void]$sb.AppendLine('};')
[void]$sb.AppendLine('')
[void]$sb.AppendLine('#endif // __FIRMWARE_DATA_H__')
[System.IO.File]::WriteAllText('C:\AMD-BC-250\AMD-BC-250-PSP-Windows-Driver\inc\firmware_data.h', $sb.ToString())
Write-Host "firmware_data.h generated - SOS=$sosLen SYSDRV=$sysdrvLen" -ForegroundColor Green
