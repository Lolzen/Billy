# Billy für Linux

Ein nativer Linux-Nachbau von **Billy**, dem kleinen „no nonsense“-Audioplayer aus der Windows-XP/Vista-Zeit
(Version 1.04l, 2004–2008, Sanne Schaap / Sheep Productions).

Billy spielt ganze Ordner voller Musik in Sekunden ab, **lückenlos (gapless)**, und lässt sich komplett per
Tastatur bedienen. Es gibt keine Tags, keine Skins und keine Bibliothek, sondern nur Dateinamen, eine Playlist und Musik.

![Billy unter Linux](docs/screenshot.png)

## Was ist das genau?

Dieses Projekt ist eine **eigenständige Neuimplementierung** in C++17/Qt 6. Das Original war ein Delphi-Programm
auf Basis der BASS-Soundbibliothek. Aus der `Billy.exe` wurden nur Formular-Layouts, Menüs, Tastenkürzel,
INI-Schlüssel und Playlist-Formate ausgelesen. Der Programmcode ist komplett neu geschrieben. Code, Grafiken
und Dokumentation des Originals sind nicht enthalten: Laut Lizenz des Originals darf es nicht verändert werden,
und alle Rechte bleiben beim Autor.

## Funktionen

* **Gapless-Wiedergabe**: Ein Decoder-Thread hängt den nächsten Titel nahtlos an denselben Ausgabestrom an.
  Bei MP3 entfernt libmpg123 den Encoder-Delay und das Padding (LAME/Xing-Header).
  Getestet mit einem Sinuston, der auf mehrere Dateien aufgeteilt war: keine Lücke und kein Knacken an den
  Titelgrenzen, bei MP3, FLAC, Ogg Vorbis und WAV.
* Formate: MP3/MP2/MP1, WAV, FLAC, Ogg Vorbis, Opus, AIFF
* Ordner öffnen (F4) oder hinzufügen (F3), mit oder ohne Unterordner, optional mit Datumsfilter
  (heute, letzte Woche, letzter Monat). Das Einlesen läuft im Hintergrund und lässt sich mit ESC abbrechen.
* Playmodes: Repeat list, Repeat file, Play list, Play file, Shuffle (mit Historie). Umschalten per F9 oder
  Klick auf die Statusleiste.
* Warteschlange (Insert / Numpad 9), Crop (Ctrl+Del), Duplikate oder fehlende Dateien entfernen,
  Liste mischen
* Ansicht als Dateiname, voller Pfad oder relativer Pfad zum Musikordner (F8)
* Finder: einfach lostippen
* Umbenennen (F2) mit Dateiinfos (Format, Bitrate, Länge, ID3v1/v2- bzw. Vorbis-Tags) und dem
  „R“-Aufräumknopf. **Mehrfach-Umbenenner** (F2 bei Mehrfachauswahl) mit Vorschau und den Codes
  `[case]`, `[upper]` und `[lower]`.
* Kalibrierte Pegelanzeigen: 7 LEDs (−60, −30, −15, −6, −3, −1 dB) oder 14 LEDs (−∞ bis 0 dB)
* Lautstärke durch Ziehen am „100 %“, mit Numpad +/− oder dem Mausrad. Softmute mit der Pause-Taste.
* Favoriten (Ctrl+D, Ctrl+B, Ctrl+1 … Ctrl+0), Playlists als txt, m3u oder pls laden und speichern
* F5 sucht neue Dateien in den geöffneten Ordnern (sortiert nach Dateidatum) und entfernt verschwundene
* Gesamtspielzeit der Auswahl (Ctrl+Alt+T), Sleep-Timer (Alt+T, optional mit Herunterfahren),
  „Copy to MP3 player“ (Ctrl+M)
* Tray-Icon (F11) mit dem Original-Menü (100 %/60 %/30 %/0 %, Play, Pause …)
* Farbschemata und Schrift für die Playlist (`*.billyscheme`)
* Automatisches Fortsetzen beim Start (optional), Single-Instance: Dateien aus dem Dateimanager landen im
  laufenden Billy
* **MPRIS2**: Die Medientasten der Tastatur und die Mediensteuerung des Desktops funktionieren systemweit.
  Das ersetzt die „globalen Hotkeys“ des Originals.
