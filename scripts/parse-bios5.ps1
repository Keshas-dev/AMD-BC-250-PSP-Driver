$rom = [System.IO.File]::ReadAllBytes('C:\AMD-BC-250\AMD-BC-250-Windows-Driver-main\third-party\Bios\BC250_5.00_clv.bin')
$romSize = $rom.Length
Write-Host ("BIOS 5.00 size: {0} bytes ({1}KB)" -f $romSize, [int]($romSize/1024))

# Known $PSP location from BIOS 3.00 analysis - check at same offset
$pspOffset = 0x8E0000
$sigCheck = ($rom[$pspOffset] -eq 0x24 -and $rom[$pspOffset+1] -eq 0x50 -and $rom[$pspOffset+2] -eq 0x53 -and $rom[$pspOffset+3] -eq 0x50)
if (-not $sigCheck) {
    Write-Host "`$PSP not at expected offset, scanning..."
    for ($i = 0; $i -lt $romSize - 4; $i += 4) {
        if ($rom[$i] -eq 0x24 -and $rom[$i+1] -eq 0x50 -and $rom[$i+2] -eq 0x53 -and $rom[$i+3] -eq 0x50) {
            $pspOffset = $i; break
        }
    }
}

Write-Host ("Found `$PSP at ROM offset 0x{0:X8}" -f $pspOffset) -ForegroundColor Green
        Write-Host ""
        Write-Host "  Entry Type    Size     ROM Offset"
        Write-Host "  ----- ----    ----     ----------"
        
        $entryCount = [System.BitConverter]::ToUInt32($rom, $i + 8)
        Write-Host ("  Table entries: {0}" -f $entryCount)
        
        for ($e = 0; $e -lt $entryCount; $e++) {
            $addr = $i + 16 + ($e * 32)
            $type = [System.BitConverter]::ToUInt32($rom, $addr)
            $size = [System.BitConverter]::ToUInt32($rom, $addr + 4)
            $offPhys = [System.BitConverter]::ToUInt32($rom, $addr + 8)
            $offFile = $offPhys -band 0xFFFFFF
            
            if ($type -eq 0xFFFFFFFF) { break }
            
            Write-Host ("  [{0}] type={1,-5} size={2,8} ({3,6}KB) off=0x{4:X6}" -f $e, $type, $size, [int]($size/1024), $offFile)
            
            # Extract SYSDRV (type 8, usually) and SOS (type 1)
            if ($type -eq 8 -and $offFile -lt $romSize -and $size -le 512*1024) {
                $fw = New-Object Byte[] $size
                [Array]::Copy($rom, $offFile, $fw, 0, $size)
                [System.IO.File]::WriteAllBytes('C:\AMD-BC-250\AMD-BC-250-PSP-Windows-Driver\Firmware\cyan_skillfish2_sysdrv.bin', $fw)
                Write-Host ("    => Extracted SYSDRV ({0}KB)" -f [int]($size/1024)) -ForegroundColor Yellow
            }
            if ($type -eq 1 -and $offFile -lt $romSize -and $size -le 512*1024) {
                $fw = New-Object Byte[] $size
                [Array]::Copy($rom, $offFile, $fw, 0, $size)
                # Pad to 256KB
                $padded = New-Object Byte[] 262144
                [Array]::Copy($fw, $padded, [Math]::Min($size, 262144))
                [System.IO.File]::WriteAllBytes('C:\AMD-BC-250\AMD-BC-250-PSP-Windows-Driver\Firmware\cyan_skillfish2_sos_42kb.bin', $padded)
                Write-Host ("    => Extracted SOS ({0}KB, padded to 256KB)" -f [int]($size/1024)) -ForegroundColor Yellow
            }
        }
        break
    }
}

Write-Host ""
Write-Host "Done. Check C:\AMD-BC-250\AMD-BC-250-PSP-Windows-Driver\Firmware\"
