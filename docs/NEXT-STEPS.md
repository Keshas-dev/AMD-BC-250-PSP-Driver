# Sekantys Žingsniai — Prioritizuoti

## 1. 🔴 AUKŠTAS — Patvirtinti GC_BASE offset'us

**Kritiškiausias** — atnaujinti kodo prielaidas.

```cmd
test-psp-driver.exe -i 0xFE800000 0x200000
test-psp-driver.exe -r 0x3260      # Tikėtina: ne 0xFFFFFFFF
test-psp-driver.exe -r 0x3264      # Tikėtina: validi reikšmė
test-psp-driver.exe -r 0x34FC      # Tikėtina: validi reikšmė
```

Jei patvirtinta:
- Pridėti `#define PSP_GC_BASE 0x1260` į `inc/PspIoctl.h`
- Atnaujinti visus `0x2004` → `GC_BASE + 0x2004`
- Atnaujinti test tool, kad palaikytų simbolinius offset'us

## 2. 🔴 AUKŠTAS — Pakeisti GRBM offset driver'yje

Šiuo metu `PspDriver.c` naudoja `0x2004` GRBM_STATUS ir kitiems registrams. Reikia pakeisti į `0x3260+`.

## 3. 🟡 VIDUTINIS — UEFI BIOS konfigūracija per RU.efi

**Tikslas**: Išjungti IOMMU ir pamatyti ar tai įtakoja NBIO elgesį.

- Boot iš USB → RU.efi → `Alt+=` → `Setup` (EC87D643...) → offset 0xD2 → `01→00`
- Taip pat bandyti: SVM Mode (0xB0→00), Above 4G (0xDA→01)
- Po pakeitimų: reboot → test GRBM offset'us

## 4. 🟡 VIDUTINIS — 40 CU Unlock bandymas

Jei GC_BASE patvirtinta ir GRBM/SHADER_CONFIG pasiekiami:
```cmd
test-psp-driver.exe -w 0x3264 0xFFE00000   # CC_GC_SHADER_ARRAY_CONFIG
test-psp-driver.exe -w 0x34FC 0x0000001F   # SPI_PG_ENABLE
```

## 5. 🟢 ŽEMAS — CSHA alternativa ring'ui

Linux psp_v11_0_8 turi CSHA (hardware crypto) komandas. Galima tirti ar CSHA gali atlikti TMR setup be ring protokolo. Reikia Linux kernel šaltinio analizės.

## 6. 🟢 ŽEMAS — v3 BIOS testavimas

Jei v5 SOS visiškai nepalaiko ring creation, galima:
- Flash v3 BIOS (`BC250_3.00.ROM`)
- Testuoti ring creation iš naujo
- Jei v3 irgi nepalaiko → ring creation yra HW limitation, ne BIOS versijos klausimas

## 7. 🟢 ŽEMAS — GPU driver proxy integracija

AGENTS.md dokumentuoja GPU driver proxy API (IOCTL 0x803-0x815). Jei ring creation neįmanomas, galima:
- Naudoti mailbox-based PROG_REG (jau veikia, bet write ignoruojamas)
- Plėtoti tiesioginį MMIO read/write per BAR5
- Sukurti user-mode GPU kontrolerį

## 8. 🟢 ŽEMAS — Dokumentacija
- Išvalyti AGENTS.md (didelis, daug pasikartojančių/istorinių fragmentų)
- Perkelti visą naujausią info į `docs/`
- Ištrinti pasenusius failus (pvz., `tikslas.txt`)