* Billy's love mode (Numpad 7) :)

Alle Tastenkürzel zeigt F1.

### Nicht übernommen

| Original | Grund / Ersatz |
| --- | --- |
| Internetradio und Aufnahme | im Original schon halb versteckt; vielleicht später |
| last.fm-Scrobbling | die alte Audioscrobbler-API 1.2 gibt es nicht mehr |
| Update-Check auf sheepfriends.com | die Seite existiert nicht mehr |
| Dateizuordnungen in der Registry | stattdessen `billy.desktop` mit MIME-Typen |
| „Windows-Mixer zurücksetzen“ (Ctrl+Space) | setzt nur Billys eigene Lautstärke auf 100 % |
| Globale Hotkeys per RegisterHotKey | MPRIS (Medientasten, Desktop-Widgets, `playerctl`) |

## Bauen

Abhängigkeiten: CMake ≥ 3.16, ein C++17-Compiler, Qt 6 (Widgets, Multimedia, Network, optional DBus),
libmpg123 und libsndfile (≥ 1.0.29 für Opus).

```sh
# Debian / Ubuntu (24.04+)
sudo apt install build-essential cmake pkg-config qt6-base-dev qt6-multimedia-dev libmpg123-dev libsndfile1-dev

# Fedora
sudo dnf install gcc-c++ cmake qt6-qtbase-devel qt6-qtmultimedia-devel mpg123-devel libsndfile-devel

# Arch
sudo pacman -S base-devel cmake qt6-base qt6-multimedia mpg123 libsndfile
```

```sh
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build -j
sudo cmake --install build      # optional: installiert billy, .desktop-Datei und Icon
```

Starten mit `./build/billy [Dateien, Ordner oder Playlists …]`.

## Konfiguration

Alles liegt in `~/.config/Billy/`:

* `Billy.ini`: Einstellungen (die Abschnitte heißen wie im Original `[General]`, `[Playlist]` usw.)
* `Playlist.txt`: die aktuelle Liste. Ist die Datei leer und schreibgeschützt, startet Billy immer mit
  leerer Liste, wie im Original.
* `Favorites/*.txt`, `Schemes/*.billyscheme`, `love.txt`

Ctrl+Alt+E öffnet den Ordner.

Debug-Ausgaben der Audio-Engine: `QT_LOGGING_RULES="billy.*.debug=true" billy`

## Aufbau des Codes

```
src/audio/Decoder.*       mpg123- und libsndfile-Decoder (float), Datei-Infos und Tags
src/audio/AudioEngine.*   Decoder-Thread, Puffer (Standard 3000 ms), Gapless-Übergänge, Ausgabe-Thread mit QAudioSink
src/PlaylistModel.*       Playlist, Queue, Shuffle-Historie, Playmodes, Laden/Speichern, Ordner-Scan, Sortierung
src/MainWindow.*          Hauptfenster, Aktionen und Tastenkürzel, Numpad, Tray, Pegelanzeige
src/widgets/              SheepProgress (Balken und LED-Meter), Playlist-Ansicht, Button-Glyphen
src/dialogs/              Finder, Umbenennen, Mehrfach-Umbenenner, Ordnerauswahl, Favoriten, Einstellungen, …
src/Mpris.*               MPRIS2 über D-Bus
```

Zur Audioausgabe: Wechselt die Samplerate zwischen zwei Titeln (z. B. von 44,1 auf 48 kHz), wird der
Ausgabestrom neu geöffnet. Nur an dieser Stelle entsteht eine kurze Lücke. Innerhalb eines Albums ist die
Wiedergabe lückenlos.

### Warum nicht BASS?

BASS (un4seen) gibt es auch für Linux, ist aber closed source und nur für nicht-kommerzielle Nutzung frei.
Die Bibliothek darf nicht mit ins Repository und nicht in Distributionspakete. mpg123, libsndfile und
Qt Multimedia sind frei, in jeder Distribution enthalten, und mpg123 beherrscht exaktes MP3-Gapless.
Ein optionales BASS-Backend wäre über die `Decoder`-Schnittstelle möglich.
