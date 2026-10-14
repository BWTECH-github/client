# Windows-Code-Signierung (DigiCert KeyLocker)

Die Windows-Pakete des Desktop-Clients werden in der CI mit dem
Code-Signing-Zertifikat der BW-TECH GmbH signiert. Der private Schlüssel liegt
bei DigiCert KeyLocker (Cloud-HSM) und verlässt ihn nie; `signtool` erreicht ihn
über den DigiCert Signing Manager KSP.

## Was signiert wird

| Datei | Wer signiert |
| --- | --- |
| eigene Binärdateien (`owncloud.online.exe`, `*.dll` des Clients, VFS-Plugin) | craft beim Paketieren (`CodeSigning/SignCache = True`: Qt-DLLs aus dem Cache werden **nicht** signiert) |
| craft-Installer `owncloud.online-client-<version>-win64.exe` | craft nach `makensis` |
| `owncloud.online-uninstall.exe` (Helfer im Setup) | `main.yml`, vor dem Einbetten |
| `…-Setup.exe` (Wrapper mit Neustart) | `main.yml`, nach `makensis` |
| `.7z` (portabel) | enthält die signierten Binärdateien |

Jede Signatur trägt einen Zeitstempel (`http://timestamp.digicert.com`, SHA-256).
Der Schritt „Check Windows signatures“ prüft danach jede `.exe` in `binaries/`
mit `signtool verify /pa` und lässt den Lauf bei fehlender Signatur scheitern.

Kontingent: 1000 Signaturen im Jahr. Pro Lauf sind es etwa 15 bis 20
(eigene Binärdateien, Installer, Helfer, Setup, eine 7za-Kopie von craft).
Signiert wird nur bei Läufen, die die Zugangsdaten sehen – nie bei Pull Requests.

## Einrichtung (einmalig, im Repository unter Settings → Secrets and variables → Actions)

Aus DigiCert ONE (Software Trust Manager / KeyLocker):

| Name | Art | Herkunft |
| --- | --- | --- |
| `SM_HOST` | Variable | Endpunkt des Kontos, z. B. `https://clientauth.one.digicert.com` (USA) oder `https://clientauth.one.nl.digicert.com` (NL) |
| `SM_API_KEY` | Secret | API-Schlüssel eines Dienstbenutzers (Account → Access → API tokens), wird nur einmal angezeigt |
| `SM_CLIENT_CERT_FILE_B64` | Secret | Client-Authentifizierungszertifikat (`.p12`) des Benutzers, Base64-kodiert: `[Convert]::ToBase64String([IO.File]::ReadAllBytes('client.p12'))` |
| `SM_CLIENT_CERT_PASSWORD` | Secret | Kennwort dieses `.p12` |
| `SM_KEYPAIR_ALIAS` | Variable, optional | Alias des Schlüsselpaars, wenn im Konto mehrere liegen |
| `SM_CERT_COMMONNAME` | Variable, optional | Antragstellername zur Auswahl im Zertifikatsspeicher, Vorgabe `BW-TECH GmbH` |

Fehlt eines der drei Secrets oder `SM_HOST`, läuft die CI wie bisher und meldet
„Windows-Pakete sind unsigniert“ als Hinweis.

## Ablauf in der CI

1. `.p12` aus dem Secret entschlüsseln, DigiCert Keylocker Tools per API
   herunterladen und still installieren.
2. `smctl healthcheck`, dann `smctl windows certsync`: legt das Zertifikat in den
   Zertifikatsspeicher des Benutzers, der Schlüssel bleibt im KSP.
3. `SIGN_PACKAGE=True` für craft; `admin/win/codesign.ps1` für Helfer und Setup.
4. Prüfung aller `.exe` in `binaries/`, Löschen des `.p12`.

## Lokal signieren

Keylocker Tools installieren, `smctl credentials save <API-Schlüssel> <Kennwort>`
und `smctl windows certsync` ausführen, Umgebungsvariablen `SM_HOST` und
`SM_CLIENT_CERT_FILE` setzen. Danach:

```powershell
.\admin\win\codesign.ps1 -Sign .\owncloud.online-client-7.5.0-win64-Setup.exe
.\admin\win\codesign.ps1 -Verify .\owncloud.online-client-7.5.0-win64-Setup.exe
```

## Erneuerung

Das Zertifikat läuft am 16.09.2027 ab. Nach der Erneuerung in DigiCert ONE genügt
ein neuer Lauf; `certsync` holt das aktuelle Zertifikat. Alte Signaturen bleiben
durch den Zeitstempel gültig.
