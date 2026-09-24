# monitor.ps1 - oeffnet die Konsole des Spritpreis-Monitors (CYD)
#
# Aufruf aus dem Projektordner:      .\tools\monitor.ps1
# Oder mit anderem Port:             .\tools\monitor.ps1 -Port COM6
#
# Das Skript aktiviert die ESP-IDF-Umgebung und startet den seriellen Monitor.
# Beenden des Monitors: Strg + ]
#
# Hinweis: Im Stromsparmodus schlaeft die CPU. Vor dem ersten Zeichen einer
# Eingabe deshalb einmal Enter druecken - sonst geht das erste Zeichen verloren.

param(
    [string]$Port = "COM4"
)

$ErrorActionPreference = "Stop"

# Projektordner = ein Verzeichnis ueber diesem Skript
$projectDir = Split-Path -Parent $PSScriptRoot

# ESP-IDF aktivieren (Skript in die laufende Sitzung einbinden - der Punkt ist wichtig)
$activate = Join-Path $projectDir "activate-esp-idf.ps1"
if (-not (Test-Path $activate)) {
    Write-Error "activate-esp-idf.ps1 nicht gefunden: $activate"
    return
}
. $activate

# PATH entdoppeln: mehrfaches Aktivieren laesst den PATH so weit wachsen,
# dass Windows den Prozessstart verweigert (Build/Flash laufen dann still ins Leere)
$env:PATH = (($env:PATH -split ';' | Where-Object { $_ -ne '' }) | Select-Object -Unique) -join ';'

# Verwaiste Monitor-Prozesse beenden - sie belegen den Port
Get-CimInstance Win32_Process -Filter "Name like '%python%'" |
    Where-Object { $_.CommandLine -match 'idf_monitor' } |
    ForEach-Object { Stop-Process -Id $_.ProcessId -Force }

Set-Location $projectDir
Write-Host "Projekt: $projectDir"
Write-Host "Port   : $Port   (Beenden mit Strg + ])"
idf.py -p $Port monitor
