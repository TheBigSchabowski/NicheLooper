# NicheLooper (macOS und Linux)

NicheLooper ist ein Live-Looper mit Metronom und Drum-Machine für Gitarre
über ein Audio-Interface (z. B. Roland Rubix44). Drei umschaltbare
Effekt-Chains (Tasten A / S / D) sitzen vor dem Looper, sodass der
Loop den Amp-Sound mit aufnimmt. Die Chains hosten am Mac Audio Units, unter
Linux VST3-Plugins.

Gebaut mit Kotlin + Compose Multiplatform for Desktop (UI) und einer
C++-Audio-Engine (miniaudio → CoreAudio am Mac, PulseAudio/PipeWire, ALSA
oder JACK unter Linux) über JNI.

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

> Aktuelle Version: [**NicheLooper 1.1.2**](https://github.com/TheBigSchabowski/NicheLooper/releases/tag/v1.1.2)
> — `NicheLooper-1.1.2.dmg` direkt von der Releases-Seite laden.

### Linux

Für Linux baut `tools/build-linux.sh` zwei Pakete, die zu einem
[Release](https://github.com/TheBigSchabowski/NicheLooper/releases) gehören:

| Datei | Hinweis |
|---|---|
| `NicheLooper_<version>_linux-amd64.deb` | Ubuntu 24.04 und neuer, Debian 13 und neuer, Mint u. a. (x86-64) |
| `NicheLooper_<version>_linux-amd64.tar.gz` | jede andere Distribution, ohne Installation |

- **DEB:** `sudo apt install ./NicheLooper_<version>_linux-amd64.deb` installiert die App samt Java und
  legt „NicheLooper“ ins Anwendungsmenü (Audio); gestartet wird auch mit `/opt/nichelooper/bin/NicheLooper`.
- **tar.gz:** entpacken und `NicheLooper/bin/NicheLooper` starten. Gebraucht werden die X11-Bibliotheken
  (`libx11`, `libxtst`, `libxi`, `libxrandr`) und ALSA (`libasound2`), die jede Desktop-Installation mitbringt.
- Der Ton läuft über PulseAudio (auch PipeWire mit `pipewire-pulse`), ALSA oder JACK, die zur Laufzeit geladen
  werden — Auswahl siehe „Fehlerbehebung“. Plugin-Fenster sind X11-Fenster; unter Wayland läuft das über XWayland.
- Plugins: VST3-Versionen in `~/.vst3`, `/usr/lib/vst3` oder `/usr/local/lib/vst3` (siehe „Plugin-Chains“).
- **Teststand:** Gebaut und ausprobiert wurde unter Ubuntu 24.04 (x86-64) in einer virtuellen X-Umgebung mit dem
  Null-Audiogerät: Engine-Start, Plugin-Suche, Plugins einhängen und entfernen, Plugin-Fenster, Presets. Der
  M4A-Decoder und die Ordner-Auflösung laufen als Unit-Tests. **Noch nicht getestet** sind echte Audio-Hardware
  (Latenz, Geräteauswahl) und ein echter Desktop mit Fenstermanager bzw. Wayland — Fehler bitte als Issue melden.
- Das Icon ist ein Platzhalter.

### Für Entwickler (aus dem Quellcode)

```sh
./gradlew run          # App direkt aus dem Source starten
./gradlew packageDmg   # → build/compose/binaries/main/dmg/NicheLooper-1.1.2.dmg (am Mac)
./tools/build-linux.sh # → dist/linux: .deb, .tar.gz und SHA256SUMS.txt (unter Linux)
```

Build-Voraussetzungen siehe Abschnitt „Starten (Entwicklung)“.

## Architektur

| Schicht | Technologie |
|---|---|
| UI | Kotlin, Compose Multiplatform for Desktop (Material 3) |
| Looper-Kern (C++) | `LooperEngine`, `RhythmSection` (`native/`) |
| Audio-I/O (C++) | [miniaudio](https://miniaud.io) → CoreAudio (`native/MacAudioEngine.cpp`), unter Linux PulseAudio/ALSA/JACK (`native/LinuxAudioEngine.cpp`) |
| Bridge | JNI (`native/jni_bridge.cpp`) |
| Plugins | 3× Chain vor dem Looper, Tasten A/S/D: Audio Units am Mac (`native/AuPluginChain.mm`), VST3 unter Linux (`native/LinuxVstPluginChain.cpp`, SDK als Submodul in `third_party/vst3sdk`) |
| Loop-Dateien | verlustfreies Float32-WAV nach `~/Music/NicheLooper` (Linux: Musik-Ordner laut XDG, z. B. `~/Musik/NicheLooper`); M4A-Import am Mac via `afconvert`, unter Linux über einen eingebauten Java-Decoder |

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
- **Linux:** `buildNative` nimmt stattdessen `g++` und bettet `native/libnichelooper.so` ein.
  Gebraucht werden `g++`, `libx11-dev` und ein JDK 21 mit JNI-Headern (Ubuntu/Debian:
  `sudo apt install g++ libx11-dev openjdk-21-jdk`). Das VST3-SDK (MIT-Lizenz) liegt als Git-Submodul
  im Repository und muss einmal geholt werden:

  ```sh
  git submodule update --init --depth 1 third_party/vst3sdk
  git -C third_party/vst3sdk submodule update --init --depth 1 base pluginterfaces public.sdk
  ```

  Für das .deb kommen `fakeroot` und `dpkg-deb` dazu; das JDK muss `jpackage` mitbringen
  (`openjdk-21-jdk`, nicht die `-headless`-Variante allein).

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

- **macOS:** Gehostet werden die **Audio-Unit**-Builds der installierten
  Plugins (NAM, Gateway, TONEX, Neural-DSP-Archetypes, … plus Apples
  eingebaute Effekte) — über die System-API, kein VST-SDK nötig. Klanglich
  identisch zu den VST3s.
- **Linux:** Gehostet werden **VST3**-Effekte über das VST3-SDK (MIT-Lizenz).
  Gesucht wird in `~/.vst3`, `/usr/lib/vst3` und `/usr/local/lib/vst3`. Liegt
  ein Plugin woanders, wählt **„+ ADD FROM FILE…“** den `.vst3`-Ordner direkt
  aus. Es zählen nur Audio-Effekte — Instrumente (Synths) lassen sich nicht
  einhängen. Ob ein Plugin aus der Mac-Welt dabei ist, hängt davon ab, ob es
  einen Linux-VST3-Build gibt.
  Getestet mit LSP-Plugins, Dragonfly Hall/Room Reverb, 3 Band EQ und MVerb:
  Fenster öffnen, Signal verarbeiten, entfernen, Presets speichern und laden.
  Nicht getestet sind kommerzielle Plugins (NAM, TONEX, Neural DSP …) und die
  Tastatureingabe in Plugin-Fenstern.
- **Tasten `A` / `S` / `D`** (oder die Chips) schalten die Live-Chain
  knackfrei um. **`M`** (oder der Mute-Chip) blendet die Drums live aus.
  Die Tasten greifen nur, wenn kein Textfeld den Fokus hat.
- „+ ADD PLUGIN" fügt der aktiven Chain ein Plugin hinzu; **EDIT** (oder
  Klick auf den Namen) öffnet das Original-Plugin-Fenster — dort z. B. das
  .nam-Modell laden.
- **Presets:** Das Menü **Presets ▾** oben rechts speichert alle drei Chains
  samt Plugin-Zustand unter einem Namen („Als Preset speichern…“). Ein Klick
  auf den Namen lädt es, **✎** überschreibt es mit dem aktuellen Stand, **✕**
  löscht es. Speichern und Laden brauchen eine laufende Engine. Gespeichert
  wird am Mac in `~/Library/Application Support/NicheLooper/presets`, unter
  Linux in `~/.local/share/NicheLooper/presets` (bzw. `$XDG_DATA_HOME`). Die
  Dateien sind plattformspezifisch: Mac-Presets (Audio Units) lassen sich
  unter Linux (VST3) nicht laden und umgekehrt.

## Loops

- SAVE schreibt verlustfreies Float32-WAV nach `~/Music/NicheLooper`. Unter
  Linux ist es der Musik-Ordner laut XDG-Einstellung, z. B.
  `~/Musik/NicheLooper`.
- LOAD liest WAV **und** M4A — vom Handy kopierte `Loop_*.m4a` einfach in
  denselben Ordner legen (die Konvertierung läuft am Mac über das
  mitgelieferte macOS-Tool `afconvert`, unter Linux über einen eingebauten
  Java-Decoder).

## Fehlerbehebung

### Nach einem Update bleibt der Eingang stumm (macOS)

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
zugedreht) — nicht am Eingang. Bleibt schon In stumm, siehe oben (macOS);
unter Linux prüfen, ob in den Audio-Einstellungen (z. B. `pavucontrol`) der
richtige Eingang gewählt und nicht stummgeschaltet ist.

### Linux: Die Engine startet nicht

Beim Start probiert die App nacheinander PulseAudio (auch PipeWire mit
`pipewire-pulse`), ALSA und JACK. Steht in der Konsole
`NicheLooper: no audio system found`, ist keines davon nutzbar (nicht
gestartet oder Bibliothek nicht installiert). Mit der Umgebungsvariable
`NICHELOOPER_AUDIO_BACKEND` lässt sich ein System erzwingen:

```sh
NICHELOOPER_AUDIO_BACKEND=alsa NicheLooper/bin/NicheLooper
```

Erlaubt sind `pulseaudio` (oder `pulse`), `alsa`, `jack`, `auto` (die
Standardreihenfolge) und `null` — ein Dummy-Gerät ohne Ton, mit dem sich die
Oberfläche auch ohne Audio-Hardware ausprobieren lässt. Welches System
läuft, steht beim Start in der Konsole (`NicheLooper: audio backend …`).

### Linux: Ein Plugin fehlt in der Liste oder lässt sich nicht laden

- Die Liste zeigt nur VST3-**Effekte**; ein Instrument (Synth) wird beim
  Laden abgelehnt (Konsole: `no audio-effect class`).
- Liegt das Plugin nicht in `~/.vst3`, `/usr/lib/vst3` oder
  `/usr/local/lib/vst3`, den `.vst3`-Ordner mit **+ ADD FROM FILE…** wählen.
- Warum ein Plugin übersprungen wurde, steht in der Konsole. Eine Zeile wie
  `skipping …: dlopen failed … libsndfile.so.1: cannot open shared object file`
  heißt: Dem Plugin selbst fehlt eine Systembibliothek — das passende Paket
  nachinstallieren.
- Plugin-Fenster sind X11-Fenster. Unter Wayland läuft das über XWayland, das
  die gängigen Desktops mitbringen.

## Lizenzen & Drittanbieter-Inhalte

Der Quellcode in diesem Repository steht unter keiner ausdrücklichen Lizenz
(„All rights reserved" vorbehalten), **soweit nicht unten anders angegeben**.
Mitgelieferte Drittanbieter-Bibliotheken und -Samples behalten ihre
jeweiligen Lizenzen:

- **miniaudio** (`native/miniaudio.h`) — public domain oder MIT-0.
  © David Reid. Siehe Lizenztext am Ende der Datei. <https://miniaud.io>
- **VST3 SDK** (`third_party/vst3sdk`, Git-Submodul, nur im Linux-Build) —
  MIT-Lizenz, © Steinberg Media Technologies GmbH.
  <https://github.com/steinbergmedia/vst3sdk>
- **jaad** (M4A-Decoder unter Linux, als Gradle-Abhängigkeit
  `com.tianscar.javasound:jaad`) — Apache License 2.0.
  <https://github.com/Tianscar/jaadec>
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
Audio-I/O- sowie UI-Schicht. Die Linux-Portierung nutzt denselben Kern und
dieselbe Oberfläche; neu sind die Audio-I/O-Schicht (miniaudio über
PulseAudio/ALSA/JACK) und der VST3-Host, der unter Linux die Audio Units
ersetzt.

Große Teile des Codes entstanden mit Unterstützung von KI-Coding-Modellen:
**GLM 5.2** und etwas **Kimi K2.7** (chinesische Modelle, genutzt über die
Ollama Cloud) sowie **Fable**.
