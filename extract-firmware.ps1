$inFile = "C:\AMD-BC-250\AMD-BC-250-PSP-Windows-Driver\inc\firmware_data.h"
$outDir = "C:\AMD-BC-250\AMD-BC-250-PSP-Windows-Driver\output\firmware"
if (!(Test-Path $outDir)) { New-Item -ItemType Directory -Path $outDir | Out-Null }

$lines = Get-Content $inFile
$currentName = $null
$currentArray = @()
$inArray = $false

foreach ($line in $lines) {
    # Match: static const ULONG g_SysdrvFirmwareSize = NNN;
    if ($line -match 'static const ULONG g_(\w+)FirmwareSize = (\d+);') {
        $name = $matches[1]
        $size = [int]$matches[2]
        Write-Host "  Size $name = $size"
        continue
    }
    
    # Match: static const UCHAR g_SysdrvFirmwareData[] = {
    if ($line -match 'static const UCHAR g_(\w+)FirmwareData\[\]\s*=') {
        $currentName = $matches[1]
        $currentArray = @()
        $inArray = $true
        # Check if data starts on same line
        if ($line -match '\{(.+)\};?$') {
            # Single line array (unlikely for firmware)
            $data = $matches[1]
        }
        Write-Host "  Found array: $currentName"
        continue
    }
    
    if ($inArray) {
        # Check for end of array
        if ($line -match '\};') {
            $dataPart = $line -replace '\};.*$', ''
            if ($dataPart.Trim() -ne '') {
                $currentArray += $dataPart
            }
            $inArray = $false
            # Write the .bin file
            $hexStr = ($currentArray -join ',').Trim()
            if ($hexStr -ne '') {
                $bytes = $hexStr -split ',' | ForEach-Object { 
                    $t = $_.Trim()
                    if ($t -ne '') { [byte]($t) }
                }
                $outPath = Join-Path $outDir "$currentName.bin"
                [System.IO.File]::WriteAllBytes($outPath, $bytes)
                Write-Host "  -> $outPath ($($bytes.Length) bytes)"
            }
            $currentName = $null
            $currentArray = @()
            continue
        }
        
        # Accumulate hex values from this line
        $clean = $line.Trim()
        if ($clean -ne '') {
            $currentArray += $clean -replace ',?\s*$', ''
        }
    }
}

Write-Host "Done."
