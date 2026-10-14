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
                  unserer muss unverändert bleiben.
      upgrade     Vorversion (-PreviousSetup) mit Client-Spuren -> Setup ->
                  genau eine Installation, Spuren erhalten ->
                  Deinstallation -> Rückstände gegen den Ausgang

    Client-Spuren, nachgebildet nach dem Client-Code (Geheimnis = Zufallswert,
    wird nie ausgegeben): Anmeldeinformation <app>_credentials:<host>:<uuid>:
    http/oauthtoken (CredentialManager), Einstellungen in %APPDATA%\<app>
    (ConfigFile::configPath), Cache in %LOCALAPPDATA%\<app>, Autostart in HKCU
    Run (Utility::setLaunchOnStartup), Eintrag im Explorer-Navigationsbereich
    (NavigationPaneHelper) und eine Nutzerdatei im Sync-Ordner. Nach der
    Deinstallation müssen Anmeldedaten, Einstellungen, Cache, Autostart und
    Explorer-Eintrag weg sein, die Nutzerdatei muss bleiben (Entscheidung E4).
    CfApi-Sync-Root, Windows-Search-Regeln und Schnellzugriff lassen sich ohne
    laufenden Client mit Konto nicht nachbilden und werden als "nicht geprüft"
    gemeldet.

    Ergebnisse in -OutDir: Schnappschüsse (NN-*.json), Vergleiche (diff-*.md),
    bericht.md, zusammenfassung.md (kurz, für die Job-Zusammenfassung) und
    ergebnis.json. Exit-Code: 0 = keine Fehler, 1 = mindestens ein Fehler,
    2 = Probe abgebrochen.

.PARAMETER Scenario
    frisch, koexistenz oder upgrade.

.PARAMETER Setup
    Setup des Prüflings (*-Setup.exe aus dem CI-Artefakt windows-cl-x86_64).

.PARAMETER OutDir
    Ordner für Schnappschüsse und Berichte. Nicht unter den erfassten Orten
    anlegen (Programme, Benutzerprofil, %TEMP%), z. B. unter RUNNER_TEMP.

.PARAMETER PreviousSetup
    upgrade: Setup der Vorversion.

.PARAMETER OfficialMsi
    koexistenz: MSI des offiziellen Clients 6.0.3.

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
    [ValidateSet('frisch', 'koexistenz', 'upgrade')]
    [string] $Scenario,

    [Parameter(Mandatory = $true)]
    [string] $Setup,

    [Parameter(Mandatory = $true)]
    [string] $OutDir,

    [string] $PreviousSetup,

    [string] $OfficialMsi,

    [string] $ExpectedVersion,

    [ValidateRange(1, 120)]
    [int] $TimeoutMinutes = 8,

    [switch] $AllowLocal
)

$ErrorActionPreference = 'Stop'
Set-StrictMode -Version 2.0

if (-not $AllowLocal -and $env:GITHUB_ACTIONS -ne 'true') {
    [Console]::Error.WriteLine('probe.ps1 installiert und deinstalliert Software und legt Client-Spuren an. Nur auf einer Wegwerf-Maschine ausführen (CI-Runner oder VM-Snapshot); außerhalb von GitHub Actions nur mit -AllowLocal.')
    exit 2
}

$appName = 'owncloud.online'
# gehört eindeutig zu unserem Client
$ownPattern = 'owncloud\.online'
# gehört zu einem der beiden Clients: Namen, Pfade sowie die mit dem
# offiziellen Client geteilten COM-Klassen und Overlay-Namen (Audit H3)
$clientPattern = 'owncloud|OCOverlays|OCContextMenu|\bOC(Error|OK|Sync|Warning)(Shared)?\b|0960F09[0-7]-F328-48A3-B746-276B1E3C3722|841A0AAD-AA11-4B50-84D9-7F8E727D77D7'
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

$checks = New-Object 'System.Collections.Generic.List[object]'
$runs = New-Object 'System.Collections.Generic.List[object]'
$lists = New-Object 'System.Collections.Generic.List[string]'
$snapshotCount = 0

