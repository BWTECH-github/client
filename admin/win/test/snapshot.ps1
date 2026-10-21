<#
.SYNOPSIS
    Schnappschuss aller Spuren von owncloud.online (und des offiziellen
    ownCloud-Clients) unter Windows als JSON - Grundlage für compare.ps1.

.DESCRIPTION
    Rein lesend. Erfasst je Kategorie eine Zuordnung "Eintrag -> Wert":

      Registry  Programme und Features (beide Sichten, HKLM und HKCU),
                Hersteller-/Produktschlüssel, SyncRootManager (CfApi),
                Explorer-Navigationsbereich (HKCU-CLSID, Desktop\NameSpace,
                NewStartPanel), StorageSense-Richtlinien, Shell-Erweiterungen
                (COM-Klassen, Overlay-Kennungen, Kontextmenü-Handler,
                Approved-Liste), Autostart (Run, RunOnce, StartupApproved),
                Dateitypen/Protokolle/App Paths, Windows-Search-Regeln,
                ausstehende Neustart-Aktionen (PendingFileRenameOperations)
      Dateien   Programmverzeichnisse (mit SHA256), Benutzerprofil
                (Konfiguration, Cache, Temp, Absturzberichte), Verknüpfungen in
                Startmenü und auf dem Desktop, Autostart-Ordner, Sync-Ordner
                mit Platzhalter-Bilanz
      System    Anmeldeinformationsverwaltung (nur Zielnamen, nie Geheimnisse),
                geplante Aufgaben, Firewall-Regeln, Dienste, laufende Prozesse

    Registry-Pfade werden immer in der 64-Bit-Sicht gelesen; die 32-Bit-Sicht
    steht ausdrücklich als WOW6432Node im Pfad. Ein Fehler beim Lesen einer
    Kategorie landet als Eintrag "!Fehler" im Schnappschuss, statt den Lauf
    abzubrechen.

    Läuft unter Windows PowerShell 5.1 und PowerShell 7. Erhöht ausführen,
    sonst fehlen geschützte Teile. HKCU ist der Hive des ausführenden Kontos.

.PARAMETER OutFile
    Zieldatei (JSON, UTF-8).

.PARAMETER Label
    Bezeichnung des Schnappschusses im Bericht, z. B. "A-vorher".

.PARAMETER Pattern
    Regulärer Ausdruck (ohne Groß-/Kleinschreibung) für Namen, Werte und
    Pfade, die zu einem der beiden Clients gehören.

.EXAMPLE
    .\snapshot.ps1 -OutFile C:\probe\A.json -Label A-vorher
#>
[CmdletBinding()]
param(
    [Parameter(Mandatory = $true)]
    [string] $OutFile,

    [string] $Label = 'Schnappschuss',

    [string] $Pattern = 'owncloud'
)

$ErrorActionPreference = 'Stop'
Set-StrictMode -Version 2.0

# COM-Klassen der Shell-Erweiterungen (Overlays, Kontextmenü). owncloud.online
# und der offizielle Client verwenden dieselben GUIDs (Audit H3) - sie werden
# deshalb auch erfasst, wenn InprocServer32 fehlt oder auf einen fremden Pfad
# zeigt.
$knownShellClsids = @(
    '{0960F090-F328-48A3-B746-276B1E3C3722}', '{0960F091-F328-48A3-B746-276B1E3C3722}',
    '{0960F092-F328-48A3-B746-276B1E3C3722}', '{0960F093-F328-48A3-B746-276B1E3C3722}',
    '{0960F094-F328-48A3-B746-276B1E3C3722}', '{0960F095-F328-48A3-B746-276B1E3C3722}',
    '{0960F096-F328-48A3-B746-276B1E3C3722}', '{0960F097-F328-48A3-B746-276B1E3C3722}',
    '{841A0AAD-AA11-4B50-84D9-7F8E727D77D7}'
)

# Markierungen, an denen compare.ps1 Behälter (Schlüssel/Verzeichnisse) erkennt
$keyMarker = '(Schlüssel)'
$dirMarker = '(Verzeichnis)'
# größere Dateien nur mit Größe und Zeitstempel statt SHA256
$hashLimit = 256MB

$hives = @{
    HKLM = [Microsoft.Win32.RegistryKey]::OpenBaseKey([Microsoft.Win32.RegistryHive]::LocalMachine, [Microsoft.Win32.RegistryView]::Registry64)
    HKCU = [Microsoft.Win32.RegistryKey]::OpenBaseKey([Microsoft.Win32.RegistryHive]::CurrentUser, [Microsoft.Win32.RegistryView]::Registry64)
}

$categories = [ordered]@{}

