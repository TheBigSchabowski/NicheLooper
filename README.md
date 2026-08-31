# NicheLooper (macOS)

NicheLooper ist ein Live-Looper mit Metronom und Drum-Machine für Gitarre
über ein Audio-Interface (z. B. Roland Rubix44). Drei umschaltbare
Audio-Unit-Effekt-Chains (Tasten A / S / D) sitzen vor dem Looper, sodass der
Loop den Amp-Sound mit aufnimmt.

Gebaut mit Kotlin + Compose Multiplatform for Desktop (UI) und einer
C++-Audio-Engine (miniaudio → CoreAudio) über JNI.

## Installation

### Für Nutzer (ohne Programmieren)

1. Lade die neueste `NicheLooper-*.dmg` von der
   [Releases-Seite](https://github.com/TheBigSchabowski/NicheLooper/releases)
   herunter.
2. DMG öffnen und **NicheLooper** in den Ordner »Programme« ziehen.
3. Beim ersten Start macOS-**Mikrofon-Zugriff** erlauben (sonst bleibt der
   Eingang stumm). Da die App nicht von Apple signiert/notarisiert ist,
   beim ersten Start ggf. *Systemeinstellungen → Datenschutz & Sicherheit →
   „trotzdem öffnen“*. Nach einem **Update** kann der Eingang stumm
   bleiben, ohne dass macOS erneut fragt — siehe „Fehlerbehebung“.
4. Input/Output-Gerät wählen, **START ENGINE** — loslegen.

> Aktuelle Version: [**NicheLooper 1.1.1**](https://github.com/TheBigSchabowski/NicheLooper/releases/tag/v1.1.1)
> — `NicheLooper-1.1.1.dmg` direkt von der Releases-Seite laden.

### Für Entwickler (aus dem Quellcode)

```sh
./gradlew run          # App direkt aus dem Source starten
./gradlew packageDmg   # → build/compose/binaries/main/dmg/NicheLooper-1.1.1.dmg
```

Build-Voraussetzungen siehe Abschnitt „Starten (Entwicklung)“.

## Architektur

| Schicht | Technologie |
|---|---|
| UI | Kotlin, Compose Multiplatform for Desktop (Material 3) |
| Looper-Kern (C++) | `LooperEngine`, `RhythmSection` (`native/`) |
| Audio-I/O (C++) | [miniaudio](https://miniaud.io) → CoreAudio (`native/MacAudioEngine.cpp`) |
| Bridge | JNI (`native/jni_bridge.cpp`) |
| Plugins | 3× AU-Chain vor dem Looper (`native/AuPluginChain.mm`), Tasten A/S/D |
| Loop-Dateien | verlustfreies Float32-WAV nach `~/Music/NicheLooper`; M4A-Import via `afconvert` |

Der Echtzeit-Audiopfad (Callback → Mono-Downmix → `LooperEngine::process`
→ Monitor-Mix → Limiter) läuft komplett nativ.

## Starten (Entwicklung)

```sh
./gradlew run
```

- Beim ersten Engine-Start fragt macOS nach **Mikrofon-Zugriff** für das
  Terminal bzw. IntelliJ — erlauben, sonst bleibt der Eingang stumm.
- **Build-Voraussetzungen:** Gradle 9 muss auf einem JDK laufen, das es
  unterstützt (JDK 17–21). Die App selbst wird mit JDK 21 kompiliert — das
  wird vom Gradle-JVM-Toolchain **automatisch** (via Foojay-Resolver)
  heruntergeladen, falls kein JDK 21 installiert ist. Ist dein
  Standard-`java` zu neu für den Gradle-Daemon, setze `JAVA_HOME` (oder
  `org.gradle.java.home` in einer *lokalen*, nicht committeten
  `~/.gradle/gradle.properties`) auf ein kompatibles JDK.
- Der native Teil wird vom Gradle-Task `buildNative` mit `clang++` gebaut
  und als Ressource (`native/libnichelooper.dylib`) eingebettet.

## App-Bundle / DMG bauen

```sh
./gradlew packageDmg      # → build/compose/binaries/main/dmg/
```

Das Info.plist enthält bereits `NSMicrophoneUsageDescription`.

## Bedienung

1. Input/Output-Gerät wählen (Standard: System-Default; fürs Rubix44 beide
   auf „Rubix44" stellen — ein Gerät für beide Richtungen = keine Drift).
2. **START ENGINE** drücken.
3. REC / SET LOOP / OVERDUB wie gewohnt; Metronom, Drums, Count-in,
   Auto-Loop und alle Regler verhalten sich identisch zur Android-Vorlage.
   Taktart (4/4, 3/4, 2/4, 6/8) setzt Taktlänge und Klick-Akzente, das
   **Groove**-Menü darunter das Drum-Pattern innerhalb des Takts. Die
   Groove-Liste zeigt nur Patterns der gewählten Taktart; ein Taktart-Wechsel
   springt automatisch auf deren ersten Groove. Beide Tabellen stehen in
   `native/RhythmSection.cpp` — ein neuer Groove ist ein Eintrag in
   `kGrooves`, die UI liest Namen und Zuordnung über JNI aus.
   - **Count-in** ist immer genau 2 volle Takte und startet das Drum-Pattern
     beim REC-Druck neu auf Zählzeit 1. Ohne Count-in bleibt es beim alten
     Verhalten: REC klinkt sich auf die nächste Taktlinie ein, ohne das
     Raster zu verschieben.
   - **Mute (`M`)** schaltet nur die Drums stumm, die Takt-Clock läuft
     weiter — der Wiedereinstieg landet also immer im Groove. Bereits
     angeschlagene Schläge klingen aus; das Metronom bleibt hörbar.
4. **Achtung Feedback:** Bei eingebautem Mikrofon + Lautsprechern den
   „Monitor input"-Schalter ausschalten.
5. **Amp-Sound hörbar machen (wichtig!):** Interfaces wie das Rubix44 haben
   **Hardware-Direct-Monitoring** — das mischt das trockene Signal direkt am
   Gerät auf den Ausgang, egal was die Software macht. Für den Chain-Sound:
   Direct-Monitor-Regler am Interface zu, „Monitor input" in der App an.
   Die Meter zeigen den Signalfluss: **In** = roh vom Interface (vor der
   Chain), **FX** = nach der Chain (das hören Loop & Monitor), **Out** =
   Summe nach dem Limiter. Audio fließt nur bei laufender Engine
   (START ENGINE) — die Plugin-Fenster öffnen auch ohne, bekommen dann
   aber kein Signal.

## Plugin-Chains (A / S / D)

Drei umschaltbare Effekt-Chains sitzen **vor** dem Looper:
Gitarre → aktive Chain → Loop-Aufnahme + Monitor. Der Loop nimmt also den
Amp-Sound auf; Umschalten ändert nur den Live-Sound, nie fertige Loops.

- Gehostet werden die **Audio-Unit**-Builds der installierten Plugins (NAM,
  Gateway, TONEX, Neural-DSP-Archetypes, … plus Apples eingebaute Effekte) —
  über die System-API, kein VST-SDK nötig. Klanglich identisch zu den VST3s.
- **Tasten `A` / `S` / `D`** (oder die Chips) schalten die Live-Chain
  knackfrei um. **`M`** (oder der Mute-Chip) blendet die Drums live aus.
  Die Tasten greifen nur, wenn kein Textfeld den Fokus hat.
- „+ ADD PLUGIN" fügt der aktiven Chain ein Plugin hinzu; **EDIT** (oder
  Klick auf den Namen) öffnet das Original-Plugin-Fenster — dort z. B. das
  .nam-Modell laden.
- Chains gelten pro Sitzung; Presets/Persistenz ist noch offen (Plugins in
  ihren eigenen Fenstern konfigurieren).

## Loops

- SAVE schreibt verlustfreies Float32-WAV nach `~/Music/NicheLooper`.
- LOAD liest WAV **und** M4A — vom Handy kopierte `Loop_*.m4a` einfach in
  denselben Ordner legen (Konvertierung läuft über das mitgelieferte
  macOS-Tool `afconvert`).

## Fehlerbehebung

### Nach einem Update bleibt der Eingang stumm

Symptom: Die Engine startet ohne Fehlermeldung (`inCh=1` in der Konsolen-
Ausgabe), aber das **In**-Meter rührt sich nicht — und macOS fragt auch nicht
nach dem Mikrofon.

Ursache: Die App ist nur **ad-hoc signiert** (kein Apple-Developer-Zertifikat).
macOS hängt die Mikrofon-Freigabe an die Code-Signatur des Bundles, und die
ist bei jedem Build eine andere. Nach dem Ersetzen der App passt der alte
Eintrag also nicht mehr — statt neu zu fragen, liefert macOS in dem Fall
einfach Stille.

Abhilfe: Freigabe zurücksetzen, App neu starten, Dialog erlauben.

```sh
tccutil reset Microphone com.example.nichelooper
```

Dauerhaft verschwindet das erst mit einer echten Developer-ID-Signatur plus
Notarisierung — dann bleibt die Signatur über Versionen hinweg stabil.

### Kein Signal, obwohl die Engine läuft

Die Meter zeigen, wo es klemmt: **In** ist das rohe Signal vom Interface (vor
der Chain), **FX** das Signal nach der Chain. Schlägt In aus und FX nicht,
liegt es an der aktiven Plugin-Chain (Plugin ohne geladenes Preset, Output
zugedreht) — nicht am Eingang. Bleibt schon In stumm, siehe oben.

## Lizenzen & Drittanbieter-Inhalte

Der Quellcode in diesem Repository steht unter keiner ausdrücklichen Lizenz
(„All rights reserved" vorbehalten), **soweit nicht unten anders angegeben**.
Mitgelieferte Drittanbieter-Bibliotheken und -Samples behalten ihre
jeweiligen Lizenzen:

- **miniaudio** (`native/miniaudio.h`) — public domain oder MIT-0.
  © David Reid. Siehe Lizenztext am Ende der Datei. <https://miniaud.io>
- **Drum-Samples** (`src/main/resources/drums/*.raw`) — aus dem Hydrogen
  Drumkit **„The Black Pearl 1.0"** von Glen MacArthur (AVL Drumkits),
  lizenziert unter **GPL**. Quelle:
  <https://sourceforge.net/projects/hydrogen/files/Sound%20Libraries/Main%20sound%20libraries/>
  Die Samples wurden zu 48 kHz mono float32 konvertiert. Die vollständige
  Zuordnung der Einzeldateien steht in
  `src/main/resources/drums/ATTRIBUTION.txt`.
  Da die Samples unter der GPL stehen, gelten beim Weiterverteilen die
  GPL-Bedingungen (insbesondere Namensnennung + Verfügbarkeit der Quelle)
  **für diese Samples**.

## Mitwirkende / Danksagung

Original-Konzept und Looper-Kern basieren auf einer Android-Vorversion;
die macOS-Portierung übernimmt den C++-Looper-Kern und ersetzt die
Audio-I/O- sowie UI-Schicht.

Große Teile des Codes entstanden mit Unterstützung von KI-Coding-Modellen:
**GLM 5.2** und etwas **Kimi K2.7** (chinesische Modelle, genutzt über die
Ollama Cloud) sowie **Fable**.