# Client-Spuren; Schlüssel = Name der Spur in den Prüfungen
$trace = @{
    Credential = '{0}_credentials:probe.invalid:{1}:http/oauthtoken' -f $appName, [guid]::NewGuid()
    ConfigDir  = Join-Path ([Environment]::GetFolderPath('ApplicationData')) $appName
    CacheDir   = Join-Path ([Environment]::GetFolderPath('LocalApplicationData')) $appName
    NavClsid   = '{' + [guid]::NewGuid().ToString().ToUpperInvariant() + '}'
    SyncDir    = Join-Path $env:USERPROFILE $appName
    UserHash   = $null
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
    # Ereignis 1074: ein Prozess hat Neustart oder Herunterfahren ausgelöst
    $restarts = 0
    try {
        $restarts = @(Get-WinEvent -FilterHashtable @{ LogName = 'System'; Id = 1074; StartTime = $start } -ErrorAction Stop).Count
    } catch {
        $restarts = 0
    }
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

function Find-ClientExe {
    foreach ($root in @($programFiles, ${env:ProgramFiles(x86)}) | Where-Object { $_ } | Select-Object -Unique) {
        foreach ($dir in @(Get-ChildItem -LiteralPath $root -Directory -ErrorAction SilentlyContinue | Where-Object { $_.Name -match $ownPattern })) {
            $exe = Get-ChildItem -LiteralPath $dir.FullName -Recurse -File -Filter "$appName.exe" -ErrorAction SilentlyContinue | Select-Object -First 1
            if ($exe) { return $exe.FullName }
        }
    }
    return $null
}

function Get-OwnUninstallCommand {
    foreach ($root in 'HKLM:\SOFTWARE\Microsoft\Windows\CurrentVersion\Uninstall',
                      'HKLM:\SOFTWARE\WOW6432Node\Microsoft\Windows\CurrentVersion\Uninstall',
                      'HKCU:\Software\Microsoft\Windows\CurrentVersion\Uninstall') {
        if (-not (Test-Path -LiteralPath $root)) { continue }
        foreach ($key in @(Get-ChildItem -LiteralPath $root -ErrorAction SilentlyContinue)) {
            $values = Get-ItemProperty -LiteralPath $key.PSPath -ErrorAction SilentlyContinue
            $name = Get-Property $values 'DisplayName'
            if (-not $name -or $name -notmatch $ownPattern) { continue }
            $quiet = Get-Property $values 'QuietUninstallString'
            if ($quiet) { return [string]$quiet }
            $plain = Get-Property $values 'UninstallString'
            if ($plain) { return [string]$plain }
        }
    }
    return $null
}

function Uninstall-Candidate {
    param([string] $Id)
    $title = 'Deinstallation still, ohne Neustart'
    $command = Get-OwnUninstallCommand
    if (-not $command) {
        Add-Check $Id $title 'FEHLER' 'kein Eintrag mit owncloud.online in Programme und Features' 'H19'
        return
    }
    $parts = @(Split-CommandLine $command)
    $arguments = $parts[1]
    # NSIS wertet nur das große /S aus
    if ($arguments -cnotmatch '(^|\s)/S(\s|$)') { $arguments = "/S $arguments" }
    $run = Invoke-Step 'Deinstallation (Prüfling)' $parts[0] ("$arguments /norestart".Trim())
    Test-Run $run $Id $title
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
            [IO.File]::WriteAllText((Join-Path $trace.ConfigDir "$appName.cfg"), "[General]`r`nprobe=true`r`n")
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
    return ($parts -join '|')
}

function Test-TracesAfterUninstall {
    param($Snapshot)
    $state = Get-TraceState $Snapshot
    # Spur, soll vorhanden sein, Audit
    $expectations = @(
        @('Anmeldedaten', $false, 'E4, H6'),
        @('Einstellungen', $false, 'E4, H6'),
        @('Cache', $false, 'H6'),
        @('Autostart', $false, 'H6'),
        @('Navigationsbereich', $false, 'H4'),
        @('Nutzerdatei', $true, 'E4, H5')
    )
    $number = 2
    foreach ($expectation in $expectations) {
        $number++
        $name = $expectation[0]
        $shouldExist = $expectation[1]
        $title = if ($shouldExist) { 'Nutzerdatei im Sync-Ordner bleibt erhalten' } else { "$name entfernt" }
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
    Add-Check 'D9' 'Sync-Root (CfApi), Search-Regeln, Schnellzugriff entfernt' 'NICHT GEPRÜFT' 'braucht einen laufenden Client mit Konto, im CI nicht nachbildbar' 'H4, H6'
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
    param($Base, $Final, [string] $DiffName, [hashtable] $Ignore)
    $entries = @(Get-UninstallEntries $Final $ownPattern)
    if ($entries.Count -gt 0) {
        Add-Check 'D2' 'Eintrag in Programme und Features entfernt' 'FEHLER' (($entries | ForEach-Object { $_.Key }) -join '; ') ''
    } else {
        Add-Check 'D2' 'Eintrag in Programme und Features entfernt' 'OK' '' ''
    }
    Test-TracesAfterUninstall $Final
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
    Uninstall-Candidate 'D1'
    $removed = New-Snapshot 'deinstalliert' 'Prüfling deinstalliert'
    Test-Removal $base $removed 'ausgang-deinstalliert' @{}
}

function Invoke-Upgrade {
    $base = New-Snapshot 'ausgang' 'Ausgangszustand'
    Assert-Fresh $base
    $run = Invoke-Step 'Setup (Vorversion)' $PreviousSetup '/S /norestart'
    Test-Run $run 'U1' 'Setup (Vorversion) still, ohne Neustart'
    if ($run.zeitueberschreitung -or ($run.exitcode -ne 0 -and $run.exitcode -ne 3010)) {
        throw 'Vorversion ließ sich nicht installieren - Update nicht prüfbar'
    }
    Add-ClientTraces (Find-ClientExe)
    $previous = New-Snapshot 'vorversion' 'Vorversion installiert, Client-Spuren angelegt'
    Install-Candidate 'I1'
    $exe = Find-ClientExe
    $installed = New-Snapshot 'installiert' 'Prüfling über die Vorversion installiert'
    Test-Installation $previous $installed $exe 'vorversion-installiert'
    Test-TracesAfterUpgrade $installed
    Uninstall-Candidate 'D1'
    $removed = New-Snapshot 'deinstalliert' 'Prüfling deinstalliert'
    Test-Removal $base $removed 'ausgang-deinstalliert' @{}
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
    Uninstall-Candidate 'D1'
    $removed = New-Snapshot 'deinstalliert' 'Prüfling deinstalliert, offizieller Client bleibt'
    Test-Footprint $footprint $removed 'K3' 'Offizieller Client nach unserer Deinstallation unverändert' 'H2, H3'
    Test-OfficialProcess $running 'K4' 'Offizieller Client läuft nach unserer Deinstallation weiter'
    Test-Removal $official $removed 'offiziell-deinstalliert' $footprint

    # Gegenrichtung: der offizielle Deinstaller darf unseren Client nicht beschädigen
    Install-Candidate 'K5' 'Setup (Prüfling, erneut)'
    $again = New-Snapshot 'erneut-installiert' 'Prüfling erneut installiert'
    $ownFootprint = Get-Footprint $removed $again 'deinstalliert-erneut'
    $log = Join-Path $OutDir 'offiziell-deinstallation.log'
    $run = Invoke-Step 'Offizieller Client entfernen (MSI)' 'msiexec.exe' ('/x "{0}" /qn /norestart /l*v "{1}"' -f $OfficialMsi, $log)
    if ($run.zeitueberschreitung -or ($run.exitcode -ne 0 -and $run.exitcode -ne 3010)) {
        Add-Check 'K6' 'Offizieller Client lässt sich entfernen' 'WARNUNG' ('msiexec {0}, Protokoll offiziell-deinstallation.log' -f $run.exitcode) ''
    } else {
        Add-Check 'K6' 'Offizieller Client lässt sich entfernen' 'OK' ('Exit-Code {0}' -f $run.exitcode) ''
    }
    $final = New-Snapshot 'offiziell-entfernt' 'Offizieller Client entfernt, Prüfling bleibt'
    Test-Footprint $ownFootprint $final 'K7' 'Unser Client nach Entfernen des offiziellen unverändert' 'H3'
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
    if ($Scenario -eq 'upgrade') { $head.Add(('- Vorversion: `{0}`' -f (Get-Leaf $PreviousSetup))) }
    if ($Scenario -eq 'koexistenz') { $head.Add(('- Offizieller Client: `{0}`' -f (Get-Leaf $OfficialMsi))) }
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
    if ($Scenario -eq 'upgrade') { $required['PreviousSetup'] = $PreviousSetup }
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