function Add-Category {
    param([string] $Id, [string] $Label, [scriptblock] $Collect)
    $items = [System.Collections.Generic.SortedDictionary[string, string]]::new([StringComparer]::OrdinalIgnoreCase)
    $watch = [Diagnostics.Stopwatch]::StartNew()
    try {
        & $Collect $items
    } catch {
        $items['!Fehler'] = $_.Exception.Message
    }
    $categories[$Id] = [ordered]@{ label = $Label; items = $items }
    Write-Verbose ('{0}: {1} Einträge in {2:N1} s' -f $Id, $items.Count, $watch.Elapsed.TotalSeconds)
}

# ---------------------------------------------------------------------------
# Registry
# ---------------------------------------------------------------------------

function Format-Bytes {
    param($Data)
    if ($null -eq $Data) { return '(leer)' }
    $bytes = [byte[]]$Data
    if ($bytes.Length -le 48) { return ([BitConverter]::ToString($bytes) -replace '-', '') }
    $sha = [Security.Cryptography.SHA256]::Create()
    try {
        $hash = [BitConverter]::ToString($sha.ComputeHash($bytes)) -replace '-', ''
    } finally {
        $sha.Dispose()
    }
    return ('{0} Bytes, sha256 {1}' -f $bytes.Length, $hash.Substring(0, 16))
}

function Format-RegValue {
    param([Microsoft.Win32.RegistryKey] $Key, [string] $Name)
    $kind = $Key.GetValueKind($Name)
    $data = $Key.GetValue($Name, $null, [Microsoft.Win32.RegistryValueOptions]::DoNotExpandEnvironmentNames)
    switch ($kind.ToString()) {
        'String' { return "REG_SZ: $data" }
        'ExpandString' { return "REG_EXPAND_SZ: $data" }
        'MultiString' { return 'REG_MULTI_SZ: ' + (@($data) -join ' | ') }
        'DWord' {
            $unsigned = [BitConverter]::ToUInt32([BitConverter]::GetBytes([int32]$data), 0)
            return ('REG_DWORD: {0} (0x{0:X8})' -f $unsigned)
        }
        'QWord' {
            $unsigned = [BitConverter]::ToUInt64([BitConverter]::GetBytes([int64]$data), 0)
            return ('REG_QWORD: {0}' -f $unsigned)
        }
        'Binary' { return 'REG_BINARY: ' + (Format-Bytes $data) }
        default { return "REG_$($kind.ToString().ToUpperInvariant()): " + (Format-Bytes $data) }
    }
}

# Schlüssel mit seinen Werten und (bis MaxDepth) allen Unterschlüsseln
function Add-RegTree {
    param($Items, [string] $Hive, [string] $Path, [int] $MaxDepth = 20, [int] $Depth = 0)
    $display = "$Hive\$Path"
    try {
        $key = $hives[$Hive].OpenSubKey($Path, $false)
    } catch {
        $Items[$display] = 'nicht lesbar: ' + $_.Exception.Message
        return
    }
    if ($null -eq $key) { return }
    try {
        $Items[$display] = $keyMarker
        foreach ($name in $key.GetValueNames()) {
            $shown = if ($name -eq '') { '(Standard)' } else { $name }
            try {
                $Items["$display :: $shown"] = Format-RegValue -Key $key -Name $name
            } catch {
                $Items["$display :: $shown"] = 'nicht lesbar: ' + $_.Exception.Message
            }
        }
        if ($Depth -lt $MaxDepth) {
            foreach ($sub in $key.GetSubKeyNames()) {
                Add-RegTree -Items $Items -Hive $Hive -Path "$Path\$sub" -MaxDepth $MaxDepth -Depth ($Depth + 1)
            }
        }
    } finally {
        $key.Close()
    }
}

# Prüft, ob einer der Werte (Form "Unterpfad|Wertname", leer = Standardwert)
# auf das Muster passt
function Test-RegProbe {
    param([string] $Hive, [string] $Path, [string[]] $Probes)
    foreach ($probe in $Probes) {
        $parts = $probe -split '\|', 2
        $keyPath = if ($parts[0]) { "$Path\$($parts[0])" } else { $Path }
        $key = $null
        try { $key = $hives[$Hive].OpenSubKey($keyPath, $false) } catch { $key = $null }
        if ($null -eq $key) { continue }
        try {
            $value = $key.GetValue($parts[1])
            if ($value -is [string] -and $value -match $Pattern) { return $true }
        } finally {
            $key.Close()
        }
    }
    return $false
}

