$rom = [System.IO.File]::ReadAllBytes('C:\AMD-BC-250\AMD-BC-250-Windows-Driver-main\third-party\Bios\BC250_3.00_CHIPSETMENU.ROM')

# SOS: Entry 0, type=1, 43008 bytes at ROM offset 0x8E0400
$sos = New-Object Byte[] 43008
[Array]::Copy($rom, 0x8E0400, $sos, 0, 43008)
$padded = New-Object Byte[] 262144
[Array]::Copy($sos, $padded, 43008)
[System.IO.File]::WriteAllBytes('C:\AMD-BC-250\AMD-BC-250-PSP-Windows-Driver\Firmware\cyan_skillfish2_sos_42kb.bin', $padded)
Write-Host ('SOS 42KB done: size=' + $sos.Length) -ForegroundColor Green

# SYSDRV: Entry 1, type=8, 262656 bytes at ROM offset 0x8FF000
$sysdrv = New-Object Byte[] 262656
[Array]::Copy($rom, 0x8FF000, $sysdrv, 0, 262656)
[System.IO.File]::WriteAllBytes('C:\AMD-BC-250\AMD-BC-250-PSP-Windows-Driver\Firmware\cyan_skillfish2_sysdrv.bin', $sysdrv)
Write-Host ('SYSDRV 256KB done: size=' + $sysdrv.Length) -ForegroundColor Green

# Also generate updated firmware_data.h with BOTH arrays
$sb = New-Object System.Text.StringBuilder
[void]$sb.AppendLine('#ifndef __FIRMWARE_DATA_H__')
[void]$sb.AppendLine('#define __FIRMWARE_DATA_H__')
[void]$sb.AppendLine('')
[void]$sb.AppendLine('// SOS firmware (type 1, 42KB) from BIOS 0x8E0400')
[void]$sb.AppendLine("static const ULONG g_SosFirmwareSize = $($sos.Length);")
[void]$sb.AppendLine('static const UCHAR g_SosFirmwareData[] = {')
for ($i = 0; $i -lt $sos.Length; $i++) {
    if ($i % 16 -eq 0) { [void]$sb.Append('    ') }
    [void]$sb.AppendFormat('0x{0:X2}', $sos[$i])
    if ($i -lt $sos.Length - 1) { [void]$sb.Append(', ') }
    if (($i + 1) % 16 -eq 0) { [void]$sb.AppendLine('') }
}
[void]$sb.AppendLine('')
[void]$sb.AppendLine('};')
[void]$sb.AppendLine('')
[void]$sb.AppendLine('// SYSDRV firmware (type 8, 256KB) from BIOS 0x8FF000')
[void]$sb.AppendLine("static const ULONG g_SysdrvFirmwareSize = $($sysdrv.Length);")
[void]$sb.AppendLine('static const UCHAR g_SysdrvFirmwareData[] = {')
for ($i = 0; $i -lt $sysdrv.Length; $i++) {
    if ($i % 16 -eq 0) { [void]$sb.Append('    ') }
    [void]$sb.AppendFormat('0x{0:X2}', $sysdrv[$i])
    if ($i -lt $sysdrv.Length - 1) { [void]$sb.Append(', ') }
    if (($i + 1) % 16 -eq 0) { [void]$sb.AppendLine('') }
}
[void]$sb.AppendLine('')
[void]$sb.AppendLine('};')
[void]$sb.AppendLine('')
[void]$sb.AppendLine('#endif // __FIRMWARE_DATA_H__')
[System.IO.File]::WriteAllText('C:\AMD-BC-250\AMD-BC-250-PSP-Windows-Driver\inc\firmware_data.h', $sb.ToString())
Write-Host 'firmware_data.h generated with both SOS and SYSDRV' -ForegroundColor Green
