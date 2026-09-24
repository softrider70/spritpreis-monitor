<#
.SYNOPSIS
  Sucht die Tankstellen-ID (UUID) fuer die Tankerkoenig-API.

.DESCRIPTION
  Ruft die freie Tankerkoenig-API (Daten der Markttransparenzstelle fuer
  Kraftstoffe) mit einem Umkreis ab und listet die gefundenen Tankstellen
  samt ID auf. Mit -Ort wird der Ort vorher ueber OpenStreetMap (Nominatim)
  in Koordinaten umgerechnet.

  Den API-Key gibt es kostenlos auf https://creativecommons.tankerkoenig.de/
  (Anmeldung verlangt Vor- und Nachnamen, weil die Behoerde das fordert).

.EXAMPLE
  .\find_station.ps1 -ApiKey <key> -Ort "<Strasse> <PLZ> <Ort>"

.EXAMPLE
  .\find_station.ps1 -ApiKey <key> -Lat 50.0000 -Lng 8.0000 -Rad 3 -Filter "Bru"
#>
param(
    [Parameter(Mandatory = $true)][string]$ApiKey,
    [string]$Ort,
    [double]$Lat = 0,
    [double]$Lng = 0,
    [double]$Rad = 5,
    [string]$Filter = ""
)

$ErrorActionPreference = "Stop"
$ua = @{ "User-Agent" = "spritpreis-monitor/0.1 (privates Projekt)" }

if ($Ort) {
    $geoUrl = "https://nominatim.openstreetmap.org/search?format=json&limit=1&q=" + [uri]::EscapeDataString($Ort)
    Write-Host "Suche Koordinaten fuer '$Ort' ..." -ForegroundColor Cyan
    $geo = Invoke-RestMethod -Uri $geoUrl -Headers $ua -TimeoutSec 20
    if (-not $geo -or $geo.Count -eq 0) {
        throw "Ort nicht gefunden: $Ort"
    }
    $Lat = [double]$geo[0].lat
    $Lng = [double]$geo[0].lon
    Write-Host ("Koordinaten: {0:N5}, {1:N5} ({2})" -f $Lat, $Lng, $geo[0].display_name)
}

if ($Lat -eq 0 -and $Lng -eq 0) {
    throw "Bitte -Ort oder -Lat/-Lng angeben."
}

$url = "https://creativecommons.tankerkoenig.de/json/list.php?lat=$Lat&lng=$Lng&rad=$Rad&sort=dist&type=all&apikey=$ApiKey"
Write-Host "Frage Tankerkoenig-API ab (Umkreis $Rad km) ..." -ForegroundColor Cyan

$resp = Invoke-RestMethod -Uri $url -Headers @{ "Accept" = "application/json" } -TimeoutSec 30
if (-not $resp.ok) {
    throw "API meldet Fehler: $($resp.message)"
}

$stationen = $resp.stations
if ($Filter) {
    $stationen = $stationen | Where-Object {
        $_.name -match $Filter -or $_.street -match $Filter -or $_.place -match $Filter
    }
}

if (-not $stationen) {
    Write-Warning "Keine Treffer. Umkreis vergroessern oder Filter weglassen."
    return
}

Write-Host ""
$stationen |
    Select-Object @{n = "km"; e = { $_.dist } }, name, brand, street, houseNumber, postCode, place, id |
    Format-Table -AutoSize

Write-Host "Lizenz der Daten: CC BY 4.0 - Quelle: MTS-K / Tankerkoenig" -ForegroundColor DarkGray
Write-Host "Die passende ID mit 'station <uuid>' auf dem Geraet setzen (Konsole)." -ForegroundColor Green
