<#
.SYNOPSIS
  Signiert Windows-Binärdateien mit dem DigiCert-Code-Signing-Zertifikat von BW-Tech
  oder prüft vorhandene Signaturen.

.DESCRIPTION
  Der private Schlüssel liegt bei DigiCert KeyLocker (Cloud-HSM) und verlässt ihn nie.
  signtool erreicht ihn über den DigiCert Signing Manager KSP, nachdem
  `smctl windows certsync` das Zertifikat in den Zertifikatsspeicher des Benutzers
  gelegt hat (siehe .github/workflows/main.yml, Schritt „Code signing setup“).
  Das Zertifikat wird über den Antragstellernamen (CRAFT_CODESIGN_COMMONNAME,
  Teilzeichenfolge, Vorgabe „BW-TECH GmbH“) ausgewählt; ohne Angabe nimmt
  signtool das beste verfügbare Code-Signing-Zertifikat (/a).

  Jede Signatur trägt einen RFC-3161-Zeitstempel von DigiCert, damit sie nach
  Ablauf des Zertifikats gültig bleibt.

.EXAMPLE
  .\codesign.ps1 -Sign .\setup.exe
  .\codesign.ps1 -Verify .\setup.exe, .\client.exe
#>
param(
    [string[]]$Sign = @(),
    [string[]]$Verify = @()
)

$ErrorActionPreference = 'Stop'

function Find-SignTool {
    $cmd = Get-Command signtool.exe -ErrorAction SilentlyContinue
    if ($cmd) { return $cmd.Source }
    $kits = Get-ChildItem 'C:\Program Files (x86)\Windows Kits\10\bin\*\x64\signtool.exe' -ErrorAction SilentlyContinue | Sort-Object FullName
    if ($kits) { return ($kits | Select-Object -Last 1).FullName }
    throw 'signtool.exe nicht gefunden – Windows SDK fehlt.'
}

$signtool = Find-SignTool
$timestamp = 'http://timestamp.digicert.com'
$subject = $env:CRAFT_CODESIGN_COMMONNAME
if (-not $subject) { $subject = 'BW-TECH GmbH' }

foreach ($file in $Sign) {
    if (-not (Test-Path $file)) { throw "Datei fehlt: $file" }
    # Schon mit unserem Zertifikat signiert: nicht noch einmal (jede Signatur zählt im Kontingent)
    $vorhanden = Get-AuthenticodeSignature $file
    if ($vorhanden.Status -eq 'Valid' -and $vorhanden.SignerCertificate.Subject -like "*$subject*") {
        Write-Host "bereits signiert, übersprungen: $file"
        continue
    }
    & $signtool sign /tr $timestamp /td SHA256 /fd SHA256 /a /n $subject /v $file
    if ($LASTEXITCODE -ne 0) { throw "Signieren fehlgeschlagen: $file" }
}

foreach ($file in $Verify) {
    if (-not (Test-Path $file)) { throw "Datei fehlt: $file" }
    & $signtool verify /pa /v $file
    if ($LASTEXITCODE -ne 0) { throw "Signatur fehlt oder ist ungültig: $file" }
}