# Unterschlüssel von Path, deren Name oder geprüfte Werte auf das Muster passen
# (oder die in Always stehen), jeweils mit vollständigem Teilbaum
function Add-RegMatchingSubkeys {
    param($Items, [string] $Hive, [string] $Path, [string[]] $Probes = @('|'), [string[]] $Always = @())
    try {
        $root = $hives[$Hive].OpenSubKey($Path, $false)
    } catch {
        $Items["$Hive\$Path"] = 'nicht lesbar: ' + $_.Exception.Message
        return
    }
    if ($null -eq $root) { return }
    try { $names = $root.GetSubKeyNames() } finally { $root.Close() }
    foreach ($name in $names) {
        if ($name -match $Pattern -or $Always -contains $name -or (Test-RegProbe -Hive $Hive -Path "$Path\$name" -Probes $Probes)) {
            Add-RegTree -Items $Items -Hive $Hive -Path "$Path\$name"
        }
    }
}

Add-Category 'reg.uninstall' 'Registry: Programme und Features (Uninstall)' {
    param($items)
    Add-RegTree -Items $items -Hive HKLM -Path 'SOFTWARE\Microsoft\Windows\CurrentVersion\Uninstall' -MaxDepth 1
    Add-RegTree -Items $items -Hive HKLM -Path 'SOFTWARE\WOW6432Node\Microsoft\Windows\CurrentVersion\Uninstall' -MaxDepth 1
    Add-RegTree -Items $items -Hive HKCU -Path 'Software\Microsoft\Windows\CurrentVersion\Uninstall' -MaxDepth 1
}

Add-Category 'reg.vendor' 'Registry: Hersteller- und Produktschlüssel' {
    param($items)
    foreach ($spec in @(@('HKLM', 'SOFTWARE'), @('HKLM', 'SOFTWARE\WOW6432Node'), @('HKCU', 'Software'))) {
        Add-RegMatchingSubkeys -Items $items -Hive $spec[0] -Path $spec[1] -Probes @() -Always @('BW-Tech GmbH')
        Add-RegMatchingSubkeys -Items $items -Hive $spec[0] -Path "$($spec[1])\Policies" -Probes @() -Always @('BW-Tech GmbH')
    }
}

Add-Category 'reg.syncroot' 'Registry: SyncRootManager (Cloud-Files-API)' {
    param($items)
    Add-RegTree -Items $items -Hive HKLM -Path 'SOFTWARE\Microsoft\Windows\CurrentVersion\Explorer\SyncRootManager'
}

Add-Category 'reg.navpane' 'Registry: Explorer-Navigationsbereich (HKCU-CLSID, Desktop\NameSpace, NewStartPanel)' {
    param($items)
    $probes = @('|', '|ApplicationName', 'Instance\InitPropertyBag|TargetFolderPath', 'DefaultIcon|', 'InprocServer32|')
    Add-RegMatchingSubkeys -Items $items -Hive HKCU -Path 'Software\Classes\CLSID' -Probes $probes
    Add-RegMatchingSubkeys -Items $items -Hive HKCU -Path 'Software\Classes\WOW6432Node\CLSID' -Probes $probes
    Add-RegTree -Items $items -Hive HKCU -Path 'Software\Microsoft\Windows\CurrentVersion\Explorer\Desktop\NameSpace'
    Add-RegTree -Items $items -Hive HKLM -Path 'SOFTWARE\Microsoft\Windows\CurrentVersion\Explorer\Desktop\NameSpace'
    Add-RegTree -Items $items -Hive HKCU -Path 'Software\Microsoft\Windows\CurrentVersion\Explorer\HideDesktopIcons\NewStartPanel'
}

Add-Category 'reg.storagesense' 'Registry: StorageSense-Richtlinien' {
    param($items)
    Add-RegTree -Items $items -Hive HKCU -Path 'Software\Microsoft\Windows\CurrentVersion\StorageSense\Parameters\StoragePolicy'
}

