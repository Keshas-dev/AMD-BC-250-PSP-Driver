# UEFI Setup Variables — BIOS 5.00 Analizė

## Šaltinis
Failas: `Firmware/BIOS/setup-menu-5-clv.sct.0.0.en-US.uefi.ifr.txt` (2472 eilutės)
Išgauta iš SCT failo `setup-menu-5-clv.sct` naudojant IFR extractor.

## Setup variable

**GUID**: `{EC87D643-EBA4-4BB5-A1E5-3F3E36B20DA9}`
**VarStore ID**: 0x1
**Dydis**: 0x1D3 (467 baitai)
**Prieinamumas**: `EFI_VARIABLE_BOOTSERVICE_ACCESS` — NEPASIEKIAMA iš Windows OS

### Offset'ai

| Offset | Dydis | Nustatymas | Reikšmės |
|--------|-------|------------|----------|
| 0xB0 (176) | 1 | SVM Mode (Virtualization) | 00=Disabled, 01=Enabled |
| 0xD2 (210) | 1 | **IOMMU** | **00=Disabled, 01=Enabled** |
| 0xD5 (213) | 1 | Bank Interleaving | 00=Disabled, 01=Enabled |
| 0xD6 (214) | 1 | Channel Interleaving | 00=Disabled, 01=Enabled |
| 0xD7 (215) | 1 | Memory Clock Speed | 00=Auto, 01-05=specifiniai |
| 0xD8 (216) | 1 | Memory Clear | 00=Disabled, 01=Enabled |
| 0xDA (218) | 1 | Above 4G Decoding | 00=Disabled, 01=Enabled |
| 0xDB (219) | 1 | SR-IOV Support | 00=Disabled, 01=Enabled |
| 0x1C9 (457) | 1 | VCORE Fix Voltage | 00=Disabled, 01=Enabled |
| 0x1CC (460) | 1 | VCORE Loadline | 00=Auto, 01-05=Ohm |
| 0x1CD (461) | 1 | GFX Fix Voltage | 00=Disabled, 01=Enabled |
| 0x1D0 (464) | 1 | GFX Loadline | 00=Disabled, 01-05=Ohm |

### Strategija IOMMU išjungimui
1. Boot iš USB su RU.efi
2. RU.efi → `Alt+=` → UEFI Variables
3. Surasti `Setup` (GUID EC87D643-EBA4-4BB5-A1E5-3F3E36B20DA9)
4. Spausti Enter → matyti hex editor
5. Eiti į offset 0xD2 (210)
6. Pakeisti reikšmę iš `01` (Enabled) į `00` (Disabled)
7. Ctrl+W (save) → Exit → Reboot

### NBIO / GFX Configuration formos
- **GFX Configuration** (FormId 0x285B): Egzistuoja, bet **TUŠČIA** — tik subtitle'ai, jokių nustatymų
- **North Bridge** (FormId 0x285C): Taip pat tuščia
- Nėra NBIO unlock toggle BIOS nustatymuose

### Papildomi SCT failai
| Failas | Turinys |
|--------|---------|
| `BC250_5.00_clv.sct` | Originalus SCT (iš v5 BIOS) |
| `setup-menu-modifikuotas.sct` | Modifikuota versija su VDDC_GFX, GFX/VCORE voltage (galbūt iš v3 BIOS varianto) |
| `setup-menu-5-clv.sct` | Išgautas iš v5, grynai formų aprašymas |

## Windows API problemos

`GetFirmwareEnvironmentVariableW(Setup, EC87D643-EBA4-4BB5-A1E5-3F3E36B20DA9, ...)`:
- Grąžina **error 1314** (ERROR_PRIVILEGE_NOT_HELD) net kaip Administratorius
- Priežastis: kintamasis pažymėtas `BOOTSERVICE_ACCESS` — dingsta iš OS runtime namespace

**Sprendimas**: Tik RU.efi (UEFI shell) gali keisti šiuos kintamuosius. Windows GUI (`BC250UefiGui.exe`) yra convenience wrapper, bet neveiks su setup kintamaisiais.
