# Testų Rezultatai — BIOS 5.00 (BC250_5.00_clv.bin)

## Pradinė būsena
- Driver: PspDriver.sys (signed, installed)
- Test tool: test-psp-driver.exe
- Windows: Test mode ON, Secure Boot OFF
- BIOS: v5.00 (BC250_5.00_clv.bin)

## Testų rezultatai

### 1. INIT_HW (BAR5 mapinimas)
```cmd
test-psp-driver.exe -i 0xFE800000 0x200000
```
**Rezultatas**: ✅ VA=0x62800000
**PA**: 0xFE800000 (Graphics BAR5, 2MB)
**Pastaba**: C2PMSG mailbox registrai pasiekiami per BAR5, ne PSP BAR0

### 2. Connectivity test
```cmd
test-psp-driver.exe -t
```
**Rezultatas**: ✅
- C2PMSG_35: 0x00000000 (idle)
- C2PMSG_36: kinta
- C2PMSG_81: 0xF0000010 (stale BIOS error)

### 3. Boot seka
```cmd
test-psp-driver.exe -B
```
**Rezultatas**: ✅ FW loaded, SYSDRV 0x4 sent + OK, SOS 0x8 sent + OK

### 4. Status
```cmd
test-psp-driver.exe -s
```
**Rezultatas**: ✅
- PSP alive: YES
- FW loaded: YES
- NBIO unlock: YES (SIGs written)

### 5. Registry read (unlocked range)
| Komanda | Offset | Rezultatas | Statusas |
|---------|--------|-----------|----------|
| `-r 0x3000` | GC base | 0x009A0C00 | ✅ |
| `-r 0x5000` | MMHUB | 0x80840000 | ✅ |
| `-r 0x05A0` | HDP | 0x00070000 | ✅ |
| `-r 0xC100` | NBIO SIG1 | 0xFEDCBAEF | ✅ |
| `-r 0xC180` | NBIO SIG2 | 0xFEDCBADF | ✅ |

### 6. Registry read (GRBM — potential GC_BASE issue)
| Komanda | Offset | Rezultatas | Statusas |
|---------|--------|-----------|----------|
| `-r 0x2004` | GRBM (Navi10) | 0xFFFFFFFF | ❌ |
| `-r 0x3260` | GRBM (BC-250 teoriškai) | ❌ **NEPATIKRINTA** |
| `-r 0x3264` | SHADER_ARRAY (BC-250) | ❌ **NEPATIKRINTA** |
| `-r 0x34FC` | SPI_PG (BC-250) | ❌ **NEPATIKRINTA** |

### 7. Ring creation
```cmd
test-psp-driver.exe -R
```
**Rezultatas**: ❌ err=21 (STATUS_NOT_READY)
**Priežastis**: C2PMSG_64 bit 31 niekad nesetina — HW/firmware apribojimas

### 8. TMR init
```cmd
test-psp-driver.exe -M
```
**Rezultatas**: ❌ err=21 (reikalingas ringas)

### 9. Comprehensive HW probe
```cmd
test-psp-driver.exe -H
```
**Rezultatas**: ✅ (info pateikta)

## Atrasti HW apribojimai

1. **Ring protokolas nepalaikomas** — šio BIOS SOS firmware neturi GPCOM/RBI ring palaikymo
2. **C2PMSG_64 bit 31 niekad nesetina** — nėra TrustedOS ready signalo
3. **C2PMSG_69/70/71 rašymas tyliai atmetamas** — readback grąžina 0
4. **SMN rašymas neveikia** iš host'o per PCI config (tik skaitymas)
5. **0x2004-0x2FFF** grąžina 0xFFFFFFFF — tikėtina dėl neteisingo GC_BASE offset'o, ne NBIO firewall

## Neatrasti dalykai (reikia tirti)
- Ar 0x3260, 0x3264, 0x34FC grąžina teisingas reikšmes?
- Ar pakeitus IOMMU (offset 0xD2→00) per RU.efi pasikeis NBIO elgesys?
- Ar v3 BIOS SOS palaiko ring protokolą?
- Ar galima CSHA (hardware crypto) panaudoti TMR setup be ring?
