# Symbolset owncloud.online (Entwurf)

Ersetzt die alte ownCloud-Wolke im Desktop-Client durch die Wolke von
owncloud.online (Kreise aus `theme-owncloudonline/core/img/favicon-touch.svg`).
Noch **nicht** eingebunden: die Dateien liegen hier zur Abnahme, die alten
unter `src/resources/theme/` sind unverändert.

## Bauen

```sh
# WSL, einmalig (nicht /tmp: ein Neustart der WSL leert es)
python3 -m venv /root/venv-icons && /root/venv-icons/bin/pip install cairosvg pillow
cd admin/branding/icons
/root/venv-icons/bin/python test_csg.py   # Selbsttest der Geometrie
/root/venv-icons/bin/python build.py      # schreibt src/ und out/, prüft Kontrast
```

`build.py` bricht ab, wenn eine Farbe gegen eine der Ziel-Taskleisten
(hell: #ffffff/#f3f3f3/#e6e6e6, dunkel: #000000/#1c1c1c/#202020/#2b2b2b)
unter 3:1 liegt. Schwächster Wert derzeit 4,11:1 (colored/ok).

## Aufbau

- `csg.py` – kleine 2D-CSG; alle Formen werden zu reinen SVG-Pfaden
  (M/L/A/Z, keine Masken/Filter/CSS), damit QtSvg genau so zeichnet wie
  cairosvg.
- Tray (32er-Raster): Wolke oben, Statusknopf unten rechts mit 1,5 Einheiten
  Spalt; die Glyphe ist aus dem Knopf **ausgestanzt** und zeigt also immer
  die Farbe der Taskleiste.
- App-Symbol (128er-Raster): Markentürkis #00e2bb, weiße Wolke. Ab 40 px mit
  den Spalten des Logos, bis 32 px als geschlossene, größere Silhouette.
- `out/kontaktbogen.png` – alt neben neu, 16/20/24/32 px, hell und dunkel.

## Zuordnung neu → alter Pfad

Laufzeitname ist der Alias aus `src/resources/core_theme.qrc`
(`theme/<flavor>/state-<x>.svg`); die Dateinamen in `out/theme/` sind die
alten, ein Austausch ist also reines Kopieren.

| neu (`out/…`) | alter Pfad (`src/resources/…`) | Verwendung |
|---|---|---|
| `theme/colored/ui-light-plain-color-state-*.svg` | `theme/colored/` (gleiche Namen) | Tray farbig, helle Leiste |
| `theme/dark/ui-dark-plain-color-state-*.svg` | `theme/dark/` | Tray farbig, dunkle Leiste |
| `theme/black/ui-light-plain-monochrom-state-*.svg` | `theme/black/` | Tray einfarbig, helle Leiste; macOS-Maske |
| `theme/white/ui-dark-plain-monochrom-state-*.svg` | `theme/white/` | Tray einfarbig, dunkle Leiste |
| `app/owncloud-online-icon.svg` | `theme/universal/owncloud-online-icon.svg` | Fenster-/About-Symbol |
| `app/owncloud-online.ico` (16–256) | `theme/universal/owncloud-online.ico` | Windows-EXE/Installer (`src/gui/CMakeLists.txt`) |
| `app/png/<n>-owncloud-online-icon.png` (16–512) | `theme/colored/<n>-owncloud-online-icon.png` | Linux/macOS-App-Symbol |

Zustände (Datei-Suffix → Alias): `checkmark` → `state-ok`, `sync` →
`state-sync`, `info` → `state-information`, `pause` → `state-pause`,
`offline` → `state-offline`, `error` → `state-error`. `warning` ist neu und
hat in `core_theme.qrc` noch keinen Alias (`Theme::syncStateIconName` liefert
es derzeit nie). `black/…-downloaded.svg` bleibt unberührt.

Nicht ersetzt: `*-owncloud-online-icon-sidebar.png`, `sidebar.svg`,
`wizard_logo*.svg`, Overlay-Symbole unter `shell_integration/`.
