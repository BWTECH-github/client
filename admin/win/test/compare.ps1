<#
.SYNOPSIS
    Vergleicht zwei Schnappschüsse von snapshot.ps1 (vorher -> nachher).

.DESCRIPTION
    Rein lesend. Meldet je Kategorie neue, entfernte und geänderte Einträge.

    Ohne -PassThru schreibt das Skript einen lesbaren Bericht in die Konsole
    (mit -OutFile zusätzlich als Markdown-Datei) und endet mit Exit-Code 0
    (keine Unterschiede) oder 1 (Unterschiede). Im Bericht werden die
    Kindeinträge neuer oder entfernter Behälter (Registry-Schlüssel,
    Verzeichnisse) zu einer Zahl zusammengefasst.

    Mit -PassThru gibt das Skript alle Unterschiede ungekürzt als Objekte
    zurück (Category, Label, Change, Key, Before, After) und setzt keinen
    Exit-Code; -OutFile schreibt den Bericht trotzdem. So nutzt probe.ps1 das
    Skript.

    Läuft unter Windows PowerShell 5.1 und PowerShell 7.

.PARAMETER Before
    Schnappschuss vorher (JSON von snapshot.ps1).

.PARAMETER After
    Schnappschuss nachher.

.PARAMETER Category
    Nur diese Kategorien vergleichen (z. B. reg.shellext, files.program).

.PARAMETER ExcludeCategory
    Diese Kategorien auslassen (z. B. processes).

.PARAMETER OutFile
    Bericht zusätzlich als Markdown-Datei (UTF-8) schreiben.

.PARAMETER MaxLines
    Höchstzahl Zeilen je Kategorie im Bericht; der Rest wird nur gezählt.

.EXAMPLE
    .\compare.ps1 C:\probe\01-ausgang.json C:\probe\03-deinstalliert.json

.EXAMPLE
    .\compare.ps1 A.json B.json -Category reg.shellext, reg.run -OutFile diff.md
#>
[CmdletBinding()]
param(
    [Parameter(Mandatory = $true, Position = 0)]
    [string] $Before,

    [Parameter(Mandatory = $true, Position = 1)]
    [string] $After,

    [string[]] $Category = @(),

    [string[]] $ExcludeCategory = @(),

    [string] $OutFile,

    [ValidateRange(1, 1000000)]
    [int] $MaxLines = 300,

    [switch] $PassThru
)

$ErrorActionPreference = 'Stop'
Set-StrictMode -Version 2.0

# ausdrücklich übergebenes $null wie "nicht angegeben" behandeln
$Category = @($Category | Where-Object { $_ })
$ExcludeCategory = @($ExcludeCategory | Where-Object { $_ })

# So kennzeichnet snapshot.ps1 Behälter; Verzeichnisse tragen dahinter ggf.
# Attribut-Hinweise wie " [Reparse]"
$containerPattern = '^\((Schlüssel|Verzeichnis)\)'
# lange Werte (Befehlszeilen, MULTI_SZ) im Bericht kürzen
$valueLimit = 300

function Read-Snapshot {
    param([string] $Path)
    $full = $ExecutionContext.SessionState.Path.GetUnresolvedProviderPathFromPSPath($Path)
    $document = [IO.File]::ReadAllText($full, [Text.Encoding]::UTF8) | ConvertFrom-Json
    if (-not $document.PSObject.Properties['schema'] -or $document.schema -ne 1) {
        throw "$Path ist kein Schnappschuss von snapshot.ps1 (schema 1)."
    }
    $categories = [ordered]@{}
    foreach ($category in $document.categories.PSObject.Properties) {
        $items = New-Object 'System.Collections.Generic.Dictionary[string,string]' ([StringComparer]::OrdinalIgnoreCase)
        foreach ($item in $category.Value.items.PSObject.Properties) {
            $items[$item.Name] = [string]$item.Value
        }
        $categories[$category.Name] = [pscustomobject]@{ Label = [string]$category.Value.label; Items = $items }
    }
    return [pscustomobject]@{ Label = [string]$document.meta.label; Categories = $categories }
}

