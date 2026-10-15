# owncloud.online: Aufräumlauf beim Deinstallieren (aufgerufen von uninstall-helper.nsi).
#
# Läuft erhöht in der 64-Bit-PowerShell, nachdem der Client beendet und die
# Programmdateien entfernt wurden. Der Helfer übergibt das Skript als ScriptBlock
# über -Command, damit eine Ausführungsrichtlinie für Skriptdateien nicht greift.
#
# 1. Sync-Roots (Windows-Platzhalter, Cloud Files API) des Clients abmelden, für
#    alle Benutzer - ohne Client sind die Platzhalter für alle unbrauchbar
#    (Audit H4/H5). Beim Abmelden löscht Windows die nur online verfügbaren
#    Platzhalter (die Dateien bleiben auf dem Server) und macht vollständig
#    geladene Dateien zu normalen Dateien; so verfährt auch der Client, wenn ein
#    Ordner entfernt wird (VfsWin::unregisterFolder).
#    VORHER wird die Sync-Datenbank im Ordner gelöscht: Eine Datenbank, die die
#    entfernten Platzhalter noch kennt, hielte sie nach einer Neuinstallation mit
#    behaltenen Einstellungen für lokal gelöscht und löschte sie auf dem Server.
#    Ohne Datenbank gleicht der Client neu ab und löscht nichts. Lässt sich die
#    Datenbank nicht löschen, bleibt der Sync-Root angemeldet.
# 2. -SyncDb: Sync-Datenbanken und Protokolle in allen Sync-Ordnern des
#    angemeldeten Benutzers löschen (laut Konfiguration).
# Dateien der Nutzer werden nie gelöscht. Ausgaben landen im Protokoll des Helfers.
param([switch] $SyncDb)

Set-StrictMode -Version 2.0
$ErrorActionPreference = 'Stop'

$app = 'owncloud.online'
# Sync-Datenbank (SyncJournalDb::makeDbName) samt SQLite-Begleitdateien, ältere Namen eingeschlossen
$journalPattern = '^(\._?sync_[^\\/]+|\.csync_journal)\.db(-wal|-shm|-journal)?$'
$syncRootBase = 'HKLM:\SOFTWARE\Microsoft\Windows\CurrentVersion\Explorer\SyncRootManager'

# Wert aus der Qt-INI-Datei: Anführungszeichen und Escapes (\\, \", \xHHHH, ...) auflösen
function ConvertFrom-IniValue([string] $Raw) {
    $value = $Raw.Trim()
    if ($value.Length -ge 2 -and $value.StartsWith('"') -and $value.EndsWith('"')) {
        $value = $value.Substring(1, $value.Length - 2)
    }
    return [regex]::Replace($value, '\\(x[0-9A-Fa-f]{1,4}|.)', {
            param($match)
            $token = $match.Groups[1].Value
            if ($token.Length -gt 1) { return [string][char][Convert]::ToInt32($token.Substring(1), 16) }
            switch -CaseSensitive ($token) {
                'n' { return "`n" }
                'r' { return "`r" }
                't' { return "`t" }
                '0' { return "`0" }
                default { return $token }
            }
        })
}

