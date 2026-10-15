# Diagnose für WinVfsTest im Windows-Runner: Cloud-Files-Umgebung ausgeben, dann
# jede Testfunktion einzeln mit Zeitlimit ausführen. Hängt eine Funktion, werden
# die Thread-Stacks des Testprozesses per cdb (nicht-invasiv) mitgeschrieben.
# Läuft über `.craft.ps1 -c --run pwsh <Skript> <buildDir>`, damit Qt und die
# Client-DLLs wie unter ctest im PATH liegen.
param(
    [Parameter(Mandatory = $true)][string]$BuildDir,
    [int]$TimeoutSec = 90
)

$ErrorActionPreference = 'Continue'
$bin = Join-Path $BuildDir 'bin'
$exe = Join-Path $bin 'WinVfsTest.exe'

function Write-Short([string[]]$Lines) {
    foreach ($l in $Lines) { if ($l.Length -gt 260) { $l.Substring(0, 260) } else { $l } }
}

Write-Host '== Umgebung'
$os = Get-CimInstance Win32_OperatingSystem
Write-Host "$($os.Caption) $($os.Version)"
$admin = ([Security.Principal.WindowsPrincipal][Security.Principal.WindowsIdentity]::GetCurrent()).IsInRole([Security.Principal.WindowsBuiltInRole]::Administrator)
Write-Host "Administrator: $admin  TEMP: $env:TEMP"
foreach ($svc in 'cldflt', 'wsearch') {
    $state = (sc.exe query $svc | Select-String 'STATE|FAILED' | Select-Object -First 1)
    $start = (sc.exe qc $svc | Select-String 'START_TYPE' | Select-Object -First 1)
    Write-Host "$svc :: $state :: $start"
}
Write-Short (fltmc filters 2>&1 | Select-String -Pattern 'Filter Name|cldflt' | ForEach-Object { $_.Line })
Write-Short (fltmc instances -f cldflt 2>&1 | Select-Object -First 8 | ForEach-Object { "$_" })

if (-not (Test-Path $exe)) {
    Write-Host "Nicht gefunden: $exe"
    exit 0
}

$functions = & $exe -functions 2>$null | Where-Object { $_ -match '^\w+\(\)$' } | ForEach-Object { $_ -replace '\(\)$', '' }
Write-Host "== $($functions.Count) Testfunktionen, Zeitlimit je $TimeoutSec s"

$out = Join-Path $env:RUNNER_TEMP 'winvfs'
New-Item -ItemType Directory -Force $out | Out-Null
$cdb = Join-Path ${env:ProgramFiles(x86)} 'Windows Kits\10\Debuggers\x64\cdb.exe'
Write-Host "cdb vorhanden: $(Test-Path $cdb)"
$dumps = 0

foreach ($fn in $functions) {
    $log = Join-Path $out "$fn.log"
    $err = Join-Path $out "$fn.err"
    $sw = [Diagnostics.Stopwatch]::StartNew()
    $p = Start-Process -FilePath $exe -ArgumentList $fn -WorkingDirectory $bin -RedirectStandardOutput $log -RedirectStandardError $err -NoNewWindow -PassThru
    $null = $p.Handle # sonst liefert ExitCode unter PowerShell nichts
    if ($p.WaitForExit($TimeoutSec * 1000)) {
        $totals = Select-String -Path $log -Pattern '^Totals:' | Select-Object -Last 1
        Write-Host ("{0} exit={1} {2:n1}s {3}" -f $fn, $p.ExitCode, $sw.Elapsed.TotalSeconds, $totals.Line)
        if ($p.ExitCode -ne 0) {
            Write-Short (Select-String -Path $log -Pattern '^(FAIL!|QFATAL|   Loc:)' | Select-Object -First 12 | ForEach-Object { $_.Line })
            Write-Short (Get-Content $log -Tail 30)
            Write-Short (Get-Content $err -Tail 10)
        }
        continue
    }

    Write-Host "$fn HAENGT nach $TimeoutSec s, letzte Ausgabe:"
    Write-Short (Get-Content $log -Tail 40)
    if ($dumps -lt 2 -and (Test-Path $cdb)) {
        $dumps++
        $stacks = Join-Path $out "$fn.stacks.txt"
        $sym = "srv*$out\sym*https://msdl.microsoft.com/download/symbols;$bin"
        $d = Start-Process -FilePath $cdb -ArgumentList "-pv -p $($p.Id) -y `"$sym`" -c `"~*kc 40; q`"" -RedirectStandardOutput $stacks -NoNewWindow -PassThru
        if (-not $d.WaitForExit(300000)) { Stop-Process -Id $d.Id -Force -ErrorAction SilentlyContinue }
        # Nur Threads mit Client-, Qt- oder CfAPI-Frames ausgeben (Thread 0 immer).
        $text = Get-Content $stacks -Raw
        foreach ($block in [regex]::Split($text, '(?m)^(?=[ .#]*\d+\s+Id:)')) {
            if ($block -match '^[ .#]*0\s+Id:' -or $block -match 'WinVfsTest|owncloud|vfs_|cldapi|Qt6|!Cf[A-Z]') {
                Write-Short (($block -split "`r?`n") | Select-Object -First 32)
            }
        }
    }
    Stop-Process -Id $p.Id -Force -ErrorAction SilentlyContinue
}
exit 0
