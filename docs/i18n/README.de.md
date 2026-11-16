<!-- translated from README.md eefd37b3fef2 body d1e6f98f38c7 -->

<p align="center"><img src="../images/logo.svg" width="128" alt="mkw-recomp logo"></p>

<h1 align="center">mkw-recomp</h1>

<p align="center">
  <b>Mario Kart World (Nintendo Switch 2), rekompiliert, um nativ unter Linux und Windows zu laufen.</b>
</p>

<p align="center">
  <a href="https://github.com/adriadam10/mkw-recomp/releases/latest"><img src="https://img.shields.io/badge/%E2%AC%87_Download-neueste_f%C3%BCr_Windows-0078d6?style=for-the-badge&labelColor=1b2740" alt="Download: neueste für Windows"></a>
  <a href="https://github.com/adriadam10/mkw-recomp/releases/latest"><img src="https://img.shields.io/badge/%E2%AC%87_Download-neueste_f%C3%BCr_Linux_%28AppImage%29-e95420?style=for-the-badge&labelColor=1b2740" alt="Download: neueste für Linux (AppImage)"></a>
  <a href="https://github.com/adriadam10/mkw-recomp/releases/latest"><img src="https://img.shields.io/badge/%E2%AC%87_Download-neueste_f%C3%BCr_Linux_ARM-8957e5?style=for-the-badge&labelColor=1b2740" alt="Download: neueste für Linux ARM"></a>
</p>

<p align="center">
  <b><a href="https://github.com/adriadam10/mkw-recomp/releases">Releases</a></b> ·
  <b><a href="https://github.com/adriadam10/mkw-recomp/wiki">Hilfe-Wiki</a></b> ·
  <b><a href="https://discord.gg/8uYBkjQkEg">Änderungen</a></b> ·
  <a href="../../CHANGELOG.md">Mitwirken</a> ·
  <a href="../../CONTRIBUTING.md">Quellen</a> ·
  <a href="../../docs/project/research.md#sources">Roadmap</a> ·
  <a href="../../docs/project/roadmap.md">Leistung</a> ·
  <a href="../../docs/project/performance.md">Issues</a> ·
  <a href="https://github.com/adriadam10/mkw-recomp/issues">Discussions</a> ·
  <a href="https://github.com/adriadam10/mkw-recomp/discussions">Archiv</a>
</p>

<!-- i18n-bar:start -->
<p align="center">
  <a href="../../README.md">English</a> ·
  <a href="README.zh-CN.md">简体中文</a> ·
  <a href="README.es.md">Español</a> ·
  <a href="README.ar.md">العربية</a> ·
  <a href="README.hi.md">हिन्दी</a> ·
  <a href="README.pt-BR.md">Português (Brasil)</a> ·
  <a href="README.pt-PT.md">Português (Portugal)</a> ·
  <b>Deutsch</b>
</p>
<!-- i18n-bar:end -->

Eine statische Rekompilierung von *Mario Kart World* (Nintendo Switch 2, Versionen 1.0.0 und 1.8.0) zu einem nativen
PC-Programm für Linux und Windows. Du brauchst deine eigene Kopie des Spiels, von deiner eigenen Konsole gedumpt:
nichts vom Spiel steckt in diesem Repository oder in seinen Releases.

**Es ist eine Rekompilierung, kein Emulator.** Der AArch64-Code des Spiels wird vorab nach C übersetzt und für die CPU
des PCs kompiliert (x86-64, oder AArch64 unter Linux); es gibt keinen CPU-Interpreter und kein JIT. Auch die GPU wird
nicht emuliert: NVN2, die Grafik-API der Switch 2, ist auf API-Ebene über Vulkan neu umgesetzt, und die SM86-Shader des
Spiels werden vorab nach SPIR-V übersetzt. Funktionen des Nintendo-SDK (`nn::*`) werden über ihren Namen durch eigenen
Code ersetzt.