# Sync-Ordner aus der Konfiguration des angemeldeten Benutzers
function Get-ConfiguredFolders {
    $file = Join-Path ([Environment]::GetFolderPath('ApplicationData')) "$app\$app.cfg"
    if (-not (Test-Path -LiteralPath $file -PathType Leaf)) { return @() }
    $folders = foreach ($line in [IO.File]::ReadAllLines($file)) {
        if ($line -match '\\localPath=(.*)$') { (ConvertFrom-IniValue $Matches[1]) -replace '/', '\' }
    }
    return @($folders | Where-Object { $_ } | Select-Object -Unique)
}

# Sync-Datenbank (und mit -WithLog das Protokoll) im Wurzelverzeichnis eines
# Sync-Ordners löschen. $true, wenn danach keine Datenbank mehr dort liegt.
function Remove-Journal([string] $Folder, [switch] $WithLog) {
    if (-not $Folder -or -not (Test-Path -LiteralPath $Folder -PathType Container)) { return $true }
    $files = @(Get-ChildItem -LiteralPath $Folder -Force -File -ErrorAction SilentlyContinue |
            Where-Object { $_.Name -match $journalPattern -or ($WithLog -and $_.Name -eq '.owncloudsync.log') })
    $clean = $true
    foreach ($file in $files) {
        try {
            Remove-Item -LiteralPath $file.FullName -Force
            Write-Host "  deleted: $($file.FullName)"
        } catch {
            if ($file.Name -match $journalPattern) { $clean = $false }
            Write-Host "  not deleted: $($file.FullName) ($($_.Exception.Message))"
        }
    }
    return $clean
}

function Unregister-SyncRoots {
    $roots = @(Get-ChildItem -LiteralPath $syncRootBase -ErrorAction SilentlyContinue | Where-Object {
            $_.PSChildName.StartsWith("$app!", [StringComparison]::OrdinalIgnoreCase) -or
            $_.PSChildName.StartsWith("${app}_", [StringComparison]::OrdinalIgnoreCase)
        })
    if ($roots.Count -eq 0) {
        Write-Host 'Sync roots: none registered'
        return
    }
    try {
        $manager = [Type]::GetType('Windows.Storage.Provider.StorageProviderSyncRootManager, Windows.Storage, ContentType=WindowsRuntime', $true)
    } catch {
        Write-Host "Sync roots: Windows API not available, kept registered ($($_.Exception.Message))"
        return
    }
    $currentSid = [Security.Principal.WindowsIdentity]::GetCurrent().User.Value
    foreach ($root in $roots) {
        $id = $root.PSChildName
        Write-Host "Sync root $id"
        $paths = @{}
        $userRoots = Join-Path $root.PSPath 'UserSyncRoots'
        if (Test-Path -LiteralPath $userRoots) {
            $key = Get-Item -LiteralPath $userRoots
            foreach ($sid in $key.GetValueNames()) {
                if ($sid) { $paths[$sid] = [string]$key.GetValue($sid) }
            }
        }
        $namespace = [string]$root.GetValue('NamespaceCLSID')
        $clean = $true
        foreach ($path in $paths.Values) {
            Write-Host "  folder: $path"
            if (-not (Remove-Journal $path)) { $clean = $false }
        }
        if (-not $clean) {
            Write-Host '  kept registered: the sync database could not be deleted'
            continue
        }
        try {
            $manager::Unregister($id)
            Write-Host '  unregistered (online-only placeholders removed, downloaded files kept)'
        } catch {
            Write-Host "  unregister failed: $($_.Exception.Message)"
            continue
        }
        # Eintrag im Navigationsbereich des Explorers, falls Windows ihn stehen lässt -
        # nur im eigenen Profil erreichbar; die Kennung muss eine CLSID sein, sonst
        # träfe der Pfad den ganzen CLSID-Zweig
        if ($paths.ContainsKey($currentSid) -and $namespace -match '^\{[0-9A-Fa-f]{8}-[0-9A-Fa-f]{4}-[0-9A-Fa-f]{4}-[0-9A-Fa-f]{4}-[0-9A-Fa-f]{12}\}$') {
            foreach ($path in "HKCU:\Software\Classes\CLSID\$namespace",
                              "HKCU:\Software\Microsoft\Windows\CurrentVersion\Explorer\Desktop\NameSpace\$namespace") {
                if (Test-Path -LiteralPath $path) {
                    Remove-Item -LiteralPath $path -Recurse -Force -ErrorAction SilentlyContinue
                    Write-Host "  removed: $path"
                }
            }
            Remove-ItemProperty -LiteralPath 'HKCU:\Software\Microsoft\Windows\CurrentVersion\Explorer\HideDesktopIcons\NewStartPanel' -Name $namespace -ErrorAction SilentlyContinue
        }
    }
}

try {
    Unregister-SyncRoots
    if ($SyncDb) {
        foreach ($folder in Get-ConfiguredFolders) {
            Write-Host "Sync folder $folder"
            $null = Remove-Journal $folder -WithLog
        }
    }
} catch {
    Write-Host "Clean-up error: $($_.Exception.Message)"
    exit 1
}
