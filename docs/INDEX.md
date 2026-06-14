# AMD BC-250 PSP Windows Driver — Dokumentacija

**BIOS versija**: BC250_5.00_clv.bin (v5.00)
**Lustas**: AMD Oberon (Zen 2, PCI 0x1022:0x13E0)
**Target**: `VEN_1022&DEV_143E` (AMD PSP)

## Turinys

| Dokumentas | Aprašymas |
|------------|-----------|
| `BIOS-ANALYSIS.md` | BIOS 5.00 struktūra, PSP lentelė, firmware išgavimas, SOS/SYSDRV dydžiai |
| `HARDWARE-REGISTERS.md` | Registrų žemėlapis, GC_BASE=0x1260, NBIO būsena, C2PMSG mailbox |
| `UEFI-VARIABLES.md` | BIOS Setup/AmdSetup kintamieji, IFR analizė, offset'ai, RU.efi strategija |
| `TEST-RESULTS.md` | Testų rezultatai, HW apribojimai, patvirtintos/atrastos problemos |
| `NEXT-STEPS.md` | Prioritizuoti sekantys žingsniai |
| `RETROSPEKTYVA.md` | Projekto retrospektyva (senesnė, papildoma info) |

## Trumpa santrauka

### Kas veikia
- BAR5 MMIO mapinimas (0xFE800000, 2MB)
- Mailbox komandos: CMD 0x4 (SYSDRV) + CMD 0x8 (SOS)
- NBIO atrakinimas (signature registrai 0xC100/0xC180)
- GC (0x3000+), MMHUB (0x5000+), HDP (0x05A0) registrai pasiekiami
- Boot seka puikiai veikia

### Kas neveikia
- Ring protokolas (C2PMSG_64 bit 31 niekad nesetina) — HW apribojimas
- TMR init — reikalingas ringas
- GRBM/CP registrai ties 0x2004 grąžina 0xFFFFFFFF

### Kritinis atradimas
GC_BASE = 0x1260 — BC-250 naudoja nestandartinį registrų žemėlapį. Visi GC/GRBM/CP registrai yra postūmti 0x1260 baitų BAR5 erdvėje. **Šis offset'as nėra pritaikytas kode** — vis dar naudojami seni Navi10 offset'ai.

### Kitas žingsnis
Patikrinti offset'us 0x3260, 0x3264, 0x34FC per driver'į. Jei reikšmės teisingos — atnaujinti kodą su GC_BASE konstanta.