# Behälter, zu denen ein Eintrag gehört, vom nächsten zum entferntesten:
# "Schlüssel :: Wert" gehört zum Schlüssel, ein Pfad zu seinen Vorfahren.
function Get-ContainerPath {
    param([string] $Key)
    $path = $Key
    $separator = $path.IndexOf(' :: ')
    if ($separator -ge 0) {
        $path = $path.Substring(0, $separator)
        $path
    }
    while (($cut = $path.LastIndexOf('\')) -gt 0) {
        $path = $path.Substring(0, $cut)
        $path
    }
}

function Format-Value {
    param([string] $Value)
    if ($null -eq $Value) { return '' }
    # Codeblöcke im Markdown nicht aufbrechen
    $text = $Value -replace '```', "'''"
    if ($text.Length -gt $valueLimit) { $text = $text.Substring(0, $valueLimit) + ' …' }
    return $text
}

$old = Read-Snapshot $Before
$new = Read-Snapshot $After
$empty = New-Object 'System.Collections.Generic.Dictionary[string,string]' ([StringComparer]::OrdinalIgnoreCase)

$ids = New-Object 'System.Collections.Generic.List[string]'
foreach ($id in @($old.Categories.Keys) + @($new.Categories.Keys)) {
    if ($ids.Contains($id)) { continue }
    if ($Category.Count -gt 0 -and $Category -notcontains $id) { continue }
    if ($ExcludeCategory -contains $id) { continue }
    $ids.Add($id)
}

$diffs = New-Object 'System.Collections.Generic.List[object]'
foreach ($id in $ids) {
    $label = $id
    $oldItems = $empty
    $newItems = $empty
    if ($old.Categories.Contains($id)) {
        $oldItems = $old.Categories[$id].Items
        $label = $old.Categories[$id].Label
    }
    if ($new.Categories.Contains($id)) {
        $newItems = $new.Categories[$id].Items
        $label = $new.Categories[$id].Label
    }
    $keys = New-Object 'System.Collections.Generic.SortedSet[string]' ([StringComparer]::OrdinalIgnoreCase)
    foreach ($key in $oldItems.Keys) { [void]$keys.Add($key) }
    foreach ($key in $newItems.Keys) { [void]$keys.Add($key) }
    foreach ($key in $keys) {
        $inOld = $oldItems.ContainsKey($key)
        $inNew = $newItems.ContainsKey($key)
        if ($inOld -and $inNew) {
            if ([string]::Equals($oldItems[$key], $newItems[$key], [StringComparison]::Ordinal)) { continue }
            $change = 'geändert'
        } elseif ($inNew) {
            $change = 'neu'
        } else {
            $change = 'entfernt'
        }
        $diffs.Add([pscustomobject]@{
                Category = $id
                Label    = $label
                Change   = $change
                Key      = $key
                Before   = $(if ($inOld) { $oldItems[$key] } else { $null })
                After    = $(if ($inNew) { $newItems[$key] } else { $null })
            })
    }
}

# ---------------------------------------------------------------------------
# Bericht
# ---------------------------------------------------------------------------

$counts = @{ 'neu' = 0; 'entfernt' = 0; 'geändert' = 0 }
foreach ($diff in $diffs) { $counts[$diff.Change]++ }

$lines = New-Object 'System.Collections.Generic.List[string]'
$lines.Add(('# Vergleich: {0} -> {1}' -f $old.Label, $new.Label))
$lines.Add('')
if ($diffs.Count -eq 0) {
    $lines.Add('Keine Unterschiede.')
} else {
    $lines.Add(('{0} Unterschiede: {1} neu, {2} entfernt, {3} geändert.' -f $diffs.Count, $counts['neu'], $counts['entfernt'], $counts['geändert']))
}

foreach ($id in $ids) {
    $categoryDiffs = @($diffs | Where-Object { $_.Category -eq $id })
    if ($categoryDiffs.Count -eq 0) { continue }

    # neue/entfernte Behälter; ihre Kindeinträge derselben Art werden gezählt
    $containers = @{}
    foreach ($diff in $categoryDiffs) {
        if ($diff.Change -eq 'geändert') { continue }
        $value = if ($diff.Change -eq 'neu') { $diff.After } else { $diff.Before }
        if ($value -match $containerPattern) { $containers["$($diff.Change)|$($diff.Key)"] = 0 }
    }
    $shown = New-Object 'System.Collections.Generic.List[object]'
    foreach ($diff in $categoryDiffs) {
        $outer = $null
        if ($diff.Change -ne 'geändert') {
            foreach ($parent in Get-ContainerPath $diff.Key) {
                if ($containers.ContainsKey("$($diff.Change)|$parent")) { $outer = "$($diff.Change)|$parent" }
            }
        }
        if ($outer) { $containers[$outer]++ } else { $shown.Add($diff) }
    }

    $lines.Add('')
    $lines.Add(('## {0} (`{1}`): {2} Unterschiede' -f $categoryDiffs[0].Label, $id, $categoryDiffs.Count))
    $lines.Add('')
    $lines.Add('```diff')
    $written = 0
    foreach ($diff in $shown) {
        if ($written -ge $MaxLines) { break }
        $written++
        $key = Format-Value $diff.Key
        switch ($diff.Change) {
            'neu' { $sign = '+'; $text = Format-Value $diff.After }
            'entfernt' { $sign = '-'; $text = Format-Value $diff.Before }
            default { $sign = '~'; $text = '{0} -> {1}' -f (Format-Value $diff.Before), (Format-Value $diff.After) }
        }
        $children = 0
        if ($containers.ContainsKey("$($diff.Change)|$($diff.Key)")) { $children = $containers["$($diff.Change)|$($diff.Key)"] }
        if ($children -gt 0) {
            $lines.Add(('{0} {1} = {2} (+ {3} Einträge darin)' -f $sign, $key, $text, $children))
        } else {
            $lines.Add(('{0} {1} = {2}' -f $sign, $key, $text))
        }
    }
    if ($shown.Count -gt $written) {
        $lines.Add(('… {0} weitere Zeilen nicht gezeigt (-MaxLines)' -f ($shown.Count - $written)))
    }
    $lines.Add('```')
}

if ($OutFile) {
    $outPath = $ExecutionContext.SessionState.Path.GetUnresolvedProviderPathFromPSPath($OutFile)
    $outDir = Split-Path -Parent $outPath
    if ($outDir -and -not (Test-Path -LiteralPath $outDir)) { New-Item -ItemType Directory -Path $outDir -Force | Out-Null }
    [IO.File]::WriteAllText($outPath, ($lines -join "`n") + "`n", (New-Object System.Text.UTF8Encoding $false))
}

if ($PassThru) {
    return $diffs
}

foreach ($line in $lines) { Write-Host $line }
if ($diffs.Count -gt 0) { exit 1 }
exit 0
