# BC250_5.00_clv.bin — BIOS 5.00 Analizė

## Bendra informacija
- **Failas**: `Firmware/BIOS/BC250_5.00_clv.bin` (16MB = 0x1000000)
- **Platform**: AMD Oberon (Zen 2) — PCI Host Bridge 0x1022:0x13E0
- **Codename**: "ROBIN" (rasta firmware `#ifdef ROBIN` kintamuosiuose)
- **Iš kur**: CLV (Clevo?) BIOS variantas, tikėtina iš ASRock BC-250 plokštės

## PSP lentelė ($PSP)
**Offset**: 0x8E0000 (nustatyta ieškant `$PSP` magic)
**Formatas**: `$PSP` + type(4) + size(4) + baseOffset(4) + reserved(4)

| Type | Reikšmė | Offset | Dydis | Aprašymas |
|------|---------|--------|-------|-----------|
| 1 | SOS | 0x8E0400 | **46592** (0xB600) | Pagrindinis SOS firmware |
| 8 | SYSDRV | 0x8FEE00 | 262656 (0x40200) | System Driver firmware |
| 68 | - | - | - | GPIO konfigūracija |
| 79 | - | - | - | Papildomas PSP blob |
| 81 | - | - | - | Trusted OS papildymai |
| 18,32,40,49,51 | - | - | - | Kiti PSP komponentai |

### Pastabos
- **SOS dydis** = 46592 baitai (0xB600), NE 43008 kaip buvo manyta iš pradžių
- **SYSDRV offset** = 0x8FEE00 (iš pradžių rašyta 0x8FF000 — tai buvo SMU loader offset)
- $PSP lentelė identiška tarp v3 (BC250_3.00.ROM) ir v5

## SOS vs v3 palyginimas
| Aspektas | v5 (5.00_clv) | v3 (3.00.ROM) |
|----------|---------------|---------------|
| SOS dydis | 46592 (0xB600) | 43008 (0xA800) |
| SOS skirtumas | - | ~94% skiriasi |
| SYSDRV dydis | 262656 | 262656 |
| SYSDRV skirtumas | - | ~61% skiriasi |
| Ring protokolas | ❌ NEPALAIKO | ❌ NEPALAIKO (tikėtina) |
| $PSP struktūra | Identiška | Identiška |

## SMU firmware
Iš SomnacinDumper-CPUCoreMod:
- SMU firmware SPI adresas: `0x8FEE00` (= SYSDRV!)
- Loader offset firmware: `0x1B340` → absoliutus `0x8FF240`
- C-payload offset: `0x3AE00` → absoliutus `0x8F8D00`
- BC-250: 8 CPU cores (0x13 config), 4700S: 6 cores (0x12 config)

## PSP firmware path (boot seka)
1. BIOS POST → SPI flash skaito ABL0 (0x962D00, 0x440 baitų)
2. ABL0 inicijuoja SecureOS (0x99E200)
3. SecureOS paleidžia SOS iš $PSP (type 1)
4. SOS pasiruošia C2PMSG mailbox protokolui
5. C2PMSG_81 = 0xF0000010 (stale BIOS loader error — SOS gyvas, bet bootloader signalizavo klaidą)
6. Mūsų `-B` seka įkelia šviežią SOS per mailbox → C2PMSG_81 = 0x00000000

## SMN (System Management Network) registrai
**Dostup per**: PCI config B0D0F0 + 0xB8 (addr) / 0xBC (data)
**Apribojimas**: Tik skaitymui — rašymas neveikia iš host'o

| SMN Adresas | Aprašymas |
|-------------|-----------|
| 0x115A81C | CPU core config (0x13=8 cores) |
| 0x115A870 | CPU core mask |
| 0x3220000 | TLB entries |
| 0x0154002C | SMU NBIF/IOMMU page table |
| 0x0154001C | SMU NBIF control |
| 0x015400F0 | SMU command register |
| 0x015400F4 | SMU argument |
| 0x015400F8 | SMU trigger (write 1 to execute) |
