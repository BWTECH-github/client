# Update-Feed des Desktop-Clients

Der Client fragt beim Start und danach regelmäßig eine XML-Datei ab, die die
neueste Version und – je nach Plattform – den Download nennt. Wir betreiben
dafür keinen Server: die Feed-Dateien hängen als Assets am GitHub-Release.

## Adressen

Je Plattform und Kanal eine Datei:

    https://github.com/BWTECH-github/client/releases/latest/download/owncloud.online-update-<plattform>-<kanal>.xml

| Plattform      | Datei (Kanal stable)                              | `<downloadurl>`                       |
|----------------|---------------------------------------------------|---------------------------------------|
| `win-x64`      | `owncloud.online-update-win-x64-stable.xml`       | `…-win-x64-Setup.exe` des Releases    |
| `linux-x86_64` | `owncloud.online-update-linux-x86_64-stable.xml`  | `…AppImage.zsync`, sonst keiner       |
| `macos-arm64`  | `owncloud.online-update-macos-arm64-stable.xml`   | keiner (nur Hinweis mit Link)         |

`releases/latest/download/` zeigt immer auf das neueste stabile Release (nicht
auf Vorabversionen). Die Abfrageparameter, die der Client anhängt
(`?version=…&platform=…&channel=…&msi=true`), stören nicht: GitHub ignoriert sie
und leitet trotzdem auf die Datei weiter (geprüft am 14.10.2026, 200 nach 2
Weiterleitungen).

Kanäle: `stable` und `beta`. Ein Release ohne Suffix (`v11.0.0`) schreibt beide
Kanäle, ein Tag mit Suffix `rc`/`beta`/`alpha`/`daily`/`nightly` nur `beta` –
dieselbe Regel wie `updateChannel()` im Client. Weil `latest` keine
Vorabversionen liefert, bekommt der Beta-Kanal über die feste Adresse nur
stabile Releases; Beta-Feeds einer Vorabversion liegen unter
`releases/download/<tag>/…`.

## Format

Genau das, was `src/gui/updater/updateinfo.cpp` liest (Wurzel `owncloudclient`,
Felder `version`, `versionstring`, `web`, `downloadurl`):

```xml
<?xml version="1.0" encoding="UTF-8"?>
<owncloudclient>
  <version>11.0.0</version>
  <versionstring>owncloud.online 11.0.0</versionstring>
  <web>https://github.com/BWTECH-github/client/releases/tag/v11.0.0</web>
  <downloadurl>https://github.com/BWTECH-github/client/releases/download/v11.0.0/owncloud.online-client-11.0.0-win-x64-Setup.exe</downloadurl>
</owncloudclient>
```

`<version>` wird mit `QVersionNumber` gegen `Version::versionWithBuildNumber()`
verglichen. Folge: Ein Client `11.0.0.1234` (mit Baunummer) gilt gegenüber
einem Feed `11.0.0` als aktuell; ein Release-Kandidat `11.0.0-rc1` mit
Baunummer sieht das finale `11.0.0` nur, wenn der Feed eine höhere Baunummer
nennt (`--version 11.0.0.<build>`). Der Selbsttest deckt diese Fälle ab.

## Erzeugung

- `admin/release/make-update-feed.py` – Generator (nur Standardbibliothek).
  Prüft https, Dateiendung je Plattform, Version im Dateinamen, Tag-Format,
  Kanalregel und Zeichen, die aus dem Rich-Text des Clients ausbrechen würden.
- `admin/release/test_make_update_feed.py` – Selbsttest (39 Fälle); parst die
  Ausgabe wie der Client und liest dafür `updateinfo.cpp`, `updater.cpp` und
  `publish-release.yml`, damit Format, Kanalregel und Paketnamen nicht
  auseinanderlaufen.
- `.github/workflows/update-feed.yml` – erzeugt die Dateien und hängt sie mit
  `--clobber` an das Release; prüft anschließend, ob die feste Adresse die neue
  Datei ausliefert.

Auslöser:

1. `publish-release.yml` ruft den Workflow als Job `update-feed` auf
   (`workflow_call`). Nötig, weil ein mit dem `GITHUB_TOKEN` angelegtes Release
   keinen `release: published`-Lauf startet.
2. `release: published` – wenn ein Mensch das Release in der Oberfläche
   veröffentlicht.
3. Von Hand, z. B. für ein älteres Release:
   `gh workflow run update-feed.yml -R BWTECH-github/client -f tag=v11.0.0`
   (geht erst, wenn die Datei auf `master` liegt).

Lokal: `python3 admin/release/test_make_update_feed.py -v`.

Hinweis: `admin/release/` fällt unter das Muster `[Rr]elease/` in `.gitignore`;
neue Dateien dort mit `git add -f` aufnehmen.

