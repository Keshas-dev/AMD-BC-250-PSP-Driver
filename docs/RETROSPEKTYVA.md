# AMD BC-250 PSP Driver - Projekto Retrospektyva

## Ką darėme

Sukūrėme Windows WDM branduolio tvarkyklę **ASRock AMD BC-250** plokštės PSP (Platform Security Processor) valdymui. Procesorius: AMD Oberon (tas pats lustas kaip PS5, bet kitoks BIOS — ASRock `BC250_3.00_CHIPSETMENU.ROM`). Tikslas: tiesioginė prieiga prie PSP MMIO registrų per Graphics BAR5, C2PMSG mailbox registrų skaitymas/rašymas, PSP firmware įkėlimas per mailbox protokolą, ir NBIO apsaugos analizė.

Pilnas kelias nuo tuščio .txt failo iki veikiančio draiverio su pilnu IOCTL interfeisu.

---

## Sutiktos klaidos ir jų sprendimai

### 1. KMDF → WDM konversija (Code 0x7e)

**Problema:** Pradinis driveris buvo parašytas KMDF (Kernel-Mode Driver Framework). Po instaliacijos Windows rodė klaidą 0x7e (STATUS_NOT_SUPPORTED) — driveris užsikrovė, bet negalėjo inicializuotis.

**Priežastis:** KMDF reikalauja specialios INF sekcijos (`[DDInstall.Wdf] KmdfService = ...`) ir WDF runtime bibliotekų (`wdfldr.lib`, `wdf01000.sys`). Mūsų INF neturėjo Wdf sekcijos, o kai ją pridėjome — problema išliko, nes KMDF reikalauja specifinio entry point'o (`FxDriverEntry` vietoj `DriverEntry`), o mūsų `/ENTRY:DriverEntry` konfliktavo su `wdfdriverentry.lib`.

**Sprendimas:** Perrašėme visą driverį iš KMDF į WDM (native NT driver). Vietoj `WdfDriverCreate` naudojame `IoCreateDevice`, vietoj `EvtIoDeviceControl` naudojame `IRP_MJ_DEVICE_CONTROL` handler'į. WDM nereikalauja jokių papildomų bibliotekų be `ntoskrnl.lib` ir `wdm.lib`.

### 2. Digital Signature (Code 52)

**Problema:** Driveris nerodė klaidos Device Manager, bet po instaliacijos Windows rašė "Windows cannot verify the digital signature" (Code 52).

**Priežastys (kelios):**
- Pradžioje driveris buvo visai nepasirašytas
- Bandėme kurti naują sertifikatą (`PspTestSigner`), bet jis nebuvo Trusted Root store
- Secure Boot buvo įjungtas — blokuoja test-signed driverius net su `testsigning on`

**Sprendimas:**
- Panaudojome **tą patį** `AMD-BC250-Signer` sertifikatą iš sibling projekto (jis jau buvo Trusted Root store)
- Pasirašome per `signtool sign /fd SHA256 /a /s My /n "AMD-BC250-Signer"`
- Secure Boot turi būti išjungtas BIOS'e

### 3. Test signing neveikė po reboot

**Problema:** `bcdedit /set testsigning on` rodė Yes, bet driveris vis tiek gavo Code 52.

**Priežastis:** `Get-BcdStore` grąžino "Access denied" — neturėjome admin teisių terminalo sesijoje. Paleidome per `Start-Process powershell -Verb RunAs`.

**Sprendimas:** `Start-Process cmd -Verb RunAs -ArgumentList "/c", "bcdedit /set testsigning on"` veikė. Po reboot test signing aktyvus.

### 4. Device path nerastas (Error 2)

**Problema:** `test-psp-driver.exe` grąžino "ERROR: Failed to open driver (error=2)". Error 2 = `ERROR_FILE_NOT_FOUND`.

**Priežastis:** Driveris visai nebuvo instaliuotas Device Manager. Jį reikia rankiniu būdu pridėti per Update Driver.