Add-Category 'reg.shellext' 'Registry: Shell-Erweiterungen (COM-Klassen, Overlays, Kontextmenü)' {
    param($items)
    $probes = @('|', 'InprocServer32|', 'LocalServer32|')
    Add-RegMatchingSubkeys -Items $items -Hive HKLM -Path 'SOFTWARE\Classes\CLSID' -Probes $probes -Always $knownShellClsids
    Add-RegMatchingSubkeys -Items $items -Hive HKLM -Path 'SOFTWARE\WOW6432Node\Classes\CLSID' -Probes $probes -Always $knownShellClsids
    Add-RegTree -Items $items -Hive HKLM -Path 'SOFTWARE\Microsoft\Windows\CurrentVersion\Explorer\ShellIconOverlayIdentifiers'
    Add-RegTree -Items $items -Hive HKLM -Path 'SOFTWARE\WOW6432Node\Microsoft\Windows\CurrentVersion\Explorer\ShellIconOverlayIdentifiers'
    foreach ($class in 'AllFileSystemObjects', '*', 'Directory', 'Directory\Background', 'Folder', 'Drive') {
        Add-RegTree -Items $items -Hive HKLM -Path "SOFTWARE\Classes\$class\shellex\ContextMenuHandlers"
        Add-RegTree -Items $items -Hive HKCU -Path "Software\Classes\$class\shellex\ContextMenuHandlers"
    }
    Add-RegTree -Items $items -Hive HKLM -Path 'SOFTWARE\Microsoft\Windows\CurrentVersion\Shell Extensions\Approved' -MaxDepth 0
    Add-RegTree -Items $items -Hive HKLM -Path 'SOFTWARE\WOW6432Node\Microsoft\Windows\CurrentVersion\Shell Extensions\Approved' -MaxDepth 0
    Add-RegTree -Items $items -Hive HKCU -Path 'Software\Microsoft\Windows\CurrentVersion\Shell Extensions\Approved' -MaxDepth 0
}

Add-Category 'reg.run' 'Registry: Autostart (Run, RunOnce, StartupApproved)' {
    param($items)
    foreach ($name in 'Run', 'RunOnce') {
        Add-RegTree -Items $items -Hive HKCU -Path "Software\Microsoft\Windows\CurrentVersion\$name" -MaxDepth 0
        Add-RegTree -Items $items -Hive HKLM -Path "SOFTWARE\Microsoft\Windows\CurrentVersion\$name" -MaxDepth 0
        Add-RegTree -Items $items -Hive HKLM -Path "SOFTWARE\WOW6432Node\Microsoft\Windows\CurrentVersion\$name" -MaxDepth 0
    }
    foreach ($name in 'Run', 'Run32', 'StartupFolder') {
        Add-RegTree -Items $items -Hive HKCU -Path "Software\Microsoft\Windows\CurrentVersion\Explorer\StartupApproved\$name" -MaxDepth 0
        Add-RegTree -Items $items -Hive HKLM -Path "SOFTWARE\Microsoft\Windows\CurrentVersion\Explorer\StartupApproved\$name" -MaxDepth 0
    }
}

Add-Category 'reg.classes' 'Registry: Dateitypen, Protokolle, App Paths' {
    param($items)
    foreach ($spec in @(@('HKLM', 'SOFTWARE\Classes'), @('HKCU', 'Software\Classes'))) {
        Add-RegMatchingSubkeys -Items $items -Hive $spec[0] -Path $spec[1] -Probes @()
        Add-RegMatchingSubkeys -Items $items -Hive $spec[0] -Path "$($spec[1])\Applications" -Probes @()
    }
    foreach ($spec in @(@('HKLM', 'SOFTWARE\Microsoft\Windows\CurrentVersion\App Paths'),
                        @('HKLM', 'SOFTWARE\WOW6432Node\Microsoft\Windows\CurrentVersion\App Paths'),
                        @('HKCU', 'Software\Microsoft\Windows\CurrentVersion\App Paths'))) {
        Add-RegMatchingSubkeys -Items $items -Hive $spec[0] -Path $spec[1] -Probes @('|', '|Path')
    }
}

Add-Category 'reg.search' 'Registry: Windows-Search-Regeln (CrawlScopeManager)' {
    param($items)
    $base = 'SOFTWARE\Microsoft\Windows Search\CrawlScopeManager\Windows\SystemIndex'
    Add-RegTree -Items $items -Hive HKLM -Path "$base\DefaultRules"
    Add-RegTree -Items $items -Hive HKLM -Path "$base\WorkingSetRules"
}

Add-Category 'reboot' 'Ausstehende Neustart-Aktionen' {
    param($items)
    $session = $hives['HKLM'].OpenSubKey('SYSTEM\CurrentControlSet\Control\Session Manager', $false)
    try {
        foreach ($valueName in 'PendingFileRenameOperations', 'PendingFileRenameOperations2') {
            $operations = @($session.GetValue($valueName))
            # Paare (Quelle, Ziel); leeres Ziel = beim Neustart löschen
            for ($i = 0; $i + 1 -lt $operations.Count; $i += 2) {
                # NT-Pfade "\??\C:\..."; Windows Server 2025/Windows 11 stellen
                # "*<Zahl>" voran, "!" vor dem Ziel heißt "vorhandene Datei ersetzen"
                $source = ([string]$operations[$i]) -replace '^(\*\d+)?\\\?\?\\', ''
                $target = ([string]$operations[$i + 1]) -replace '^(\*\d+)?!?\\\?\?\\', ''
                if (-not $source) { continue }
                if ($target) {
                    $items["Umbenennen beim Neustart: $source"] = "-> $target"
                } else {
                    $items["Löschen beim Neustart: $source"] = 'vorgemerkt'
                }
            }
        }
    } finally {
        $session.Close()
    }
    foreach ($flag in @(
            @('Component Based Servicing: RebootPending', 'SOFTWARE\Microsoft\Windows\CurrentVersion\Component Based Servicing\RebootPending'),
            @('Windows Update: RebootRequired', 'SOFTWARE\Microsoft\Windows\CurrentVersion\WindowsUpdate\Auto Update\RebootRequired'))) {
        $key = $hives['HKLM'].OpenSubKey($flag[1], $false)
        if ($key) {
            $key.Close()
            $items[$flag[0]] = 'ja'
        } else {
            $items[$flag[0]] = 'nein'
        }
    }
}