## Stand

Früh, und es geht schnell voran. Stand 12.11.2026:

- Es startet, spielt die Intro-Videos ab, geht durch die Menüs und fährt Rennen, unter Linux und Windows (x86-64) und
  unter Linux auf AArch64 (mit eigenem AppImage; getestet auf einem Handheld mit Adreno 740).
- Der Startbildschirm richtet das Spiel aus deiner Kopie auf deinem Rechner ein (erzeugt und kompiliert das C, übersetzt
  die Shader, wärmt den Pipeline-Cache vor) und hält alle Einstellungen: Bildschirm, Grafikqualität, Steuerung, Hacks.
- Rennen laufen mit 60 fps auf High-End-GPUs und mit 30 auf Mittelklasse-GPUs; siehe [Leistung](#leistung).
- Bekannte Probleme: einige Menüs (Modus-, Charakter- und Fahrzeugauswahl) brauchen noch Arbeit, dem Schatten des Karts
  fehlt ab und zu ein Frame, in Videos bricht manchmal der Ton ab, und mit manchen AMD-Treibern kann die GPU hängen.
  Es gibt kein Online-Spiel.

## Roadmap

Die wichtigsten offenen Arbeiten, der Reihe nach; die Details stehen in [docs/roadmap.md](../../docs/project/roadmap.md).

- **60 fps im Rennen.** Heute ist ein Renn-Frame eine serielle Kette: das Spiel beendet seinen Frame (~3 ms\*), dann
  zeichnet das Vulkan-Backend den großen Submit des Spiels auf (~19 ms\*), dann führt die GPU ihn aus (~10 ms\*), und
  erst dann beginnt der nächste Frame.
  Das Aufzeichnen liegt auf dem kritischen Pfad, also geht es darum, mit weniger CPU aufzuzeichnen: dynamischer Zustand
  nur bei Änderungen, Draw-Caches über Frames hinweg, paralleles Aufzeichnen und ein schnellerer Spiel-Thread.
- **Das Bild wie auf der Konsole**: die letzten Fehler im Shader-Übersetzer, der Schatten des Karts, Push-Konstanten,
  null Vulkan-Validierungsfehler.
- **Jedes Menü geprüft** gegen die Konsole: als Nächstes Modus-, Charakter- und Fahrzeugauswahl.
- **Keine GPU-Hänger** und weniger Grafikspeicher, dann Laden ohne Ruckler und ein Linux-Paket für Distributionen ohne
  Rolling Release.
- **1.8.0 gleichauf mit 1.0.0**: Ultrawide-Seitenverhältnis, Größe der Shadow-Map und fester Seed
  ([#19](https://github.com/adriadam10/mkw-recomp/issues/19)).

\* Gemessen am 09.11.2026 auf unserem Testrechner: Radeon-660M-APU (Ryzen 5 7535HS), Linux, interne Auflösung 720p.

## Leistung

Gemessene Frame-Zeiten, jeweils mit der Version, auf der sie liefen, stehen in [docs/performance.md](../../docs/project/performance.md).
Um deinen Rechner hinzuzufügen, öffne einen
[Performance-Bericht](https://github.com/adriadam10/mkw-recomp/issues/new?template=performance.yml): er kommt mit der
gemeldeten Version in die Tabelle. Die Zeiten sind die eigenen Frames des Spiels: Frame-Generierung zeigt mehr Frames,
macht sie aber nicht kürzer. 16,7 ms sind 60 fps; Bildrate auf Automatisch (wie auf der Konsole) fällt auf 30 fps, wenn
60 nicht erreichbar sind, und ein Frame über 33,3 ms lässt das Rennen langsamer als in Echtzeit laufen.

| Datum | GPU | Interne Auflösung | fps im Rennen | Frame-Zeit im Rennen |
|---|---|---|---|---|
| 11.11.2026 | RX 7900 XTX (Windows) | 2160p | 60 | 16,7 ms |
| 11.11.2026 | RX 9070 XT (Linux, RADV) | 720p bis 2160p | 60 | 16,7 ms |
| 10.11.2026 | RTX 3060 (Windows) | 1080p | ~48 | ~21 ms |
| 11.11.2026 | RTX 3050 Laptop (Windows) | 720p | ~37 | 26,7 ms |
| 09.11.2026 | Radeon 660M (Linux, RADV) | 720p | 30-32 | 31-33 ms |
| 11.11.2026 | GTX 1650 SUPER (Windows) | 1080p | ~24 | ~41 ms |
| 09.11.2026 | GTX 1050 Ti (Windows) | 1080p | 22-24 | 42-45 ms |

## Spielen

> **Hängst du fest oder hast eine Frage?** Schau zuerst im [Wiki](https://github.com/adriadam10/mkw-recomp/wiki) nach:
> Installation, Einstellungen, Steuerung,
> [bekannte Fehler nach ihrer Log-Zeile](https://github.com/adriadam10/mkw-recomp/wiki/Troubleshooting) und eine
> [FAQ](https://github.com/adriadam10/mkw-recomp/wiki/FAQ) (auf Englisch).

![Der Startbildschirm, wenn das Spiel eingerichtet ist](../../docs/images/launcher-main.png)

![Die Seite Spieldateien, auf der die Einrichtung den Dump des Spielers übernimmt](../../docs/images/launcher-files.png)

1. Dumpe Mario Kart World 1.0.0 oder 1.8.0 von deiner Switch 2 (`exefs/` und `romfs/`, entschlüsselt; bei 1.8.0 mit dem
   Update in die RomFS des Grundspiels eingespielt), oder denselben Dump gepackt als eine `.nup`- oder `.nx`-Datei.
2. Lade den Launcher für dein System von den [Releases](https://github.com/adriadam10/mkw-recomp/releases) herunter:
   unter Linux das `.AppImage` (`-aarch64` auf ARM; der Standard-Spielordner ist `~/.local/share/mkw-recomp`; glibc 2.31
   oder neuer: Debian 11, Ubuntu 20.04, SteamOS und neuer), unter Windows die `.zip`, entpackt in einen Ordner. Beide
   brauchen etwa 30 GB freien Platz.
3. Starte das AppImage (vorher `chmod +x`) oder `mkw.exe`, öffne **Spieldateien**, wähle optional mit
   **Spielordner wählen**, wohin das Spiel installiert wird (oder eine vorhandene Installation), zeige dann auf deinen
   Dump (**Ordner wählen** oder **.nup- oder .nx-Datei wählen**) und drücke **Einrichten**. Der Launcher merkt sich den
   Installationsordner; das kompilierte Spiel, vorbereitete Dateien, Spielstände, Einstellungen und Caches liegen dort.
   Wählst du einen anderen Ordner, bleiben vorhandene Dateien, wo sie sind. Die erste Einrichtung dauert eine Weile
   (sie kompiliert das ganze Spiel und lädt NVIDIAs `nvdisasm` und `ptxas` herunter, 84 MB unter Linux und 126 MB unter
   Windows, um die Shader-Kodierung auf deinem Rechner zu beschreiben); danach **Spielen**.
4. Wenn ein neueres Release erscheint, bietet der Startbildschirm es an (**Update to vX**): er lädt es herunter, prüft
   es, ersetzt das Programm und richtet nur neu ein, was sich geändert hat. **Einstellungen → Menü → Nach Updates
   suchen** schaltet die Prüfung ab.

Voraussetzungen: x86-64-CPU mit AVX2 (oder, unter Linux, AArch64 mit ARMv8.2: LSE, FP16, RCpc, DotProd, CRC32), eine
GPU mit Vulkan 1.3 (`VK_EXT_mesh_shader` empfohlen; ohne laufen Task- und Mesh-Shader als Compute), 16 GB RAM
empfohlen. In einem Docker- oder Podman-Container unter Linux erlaube `userfaultfd` im seccomp-Profil (Dockers Standard
verweigert es): ohne kann der Port nicht erkennen, welcher Spielspeicher sich geändert hat, und läuft um ein Mehrfaches
langsamer.

## Zusätzliche Funktionen

Über die Konsole hinaus, alle optional und standardmäßig aus (oder auf dem Wert der Konsole):

- **Bildschirm**: interne Auflösung über oder unter der der Konsole, 21:9, 32:9 und 48:9 (Rennen zeigen mehr an den
  Seiten), Skalierung mit AMD FSR 1, HDR auf HDR-Bildschirmen, VSync, Mailbox- oder Immediate-Präsentation.
- **Frame-Generierung**: Frames zwischen denen des Spiels (ein Rennen mit 30 fps wird mit 60 gezeigt, oder mit 240 auf
  einem 240-Hz-Bildschirm). Flüssiger, nicht schneller: das Bild ist bis zu einen Frame später, und schnelle Bewegungen
  und die Minikarte können verschmieren.
- **Bildrate**: 20 und 15 fps neben den 60 und 30 der Konsole; Automatisch fällt auf sie zurück, wenn Frames zu lange
  dauern, sodass eine langsame Grafikkarte das Rennen in Echtzeit hält. **Niedrige Latenz** (standardmäßig an: sie
  ändert kein Bild, nur wann jeder Frame beginnt) startet jeden Frame so spät, wie die Karte es zulässt, sodass ein
  Tastendruck früher auf dem Bildschirm ankommt.
- **Grafik**: Texturfilterung und -qualität, Detaildistanz und Schattenqualität über oder unter denen der Konsole.
- **Ton**: 5.1-Surround.
- **Steuerung**: jede Taste frei belegbar, Mauskamera, Tastensymbole von Nintendo, Xbox oder PlayStation,
  Bewegungssteuerung, Vibrationsstärke; lokaler Mehrspieler mit je einem Controller.
  Switch-2-Controller (Pro Controller, Joy-Con, GameCube) funktionieren über Bluetooth und unter Linux auch über USB;
  unter Windows braucht USB einmalig einen Treiberschritt, siehe unten.
- **Hacks**: Turbo (F2 halten oder umschalten), Videos überspringen, alles freischalten, jedes Mal derselbe Zufall,
  direkt ins Zeitfahren oder in die freie Fahrt.
- **Sprachen**: die Menüs in 20 Sprachen; das Spiel selbst zusätzlich auf Katalanisch, Galicisch und Baskisch.
- **Spieldateien**: Installationsordner wählen; ein Dump als Ordner oder als eine `.nup`- oder `.nx`-Datei, genutzt wo er
  liegt; Updates über den Startbildschirm.
- **Performance-Overlay** (FPS, oder CPU, GPU und Speicher im Detail) und Systeminfos mit einem Klick für Fehlerberichte.

### Switch-2-Controller über USB unter Windows

Diese Controller starten erst nach einem Befehl an ihre zweite USB-Schnittstelle, die Windows ohne einen Treiber lässt,
den eine App benutzen kann. Einmal pro Controller, mit [Zadig](https://zadig.akeo.ie/):

1. Schließe den Controller mit einem USB-C-Kabel an und öffne Zadig.
2. **Options → List All Devices**, dann wähle **Interface 1** des Controllers (USB-ID `057E` und `2069` für den Pro
   Controller, `2066`/`2067` für den rechten/linken Joy-Con, `2073` für den GameCube-Controller). Nicht Interface 0: das
   ist die normale Eingabe des Controllers, und wenn du sie ersetzt, sehen ihn alle anderen Apps nicht mehr.
3. Treiber **WinUSB**, dann **Replace Driver** (oder **Install Driver**).

Dieselben Schritte gelten für einen virtuellen Controller, der mit diesen USB-IDs erscheint (der USB/IP-Modus von
Switch2Connect). Rückgängig machen: im Geräte-Manager das Gerät dieser Schnittstelle deinstallieren, mit Haken bei
„Treiber löschen“.

## Mods

Die Seite **Mods** des Startbildschirms installiert Mods so, wie sie für das Spiel veröffentlicht werden, und listet sie
in zwei Tabs:

- **Auf GameBanana suchen**: die Mods für Mario Kart World auf [GameBanana](https://gamebanana.com/games/22152),
  gelistet oder durchsucht, mit Autor, Lizenz, Screenshots und Beschreibung jedes Mods. **Installieren** lädt die Datei
  von GameBanana herunter, nur wenn du darum bittest.
- **Installiert**: deine Mods, jeder mit einem An/Aus-Schalter; **Zip importieren / Ordner importieren** übernimmt einen
  Mod, den du schon hast (`.zip`, `.rar` oder `.7z`, mit `romfs/` und/oder `exefs/` irgendwo darin, auch im Aufbau
  `atmosphere/contents/`).

Mods, die Spieldateien ersetzen (`romfs/`: Skins, Texturen, Texte, Strecken), wirken beim nächsten Start. Mods, die den
Code des Spiels ändern (`exefs/`-Patches als `.ips` oder `.pchtxt`, etwa 200cc oder 30 FPS), werden in den Spielcode
eingebaut: Ein- oder Ausschalten baut ihn vor dem Spielen neu (ein paar Minuten). Zwei Mods, die dasselbe Archiv ändern,
werden Datei für Datei zusammengeführt, und die Seite fragt, welcher gewinnt, wo beide dieselbe Datei ändern. Die Seite
warnt vor Mods, die die Grafikeinstellungen oder Shader des Spiels ersetzen, die der Port auf seine eigene Weise
behandelt.

Mods stammen von anderen Leuten und werden von diesem Projekt nicht getestet: einer kann das Spiel abstürzen lassen oder
deinen Spielstand beschädigen. Lies die Seite eines Mods, bevor du ihn installierst, behalte eine Kopie deines
Spielstands, und melde Probleme eines Mods seinem Autor; wenn du hier einen Fehler meldest, sag, welche Mods an waren
(das Log listet sie), oder probier es zuerst ohne sie.

## Aus dem Quellcode bauen

Linux, mit `uv`, `clang`, `lld`, `ninja`, SDL3, Opus, libvpx, zstd, xxHash, den Vulkan-Headern und VMA 3.4.0 (Details
in [tools/build/README.md](../../tools/build/README.md)). Leg deinen Dump in `game/` (`game/exefs`, `game/romfs`) oder setz
`MKW_GAME=<dir>`, dann im Wurzelverzeichnis des Repos:

```sh
uv run python recomp/gen.py --exefs game/exefs --out build/generated --jobs 8 --patches recomp/patches-mkw.txt  # 1.8.0: patches-mkw-180.txt
uv run tools/shaders/extract.py && uv run tools/shaders/translate.py
uv run tools/build/build.py              # build/mkw; --target windows baut mkw.exe mit llvm-mingw (windeps.py)
build/mkw
```

Abkürzungen für diese Abläufe und die für Releases: `make build`, `make appimage` (`make help` listet alle).

Unter Windows selbst (uv, CMake und Ninja aus MSYS2 UCRT64; ohne Linux-Rechner, WSL oder Wine): `windeps.py`, dann
`build.py --target windows`, siehe [tools/build/README.md](../../tools/build/README.md#on-windows). Aus einem gepackten Dump
(`.nup`, `.nx`) und für Linux auf AArch64: [tools/build/README.md](../../tools/build/README.md#a-packed-dump) und
[dessen AArch64-Abschnitt](../../tools/build/README.md#linux-aarch64).

Das Übersetzen der Shader braucht die SM86-Kodierungsspezifikation, erzeugt aus NVIDIAs `nvdisasm` (nicht mitgeliefert):
siehe [tools/shaders/README.md](../../tools/shaders/README.md). Alles Erzeugte landet in `build/`, nie in git.

| Ordner | Inhalt |
|---|---|
| [recomp/](../../recomp/README.md) | Der Generator: AArch64 → C |
| [runtime/](../../runtime/README.md) | Was die Konsole spielt: Kernel, Speicher, SDK-Ersatz, NVN2 über Vulkan, Audio, Eingabe, Menüs und Launcher |
| [sm86/](../../sm86/README.md) | Der Shader-Übersetzer: SM86 SASS → GLSL → SPIR-V |
| [i18n/](../../i18n/README.md) | Die Texte von Menü, Startbildschirm und Einrichtung in allen Sprachen (gettext `.po`) |
| [tools/](../../tools/README.md) | Analyse, Einrichtung, Build, Spielläufe und Diagnose |
| [tests/](../../tests/README.md) | Tests ohne Spiel und ohne GPU, in der CI unter Linux und Windows |
| [docs/](../../docs/README.md) | Recherche, Quellen und Leistung |
| `.github/` | CI, Vorlagen für Issues und Pull Requests |

Im Wurzelverzeichnis: `pyproject.toml` und `uv.lock` (die Python-Umgebung der Tools), `.gitignore`, `LICENSE`,
`CONTRIBUTING.md`, `CHANGELOG.md` (die Änderungen jedes Releases, seine Notizen), `AGENTS.md` (ein Leitfaden für
Coding-Agenten) und `Makefile` (Abkürzungen für die Build-Abläufe: `make help`).

## Designentscheidungen (30.10.2026)

| Thema | Entscheidung | Warum |
|---|---|---|
| CPU | AArch64→C-Lifter auf Basis dessen von [totk-pc-recomp](https://github.com/valentderah/totk-pc-recomp) (MIT), erweitert auf ARMv8.1-8.3: LSE-Atomics, CAS, LDAPR; PAC als Identität, MTE-Tags als 0 | Dieselbe Engine-Familie wie MKW, und er skaliert schon auf eine große Executable |
| System | Die `nn::*`-Funktionen des SDK über ihren Namen ersetzt; Signaturen aus den gemangelten Namen der 1 289 Importe von `main` | Keine Konsolendienste zu emulieren |
| Grafik | NVN2 über Vulkan: 529 seiner 699 Funktionen teilen ihren Namen mit NVN1 (Signaturen bekannt aus totk-pc-recomp und nxnative); 170 sind neu | Rekompilierung, keine GPU-Emulation |
| Shader | Eigener SM86→SPIR-V-Übersetzer, offline, mit einem Decoder, der aus der Spezifikation von DocumentSASS / nvsass-disassembler erzeugt wird | Deckt Mesh/Task ab und was Maxwell fehlt |
| Optionale Switch-2-Funktionen | HDR von selbst an, wenn der Bildschirm in HDR ist, wie auf der Konsole (Einstellungen > Bildschirm > HDR: Automatisch oder Aus); DLSS, Raytracing und Touch hinter Optionen, standardmäßig aus; ausgeschaltet gibt es ihre NVN2-Funktionen, sie tun aber nichts, damit das Spiel seinen normalen Weg nimmt | Hardware-Extras der Konsole, zum Spielen nicht nötig |
| Standardwerte | Aussehen und Auflösung der Konsole; niedrigere Einstellungen sind Optionen | Der Spieler entscheidet |

## Wie es entstanden ist

Dieser Port wurde mit [Claude](https://claude.com/claude-code) (dem Coding-Agenten von Anthropic) geschrieben, der den
größten Teil des Tippens übernahm, unter meiner Leitung, in etwa zehn Tagen Vollzeitarbeit: 489 Commits ab dem
30.10.2026, jeder davon geprüft, gemessen oder getestet, bevor er hineinkam. Es war nicht ein Prompt und fertig.
Dahinter steckt:

- **Mehr Methode als Prompts.** Jede Leistungsänderung wurde mit einem A/B innerhalb desselben Laufs gemessen (zwei Läufe
  desselben Builds unterscheiden sich um bis zu 12 ms pro Frame); jeder Rendering-Fix wurde gegen das Bild der Konsole
  selbst geprüft; Änderungen am Shader-Übersetzer werden vor einem Spiellauf ohne GPU an allen 11 561 Programmen von MKW
  getestet; Frame-Captures spielen das Backend in Sekunden ohne das Spiel ab. Die Regeln, die aus Fehlern entstanden
  sind, sind aufgeschrieben und werden von Skripten durchgesetzt, damit derselbe Fehler nicht zweimal passiert.
- **Was ich vorher gelernt hatte.** Der Recompiler und die Runtime sind aus
  [totk-pc-recomp](https://github.com/valentderah/totk-pc-recomp) und seinem Linux-Port gewachsen, und aus meiner
  früheren Arbeit an anderen Recomps (SavageWindRecomp, smo-recomp): der Lifter, der Launcher und die Einrichtung, die
  das Spiel auf dem Rechner des Spielers baut, die Tastensymbole, der Kontrollfluss der Shader. Als Maintainer von
  [AnyPS5](https://github.com/boykopovar/AnyPS5) habe ich gelernt, GPU-Shader originalgetreu zu übersetzen, Mesh-Shader
  als Compute auf GPUs ohne sie laufen zu lassen und ein großes Projekt mit Prüfungen und Messungen ehrlich zu halten.
- **Die Forschung der Community.** Die Notizen von [eden-nx2](https://git.eden-emu.dev/Takoroka/eden-nx2) zur Switch 2,
  NVIDIAs SASS-Kodierung, wie DocumentSASS und nvsass-disassembler sie dokumentieren, Ryujinx und Mesa NAK für die
  Semantik der Shader, und die anderen Switch-2-Ports, die in den Quellen aufgeführt sind.

## Lizenzen und Danksagungen

Dieses Projekt steht unter der [MIT-Lizenz](../../LICENSE). Code aus anderen Projekten behält seine eigene Lizenz, die neben
ihm liegt (`LICENSE.*`-Dateien in [recomp/](../../recomp/), [runtime/](../../runtime/), [sm86/decode/](../../sm86/decode/) und
[runtime/third_party/](../../runtime/third_party/README.md)): totk-pc-recomp, nvsass-disassembler, Dear ImGui, bc7enc,
stb_image, AMD FidelityFX FSR 1 und Roboto. Jede verwendete Quelle, mit ihrer Lizenz und dem, was daraus übernommen
wurde (Code nur aus MIT-Projekten; aus GPL-Projekten nur Ideen), steht in [docs/research.md](../../docs/project/research.md#sources).

Mario Kart World ist © Nintendo. Dieses Projekt ist nicht mit Nintendo verbunden oder von Nintendo unterstützt, enthält
keinen Code und keine Assets des Spiels und hilft nicht, sie zu beschaffen.

Mods gehören ihren Autoren und stehen unter ihren eigenen Lizenzen. Dieses Projekt ist nicht mit GameBanana oder dem
Autor eines Mods verbunden oder von ihnen unterstützt, hostet, prüft und verteilt keine Mods und ist nicht
verantwortlich für das, was ein Mod tut: der Startbildschirm zeigt nur die öffentliche Liste von GameBanana und lädt
einen Mod von GameBanana herunter, wenn du ihn darum bittest.

## Mitwirken

Siehe [CONTRIBUTING.md](../../CONTRIBUTING.md). Fehler und Performance-Berichte gehören in die
[Issues](https://github.com/adriadam10/mkw-recomp/issues), Fragen in die
[Discussions](https://github.com/adriadam10/mkw-recomp/discussions).
