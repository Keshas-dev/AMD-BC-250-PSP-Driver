# Sekantys Žingsniai — Prioritizuoti

## 1. 🔴 AUKŠTAS — GC_BASE offset'ai JAU PATAISYTI ✅

**Atlikta** — `inc/PspIoctl.h` turi `#define AMDBC250_GC_BASE 0x1260` ir `PspDriver.c` naudoja šį offset'ą visur.

```cmd
test-psp-driver.exe -i 0xFE800000 0x200000
test-psp-driver.exe -r 0x3260      # GRBM_STATUS - validi reikšmė
test-psp-driver.exe -r 0x3264      # CC_GC_SHADER_ARRAY_CONFIG - validi reikšmė  
test-psp-driver.exe -r 0x34FC      # SPI_PG_ENABLE_STATIC_WGP_MASK - validi reikšmė
```

## 2. 🔴 AUKŠTAS — Ring protokolas neveikia (HW apribojimas)

C2PMSG_64 bit 31 (TOS_READY/TOS_RESP) niekad nesetina. SOS firmware šioje BIOS versijoje nepalaiko GPCOM ring.

## 3. 🟡 VIDUTINIS — SDMA FW type pataisyta ✅

test-psp-driver.c `FwTypeName()` dabar teisingai atpažįsta: CE=3, PFP=2, ME=1, SDMA=9, SDMA1=10, RLC=8.

## 4. 🟡 VIDUTINIS — UEFI BIOS konfigūracija per RU.efi

**Tikslas**: Išjungti IOMMU ir pamatyti ar tai įtakoja NBIO elgesį.

- Boot iš USB → RU.efi → `Alt+=` → `Setup` (EC87D643...) → offset 0xD2 → `01→00`
- Taip pat bandyti: SVM Mode (0xB0→00), Above 4G (0xDA→01)
- Po pakeitimų: reboot → test GRBM offset'us

## 4. 🟢 GRĮŽTAS — 40 CU Unlock bandymas

GC_BASE patvirtintas. Bandyti GRBM/SHADER_CONFIG:

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