# ---------------------------------------------------------------------------
# Dateien
# ---------------------------------------------------------------------------

function Get-AttributeNote {
    param([int] $Attributes)
    $notes = @()
    if ($Attributes -band 0x400) { $notes += 'Reparse' }
    if ($Attributes -band 0x1000) { $notes += 'Offline' }
    if ($Attributes -band 0x40000) { $notes += 'RecallOnOpen' }
    if ($Attributes -band 0x400000) { $notes += 'RecallOnDataAccess' }
    if ($Attributes -band 0x80000) { $notes += 'Pinned' }
    if ($Attributes -band 0x100000) { $notes += 'Unpinned' }
    if ($notes.Count -gt 0) { return ' [' + ($notes -join ', ') + ']' }
    return ''
}

function Get-FsValue {
    param($Item)
    $attributes = [int]$Item.Attributes
    $note = Get-AttributeNote $attributes
    if ($Item.PSIsContainer) { return $dirMarker + $note }
    $value = 'Datei, {0} B' -f $Item.Length
    # Platzhalter nie lesen - das würde sie hydrieren
    if (($attributes -band 0x441000) -ne 0) { return "$value, Platzhalter (nicht gelesen)$note" }
    if ($Item.Length -gt $hashLimit) { return "$value, geändert $($Item.LastWriteTimeUtc.ToString('o'))$note" }
    try {
        # -ErrorAction Stop: Get-FileHash meldet gesperrte Dateien sonst nur
        # nicht abbrechend, und der Wert bliebe leer
        $hash = (Get-FileHash -LiteralPath $Item.FullName -Algorithm SHA256 -ErrorAction Stop).Hash
    } catch {
        $hash = 'nicht lesbar'
    }
    return "$value, sha256 $hash$note"
}

function Add-FileTree {
    param($Items, [string] $Root)
    if (-not (Test-Path -LiteralPath $Root)) { return }
    $rootItem = Get-Item -LiteralPath $Root -Force
    $Items[$rootItem.FullName] = Get-FsValue $rootItem
    if ($rootItem.PSIsContainer) {
        foreach ($child in Get-ChildItem -LiteralPath $Root -Recurse -Force -ErrorAction SilentlyContinue) {
            $Items[$child.FullName] = Get-FsValue $child
        }
    }
}

# direkte Einträge von Parent, deren Name auf das Muster passt
function Get-MatchingChildren {
    param([string] $Parent, [switch] $DirectoriesOnly)
    if (-not $Parent -or -not (Test-Path -LiteralPath $Parent)) { return }
    foreach ($child in Get-ChildItem -LiteralPath $Parent -Force -ErrorAction SilentlyContinue) {
        if ($DirectoriesOnly -and -not $child.PSIsContainer) { continue }
        if ($child.Name -match $Pattern) { $child.FullName }
    }
}

