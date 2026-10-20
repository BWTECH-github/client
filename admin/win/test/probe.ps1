<#
.SYNOPSIS
    Install-/Deinstall-Probe für das Windows-Setup (Audit T-I).

.DESCRIPTION
    VERÄNDERT DAS SYSTEM: installiert und deinstalliert Software und legt
    typische Spuren eines eingerichteten Clients an. Nur auf einer
    Wegwerf-Maschine ausführen (CI-Runner, VM mit Snapshot) - außerhalb von
    GitHub Actions verweigert das Skript den Lauf, solange -AllowLocal fehlt.
    Startet Windows nie neu: Setups laufen mit /S /norestart, msiexec mit
    /norestart; die Probe selbst löst keinen Neustart aus.

    Szenarien (Schnappschüsse mit snapshot.ps1, Vergleiche mit compare.ps1):

      frisch      Ausgang -> Setup -> Client-Spuren -> Prüfen ->
                  Deinstallation -> Rückstände gegen den Ausgang
      koexistenz  Der offizielle Client 6.0.3 (-OfficialMsi) wird vorher still
                  installiert und gestartet. Sein Fußabdruck muss nach unserer
                  Installation und nach unserer Deinstallation unverändert
                  sein, sein Prozess muss weiterlaufen. Gegenrichtung: unseren
                  Client erneut installieren, den offiziellen deinstallieren -
                  unserer muss unverändert bleiben. Zeigt ein AppSearch-Ort des
                  MSI (Altinstallation) auf unseren Client, ist das ein Fehler
                  und die Deinstallation unterbleibt: msiexec würde unseren
                  Deinstaller still ohne /norestart starten.
      upgrade     Vorversion (-PreviousSetup) mit Client-Spuren -> Setup ->
                  genau eine Installation, Spuren erhalten ->
                  Deinstallation -> Rückstände gegen den Ausgang
      upgrade-beta1  wie upgrade, Vorversion ist die veröffentlichte
                  11.0.0-beta1 mit derselben Kennung wie der Prüfling. Das
                  Update muss sie an Ort und Stelle ersetzen: Vorversion
                  wirklich beta1, Version in Programme und Features ersetzt,
                  keine Doppel-Einträge (auch unter fremdem Namen), genau eine
                  Programmdatei im selben Ordner, Programmdatei ersetzt,
                  Einstellungen der Vorversion erhalten
      upgrade-beta2  wie upgrade-beta1 mit der veröffentlichten
                  11.0.0-beta2. Die Einstellungen sind eine Konto- und
                  Ordnerkonfiguration, wie beta2 sie schreibt (Konto auf
                  probe.invalid, ein Ordnerpaar auf den Sync-Ordner,
                  Einstellungen an der Wurzel [General], zwei davon abweichend
                  vom Standard). Nach dem Update startet der Prüfling und wird
                  mit --quit beendet: Er muss das Ordnerpaar lesen (Protokoll)
                  und Konto, Ordnerpaar und Einstellungen ohne Verlust
                  zurückschreiben. Ein zweiter Start mit demselben Ordnerpaar
                  als Platzhalter-Ordner (virtualFilesMode=wincfapi, unter
                  Windows die Vorgabe von beta2) prüft, dass auch diese
                  Ordnerdefinition bleibt; ob der Runner den Sync-Root anmelden
                  kann, steht als Information im Bericht
      deinstall-policies  Leerer Richtlinien-Schlüssel HKLM\Software\Policies\
                  BW-Tech GmbH\owncloud.online, wie beta1/beta2 ihn mit
                  Administratorrechten anlegten. Je Durchgang: Setup, Schlüssel
                  anlegen, Deinstallation still (/S) oder über die Oberfläche
                  (Vorauswahl der Wahlseite; die Abschlussseite wird beendet,
                  ohne "Fertig stellen" zu wählen - dort ist "Jetzt neu
                  starten" vorausgewählt). Der leere Schlüssel und ein leerer
                  Elternschlüssel müssen danach weg sein; eine Richtlinie (Wert
                  im Schlüssel oder in Setup\ServerUrl) und fremde Richtlinien
                  unter BW-Tech GmbH bleiben unverändert. Am Ende Rückstände
                  gegen den Ausgang
      assistent   Nur auf Anforderung (nicht in "alle"): Kontoassistent wie
                  ein Tester. Prüfling (und, mit -OfficialMsi, der
                  offizielle Client 6.0.3) still installieren, WER-
                  Speicherabbilder einschalten, dann mehrere Durchgänge mit
                  frischer Konfiguration (Oberfläche deutsch): Umstieg-Dialog
                  aus einer owncloud.cfg des offiziellen Clients (Konto auf
                  -WrongServerUrl, ein Sync-Ordner), "Neues Konto
                  einrichten", Adresse eintragen, "Anmelden" - erst
                  -WrongServerUrl (ohne OAuth2, Warnung) und/oder gleich
                  -ServerUrl. Die Browser-Anmeldung übernimmt
                  assistent-anmeldung.py mit dem Anmelde-Link des Clients und
                  dem Testkonto aus OC_USER/OC_PASS (login.py aus dem Zweig
                  sync-e2e, -LoginScript), dann Erfolgsseite, erweiterte
                  Einstellungen, "Fertigstellen" und 90 s Beobachtung. Die
                  Oberfläche fährt UI Automation; ein Durchgang nur mit
                  Tastatureingaben (ohne UI Automation im Client) prüft, ob
                  die Barrierefreiheits-Schnittstelle selbst etwas ändert.
                  Nach jedem Schritt ein Bildschirmfoto und der Zustand des
                  Prozesses. Endet der Prozess: Exit-Code, Ereignisprotokoll
                  (Application Error 1000), Speicherabbild, Absturzprotokoll
                  des Clients (%TEMP%\owncloud.online-crash.log), Ende seines
                  Protokolls; Auswertung des Speicherabbilds mit cdb. Ein
                  vollständiges Speicherabbild kommt nur ins Artefakt, wenn
                  der Client noch keine Tokens hatte (sonst ein Auszug ohne
                  Heap); Protokolle ohne Code, Tokens und Zugangsdaten

    Client-Spuren, nachgebildet nach dem Client-Code (Geheimnis = Zufallswert,
    wird nie ausgegeben): Anmeldeinformation <app>_credentials:<host>:<uuid>:
    http/oauthtoken (CredentialManager), Einstellungen in %APPDATA%\<app>
    (ConfigFile::configPath), Cache in %LOCALAPPDATA%\<app>, Autostart in HKCU
    Run (Utility::setLaunchOnStartup), Eintrag im Explorer-Navigationsbereich
    (NavigationPaneHelper) und eine Nutzerdatei im Sync-Ordner. Nach der
    Deinstallation müssen Autostart und Explorer-Eintrag weg sein, die
    Nutzerdatei muss bleiben. Anmeldedaten, Einstellungen und Cache
    (Entscheidung E4): frisch und koexistenz deinstallieren mit /S /purge -
    sie müssen weg sein; upgrade deinstalliert mit /S - sie müssen bleiben.
    Einen CfApi-Sync-Root meldet in upgrade-beta2 der Prüfling selbst mit dem
    Platzhalter-Ordner an; dann muss er samt Explorer-Eintrag (beide
    Registry-Sichten) mit der Deinstallation gehen. Windows-Search-Regeln und
    Schnellzugriff lassen sich ohne laufenden Client mit Konto nicht
    nachbilden und werden als "nicht geprüft" gemeldet.

    Ergebnisse in -OutDir: Schnappschüsse (NN-*.json), Vergleiche (diff-*.md),
    bericht.md, zusammenfassung.md (kurz, für die Job-Zusammenfassung) und
    ergebnis.json. Exit-Code: 0 = keine Fehler, 1 = mindestens ein Fehler,
    2 = Probe abgebrochen.

.PARAMETER Scenario
    frisch, koexistenz, upgrade, upgrade-beta1, upgrade-beta2,
    deinstall-policies oder assistent.

.PARAMETER Setup
    Setup des Prüflings (*-Setup.exe aus dem CI-Artefakt windows-cl-x86_64).

.PARAMETER OutDir
    Ordner für Schnappschüsse und Berichte. Nicht unter den erfassten Orten
    anlegen (Programme, Benutzerprofil, %TEMP%), z. B. unter RUNNER_TEMP.

.PARAMETER PreviousSetup
    upgrade, upgrade-beta1, upgrade-beta2: Setup der Vorversion.

.PARAMETER OfficialMsi
    koexistenz: MSI des offiziellen Clients 6.0.3. assistent: optional, wird
    vorher installiert (und im ersten Durchgang gestartet).

.PARAMETER LoginScript
    assistent: test/e2e/oauth/login.py aus dem Zweig sync-e2e. Benutzer und
    Kennwort des Testkontos kommen aus OC_USER/OC_PASS; die Probe nimmt sie
    sofort aus der Umgebung, damit der Client sie nicht erbt.

.PARAMETER ServerUrl
    assistent: Server mit der App "OAuth2", an dem das Konto entsteht.

.PARAMETER WrongServerUrl
    assistent: Server ohne die App "OAuth2" (Warnung im Assistenten); auch das
    Konto des offiziellen Clients zeigt dorthin.

.PARAMETER ExpectedVersion
    Erwartete DisplayVersion des Prüflings, z. B. 7.11.1; leer = nicht prüfen.

.PARAMETER TimeoutMinutes
    Höchstdauer je Setup- oder Deinstallationslauf.

.PARAMETER AllowLocal
    Lauf außerhalb von GitHub Actions erlauben - nur auf einer Wegwerf-VM.

.EXAMPLE
    .\probe.ps1 -Scenario frisch -Setup D:\a\_temp\pruefling\x-Setup.exe -OutDir D:\a\_temp\probe -ExpectedVersion 7.11.1
#>
[CmdletBinding()]
param(
    [Parameter(Mandatory = $true)]
    [ValidateSet('frisch', 'koexistenz', 'upgrade', 'upgrade-beta1', 'upgrade-beta2', 'deinstall-policies', 'assistent')]
    [string] $Scenario,

    [Parameter(Mandatory = $true)]
    [string] $Setup,

    [Parameter(Mandatory = $true)]
    [string] $OutDir,

    [string] $PreviousSetup,

    [string] $OfficialMsi,

    [string] $ExpectedVersion,

    [string] $LoginScript,

    [string] $ServerUrl = 'https://testoo.bw.tech',

    [string] $WrongServerUrl = 'https://testool.bw.tech',

    [ValidateRange(1, 120)]
    [int] $TimeoutMinutes = 8,

    [switch] $AllowLocal
)

$ErrorActionPreference = 'Stop'
Set-StrictMode -Version 2.0

# assistent: Zugang des Testkontos sofort aus der Umgebung nehmen. Gestartete Programme erben sie sonst, und
# ein Speicherabbild des Clients trüge sie ins (öffentliche) Artefakt; dasselbe gilt für das Token des Jobs.
$loginUser = $env:OC_USER
$loginPass = $env:OC_PASS
$loginServer = $env:OC_URL
foreach ($name in 'OC_USER', 'OC_PASS', 'OC_URL', 'GH_TOKEN', 'GITHUB_TOKEN') {
    [Environment]::SetEnvironmentVariable($name, $null, 'Process')
}

if (-not $AllowLocal -and $env:GITHUB_ACTIONS -ne 'true') {
    [Console]::Error.WriteLine('probe.ps1 installiert und deinstalliert Software und legt Client-Spuren an. Nur auf einer Wegwerf-Maschine ausführen (CI-Runner oder VM-Snapshot); außerhalb von GitHub Actions nur mit -AllowLocal.')
    exit 2
}

$appName = 'owncloud.online'
# gehört eindeutig zu unserem Client
$ownPattern = 'owncloud\.online'
# gehört zu einem der beiden Clients: Namen, Pfade sowie die mit dem
# offiziellen Client geteilten COM-Klassen und Overlay-Namen (Audit H3) und
# die eigenen seit 11.0 ("  OCOError" usw., {CCD18E2x-...}, {27041A83-...})
$clientPattern = 'owncloud|OCOverlays|OCContextMenu|\bOCO?(Error|OK|Sync|Warning)(Shared)?\b|0960F09[0-7]-F328-48A3-B746-276B1E3C3722|841A0AAD-AA11-4B50-84D9-7F8E727D77D7|CCD18E2[0-7]-5D74-4A54-AFCF-65346B89869F|27041A83-BC80-4E2F-AB7C-D6A0F13870CC'
# Laufzeitspuren ändern sich durch den Betrieb des offiziellen Clients - sie
# zählen nicht zu seinem Fußabdruck, der unverändert bleiben muss
$footprintSkip = @('processes', 'files.user', 'syncfolders')
$delegateFolderClsid = '{0E5AAE11-A475-4C5B-AB00-C66DE400274E}'
$runKeyPath = 'Software\Microsoft\Windows\CurrentVersion\Run'

$programFiles = if ($env:ProgramW6432) { $env:ProgramW6432 } else { $env:ProgramFiles }
$snapshotScript = Join-Path $PSScriptRoot 'snapshot.ps1'
$compareScript = Join-Path $PSScriptRoot 'compare.ps1'
$OutDir = $ExecutionContext.SessionState.Path.GetUnresolvedProviderPathFromPSPath($OutDir)
New-Item -ItemType Directory -Force -Path $OutDir | Out-Null

$hkcu = [Microsoft.Win32.RegistryKey]::OpenBaseKey([Microsoft.Win32.RegistryHive]::CurrentUser, [Microsoft.Win32.RegistryView]::Registry64)
$hklm = [Microsoft.Win32.RegistryKey]::OpenBaseKey([Microsoft.Win32.RegistryHive]::LocalMachine, [Microsoft.Win32.RegistryView]::Registry64)

# deinstall-policies: Richtlinien liest der Client aus HKLM\Software\Policies\<Hersteller>\<Produkt>
# (AppConfig::configPath, 64-Bit-Sicht; Software\Policies teilen sich beide Sichten)
$policyVendorPath = 'SOFTWARE\Policies\BW-Tech GmbH'
$policyKeyPath = "$policyVendorPath\$appName"

$checks = New-Object 'System.Collections.Generic.List[object]'
$runs = New-Object 'System.Collections.Generic.List[object]'
$lists = New-Object 'System.Collections.Generic.List[string]'
$snapshotCount = 0

# upgrade-beta2: Konto- und Ordnerkonfiguration, wie 11.0.0-beta2 sie schreibt
# (ConfigFile, AccountManager::saveAccount, FolderDefinition::save und die
# Liste der Anmeldedaten im CredentialManager). Erzeugt mit QSettings aus Qt
# 6.8 mit denselben Schlüsseln und Typen; QUuid, QUrl und die Capabilities
# stehen deshalb als @Variant darin; davUrl ohne Schrägstrich am Ende, den
# FolderDefinition schon in beta2 entfernt. ConfigFile schreibt ohne Gruppe,
# QSettings legt das als [General] ab ([%General] wäre eine Gruppe "General",
# die der Client nie liest); optionalDesktopNotifications und monoIcons weichen
# vom Standard ab. Get-Beta2Config setzt Sync-Ordner, Version und VFS-Modus
# ein. Das Konto zeigt auf probe.invalid: Der Prüfling erreicht keinen Server,
# liest und schreibt aber Konto und Ordnerpaar.
$beta2AccountId = '3f1c2b7e-5a64-4d2e-9b1f-0c7d8e9a1b2c'
$beta2FolderId = '8d5e2c41-7b3a-4f69-a0d2-6e1f4b9c3a57'
$beta2ConfigTemplate = @'
[General]
clientVersion=PROBE_CLIENTVERSION
probe=true
optionalDesktopNotifications=false
monoIcons=true

[Accounts]
0\url=https://probe.invalid
0\dav_user=probe
0\display-name=Probe
0\account-alias=probe@probe.invalid
0\uuid="@Variant(\0\0\0\x7f\0\0\0\x6QUuid\0?\x1c+~ZdM.\x9b\x1f\f}\x8e\x9a\x1b,)"
0\capabilities=@Variant(\0\0\0\b\0\0\0\x4\0\0\0\x12\0\x63\0h\0\x65\0\x63\0k\0s\0u\0m\0s\0\0\0\b\0\0\0\x2\0\0\0&\0p\0r\0\x65\0\x66\0\x65\0r\0r\0\x65\0\x64\0U\0p\0l\0o\0\x61\0\x64\0T\0y\0p\0\x65\0\0\0\n\0\0\0\b\0S\0H\0\x41\0\x31\0\0\0\x1c\0s\0u\0p\0p\0o\0r\0t\0\x65\0\x64\0T\0y\0p\0\x65\0s\0\0\0\t\0\0\0\x1\0\0\0\n\0\0\0\b\0S\0H\0\x41\0\x31\0\0\0\b\0\x63\0o\0r\0\x65\0\0\0\b\0\0\0\x2\0\0\0\x18\0p\0o\0l\0l\0i\0n\0t\0\x65\0r\0v\0\x61\0l\0\0\0\x2\0\0\0<\0\0\0\f\0s\0t\0\x61\0t\0u\0s\0\0\0\b\0\0\0\a\0\0\0\xe\0\x65\0\x64\0i\0t\0i\0o\0n\0\0\0\n\0\0\0\x12\0\x43\0o\0m\0m\0u\0n\0i\0t\0y\0\0\0\x12\0i\0n\0s\0t\0\x61\0l\0l\0\x65\0\x64\0\0\0\n\0\0\0\x2\0\x31\0\0\0\x16\0m\0\x61\0i\0n\0t\0\x65\0n\0\x61\0n\0\x63\0\x65\0\0\0\n\0\0\0\x2\0\x30\0\0\0\x1c\0n\0\x65\0\x65\0\x64\0s\0\x44\0\x62\0U\0p\0g\0r\0\x61\0\x64\0\x65\0\0\0\n\0\0\0\x2\0\x30\0\0\0\x16\0p\0r\0o\0\x64\0u\0\x63\0t\0n\0\x61\0m\0\x65\0\0\0\n\0\0\0\x1e\0o\0w\0n\0\x63\0l\0o\0u\0\x64\0.\0o\0n\0l\0i\0n\0\x65\0\0\0\xe\0v\0\x65\0r\0s\0i\0o\0n\0\0\0\n\0\0\0\x10\0\x31\0\x31\0.\0\x30\0.\0\x30\0.\0\x30\0\0\0\x1a\0v\0\x65\0r\0s\0i\0o\0n\0s\0t\0r\0i\0n\0g\0\0\0\n\0\0\0\f\0\x31\0\x31\0.\0\x30\0.\0\x30\0\0\0\x6\0\x64\0\x61\0v\0\0\0\b\0\0\0\x1\0\0\0\x10\0\x63\0h\0u\0n\0k\0i\0n\0g\0\0\0\n\0\0\0\x6\0\x31\0.\0\x30\0\0\0\x1a\0\x66\0i\0l\0\x65\0s\0_\0s\0h\0\x61\0r\0i\0n\0g\0\0\0\b\0\0\0\x1\0\0\0\x16\0\x61\0p\0i\0_\0\x65\0n\0\x61\0\x62\0l\0\x65\0\x64\0\0\0\x1\x1)
0\default_sync_root=C:/PROBE_SYNCDIR
0\userExplicitlySignedOut=false
0\Folders\8d5e2c41-7b3a-4f69-a0d2-6e1f4b9c3a57\localPath=C:/PROBE_SYNCDIR/
0\Folders\8d5e2c41-7b3a-4f69-a0d2-6e1f4b9c3a57\journalPath=.sync_5f3a9c0e1b7d.db
0\Folders\8d5e2c41-7b3a-4f69-a0d2-6e1f4b9c3a57\targetPath=/
0\Folders\8d5e2c41-7b3a-4f69-a0d2-6e1f4b9c3a57\davUrl=@Variant(\0\0\0\x11\0\0\0\x30https://probe.invalid/remote.php/dav/files/probe)
0\Folders\8d5e2c41-7b3a-4f69-a0d2-6e1f4b9c3a57\displayName=Probe
0\Folders\8d5e2c41-7b3a-4f69-a0d2-6e1f4b9c3a57\paused=false
0\Folders\8d5e2c41-7b3a-4f69-a0d2-6e1f4b9c3a57\deployed=false
0\Folders\8d5e2c41-7b3a-4f69-a0d2-6e1f4b9c3a57\priority=0
0\Folders\8d5e2c41-7b3a-4f69-a0d2-6e1f4b9c3a57\virtualFilesMode=PROBE_VFSMODE

[Credentials]
owncloud.online_credentials%3Aprobe.invalid%3A3f1c2b7e-5a64-4d2e-9b1f-0c7d8e9a1b2c\http\oauthtoken=true
'@

# Client-Spuren; Schlüssel = Name der Spur in den Prüfungen
$trace = @{
    # upgrade-beta2: zum Konto der Konfiguration, sonst eine beliebige Kennung
    Credential = '{0}_credentials:probe.invalid:{1}:http/oauthtoken' -f $appName, $(if ($Scenario -eq 'upgrade-beta2') { $beta2AccountId } else { [guid]::NewGuid() })
    ConfigDir  = Join-Path ([Environment]::GetFolderPath('ApplicationData')) $appName
    CacheDir   = Join-Path ([Environment]::GetFolderPath('LocalApplicationData')) $appName
    NavClsid   = '{' + [guid]::NewGuid().ToString().ToUpperInvariant() + '}'
    SyncDir    = Join-Path $env:USERPROFILE $appName
    UserHash   = $null
    ConfigWritten = $false
    # upgrade-beta2: clientVersion der Vorversion, wie sie in deren Konfiguration steht
    PreviousVersion = ''
    # upgrade-beta2: vom Prüfling angemeldete Sync-Roots (Kennung, Explorer-Eintrag), siehe U21 und D9
    SyncRoots = @()
}
$trace.UserFile = Join-Path $trace.SyncDir 'Probe-Nutzerdatei.txt'
$traceErrors = @{}

# ---------------------------------------------------------------------------
# Werkzeuge
# ---------------------------------------------------------------------------

# GitHub-Annotation: der Runner meldet sie laufend an den Server, sie bleiben
# also auch sichtbar, wenn der Runner unterwegs verloren geht
function Write-Annotation {
    param([string] $Level, [string] $Message)
    if ($env:GITHUB_ACTIONS -ne 'true') { return }
    $text = (($Message -replace '%', '%25') -replace "`r", '%0D') -replace "`n", '%0A'
    Write-Host "::$Level title=Install-Probe $Scenario::$text"
}

function Add-Check {
    param([string] $Id, [string] $Title, [string] $Status, [string] $Detail = '', [string] $Audit = '')
    $checks.Add([pscustomobject]@{ id = $Id; pruefung = $Title; ergebnis = $Status; audit = $Audit; detail = $Detail })
    $suffix = if ($Detail) { ': ' + $Detail } else { '' }
    Write-Host ('[{0}] {1} {2}{3}' -f $Status, $Id, $Title, $suffix)
    if ($Status -eq 'FEHLER') { Write-Annotation 'error' "$Id $Title$suffix" }
}

# lange Listen für bericht.md (nicht für die Job-Zusammenfassung)
function Add-List {
    param([string] $Title, [object[]] $Entries)
    if ($null -eq $Entries -or $Entries.Count -eq 0) { return }
    $lists.Add('')
    $lists.Add("### $Title")
    $lists.Add('')
    $lists.Add('```diff')
    foreach ($entry in @($Entries | Select-Object -First 300)) { $lists.Add((([string]$entry) -replace '```', "'''")) }
    if ($Entries.Count -gt 300) { $lists.Add(('… {0} weitere' -f ($Entries.Count - 300))) }
    $lists.Add('```')
}

function Format-Diff {
    param($Diff)
    switch ($Diff.Change) {
        'neu' { return ('+ [{0}] {1} = {2}' -f $Diff.Category, $Diff.Key, $Diff.After) }
        'entfernt' { return ('- [{0}] {1} = {2}' -f $Diff.Category, $Diff.Key, $Diff.Before) }
        default { return ('~ [{0}] {1}: {2} -> {3}' -f $Diff.Category, $Diff.Key, $Diff.Before, $Diff.After) }
    }
}

function Get-Property {
    param($Object, [string] $Name)
    if ($null -eq $Object) { return $null }
    $property = $Object.PSObject.Properties[$Name]
    if ($property) { return $property.Value }
    return $null
}

function Test-UnderPath {
    param([string] $Path, [string[]] $Roots)
    foreach ($root in $Roots) {
        if (-not $root) { continue }
        $trimmed = $root.TrimEnd('\')
        if ($Path.Equals($trimmed, [StringComparison]::OrdinalIgnoreCase) -or
            $Path.StartsWith($trimmed + '\', [StringComparison]::OrdinalIgnoreCase)) { return $true }
    }
    return $false
}

function Split-CommandLine {
    param([string] $CommandLine)
    $line = $CommandLine.Trim()
    if ($line -match '^"([^"]+)"\s*(.*)$') { return @($Matches[1], $Matches[2]) }
    if ($line -match '^(\S+)\s*(.*)$') { return @($Matches[1], $Matches[2]) }
    throw "Befehlszeile nicht lesbar: $CommandLine"
}

function Get-SignatureNote {
    param([string] $Path)
    try {
        $signature = Get-AuthenticodeSignature -FilePath $Path
        $signer = '-'
        if ($signature.SignerCertificate) { $signer = $signature.SignerCertificate.Subject -replace '^CN="?([^,"]+)"?.*$', '$1' }
        return '{0} ({1})' -f $signature.Status, $signer
    } catch {
        return 'nicht prüfbar'
    }
}

# --- Schnappschüsse ---------------------------------------------------------

function Read-Snapshot {
    param([string] $Path)
    $document = [IO.File]::ReadAllText($Path, [Text.Encoding]::UTF8) | ConvertFrom-Json
    $categories = [ordered]@{}
    foreach ($category in $document.categories.PSObject.Properties) {
        $items = New-Object 'System.Collections.Generic.Dictionary[string,string]' ([StringComparer]::OrdinalIgnoreCase)
        foreach ($item in $category.Value.items.PSObject.Properties) { $items[$item.Name] = [string]$item.Value }
        $categories[$category.Name] = $items
    }
    return [pscustomobject]@{ Path = $Path; Categories = $categories }
}

function Get-Items {
    param($Snapshot, [string] $Id)
    if ($Snapshot.Categories.Contains($Id)) { return $Snapshot.Categories[$Id] }
    return New-Object 'System.Collections.Generic.Dictionary[string,string]' ([StringComparer]::OrdinalIgnoreCase)
}

function New-Snapshot {
    param([string] $Name, [string] $Label)
    $script:snapshotCount++
    $file = Join-Path $OutDir ('{0:D2}-{1}.json' -f $script:snapshotCount, $Name)
    Write-Host "== Schnappschuss $Label"
    & $snapshotScript -OutFile $file -Label $Label | Out-Null
    $snapshot = Read-Snapshot $file
    # Zwischenstand sichern: auch ein abgebrochener Job liefert dann Ergebnisse
    Write-Report $null
    return $snapshot
}

function Get-Diff {
    param($From, $To, [string] $Name, [string[]] $Exclude = @())
    $parameters = @{ Before = $From.Path; After = $To.Path; OutFile = (Join-Path $OutDir "diff-$Name.md"); PassThru = $true }
    if ($Exclude.Count -gt 0) { $parameters.ExcludeCategory = $Exclude }
    return @(& $compareScript @parameters)
}

# Wert aus dem Schnappschuss ohne Typ-Präfix ("REG_SZ: ...")
function Get-Data {
    param([string] $Value)
    return ($Value -replace '^REG_[A-Z_]+: ', '')
}

# Einträge in Programme und Features, deren DisplayName auf Pattern passt
function Get-UninstallEntries {
    param($Snapshot, [string] $Pattern)
    $byKey = @{}
    $items = Get-Items $Snapshot 'reg.uninstall'
    foreach ($key in $items.Keys) {
        $separator = $key.IndexOf(' :: ')
        if ($separator -lt 0) { continue }
        $path = $key.Substring(0, $separator)
        if (-not $byKey.ContainsKey($path)) { $byKey[$path] = @{} }
        $byKey[$path][$key.Substring($separator + 4)] = Get-Data $items[$key]
    }
    foreach ($path in @($byKey.Keys | Sort-Object)) {
        $values = $byKey[$path]
        if ($values.ContainsKey('DisplayName') -and $values['DisplayName'] -match $Pattern) {
            [pscustomobject]@{ Key = $path; Values = $values }
        }
    }
}

function Get-EntryValue {
    param($Entry, [string] $Name)
    if ($Entry.Values.ContainsKey($Name)) { return [string]$Entry.Values[$Name] }
    return ''
}

# --- Setups -----------------------------------------------------------------

# Ereignis 1074: ein Prozess hat Neustart oder Herunterfahren ausgelöst
function Get-RestartEventCount {
    param([datetime] $Since)
    try {
        return @(Get-WinEvent -FilterHashtable @{ LogName = 'System'; Id = 1074; StartTime = $Since } -ErrorAction Stop).Count
    } catch {
        return 0
    }
}

function Invoke-Step {
    param([string] $Label, [string] $File, [string] $Arguments)
    Write-Host "== $Label"
    Write-Host "   $File $Arguments"
    Write-Annotation 'notice' "Start: $Label"
    $start = Get-Date
    $process = Start-Process -FilePath $File -ArgumentList $Arguments -PassThru
    # ohne diesen Zugriff liefert ExitCode nach dem Ende $null
    $null = $process.Handle
    $timedOut = -not $process.WaitForExit($TimeoutMinutes * 60 * 1000)
    $exitCode = $null
    $hang = ''
    if ($timedOut) {
        # festhalten, worauf der Schritt wartet
        $children = @(Get-CimInstance -ClassName Win32_Process -Filter "ParentProcessId=$($process.Id)" -ErrorAction SilentlyContinue | ForEach-Object { $_.Name })
        $windows = @(Get-Process -ErrorAction SilentlyContinue | Where-Object { $_.MainWindowTitle } | ForEach-Object { '{0}: "{1}"' -f $_.ProcessName, $_.MainWindowTitle })
        $hang = 'Kindprozesse: {0}; Fenster: {1}' -f $(if ($children.Count) { $children -join ', ' } else { '-' }), $(if ($windows.Count) { $windows -join '; ' } else { '-' })
        Write-Host "   hängt - $hang"
        # ganzen Prozessbaum beenden, damit nichts in spätere Schritte hineinläuft
        $null = & taskkill.exe /PID $process.Id /T /F
        try { if (-not $process.HasExited) { $process.Kill() } } catch { Write-Host "   Beenden fehlgeschlagen: $($_.Exception.Message)" }
    } else {
        $exitCode = $process.ExitCode
    }
    $restarts = Get-RestartEventCount $start
    $run = [pscustomobject]@{
        schritt             = $Label
        befehl              = "$File $Arguments"
        exitcode            = $exitCode
        sekunden            = [int]((Get-Date) - $start).TotalSeconds
        zeitueberschreitung = $timedOut
        haengt              = $hang
        neustartereignisse  = $restarts
    }
    $runs.Add($run)
    $shown = if ($timedOut) { 'Zeitüberschreitung' } else { [string]$exitCode }
    Write-Host ('   Exit-Code {0} nach {1} s' -f $shown, $run.sekunden)
    return $run
}

function Test-Run {
    param($Run, [string] $Id, [string] $Title, [string] $Audit = 'H7')
    if ($Run.zeitueberschreitung) {
        Add-Check $Id $Title 'FEHLER' "nach $TimeoutMinutes min abgebrochen, wartet vermutlich auf eine Eingabe ($($Run.haengt))" $Audit
        return
    }
    $problems = @()
    if ($Run.exitcode -ne 0 -and $Run.exitcode -ne 3010) { $problems += "Exit-Code $($Run.exitcode)" }
    if ($Run.neustartereignisse -gt 0) { $problems += 'Neustart ausgelöst (Ereignis 1074)' }
    if ($problems.Count -gt 0) {
        Add-Check $Id $Title 'FEHLER' ($problems -join '; ') $Audit
    } else {
        $note = if ($Run.exitcode -eq 3010) { ' (Neustart ausstehend)' } else { '' }
        Add-Check $Id $Title 'OK' ('Exit-Code {0}{1}, {2} s' -f $Run.exitcode, $note, $Run.sekunden) $Audit
    }
}

function Install-Candidate {
    param([string] $Id, [string] $Label = 'Setup (Prüfling)')
    $run = Invoke-Step $Label $Setup '/S /norestart'
    Test-Run $run $Id "$Label still, ohne Neustart"
}

# alle installierten Programmdateien unseres Clients unter Programme und Programme (x86)
function Get-ClientExes {
    foreach ($root in @($programFiles, ${env:ProgramFiles(x86)}) | Where-Object { $_ } | Select-Object -Unique) {
        foreach ($dir in @(Get-ChildItem -LiteralPath $root -Directory -ErrorAction SilentlyContinue | Where-Object { $_.Name -match $ownPattern })) {
            Get-ChildItem -LiteralPath $dir.FullName -Recurse -File -Filter "$appName.exe" -ErrorAction SilentlyContinue | ForEach-Object { $_.FullName }
        }
    }
}

function Find-ClientExe {
    foreach ($root in @($programFiles, ${env:ProgramFiles(x86)}) | Where-Object { $_ } | Select-Object -Unique) {
        foreach ($dir in @(Get-ChildItem -LiteralPath $root -Directory -ErrorAction SilentlyContinue | Where-Object { $_.Name -match $ownPattern })) {
            $exe = Get-ChildItem -LiteralPath $dir.FullName -Recurse -File -Filter "$appName.exe" -ErrorAction SilentlyContinue | Select-Object -First 1
            if ($exe) { return $exe.FullName }
        }
    }
    return $null
}

# $Interactive: der UninstallString, den Programme und Features beim Deinstallieren von Hand startet
function Get-OwnUninstallCommand {
    param([switch] $Interactive)
    foreach ($root in 'HKLM:\SOFTWARE\Microsoft\Windows\CurrentVersion\Uninstall',
                      'HKLM:\SOFTWARE\WOW6432Node\Microsoft\Windows\CurrentVersion\Uninstall',
                      'HKCU:\Software\Microsoft\Windows\CurrentVersion\Uninstall') {
        if (-not (Test-Path -LiteralPath $root)) { continue }
        foreach ($key in @(Get-ChildItem -LiteralPath $root -ErrorAction SilentlyContinue)) {
            $values = Get-ItemProperty -LiteralPath $key.PSPath -ErrorAction SilentlyContinue
            $name = Get-Property $values 'DisplayName'
            if (-not $name -or $name -notmatch $ownPattern) { continue }
            $quiet = Get-Property $values 'QuietUninstallString'
            if ($quiet -and -not $Interactive) { return [string]$quiet }
            $plain = Get-Property $values 'UninstallString'
            if ($plain) { return [string]$plain }
        }
    }
    return $null
}

# $Purge: /purge anhängen - entfernt nach E4 auch Einstellungen, Konten und
# Anmeldedaten; ohne behält die stille Deinstallation sie
function Uninstall-Candidate {
    param([string] $Id, [switch] $Purge)
    $extra = if ($Purge) { '/purge ' } else { '' }
    $title = 'Deinstallation still {0}ohne Neustart' -f $(if ($Purge) { 'mit /purge, ' } else { '(Daten behalten), ' })
    $command = Get-OwnUninstallCommand
    if (-not $command) {
        Add-Check $Id $title 'FEHLER' 'kein Eintrag mit owncloud.online in Programme und Features' 'H19'
        return
    }
    $parts = @(Split-CommandLine $command)
    $arguments = $parts[1]
    # NSIS wertet nur das große /S aus
    if ($arguments -cnotmatch '(^|\s)/S(\s|$)') { $arguments = "/S $arguments" }
    $run = Invoke-Step 'Deinstallation (Prüfling)' $parts[0] ("$arguments $extra/norestart".Trim())
    Test-Run $run $Id $title
}

# Fenster und Steuerelemente eines anderen Prozesses lesen und eine Schaltfläche auslösen (Win32).
# Texte über WM_GETTEXT, das Windows zwischen Prozessen überträgt.
$uiSource = @'
using System;
using System.Collections.Generic;
using System.Runtime.InteropServices;
using System.Text;

namespace OcProbe
{
    public class UiControl
    {
        public IntPtr Handle;
        public string ClassName;
        public int Id;
        public int Style;
        public bool Visible;
        public string Text;
    }

    public static class Ui
    {
        private delegate bool EnumProc(IntPtr hwnd, IntPtr lParam);

        [DllImport("user32.dll")]
        private static extern bool EnumWindows(EnumProc callback, IntPtr lParam);
        [DllImport("user32.dll")]
        private static extern bool EnumChildWindows(IntPtr parent, EnumProc callback, IntPtr lParam);
        [DllImport("user32.dll")]
        private static extern uint GetWindowThreadProcessId(IntPtr hwnd, out uint processId);
        [DllImport("user32.dll")]
        private static extern bool IsWindowVisible(IntPtr hwnd);
        [DllImport("user32.dll", CharSet = CharSet.Unicode)]
        private static extern int GetClassName(IntPtr hwnd, StringBuilder name, int size);
        [DllImport("user32.dll")]
        private static extern int GetDlgCtrlID(IntPtr hwnd);
        [DllImport("user32.dll")]
        private static extern IntPtr GetDlgItem(IntPtr dialog, int id);
        [DllImport("user32.dll", EntryPoint = "GetWindowLongW")]
        private static extern int GetWindowLong(IntPtr hwnd, int index);
        [DllImport("user32.dll", EntryPoint = "SendMessageTimeoutW", CharSet = CharSet.Unicode)]
        private static extern IntPtr SendMessageTimeoutText(IntPtr hwnd, uint message, IntPtr wParam, StringBuilder lParam, uint flags, uint timeout, out IntPtr result);
        [DllImport("user32.dll", EntryPoint = "SendMessageTimeoutW")]
        private static extern IntPtr SendMessageTimeout(IntPtr hwnd, uint message, IntPtr wParam, IntPtr lParam, uint flags, uint timeout, out IntPtr result);
        [DllImport("user32.dll", EntryPoint = "PostMessageW")]
        private static extern bool PostMessage(IntPtr hwnd, uint message, IntPtr wParam, IntPtr lParam);

        private const uint WM_GETTEXT = 0x000D;
        private const uint WM_COMMAND = 0x0111;
        private const uint BM_GETCHECK = 0x00F0;
        private const uint SMTO_ABORTIFHUNG = 0x0002;

        // sichtbare Fenster oberster Ebene eines Prozesses
        public static UiControl[] TopWindows(int processId)
        {
            List<UiControl> found = new List<UiControl>();
            EnumWindows(delegate (IntPtr hwnd, IntPtr lParam)
            {
                uint owner;
                GetWindowThreadProcessId(hwnd, out owner);
                if (owner == (uint)processId && IsWindowVisible(hwnd)) { found.Add(Describe(hwnd)); }
                return true;
            }, IntPtr.Zero);
            return found.ToArray();
        }

        // alle Nachfahren eines Fensters
        public static UiControl[] Controls(IntPtr top)
        {
            List<UiControl> found = new List<UiControl>();
            EnumChildWindows(top, delegate (IntPtr hwnd, IntPtr lParam)
            {
                found.Add(Describe(hwnd));
                return true;
            }, IntPtr.Zero);
            return found.ToArray();
        }

        private static UiControl Describe(IntPtr hwnd)
        {
            StringBuilder name = new StringBuilder(256);
            GetClassName(hwnd, name, name.Capacity);
            UiControl control = new UiControl();
            control.Handle = hwnd;
            control.ClassName = name.ToString();
            control.Id = GetDlgCtrlID(hwnd);
            control.Style = GetWindowLong(hwnd, -16);
            control.Visible = IsWindowVisible(hwnd);
            control.Text = Text(hwnd);
            return control;
        }

        public static string Text(IntPtr hwnd)
        {
            StringBuilder text = new StringBuilder(1024);
            IntPtr result;
            if (SendMessageTimeoutText(hwnd, WM_GETTEXT, new IntPtr(text.Capacity), text, SMTO_ABORTIFHUNG, 2000, out result) == IntPtr.Zero) { return ""; }
            return text.ToString();
        }

        public static bool IsChecked(IntPtr hwnd)
        {
            IntPtr result;
            if (SendMessageTimeout(hwnd, BM_GETCHECK, IntPtr.Zero, IntPtr.Zero, SMTO_ABORTIFHUNG, 2000, out result) == IntPtr.Zero) { return false; }
            return result.ToInt64() == 1;
        }

        // wie ein Klick: WM_COMMAND/BN_CLICKED an den Dialog, der die Schaltfläche enthält
        public static bool Click(IntPtr dialog, int id)
        {
            IntPtr button = GetDlgItem(dialog, id);
            if (button == IntPtr.Zero) { return false; }
            return PostMessage(dialog, WM_COMMAND, new IntPtr(id), button);
        }
    }
}
'@

# Deinstallation über die Oberfläche, wie von Hand aus Programme und Features (UninstallString): die
# Vorauswahl der Wahlseite übernehmen und "Deinstallieren" wählen. Die Abschlussseite bietet den
# Neustart an, "Jetzt neu starten" ist vorausgewählt - die Probe wählt dort nichts, sondern beendet
# das Programm; die Deinstallation ist dann abgeschlossen (die Seite folgt erst danach).
function Uninstall-CandidateInteractive {
    param([string] $Id)
    $title = 'Deinstallation über die Oberfläche (Vorauswahl), ohne Neustart'
    $command = Get-OwnUninstallCommand -Interactive
    if (-not $command) {
        Add-Check $Id $title 'FEHLER' 'kein Eintrag mit owncloud.online in Programme und Features' 'H19'
        return
    }
    $parts = @(Split-CommandLine $command)
    if ($parts[1] -cmatch '(^|\s)/S(\s|$)') {
        Add-Check $Id $title 'FEHLER' "der UninstallString deinstalliert still: $command" 'H19'
        return
    }
    if (-not ('OcProbe.Ui' -as [type])) { Add-Type -TypeDefinition $uiSource -Language CSharp }
    $label = 'Deinstallation (Prüfling, über die Oberfläche)'
    Write-Host "== $label"
    Write-Host "   $command"
    Write-Annotation 'notice' "Start: $label"
    $start = Get-Date
    if ($parts[1]) {
        $process = Start-Process -FilePath $parts[0] -ArgumentList $parts[1] -PassThru
    } else {
        $process = Start-Process -FilePath $parts[0] -PassThru
    }
    $null = $process.Handle
    $deadline = $start.AddMinutes($TimeoutMinutes)
    $phase = 'Start'
    $sawProgress = $false
    $timedOut = $false
    $choices = ''
    $problem = ''
    $page = ''
    while (-not $process.HasExited) {
        if ((Get-Date) -gt $deadline) {
            $timedOut = $true
            $problem = "nach $TimeoutMinutes min nicht auf der Abschlussseite"
            break
        }
        Start-Sleep -Milliseconds 500
        # Dialogfenster: die Seiten des Programms und Meldungsfenster
        $windows = @([OcProbe.Ui]::TopWindows($process.Id) | Where-Object { $_.ClassName -eq '#32770' })
        if ($windows.Count -eq 0) { continue }
        if ($windows.Count -gt 1) {
            # neben der Seite ein Meldungsfenster, z. B. "Deinstallation fehlgeschlagen"
            $texts = foreach ($window in $windows) {
                @([OcProbe.Ui]::Controls($window.Handle) | Where-Object { $_.Visible -and $_.Text -and $_.ClassName -ne 'Button' } | ForEach-Object { $_.Text }) -join ' '
            }
            $problem = 'Meldungsfenster: ' + ((@($texts) -replace '\s+', ' ') -join ' | ')
            break
        }
        $controls = @([OcProbe.Ui]::Controls($windows[0].Handle) | Where-Object { $_.Visible })
        $page = (@($controls | Where-Object { $_.Text } | ForEach-Object { '{0} "{1}"' -f $_.ClassName, ($_.Text -replace '\s+', ' ') })) -join ', '
        # Schaltflächenart (BS_TYPEMASK): 2/3 Kontrollkästchen, 4/9 Optionsfeld
        $boxes = @($controls | Where-Object { $_.ClassName -eq 'Button' -and (($_.Style -band 0xF) -eq 2 -or ($_.Style -band 0xF) -eq 3) })
        $radios = @($controls | Where-Object { $_.ClassName -eq 'Button' -and (($_.Style -band 0xF) -eq 4 -or ($_.Style -band 0xF) -eq 9) })
        $progress = @($controls | Where-Object { $_.ClassName -eq 'msctls_progress32' })
        # Abschlussseite: Neustart-Wahl, oder die Fortschrittsseite ist nach getaner Arbeit weg
        if ($radios.Count -gt 0 -or ($sawProgress -and $progress.Count -eq 0)) {
            $phase = 'Abschlussseite'
            break
        }
        if ($progress.Count -gt 0) {
            $sawProgress = $true
        } elseif ($phase -eq 'Start' -and $boxes.Count -gt 0) {
            # Wahlseite: Vorauswahl festhalten und genau einmal "Deinstallieren" (Schaltfläche 1)
            $choices = (@($boxes | ForEach-Object { '{0}: {1}' -f ($_.Text -replace '&', '').Trim(), $(if ([OcProbe.Ui]::IsChecked($_.Handle)) { 'an' } else { 'aus' }) })) -join '; '
            if (-not [OcProbe.Ui]::Click($windows[0].Handle, 1)) {
                $problem = 'Schaltfläche zum Deinstallieren nicht gefunden'
                break
            }
            $phase = 'Deinstallation'
        }
    }
    if (-not $process.HasExited) {
        # nie "Fertig stellen": dort wäre "Jetzt neu starten" gewählt
        $null = & taskkill.exe /PID $process.Id /T /F
        $null = $process.WaitForExit(30 * 1000)
    } elseif ($phase -ne 'Abschlussseite' -and -not $problem) {
        $problem = 'ohne Abschlussseite beendet'
    }
    $exitCode = if ($process.HasExited) { $process.ExitCode } else { $null }
    $restarts = Get-RestartEventCount $start
    if ($restarts -gt 0) { $problem = (@($problem, 'Neustart ausgelöst (Ereignis 1074)') | Where-Object { $_ }) -join '; ' }
    $run = [pscustomobject]@{
        schritt             = $(if ($phase -eq 'Abschlussseite') { "$label, auf der Abschlussseite beendet" } else { $label })
        befehl              = $command
        exitcode            = $exitCode
        sekunden            = [int]((Get-Date) - $start).TotalSeconds
        zeitueberschreitung = $timedOut
        haengt              = $(if ($problem) { $page } else { '' })
        neustartereignisse  = $restarts
    }
    $runs.Add($run)
    Write-Host ('   {0} nach {1} s' -f $(if ($problem) { $problem } else { 'Abschlussseite erreicht' }), $run.sekunden)
    if ($problem) {
        Add-Check $Id $title 'FEHLER' ('{0}; Seite: {1}' -f $problem, $(if ($page) { $page } else { '-' })) 'H7'
    } else {
        Add-Check $Id $title 'OK' ('Abschlussseite nach {0} s, dort beendet; Wahlseite: {1}' -f $run.sekunden, $(if ($choices) { $choices } else { 'nicht gesehen' })) 'H7'
    }
}

# --- Client-Spuren ----------------------------------------------------------

function Set-UserValue {
    param([string] $Path, [string] $Name, $Value, [Microsoft.Win32.RegistryValueKind] $Kind = [Microsoft.Win32.RegistryValueKind]::String)
    $key = $hkcu.CreateSubKey($Path)
    try { $key.SetValue($Name, $Value, $Kind) } finally { $key.Close() }
}

function Test-UserKey {
    param([string] $Path)
    $key = $hkcu.OpenSubKey($Path, $false)
    if ($null -eq $key) { return $false }
    $key.Close()
    return $true
}

function Test-UserValue {
    param([string] $Path, [string] $Name)
    $key = $hkcu.OpenSubKey($Path, $false)
    if ($null -eq $key) { return $false }
    try { return ($null -ne $key.GetValue($Name)) } finally { $key.Close() }
}

function Add-ClientTraces {
    param([string] $ClientExe)
    Write-Host '== Client-Spuren anlegen'
    if (-not $ClientExe) { $ClientExe = Join-Path $programFiles "$appName\bin\$appName.exe" }
    $steps = [ordered]@{
        'Anmeldedaten'       = {
            # wie QtKeychain: generischer Eintrag, Zielname = CredentialManager::scopedKey
            $secret = [guid]::NewGuid().ToString('N')
            $null = & cmdkey.exe "/generic:$($trace.Credential)" '/user:probe' "/pass:$secret"
            if ($LASTEXITCODE -ne 0) { throw "cmdkey endete mit $LASTEXITCODE" }
        }
        'Einstellungen'      = {
            New-Item -ItemType Directory -Force -Path $trace.ConfigDir | Out-Null
            $content = "[General]`r`nprobe=true`r`n"
            if ($Scenario -eq 'upgrade-beta2') {
                $trace.PreviousVersion = Get-VersionWithBuild $ClientExe
                $content = Get-Beta2Config $trace.PreviousVersion
            }
            [IO.File]::WriteAllText((Join-Path $trace.ConfigDir "$appName.cfg"), $content)
            $trace.ConfigWritten = $true
        }
        'Cache'              = {
            $dir = Join-Path $trace.CacheDir 'cache'
            New-Item -ItemType Directory -Force -Path $dir | Out-Null
            [IO.File]::WriteAllBytes((Join-Path $dir 'probe.bin'), [byte[]](1..64))
        }
        'Nutzerdatei'        = {
            New-Item -ItemType Directory -Force -Path $trace.SyncDir | Out-Null
            [IO.File]::WriteAllText($trace.UserFile, "Nutzerdatei der Install-Probe - muss jede Deinstallation überstehen.`r`n")
            $trace.UserHash = (Get-FileHash -LiteralPath $trace.UserFile -Algorithm SHA256 -ErrorAction Stop).Hash
        }
        'Autostart'          = {
            Set-UserValue $runKeyPath $appName ('"{0}" --background' -f $ClientExe)
        }
        'Navigationsbereich' = {
            $clsid = "Software\Classes\CLSID\$($trace.NavClsid)"
            $dword = [Microsoft.Win32.RegistryValueKind]::DWord
            Set-UserValue $clsid '' $appName
            Set-UserValue $clsid 'ApplicationName' $appName
            Set-UserValue "$clsid\DefaultIcon" '' "$ClientExe,0"
            Set-UserValue "$clsid\InProcServer32" '' (Join-Path $env:SystemRoot 'system32\shell32.dll')
            Set-UserValue "$clsid\Instance" 'CLSID' $delegateFolderClsid
            Set-UserValue "$clsid\Instance\InitPropertyBag" 'Attributes' 0x11 $dword
            Set-UserValue "$clsid\Instance\InitPropertyBag" 'TargetFolderPath' $trace.SyncDir
            Set-UserValue "$clsid\ShellFolder" 'FolderValueFlags' 0x28 $dword
            Set-UserValue "$clsid\ShellFolder" 'Attributes' 0xF080004D $dword
            Set-UserValue $clsid 'System.IsPinnedToNameSpaceTree' 1 $dword
            Set-UserValue $clsid 'SortOrderIndex' 0x41 $dword
            Set-UserValue "Software\Microsoft\Windows\CurrentVersion\Explorer\Desktop\NameSpace\$($trace.NavClsid)" '' $appName
            Set-UserValue 'Software\Microsoft\Windows\CurrentVersion\Explorer\HideDesktopIcons\NewStartPanel' $trace.NavClsid 1 $dword
        }
    }
    foreach ($name in $steps.Keys) {
        try {
            & $steps[$name]
        } catch {
            $traceErrors[$name] = $_.Exception.Message
            Write-Host "   $name nicht angelegt: $($_.Exception.Message)"
        }
    }
}

# je Spur: $true = vorhanden, $false = weg, $null = nicht angelegt/nicht lesbar
function Get-TraceState {
    param($Snapshot)
    $state = [ordered]@{}
    $credentials = Get-Items $Snapshot 'credentials'
    if ($credentials.ContainsKey('!Fehler')) {
        $state['Anmeldedaten'] = $null
    } else {
        $found = $false
        # Zielname vollständig oder (cmdkey-Rückfall) mit Typ-Präfix
        foreach ($key in $credentials.Keys) {
            if ($key.EndsWith($trace.Credential, [StringComparison]::OrdinalIgnoreCase)) { $found = $true }
        }
        $state['Anmeldedaten'] = $found
    }
    $state['Einstellungen'] = Test-Path -LiteralPath $trace.ConfigDir
    $state['Cache'] = Test-Path -LiteralPath $trace.CacheDir
    $state['Autostart'] = Test-UserValue $runKeyPath $appName
    $state['Navigationsbereich'] = (Test-UserKey "Software\Classes\CLSID\$($trace.NavClsid)") -or
        (Test-UserKey "Software\Microsoft\Windows\CurrentVersion\Explorer\Desktop\NameSpace\$($trace.NavClsid)") -or
        (Test-UserValue 'Software\Microsoft\Windows\CurrentVersion\Explorer\HideDesktopIcons\NewStartPanel' $trace.NavClsid)
    $state['Nutzerdatei'] = $false
    if ($trace.UserHash -and (Test-Path -LiteralPath $trace.UserFile)) {
        $state['Nutzerdatei'] = (Get-FileHash -LiteralPath $trace.UserFile -Algorithm SHA256 -ErrorAction Stop).Hash -eq $trace.UserHash
    }
    foreach ($name in @($state.Keys)) {
        if ($traceErrors.ContainsKey($name)) { $state[$name] = $null }
    }
    return $state
}

# Schlüssel der Client-Spuren - die prüfen eigene Checks, nicht die Rückstandsliste
function Get-TracePattern {
    # Klammern nötig: das Komma bindet in PowerShell stärker als +
    $parts = @(
        ([regex]::Escape($trace.Credential) + '$'),
        ('^' + [regex]::Escape($trace.ConfigDir) + '(\\|$)'),
        ('^' + [regex]::Escape($trace.CacheDir) + '(\\|$)'),
        ('^' + [regex]::Escape($trace.SyncDir) + '(\\| ::|$)'),
        [regex]::Escape($trace.NavClsid),
        ('^HKCU\\' + [regex]::Escape($runKeyPath) + ' :: ' + [regex]::Escape($appName) + '$')
    )
    # Sync-Roots des Prüflings und ihre Explorer-Einträge prüft D9
    foreach ($root in $trace.SyncRoots) {
        $parts += [regex]::Escape($root.Id)
        if ($root.Namespace) { $parts += [regex]::Escape($root.Namespace) }
    }
    return ($parts -join '|')
}

# E4: still ohne /purge bleiben Anmeldedaten, Einstellungen und Cache (z. B.
# für eine Neuinstallation per Softwareverteilung), mit /purge gehen sie
function Test-TracesAfterUninstall {
    param($Snapshot, [bool] $Purge)
    $state = Get-TraceState $Snapshot
    $keep = -not $Purge
    # Spur, soll vorhanden sein, Audit
    $expectations = @(
        @('Anmeldedaten', $keep, 'E4, H6'),
        @('Einstellungen', $keep, 'E4, H6'),
        @('Cache', $keep, 'E4, H6'),
        @('Autostart', $false, 'H6'),
        @('Navigationsbereich', $false, 'H4'),
        @('Nutzerdatei', $true, 'E4, H5')
    )
    $number = 2
    foreach ($expectation in $expectations) {
        $number++
        $name = $expectation[0]
        $shouldExist = $expectation[1]
        $title = if ($name -eq 'Nutzerdatei') { 'Nutzerdatei im Sync-Ordner bleibt erhalten' }
            elseif ($shouldExist) { "$name {0} erhalten (ohne /purge)" -f $(if ($name -eq 'Cache') { 'bleibt' } else { 'bleiben' }) }
            elseif ($Purge -and $expectation[2] -like 'E4*') { "$name entfernt (/purge)" }
            else { "$name entfernt" }
        if ($null -eq $state[$name]) {
            $why = if ($traceErrors.ContainsKey($name)) { 'Spur nicht angelegt: ' + $traceErrors[$name] } else { 'nicht lesbar' }
            Add-Check "D$number" $title 'NICHT GEPRÜFT' $why $expectation[2]
        } elseif ($state[$name] -eq $shouldExist) {
            Add-Check "D$number" $title 'OK' '' $expectation[2]
        } else {
            $detail = if ($shouldExist) { 'fehlt oder wurde verändert' } else { 'noch vorhanden' }
            Add-Check "D$number" $title 'FEHLER' $detail $expectation[2]
        }
    }
    if (@($trace.SyncRoots).Count -eq 0) {
        Add-Check 'D9' 'Sync-Root (CfApi), Search-Regeln, Schnellzugriff entfernt' 'NICHT GEPRÜFT' 'braucht einen laufenden Client mit Konto, im CI nicht nachbildbar' 'H4, H6'
        return
    }
    # upgrade-beta2: der Prüfling hat für den Platzhalter-Ordner einen Sync-Root angemeldet (U21);
    # Windows legt dazu einen Explorer-Eintrag an, in beiden Registry-Sichten
    $left = @(foreach ($category in 'reg.syncroot', 'reg.navpane') {
            foreach ($key in (Get-Items $Snapshot $category).Keys) {
                foreach ($root in $trace.SyncRoots) {
                    if ($key.EndsWith('\' + $root.Id, [StringComparison]::OrdinalIgnoreCase) -or
                        ($root.Namespace -and ($key.EndsWith('\' + $root.Namespace, [StringComparison]::OrdinalIgnoreCase) -or $key.EndsWith(' :: ' + $root.Namespace, [StringComparison]::OrdinalIgnoreCase)))) { $key }
                }
            }
        })
    $title = 'Sync-Root (CfApi) abgemeldet, sein Explorer-Eintrag entfernt'
    if ($left.Count -eq 0) {
        Add-Check 'D9' $title 'OK' (($trace.SyncRoots | ForEach-Object { '{0}, Explorer-Eintrag {1}' -f $_.Id, $(if ($_.Namespace) { $_.Namespace } else { '-' }) }) -join '; ') 'H4, H6'
    } else {
        Add-Check 'D9' $title 'FEHLER' ('{0} Einträge noch da, z. B. {1}' -f $left.Count, $left[0]) 'H4, H6'
        Add-List "D9 $title" $left
    }
    Add-Check 'D10' 'Search-Regeln, Schnellzugriff entfernt' 'NICHT GEPRÜFT' 'Search-Regel nur mit laufender Windows-Suche, Schnellzugriff nur beim Anlegen eines Ordnerpaars - im CI nicht nachbildbar' 'H4, H6'
}

function Test-TracesAfterUpgrade {
    param($Snapshot)
    $state = Get-TraceState $Snapshot
    # Cache und Explorer-Eintrag legt der Client beim nächsten Start neu an
    $soft = @('Cache', 'Navigationsbereich')
    $number = 1
    foreach ($name in $state.Keys) {
        $number++
        $title = "$name nach dem Update erhalten"
        if ($null -eq $state[$name]) {
            $why = if ($traceErrors.ContainsKey($name)) { 'Spur nicht angelegt: ' + $traceErrors[$name] } else { 'nicht lesbar' }
            Add-Check "U$number" $title 'NICHT GEPRÜFT' $why ''
        } elseif ($state[$name]) {
            Add-Check "U$number" $title 'OK' '' ''
        } elseif ($soft -contains $name) {
            Add-Check "U$number" $title 'WARNUNG' 'entfernt (der Client legt es beim nächsten Start neu an)' ''
        } else {
            Add-Check "U$number" $title 'FEHLER' 'durch das Update entfernt oder verändert' ''
        }
    }
}

# upgrade-beta1, upgrade-beta2: Die Vorversion trägt dieselbe Kennung wie der Prüfling. Das
# Update muss sie an Ort und Stelle ersetzen, nicht daneben installieren oder unverändert lassen.
function Test-UpgradeFromBeta {
    param($Previous, $Installed, [string] $PreviousExe, [string] $PreviousExeHash, [string] $Exe, [string] $PreviousVersion)
    $before = @(Get-UninstallEntries $Previous $ownPattern)
    $after = @(Get-UninstallEntries $Installed $ownPattern)
    $beforeVersion = if ($before.Count -eq 1) { Get-EntryValue $before[0] 'DisplayVersion' } else { '' }
    $afterVersion = if ($after.Count -eq 1) { Get-EntryValue $after[0] 'DisplayVersion' } else { '' }

    # sonst prüfte das Szenario etwas anderes, als es vorgibt
    if ($before.Count -eq 1 -and $beforeVersion -match ('^' + [regex]::Escape($PreviousVersion) + '(\D|$)')) {
        Add-Check 'U8' "Vorversion $PreviousVersion war installiert" 'OK' $beforeVersion ''
    } else {
        Add-Check 'U8' "Vorversion $PreviousVersion war installiert" 'FEHLER' ("{0} Einträge vor dem Update, Version '{1}'" -f $before.Count, $beforeVersion) ''
    }

    if ($after.Count -eq 1 -and $afterVersion -and $afterVersion -ne $beforeVersion) {
        Add-Check 'U9' 'Version in Programme und Features ersetzt' 'OK' "$beforeVersion -> $afterVersion" 'H19'
    } else {
        Add-Check 'U9' 'Version in Programme und Features ersetzt' 'FEHLER' ("vorher '{0}', nachher '{1}', {2} Einträge" -f $beforeVersion, $afterVersion, $after.Count) 'H19'
    }

    # auch unter fremdem Namen (alte Kennung, roher craft-Installer "ownCloud"); der Runner war vorher frei davon
    $all = @(Get-UninstallEntries $Installed 'owncloud')
    if ($all.Count -eq 1) {
        Add-Check 'U10' 'Keine Doppel-Einträge in Programme und Features' 'OK' $all[0].Key 'H19'
    } else {
        Add-Check 'U10' 'Keine Doppel-Einträge in Programme und Features' 'FEHLER' ('{0} Einträge: {1}' -f $all.Count, (($all | ForEach-Object { '{0} ({1})' -f $_.Key, (Get-EntryValue $_ 'DisplayName') }) -join '; ')) 'H19'
    }

    $exes = @(Get-ClientExes)
    if ($exes.Count -eq 1 -and $PreviousExe -and $exes[0] -eq $PreviousExe) {
        Add-Check 'U11' 'Genau eine Installation, im Ordner der Vorversion' 'OK' $exes[0] ''
    } else {
        Add-Check 'U11' 'Genau eine Installation, im Ordner der Vorversion' 'FEHLER' ("Vorversion: '{0}'; nachher: {1}" -f $PreviousExe, $(if ($exes.Count) { $exes -join '; ' } else { 'keine' })) ''
    }

    if (-not $Exe -or -not $PreviousExeHash) {
        Add-Check 'U12' 'Programmdatei durch das Update ersetzt' 'NICHT GEPRÜFT' 'Programmdatei vor oder nach dem Update nicht gefunden' ''
    } elseif ((Get-FileHash -LiteralPath $Exe -Algorithm SHA256).Hash -ne $PreviousExeHash) {
        Add-Check 'U12' 'Programmdatei durch das Update ersetzt' 'OK' '' ''
    } else {
        Add-Check 'U12' 'Programmdatei durch das Update ersetzt' 'FEHLER' 'unverändert die der Vorversion' ''
    }

    # der Wert der Probe muss bleiben; ein laufender Client darf eigene Werte ergänzt haben
    $config = Join-Path $trace.ConfigDir "$appName.cfg"
    if (-not $trace.ConfigWritten) {
        Add-Check 'U13' 'Einstellungen der Vorversion erhalten (owncloud.online.cfg)' 'NICHT GEPRÜFT' 'Spur nicht angelegt' 'E4'
    } elseif ((Test-Path -LiteralPath $config) -and (@(Get-Content -LiteralPath $config -ErrorAction SilentlyContinue) -contains 'probe=true')) {
        Add-Check 'U13' 'Einstellungen der Vorversion erhalten (owncloud.online.cfg)' 'OK' '' 'E4'
    } else {
        Add-Check 'U13' 'Einstellungen der Vorversion erhalten (owncloud.online.cfg)' 'FEHLER' 'Datei oder Wert probe=true fehlt' 'E4'
    }
}

# Version mit Buildnummer, wie der Client sie als clientVersion ablegt (Version::versionWithBuildNumber):
# die vier Zahlen der FILEVERSION seiner Programmdatei; leer, wenn nicht lesbar
function Get-VersionWithBuild {
    param([string] $Exe)
    if (-not $Exe -or -not (Test-Path -LiteralPath $Exe -PathType Leaf)) { return '' }
    $info = (Get-Item -LiteralPath $Exe).VersionInfo
    if ($info.FileMajorPart -lt 1) { return '' }
    return '{0}.{1}.{2}.{3}' -f $info.FileMajorPart, $info.FileMinorPart, $info.FileBuildPart, $info.FilePrivatePart
}

# upgrade-beta2: Konfiguration von beta2 mit dem Sync-Ordner dieses Rechners, der Version der Vorversion
# (clientVersion, ConfigFile::setClientVersionWithBuildNumberString) und dem VFS-Modus des Ordnerpaars
function Get-Beta2Config {
    param([string] $Version, [ValidateSet('off', 'wincfapi')] [string] $VfsMode = 'off')
    $syncDir = $trace.SyncDir -replace '\\', '/'
    # QSettings schreibt einen solchen Pfad unmaskiert; mit Sonderzeichen passte die Vorlage nicht mehr
    if ($syncDir -notmatch '^[A-Za-z]:/[A-Za-z0-9._/-]+$') { throw "Sync-Ordner '$syncDir' passt nicht in die Vorlage" }
    if (-not $Version) { $Version = '11.0.0.0' }
    return ($beta2ConfigTemplate -replace "`r`n", "`n").Replace('C:/PROBE_SYNCDIR', $syncDir).Replace('PROBE_CLIENTVERSION', $Version).Replace('PROBE_VFSMODE', $VfsMode)
}

# Rohwerte einer Qt-INI-Datei, Schlüssel "Abschnitt|Schlüssel"
function Read-IniValues {
    param([string] $Path)
    $values = @{}
    if (-not (Test-Path -LiteralPath $Path -PathType Leaf)) { return $values }
    $section = ''
    foreach ($line in [IO.File]::ReadAllLines($Path)) {
        if ($line -match '^\[(.+)\]$') { $section = $Matches[1]; continue }
        $separator = $line.IndexOf('=')
        if ($separator -gt 0) { $values[$section + '|' + $line.Substring(0, $separator)] = $line.Substring($separator + 1) }
    }
    return $values
}

# Abschnitte einer Qt-INI-Datei in der Reihenfolge der Datei, doppelte eingeschlossen
function Get-IniSections {
    param([string] $Path)
    if (-not (Test-Path -LiteralPath $Path -PathType Leaf)) { return @() }
    return @(foreach ($line in [IO.File]::ReadAllLines($Path)) { if ($line -match '^\[(.+)\]$') { $Matches[1] } })
}

# Datei lesen, die ein laufender Prozess noch schreibt
function Read-SharedText {
    param([string] $Path)
    if (-not (Test-Path -LiteralPath $Path -PathType Leaf)) { return '' }
    try {
        $stream = [IO.File]::Open($Path, [IO.FileMode]::Open, [IO.FileAccess]::Read, [IO.FileShare]::ReadWrite -bor [IO.FileShare]::Delete)
        try { return (New-Object IO.StreamReader($stream, [Text.Encoding]::UTF8)).ReadToEnd() } finally { $stream.Dispose() }
    } catch {
        return ''
    }
}

# Prüfling mit der vorbereiteten Konfiguration starten und wie von Hand über --quit beenden.
# Belege in OutDir: <Name>-start.log und die Konfiguration vorher/nachher (sie enthält keine
# Geheimnisse, das Refresh-Token liegt im Schlüsselbund).
function Start-CandidateWithConfig {
    param([string] $Exe, [string] $Name, [string] $Label, [string] $QuitLabel)
    $config = Join-Path $trace.ConfigDir "$appName.cfg"
    $before = Read-IniValues $config
    $result = [pscustomobject]@{
        Before = $before; After = @{}; Sections = @(); Log = ''
        EndedEarly = $false; Exited = $false; ExitCode = $null; QuitExitCode = $null
    }
    Copy-Item -LiteralPath $config -Destination (Join-Path $OutDir "$Name-konfiguration-vorher.cfg") -ErrorAction SilentlyContinue
    $log = Join-Path $OutDir "$Name-start.log"
    Write-Host "== $Label"
    Write-Annotation 'notice' "Start: $Label"
    $process = Start-Process -FilePath $Exe -ArgumentList @('--logfile', ('"{0}"' -f $log), '--logdebug', '--logflush') -PassThru
    $null = $process.Handle
    # bis die Ordner aus der Konfiguration eingerichtet sind, höchstens 3 min
    $deadline = (Get-Date).AddMinutes(3)
    while ((Get-Date) -lt $deadline -and -not $process.HasExited) {
        Start-Sleep -Seconds 2
        $text = Read-SharedText $log
        if ($text -match 'Setup folders from settings file' -and $text -match '(Adding folder to Folder Map|Unable to rebuild previous folder|Unable to create Folder for|validation failed for folder)') { break }
    }
    # Konto und Ordner arbeiten lassen: Schlüsselbund, Verbindungsversuch, Sync-Root, Speichern
    Start-Sleep -Seconds 10
    $result.EndedEarly = $process.HasExited
    $result.Exited = $result.EndedEarly
    if (-not $result.EndedEarly) {
        # nur an eine laufende Instanz: sonst würde dieser Aufruf selbst zum Client
        $quit = Invoke-Step $QuitLabel $Exe '--quit'
        $result.QuitExitCode = $quit.exitcode
        $result.Exited = $process.WaitForExit(90 * 1000)
        if (-not $result.Exited) { $null = & taskkill.exe /PID $process.Id /T /F }
    }
    if ($result.Exited) { $result.ExitCode = $process.ExitCode }
    $result.Log = Read-SharedText $log
    $result.After = Read-IniValues $config
    $result.Sections = @(Get-IniSections $config)
    Copy-Item -LiteralPath $config -Destination (Join-Path $OutDir "$Name-konfiguration-nachher.cfg") -ErrorAction SilentlyContinue
    return $result
}

# Ende über --quit: Prüfling und der Aufruf, der --quit überbringt, enden mit 0; leer = in Ordnung
function Get-CandidateExitProblem {
    param($Result)
    if ($Result.EndedEarly) { return ('vor --quit beendet, Exit-Code {0}' -f $Result.ExitCode) }
    if (-not $Result.Exited) { return 'nach --quit nicht in 90 s beendet, Prozess abgebrochen' }
    if ($Result.QuitExitCode -ne 0) {
        $code = if ($null -eq $Result.QuitExitCode) { 'Zeitüberschreitung' } else { "Exit-Code $($Result.QuitExitCode)" }
        return "der Aufruf mit --quit endete mit $code"
    }
    if ($Result.ExitCode -ne 0) { return ('Exit-Code {0} statt 0' -f $Result.ExitCode) }
    return ''
}

# Einrichten des Ordnerpaars laut Protokoll und Meldungen, nach denen der Client Konto oder Ordner verwirft
function Get-FolderSetupResult {
    param([string] $Text, [string] $FolderPath, [string] $LogName)
    $lines = @($Text -split "`r?`n")
    $added = @($lines | Where-Object { $_.Contains('Adding folder to Folder Map') -and $_.Contains($FolderPath) })
    $lossPattern = 'Unable to rebuild previous folder|will be removed from the configuration|are not supported by this client|Folder Definition validation failed|Unable to create Folder for|missing from Config|No URL for account|No user name provided|Could not load vfs plugin'
    $loss = @($lines | Where-Object { $_ -match $lossPattern })
    $result = [pscustomobject]@{ Status = 'FEHLER'; Detail = "kein 'Adding folder to Folder Map' für $FolderPath, siehe $LogName"; Loss = $loss }
    if (-not $Text) {
        $result.Detail = "kein Protokoll ($LogName fehlt oder ist leer)"
    } elseif ($loss.Count -gt 0) {
        $result.Detail = '{0} Meldungen, z. B. {1}' -f $loss.Count, $loss[0].Trim()
    } elseif ($added.Count -gt 0 -and $Text.Contains('Setup folders from settings file')) {
        $result.Status = 'OK'
        $result.Detail = $FolderPath
    }
    return $result
}

# Konto und Ordnerpaar der Vorlage; nach dem Start unverändert, auch QUuid, Capabilities und QUrl
# (@Variant, Byte für Byte wie von beta2)
function Get-Beta2Keys {
    $account = 'Accounts|0\'
    $folder = "Accounts|0\Folders\$beta2FolderId\"
    return @('url', 'dav_user', 'display-name', 'uuid', 'capabilities', 'default_sync_root' | ForEach-Object { $account + $_ }) +
        @('localPath', 'journalPath', 'targetPath', 'davUrl', 'displayName', 'paused', 'virtualFilesMode' | ForEach-Object { $folder + $_ })
}

# Werte, die nach dem Start fehlen oder sich verändert haben
function Get-ChangedValues {
    param([hashtable] $Before, [hashtable] $After, [string[]] $Keys)
    foreach ($key in $Keys) {
        if (-not $After.ContainsKey($key)) { "$key fehlt" }
        elseif ($After[$key] -cne $Before[$key]) { '{0}: {1} -> {2}' -f $key, $Before[$key], $After[$key] }
    }
}

# Einstellungen an der Wurzel, wie beta2 sie schreibt: die Werte der Vorlage unverändert,
# clientVersion auf der Version des Prüflings (checkClientVersion), genau ein Abschnitt [General]
function Get-Beta2SettingsProblems {
    param([hashtable] $Before, [hashtable] $After, [string[]] $Sections, [string] $ExpectedVersion)
    foreach ($key in 'General|probe', 'General|optionalDesktopNotifications', 'General|monoIcons') {
        if (-not $After.ContainsKey($key)) { "$key fehlt" }
        elseif ($After[$key] -cne $Before[$key]) { '{0}: {1} -> {2}' -f $key, $Before[$key], $After[$key] }
    }
    $version = $After['General|clientVersion']
    if (-not $version) { 'General|clientVersion fehlt' }
    elseif ($ExpectedVersion -and $version -cne $ExpectedVersion) { 'General|clientVersion: {0} statt {1} (Version des Prüflings)' -f $version, $ExpectedVersion }
    # ein zweiter Abschnitt hieße: der Client hat die Werte der Vorlage nicht als seine gelesen
    $general = @($Sections | Where-Object { $_ -eq 'General' -or $_ -eq '%General' })
    if ($general.Count -ne 1 -or $general[0] -cne 'General') {
        'Abschnitte für Einstellungen: {0} statt genau einem [General]' -f $(if ($general.Count) { ($general | ForEach-Object { "[$_]" }) -join ', ' } else { 'keiner' })
    }
}

# Sync-Roots des Clients laut SyncRootManager (Kennung "owncloud.online!<SID>!<Hash>", ältere
# "owncloud.online_<Hash>") mit Ordnern und der CLSID ihres Explorer-Eintrags (NamespaceCLSID)
function Get-OwnSyncRoots {
    $manager = $hklm.OpenSubKey('SOFTWARE\Microsoft\Windows\CurrentVersion\Explorer\SyncRootManager', $false)
    if ($null -eq $manager) { return }
    try {
        foreach ($id in $manager.GetSubKeyNames()) {
            if (-not ($id.StartsWith("$appName!", [StringComparison]::OrdinalIgnoreCase) -or $id.StartsWith("${appName}_", [StringComparison]::OrdinalIgnoreCase))) { continue }
            $paths = @()
            $userRoots = $manager.OpenSubKey("$id\UserSyncRoots", $false)
            if ($userRoots) {
                try { $paths = @($userRoots.GetValueNames() | ForEach-Object { [string]$userRoots.GetValue($_) }) } finally { $userRoots.Close() }
            }
            $namespace = ''
            $root = $manager.OpenSubKey($id, $false)
            if ($root) {
                try { $namespace = [string]$root.GetValue('NamespaceCLSID') } finally { $root.Close() }
            }
            # die CLSID landet als Teil eines Suchmusters im Pfad; nur eine echte CLSID taugt dafür
            if ($namespace -notmatch '^\{[0-9A-Fa-f]{8}-[0-9A-Fa-f]{4}-[0-9A-Fa-f]{4}-[0-9A-Fa-f]{4}-[0-9A-Fa-f]{12}\}$') { $namespace = '' }
            [pscustomobject]@{ Id = $id; Paths = $paths; Namespace = $namespace }
        }
    } finally {
        $manager.Close()
    }
}

# upgrade-beta2: Der Prüfling startet mit der Konfiguration von beta2 und wird wie von Hand über
# --quit beendet. Er muss das Ordnerpaar lesen (Protokoll) und Konto, Ordnerpaar und Einstellungen
# beim Beenden ohne Verlust zurückschreiben (FolderMan::shutdown, AccountManager).
function Test-Beta2ConfigAfterStart {
    param([string] $Exe)
    $titles = [ordered]@{
        U14 = 'Prüfling startet mit der Konfiguration von beta2 und endet über --quit'
        U15 = 'Ordnerpaar von beta2 vom Prüfling gelesen (Protokoll)'
        U16 = 'Konto und Ordnerpaar nach dem Start ohne Verlust (owncloud.online.cfg)'
        U17 = 'Einstellungen und Anmeldedaten nach dem Start erhalten'
    }
    if (-not $Exe -or -not $trace.ConfigWritten) {
        foreach ($id in $titles.Keys) { Add-Check $id $titles[$id] 'NICHT GEPRÜFT' 'Programmdatei oder Konfiguration fehlt' '' }
        return
    }
    $run = Start-CandidateWithConfig $Exe 'pruefling' 'Prüfling mit der Konfiguration von beta2 starten' 'Prüfling beenden (--quit)'

    $problem = Get-CandidateExitProblem $run
    if ($problem) {
        Add-Check 'U14' $titles.U14 'FEHLER' "$problem, siehe pruefling-start.log" ''
    } else {
        Add-Check 'U14' $titles.U14 'OK' 'Exit-Code 0' ''
    }

    $setup = Get-FolderSetupResult $run.Log (($trace.SyncDir -replace '\\', '/') + '/') 'pruefling-start.log'
    Add-Check 'U15' $titles.U15 $setup.Status $setup.Detail ''
    Add-List 'U15 Meldungen beim Einrichten der Ordner' $setup.Loss

    $keys = @(Get-Beta2Keys)
    $problems = @(Get-ChangedValues $run.Before $run.After $keys)
    if ($problems.Count -eq 0) {
        Add-Check 'U16' $titles.U16 'OK' ('{0} Werte unverändert' -f $keys.Count) ''
    } else {
        Add-Check 'U16' $titles.U16 'FEHLER' ('{0} Werte verloren oder verändert, z. B. {1}' -f $problems.Count, $problems[0]) ''
        Add-List 'U16 Konto und Ordnerpaar nach dem Start' $problems
    }

    # Einstellungen an der Wurzel, Liste der Anmeldedaten und der Eintrag im Schlüsselbund
    if (-not $run.Before.ContainsKey('General|monoIcons')) {
        Add-Check 'U17' $titles.U17 'NICHT GEPRÜFT' 'die Vorlage enthält keine Einstellungen an der Wurzel ([General])' 'E4'
        return
    }
    $lost = New-Object 'System.Collections.Generic.List[string]'
    foreach ($entry in @(Get-Beta2SettingsProblems $run.Before $run.After $run.Sections (Get-VersionWithBuild $Exe))) { $lost.Add($entry) }
    if (-not $run.After.ContainsKey("Credentials|owncloud.online_credentials%3Aprobe.invalid%3A$beta2AccountId\http\oauthtoken")) { $lost.Add('Liste der Anmeldedaten ohne das Konto') }
    $listed = (& cmdkey.exe "/list:$($trace.Credential)" | Out-String)
    if (-not $listed.Contains($trace.Credential)) { $lost.Add('Anmeldedaten im Schlüsselbund fehlen') }
    if ($lost.Count -eq 0) {
        Add-Check 'U17' $titles.U17 'OK' ('optionalDesktopNotifications={0}, monoIcons={1}, clientVersion={2}' -f $run.After['General|optionalDesktopNotifications'], $run.After['General|monoIcons'], $run.After['General|clientVersion']) 'E4'
    } else {
        Add-Check 'U17' $titles.U17 'FEHLER' ($lost -join '; ') 'E4'
    }
}

# upgrade-beta2 mit Platzhalter-Ordner: Unter Windows legte beta2 neue Ordnerpaare als
# Platzhalter-Ordner an (Vorgabe im Assistenten, sobald das CfApi-Plug-in lädt). Der Prüfling
# startet ein zweites Mal mit der Konfiguration von beta2, nun mit virtualFilesMode=wincfapi.
# Scheitert addFolder(), entfernt der Client die Ordnerdefinition aus der Konfiguration
# (FolderMan::addFoldersFromConfigByAccount) - etwa wenn das Plug-in nicht lädt. Ein Fehler beim
# Anmelden des Sync-Roots kommt später (Folder::startVfs) und lässt die Definition stehen.
function Test-Beta2VfsAfterStart {
    param([string] $Exe)
    $titles = [ordered]@{
        U18 = 'Prüfling startet mit dem Platzhalter-Ordner von beta2 (wincfapi) und endet über --quit'
        U19 = 'Platzhalter-Ordnerpaar von beta2 vom Prüfling gelesen (Protokoll)'
        U20 = 'Ordnerdefinition des Platzhalter-Ordners nach dem Start erhalten (owncloud.online.cfg)'
        U21 = 'Platzhalter (CfApi) auf diesem Rechner'
    }
    $config = Join-Path $trace.ConfigDir "$appName.cfg"
    $why = ''
    if (-not $Exe -or -not $trace.ConfigWritten) {
        $why = 'Programmdatei oder Konfiguration fehlt'
    } else {
        try {
            [IO.File]::WriteAllText($config, (Get-Beta2Config $trace.PreviousVersion -VfsMode 'wincfapi'))
        } catch {
            $why = 'Konfiguration nicht schreibbar: ' + $_.Exception.Message
        }
    }
    if ($why) {
        foreach ($id in $titles.Keys) { Add-Check $id $titles[$id] 'NICHT GEPRÜFT' $why '' }
        return
    }
    $run = Start-CandidateWithConfig $Exe 'pruefling-vfs' 'Prüfling mit dem Platzhalter-Ordner von beta2 starten' 'Prüfling beenden (--quit, Platzhalter-Ordner)'

    $problem = Get-CandidateExitProblem $run
    if ($problem) {
        Add-Check 'U18' $titles.U18 'FEHLER' "$problem, siehe pruefling-vfs-start.log" ''
    } else {
        Add-Check 'U18' $titles.U18 'OK' 'Exit-Code 0' ''
    }

    $setup = Get-FolderSetupResult $run.Log (($trace.SyncDir -replace '\\', '/') + '/') 'pruefling-vfs-start.log'
    Add-Check 'U19' $titles.U19 $setup.Status $setup.Detail ''
    Add-List 'U19 Meldungen beim Einrichten des Platzhalter-Ordners' $setup.Loss

    $keys = @(Get-Beta2Keys)
    $problems = @(Get-ChangedValues $run.Before $run.After $keys)
    if ($run.Before["Accounts|0\Folders\$beta2FolderId\virtualFilesMode"] -cne 'wincfapi') {
        Add-Check 'U20' $titles.U20 'NICHT GEPRÜFT' 'die Konfiguration vor dem Start hat kein Ordnerpaar mit virtualFilesMode=wincfapi' ''
    } elseif ($problems.Count -eq 0) {
        Add-Check 'U20' $titles.U20 'OK' ('{0} Werte unverändert, virtualFilesMode=wincfapi' -f $keys.Count) ''
    } else {
        Add-Check 'U20' $titles.U20 'FEHLER' ('{0} Werte verloren oder verändert, z. B. {1}; Ursache siehe U19/U21' -f $problems.Count, $problems[0]) ''
        Add-List 'U20 Platzhalter-Ordner nach dem Start' $problems
    }

    # was der Runner kann: Plug-in geladen, Sync-Root angemeldet (bleibt nach --quit angemeldet)
    $lines = @($run.Log -split "`r?`n")
    $plugin = @($lines | Where-Object { $_ -match 'Created VFS instance from plugin .*vfs_wincfapi|Could not load (vfs )?plugin|Plugin failed to load|Plugin doesn.t exist|Plugin has wrong' })
    $cfapi = @($lines | Where-Object { $_ -match 'Error registering StorageProvider|Could not find StorageFolder|Could not retrieve StorageFolder|managed by another sync client|used by a different client|Downgrading the folder|Virtual filesystem feature' })
    $roots = @(Get-OwnSyncRoots)
    $trace.SyncRoots = $roots
    if ($plugin.Count -eq 0) {
        $note = 'Plug-in: keine Meldung im Protokoll'
    } elseif ($plugin[0] -match 'Created VFS instance') {
        $note = 'Plug-in wincfapi geladen'
    } else {
        $note = 'Plug-in nicht geladen: ' + $plugin[0].Trim()
    }
    if ($roots.Count -gt 0) {
        $note += '; Sync-Root angemeldet: ' + (($roots | ForEach-Object { '{0} -> {1}, Explorer-Eintrag {2}' -f $_.Id, ($_.Paths -join ', '), $(if ($_.Namespace) { $_.Namespace } else { '-' }) }) -join '; ')
    } elseif ($cfapi.Count -gt 0) {
        $note += '; kein Sync-Root angemeldet: ' + $cfapi[0].Trim()
    } else {
        $note += '; kein Sync-Root angemeldet, keine Ursache im Protokoll'
    }
    Add-Check 'U21' $titles.U21 'INFO' $note ''
    Add-List 'U21 Meldungen zu Plug-in und Sync-Root' ($plugin + $cfapi)
}

# --- Prüfungen --------------------------------------------------------------

function Test-Installation {
    param($Base, $Installed, [string] $Exe, [string] $DiffName)

    # I2: Programme und Features
    $entries = @(Get-UninstallEntries $Installed $ownPattern)
    $problems = @()
    if ($entries.Count -eq 0) { $problems += 'kein Eintrag mit owncloud.online' }
    if ($entries.Count -gt 1) { $problems += ('{0} Einträge statt einem: {1}' -f $entries.Count, (($entries | ForEach-Object { $_.Key }) -join '; ')) }
    foreach ($entry in $entries) {
        foreach ($name in 'DisplayName', 'Publisher') {
            $value = Get-EntryValue $entry $name
            if ($value -cmatch 'ownCloud') { $problems += "$name trägt die fremde Marke: $value" }
        }
        $command = Get-EntryValue $entry 'UninstallString'
        if (-not $command) {
            $problems += 'UninstallString fehlt'
        } else {
            $file = @(Split-CommandLine $command)[0]
            # ohne Pfad (z. B. MsiExec.exe) über PATH auflösen
            $found = if ([IO.Path]::IsPathRooted($file)) { Test-Path -LiteralPath $file } else { [bool](Get-Command $file -ErrorAction SilentlyContinue) }
            if (-not $found) { $problems += "Deinstallationsprogramm fehlt: $file" }
        }
        $version = Get-EntryValue $entry 'DisplayVersion'
        if ($ExpectedVersion -and $version -notmatch ('^' + [regex]::Escape($ExpectedVersion) + '(\D|$)')) {
            $problems += "DisplayVersion '$version' statt $ExpectedVersion"
        }
    }
    if ($problems.Count -gt 0) {
        Add-Check 'I2' 'Genau ein Eintrag in Programme und Features (eigene Marke, Version)' 'FEHLER' ($problems -join '; ') 'H19'
    } else {
        $first = $entries[0]
        Add-Check 'I2' 'Genau ein Eintrag in Programme und Features (eigene Marke, Version)' 'OK' ('{0} | {1} | {2}' -f (Get-EntryValue $first 'DisplayName'), (Get-EntryValue $first 'Publisher'), (Get-EntryValue $first 'DisplayVersion')) 'H19'
    }
    foreach ($entry in $entries) {
        $about = Get-EntryValue $entry 'URLInfoAbout'
        if ($about -and $about -notmatch '^https://owncloud\.online') {
            Add-Check 'I8' 'Herausgeber-Link zeigt auf owncloud.online' 'WARNUNG' $about 'M9'
        }
    }

    # I3: Programmdatei
    if ($Exe) {
        Add-Check 'I3' 'Programmdatei installiert' 'OK' $Exe ''
    } else {
        Add-Check 'I3' 'Programmdatei installiert' 'FEHLER' "$appName.exe unter $programFiles nicht gefunden" ''
    }

    # I4: Verknüpfungen
    $shortcuts = Get-Items $Installed 'files.shortcuts'
    $own = @(foreach ($key in $shortcuts.Keys) {
            if ($key -like '*.lnk' -and ($key -match $ownPattern -or $shortcuts[$key] -match $ownPattern)) { $key }
        })
    $broken = @($own | Where-Object { $shortcuts[$_] -match 'Ziel vorhanden: nein' })
    $startMenu = @($own | Where-Object { $_ -match '\\Start Menu\\Programs\\' -and $shortcuts[$_] -match 'Ziel vorhanden: ja' })
    if ($broken.Count -gt 0) {
        Add-Check 'I4' 'Startmenü-Verknüpfung funktioniert' 'FEHLER' ('Ziel fehlt: ' + (($broken | ForEach-Object { '{0} ({1})' -f $_, $shortcuts[$_] }) -join '; ')) 'H19'
    } elseif ($startMenu.Count -eq 0) {
        Add-Check 'I4' 'Startmenü-Verknüpfung funktioniert' 'FEHLER' 'keine Verknüpfung im Startmenü' 'H19'
    } else {
        Add-Check 'I4' 'Startmenü-Verknüpfung funktioniert' 'OK' ($startMenu -join '; ') 'H19'
    }

    # I5: Explorer-Integration (Overlays, Kontextmenü) aus dem Installationsordner
    if (-not $Exe) {
        Add-Check 'I5' 'Explorer-Integration registriert' 'NICHT GEPRÜFT' 'ohne Programmdatei kein Installationsordner' 'H3'
    } else {
        $installRoot = Split-Path -Parent $Exe
        if ((Split-Path -Leaf $installRoot) -eq 'bin') { $installRoot = Split-Path -Parent $installRoot }
        $shell = Get-Items $Installed 'reg.shellext'
        $servers = @{}
        foreach ($key in $shell.Keys) {
            if ($key -notmatch 'WOW6432Node' -and $key -match '\\CLSID\\(\{[0-9A-Fa-f-]{36}\})\\InprocServer32 :: \(Standard\)$') {
                $clsid = $Matches[1]
                $servers[$clsid] = [Environment]::ExpandEnvironmentVariables((Get-Data $shell[$key]).Trim().Trim('"'))
            }
        }
        $overlayRoot = 'HKLM\SOFTWARE\Microsoft\Windows\CurrentVersion\Explorer\ShellIconOverlayIdentifiers\'
        $allOverlays = New-Object 'System.Collections.Generic.List[string]'
        $ownOverlays = New-Object 'System.Collections.Generic.List[string]'
        $menus = New-Object 'System.Collections.Generic.List[string]'
        foreach ($key in $shell.Keys) {
            if ($key.StartsWith($overlayRoot, [StringComparison]::OrdinalIgnoreCase)) {
                $rest = $key.Substring($overlayRoot.Length)
                if ($rest -notmatch '[\\]| :: ') { $allOverlays.Add($rest) }
                if ($rest -match '^([^\\]+) :: \(Standard\)$') {
                    $name = $Matches[1]
                    $clsid = (Get-Data $shell[$key]).Trim()
                    if ($servers.ContainsKey($clsid) -and (Test-UnderPath $servers[$clsid] @($installRoot))) { $ownOverlays.Add($name) }
                }
            }
            if ($key -match '\\shellex\\ContextMenuHandlers\\([^\\]+?)( :: \(Standard\))?$') {
                $name = $Matches[1]
                $clsid = if ($Matches[2]) { (Get-Data $shell[$key]).Trim() } else { $name }
                if ($servers.ContainsKey($clsid) -and (Test-UnderPath $servers[$clsid] @($installRoot)) -and -not $menus.Contains($name)) { $menus.Add($name) }
            }
        }
        if ($ownOverlays.Count -gt 0 -and $menus.Count -gt 0) {
            Add-Check 'I5' 'Explorer-Integration registriert' 'OK' ('{0} Overlay-Handler, Kontextmenü {1}' -f $ownOverlays.Count, ($menus -join ', ')) 'H3'
        } else {
            Add-Check 'I5' 'Explorer-Integration registriert' 'FEHLER' ('Overlay-Handler: {0}, Kontextmenü-Handler: {1} (aus {2})' -f $ownOverlays.Count, $menus.Count, $installRoot) 'H3'
        }
        # Explorer lädt nur die ersten 15 Overlay-Handler in Namensreihenfolge
        $sorted = $allOverlays.ToArray()
        [Array]::Sort($sorted, [StringComparer]::OrdinalIgnoreCase)
        $late = @(foreach ($name in $ownOverlays) { $rank = [Array]::IndexOf($sorted, $name) + 1; if ($rank -gt 15) { "$($name.Trim()) (Platz $rank)" } })
        if ($late.Count -gt 0) {
            Add-Check 'I5b' 'Overlay-Handler unter den ersten 15' 'WARNUNG' ($late -join '; ') 'H3'
        } elseif ($ownOverlays.Count -gt 0) {
            Add-Check 'I5b' 'Overlay-Handler unter den ersten 15' 'OK' ('{0} Handler insgesamt registriert' -f $sorted.Count) 'H3'
        }
    }

    # I6: das Setup darf nichts Fremdes zum Löschen beim Neustart vormerken
    $diffs = @(Get-Diff $Base $Installed $DiffName)
    $foreignPending = @(foreach ($diff in $diffs) {
            if ($diff.Category -eq 'reboot' -and $diff.Change -eq 'neu' -and $diff.Key -match '^(Löschen|Umbenennen) beim Neustart: ' -and $diff.Key -notmatch $ownPattern) { $diff.Key }
        })
    if ($foreignPending.Count -gt 0) {
        Add-Check 'I6' 'Keine fremden Dateien zum Löschen beim Neustart vorgemerkt' 'FEHLER' ('{0} Einträge, z. B. {1}' -f $foreignPending.Count, $foreignPending[0]) 'H2'
        Add-List 'Fremde Neustart-Aktionen nach der Installation' $foreignPending
    } else {
        Add-Check 'I6' 'Keine fremden Dateien zum Löschen beim Neustart vorgemerkt' 'OK' '' 'H2'
    }

    # I7: Signaturen (Information; CI-Bauten ohne sign=true sind unsigniert)
    $notes = @('Setup ' + (Get-SignatureNote $Setup))
    if ($Exe) { $notes += "$appName.exe " + (Get-SignatureNote $Exe) }
    Add-Check 'I7' 'Signaturen' 'INFO' ($notes -join '; ') ''
}

function Test-Residue {
    param($Base, $Final, [string] $DiffName, [hashtable] $Ignore)
    $diffs = @(Get-Diff $Base $Final $DiffName)
    $tracePattern = Get-TracePattern
    $pending = @(foreach ($key in (Get-Items $Final 'reboot').Keys) { if ($key -match '^Löschen beim Neustart: (.+)$') { $Matches[1] } })
    $temp = [IO.Path]::GetTempPath()
    $errors = New-Object 'System.Collections.Generic.List[string]'
    $warnings = New-Object 'System.Collections.Generic.List[string]'
    $foreign = New-Object 'System.Collections.Generic.List[string]'
    foreach ($diff in $diffs) {
        if ($diff.Key -match $tracePattern) { continue }
        if ($Ignore.ContainsKey("$($diff.Category)|$($diff.Key)")) { continue }
        $line = Format-Diff $diff
        $text = '{0} {1} {2}' -f $diff.Key, $diff.Before, $diff.After
        # Neben dem offiziellen Client lässt sich "ownCloud" oder eine gemeinsame
        # COM-Kennung nicht eindeutig zuordnen (dessen Laufzeitspuren entstehen
        # auch nach dem Fußabdruck). Dort zählt nur owncloud.online; Rückstände
        # der gemeinsamen Kennungen deckt das Szenario frisch ab.
        $own = $text -match $ownPattern
        if (-not $own -and $Scenario -ne 'koexistenz' -and $text -match $clientPattern) { $own = $true }
        if (-not $own) { $foreign.Add($line); continue }
        if ($diff.Category -eq 'reboot') { $warnings.Add("$line   [Neustart-Aktion]"); continue }
        if ($diff.Change -ne 'entfernt') {
            $path = ($diff.Key -split ' :: ', 2)[0]
            if (Test-UnderPath $path $pending) { $warnings.Add("$line   [wird beim Neustart gelöscht]"); continue }
            if (Test-UnderPath $path @($temp)) { $warnings.Add("$line   [Temp-Ordner]"); continue }
        }
        $errors.Add($line)
    }
    $title = 'Keine Rückstände nach der Deinstallation'
    if ($errors.Count -gt 0) {
        Add-Check 'R1' $title 'FEHLER' ('{0} Einträge, z. B. {1}' -f $errors.Count, (@($errors | Select-Object -First 3) -join ' ; ')) 'H4, H6'
    } elseif ($warnings.Count -gt 0) {
        Add-Check 'R1' $title 'WARNUNG' ('{0} Einträge nur bis zum nächsten Neustart oder im Temp-Ordner' -f $warnings.Count) 'H7'
    } else {
        Add-Check 'R1' $title 'OK' '' 'H4, H6'
    }
    Add-List 'Rückstände (Fehler)' $errors.ToArray()
    Add-List 'Rückstände bis zum Neustart oder im Temp-Ordner (Warnungen)' $warnings.ToArray()
    if ($foreign.Count -gt 0) {
        Add-Check 'R2' 'Nicht zuzuordnende Änderungen' 'INFO' ('{0} Einträge (Runner bzw. offizieller Client zur Laufzeit), siehe bericht.md' -f $foreign.Count) ''
        Add-List 'Nicht zuzuordnende Änderungen' $foreign.ToArray()
    }
}

function Test-Removal {
    param($Base, $Final, [string] $DiffName, [hashtable] $Ignore, [bool] $Purge)
    $entries = @(Get-UninstallEntries $Final $ownPattern)
    if ($entries.Count -gt 0) {
        Add-Check 'D2' 'Eintrag in Programme und Features entfernt' 'FEHLER' (($entries | ForEach-Object { $_.Key }) -join '; ') ''
    } else {
        Add-Check 'D2' 'Eintrag in Programme und Features entfernt' 'OK' '' ''
    }
    Test-TracesAfterUninstall $Final $Purge
    Test-Residue $Base $Final $DiffName $Ignore
}

# Einträge, die zwischen zwei Schnappschüssen dazukamen oder sich änderten und
# einem der Clients zuzuordnen sind: "Kategorie|Schlüssel" -> Wert
function Get-Footprint {
    param($Before, $After, [string] $DiffName)
    $footprint = @{}
    foreach ($diff in @(Get-Diff $Before $After $DiffName $footprintSkip)) {
        if ($diff.Change -eq 'entfernt') { continue }
        if (('{0} {1}' -f $diff.Key, $diff.After) -notmatch $clientPattern) { continue }
        $footprint["$($diff.Category)|$($diff.Key)"] = $diff.After
    }
    return $footprint
}

function Test-Footprint {
    param([hashtable] $Footprint, $Snapshot, [string] $Id, [string] $Title, [string] $Audit)
    if ($Footprint.Count -eq 0) {
        Add-Check $Id $Title 'NICHT GEPRÜFT' 'kein Fußabdruck erfasst' $Audit
        return
    }
    $problems = New-Object 'System.Collections.Generic.List[string]'
    $files = New-Object 'System.Collections.Generic.HashSet[string]' ([StringComparer]::OrdinalIgnoreCase)
    foreach ($entry in $Footprint.GetEnumerator()) {
        $category, $key = $entry.Key -split '\|', 2
        if ($category -like 'files.*') { [void]$files.Add($key) }
        $items = Get-Items $Snapshot $category
        if (-not $items.ContainsKey($key)) {
            $problems.Add(('- [{0}] {1} = {2}' -f $category, $key, $entry.Value))
        } elseif (-not [string]::Equals($items[$key], [string]$entry.Value, [StringComparison]::Ordinal)) {
            $problems.Add(('~ [{0}] {1}: {2} -> {3}' -f $category, $key, $entry.Value, $items[$key]))
        }
    }
    foreach ($key in (Get-Items $Snapshot 'reboot').Keys) {
        if ($key -match '^(Löschen|Umbenennen) beim Neustart: (.+)$' -and $files.Contains($Matches[2])) {
            $problems.Add("! [reboot] $key")
        }
    }
    $sorted = @($problems | Sort-Object)
    if ($sorted.Count -gt 0) {
        Add-Check $Id $Title 'FEHLER' ('{0} von {1} Einträgen betroffen, z. B. {2}' -f $sorted.Count, $Footprint.Count, (@($sorted | Select-Object -First 2) -join ' ; ')) $Audit
        Add-List "$Id $Title" $sorted
    } else {
        Add-Check $Id $Title 'OK' ('{0} Einträge unverändert' -f $Footprint.Count) $Audit
    }
}

# --- Richtlinien (deinstall-policies) ---------------------------------------

# Teilbaum unter HKLM\$Path als "Schlüssel" bzw. "Schlüssel :: Wert" -> Inhalt
function Get-RegistryTree {
    param([string] $Path)
    $items = New-Object 'System.Collections.Generic.SortedDictionary[string,string]' ([StringComparer]::OrdinalIgnoreCase)
    Add-RegistryTreeItems $items $Path
    # sonst zählte PowerShell das Wörterbuch Eintrag für Eintrag auf
    return , $items
}

function Add-RegistryTreeItems {
    param($Items, [string] $Path)
    $key = $hklm.OpenSubKey($Path, $false)
    if ($null -eq $key) { return }
    try {
        $Items["HKLM\$Path"] = '(Schlüssel)'
        foreach ($name in $key.GetValueNames()) {
            $shown = if ($name) { $name } else { '(Standard)' }
            $Items["HKLM\$Path :: $shown"] = '{0}: {1}' -f $key.GetValueKind($name), (@($key.GetValue($name)) -join ' | ')
        }
        foreach ($sub in $key.GetSubKeyNames()) { Add-RegistryTreeItems $Items "$Path\$sub" }
    } finally {
        $key.Close()
    }
}

# Ausgangslage eines Durchgangs:
#   leer     der Schlüssel ohne Inhalt, wie beta1/beta2 ihn anlegten (samt Elternschlüssel)
#   wert     eine Richtlinie als Wert direkt im Schlüssel
#   setup    eine Richtlinie, wie AppConfig sie liest (Setup\ServerUrl, Unterschlüssel ohne Wert im Schlüssel)
#   nachbar  der leere Schlüssel neben der Richtlinie eines anderen Produkts unter BW-Tech GmbH
function Set-PolicyLayout {
    param([string] $Layout)
    $string = [Microsoft.Win32.RegistryValueKind]::String
    $key = $hklm.CreateSubKey($policyKeyPath)
    try {
        if ($Layout -eq 'wert') { $key.SetValue('InstallProbe', 'Richtlinie der Install-Probe', $string) }
        if ($Layout -eq 'setup') {
            $setup = $key.CreateSubKey('Setup')
            try { $setup.SetValue('ServerUrl', 'https://probe.invalid', $string) } finally { $setup.Close() }
        }
    } finally {
        $key.Close()
    }
    if ($Layout -eq 'nachbar') {
        $neighbor = $hklm.CreateSubKey("$policyVendorPath\Probe-Nachbar")
        try { $neighbor.SetValue('InstallProbe', 'fremde Richtlinie der Install-Probe', $string) } finally { $neighbor.Close() }
    }
}

# Erwarteter Teilbaum nach der Deinstallation: leere Schlüssel des Clients weg (der Elternschlüssel,
# wenn er dadurch leer wird), jede Richtlinie unverändert
function Get-ExpectedPolicyTree {
    param($Before, [string] $Layout)
    $expected = New-Object 'System.Collections.Generic.SortedDictionary[string,string]' ([StringComparer]::OrdinalIgnoreCase)
    if ($Layout -ne 'leer') {
        foreach ($entry in $Before.GetEnumerator()) {
            if ($Layout -eq 'nachbar' -and $entry.Key -eq "HKLM\$policyKeyPath") { continue }
            $expected[$entry.Key] = $entry.Value
        }
    }
    return , $expected
}

# Unterschiede zwischen erwartetem und tatsächlichem Teilbaum
function Compare-RegistryTree {
    param($Expected, $Actual)
    foreach ($entry in $Expected.GetEnumerator()) {
        if (-not $Actual.ContainsKey($entry.Key)) { 'fehlt: {0} = {1}' -f $entry.Key, $entry.Value }
        elseif ($Actual[$entry.Key] -cne $entry.Value) { 'verändert: {0} = {1} -> {2}' -f $entry.Key, $entry.Value, $Actual[$entry.Key] }
    }
    foreach ($entry in $Actual.GetEnumerator()) {
        if (-not $Expected.ContainsKey($entry.Key)) { 'noch da: {0} = {1}' -f $entry.Key, $entry.Value }
    }
}

# --- offizieller Client -----------------------------------------------------

function Get-OfficialProcess {
    return [bool](Get-Process -Name 'owncloud' -ErrorAction SilentlyContinue)
}

function Start-OfficialClient {
    $root = Join-Path $programFiles 'ownCloud'
    $exe = $null
    if (Test-Path -LiteralPath $root) {
        $exe = Get-ChildItem -LiteralPath $root -Recurse -File -Filter 'owncloud.exe' -ErrorAction SilentlyContinue | Select-Object -First 1
    }
    if (-not $exe) {
        Write-Host '   owncloud.exe des offiziellen Clients nicht gefunden'
        return $false
    }
    Write-Host "== Offiziellen Client starten: $($exe.FullName)"
    Write-Annotation 'notice' 'Start: offizieller Client'
    Start-Process -FilePath $exe.FullName | Out-Null
    for ($i = 0; $i -lt 20 -and -not (Get-OfficialProcess); $i++) { Start-Sleep -Seconds 1 }
    # dem Erststart Zeit geben, Einstellungen und Autostart zu schreiben,
    # bevor der Fußabdruck entsteht
    Start-Sleep -Seconds 15
    return (Get-OfficialProcess)
}

# Registry-Orte, an denen das MSI per AppSearch/RegLocator sucht und deren Wert
# auf unseren Client zeigt ("Eigenschaft <- Ort = Wert"). Liest nur die
# MSI-Datenbank, installiert nichts.
function Get-MsiLegacyHits {
    param([string] $MsiPath)
    $installer = New-Object -ComObject WindowsInstaller.Installer
    $database = $installer.GetType().InvokeMember('OpenDatabase', 'InvokeMethod', $null, $installer, @([string]$MsiPath, [int]0))
    $query = 'SELECT `AppSearch`.`Property`, `RegLocator`.`Root`, `RegLocator`.`Key`, `RegLocator`.`Name` FROM `AppSearch`, `RegLocator` WHERE `AppSearch`.`Signature_` = `RegLocator`.`Signature_`'
    $view = $database.GetType().InvokeMember('OpenView', 'InvokeMethod', $null, $database, @([string]$query))
    [void]$view.GetType().InvokeMember('Execute', 'InvokeMethod', $null, $view, $null)
    $hives = @([Microsoft.Win32.RegistryHive]::ClassesRoot, [Microsoft.Win32.RegistryHive]::CurrentUser,
        [Microsoft.Win32.RegistryHive]::LocalMachine, [Microsoft.Win32.RegistryHive]::Users)
    $names = @('HKCR', 'HKCU', 'HKLM', 'HKU')
    while ($record = $view.GetType().InvokeMember('Fetch', 'InvokeMethod', $null, $view, $null)) {
        $fields = @(foreach ($index in 1..4) { [string]$record.GetType().InvokeMember('StringData', 'GetProperty', $null, $record, $index) })
        $root = [int]$fields[1]
        if ($root -lt 0 -or $root -gt 3) { continue }
        # je nach Locator-Typ liest msiexec die 32- oder 64-Bit-Sicht - beide prüfen
        foreach ($registryView in [Microsoft.Win32.RegistryView]::Registry64, [Microsoft.Win32.RegistryView]::Registry32) {
            $key = [Microsoft.Win32.RegistryKey]::OpenBaseKey($hives[$root], $registryView).OpenSubKey($fields[2], $false)
            if ($null -eq $key) { continue }
            try { $value = [string]$key.GetValue($fields[3]) } finally { $key.Close() }
            if ($value -match $ownPattern) {
                $shownName = if ($fields[3]) { $fields[3] } else { '(Standard)' }
                '{0} <- {1}\{2} :: {3} = {4}' -f $fields[0], $names[$root], $fields[2], $shownName, $value
                break
            }
        }
    }
}

function Test-OfficialProcess {
    param([bool] $WasRunning, [string] $Id, [string] $Title)
    if (-not $WasRunning) {
        Add-Check $Id $Title 'NICHT GEPRÜFT' 'offizieller Client lief vorher nicht (auf dem Runner nicht startbar)' 'H2'
    } elseif (Get-OfficialProcess) {
        Add-Check $Id $Title 'OK' 'läuft weiter' 'H2'
    } else {
        Add-Check $Id $Title 'FEHLER' 'Prozess owncloud.exe wurde beendet' 'H2'
    }
}

# ---------------------------------------------------------------------------
# Szenarien
# ---------------------------------------------------------------------------

function Assert-Fresh {
    param($Base)
    $existing = @(Get-UninstallEntries $Base 'owncloud')
    if ($existing.Count -gt 0) {
        throw ('kein frischer Rechner, schon installiert: {0}' -f (($existing | ForEach-Object { $_.Key }) -join '; '))
    }
}

function Invoke-Fresh {
    $base = New-Snapshot 'ausgang' 'Ausgangszustand'
    Assert-Fresh $base
    Install-Candidate 'I1'
    $exe = Find-ClientExe
    Add-ClientTraces $exe
    $installed = New-Snapshot 'installiert' 'Prüfling installiert, Client-Spuren angelegt'
    Test-Installation $base $installed $exe 'ausgang-installiert'
    Uninstall-Candidate 'D1' -Purge
    $removed = New-Snapshot 'deinstalliert' 'Prüfling deinstalliert'
    Test-Removal $base $removed 'ausgang-deinstalliert' @{} $true
}

function Invoke-Upgrade {
    $base = New-Snapshot 'ausgang' 'Ausgangszustand'
    Assert-Fresh $base
    $run = Invoke-Step 'Setup (Vorversion)' $PreviousSetup '/S /norestart'
    Test-Run $run 'U1' 'Setup (Vorversion) still, ohne Neustart'
    if ($run.zeitueberschreitung -or ($run.exitcode -ne 0 -and $run.exitcode -ne 3010)) {
        throw 'Vorversion ließ sich nicht installieren - Update nicht prüfbar'
    }
    $previousExe = Find-ClientExe
    $previousExeHash = if ($previousExe) { (Get-FileHash -LiteralPath $previousExe -Algorithm SHA256).Hash } else { '' }
    Add-ClientTraces $previousExe
    $previous = New-Snapshot 'vorversion' 'Vorversion installiert, Client-Spuren angelegt'
    Install-Candidate 'I1'
    $exe = Find-ClientExe
    $installed = New-Snapshot 'installiert' 'Prüfling über die Vorversion installiert'
    Test-Installation $previous $installed $exe 'vorversion-installiert'
    Test-TracesAfterUpgrade $installed
    if ($Scenario -eq 'upgrade-beta1') { Test-UpgradeFromBeta $previous $installed $previousExe $previousExeHash $exe '11.0.0-beta1' }
    if ($Scenario -eq 'upgrade-beta2') {
        Test-UpgradeFromBeta $previous $installed $previousExe $previousExeHash $exe '11.0.0-beta2'
        Test-Beta2ConfigAfterStart $exe
        Test-Beta2VfsAfterStart $exe
    }
    Uninstall-Candidate 'D1'
    $removed = New-Snapshot 'deinstalliert' 'Prüfling deinstalliert'
    Test-Removal $base $removed 'ausgang-deinstalliert' @{} $false
}

function Invoke-Coexistence {
    $base = New-Snapshot 'ausgang' 'Ausgangszustand'
    Assert-Fresh $base
    $log = Join-Path $OutDir 'offiziell-installation.log'
    # SKIPAUTOUPDATE: der gestartete Client soll kein Update nachladen - dessen
    # Installation liefe ohne /norestart und könnte den Runner neu starten
    $run = Invoke-Step 'Offizieller Client 6.0.3 (MSI)' 'msiexec.exe' ('/i "{0}" /qn /norestart DO_NOT_SCHEDULE_REBOOT=1 LAUNCH_AT_SYSTEM_START=1 SKIPAUTOUPDATE=1 /l*v "{1}"' -f $OfficialMsi, $log)
    if ($run.zeitueberschreitung -or ($run.exitcode -ne 0 -and $run.exitcode -ne 3010)) {
        throw "offizieller Client ließ sich nicht installieren (msiexec $($run.exitcode), Protokoll offiziell-installation.log)"
    }
    # Anmeldeinformation mit dem Präfix des offiziellen Clients: gehört zu dessen
    # Fußabdruck und darf von unserer Deinstallation nicht berührt werden
    $secret = [guid]::NewGuid().ToString('N')
    $null = & cmdkey.exe ('/generic:ownCloud_credentials:probe.invalid:{0}:http/oauthtoken' -f [guid]::NewGuid()) '/user:probe' "/pass:$secret"
    $started = Start-OfficialClient
    $official = New-Snapshot 'offiziell' 'Offizieller Client installiert und gestartet'
    $footprint = Get-Footprint $base $official 'ausgang-offiziell'
    if ($footprint.Count -eq 0) { throw 'Fußabdruck des offiziellen Clients ist leer - Koexistenz nicht prüfbar' }
    Add-Check 'K0' 'Fußabdruck des offiziellen Clients erfasst' 'INFO' ('{0} Einträge, Client gestartet: {1}' -f $footprint.Count, $(if ($started) { 'ja' } else { 'nein' })) ''

    $running = Get-OfficialProcess
    Install-Candidate 'I1'
    $exe = Find-ClientExe
    Add-ClientTraces $exe
    $installed = New-Snapshot 'installiert' 'Prüfling neben dem offiziellen Client installiert'
    Test-Footprint $footprint $installed 'K1' 'Offizieller Client nach unserer Installation unverändert' 'H2, H3'
    Test-OfficialProcess $running 'K2' 'Offizieller Client läuft nach unserer Installation weiter'
    Test-Installation $official $installed $exe 'offiziell-installiert'

    $running = Get-OfficialProcess
    Uninstall-Candidate 'D1' -Purge
    $removed = New-Snapshot 'deinstalliert' 'Prüfling deinstalliert, offizieller Client bleibt'
    Test-Footprint $footprint $removed 'K3' 'Offizieller Client nach unserer Deinstallation unverändert' 'H2, H3'
    Test-OfficialProcess $running 'K4' 'Offizieller Client läuft nach unserer Deinstallation weiter'
    Test-Removal $official $removed 'offiziell-deinstalliert' $footprint $true

    # Gegenrichtung: der offizielle Deinstaller darf unseren Client nicht beschädigen
    Install-Candidate 'K5' 'Setup (Prüfling, erneut)'
    $again = New-Snapshot 'erneut-installiert' 'Prüfling erneut installiert'
    $ownFootprint = Get-Footprint $removed $again 'deinstalliert-erneut'

    # Das offizielle MSI sucht seine NSIS-Altinstallation per AppSearch und
    # startet deren UninstallString bei jeder Ausführung still mit /S (ohne
    # /norestart). Zeigt einer dieser Orte auf unseren Client, entfernt jede
    # Installation, Reparatur, Aktualisierung oder Deinstallation des
    # offiziellen Clients unseren - und ein Deinstaller, der still neu startet,
    # würde den Runner mitnehmen. Dann nur melden, nicht ausführen.
    $legacy = @(Get-MsiLegacyHits $OfficialMsi)
    if ($legacy.Count -gt 0) {
        Add-Check 'K6' 'Offizieller Installer hält unseren Client nicht für seine Altinstallation' 'FEHLER' ('msiexec liest {0}; bei Installation, Reparatur, Update und Deinstallation des offiziellen Clients startet es diesen Befehl still mit /S, unser Client wird entfernt (eigener Uninstall-Schlüssel nötig)' -f ($legacy -join '; ')) 'H3, AP 3.3'
        Add-Check 'K7' 'Unser Client nach Entfernen des offiziellen unverändert' 'NICHT GEPRÜFT' 'Deinstallation des offiziellen Clients übersprungen: sie würde unseren Deinstaller ohne /norestart starten' 'H3'
        return
    }
    Add-Check 'K6' 'Offizieller Installer hält unseren Client nicht für seine Altinstallation' 'OK' 'keiner seiner AppSearch-Orte zeigt auf owncloud.online' 'H3, AP 3.3'
    $log = Join-Path $OutDir 'offiziell-deinstallation.log'
    $run = Invoke-Step 'Offizieller Client entfernen (MSI)' 'msiexec.exe' ('/x "{0}" /qn /norestart /l*v "{1}"' -f $OfficialMsi, $log)
    if ($run.zeitueberschreitung -or ($run.exitcode -ne 0 -and $run.exitcode -ne 3010)) {
        Add-Check 'K6b' 'Offizieller Client lässt sich entfernen' 'WARNUNG' ('msiexec {0}, Protokoll offiziell-deinstallation.log' -f $run.exitcode) ''
    }
    $final = New-Snapshot 'offiziell-entfernt' 'Offizieller Client entfernt, Prüfling bleibt'
    Test-Footprint $ownFootprint $final 'K7' 'Unser Client nach Entfernen des offiziellen unverändert' 'H3'
}

# O1: beta1/beta2 legten HKLM\Software\Policies\BW-Tech GmbH\owncloud.online leer an, wenn der Client
# mit Administratorrechten lief. Die Deinstallation muss leere Schlüssel entfernen und jede Richtlinie
# eines Administrators stehen lassen - still und über die Oberfläche. Die Ausgangslagen decken ab,
# woran ein "nur wenn leer" scheitern kann: ein Wert direkt im Schlüssel (wert), nur ein
# Unterschlüssel mit Wert (setup), ein anderes Produkt unter demselben Hersteller (nachbar).
function Invoke-PolicyScenario {
    $base = New-Snapshot 'ausgang' 'Ausgangszustand'
    Assert-Fresh $base
    if ((Get-RegistryTree $policyVendorPath).Count -gt 0) { throw "kein frischer Rechner, HKLM\$policyVendorPath besteht schon" }
    # Durchgang, Ausgangslage, über die Oberfläche, Prüfung, Audit
    $passes = @(
        @('P1', 'leer', $false, 'Leerer Richtlinien-Schlüssel samt Elternschlüssel nach stiller Deinstallation entfernt', 'H6'),
        @('P2', 'leer', $true, 'Leerer Richtlinien-Schlüssel samt Elternschlüssel nach Deinstallation über die Oberfläche entfernt', 'H6'),
        @('P3', 'wert', $false, 'Richtlinie (Wert im Schlüssel) bleibt bei stiller Deinstallation unverändert', ''),
        @('P4', 'setup', $true, 'Richtlinie (Setup\ServerUrl) bleibt bei Deinstallation über die Oberfläche unverändert', ''),
        @('P5', 'nachbar', $false, 'Leerer Schlüssel entfernt, fremde Richtlinie unter BW-Tech GmbH bleibt (still)', 'H6')
    )
    foreach ($pass in $passes) {
        $id, $layout, $interactive, $title, $audit = $pass
        Install-Candidate "${id}a" "Setup (Prüfling, Durchgang $id)"
        Set-PolicyLayout $layout
        $before = Get-RegistryTree $policyVendorPath
        if ($interactive) { Uninstall-CandidateInteractive "${id}b" } else { Uninstall-Candidate "${id}b" }
        # wie D2: die Deinstallation ist durchgelaufen, auch die über die Oberfläche
        $entry = Get-OwnUninstallCommand
        if ($entry) {
            Add-Check "${id}c" 'Eintrag in Programme und Features entfernt' 'FEHLER' "noch da: $entry" ''
        } else {
            Add-Check "${id}c" 'Eintrag in Programme und Features entfernt' 'OK' '' ''
        }
        $after = Get-RegistryTree $policyVendorPath
        $differences = @(Compare-RegistryTree (Get-ExpectedPolicyTree $before $layout) $after | Sort-Object)
        if ($differences.Count -gt 0) {
            Add-Check $id $title 'FEHLER' ('{0} Abweichungen, z. B. {1}' -f $differences.Count, $differences[0]) $audit
            Add-List "$id $title" $differences
        } elseif ($layout -eq 'leer') {
            Add-Check $id $title 'OK' "HKLM\$policyVendorPath entfernt" $audit
        } else {
            Add-Check $id $title 'OK' ('{0} Einträge wie erwartet' -f $after.Count) $audit
        }
        # eigene Schlüssel der Probe für den nächsten Durchgang entfernen (vorher bestand keiner)
        $hklm.DeleteSubKeyTree($policyVendorPath, $false)
    }
    $final = New-Snapshot 'ende' 'Nach allen Durchgängen'
    Test-Residue $base $final 'ausgang-ende' @{}
}

# --- Kontoassistent (assistent) ----------------------------------------------
#
# Meldung zu v11.0.0-beta3: erster Start mit der Konfiguration des offiziellen Clients, "Neues Konto
# einrichten", Serveradresse, "Anmelden" - danach stürzte der Client ab. Die Durchgänge fahren den Ablauf
# eines Testers nach und belegen nach jedem Schritt Bildschirm und Prozess.

# Rohdaten, die nicht ins Artefakt gehören: ungefiltertes Protokoll des Clients (Rückruf mit Code),
# Anmelde-Link, Speicherabbilder vor der Prüfung, Debugger und Symbole
$rawRoot = Join-Path $(if ($env:RUNNER_TEMP) { $env:RUNNER_TEMP } else { [IO.Path]::GetTempPath() }) 'assistent-roh'
$dumpDir = Join-Path $rawRoot 'speicherabbilder'
$timeline = New-Object 'System.Collections.Generic.List[string]'
# Speicherabbilder, die schon einem Durchgang gehören (WerFault schreibt manchmal noch, wenn der nächste beginnt)
$knownDumps = New-Object 'System.Collections.Generic.HashSet[string]' ([StringComparer]::OrdinalIgnoreCase)
$shotCount = 0
$cdbPath = $null
$cdbSource = ''
$legacyConfigPath = Join-Path ([Environment]::GetFolderPath('ApplicationData')) 'ownCloud\owncloud.cfg'
$clientConfigDir = Join-Path ([Environment]::GetFolderPath('ApplicationData')) $appName
# Texte, wo die Oberfläche keine AutomationId trägt (deutsch über uiLanguage=de, englisch als Rückfall)
$legacyNewAccountPattern = '^(Neues Konto einrichten|Set up a new account)$'
$legacyTitlePattern = '^(Vom offiziellen Client übernehmen|Import from the official client)$'
$wizardTitlePattern = '^(Konto hinzufügen|Add account)'
$nextPattern = '^(Anmelden|Sign in|Anmeldung erneut öffnen|Open sign in again)$'
$finishPattern = '^(Abschließen|Fertigstellen|Fertig stellen|Fertig|Finish|Done)$'
$copyLinkPattern = '^(Anmelde-?URL kopieren|Anmeldelink kopieren|Copy sign in URL)$'

# Vordergrund und Tastatur für den Durchgang ohne UI Automation: Eingaben wie von Hand (SendInput)
$deskSource = @'
using System;
using System.Collections.Generic;
using System.Runtime.InteropServices;

namespace OcProbe
{
    public static class Desk
    {
        [StructLayout(LayoutKind.Sequential)]
        private struct KEYBDINPUT { public ushort wVk; public ushort wScan; public uint dwFlags; public uint time; public IntPtr dwExtraInfo; }
        [StructLayout(LayoutKind.Sequential)]
        private struct MOUSEINPUT { public int dx; public int dy; public uint mouseData; public uint dwFlags; public uint time; public IntPtr dwExtraInfo; }
        [StructLayout(LayoutKind.Explicit)]
        private struct INPUTUNION { [FieldOffset(0)] public MOUSEINPUT mi; [FieldOffset(0)] public KEYBDINPUT ki; }
        [StructLayout(LayoutKind.Sequential)]
        private struct INPUT { public uint type; public INPUTUNION u; }

        [DllImport("user32.dll")]
        private static extern bool SetForegroundWindow(IntPtr hwnd);
        [DllImport("user32.dll")]
        public static extern IntPtr GetForegroundWindow();
        [DllImport("user32.dll")]
        private static extern bool ShowWindow(IntPtr hwnd, int command);
        [DllImport("user32.dll")]
        private static extern bool BringWindowToTop(IntPtr hwnd);
        [DllImport("user32.dll")]
        private static extern void keybd_event(byte vk, byte scan, uint flags, UIntPtr extra);
        [DllImport("user32.dll", SetLastError = true)]
        private static extern uint SendInput(uint count, INPUT[] inputs, int size);

        private const uint INPUT_KEYBOARD = 1;
        private const uint KEYEVENTF_KEYUP = 0x0002;
        private const uint KEYEVENTF_UNICODE = 0x0004;

        // Alt kurz drücken: dann erlaubt Windows SetForegroundWindow auch einem Prozess im Hintergrund
        public static bool Activate(IntPtr hwnd)
        {
            keybd_event(0x12, 0, 0, UIntPtr.Zero);
            keybd_event(0x12, 0, KEYEVENTF_KEYUP, UIntPtr.Zero);
            ShowWindow(hwnd, 9);
            BringWindowToTop(hwnd);
            SetForegroundWindow(hwnd);
            return GetForegroundWindow() == hwnd;
        }

        private static INPUT Key(ushort vk, ushort scan, uint flags)
        {
            INPUT input = new INPUT();
            input.type = INPUT_KEYBOARD;
            input.u.ki.wVk = vk;
            input.u.ki.wScan = scan;
            input.u.ki.dwFlags = flags;
            return input;
        }

        private static uint Send(List<INPUT> inputs)
        {
            return SendInput((uint)inputs.Count, inputs.ToArray(), Marshal.SizeOf(typeof(INPUT)));
        }

        // Text Zeichen für Zeichen, unabhängig vom Tastaturlayout
        public static uint TypeText(string text)
        {
            List<INPUT> inputs = new List<INPUT>();
            foreach (char c in text)
            {
                inputs.Add(Key(0, c, KEYEVENTF_UNICODE));
                inputs.Add(Key(0, c, KEYEVENTF_UNICODE | KEYEVENTF_KEYUP));
            }
            return Send(inputs);
        }

        // eine Taste, wahlweise mit Strg (virtuelle Tastencodes: Enter 0x0D, Esc 0x1B, A 0x41)
        public static uint Press(ushort vk, bool control)
        {
            List<INPUT> inputs = new List<INPUT>();
            if (control) { inputs.Add(Key(0x11, 0, 0)); }
            inputs.Add(Key(vk, 0, 0));
            inputs.Add(Key(vk, 0, KEYEVENTF_KEYUP));
            if (control) { inputs.Add(Key(0x11, 0, KEYEVENTF_KEYUP)); }
            return Send(inputs);
        }
    }
}
'@

function Initialize-Desktop {
    if (-not ('OcProbe.Ui' -as [type])) { Add-Type -TypeDefinition $uiSource -Language CSharp }
    if (-not ('OcProbe.Desk' -as [type])) { Add-Type -TypeDefinition $deskSource -Language CSharp }
    Add-Type -AssemblyName System.Windows.Forms, System.Drawing, UIAutomationClient, UIAutomationTypes
}

function Add-Timeline {
    param([string] $Text)
    $line = '{0} {1}' -f (Get-Date -Format 'HH:mm:ss'), $Text
    $timeline.Add($line)
    Write-Host "   $line"
}

function Limit-Text {
    param([string] $Text, [int] $Length = 400)
    if (-not $Text) { return '' }
    $flat = ($Text -replace '\s+', ' ').Trim()
    if ($flat.Length -gt $Length) { return $flat.Substring(0, $Length) + ' …' }
    return $flat
}

# Protokolle und Ausgaben vor dem Hochladen: Code des Rückrufs, Tokens und der Zugang des Testkontos
function Protect-Text {
    param([string] $Text)
    if (-not $Text) { return $Text }
    $Text = [regex]::Replace($Text, '(?i)([?&]code=)[^&\s"''<>]+', '$1[entfernt]')
    $Text = [regex]::Replace($Text, '(?i)((?:access_token|refresh_token|id_token|code_verifier|client_secret|password)["'']?\s*[:=]\s*["'']?)[^"''&\s,}<>]+', '$1[entfernt]')
    $Text = [regex]::Replace($Text, '(?i)\b(Bearer|Basic)\s+[A-Za-z0-9._~+/=-]{8,}', '$1 [entfernt]')
    foreach ($secret in @($loginPass, $loginUser)) {
        if ($secret -and $secret.Length -ge 3) { $Text = $Text.Replace($secret, '[testkonto]') }
    }
    return $Text
}

function Copy-Protected {
    param([string] $Source, [string] $Target)
    $text = Read-SharedText $Source
    if (-not $text) { return $false }
    [IO.File]::WriteAllText($Target, (Protect-Text $text), (New-Object System.Text.UTF8Encoding $false))
    return $true
}

function Format-ExitCode {
    param($Code)
    if ($null -eq $Code) { return '-' }
    $unsigned = [BitConverter]::ToUInt32([BitConverter]::GetBytes([int]$Code), 0)
    return '{0} (0x{1:X8})' -f $Code, $unsigned
}

function Get-ProcessNote {
    param($Process)
    try {
        if ($Process.HasExited) { return 'beendet, Exit-Code ' + (Format-ExitCode $Process.ExitCode) }
        $Process.Refresh()
        return 'läuft (reagiert: {0}, {1} MB)' -f $(if ($Process.Responding) { 'ja' } else { 'nein' }), [int]($Process.WorkingSet64 / 1MB)
    } catch {
        return 'Zustand nicht lesbar: ' + $_.Exception.Message
    }
}

# ganzer Bildschirm als PNG in OutDir; Rückgabe: Dateiname oder der Grund, warum es keinen gibt
function Save-Screenshot {
    param([string] $Name)
    $script:shotCount++
    $file = Join-Path $OutDir ('foto-{0:D2}-{1}.png' -f $script:shotCount, $Name)
    try {
        $bounds = [System.Windows.Forms.SystemInformation]::VirtualScreen
        $bitmap = New-Object System.Drawing.Bitmap $bounds.Width, $bounds.Height
        try {
            $graphics = [System.Drawing.Graphics]::FromImage($bitmap)
            try { $graphics.CopyFromScreen($bounds.X, $bounds.Y, 0, 0, $bitmap.Size) } finally { $graphics.Dispose() }
            $bitmap.Save($file, [System.Drawing.Imaging.ImageFormat]::Png)
        } finally {
            $bitmap.Dispose()
        }
        return (Split-Path -Leaf $file)
    } catch {
        return ('kein Foto: {0}' -f $_.Exception.Message)
    }
}

# Fenstertitel über Win32 (WM_GETTEXT) - ohne UI Automation, für den Durchgang mit Tastatur
function Get-WindowTitles {
    param([int] $ProcessId)
    $titles = @(foreach ($window in @([OcProbe.Ui]::TopWindows($ProcessId))) { '"{0}" ({1})' -f $window.Text, $window.ClassName })
    if ($titles.Count -eq 0) { return 'keine Fenster' }
    return 'Fenster ' + ($titles -join ', ')
}

function Find-TopWindow {
    param([int] $ProcessId, [string] $Pattern)
    foreach ($window in @([OcProbe.Ui]::TopWindows($ProcessId))) {
        if ($window.Text -match $Pattern) { return $window }
    }
    return $null
}

# sichtbare Fenster des Prozesses mit ihren Elementen (UI Automation). Das Hauptfenster nur auf Wunsch
# mit Inhalt: Assistent und Dialoge sind eigene Fenster, und das Hauptfenster ist groß.
function Get-UiItems {
    param([int] $ProcessId, [switch] $IncludeMain)
    $items = New-Object 'System.Collections.Generic.List[object]'
    foreach ($top in @([OcProbe.Ui]::TopWindows($ProcessId))) {
        $scope = if ($top.Text -eq $appName -and -not $IncludeMain) { [System.Windows.Automation.TreeScope]::Element } else { [System.Windows.Automation.TreeScope]::Subtree }
        try {
            $window = [System.Windows.Automation.AutomationElement]::FromHandle($top.Handle)
            $title = [string]$window.Current.Name
            $elements = $window.FindAll($scope, [System.Windows.Automation.Condition]::TrueCondition)
        } catch {
            continue
        }
        foreach ($element in $elements) {
            try {
                $current = $element.Current
                $rect = $current.BoundingRectangle
                $type = ([string]$current.ControlType.ProgrammaticName).Replace('ControlType.', '')
                $visible = (-not $current.IsOffscreen) -and (-not $rect.IsEmpty) -and $rect.Width -gt 0 -and $rect.Height -gt 0
                $item = [pscustomobject]@{
                    Window  = $title
                    Element = $element
                    Type    = $type
                    Name    = [string]$current.Name
                    Id      = [string]$current.AutomationId
                    Enabled = [bool]$current.IsEnabled
                    Visible = $visible
                    Rect    = $rect
                }
                $items.Add($item)
            } catch {
                # Element inzwischen verschwunden
            }
        }
    }
    return , $items
}

function Get-UiValue {
    param($Item)
    try {
        $value = $Item.Element.GetCurrentPropertyValue([System.Windows.Automation.ValuePattern]::ValueProperty)
        if ($value -eq [System.Windows.Automation.AutomationElement]::NotSupported) { return '' }
        return [string]$value
    } catch {
        return '?'
    }
}

function Test-UiSelected {
    param($Item)
    try { return [bool]$Item.Element.GetCurrentPropertyValue([System.Windows.Automation.SelectionItemPattern]::IsSelectedProperty) } catch { return $false }
}

function Invoke-UiItem {
    param($Item)
    if (-not $Item) { return $false }
    $pattern = $null
    try {
        if ($Item.Element.TryGetCurrentPattern([System.Windows.Automation.InvokePattern]::Pattern, [ref] $pattern)) {
            $pattern.Invoke()
            return $true
        }
    } catch {
        Add-Timeline ('Auslösen von "{0}" fehlgeschlagen: {1}' -f $Item.Name, $_.Exception.Message)
    }
    return $false
}

function Set-UiValue {
    param($Item, [string] $Text)
    $pattern = $null
    try {
        if ($Item.Element.TryGetCurrentPattern([System.Windows.Automation.ValuePattern]::Pattern, [ref] $pattern)) {
            $pattern.SetValue($Text)
            return $true
        }
    } catch {
        Add-Timeline ('Eintragen in "{0}" fehlgeschlagen: {1}' -f $Item.Name, $_.Exception.Message)
    }
    return $false
}

# Knopf über die AutomationId (objectName, sprachunabhängig), sonst über den Text
function Find-UiButton {
    param($Items, [string] $IdPattern, [string] $NamePattern)
    $visible = @($Items | Where-Object { $_.Visible -and $_.Type -eq 'Button' })
    if ($IdPattern) {
        $byId = @($visible | Where-Object { $_.Id -like $IdPattern })
        if ($byId.Count -gt 0) { return $byId[0] }
    }
    if ($NamePattern) {
        $byName = @($visible | Where-Object { $_.Name -match $NamePattern })
        if ($byName.Count -gt 0) { return $byName[0] }
    }
    return $null
}

function Get-UrlField {
    param($Items)
    return (@($Items | Where-Object { $_.Visible -and ($_.Id -like '*ServerAddressLineEdit*' -or ($_.Type -eq 'Edit' -and $_.Name -match 'Serveradress|Server address')) }) | Select-Object -First 1)
}

# Zustand des Ablaufs aus den sichtbaren Elementen: umstieg, adresse, anmeldung, erfolg, erweitert oder leer
function Get-AssistentState {
    param($Items)
    $visible = @($Items | Where-Object { $_.Visible })
    if (@($visible | Where-Object { $_.Type -eq 'Button' -and $_.Name -match $legacyNewAccountPattern }).Count -gt 0) { return 'umstieg' }
    if (@($visible | Where-Object { $_.Id -like '*CopyUrlToClipboardButton*' -or ($_.Type -eq 'Button' -and $_.Name -match $copyLinkPattern) }).Count -gt 0) { return 'anmeldung' }
    if (@($visible | Where-Object { $_.Type -eq 'RadioButton' }).Count -gt 0) { return 'erweitert' }
    if (Get-UrlField $visible) { return 'adresse' }
    if (Find-UiButton $visible '*qt_wizard_finish*' $finishPattern) { return 'erfolg' }
    return ''
}

# Meldungen unter dem Adressfeld (Fehler und Warnungen der Adressprüfung)
function Get-UrlPageMessages {
    param($Items)
    $field = Get-UrlField $Items
    if (-not $field) { return @() }
    return @($Items | Where-Object { $_.Visible -and $_.Type -eq 'Text' -and $_.Name -and $_.Window -eq $field.Window -and $_.Rect.Top -ge ($field.Rect.Bottom - 1) -and $_.Name -notmatch '[Ll]ogo' })
}

# Warnung samt Lage: reicht das Feld der Meldung unter die Knopfzeile, gibt es eine Bildlaufleiste?
function Get-WarningDetail {
    param($Items, $Messages)
    $label = $Messages[0]
    $text = Limit-Text ((@($Messages | ForEach-Object { $_.Name })) -join ' ') 900
    $buttons = @($Items | Where-Object { $_.Visible -and $_.Type -eq 'Button' -and $_.Window -eq $label.Window })
    $buttonTop = if ($buttons.Count -gt 0) { ($buttons | ForEach-Object { $_.Rect.Top } | Measure-Object -Minimum).Minimum } else { $null }
    $scroll = @($Items | Where-Object { $_.Visible -and $_.Type -eq 'ScrollBar' -and $_.Window -eq $label.Window }).Count -gt 0
    $window = @($Items | Where-Object { $_.Type -eq 'Window' -and $_.Window -eq $label.Window }) | Select-Object -First 1
    $rect = $label.Rect
    $where = if ($null -eq $buttonTop) { 'Knopfzeile nicht gefunden' } elseif ($rect.Bottom -gt $buttonTop) { 'ragt unter die Knopfzeile' } else { 'endet über der Knopfzeile' }
    return ('„{0}“ | Meldungsfeld {1:N0}x{2:N0} bei y={3:N0}..{4:N0}, Knopfzeile ab y={5}, Fenster {6}: {7}; Bildlaufleiste: {8}' -f $text, $rect.Width, $rect.Height, $rect.Top, $rect.Bottom,
        $(if ($null -ne $buttonTop) { '{0:N0}' -f $buttonTop } else { '-' }), $(if ($window) { $window.Rect } else { '-' }), $where, $(if ($scroll) { 'ja' } else { 'nein' }))
}

# kurze Beschreibung der Fenster für den Ablauf
function Get-UiSummary {
    param($Items)
    $parts = New-Object 'System.Collections.Generic.List[string]'
    foreach ($group in @($Items | Group-Object -Property Window)) {
        $entries = New-Object 'System.Collections.Generic.List[string]'
        foreach ($item in @($group.Group | Where-Object { $_.Visible -and $_.Type -ne 'Window' })) {
            switch ($item.Type) {
                'Text' { if ($item.Name) { $entries.Add(('Text "{0}"' -f (Limit-Text $item.Name 300))) } }
                'Button' { if ($item.Name) { $entries.Add(('Knopf "{0}"{1}' -f $item.Name, $(if ($item.Enabled) { '' } else { ' (gesperrt)' }))) } }
                'Edit' { $entries.Add(('Eingabe "{0}" = "{1}"' -f $item.Name, (Get-UiValue $item))) }
                'RadioButton' { $entries.Add(('Option "{0}"{1}' -f $item.Name, $(if (Test-UiSelected $item) { ' (gewählt)' } else { '' }))) }
                'ScrollBar' { $entries.Add('Bildlaufleiste') }
            }
        }
        $parts.Add(('Fenster "{0}": {1}' -f $group.Name, (Limit-Text ($entries -join '; ') 1500)))
    }
    if ($parts.Count -eq 0) { return 'keine Fenster' }
    return ($parts -join ' | ')
}

# alle Elemente in eine Datei, zum Nachsehen der AutomationIds
function Save-UiTree {
    param($Ctx, $Items, [string] $Label)
    $file = Join-Path $OutDir ('{0}-ui-{1}.txt' -f $Ctx.Name, $Label)
    $lines = @(foreach ($item in $Items) {
            '{0} | {1} | "{2}" | id={3} | aktiv={4} | sichtbar={5} | {6}' -f $item.Window, $item.Type, (Limit-Text $item.Name 200), $item.Id, $item.Enabled, $item.Visible, $item.Rect
        })
    [IO.File]::WriteAllLines($file, [string[]]$lines, (New-Object System.Text.UTF8Encoding $false))
}

# wartet, bis $Condition etwas liefert, der Prozess endet oder die Zeit abläuft
function Wait-Ui {
    param($Ctx, [scriptblock] $Condition, [int] $Seconds)
    $deadline = (Get-Date).AddSeconds($Seconds)
    do {
        if ($Ctx.Process.HasExited) { return $null }
        $value = $null
        try { $value = & $Condition } catch { $value = $null }
        if ($value) { return $value }
        Start-Sleep -Milliseconds 500
    } while ((Get-Date) -lt $deadline)
    return $null
}

# nach jedem Schritt: Bildschirmfoto, Zustand des Prozesses und was die Fenster zeigen
function Write-StepRecord {
    param($Ctx, [string] $Step, [string] $Label)
    $Ctx.Step++
    $shot = Save-Screenshot ('{0}-{1:D2}-{2}' -f $Ctx.Name, $Ctx.Step, $Step)
    $Ctx.Shots.Add($shot)
    $status = Get-ProcessNote $Ctx.Process
    $windows = '-'
    if (-not $Ctx.Process.HasExited) {
        try {
            $windows = if ($Ctx.Uia) { Get-UiSummary (Get-UiItems $Ctx.Process.Id -IncludeMain) } else { Get-WindowTitles $Ctx.Process.Id }
        } catch {
            $windows = 'Fenster nicht lesbar: ' + $_.Exception.Message
        }
    }
    Add-Timeline ('{0} [{1}] {2} | Prozess: {3} | Foto: {4} | {5}' -f $Ctx.Name, $Step, $Label, $status, $shot, $windows)
}

# Prozess beendet? Dann den Schritt festhalten, in dem es geschah
function Test-Crashed {
    param($Ctx, [string] $During)
    if (-not $Ctx.Process.HasExited) { return $false }
    if (-not $Ctx.Crashed) {
        $Ctx.Crashed = $true
        $Ctx.CrashStep = $During
        Add-Timeline ('{0}: Prozess beendet während: {1} - Exit-Code {2}' -f $Ctx.Name, $During, (Format-ExitCode $Ctx.Process.ExitCode))
        Write-StepRecord $Ctx 'absturz' "Prozess beendet ($During)"
    }
    return $true
}

function Enable-LocalDumps {
    New-Item -ItemType Directory -Force -Path $dumpDir | Out-Null
    $dword = [Microsoft.Win32.RegistryValueKind]::DWord
    $wer = $hklm.CreateSubKey('SOFTWARE\Microsoft\Windows\Windows Error Reporting')
    try {
        $wer.SetValue('Disabled', 0, $dword)
        $wer.SetValue('DontShowUI', 1, $dword)
    } finally {
        $wer.Close()
    }
    $key = $hklm.CreateSubKey("SOFTWARE\Microsoft\Windows\Windows Error Reporting\LocalDumps\$appName.exe")
    try {
        $key.SetValue('DumpFolder', $dumpDir, [Microsoft.Win32.RegistryValueKind]::ExpandString)
        $key.SetValue('DumpType', 2, $dword)
        $key.SetValue('DumpCount', 10, $dword)
    } finally {
        $key.Close()
    }
    $notes = @("HKLM\SOFTWARE\Microsoft\Windows\Windows Error Reporting\LocalDumps\$appName.exe: DumpType=2, DumpFolder außerhalb des Artefakts")
    $policy = $hklm.OpenSubKey('SOFTWARE\Policies\Microsoft\Windows\Windows Error Reporting', $false)
    if ($policy) {
        try { $notes += 'Richtlinie Disabled=' + [string]$policy.GetValue('Disabled') } finally { $policy.Close() }
    }
    try { $notes += 'Dienst WerSvc: ' + (Get-Service -Name WerSvc -ErrorAction Stop).StartType } catch { $notes += 'Dienst WerSvc: nicht lesbar' }
    return ($notes -join '; ')
}

function Stop-Client {
    foreach ($process in @(Get-Process -Name $appName -ErrorAction SilentlyContinue)) {
        try { $process.Kill(); $null = $process.WaitForExit(15000) } catch { }
    }
}

function Stop-OfficialClient {
    foreach ($process in @(Get-Process -Name 'owncloud' -ErrorAction SilentlyContinue)) {
        try { $process.Kill(); $null = $process.WaitForExit(15000) } catch { }
    }
}

# Browser, die der Client für die Anmeldung öffnet (und die Auswahl "Öffnen mit", falls es keinen gibt)
function Stop-Browsers {
    foreach ($process in @(Get-Process -Name 'msedge', 'chrome', 'firefox', 'OpenWith' -ErrorAction SilentlyContinue)) {
        try { $process.Kill() } catch { }
    }
}

# frische Ausgangslage je Durchgang: keine Konfiguration, kein Cache, keine Anmeldedaten des Clients. Die
# Sync-Ordner früherer Durchgänge bleiben; der Assistent schlägt dann "owncloud.online (2)" usw. vor.
function Reset-ClientState {
    Stop-Client
    Stop-Browsers
    foreach ($dir in $clientConfigDir, (Join-Path ([Environment]::GetFolderPath('LocalApplicationData')) $appName)) {
        if (Test-Path -LiteralPath $dir) { Remove-Item -LiteralPath $dir -Recurse -Force -ErrorAction SilentlyContinue }
    }
    foreach ($line in @(& cmdkey.exe /list)) {
        if ($line -match 'target=(owncloud\.online_credentials\S*)') { $null = & cmdkey.exe "/delete:$($Matches[1])" }
    }
    $crashLog = Join-Path ([IO.Path]::GetTempPath()) "$appName-crash.log"
    if (Test-Path -LiteralPath $crashLog) { Remove-Item -LiteralPath $crashLog -Force -ErrorAction SilentlyContinue }
}

# Oberfläche deutsch wie beim Tester (ConfigFile::uiLanguage); sonst eine leere Konfiguration
function Write-ClientConfig {
    New-Item -ItemType Directory -Force -Path $clientConfigDir | Out-Null
    [IO.File]::WriteAllText((Join-Path $clientConfigDir "$appName.cfg"), "[General]`r`nuiLanguage=de`r`n")
}

# Konfiguration des offiziellen Clients wie beim Tester: ein Konto admin auf dem Server ohne OAuth2 und ein
# Sync-Ordner (LegacyClient::readConfig liest url, dav_user, display-name und die Ordnergruppen)
function Set-LegacyConfig {
    param([bool] $Present)
    if (-not $Present) {
        if (Test-Path -LiteralPath $legacyConfigPath) { Remove-Item -LiteralPath $legacyConfigPath -Force }
        return
    }
    $server = $WrongServerUrl.TrimEnd('/')
    $syncDir = Join-Path $env:USERPROFILE 'ownCloud'
    New-Item -ItemType Directory -Force -Path $syncDir, (Split-Path -Parent $legacyConfigPath) | Out-Null
    [IO.File]::WriteAllText((Join-Path $syncDir 'Datei des offiziellen Clients.txt'), "Probe`r`n")
    $content = @(
        '[General]'
        'clientVersion=6.0.3.18040'
        ''
        '[Accounts]'
        "0\url=$server"
        '0\dav_user=admin'
        '0\display-name=admin'
        ('0\Folders\1\localPath={0}/' -f ($syncDir -replace '\\', '/'))
        '0\Folders\1\journalPath=.sync_journal.db'
        '0\Folders\1\targetPath=/'
        "0\Folders\1\davUrl=$server/remote.php/dav/files/admin/"
        '0\Folders\1\displayString=ownCloud'
        '0\Folders\1\paused=false'
        '0\Folders\1\virtualFilesMode=off'
        'version=13'
    ) -join "`r`n"
    [IO.File]::WriteAllText($legacyConfigPath, $content + "`r`n")
}

function Test-LegacyConfig {
    if (-not (Test-Path -LiteralPath $legacyConfigPath -PathType Leaf)) { return $false }
    return ((Get-Content -LiteralPath $legacyConfigPath -Raw) -match '(?m)^0\\url=')
}

function Get-Python {
    foreach ($candidate in 'python', 'py') {
        $command = Get-Command $candidate -CommandType Application -ErrorAction SilentlyContinue | Select-Object -First 1
        # der Platzhalter aus dem Store startet keinen Interpreter
        if ($command -and $command.Source -notlike '*\WindowsApps\*') { return $command.Source }
    }
    return $null
}

# Anmelde-Link, den der Client öffnet: aus der Befehlszeile des Browsers, sonst über "Anmelde-URL kopieren"
function Get-AuthorizeUrl {
    param($Ctx)
    $serverHost = ([uri]$ServerUrl).Host
    $deadline = (Get-Date).AddSeconds(45)
    while ((Get-Date) -lt $deadline -and -not $Ctx.Process.HasExited) {
        foreach ($browser in @(Get-CimInstance -ClassName Win32_Process -Filter "Name='msedge.exe' OR Name='chrome.exe' OR Name='firefox.exe'" -ErrorAction SilentlyContinue)) {
            $match = [regex]::Match([string]$browser.CommandLine, 'https://[^\s"]+')
            if (-not $match.Success) { continue }
            $uri = $null
            if (-not [uri]::TryCreate($match.Value, [UriKind]::Absolute, [ref] $uri)) { continue }
            if ($uri.Host -eq $serverHost -and $match.Value -match '[?&]redirect_uri=') {
                return @{ Url = $match.Value; Source = "Befehlszeile von $($browser.Name)" }
            }
        }
        if ($Ctx.Uia) {
            $copy = Find-UiButton (Get-UiItems $Ctx.Process.Id) '*CopyUrlToClipboardButton*' $copyLinkPattern
            if ($copy) {
                try { [System.Windows.Forms.Clipboard]::Clear() } catch { }
                if (Invoke-UiItem $copy) {
                    Start-Sleep -Milliseconds 800
                    $text = ''
                    try { $text = ([string](Get-Clipboard -Raw)).Trim() } catch { }
                    if ($text -match '^https://' -and $text -match '[?&]redirect_uri=') {
                        return @{ Url = $text; Source = 'Zwischenablage (Knopf "Anmelde-URL kopieren")' }
                    }
                }
            }
        }
        Start-Sleep -Milliseconds 700
    }
    return $null
}

# Browser-Anmeldung mit dem Link des Clients (assistent-anmeldung.py). Der Zugang geht nur in die Umgebung
# dieses einen Prozesses; die Ausgabe enthält weder Code noch Tokens.
function Invoke-BrowserLogin {
    param($Ctx, [string] $AuthorizeUrl)
    $result = @{ Ok = $false; Summary = ''; ExitCode = $null }
    $wrapper = Join-Path $PSScriptRoot 'assistent-anmeldung.py'
    if (-not $LoginScript -or -not (Test-Path -LiteralPath $LoginScript -PathType Leaf)) { $result.Summary = 'login.py aus dem Zweig sync-e2e fehlt (-LoginScript)'; return $result }
    if (-not $loginUser -or -not $loginPass) { $result.Summary = 'Zugang des Testkontos fehlt (Secrets E2E_ONLINE_USER_1, E2E_ONLINE_PASS_1)'; return $result }
    $python = Get-Python
    if (-not $python) { $result.Summary = 'kein Python gefunden'; return $result }
    $urlFile = Join-Path $rawRoot ('{0}-anmeldelink.txt' -f $Ctx.Name)
    [IO.File]::WriteAllText($urlFile, $AuthorizeUrl)
    $info = New-Object System.Diagnostics.ProcessStartInfo
    $info.FileName = $python
    $info.Arguments = '-I "{0}" --login-py "{1}" --url-file "{2}"' -f $wrapper, $LoginScript, $urlFile
    $info.UseShellExecute = $false
    $info.CreateNoWindow = $true
    $info.RedirectStandardOutput = $true
    $info.RedirectStandardError = $true
    $info.StandardOutputEncoding = [Text.Encoding]::UTF8
    $info.StandardErrorEncoding = [Text.Encoding]::UTF8
    $info.EnvironmentVariables['OC_USER'] = $loginUser
    $info.EnvironmentVariables['OC_PASS'] = $loginPass
    $info.EnvironmentVariables['PYTHONIOENCODING'] = 'utf-8'
    # ab hier kann der Client Tokens im Speicher haben
    $Ctx.TokensPossible = $true
    $process = [System.Diagnostics.Process]::Start($info)
    $stdout = $process.StandardOutput.ReadToEndAsync()
    $stderr = $process.StandardError.ReadToEndAsync()
    if (-not $process.WaitForExit(180 * 1000)) {
        try { $process.Kill() } catch { }
        $result.Summary = 'nach 180 s abgebrochen; '
    }
    $process.WaitForExit()
    $output = (Protect-Text ($stdout.Result + $stderr.Result)).Trim()
    [IO.File]::WriteAllText((Join-Path $OutDir ('{0}-anmeldung.txt' -f $Ctx.Name)), $output + "`r`n", (New-Object System.Text.UTF8Encoding $false))
    $result.ExitCode = $process.ExitCode
    $result.Ok = $process.ExitCode -eq 0
    $last = @($output -split "`r?`n" | Where-Object { $_ -match 'Rückruf|Capabilities|Fehler|abgelehnt|ohne oauth2|fehlen' }) | Select-Object -Last 2
    if (-not $last) { $last = @($output -split "`r?`n" | Where-Object { $_.Trim() }) | Select-Object -Last 1 }
    $result.Summary += 'Exit-Code {0}: {1}' -f $process.ExitCode, (@($last) -join ' / ')
    return $result
}

function Complete-Login {
    param($Ctx)
    $link = Get-AuthorizeUrl $Ctx
    if (Test-Crashed $Ctx 'Warten auf den Anmelde-Link (Browser wird geöffnet)') { return }
    if (-not $link) {
        $Ctx.Problem = 'Anmelde-Link nicht ermittelbar (kein Browser mit dem Link, Zwischenablage leer)'
        Write-StepRecord $Ctx 'kein-link' $Ctx.Problem
        return
    }
    Add-Timeline ('Anmelde-Link aus {0}' -f $link.Source)
    Stop-Browsers
    $login = Invoke-BrowserLogin $Ctx $link.Url
    $Ctx.LoginSummary = $login.Summary
    Add-Timeline ('Browser-Anmeldung (assistent-anmeldung.py): {0}' -f $login.Summary)
    if ($login.Ok) {
        $Ctx.LoginDone = $true
        $Ctx.Reached = 'Browser-Anmeldung abgeschlossen'
    }
    Start-Sleep -Seconds 2
    if (Test-Crashed $Ctx 'Rückruf des Browsers und Prüfung des Kontos') { return }
    Write-StepRecord $Ctx 'nach-browser' ('nach der Browser-Anmeldung: {0}' -f $login.Summary)
    if (-not $login.Ok) { $Ctx.Problem = 'Browser-Anmeldung gescheitert: ' + $login.Summary }
}

# nach "Fertigstellen": Konto anlegen, Ordner einrichten - beobachten
function Watch-AccountSetup {
    param($Ctx)
    $start = Get-Date
    foreach ($mark in 3, 15, 30, 60, 90) {
        while (((Get-Date) - $start).TotalSeconds -lt $mark -and -not $Ctx.Process.HasExited) { Start-Sleep -Milliseconds 500 }
        if (Test-Crashed $Ctx ('Konto anlegen nach "Fertigstellen" (bis {0} s)' -f $mark)) { return }
        Write-StepRecord $Ctx ('konto-{0:D2}s' -f $mark) ('{0} s nach "Fertigstellen"' -f $mark)
    }
    $Ctx.Reached = 'Konto angelegt, 90 s beobachtet'
}

function Invoke-UiaFlow {
    param($Ctx, [hashtable] $Definition)
    if ($Definition.Legacy) {
        $found = Wait-Ui $Ctx { $i = Get-UiItems $Ctx.Process.Id; if ((Get-AssistentState $i) -eq 'umstieg') { @{ Items = $i } } } 90
        if (Test-Crashed $Ctx 'Start bis zum Umstieg-Dialog') { return }
        if (-not $found) {
            # ohne Dialog kann der Assistent schon offen sein (Konfiguration des offiziellen Clients nicht erkannt)
            $Ctx.Problem = 'Umstieg-Dialog erschien nicht innerhalb von 90 s'
            Write-StepRecord $Ctx 'kein-umstieg' $Ctx.Problem
        } else {
            $Ctx.LegacySeen = $true
            $Ctx.Reached = 'Umstieg-Dialog'
            $Ctx.LegacyText = Limit-Text ((@($found.Items | Where-Object { $_.Visible -and $_.Type -eq 'Text' -and $_.Name } | ForEach-Object { $_.Name })) -join ' / ') 600
            Save-UiTree $Ctx $found.Items 'umstieg'
            Write-StepRecord $Ctx 'umstieg' 'Umstieg-Dialog "Vom offiziellen Client übernehmen"'
            $button = Find-UiButton $found.Items '' $legacyNewAccountPattern
            if (-not (Invoke-UiItem $button)) {
                $Ctx.Problem = 'Knopf "Neues Konto einrichten" nicht auslösbar'
                return
            }
            Add-Timeline ('Klick: "{0}"' -f $button.Name)
        }
    }

    $found = Wait-Ui $Ctx { $i = Get-UiItems $Ctx.Process.Id; if ((Get-AssistentState $i) -eq 'adresse') { @{ Items = $i } } } 60
    if (Test-Crashed $Ctx 'Öffnen des Assistenten') { return }
    if (-not $found) {
        $Ctx.Problem = 'Adressseite des Assistenten erschien nicht innerhalb von 60 s'
        Write-StepRecord $Ctx 'kein-assistent' $Ctx.Problem
        return
    }
    $Ctx.Reached = 'Adressseite'
    Save-UiTree $Ctx $found.Items 'adresse'
    Write-StepRecord $Ctx 'adressseite' 'Assistent, Adressseite'

    foreach ($url in $Definition.Urls) {
        $items = Get-UiItems $Ctx.Process.Id
        $field = Get-UrlField $items
        $next = Find-UiButton $items '*wizardbutton1' $nextPattern
        if (-not $field -or -not $next) {
            $Ctx.Problem = 'Adressfeld oder Knopf "Anmelden" nicht gefunden'
            Write-StepRecord $Ctx 'nicht-gefunden' $Ctx.Problem
            return
        }
        if (-not (Set-UiValue $field $url)) {
            $Ctx.Problem = "Adresse $url nicht eintragbar"
            return
        }
        Start-Sleep -Milliseconds 300
        Write-StepRecord $Ctx 'adresse' ('Adresse eingetragen: {0} (Feld zeigt "{1}")' -f $url, (Get-UiValue $field))
        if (-not (Invoke-UiItem $next)) {
            $Ctx.Problem = 'Knopf "Anmelden" nicht auslösbar'
            return
        }
        Add-Timeline ('Klick: "{0}" mit {1}' -f $next.Name, $url)
        $start = Get-Date
        Start-Sleep -Milliseconds 700
        # Ergebnis: Meldung auf der Adressseite (Knopf wieder frei), Anmeldeseite oder Ende des Prozesses
        $outcome = Wait-Ui $Ctx {
            $i = Get-UiItems $Ctx.Process.Id
            $state = Get-AssistentState $i
            if ($state -eq 'adresse') {
                $nextNow = Find-UiButton $i '*wizardbutton1' $nextPattern
                $messages = @(Get-UrlPageMessages $i)
                if ($messages.Count -gt 0 -and $nextNow -and $nextNow.Enabled) { @{ State = 'warnung'; Items = $i; Messages = $messages } }
            } elseif ($state) {
                @{ State = $state; Items = $i; Messages = @() }
            }
        } 150
        $seconds = [int]((Get-Date) - $start).TotalSeconds
        if (Test-Crashed $Ctx ('"Anmelden" mit {0} ({1} s danach)' -f $url, $seconds)) { return }
        if (-not $outcome) {
            $Ctx.Problem = 'kein Ergebnis {0} s nach "Anmelden" mit {1}' -f $seconds, $url
            Write-StepRecord $Ctx 'kein-ergebnis' $Ctx.Problem
            return
        }
        if ($outcome.State -eq 'warnung') {
            $detail = Get-WarningDetail $outcome.Items $outcome.Messages
            $Ctx.Warnings.Add(@{ Url = $url; Detail = $detail })
            $Ctx.Reached = "Warnung für $url"
            Save-UiTree $Ctx $outcome.Items ('warnung-' + ([uri]$url).Host)
            Write-StepRecord $Ctx 'warnung' ('Meldung {0} s nach "Anmelden" mit {1}' -f $seconds, $url)
            Add-Timeline "Meldung: $detail"
            continue
        }
        if ($outcome.State -eq 'anmeldung') {
            $Ctx.ReachedLogin = $true
            $Ctx.Reached = "Anmeldeseite für $url"
            Save-UiTree $Ctx $outcome.Items 'anmeldung'
            Write-StepRecord $Ctx 'anmeldeseite' ('Anmeldeseite {0} s nach "Anmelden" mit {1}' -f $seconds, $url)
            break
        }
        $Ctx.Problem = 'unerwarteter Zustand "{0}" nach "Anmelden" mit {1}' -f $outcome.State, $url
        Write-StepRecord $Ctx 'unerwartet' $Ctx.Problem
        return
    }
    if (-not $Ctx.ReachedLogin) { return }

    Complete-Login $Ctx
    if (-not $Ctx.LoginDone -or $Ctx.Process.HasExited) { return }

    $found = Wait-Ui $Ctx { $i = Get-UiItems $Ctx.Process.Id; if ((Get-AssistentState $i) -eq 'erfolg') { @{ Items = $i } } } 120
    if (Test-Crashed $Ctx 'Prüfung des Kontos nach dem Rückruf (bis zur Erfolgsseite)') { return }
    if (-not $found) {
        $Ctx.Problem = 'Erfolgsseite erschien nicht innerhalb von 120 s nach der Browser-Anmeldung'
        Write-StepRecord $Ctx 'keine-erfolgsseite' $Ctx.Problem
        return
    }
    $Ctx.SuccessPage = $true
    $Ctx.Reached = 'Erfolgsseite'
    Save-UiTree $Ctx $found.Items 'erfolg'
    Write-StepRecord $Ctx 'erfolgsseite' 'Erfolgsseite'
    $items = $found.Items

    if ($Definition.Advanced) {
        $advanced = Find-UiButton $items '*wizardbutton6' '^(Erweitert|Advanced)$'
        if (Invoke-UiItem $advanced) {
            Add-Timeline ('Klick: "{0}"' -f $advanced.Name)
            $found = Wait-Ui $Ctx { $i = Get-UiItems $Ctx.Process.Id; if ((Get-AssistentState $i) -eq 'erweitert') { @{ Items = $i } } } 30
            if (Test-Crashed $Ctx 'Erweiterte Einstellungen öffnen') { return }
            if ($found) {
                $items = $found.Items
                $Ctx.Reached = 'Erweiterte Einstellungen'
                Save-UiTree $Ctx $items 'erweitert'
                Write-StepRecord $Ctx 'erweitert' 'Erweiterte Einstellungen (Vorgaben unverändert)'
            } else {
                Add-Timeline 'Erweiterte Einstellungen erschienen nicht innerhalb von 30 s'
            }
        } else {
            Add-Timeline 'Knopf "Erweitert" nicht gefunden oder nicht auslösbar'
        }
    }

    $finish = Find-UiButton $items '*qt_wizard_finish*' $finishPattern
    if (-not (Invoke-UiItem $finish)) {
        $Ctx.Problem = 'Knopf "Fertigstellen" nicht gefunden oder nicht auslösbar'
        Write-StepRecord $Ctx 'kein-fertigstellen' $Ctx.Problem
        return
    }
    $Ctx.Finished = $true
    $Ctx.Reached = 'Fertigstellen'
    Add-Timeline ('Klick: "{0}"' -f $finish.Name)
    Watch-AccountSetup $Ctx
}

# derselbe Ablauf nur mit Tastatur und Fenstertiteln: im Client wird die Barrierefreiheits-Schnittstelle
# (UI Automation) nie aktiv, wie bei einem Tester ohne Hilfsprogramme. Fortschritt aus dem Protokoll.
function Invoke-KeyboardFlow {
    param($Ctx, [hashtable] $Definition)
    if ($Definition.Legacy) {
        $dialog = Wait-Ui $Ctx { $w = Find-TopWindow $Ctx.Process.Id $legacyTitlePattern; if ($w) { @{ Window = $w } } } 90
        if (Test-Crashed $Ctx 'Start bis zum Umstieg-Dialog') { return }
        if (-not $dialog) {
            $Ctx.Problem = 'Umstieg-Dialog (Fenstertitel) erschien nicht innerhalb von 90 s'
            Write-StepRecord $Ctx 'kein-umstieg' $Ctx.Problem
        } else {
            $Ctx.LegacySeen = $true
            $Ctx.Reached = 'Umstieg-Dialog'
            $Ctx.LegacyText = 'Fenstertitel "{0}"' -f $dialog.Window.Text
            Start-Sleep -Seconds 1
            Write-StepRecord $Ctx 'umstieg' 'Umstieg-Dialog (Fenstertitel)'
            # Esc löst den einzigen Knopf mit RejectRole aus: "Neues Konto einrichten"
            $active = [OcProbe.Desk]::Activate($dialog.Window.Handle)
            $null = [OcProbe.Desk]::Press(0x1B, $false)
            Add-Timeline ('Taste Esc im Umstieg-Dialog = "Neues Konto einrichten" (Vordergrund: {0})' -f $active)
        }
    }

    $wizard = Wait-Ui $Ctx { $w = Find-TopWindow $Ctx.Process.Id $wizardTitlePattern; if ($w) { @{ Window = $w } } } 60
    if (Test-Crashed $Ctx 'Öffnen des Assistenten') { return }
    if (-not $wizard) {
        $Ctx.Problem = 'Assistent (Fenstertitel) erschien nicht innerhalb von 60 s'
        Write-StepRecord $Ctx 'kein-assistent' $Ctx.Problem
        return
    }
    $Ctx.Reached = 'Adressseite'
    Start-Sleep -Seconds 1
    Write-StepRecord $Ctx 'adressseite' 'Assistent (Fenstertitel)'

    foreach ($url in $Definition.Urls) {
        $logStart = (Read-SharedText $Ctx.Log).Length
        $active = [OcProbe.Desk]::Activate($wizard.Window.Handle)
        # das Adressfeld hat den Fokus (UrlPageController::buildPage, nach einer Meldung bleibt er dort)
        $null = [OcProbe.Desk]::Press(0x41, $true)
        $null = [OcProbe.Desk]::TypeText($url)
        Start-Sleep -Milliseconds 300
        Write-StepRecord $Ctx 'adresse' ('Adresse getippt: {0} (Vordergrund: {1})' -f $url, $active)
        $null = [OcProbe.Desk]::Press(0x0D, $false)
        Add-Timeline ('Taste Enter = "Anmelden" mit {0}' -f $url)
        $start = Get-Date
        $outcome = Wait-Ui $Ctx {
            $text = Read-SharedText $Ctx.Log
            $new = if ($text.Length -gt $logStart) { $text.Substring($logStart) } else { '' }
            if ($new -match 'Waiting for the browser on') { @{ State = 'anmeldung' } }
            elseif ($new -match 'OAuth2 token endpoint answered|Did not receive (bearer|WWW-Authenticate)|Failed to determine authentication type|Network error when resolving Url|is rejected|failed with SSL errors') { @{ State = 'warnung' } }
        } 150
        $seconds = [int]((Get-Date) - $start).TotalSeconds
        Start-Sleep -Seconds 2
        if (Test-Crashed $Ctx ('Enter ("Anmelden") mit {0} ({1} s danach)' -f $url, $seconds)) { return }
        if (-not $outcome) {
            $Ctx.Problem = 'kein Ergebnis im Protokoll {0} s nach Enter mit {1}' -f $seconds, $url
            Write-StepRecord $Ctx 'kein-ergebnis' $Ctx.Problem
            return
        }
        if ($outcome.State -eq 'warnung') {
            $Ctx.Warnings.Add(@{ Url = $url; Detail = 'laut Protokoll; Text siehe Foto (ohne UI Automation nicht lesbar)' })
            $Ctx.Reached = "Warnung für $url"
            Write-StepRecord $Ctx 'warnung' ('Meldung {0} s nach Enter mit {1}' -f $seconds, $url)
            continue
        }
        $Ctx.ReachedLogin = $true
        $Ctx.Reached = "Anmeldeseite für $url"
        Write-StepRecord $Ctx 'anmeldeseite' ('Anmeldeseite (Protokoll: wartet auf den Browser) {0} s nach Enter mit {1}' -f $seconds, $url)
        break
    }
    if (-not $Ctx.ReachedLogin) { return }

    $logStart = (Read-SharedText $Ctx.Log).Length
    Complete-Login $Ctx
    if (-not $Ctx.LoginDone -or $Ctx.Process.HasExited) { return }
    # Nach dem Rückruf prüft der Assistent Benutzer und Capabilities (UserInfoAdapter, FetchCapabilitiesAdapter) und
    # wechselt zur Erfolgsseite. Diese Anfragen stehen nicht im Protokoll (sync.httplogger loggt erst ab Warnung):
    # den Rückruf abwarten, dann 10 s für die Prüfungen; das Foto belegt die Seite
    $done = Wait-Ui $Ctx {
        $text = Read-SharedText $Ctx.Log
        $new = if ($text.Length -gt $logStart) { $text.Substring($logStart) } else { '' }
        if ($new -match 'Browser callback received') { @{ State = 'rueckruf' } }
    } 60
    if (-not $done) {
        if (Test-Crashed $Ctx 'Rückruf des Browsers') { return }
        $Ctx.Problem = 'kein "Browser callback received" im Protokoll innerhalb von 60 s nach der Browser-Anmeldung'
        Write-StepRecord $Ctx 'kein-rueckruf' $Ctx.Problem
        return
    }
    Start-Sleep -Seconds 10
    if (Test-Crashed $Ctx 'Prüfung des Kontos nach dem Rückruf (bis zur Erfolgsseite)') { return }
    $Ctx.SuccessPage = $true
    $Ctx.Reached = 'Erfolgsseite (Foto)'
    Write-StepRecord $Ctx 'erfolgsseite' 'Erfolgsseite erwartet: Rückruf im Protokoll, 10 s für die Prüfung des Kontos (siehe Foto)'
    # "Fertigstellen" hat auf der Erfolgsseite den Fokus (NewAccountWizardController::onPageChanged)
    $active = [OcProbe.Desk]::Activate($wizard.Window.Handle)
    $null = [OcProbe.Desk]::Press(0x0D, $false)
    $Ctx.Finished = $true
    $Ctx.Reached = 'Fertigstellen'
    Add-Timeline ('Taste Enter = "Fertigstellen" (Vordergrund: {0})' -f $active)
    Watch-AccountSetup $Ctx
}

# Belege nach dem Ende des Prozesses: Exit-Code, Ereignisprotokoll, Speicherabbild, Absturzprotokoll, Protokollende
function Get-CrashEvidence {
    param($Ctx)
    # WerFault schreibt das Speicherabbild noch, wenn der Prozess schon fort ist
    Start-Sleep -Seconds 3
    $deadline = (Get-Date).AddSeconds(180)
    while ((Get-Date) -lt $deadline -and @(Get-Process -Name 'WerFault', 'WerFaultSecure' -ErrorAction SilentlyContinue).Count -gt 0) { Start-Sleep -Seconds 1 }
    $code = $null
    try { $code = $Ctx.Process.ExitCode } catch { }
    $Ctx.ExitCode = $code
    $Ctx.Evidence.Add('Exit-Code: ' + (Format-ExitCode $code))

    $events = @()
    try {
        $events = @(Get-WinEvent -FilterHashtable @{ LogName = 'Application'; Id = 1000, 1001; StartTime = $Ctx.Since } -ErrorAction Stop |
                Where-Object { $_.Message -match 'owncloud' })
    } catch { }
    $eventLines = @(foreach ($event in $events) { 'Ereignis {0} ({1}, {2:HH:mm:ss}): {3}' -f $event.Id, $event.ProviderName, $event.TimeCreated, (Limit-Text $event.Message 2000) })
    foreach ($line in $eventLines) { $Ctx.Evidence.Add($line) }
    if ($eventLines.Count -eq 0) { $Ctx.Evidence.Add('Ereignisprotokoll: kein Application Error/Windows Error Reporting zu owncloud.online') }
    [IO.File]::WriteAllLines((Join-Path $OutDir ('{0}-ereignisse.txt' -f $Ctx.Name)), [string[]]$eventLines, (New-Object System.Text.UTF8Encoding $false))

    $dumps = @(Get-ChildItem -LiteralPath $dumpDir -Filter '*.dmp' -File -ErrorAction SilentlyContinue | Where-Object { $_.LastWriteTime -ge $Ctx.Since.AddSeconds(-5) -and -not $knownDumps.Contains($_.FullName) })
    foreach ($dump in $dumps) {
        $null = $knownDumps.Add($dump.FullName)
        $Ctx.Dumps.Add($dump.FullName)
        $Ctx.Evidence.Add(('Speicherabbild: {0} ({1:N0} MB)' -f $dump.Name, ($dump.Length / 1MB)))
    }
    if ($dumps.Count -eq 0) { $Ctx.Evidence.Add('Speicherabbild: keins geschrieben') }

    # Logger::dumpCrashLog: die letzten 20 Meldungen vor einem qFatal (ASSERT, ENFORCE)
    $crashLog = Join-Path ([IO.Path]::GetTempPath()) "$appName-crash.log"
    if (Copy-Protected $crashLog (Join-Path $OutDir ('{0}-crash.log' -f $Ctx.Name))) {
        $Ctx.Evidence.Add(('Absturzprotokoll des Clients: {0}-crash.log' -f $Ctx.Name))
    } else {
        $Ctx.Evidence.Add("Absturzprotokoll des Clients: keins ($crashLog)")
    }

    $lines = @((Read-SharedText $Ctx.Log) -split "`r?`n" | Where-Object { $_ })
    $fatal = @($lines | Where-Object { $_ -match 'ASSERT|ENFORCE|ENSURE|\[ fatal |\[ critical ' })
    foreach ($line in @($fatal | Select-Object -Last 8)) { $Ctx.Evidence.Add('Protokoll: ' + (Limit-Text (Protect-Text $line) 700)) }
    $Ctx.LogTail = @($lines | Select-Object -Last 60 | ForEach-Object { Protect-Text $_ })
    $Ctx.Assert = @($fatal | Where-Object { $_ -match 'ASSERT|ENFORCE|ENSURE' } | Select-Object -Last 1 | ForEach-Object { Limit-Text (Protect-Text $_) 700 })
}

# cdb.exe: aus dem Windows SDK des Runners, sonst aus dem WinDbg-Paket entpackt (ohne Installation)
function Get-Cdb {
    if ($script:cdbPath) { return $script:cdbPath }
    if ($script:cdbSource) { return $null }
    foreach ($candidate in (Join-Path ${env:ProgramFiles(x86)} 'Windows Kits\10\Debuggers\x64\cdb.exe'), (Join-Path $env:ProgramFiles 'Windows Kits\10\Debuggers\x64\cdb.exe')) {
        if (Test-Path -LiteralPath $candidate -PathType Leaf) {
            $script:cdbPath = $candidate
            $script:cdbSource = 'Windows SDK des Runners'
            return $candidate
        }
    }
    $dir = Join-Path $rawRoot 'windbg'
    try {
        New-Item -ItemType Directory -Force -Path $dir | Out-Null
        $installer = Join-Path $dir 'windbg.appinstaller'
        & curl.exe -fsSL --retry 3 -o $installer 'https://aka.ms/windbg/download'
        if ($LASTEXITCODE -ne 0) { throw "appinstaller nicht ladbar (curl $LASTEXITCODE)" }
        [xml] $xml = Get-Content -LiteralPath $installer -Raw
        $bundleUri = [string]$xml.AppInstaller.MainBundle.Uri
        if (-not $bundleUri) { throw 'MainBundle fehlt im appinstaller' }
        $bundle = Join-Path $dir 'windbg.msixbundle'
        & curl.exe -fsSL --retry 3 -o $bundle $bundleUri
        if ($LASTEXITCODE -ne 0) { throw "Paket nicht ladbar (curl $LASTEXITCODE)" }
        $bundleDir = Join-Path $dir 'bundle'
        New-Item -ItemType Directory -Force -Path $bundleDir | Out-Null
        & tar.exe -xf $bundle -C $bundleDir
        $msix = @(Get-ChildItem -LiteralPath $bundleDir -Filter '*.msix' -File | Sort-Object { $_.Name -notmatch 'x64|amd64' }) | Select-Object -First 1
        if (-not $msix) { throw 'kein msix im Paket' }
        $packageDir = Join-Path $dir 'paket'
        New-Item -ItemType Directory -Force -Path $packageDir | Out-Null
        & tar.exe -xf $msix.FullName -C $packageDir
        $cdb = @(Get-ChildItem -LiteralPath $packageDir -Filter 'cdb.exe' -File -Recurse | Sort-Object { $_.FullName -notmatch '\\amd64\\' }) | Select-Object -First 1
        if (-not $cdb) { throw 'cdb.exe nicht im Paket' }
        $script:cdbPath = $cdb.FullName
        $script:cdbSource = "WinDbg-Paket ($($msix.Name))"
        return $script:cdbPath
    } catch {
        $script:cdbSource = 'nicht verfügbar: ' + $_.Exception.Message
        return $null
    }
}

function Invoke-Cdb {
    param([string] $Dump, [string] $Commands, [string] $OutFile)
    $cdb = Get-Cdb
    if (-not $cdb) { return '' }
    $symbols = Join-Path $rawRoot 'symbole'
    New-Item -ItemType Directory -Force -Path $symbols | Out-Null
    $info = New-Object System.Diagnostics.ProcessStartInfo
    $info.FileName = $cdb
    $info.Arguments = '-z "{0}" -y "srv*{1}*https://msdl.microsoft.com/download/symbols" -lines -c "{2}"' -f $Dump, $symbols, $Commands
    $info.UseShellExecute = $false
    $info.CreateNoWindow = $true
    $info.RedirectStandardOutput = $true
    $info.RedirectStandardError = $true
    $process = [System.Diagnostics.Process]::Start($info)
    $stdout = $process.StandardOutput.ReadToEndAsync()
    $stderr = $process.StandardError.ReadToEndAsync()
    if (-not $process.WaitForExit(12 * 60 * 1000)) { try { $process.Kill() } catch { } }
    $process.WaitForExit()
    $text = Protect-Text ($stdout.Result + $stderr.Result)
    if ($OutFile) { [IO.File]::WriteAllText($OutFile, $text, (New-Object System.Text.UTF8Encoding $false)) }
    return $text
}

# das Wichtigste aus !analyze -v für den Bericht
function Get-CdbSummary {
    param([string] $Text)
    $lines = @($Text -split "`r?`n")
    $keys = '^(ExceptionCode|EXCEPTION_CODE_STR|EXCEPTION_PARAMETER1|FAULTING_IP|FAULTING_SOURCE_LINE|FAULTING_SOURCE_FILE|FAULTING_THREAD|PROCESS_NAME|MODULE_NAME|IMAGE_NAME|SYMBOL_NAME|FAILURE_BUCKET_ID|BUCKET_ID_FUNC_OFFSET|ERROR_CODE|EXCEPTION_RECORD):'
    $summary = New-Object 'System.Collections.Generic.List[string]'
    for ($i = 0; $i -lt $lines.Count; $i++) {
        if ($lines[$i] -match $keys) {
            $summary.Add($lines[$i].Trim())
            if ($lines[$i] -match '^FAULTING_IP:' -and $i + 1 -lt $lines.Count) { $summary.Add('  ' + $lines[$i + 1].Trim()) }
        }
    }
    $stack = -1
    for ($i = 0; $i -lt $lines.Count; $i++) {
        if ($lines[$i] -match '^STACK_TEXT:') { $stack = $i; break }
    }
    if ($stack -ge 0) {
        $summary.Add('STACK_TEXT:')
        for ($i = $stack + 1; $i -lt $lines.Count -and $lines[$i].Trim(); $i++) {
            if ($summary.Count -gt 70) { break }
            # nur die Funktion: hinter dem letzten " : " (davor Stack- und Rücksprungadresse, Argumente)
            $frame = $lines[$i]
            $cut = $frame.LastIndexOf(' : ')
            if ($cut -ge 0) { $frame = $frame.Substring($cut + 3) }
            $summary.Add('  ' + $frame.Trim())
        }
    }
    return , $summary.ToArray()
}

# Aufrufstelle im Client eingrenzen: ohne PDB nennt cdb nur den nächstgelegenen Export. Die Befehle vor den
# Rücksprungadressen in owncloudGui zeigen das Aufrufziel (Import) und die geladenen Zeichenketten: direkt
# (QStringLiteral, UTF-16) oder über ein QString-Objekt (d, ptr, size - der Text liegt hinter ptr).
function Get-CallerDetail {
    param([string] $Dump, [string] $Text, [string] $OutFile)
    $frames = @([regex]::Matches($Text, '(?m)^[0-9a-f`]+\s+[0-9a-f`]+\s+:.*:\s+(owncloudGui!\S+)') | ForEach-Object { $_.Groups[1].Value } | Select-Object -Unique -First 2)
    if ($frames.Count -eq 0) { return @('Aufrufer: kein Rahmen in owncloudGui im Stack') }
    $disassembly = Invoke-Cdb $Dump ((@($frames | ForEach-Object { ".echo ===AUFRUFER $_===; ub $_ L48; u $_ L4" }) -join '; ') + '; q') ''
    $targets = @([regex]::Matches($disassembly, 'lea\s+\w+,\[[^\]]*\(([0-9a-f`]{8,17})\)\]') | ForEach-Object { $_.Groups[1].Value } | Select-Object -Unique -First 40)
    $direct = ''
    $indirect = ''
    if ($targets.Count -gt 0) {
        $direct = Invoke-Cdb $Dump ((@($targets | ForEach-Object { "du $_ L40; dq $_ L3" }) -join '; ') + '; q') ''
        # zweites Wort eines QString-Objekts: Zeiger auf den Text
        $pointers = @([regex]::Matches($direct, '(?m)^[0-9a-f`]{17}\s+[0-9a-f`]{17}\s+([0-9a-f`]{17})') | ForEach-Object { $_.Groups[1].Value } | Where-Object { $_ -notmatch '^0{8}`0{8}$' } | Select-Object -Unique -First 40)
        if ($pointers.Count -gt 0) {
            $indirect = Invoke-Cdb $Dump ((@($pointers | ForEach-Object { "du $_ L60" }) -join '; ') + '; q') ''
        }
    }
    [IO.File]::WriteAllText($OutFile, ($disassembly + "`r`n===ZEICHENKETTEN (lea)===`r`n" + $direct + "`r`n===ZEICHENKETTEN (QString)===`r`n" + $indirect), (New-Object System.Text.UTF8Encoding $false))
    $calls = @([regex]::Matches($disassembly, 'call\s+qword ptr \[[^\]]*?(?:__imp_|!)([^\s\]\(]+)') | ForEach-Object { $_.Groups[1].Value } | Select-Object -Unique)
    $strings = @([regex]::Matches($direct + "`n" + $indirect, '(?m)^[0-9a-f`]+\s+"([ -~]{3,})"') | ForEach-Object { $_.Groups[1].Value } | Select-Object -Unique)
    return @(
        ('Aufrufer: {0}' -f ($frames -join ', ')),
        ('Aufrufe dort: {0}' -f $(if ($calls.Count) { Limit-Text ($calls -join ', ') 900 } else { '-' })),
        ('Zeichenketten dort: {0}' -f $(if ($strings.Count) { Limit-Text (($strings | ForEach-Object { '"' + $_ + '"' }) -join ', ') 900 } else { '-' }))
    )
}

# Speicherabbilder mit cdb auswerten. Ins Artefakt nur, was keine Tokens enthalten kann: das vollständige
# Abbild, wenn der Client noch keine Anmeldung hatte, sonst ein Auszug ohne Heap (.dump /m)
function Invoke-DumpAnalysis {
    param($Ctx)
    $number = 0
    foreach ($dump in $Ctx.Dumps) {
        $number++
        $name = '{0}-cdb{1}' -f $Ctx.Name, $(if ($number -gt 1) { "-$number" } else { '' })
        Write-Host "== Speicherabbild auswerten: $(Split-Path -Leaf $dump)"
        $text = Invoke-Cdb $dump '!analyze -v; .echo ===ECXR===; .ecxr; kn 80; .echo ===THREAD0===; ~0s; kn 50; .echo ===ALLE===; ~*kn 30; .echo ===MODULE===; lmo; q' (Join-Path $OutDir "$name.txt")
        if (-not $text) {
            $Ctx.Analysis.Add("cdb $script:cdbSource")
        } else {
            $Ctx.Analysis.Add("cdb ($script:cdbSource): $name.txt")
            foreach ($line in (Get-CdbSummary $text)) { $Ctx.Analysis.Add($line) }
            try {
                foreach ($line in (Get-CallerDetail $dump $text (Join-Path $OutDir "$name-aufrufer.txt"))) { $Ctx.Analysis.Add($line) }
            } catch {
                $Ctx.Analysis.Add('Aufrufer nicht ermittelbar: ' + $_.Exception.Message)
            }
        }
        if (-not $Ctx.TokensPossible) {
            Copy-Item -LiteralPath $dump -Destination (Join-Path $OutDir ('{0}-{1}' -f $Ctx.Name, (Split-Path -Leaf $dump)))
            $Ctx.Analysis.Add('Speicherabbild vollständig im Artefakt (der Client hatte noch keine Tokens)')
        } elseif ($script:cdbPath -and $OutDir -notmatch '\s') {
            $mini = Join-Path $OutDir "$name-auszug.dmp"
            $null = Invoke-Cdb $dump ".dump /m $mini; q" ''
            $Ctx.Analysis.Add($(if (Test-Path -LiteralPath $mini) { "Auszug ohne Heap im Artefakt: $(Split-Path -Leaf $mini)" } else { 'Auszug nicht geschrieben' }))
        } else {
            $Ctx.Analysis.Add('Speicherabbild nicht im Artefakt (Tokens möglich, kein cdb für einen Auszug)')
        }
    }
}

function Add-AssistentChecks {
    param($Ctx, [hashtable] $Definition)
    $id = $Ctx.Name.ToUpperInvariant()
    if ($Definition.Legacy) {
        if ($Ctx.LegacySeen) {
            Add-Check "$id.1" 'Umstieg-Dialog erschienen' 'OK' $Ctx.LegacyText ''
        } else {
            Add-Check "$id.1" 'Umstieg-Dialog erschienen' $(if ($Ctx.Crashed) { 'NICHT GEPRÜFT' } else { 'FEHLER' }) $(if ($Ctx.Crashed) { 'Prozess vorher beendet' } else { 'nicht erschienen, obwohl die Konfiguration des offiziellen Clients ein Konto hat' }) ''
        }
    }
    foreach ($warning in $Ctx.Warnings) {
        Add-Check "$id.2" ('Meldung im Assistenten für {0}' -f $warning.Url) 'INFO' $warning.Detail ''
    }
    $loginUrl = $Definition.Urls[-1]
    if ($Ctx.ReachedLogin) {
        Add-Check "$id.3" "Anmeldeseite für $loginUrl erreicht" 'OK' '' ''
    } else {
        Add-Check "$id.3" "Anmeldeseite für $loginUrl erreicht" 'FEHLER' $(if ($Ctx.Crashed) { "Prozess beendet: $($Ctx.CrashStep)" } else { $Ctx.Problem }) ''
    }
    if ($Ctx.ReachedLogin) {
        $status = if ($Ctx.LoginDone) { 'OK' } else { 'FEHLER' }
        Add-Check "$id.4" 'Browser-Anmeldung (assistent-anmeldung.py) beim Client abgeliefert' $status $(if ($Ctx.LoginSummary) { $Ctx.LoginSummary } else { $Ctx.Problem }) ''
    }
    if ($Ctx.LoginDone) {
        Add-Check "$id.5" 'Erfolgsseite nach der Anmeldung' $(if ($Ctx.SuccessPage) { 'OK' } else { 'FEHLER' }) $(if ($Ctx.SuccessPage) { '' } elseif ($Ctx.Crashed) { "Prozess beendet: $($Ctx.CrashStep)" } else { $Ctx.Problem }) ''
    }
    if ($Ctx.Finished) {
        $detail = 'Konto gespeichert: {0}; Ordner eingerichtet: {1}' -f $(if ($Ctx.AccountSaved) { 'ja' } else { 'nein' }), $(if ($Ctx.FolderAdded) { 'ja' } else { 'nein' })
        Add-Check "$id.6" 'Konto nach "Fertigstellen" angelegt' $(if ($Ctx.AccountSaved -and -not $Ctx.Crashed) { 'OK' } else { 'FEHLER' }) $detail ''
    }
    if ($Ctx.Crashed) {
        # die Meldung des qFatal zuerst: die Tabelle der Zusammenfassung kürzt lange Zellen
        $assert = @($Ctx.Assert | ForEach-Object { ($_ -replace '^.*?\]:\s*', '') })
        $detail = @("Absturz während: $($Ctx.CrashStep)") + $assert + @($Ctx.Evidence | Select-Object -First 6)
        Add-Check $id ('Durchgang {0}: kein Absturz' -f $Ctx.Name) 'FEHLER' ($detail -join '; ') ''
    } else {
        Add-Check $id ('Durchgang {0}: kein Absturz' -f $Ctx.Name) 'OK' ('bis "{0}"{1}' -f $Ctx.Reached, $(if ($Ctx.Problem) { '; ' + $Ctx.Problem } else { '' })) ''
    }
}

function Invoke-AssistentPass {
    param([hashtable] $Definition, [string] $Exe)
    $name = $Definition.Name
    $ctx = @{
        Name = $name; Uia = ($Definition.Mode -eq 'uia'); Process = $null; Log = (Join-Path $rawRoot "$name-client.log"); Since = (Get-Date); Step = 0
        Shots = New-Object 'System.Collections.Generic.List[string]'
        Dumps = New-Object 'System.Collections.Generic.List[string]'
        Evidence = New-Object 'System.Collections.Generic.List[string]'
        Analysis = New-Object 'System.Collections.Generic.List[string]'
        Warnings = New-Object 'System.Collections.Generic.List[object]'
        LogTail = @(); Assert = @()
        Crashed = $false; CrashStep = ''; ExitCode = $null; Problem = ''; Reached = 'Start'; LegacyText = ''; LoginSummary = ''
        LegacySeen = $false; ReachedLogin = $false; TokensPossible = $false; LoginDone = $false; SuccessPage = $false; Finished = $false
        AccountSaved = $false; FolderAdded = $false
    }
    Write-Host "== Durchgang ${name}: $($Definition.Title)"
    Write-Annotation 'notice' "Durchgang ${name}: $($Definition.Title)"
    Add-Timeline "== Durchgang ${name}: $($Definition.Title)"
    Reset-ClientState
    Write-ClientConfig
    Set-LegacyConfig $Definition.Legacy
    if ($Definition.Official -and $OfficialMsi) {
        $running = (Get-OfficialProcess) -or (Start-OfficialClient)
        Add-Timeline ('Offizieller Client läuft: {0}' -f $(if ($running) { 'ja' } else { 'nein' }))
        # der offizielle Client darf seine Datei beim Start umschreiben; der Umstieg-Dialog braucht das Konto darin
        if ($Definition.Legacy -and -not (Test-LegacyConfig)) {
            Set-LegacyConfig $true
            Add-Timeline 'Konfiguration des offiziellen Clients nach dessen Start erneut geschrieben (Konto fehlte)'
        }
    } else {
        Stop-OfficialClient
    }
    if (Test-Path -LiteralPath $ctx.Log) { Remove-Item -LiteralPath $ctx.Log -Force }
    $ctx.Since = Get-Date
    $ctx.Process = Start-Process -FilePath $Exe -ArgumentList @('--logfile', ('"{0}"' -f $ctx.Log), '--logdebug', '--logflush') -PassThru
    # ohne diesen Zugriff liefert ExitCode nach dem Ende $null
    $null = $ctx.Process.Handle
    Add-Timeline ('Prüfling gestartet, PID {0}' -f $ctx.Process.Id)
    try {
        if ($ctx.Uia) { Invoke-UiaFlow $ctx $Definition } else { Invoke-KeyboardFlow $ctx $Definition }
    } catch {
        $ctx.Problem = 'Fehler der Probe: {0} (Zeile {1})' -f $_.Exception.Message, $_.InvocationInfo.ScriptLineNumber
        Add-Timeline $ctx.Problem
    }
    if ($ctx.Process.HasExited) {
        $null = Test-Crashed $ctx $(if ($ctx.Reached) { "nach: $($ctx.Reached)" } else { 'Ablauf' })
        Get-CrashEvidence $ctx
        foreach ($line in $ctx.Evidence) { Add-Timeline "Beleg: $line" }
    } else {
        $null = Invoke-Step "Prüfling beenden (--quit, $name)" $Exe '--quit'
        if ($ctx.Process.WaitForExit(90 * 1000)) {
            Add-Timeline ('nach --quit beendet, Exit-Code {0}' -f (Format-ExitCode $ctx.Process.ExitCode))
        } else {
            $null = & taskkill.exe /PID $ctx.Process.Id /T /F
            Add-Timeline 'nach --quit nicht in 90 s beendet, abgebrochen'
        }
    }
    Stop-Browsers
    # Protokoll ohne Code des Rückrufs, Tokens und Zugang ins Artefakt
    $null = Copy-Protected $ctx.Log (Join-Path $OutDir "$name-client.log")
    $text = Read-SharedText $ctx.Log
    $ctx.AccountSaved = $text.Contains('Saving account')
    $ctx.FolderAdded = $text.Contains('Adding folder to Folder Map')
    Add-AssistentChecks $ctx $Definition
    return $ctx
}

function Invoke-Assistent {
    Initialize-Desktop
    New-Item -ItemType Directory -Force -Path $rawRoot, $dumpDir | Out-Null
    Add-Check 'A0' 'Speicherabbilder bei Absturz (WER LocalDumps)' 'INFO' (Enable-LocalDumps) ''
    $bounds = [System.Windows.Forms.SystemInformation]::VirtualScreen
    $account = if ($loginUser -and $loginPass) { 'vorhanden' } else { 'fehlt - die Browser-Anmeldung unterbleibt' }
    # die Adresse aus dem Secret selbst erscheint nirgends
    $loginHost = $null
    $sameServer = '-'
    if ($loginServer -and [uri]::TryCreate($loginServer, [UriKind]::Absolute, [ref] $loginHost)) {
        $sameServer = if ($loginHost.Host -eq ([uri]$ServerUrl).Host) { 'ja' } else { 'nein (eigene Adresse, dieselbe Instanz angenommen)' }
    }
    Add-Check 'A1' 'Umgebung' 'INFO' ('Bildschirm {0}x{1}; Testkonto {2}; Server des Testkontos = {3}: {4}; login.py: {5}; Python: {6}' -f $bounds.Width, $bounds.Height, $account, ([uri]$ServerUrl).Host, $sameServer,
        $(if ($LoginScript -and (Test-Path -LiteralPath $LoginScript)) { 'vorhanden' } else { 'fehlt' }), $(if (Get-Python) { Get-Python } else { 'fehlt' })) ''

    if ($OfficialMsi) {
        $log = Join-Path $OutDir 'offiziell-installation.log'
        $run = Invoke-Step 'Offizieller Client 6.0.3 (MSI)' 'msiexec.exe' ('/i "{0}" /qn /norestart DO_NOT_SCHEDULE_REBOOT=1 SKIPAUTOUPDATE=1 /l*v "{1}"' -f $OfficialMsi, $log)
        Test-Run $run 'A2' 'Offizieller Client 6.0.3 still installiert'
    }
    Install-Candidate 'A3'
    $exe = Find-ClientExe
    if (-not $exe) { throw 'Programmdatei des Prüflings nicht gefunden' }
    Add-Check 'A4' 'Prüfling' 'INFO' ('{0}, Version {1}, Signatur {2}' -f $exe, (Get-VersionWithBuild $exe), (Get-SignatureNote $exe)) ''
    Stop-Client

    # p1 zuerst: nur Tastatur, bevor diese Sitzung UI Automation benutzt - wie ein Tester ohne Hilfsprogramme
    $passes = @(
        @{ Name = 'p1'; Mode = 'tastatur'; Legacy = $true; Official = $false; Urls = @($WrongServerUrl, $ServerUrl); Advanced = $false
            Title = 'Umstieg-Dialog, "Neues Konto einrichten" (Esc), erst ' + $WrongServerUrl + ' (Warnung), dann ' + $ServerUrl + ', Anmeldung, Fertigstellen - nur Tastatur, ohne UI Automation im Client' },
        @{ Name = 'p2'; Mode = 'uia'; Legacy = $true; Official = $true; Urls = @($ServerUrl); Advanced = $true
            Title = 'Umstieg-Dialog (offizieller Client läuft), "Neues Konto einrichten", gleich ' + $ServerUrl + ', Anmeldung, Erweitert, Fertigstellen (UI Automation)' },
        @{ Name = 'p3'; Mode = 'uia'; Legacy = $true; Official = $false; Urls = @($WrongServerUrl, $ServerUrl); Advanced = $false
            Title = 'Umstieg-Dialog, "Neues Konto einrichten", erst ' + $WrongServerUrl + ' (Warnung lesen und vermessen), dann ' + $ServerUrl + ', Anmeldung, Fertigstellen (UI Automation)' }
    )
    $contexts = New-Object 'System.Collections.Generic.List[object]'
    foreach ($definition in $passes) {
        $contexts.Add((Invoke-AssistentPass $definition $exe))
        Write-Report $null
    }
    Stop-Client
    Stop-OfficialClient

    foreach ($ctx in $contexts) {
        if ($ctx.Dumps.Count -eq 0) { continue }
        Invoke-DumpAnalysis $ctx
        Add-Check ('{0}.9' -f $ctx.Name.ToUpperInvariant()) 'Auswertung des Speicherabbilds' 'INFO' (($ctx.Analysis | Select-Object -First 12) -join '; ') ''
        Add-List ('{0}: Auswertung des Speicherabbilds' -f $ctx.Name) $ctx.Analysis.ToArray()
    }
    foreach ($ctx in $contexts) {
        if ($ctx.Crashed) {
            Add-List ('{0}: Belege des Absturzes' -f $ctx.Name) $ctx.Evidence.ToArray()
            Add-List ('{0}: Ende des Protokolls (ohne Code und Tokens)' -f $ctx.Name) $ctx.LogTail
        }
    }
    Add-List 'Ablauf (alle Durchgänge)' $timeline.ToArray()
    [IO.File]::WriteAllLines((Join-Path $OutDir 'assistent-ablauf.txt'), [string[]]$timeline.ToArray(), (New-Object System.Text.UTF8Encoding $false))
}

# ---------------------------------------------------------------------------
# Bericht
# ---------------------------------------------------------------------------

# Dateiname für den Bericht; der Bericht entsteht auch nach einem Abbruch
# wegen fehlender Eingaben
function Get-Leaf {
    param([string] $Path)
    if (-not $Path) { return '-' }
    return [IO.Path]::GetFileName($Path)
}

function ConvertTo-Cell {
    param([string] $Text)
    if (-not $Text) { return '' }
    $cell = ($Text -replace '\r?\n', ' ') -replace '\|', '\|'
    if ($cell.Length -gt 400) { $cell = $cell.Substring(0, 400) + ' …' }
    return $cell
}

function Write-Report {
    param([string] $Abort)
    $failed = @($checks | Where-Object { $_.ergebnis -eq 'FEHLER' }).Count
    $warned = @($checks | Where-Object { $_.ergebnis -eq 'WARNUNG' }).Count
    $verdict = if ($Abort) { 'Probe abgebrochen' } elseif (-not $finished) { "unvollständig, bisher $failed Fehler" } elseif ($failed -gt 0) { "$failed Fehler" } else { 'keine Fehler' }
    $os = [Environment]::OSVersion.VersionString
    try { $os = (Get-CimInstance -ClassName Win32_OperatingSystem).Caption } catch { $os = [Environment]::OSVersion.VersionString }

    $head = New-Object 'System.Collections.Generic.List[string]'
    $head.Add("## Install-Probe: $Scenario")
    $head.Add('')
    $head.Add(('- Prüfling: `{0}`, Version {1}, SHA256 `{2}`' -f (Get-Leaf $Setup), $(if ($ExpectedVersion) { $ExpectedVersion } else { 'nicht angegeben' }), $setupHash))
    if ($Scenario -like 'upgrade*') { $head.Add(('- Vorversion: `{0}`' -f (Get-Leaf $PreviousSetup))) }
    if ($Scenario -eq 'koexistenz') { $head.Add(('- Offizieller Client: `{0}`' -f (Get-Leaf $OfficialMsi))) }
    if ($Scenario -eq 'assistent') {
        $head.Add(('- Server: `{0}` (mit OAuth2), `{1}` (ohne OAuth2); offizieller Client: `{2}`' -f $ServerUrl, $WrongServerUrl, (Get-Leaf $OfficialMsi)))
        $head.Add('- Bildschirmfotos (foto-*.png), Ablauf (assistent-ablauf.txt), Protokolle des Clients (p*-client.log, ohne Code und Tokens) und Belege eines Absturzes liegen im Artefakt')
    }
    $head.Add(('- Rechner: {0}, Windows PowerShell {1}' -f $os, $PSVersionTable.PSVersion))
    $head.Add(('- Ergebnis: **{0}**, {1} Warnungen' -f $verdict, $warned))
    if ($Abort) { $head.Add("- Abbruch: $Abort") }
    $head.Add('')
    $head.Add('| ID | Prüfung | Ergebnis | Audit | Detail |')
    $head.Add('|---|---|---|---|---|')
    foreach ($check in $checks) {
        $head.Add(('| {0} | {1} | {2} | {3} | {4} |' -f $check.id, (ConvertTo-Cell $check.pruefung), $check.ergebnis, $check.audit, (ConvertTo-Cell $check.detail)))
    }
    $head.Add('')
    $head.Add('| Schritt | Exit-Code | Dauer | Befehl |')
    $head.Add('|---|---|---|---|')
    foreach ($run in $runs) {
        $code = if ($run.zeitueberschreitung) { 'Zeitüberschreitung' } else { [string]$run.exitcode }
        $head.Add(('| {0} | {1} | {2} s | `{3}` |' -f $run.schritt, $code, $run.sekunden, (ConvertTo-Cell $run.befehl)))
    }

    $report = New-Object 'System.Collections.Generic.List[string]'
    $report.AddRange($head)
    if ($lists.Count -gt 0) {
        $report.Add('')
        $report.Add('## Listen')
        $report.AddRange($lists)
    }
    $report.Add('')
    $report.Add('Schnappschüsse (NN-*.json) und vollständige Vergleiche (diff-*.md) liegen neben diesem Bericht.')

    $utf8 = New-Object System.Text.UTF8Encoding $false
    [IO.File]::WriteAllText((Join-Path $OutDir 'zusammenfassung.md'), ($head -join "`n") + "`n", $utf8)
    [IO.File]::WriteAllText((Join-Path $OutDir 'bericht.md'), ($report -join "`n") + "`n", $utf8)
    $result = [ordered]@{
        szenario   = $Scenario
        pruefling  = Get-Leaf $Setup
        version    = $ExpectedVersion
        ergebnis   = $verdict
        abbruch    = $Abort
        pruefungen = $checks.ToArray()
        schritte   = $runs.ToArray()
    }
    [IO.File]::WriteAllText((Join-Path $OutDir 'ergebnis.json'), ($result | ConvertTo-Json -Depth 5), $utf8)
}

# ---------------------------------------------------------------------------
# Ablauf
# ---------------------------------------------------------------------------

$setupHash = '-'
$abort = $null
# bis zum Ende tragen Zwischenberichte den Vermerk "unvollständig"
$finished = $false
try {
    $principal = New-Object Security.Principal.WindowsPrincipal ([Security.Principal.WindowsIdentity]::GetCurrent())
    if (-not $principal.IsInRole([Security.Principal.WindowsBuiltInRole]::Administrator)) { throw 'nicht erhöht gestartet' }
    $required = [ordered]@{ Setup = $Setup }
    if ($Scenario -like 'upgrade*') { $required['PreviousSetup'] = $PreviousSetup }
    if ($Scenario -eq 'koexistenz') { $required['OfficialMsi'] = $OfficialMsi }
    foreach ($name in $required.Keys) {
        $path = $required[$name]
        if (-not $path -or -not (Test-Path -LiteralPath $path -PathType Leaf)) { throw "-$name fehlt oder ist keine Datei: '$path'" }
    }
    $setupHash = (Get-FileHash -LiteralPath $Setup -Algorithm SHA256).Hash
    switch ($Scenario) {
        'frisch' { Invoke-Fresh }
        'koexistenz' { Invoke-Coexistence }
        'upgrade' { Invoke-Upgrade }
        'upgrade-beta1' { Invoke-Upgrade }
        'upgrade-beta2' { Invoke-Upgrade }
        'deinstall-policies' { Invoke-PolicyScenario }
        'assistent' { Invoke-Assistent }
    }
} catch {
    $abort = '{0} (Zeile {1})' -f $_.Exception.Message, $_.InvocationInfo.ScriptLineNumber
    Write-Host "Probe abgebrochen: $abort"
} finally {
    $finished = $true
    Write-Report $abort
}

if ($abort) { exit 2 }
if (@($checks | Where-Object { $_.ergebnis -eq 'FEHLER' }).Count -gt 0) { exit 1 }
exit 0
