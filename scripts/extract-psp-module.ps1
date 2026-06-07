$rom = [System.IO.File]::ReadAllBytes('C:\AMD-BC-250\AMD-BC-250-Windows-Driver-main\third-party\Bios\BC250_5.00_clv.bin')

# Search for Psp.Drv.AmdPspPeiV2 near FFS headers or PE32 images
# Only scan areas around where we'd expect UEFI modules (first 8MB is BIOS, rest is firmware)
$start = 6 * 1024 * 1024  # Start at 6MB
$end = [Math]::Min($rom.Length, 12 * 1024 * 1024)  # End at 12MB

# Search for "Psp.Drv.AmdPspPeiV2"
$pattern = [Text.Encoding]::ASCII.GetBytes('Psp.Drv.AmdPspPeiV2')
$foundPositions = @()
for ($i = $start; $i -lt $end; $i++) {
    $match = $true
    for ($j = 0; $j -lt $pattern.Length; $j++) {
        if ($rom[$i + $j] -ne $pattern[$j]) { $match = $false; break }
    }
    if ($match) {
        Write-Host ("Found at 0x{0:X8}" -f $i)
        $foundPositions += $i
        $i += $pattern.Length
        if ($foundPositions.Count -ge 3) { break }
    }
}

# For each found position, search backwards for PE/FFS header
foreach ($pos in $foundPositions) {
    Write-Host "`n=== Module near 0x{0:X8} ===" -f $pos
    
    # Search backwards for MZ (PE32) header within 64KB
    for ($i = $pos - 65536; $i -lt $pos; $i++) {
        if ($i -lt 0) { continue }
        if ($rom[$i] -eq 0x4D -and $rom[$i+1] -eq 0x5A) {
            Write-Host "  PE/MZ at 0x{0:X8} (offset from string: -0x{1:X})" -f $i, ($pos - $i)
            
            # Dump PE header info
            $peOffset = [BitConverter]::ToUInt32($rom, $i + 0x3C)
            $peStart = $i + $peOffset
            if ($peStart -lt $rom.Length - 100) {
                $machine = [BitConverter]::ToUInt16($rom, $peStart + 4)
                $sections = [BitConverter]::ToUInt16($rom, $peStart + 6)
                $entryRVA = [BitConverter]::ToUInt32($rom, $peStart + 40)
                Write-Host "  PE: Machine=0x{0:X4} Sections={1} EntryRVA=0x{2:X8}" -f $machine, $sections, $entryRVA
            }
            
            # Extract 128KB window around MZ header
            $extractSize = [Math]::Min(128 * 1024, $rom.Length - $i)
            $module = New-Object Byte[] $extractSize
            [Array]::Copy($rom, $i, $module, 0, $extractSize)
            $outPath = "C:\AMD-BC-250\AMD-BC-250-PSP-Windows-Driver\output\psp_pei_module.bin"
            [System.IO.File]::WriteAllBytes($outPath, $module)
            Write-Host "  Extracted {0}KB to {1}" -f [int]($extractSize/1024), $outPath -ForegroundColor Green
            break
        }
    }
}