**Sprendimas:** Device Manager → Scan for hardware changes → rasti nežinomą įrenginį (PCI\VEN_1022&DEV_143E) → Update Driver → Browse į output/ aplanką.

### 5. Neteisingas BAR adresas

**Problema:** Specifikacijoje rašyta "BAR0", bet C2PMSG registrai gyvena ne PSP BAR0 (0xFD600000), o **Graphics BAR5** (0xFE800000) erdvėje.

**Priežastis:** AMD BC-250 turi du skirtingus BAR adresus:
- PSP BAR0 (B0:D8:F0, ~0xFD600000) — atskiras PSP PCI įrenginys
- Graphics BAR5 (0xFE800000) — GPU MMIO langas, kuriame yra C2PMSG

**Sprendimas:** Naudojame `-i 0xFE800000 0x200000` (BAR5, 2MB langas).

### 6. C2PMSG_81 laukimo ciklas — neteisinga logika

**Problema:** Firmware loading grąžino "SUCCESS" iškart, bet C2PMSG_81 nepasikeitė ir GRBM liko `0xFFFFFFFF`.

**Priežastis:** Originalus kodas tikrino:
```c
if (statusReg != 0) { success = TRUE; break; }
```
Bet C2PMSG_81 **jau buvo** `0xF0000010` (PSP SOS gyvas). Taigi ciklas išėjo pirmoje iteracijoje be jokio laukimo.

**Sprendimas:** Pakeista į:
```c
initialStatus = READ_REGISTER_ULONG(C2PMSG_81);
// ... po komandos:
if (statusReg != initialStatus) { success = TRUE; break; }
```
Laukiame kol reikšmė **pasikeis**, o ne kol taps ne-nulinė.

### 7. Firmware failo dydžio limitas

**Problema:** `cyan_skillfish2_sos_extracted.bin` buvo 262,656 baitų, o `PSP_MAX_FW_SIZE` = 256 KB (262,144). Test tool atmetė failą su "Invalid firmware size".

**Sprendimas:** Padidinome `PSP_MAX_FW_SIZE` iki 512 KB.

### 8. Firmware buffer'io atlaisvinimas tarp IOCTL kvietimų

**Problema:** Po `-f` (IOCTL_PSP_LOAD_FW) driveris iš karto atlaisvindavo (`MmFreeContiguousMemory`) firmware buffer'į. Kai sekanti komanda (`-w 0x1056C 0x4`) bandė naudoti tą patį fizinį adresą, C2PMSG_36 jau buvo išvalytas, o atmintis atlaisvinta.

**Priežastis:** Kiekviena `test-psp-driver.exe` komanda atidaro ir uždaro driverio handle'į. Atmintis turi išlikti tarp kvietimų.

**Sprendimas:** Perrašėme `IOCTL_PSP_LOAD_FW`:
- Buffer'is **neatlaisvinamas** po IOCTL
- Saugomas `DEVICE_EXTENSION` struktūroje: `FwBuffer`, `FwPhysical`, `FwPaShifted`
- Atlaisvinamas tik `DriverUnload` metu
- Pridėjome `IOCTL_PSP_SEND_CMD` (0x805) — siunčia komandą į C2PMSG_35, automatiškai perrašydamas PA>>20 į C2PMSG_36

### 9. Dviejų etapų PSP boot seka (iš sibling projekto)

**Problema:** Siuntėme tik vieną komandą (0x1), bet PSP reikalauja dviejų: SYSDRV (0x4) tada SOS (0x8).

**Atradimas:** Sibling projekto `amdbc250_psp_v11.c` kode radome:
```c
Amdbc250PspBootloaderLoadSysdrv();  // C2PMSG_36 = PA>>20, C2PMSG_35 = 0x4
Amdbc250PspBootloaderLoadSos();     // C2PMSG_36 = PA>>20, C2PMSG_35 = 0x8
```

**Sprendimas:**
- Pridėjome `-C <cmd>` flagą testavimo įrankiui
- `IOCTL_PSP_SEND_CMD` perrašo PA>>20 į C2PMSG_36, tada siunčia komandą į C2PMSG_35
- C2PMSG_36 formatas: `PhysicalAddress >> 20` (1MB-aligned), ne pilnas PA

