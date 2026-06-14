# BC-250 Hardware Registrai — GC_BASE, NBIO ir C2PMSG

## Svarbiausias atradimas: GC_BASE = 0x1260

BC-250 (Cyan Skillfish) naudoja **nestandartinį** registrų žemėlapį. Visi GC/GRBM/CP/SDMA registrai yra postūmti 0x1260 baitų Graphics BAR5 erdvėje, palyginus su standartine Navi10 architektūra.

**Šaltinis**: Linux kernel `cyan_skillfish_ip_offset.h`:
```
#define GC_BASE__INST0_SEG0 0x00001260
```

**Pasekmė**: Visiems registrams, kurie Navi10 yra offset'e 0x2000-0x2FFF, reikia pridėti GC_BASE.

**KODE NĖRA ĮGYVENDINTA** — driver vis dar skaito `0x2004` (Navi10 offset). Tai TURI BŪTI pakeista.

## Koreguoti Registrų Offset'ai (TEORIJA — reikia patvirtinti)

| Registras | Navi10 | BC-250 (GC_BASE + offset) | Statusas |
|-----------|--------|---------------------------|----------|
| GRBM_STATUS | 0x2000 | **0x3260** | ❌ Nepatvirtinta |
| CC_GC_SHADER_ARRAY_CONFIG | 0x2004 | **0x3264** | ❌ Nepatvirtinta |
| SPI_PG_ENABLE_STATIC_WGP_MASK | 0x229C | **0x34FC** | ❌ Nepatvirtinta |
| RLC_PG_ALWAYS_ON_WGP_MASK | 0x2B04 | **0x3D64** | ❌ Nepatvirtinta |
| CP scratch registers | 0x2074+ | **0x32D4+** | ❌ Nepatvirtinta |
| SDMA registers | 0x2600+ | **0x3860+** | ❌ Nepatvirtinta |

### Testavimo komandos (būtina paleisti!):
```cmd
test-psp-driver.exe -i 0xFE800000 0x200000
test-psp-driver.exe -r 0x3260      # GRBM_STATUS (tikėtina reikšmė ~0x009A0C00)
test-psp-driver.exe -r 0x3264      # SHADER_ARRAY_CONFIG
test-psp-driver.exe -r 0x34FC      # SPI_PG_ENABLE
```

## NBIO Firewall būsena

**IŠVADA**: NBIO NEblokuoja GC/GRBM registrų koreguotuose offset'uose.

Ankstesnis teiginys "NBIO blokuoja GRBM" buvo klaidingas — mes skaitėme neteisingus offset'us (0x2000-0x2FFF). Tai buvo **tuščia/unmapped atmintis**, ne NBIO firewall.

Patvirtinti NEblokuoti registrai:

| Range | Pavyzdys | Reikšmė | Offset | Kodėl veikia |
|-------|----------|---------|--------|--------------|
| GC | 0x3000 | 0x009A0C00 | 0x3000 | Tiesioginis GC adresas |
| MMHUB | 0x5000 | 0x80840000 | 0x5000 | Tiesioginis MMHUB |
| HDP | 0x05A0 | 0x00070000 | 0x05A0 | Tiesioginis HDP |
| NBIO SIG1 | 0xC100 | 0xFEDCBAEF | 0xC100 | Rašomas mūsų driver |
| NBIO SIG2 | 0xC180 | 0xFEDCBADF | 0xC180 | Rašomas mūsų driver |

## C2PMSG Mailbox Registrai (BAR5 + offset)

| Registry | Offset | Paskirtis |
|----------|--------|-----------|
| C2PMSG_35 | 0x1056C | Komandų registras (rašyti komandą, PSP išvalo kai padaryta) |
| C2PMSG_36 | 0x10570 | Duomenų registras (PA low 32b >> 20) |
| C2PMSG_37 | 0x10574 | Duomenų registras (PA high 32b) |
| C2PMSG_64 | 0x105E0 | Ring cmd/resp — bit 31 = TOS_READY/RESP flag |
| C2PMSG_65 | 0x105E4 | RBI ring wptr |
| C2PMSG_66 | 0x105E8 | RBI ring rptr |
| C2PMSG_67 | 0x105EC | GPCOM ring wptr |
| C2PMSG_68 | 0x105F0 | GPCOM ring rptr |
| C2PMSG_69 | 0x105F4 | Ring buffer addr low |
| C2PMSG_70 | 0x105F8 | Ring buffer addr high |
| C2PMSG_71 | 0x105FC | Ring buffer size |
| C2PMSG_81 | 0x10614 | Status/response (0xF0000010 = SOS alive but bootloader error, 0 = idle/success) |

## C2PMSG Mailbox Protokolas (patvirtintas)

```
1. WRITE C2PMSG_36 = PA >> 20    # Firmware physical address (low bits)
2. WRITE C2PMSG_37 = PA >> 32    # High bits (būtina >4GB!)
3. WRITE C2PMSG_35 = cmd         # Komanda: 0x4=SYSDRV, 0x8=SOS
4. POLL C2PMSG_35 until == 0     # PSP išvalo kai baigia (NE C2PMSG_81!)
5. Write 0 to C2PMSG_35          # ACK
6. NELIESTI C2PMSG_81            # PSP valdo šį registrą
```

### PSP klaidų kodai (C2PMSG_81)
| Reikšmė | Reikšmė |
|---------|---------|
| 0x00000000 | Idle / success |
| 0xF0000010 | Firmware validation failed |
| 0xF0000020 | Unknown command / timeout |

**Svarbu**: 0xF0000010 matomas iškart po INIT_HW — tai stale BIOS bootloader error. SOS vis tiek gali būti gyvas.

## Ring protokolas (NEPALAISOMAS)

Nors Linux psp_v11_0_8.c turi ring_create kodą, **mūsų SOS firmware (tiek v5, tiek v3) nepalaiko GPCOM ring protokolo**.

Simptomai:
- C2PMSG_64 = 0x00000000 visada (bit 31 niekad nesetina)
- Po C2PMSG_69/70/71 rašymo, readback grąžina 0 (tyliai atmetama)
- C2PMSG_65/66/67/68 = 0 visada

**Priežastis**: SOS firmware neturi TrustedOS komponento, kuris valdo ring protokolą.

**Pasekmė**: Neveikia: TMR init, GPU FW loading, ring-based PROG_REG.

## LM UART (debug serial)
Offset 0x10630-0x10700 rodo įdomias reikšmes:
- 0x10638: 0x47474747 ("GGGG") — init pattern
- 0x106A4: 0x426F306D ("Bo0m") — custom SOS signature (ne AMD standartas)
- 0x1063C: 0x00000008 — size/version
- 0x10674: 0xFFAA5500 — magic/signature
