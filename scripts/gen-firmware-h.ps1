$bytes = [System.IO.File]::ReadAllBytes('C:\AMD-BC-250\AMD-BC-250-PSP-Windows-Driver\Firmware\cyan_skillfish2_sos_extracted.bin')
$sb = New-Object System.Text.StringBuilder
[void]$sb.AppendLine('#ifndef __FIRMWARE_DATA_H__')
[void]$sb.AppendLine('#define __FIRMWARE_DATA_H__')
[void]$sb.AppendLine('')
[void]$sb.AppendLine('// Embedded cyan_skillfish2 SOC firmware')
[void]$sb.AppendLine('// PSP table entry type=49, extracted from BIOS BC250_3.00_CHIPSETMENU.ROM')
[void]$sb.AppendLine("static const ULONG g_SosFirmwareSize = $($bytes.Length);")
[void]$sb.AppendLine('')
[void]$sb.AppendLine('static const UCHAR g_SosFirmwareData[] = {')
for ($i = 0; $i -lt $bytes.Length; $i++) {
    if ($i % 16 -eq 0) { [void]$sb.Append('    ') }
    [void]$sb.AppendFormat('0x{0:X2}', $bytes[$i])
    if ($i -lt $bytes.Length - 1) { [void]$sb.Append(', ') }
    if (($i + 1) % 16 -eq 0) { [void]$sb.AppendLine('') }
}
[void]$sb.AppendLine('')
[void]$sb.AppendLine('};')
[void]$sb.AppendLine('')
[void]$sb.AppendLine('#endif // __FIRMWARE_DATA_H__')
[System.IO.File]::WriteAllText('C:\AMD-BC-250\AMD-BC-250-PSP-Windows-Driver\inc\firmware_data.h', $sb.ToString())
Write-Host "firmware_data.h created ($($bytes.Length) bytes)" -ForegroundColor Green