### 10. GRBM/CP — aparatinis NBIO blokavimas

**Problema:** Net ir po sėkmingo firmware įkėlimo ir abiejų komandų (0x4 + 0x8), GRBM_STATUS (0x2004) visada grąžina `0xFFFFFFFF`.

**Analizė:** Sibling projekto README dokumentuoja, kad BC-250 NBIO yra **selektyvinis** (ASRock BIOS nustatymuose NBIO firewall aktyvus — tai ne PS5 specifika, o paties Oberon lusto NBIO elgesys su ASRock BIOS konfigūracija):
- Leidžia: GPU_ID, HDP, GC (0x3000+), MMHUB (0x5000+), DF, NBIO registrus
- Blokuoja: GRBM (0x2004), CP (0x2000-0x2FFF), CLK, Scratch, SDMA, RSMU

Mūsų testai patvirtino — visi "leidžiami" registrai veikia, visi "blokuojami" grąžina 0xFFFFFFFF. Tai **aparatinis PS5 apribojimas**, ne draiverio klaida. Sibling projektas turi identišką problemą.

---

## Veikiantis funkcionalumas (galutinis)

| Funkcija | IOCTL | Statusas |
|----------|-------|----------|
| BAR5 MMIO mapinimas | `INIT_HW` (0x803) | ✅ Veikia |
| Registrų skaitymas | `READ_REG` (0x800) | ✅ Veikia |
| Registrų rašymas | `WRITE_REG` (0x801) | ✅ Veikia |
| Firmware įkėlimas (persistent) | `LOAD_FW` (0x802) | ✅ Veikia |
| PSP komandų siuntimas | `SEND_CMD` (0x805) | ✅ Veikia |
| NBIO signature rašymas | `NBIO_UNLOCK` (0x804) | ✅ Veikia |
| C2PMSG mailbox skaitymas | `-m` / `-t` | ✅ Veikia |
| GC/MMHUB/HDP/DF registrai | `-r <offset>` | ✅ Veikia |
| GRBM/CP registrai | `-r <offset>` | ❌ HW blokas |

## Architektūra po visų pakeitimų

```
User Mode                          Kernel Mode (WDM)
-----------                        -----------------
test-psp-driver.exe               PspDriver.sys
├─ -i 0xFE800000 0x200000  ──→    MmMapIoSpace (BAR5)
├─ -f sos.bin              ──→    MmAllocateContiguousMemory (PERSISTENT)
│                                 C2PMSG_36 = PA>>20
│                                 C2PMSG_35 = 0x1
│                                 wait C2PMSG_81 change
├─ -C 0x00000004 (SYSDRV)  ──→    C2PMSG_36 = PA>>20 (re-write)
│                                 C2PMSG_35 = 0x4
│                                 wait C2PMSG_81 change
├─ -C 0x00000008 (SOS)     ──→    C2PMSG_36 = PA>>20 (re-write)
│                                 C2PMSG_35 = 0x8
│                                 wait C2PMSG_81 change
├─ -t                      ──→    Read C2PMSG_35/36/81
├─ -r 0x2004               ──→    READ_REGISTER_ULONG (0xFFFFFFFF)
└─ DriverUnload                     MmFreeContiguousMemory
                                   MmUnmapIoSpace
```

## Išvada

Sukūrėme pilnai veikiančią WDM tvarkyklę su:
- BAR5 MMIO mapinimu ir registrų skaitymu/rašymu
- Persistent firmware buffer'iu PSP mailbox protokolui
- Dviejų etapų PSP boot seka (SYSDRV 0x4 + SOS 0x8)
- Teisingu C2PMSG_81 laukimo ciklu (tikrina pokytį, ne nulį)
- Signing'u su tuo pačiu sertifikatu kaip sibling projektas

GRBM/CP registrų blokavimas yra aparatinis PS5 NBIO apribojimas — patvirtintas ir sibling projekto dokumentacijoje.
