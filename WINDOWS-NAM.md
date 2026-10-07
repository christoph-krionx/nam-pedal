# NAM-Konvertierung unter Windows

**A2-Update:** Fuer die vorhandene Boosted-6505-A2-Datei ist jetzt eine
separate C-Firmware inklusive geprueftem Konverter unter `a2-pod/` vorhanden.
Siehe `a2-pod/README.md`. Die folgenden Build-Hinweise betreffen den alten
C++-Konverter; dessen Formatgrenzen gelten nicht fuer die neue C-Firmware.

Der vorhandene MSYS2-UCRT64-Compiler braucht sein bin-Verzeichnis im PATH.
GCC/Assembler scheitern hier ausserdem an Umlauten in erzeugten Objekt- und
Temp-Dateipfaden. CMAKE_OBJECT_PATH_MAX kuerzt die Objektpfade durch Hashes;
-pipe vermeidet die temporaeren Assemblerdateien.

Im Verzeichnis `DaisySeed` in PowerShell:

```powershell
$env:PATH = 'C:\msys64\ucrt64\bin;' + $env:PATH
cmake -S nam-pedal/nam-binary-loader -B nam-pedal/nam-binary-loader/build -DCMAKE_OBJECT_PATH_MAX=170 -DCMAKE_CXX_FLAGS=-pipe
cmake --build nam-pedal/nam-binary-loader/build --target nam2namb loadmodel -j 4
```

Diese Befehle verwenden die bereits vorhandene Ninja/UCRT64-Konfiguration.

Ein Modell konvertieren (Pfad durch die eigene Datei ersetzen):

```powershell
& .\nam-pedal\Convert-Nam.ps1 -InputModel 'C:\Pfad\modell.nam' -OutputModel '.\nam-pedal\nano_relu.namb'
```

Das Skript verhindert das Ueberschreiben vorhandener Dateien, prueft die
Ausgabe mit dem originalen NAMB-Loader und warnt bei mehr als 4096 Bytes.
Erfolgreiches Laden auf dem PC garantiert keine Echtzeit-Verarbeitung auf
dem Daisy. Fuer die vorhandene Firmware zuerst ein kleines Nano-ReLU-Modell
mit 48 kHz verwenden und den Benchmark auf der Hardware pruefen.

Das vorhandene `Boosted-6505+-A2-Chugs-FullRig.nam` verwendet
`SlimmableContainer`, was dieser Konverter nicht unterstuetzt. Die beiden
enthaltenen WaveNets haben 1871 bzw. 12146 Gewichte (mindestens 7484 bzw.
48584 Bytes). Ausserdem verwendet ihre Konfiguration `kernel_sizes` und
einen `head` mit eigener Faltung; der vorhandene Konverter erwartet das
aeltere Schema mit `kernel_size`, `head_size` und `head_bias`. Einfaches
Entpacken oder Umbenennen erzeugt daher kein kompatibles Modell.

Die vorhandene Firmware ist fuer Daisy Pod, SD-Karte und zwei Potis gebaut.
Sie erwartet `nano_relu.namb` im Stammverzeichnis einer FAT32-SD-Karte.
Ein nackter Daisy Seed benoetigt eine Anpassung an die tatsaechliche
Beschaltung und Modellspeicherung. `StartLog(true)` wartet beim Start auf
eine USB-Serial-Verbindung. Ausserdem passt `LIBDAISY_DIR = ../../libDaisy`
im vorhandenen Makefile nicht zur aktuellen Ordnerstruktur; hier liegt
libDaisy neben nam-pedal, also unter `../libDaisy`.
