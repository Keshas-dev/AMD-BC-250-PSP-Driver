param(
    [string]$OutputFile = 'C:\AMD-BC-250\AMD-BC-250-PSP-Windows-Driver\inc\firmware_data.h',
    [string]$SosSource = 'C:\AMD-BC-250\AMD-BC-250-PSP-Windows-Driver\output\v3\sos_3_00.bin',
    [string]$SysdrvSource = 'C:\AMD-BC-250\AMD-BC-250-PSP-Windows-Driver\output\v3\sysdrv.bin',
    [string]$LabelPrefix = 'v3'
)

$sosData = [System.IO.File]::ReadAllBytes($SosSource)
$sysdrvData = [System.IO.File]::ReadAllBytes($SysdrvSource)

$sosFinal = $sosData
$sosLen = $sosData.Length

$sysdrvLen = [Math]::Min($sysdrvData.Length, 262144)
$sysdrvFinal = New-Object Byte[] 262144
[Array]::Copy($sysdrvData, $sysdrvFinal, $sysdrvLen)

Write-Host ("SOS (v3): {0} bytes" -f $sosData.Length)
Write-Host ("SYSDRV (v3): {0} bytes (padded to 262144)" -f $sysdrvData.Length)

$sb = New-Object System.Text.StringBuilder
[void]$sb.AppendLine('#ifndef __FIRMWARE_DATA_H__')
[void]$sb.AppendLine('#define __FIRMWARE_DATA_H__')
[void]$sb.AppendLine('')
[void]$sb.AppendLine("// SOS firmware (type 1, 42KB) from BIOS $LabelPrefix PSP table entry 0")
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
[void]$sb.AppendLine("// SYSDRV firmware (type 8, 256KB) from BIOS $LabelPrefix PSP table entry 1")
[void]$sb.AppendLine("static const ULONG g_SysdrvFirmwareSize = $($sysdrvFinal.Length);")
[void]$sb.AppendLine('static const UCHAR g_SysdrvFirmwareData[] = {')
for ($i = 0; $i -lt $sysdrvFinal.Length; $i++) {
    if ($i % 16 -eq 0) { [void]$sb.Append('    ') }
    [void]$sb.AppendFormat('0x{0:X2}', $sysdrvFinal[$i])
    if ($i -lt $sysdrvFinal.Length - 1) { [void]$sb.Append(', ') }
    if (($i + 1) % 16 -eq 0) { [void]$sb.AppendLine('') }
}
[void]$sb.AppendLine('')
[void]$sb.AppendLine('};')
[void]$sb.AppendLine('')
[void]$sb.AppendLine('#endif // __FIRMWARE_DATA_H__')
[System.IO.File]::WriteAllText($OutputFile, $sb.ToString())
Write-Host ("firmware_data.h generated from $LabelPrefix blobs - SOS=$sosLen SYSDRV=$sysdrvLen") -ForegroundColor Green