## Was der Client nach dem Merge (Zweig `merge-upstream-7.2`) noch braucht

Zeilen beziehen sich auf `origin/merge-upstream-7.2` (Stand 97eaca3db). Ohne
diese Änderungen bleibt der Feed wirkungslos oder ist auf Windows schädlich.

1. **Updater ist abgeschaltet.** `OWNCLOUD.cmake:7` setzt
   `APPLICATION_UPDATE_URL ""`; `src/libsync/theme.cpp:113-120` liefert dann
   eine leere URL und `src/gui/updater/updater.cpp:142-147` legt keinen Updater
   an. Benötigt: eine Basisadresse je Plattform (eine einzige URL reicht nicht,
   weil die Feed-Datei plattform- und kanalabhängig ist), z. B. in
   `Theme::updateCheckUrl()` aus `…/releases/latest/download/owncloud.online-update-`
   + Plattform + `-` + `updateChannel()` + `.xml` zusammensetzen.
   `OCC_UPDATE_URL` (`updater.cpp:65`) bleibt als Testschalter.
2. **Windows installiert nur MSI.** `src/gui/updater/ocupdater.cpp:489`
   (`Q_ASSERT(… ".msi")`, im Release-Bau wirkungslos) und `:503`
   (`msiexec /i …`) – wir liefern ein NSIS-Setup (`…-win-x64-Setup.exe`).
   `msiexec` auf eine .exe schlägt fehl, `autoUpdateAttempted` steht dann auf
   true und der Nutzer bekommt bei jedem Start den Fehlerdialog
   (`ocupdater.cpp:450-472`). Benötigt: Setup direkt starten
   (`/S` bzw. die im Release beschriebenen Schalter, Neustart beachten),
   `updater.cpp:79-81` (`msi=true`) entfernen.
3. **Signaturprüfung des Downloads (Pflicht).** `ocupdater.cpp:297-328` kopiert
   die heruntergeladene Datei ungeprüft nach `configPath()` und startet sie
   später; `ocupdater.cpp:355-357` vertraut sogar einer bereits vorhandenen
   Datei gleichen Namens ohne jede Prüfung. Benötigt vor dem Start:
   `WinVerifyTrust` (Authenticode gültig) **und** Abgleich des
   Signaturzertifikats (Herausgeber = unser Code-Signing-Zertifikat, nicht nur
   „irgendeine gültige Signatur“); bei Fehlschlag Datei löschen,
   `DownloadFailed`. Zusätzlich die vorhandene Datei aus `:355` erneut prüfen.
   Die Feed-Datei selbst ist unsigniert; Schutz kommt nur aus HTTPS plus dieser
   Prüfung.
4. **Ziel des Downloads einschränken.** `ocupdater.cpp:359-360` folgt Weiterleitungen
   (`NoLessSafeRedirectPolicy`) zu beliebigen Hosts. Empfehlung: nur
   `https://github.com/BWTECH-github/client/releases/download/…` als
   `downloadurl` akzeptieren (Weiterleitung zu `objects.githubusercontent.com`
   zulassen).
5. **Weiterleitung der Feed-Abfrage.** `ocupdater.cpp:202` nutzt einen
   `QNetworkRequest` ohne explizite Redirect-Policy; die feste Adresse antwortet
   mit 302. Qt 6 folgt standardmäßig (`NoLessSafeRedirectPolicy`), sofern der
   `AccessManager` das nicht überschreibt – im Test des fertigen Baus prüfen.
6. **Rich-Text.** `ocupdater.cpp:150` und `:158` setzen `<web>` ungeschützt in
   `<a href='%1'>`. Der Generator lehnt dafür gefährliche Zeichen ab; sauberer
   ist `.toHtmlEscaped()` im Client.
7. **macOS** meldet nur (`updater.cpp:156-162`, `PassiveUpdateNotifier`);
   Sparkle bräuchte einen eigenen Appcast mit EdDSA-Signatur – nicht Teil
   dieses Feeds.
8. **Linux/AppImage** (`src/gui/updater/appimageupdater.cpp:48`, `:143`) braucht
   eine `.zsync`-Datei als `downloadurl`. `publish-release.yml` erzeugt noch
   keine; bis dahin enthält der Linux-Feed nur den Hinweis mit Link.
9. **Version 11.x (E2).** Ein Client 7.x sieht den Sprung auf 11.0.0 als
   Update (Selbsttest `test_jump_to_the_11_line_is_offered`); Clients aus der
   alten Reihe haben aber den Updater abgeschaltet (Punkt 1) und erfahren davon
   nichts – sie brauchen einmalig eine manuelle Installation.