$programFiles = if ($env:ProgramW6432) { $env:ProgramW6432 } else { $env:ProgramFiles }
$programFilesX86 = ${env:ProgramFiles(x86)}
$programData = $env:ProgramData
$appData = [Environment]::GetFolderPath('ApplicationData')
$localAppData = [Environment]::GetFolderPath('LocalApplicationData')
$tempDir = [IO.Path]::GetTempPath().TrimEnd('\')

Add-Category 'files.program' 'Dateien: Programmverzeichnisse' {
    param($items)
    foreach ($parent in @($programFiles, $programFilesX86, $programData) | Where-Object { $_ } | Select-Object -Unique) {
        foreach ($dir in Get-MatchingChildren -Parent $parent -DirectoriesOnly) {
            Add-FileTree -Items $items -Root $dir
        }
    }
}

Add-Category 'files.user' 'Dateien: Benutzerprofil (Konfiguration, Cache, Temp, Absturzberichte)' {
    param($items)
    foreach ($parent in @($appData, $localAppData, $tempDir,
            (Join-Path $localAppData 'CrashDumps'),
            (Join-Path $localAppData 'Microsoft\Windows\WER\ReportArchive'),
            (Join-Path $localAppData 'Microsoft\Windows\WER\ReportQueue'),
            (Join-Path $programData 'Microsoft\Windows\WER\ReportArchive'),
            (Join-Path $programData 'Microsoft\Windows\WER\ReportQueue'))) {
        foreach ($entry in Get-MatchingChildren -Parent $parent) {
            Add-FileTree -Items $items -Root $entry
        }
    }
}

$wsh = New-Object -ComObject WScript.Shell

function Get-LinkValue {
    param($Item)
    if ($Item.PSIsContainer) { return $dirMarker }
    if ($Item.Extension -ne '.lnk') { return Get-FsValue $Item }
    try {
        $link = $wsh.CreateShortcut($Item.FullName)
        $target = [string]$link.TargetPath
        $exists = if ($target -and (Test-Path -LiteralPath $target)) { 'ja' } else { 'nein' }
        $command = ("$target $($link.Arguments)").Trim()
        return ('Verknüpfung -> {0} | Ziel vorhanden: {1} | Symbol: {2}' -f $command, $exists, $link.IconLocation)
    } catch {
        return 'Verknüpfung, nicht lesbar: ' + $_.Exception.Message
    }
}

Add-Category 'files.shortcuts' 'Dateien: Verknüpfungen (Startmenü, Desktop) und Autostart-Ordner' {
    param($items)
    foreach ($root in @([Environment]::GetFolderPath('CommonPrograms'), [Environment]::GetFolderPath('Programs'))) {
        if (-not $root -or -not (Test-Path -LiteralPath $root)) { continue }
        foreach ($entry in Get-ChildItem -LiteralPath $root -Recurse -Force -ErrorAction SilentlyContinue) {
            if ($entry.FullName.Substring($root.Length) -match $Pattern) { $items[$entry.FullName] = Get-LinkValue $entry }
        }
    }
    foreach ($root in @([Environment]::GetFolderPath('CommonDesktopDirectory'), [Environment]::GetFolderPath('DesktopDirectory'))) {
        foreach ($entry in Get-MatchingChildren -Parent $root) {
            $items[$entry] = Get-LinkValue (Get-Item -LiteralPath $entry -Force)
        }
    }
    # Autostart-Ordner vollständig: jede neue Datei dort ist relevant
    foreach ($root in @([Environment]::GetFolderPath('CommonStartup'), [Environment]::GetFolderPath('Startup'))) {
        if (-not $root -or -not (Test-Path -LiteralPath $root)) { continue }
        foreach ($entry in Get-ChildItem -LiteralPath $root -Force -ErrorAction SilentlyContinue) {
            $items[$entry.FullName] = Get-LinkValue $entry
        }
    }
}

Add-Category 'syncfolders' 'Sync-Ordner und Platzhalter' {
    param($items)
    $roots = New-Object 'System.Collections.Generic.List[string]'
    foreach ($dir in Get-MatchingChildren -Parent $env:USERPROFILE -DirectoriesOnly) { $roots.Add($dir) }
    # Sync-Roots aller Benutzer laut SyncRootManager
    $manager = $hives['HKLM'].OpenSubKey('SOFTWARE\Microsoft\Windows\CurrentVersion\Explorer\SyncRootManager', $false)
    if ($manager) {
        try {
            foreach ($id in $manager.GetSubKeyNames()) {
                if ($id -notmatch $Pattern) { continue }
                $userRoots = $manager.OpenSubKey("$id\UserSyncRoots", $false)
                if (-not $userRoots) { continue }
                try {
                    foreach ($valueName in $userRoots.GetValueNames()) {
                        $path = [string]$userRoots.GetValue($valueName)
                        if ($path -and -not $roots.Contains($path)) { $roots.Add($path) }
                    }
                } finally {
                    $userRoots.Close()
                }
            }
        } finally {
            $manager.Close()
        }
    }
    foreach ($root in $roots) {
        if (-not (Test-Path -LiteralPath $root)) {
            $items[$root] = 'fehlt'
            continue
        }
        $items[$root] = Get-FsValue (Get-Item -LiteralPath $root -Force)
        $files = 0
        $dehydrated = 0
        $bookkeeping = New-Object 'System.Collections.Generic.SortedSet[string]' ([StringComparer]::OrdinalIgnoreCase)
        foreach ($file in Get-ChildItem -LiteralPath $root -Recurse -Force -File -ErrorAction SilentlyContinue) {
            $files++
            if (([int]$file.Attributes -band 0x441000) -ne 0) { $dehydrated++ }
            if ($file.Name -match '^\.?_?sync_.*\.db(-wal|-shm)?$|^\.owncloudsync\.log$') { [void]$bookkeeping.Add($file.Name) }
        }
        $items["$root :: Dateien"] = "$files"
        $items["$root :: nur online (dehydriert)"] = "$dehydrated"
        $items["$root :: Journal und Protokoll"] = if ($bookkeeping.Count -gt 0) { @($bookkeeping) -join ', ' } else { '-' }
    }
}

# ---------------------------------------------------------------------------
# System
# ---------------------------------------------------------------------------

$credentialSource = @'
using System;
using System.Collections.Generic;
using System.Runtime.InteropServices;

namespace OcProbe
{
    public static class CredList
    {
        [StructLayout(LayoutKind.Sequential, CharSet = CharSet.Unicode)]
        private struct CREDENTIAL
        {
            public int Flags;
            public int Type;
            public IntPtr TargetName;
            public IntPtr Comment;
            public System.Runtime.InteropServices.ComTypes.FILETIME LastWritten;
            public int CredentialBlobSize;
            public IntPtr CredentialBlob;
            public int Persist;
            public int AttributeCount;
            public IntPtr Attributes;
            public IntPtr TargetAlias;
            public IntPtr UserName;
        }

        [DllImport("advapi32.dll", EntryPoint = "CredEnumerateW", CharSet = CharSet.Unicode, SetLastError = true)]
        private static extern bool CredEnumerate(string filter, int flags, out int count, out IntPtr credentials);

        [DllImport("advapi32.dll")]
        private static extern void CredFree(IntPtr buffer);

        // "Typ<TAB>Persistenz<TAB>Zielname" je Eintrag. Das Geheimnis
        // (CredentialBlob) und der Benutzername werden nie gelesen.
        public static string[] Targets()
        {
            int count;
            IntPtr list;
            if (!CredEnumerate(null, 0, out count, out list))
            {
                int error = Marshal.GetLastWin32Error();
                if (error == 1168) { return new string[0]; } // ERROR_NOT_FOUND: keine Einträge
                throw new InvalidOperationException("CredEnumerate fehlgeschlagen, Fehler " + error);
            }
            try
            {
                var result = new List<string>(count);
                for (int i = 0; i < count; i++)
                {
                    IntPtr entry = Marshal.ReadIntPtr(list, i * IntPtr.Size);
                    var credential = (CREDENTIAL)Marshal.PtrToStructure(entry, typeof(CREDENTIAL));
                    result.Add(credential.Type + "\t" + credential.Persist + "\t" + Marshal.PtrToStringUni(credential.TargetName));
                }
                return result.ToArray();
            }
            finally
            {
                CredFree(list);
            }
        }
    }
}
'@

Add-Category 'credentials' 'Anmeldeinformationsverwaltung (nur Zielnamen)' {
    param($items)
    $typeNames = @{ 1 = 'generisch'; 2 = 'Domänenkennwort'; 3 = 'Domänenzertifikat'; 4 = 'sichtbares Domänenkennwort'; 5 = 'generisches Zertifikat'; 6 = 'Domäne erweitert' }
    $persistNames = @{ 1 = 'Sitzung'; 2 = 'lokaler Computer'; 3 = 'Unternehmen' }
    try {
        if (-not ('OcProbe.CredList' -as [type])) { Add-Type -TypeDefinition $credentialSource -Language CSharp }
        $entries = [OcProbe.CredList]::Targets()
    } catch {
        $entries = $null
    }
    if ($null -ne $entries) {
        foreach ($entry in $entries) {
            $parts = $entry -split "`t", 3
            if ($parts[2] -match $Pattern) {
                $items[$parts[2]] = 'Typ {0}, Persistenz {1}' -f $typeNames[[int]$parts[0]], $persistNames[[int]$parts[1]]
            }
        }
        return
    }
    # Rückfall ohne Add-Type: cmdkey. Zielzeilen enthalten "target=" o. ä.,
    # Benutzerzeilen werden nicht übernommen.
    foreach ($line in (& cmdkey.exe /list)) {
        if ($line -match '^\s*[^:=]+:\s+(\S*=.*)$') {
            $target = $Matches[1].Trim()
            if ($target -match $Pattern) { $items[$target] = 'laut cmdkey' }
        }
    }
}

function Get-OptionalProperty {
    param($Object, [string] $Name)
    $property = $Object.PSObject.Properties[$Name]
    if ($property) { return $property.Value }
    return $null
}

Add-Category 'tasks' 'Geplante Aufgaben' {
    param($items)
    foreach ($task in Get-ScheduledTask) {
        $actions = @(foreach ($action in @($task.Actions)) {
                ('{0} {1}' -f (Get-OptionalProperty $action 'Execute'), (Get-OptionalProperty $action 'Arguments')).Trim()
            }) -join ' ; '
        $name = "$($task.TaskPath)$($task.TaskName)"
        if ("$name $actions" -match $Pattern) {
            $items[$name] = "Zustand $($task.State); Aktion: $actions"
        }
    }
}

Add-Category 'firewall' 'Firewall-Regeln' {
    param($items)
    foreach ($path in 'SYSTEM\CurrentControlSet\Services\SharedAccess\Parameters\FirewallPolicy\FirewallRules',
                      'SOFTWARE\Policies\Microsoft\WindowsFirewall\FirewallRules') {
        $key = $hives['HKLM'].OpenSubKey($path, $false)
        if (-not $key) { continue }
        try {
            foreach ($name in $key.GetValueNames()) {
                $value = [string]$key.GetValue($name)
                if ("$name $value" -match $Pattern) { $items["HKLM\$path :: $name"] = $value }
            }
        } finally {
            $key.Close()
        }
    }
}

Add-Category 'services' 'Dienste und Treiber' {
    param($items)
    $root = 'SYSTEM\CurrentControlSet\Services'
    $key = $hives['HKLM'].OpenSubKey($root, $false)
    try { $names = $key.GetSubKeyNames() } finally { $key.Close() }
    foreach ($name in $names) {
        if ($name -match $Pattern -or (Test-RegProbe -Hive HKLM -Path "$root\$name" -Probes @('|DisplayName', '|ImagePath', '|Description'))) {
            Add-RegTree -Items $items -Hive HKLM -Path "$root\$name" -MaxDepth 0
        }
    }
}

Add-Category 'processes' 'Laufende Prozesse' {
    param($items)
    $running = @{}
    foreach ($process in Get-Process) {
        $path = $null
        try { $path = $process.Path } catch { $path = $null }
        if ($process.ProcessName -match $Pattern -or ($path -and $path -match $Pattern)) {
            $name = '{0}.exe | {1}' -f $process.ProcessName, $path
            $running[$name] = 1 + [int]$running[$name]
        }
    }
    foreach ($name in $running.Keys) { $items[$name] = 'läuft ({0}x)' -f $running[$name] }
}

# ---------------------------------------------------------------------------
# Ausgabe
# ---------------------------------------------------------------------------

function Get-RegistryNumber {
    param([string] $Path, [string] $Name)
    $key = $hives['HKLM'].OpenSubKey($Path, $false)
    if (-not $key) { return $null }
    try { return $key.GetValue($Name) } finally { $key.Close() }
}

$identity = [Security.Principal.WindowsIdentity]::GetCurrent()
$os = $null
try { $os = Get-CimInstance -ClassName Win32_OperatingSystem } catch { $os = $null }
$searchService = Get-Service -Name WSearch -ErrorAction SilentlyContinue

$meta = [ordered]@{
    label         = $Label
    created       = (Get-Date).ToString('o')
    computer      = $env:COMPUTERNAME
    user          = $identity.Name
    elevated      = ([Security.Principal.WindowsPrincipal]$identity).IsInRole([Security.Principal.WindowsBuiltInRole]::Administrator)
    os            = if ($os) { '{0} {1}' -f $os.Caption, $os.Version } else { [Environment]::OSVersion.VersionString }
    powershell    = $PSVersionTable.PSVersion.ToString()
    uacEnableLua  = Get-RegistryNumber -Path 'SOFTWARE\Microsoft\Windows\CurrentVersion\Policies\System' -Name 'EnableLUA'
    explorer      = [bool](Get-Process -Name explorer -ErrorAction SilentlyContinue)
    windowsSearch = if ($searchService) { $searchService.Status.ToString() } else { 'nicht installiert' }
    pattern       = $Pattern
}

$document = [ordered]@{ schema = 1; meta = $meta; categories = $categories }
$json = $document | ConvertTo-Json -Depth 8
# relativ zum PowerShell-Ort auflösen, nicht zum Prozessverzeichnis
$outPath = $ExecutionContext.SessionState.Path.GetUnresolvedProviderPathFromPSPath($OutFile)
$outDir = Split-Path -Parent $outPath
if (-not (Test-Path -LiteralPath $outDir)) { New-Item -ItemType Directory -Path $outDir -Force | Out-Null }
[IO.File]::WriteAllText($outPath, $json, (New-Object System.Text.UTF8Encoding $false))

$total = 0
foreach ($category in $categories.Values) { $total += $category.items.Count }
Write-Host ('Schnappschuss "{0}": {1} Einträge in {2} Kategorien -> {3}' -f $Label, $total, $categories.Count, $outPath)
