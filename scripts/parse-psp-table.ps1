$rom = [System.IO.File]::ReadAllBytes('C:\AMD-BC-250\AMD-BC-250-Windows-Driver-main\third-party\Bios\BC250_3.00_CHIPSETMENU.ROM')
$pspOffset = 0x8E0000

# Entry structure: type(u32), size(u32), offset_low(u32), offset_high(u32)
for ($i = 0; $i -lt 19; $i++) {
    $addr = $pspOffset + 16 + ($i * 32)
    $type = [System.BitConverter]::ToUInt32($rom, $addr)
    $size = [System.BitConverter]::ToUInt32($rom, $addr + 4)
    $offLow = [System.BitConverter]::ToUInt32($rom, $addr + 8)
    
    # The ROM is mapped at physical address 0xFF000000
    # File offset = physical address & 0xFFFFFF
    $fileOff = $offLow -band 0xFFFFFF
    
    Write-Host ("[{0}] type={1} size={2,8} ({3,6}KB) phys=0x{4:X8} fileOff=0x{5:X6}" -f $i, $type, $size, [int]($size/1024), $offLow, $fileOff)
    
    # Extract type 1 (SOS firmware)
    if ($type -eq 1 -and $size -gt 0 -and $fileOff -lt $rom.Length) {
        Write-Host ("  => SOS firmware found! Extracting {0} bytes from file offset 0x{1:X6}..." -f $size, $fileOff) -ForegroundColor Green
        
        $sos = New-Object Byte[] $size
        [Array]::Copy($rom, $fileOff, $sos, 0, $size)
        
        # Pad to 256KB (262144 bytes)
        $padded = New-Object Byte[] 262144
        [Array]::Copy($sos, $padded, [Math]::Min($size, 262144))
        
        $outFile = 'C:\AMD-BC-250\AMD-BC-250-PSP-Windows-Driver\output\cyan_skillfish2_sos_extracted.bin'
        [System.IO.File]::WriteAllBytes($outFile, $padded)
        Write-Host ("  => Saved to {0} (padded to 256KB)" -f $outFile) -ForegroundColor Green
    }
}
