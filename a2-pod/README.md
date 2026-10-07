# A2 auf deinem Daisy Pod

Die Firmware und `model.namb` sind fertig gebaut. Die Modell-Datei enthaelt
das kleinere Teilmodell aus `Boosted-6505+-A2-Chugs-FullRig.nam` unveraendert
als 1871 float32-Gewichte, einschliesslich des trainierten head_scale.
Das grosse Teilmodell wird nicht ausgefuehrt.

Quelle der C-Engine und Pod-Anbindung: offizieller TONE3000-Branch
[a2-nano, Commit afbfb9f07d15473e05d986a01a258d13dc1bb438](https://github.com/tone-3000/nam-pedal/tree/afbfb9f07d15473e05d986a01a258d13dc1bb438).
Der Branch nennt die feste Architektur A2-nano. Die kleinere Variante deiner
Datei passt exakt dazu (3 Kanaele, 23 Schichten, 1871 Gewichte). Das ist keine
beliebige A2-Kompatibilitaet: andere Konfigurationen werden vom Konverter
abgewiesen. Der alte main-Branch samt C++-Konverter bleibt separat erhalten.

## Test auf dem Pod

1. `model.namb` (7516 Bytes) ins Stammverzeichnis einer FAT32-SD-Karte
   kopieren, dann die Karte in den Daisy Pod stecken.
2. Falls noch kein Daisy-Bootloader installiert ist: BOOT halten, RESET
   druecken und BOOT loslassen. In PowerShell aus dem Verzeichnis `DaisySeed`:

   ```powershell
   & 'C:\Program Files\DaisyToolchain\bin\dfu-util.exe' -a 0 -s 0x08000000:leave -D './libDaisy/core/dsy_bootloader_v6_4-intdfu-2000ms.bin' -d ',0483:df11'
   ```

3. RESET druecken und im Daisy-Bootloader BOOT druecken, um die Ladephase zu
   halten. Die Firmware in QSPI laden:

   ```powershell
   & 'C:\Program Files\DaisyToolchain\bin\dfu-util.exe' -a 0 -s 0x90040000:leave -D './nam-pedal/a2-pod/build/NAMPedalA2.bin' -d ',0483:df11'
   ```

4. USB-Serial-Monitor mit 115200 Baud oeffnen und RESET druecken. Erwartet:
   `FS mount: OK`, `Model load: OK`, Benchmark unter 1 ms und
   `Audio engine started`. USB-Serial ist optional; die Firmware startet
   auch ohne geoeffneten Monitor.
5. Knopf 1 schaltet vom roten Bypass zur Modellverarbeitung.
   LED 1: Gruen = NAM ohne Gate, Cyan = NAM mit eingeschaltetem Gate.
   Poti 1 regelt Eingangsgain, Poti 2 die Ausgangslautstaerke.
   Die periodische `max`-Zykluszahl sollte unter 480000 bleiben. Das misst
   nur die Inferenz; fuer den gesamten Callback ist zusaetzliche Reserve
   erforderlich. Ein Hardwaretest ist noch ausstehend.

Flashen ersetzt das bisherige Programm. Bootloader nur installieren, wenn
er fehlt; eine vorhandene funktionierende Installation reicht aus.

## Eingangspegel und Noise Gate

LED 2 misst den linken Eingang vor Gain, Gate und NAM, auch im Bypass.
Gruen bedeutet Signal ueber 0.01 (-40 dBFS); ein 20-Hz-DC-Tracker entfernt
Gleichspannungsanteile nur fuer diese Signalerkennung. Rot bedeutet mindestens
digitale Vollaussteuerung (1.0). Rot bleibt 500 ms sichtbar, Gruen 80 ms.
Bei kleineren Pegeln ist die LED aus. Die Anzeige stammt aus LevelMeter;
die Peak-Uebergabe an die Hauptschleife ist gegen Interrupts geschuetzt.

Encoder drehen: Gate-Schwelle um 1 dB pro Schritt aendern, im Uhrzeigersinn
hoeher (staerkeres Gate). Bereich -80 bis -20 dBFS, Startwert -50 dBFS.
Encoder druecken: Gate ein/aus. Es startet ausgeschaltet. Die Einstellung
wird auch bei ausgeschaltetem Gate geaendert; USB-Serial zeigt den Wert.
Einstellungen sind fluechtig und werden beim Neustart zurueckgesetzt.

Das Gate erkennt den rohen Eingang unabhaengig vom Gain-Poti. Es verwendet
6 dB Hysterese, mindestens 20 ms Haltezeit, 1 ms Oeffnungsrampe und 50 ms
Schliessrampe. Der Detektor faellt mit einer Zeitkonstante von 10 ms ab.
Es daempft den NAM-Eingang und den Modell-Ausgang, damit auch modellinternes
Rauschen bei geschlossenem Gate stumm ist. Bypass bleibt ungegatet.
Hoehere Schwellen koennen leise Toene und ausklingende Noten abschneiden.
`total_max` in der Serial-Ausgabe misst zusaetzlich die Callback-Verarbeitung
inklusive Gate/Pegelmessung; die Inferenzwerte `cycles`/`max` bleiben erhalten.

Gate-Hosttests bestanden: ausgeschaltetes Gate transparent, Rauschboden
geschlossen, Attack, Hysterese, Hold, Release und weiches Abschalten.
Die neue Gate/Pegel-Version muss noch auf dem Pod gehoert und zeitlich
geprueft werden; die numerische NAM-Engine bleibt unveraendert.

## Modell erneut konvertieren / Firmware bauen

Im Verzeichnis `DaisySeed`:

```powershell
& './nam-pedal/a2-pod/Build-A2.ps1'
& 'C:\Program Files\KiCad\9.0\bin\python.exe' './nam-pedal/a2-pod/convert_a2.py' './nam-pedal/Boosted-6505+-A2-Chugs-FullRig.nam' './nam-pedal/a2-pod/anderes-modell.namb'
```

Der Konverter prueft die gesamte benoetigte Architektur vor dem Export und
ueberschreibt keine existierende Datei. Er erzeugt ein festes weights-only
NAMB-Profil mit Profilkennung `0xA203` und CRC32, das zu dieser Firmware
gehoert. Dieses Profil ist nicht mit dem allgemeinen alten NAMB-Loader
austauschbar. Die Firmware prueft Laenge, Profil, Header und CRC, bevor sie
Gewichte laedt. Die Inferenz selbst stammt unveraendert vom offiziellen Branch.
Lokale Pod-Anpassungen: korrekter libDaisy-Pfad, Windows `-pipe`, optionales
USB-Logging, 48 kHz, vollstaendiges Prewarming samt Head-Faltung und
Abbruch bei kurzen SD-Lesevorgaengen.

## Verifikation

- ARM-Firmware mit Gate/Pegelmessung erfolgreich kompiliert und gelinkt: 133888 Bytes QSPI.
- DTCM 94076 / 131072 Bytes, AXI SRAM 152364 / 524288 Bytes.
- CRC und exportierte Gewichte gegen JSON geprueft.
- C-Inferenz gegen unabhaengige NumPy-Faltungen mit Stille, Impuls und
  Rauschen ueber 20400 Samples geprueft: maximale Abweichung 3.84e-6.
- Identisches Verhalten fuer Blockgroessen 1, 17, 47 und 48 innerhalb der
  Float-Toleranzen; falsche Gewichtsanzahl wird abgewiesen.
- Laufzeit/Audio auf echter Hardware noch nicht gemessen. Bei der letzten
  USB-Abfrage waren kein DFU-Geraet und kein serieller Port vorhanden.

Der numerische Test (`verify_a2.py`) benoetigt NumPy und den lokalen
Host-Build der Engine (`nam_host.dll`); er misst keine Cortex-M7-Laufzeit.
