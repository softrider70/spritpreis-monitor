<#
.SYNOPSIS
    Holt Archiv-CSV und Logfile vom Spritpreis-Monitor ueber die serielle
    Schnittstelle auf den PC.

.DESCRIPTION
    Das Geraet kann die Dateien von der microSD selbst auf die Konsole
    ausgeben (Befehl "file csv" bzw. "file log"). Dieses Skript oeffnet den
    COM-Port, schickt die beiden Befehle und schreibt die Antworten in
    Dateien im Zielordner. Damit braucht man die Karte nicht herauszunehmen.

    Der DTR-Pin wird NICHT gesetzt: Sonst startet das Geraet beim Oeffnen des
    Ports neu (ueber den Kondensator am Reset-Pin) und die Ausgabe waere mit
    Boot-Meldungen vermischt.

.BEISPIEL
    .\hole_dateien.ps1
    .\hole_dateien.ps1 -Port COM4 -Ziel .\vom_geraet
#>

param(
    [string]$Port = "COM4",
    [string]$Ziel = "vom_geraet",
    [int]$Baud = 115200,
    [int]$WarteSekunden = 90
)

$ErrorActionPreference = "Stop"

if (-not (Test-Path $Ziel)) {
    New-Item -ItemType Directory -Path $Ziel | Out-Null
}

$stempel = Get-Date -Format "yyyy-MM-dd"

$p = New-Object System.IO.Ports.SerialPort($Port, $Baud, "None", 8, "One")
$p.DtrEnable = $false
$p.RtsEnable = $false
$p.ReadTimeout = 500
$p.WriteTimeout = 2000

Write-Host "Oeffne $Port ..."
$p.Open()

function Get-Datei {
    param([string]$was, [string]$dateiname)

    # Aufwecken: Im Stromsparmodus gehen die ersten Zeichen einer Eingabe
    # verloren (die CPU schlaeft noch, waehrend die UART den Rest empfaengt).
    # Deshalb wird mehrfach ein Enter geschickt - das verbraucht kein
    # Kommando - und danach gewartet.
    for ($i = 0; $i -lt 3; $i++) {
        $p.Write("`r")
        Start-Sleep -Milliseconds 400
    }
    $p.DiscardInBuffer()

    # Zweimal versuchen: Der erste Befehl kann trotzdem im Aufwachen verloren
    # gehen.
    foreach ($versuch in 1..2) {
        $p.WriteLine("file $was")

        $sb = New-Object System.Text.StringBuilder
        $frist = (Get-Date).AddSeconds($WarteSekunden)
        $angefangen = $false
        $fertig = $false

        while ((Get-Date) -lt $frist -and -not $fertig) {
            $stueck = $p.ReadExisting()
            if ($stueck) {
                [void]$sb.Append($stueck)
                $text = $sb.ToString()
                if ($text -match "----- Anfang") { $angefangen = $true }
                if ($angefangen -and $text -match "----- Ende") { $fertig = $true }
            }
            else {
                Start-Sleep -Milliseconds 200
                # Nach 8 s ohne Anfang einen zweiten Versuch starten
                if (-not $angefangen -and $versuch -eq 1 -and
                    ((Get-Date) -gt $frist.AddSeconds(-($WarteSekunden - 8)))) {
                    break
                }
            }
        }

        $text = $sb.ToString()
        $muster = "(?s)----- Anfang [^\r\n]*\r?\n(.*?)----- Ende"
        if ($text -match $muster) {
            $inhalt = $Matches[1]
            $pfad = Join-Path $Ziel $dateiname
            # Zeilenenden vereinheitlichen und in UTF-8 ablegen
            Set-Content -Path $pfad -Value $inhalt -NoNewline -Encoding UTF8
            $groesse = (Get-Item $pfad).Length
            Write-Host ("gespeichert: {0} ({1} Byte)" -f $pfad, $groesse)
            return
        }

        if ($versuch -eq 1) {
            Write-Host "Keine Antwort - zweiter Versuch ..."
            $p.DiscardInBuffer()
            $p.Write("`r")
            Start-Sleep -Milliseconds 500
        }
        else {
            Write-Warning "Keine vollstaendige Ausgabe fuer '$was' empfangen."
            Write-Warning "Antwort war: $($text.Trim())"
        }
    }
}

try {
    # Das Oeffnen des Ports setzt bei diesen Boards den Reset-Pin (DTR/RTS
    # lassen sich vor dem Oeffnen nicht wirksam vorbelegen) - das Geraet
    # startet also neu. Deshalb wird gewartet, bis die Konsole wieder bereit
    # ist; die Boot-Meldungen werden verworfen.
    Write-Host "Warte auf den Neustart des Geraets ..."
    $sb = New-Object System.Text.StringBuilder
    $frist = (Get-Date).AddSeconds(25)
    while ((Get-Date) -lt $frist) {
        $stueck = $p.ReadExisting()
        if ($stueck) {
            [void]$sb.Append($stueck)
            if ($sb.ToString() -match "Konsole bereit") { break }
        }
        else {
            Start-Sleep -Milliseconds 200
        }
    }
    $p.DiscardInBuffer()

    if ($sb.ToString() -match "Konsole bereit") {
        Write-Host "Geraet ist bereit."
    }
    else {
        Write-Warning "Konsole meldet sich nicht - es wird trotzdem versucht."
    }

    Get-Datei -was "csv" -dateiname "spritpreis-$stempel.csv"
    Get-Datei -was "log" -dateiname "log-$stempel.txt"
}
finally {
    $p.Close()
    $p.Dispose()
}
