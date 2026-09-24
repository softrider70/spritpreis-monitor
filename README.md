# Spritpreis-Monitor

Kleines ESP32-Geraet (CYD ESP32-2432S028R) fuer eine **feste Tankstelle**:
aktueller Preis gross auf dem Display, darunter der bisherige Verlauf als
Diagramm. Das Zeitfenster ist in vier Stufen umschaltbar (**24h / 3T / 1W / 4W**)
und laesst sich verschieben.

Ziel: leichter entscheiden, wann getankt wird.

## Datenquelle

Amtliche Preise der **Markttransparenzstelle fuer Kraftstoffe (MTS-K)**,
bereitgestellt ueber die freie API von Tankerkoenig (CC BY 4.0).
Details, Endpunkte und Grenzen: `docs/datenquelle.md`.

- Abfrage alle **5 Minuten** (die freie API erlaubt max. 1 Abfrage/Minute).
- Die API liefert nur den **aktuellen** Preis - den Verlauf schreibt das
  Geraet selbst mit (stuendliche und taegliche Werte im NVS).
- Quellenangabe nach CC BY 4.0 ist Pflicht und erscheint in der
  **Startmeldung** ("Daten: MTS-K / Tankerkoenig, CC BY 4.0").

## Hardware

| Teil | Anschluss |
| --- | --- |
| CYD ESP32-2432S028R | USB-C zum Flashen (CP210x/CH340) |
| Display ILI9341 320x240 | eigener SPI-Bus: CLK=14, MOSI=13, CS=15, DC=2, BL=21 |
| Touch XPT2046 | Pins MOSI=32, MISO=39, CLK=25, CS=33, IRQ=36 - **per Software getaktet** (Bitbang) |
| microSD | VSPI-Bus: CS=5, SCK=18, MISO=19, MOSI=23 |
| BOOT-Taste | IO0, als Wecktaste nutzbar (kein Löten nötig) |

Der Touch muss in Software laufen: Der CYD verdrahtet Display, Touch und
microSD auf drei Geräte, der ESP32 hat aber nur zwei nutzbare SPI-Busse
(SPI2/HSPI und SPI3/VSPI). Der Touch hängt auf den Pins des VSPI - und den
braucht die Karte. Zwei Pin-Sätze auf einem Hardware-Bus gehen nicht, also
taktet `touch.c` den Touch selbst.

## Erste Einrichtung

1. **API-Key besorgen:** `https://creativecommons.tankerkoenig.de/`
   (kostenlos, verlangt Vor-/Nachnamen wegen behoerdlicher Vorgabe).
2. **Tankstellen-ID finden** (auf dem PC):
   ```powershell
   cd tools
   .\find_station.ps1 -ApiKey <key> -Ort "<Strasse> <PLZ> <Ort>"
   ```
3. **Auf dem Geraet setzen** - entweder per Konsole (USB, 115200):
   ```
   wifi <ssid> <passwort>
   station <uuid>
   key <apikey>
   poll
   ```
   Danach `status` - dort muss WLAN "verbunden", Uhr "gestellt" und die letzte
   Abfrage "ok" sein.

   Oder bequem über die **microSD** (siehe nächster Abschnitt): Datei
   `spritpreis.cfg` am PC ausfüllen, Karte zurück, Gerät neu starten.

## Zugangsdaten (Tankstellen-ID und API-Key)

Gespeichert werden beide im **NVS** des Geräts, Namespace `spritcfg`
(`settings.c`). Verwendet wird der Key in `fuel_api.c` als URL-Parameter:
`https://creativecommons.tankerkoenig.de/json/prices.php?ids=<uuid>&apikey=<key>`

### Weg über die microSD (vorgesehen)

1. Gerät einmal mit gesteckter Karte starten - es legt `/sdcard/spritpreis.cfg`
   an, mit den Schlüsselwörtern und Platzhaltern:
   ```
   station=HIER_TANKSTELLEN_ID_EINTRAGEN
   name=HIER_ANZEIGENAME_EINTRAGEN
   apikey=HIER_API_KEY_EINTRAGEN
   ssid=HIER_WLAN_NAME_EINTRAGEN
   pass=HIER_WLAN_PASSWORT_EINTRAGEN
   sim=
   fill=
   ```
   - `sim=on` / `sim=off` – Simulationsmodus (Testpreise) an/aus, leer = nichts ändern
   - `fill=<1..13>` – erzeugt Testverlauf (so viele Tage stündlich, dazu
     Tageswerte bis 30 Tage) und füllt das SD-Archiv. Läuft automatisch,
     sobald die Uhr gestellt ist – man braucht dafür kein Terminal.
2. Karte in den PC, die Werte hinter den Schlüsselwörtern eintragen (die
   Zeilen mit `#` drumherum sind Kommentar und können bleiben). Man muss nur
   die Werte nachtragen, die man gerade hat - jeder Platzhalter wird einzeln
   geprüft und einfach übersprungen. Werte **ohne** umgebende Leerzeichen.
3. Karte zurück ins Gerät, Gerät **neu starten**.
4. Beim Start liest das Gerät die Werte, legt sie ins NVS und setzt die Datei
   **wieder auf die Platzhalter zurück**. Danach liegen die Geheimnisse nur
   noch im Flash des Geräts.

Stehen noch Platzhalter in der Datei, passiert nichts - der laufende Betrieb
(z. B. die Simulation) wird nicht angefasst. Prüfen ohne Neustart: `cfg`,
einlesen von Hand: `cfg import`, Muster neu schreiben: `cfg template`.

Die Werte landen alle im NVS (`spritcfg`): `station`, `name`, `apikey`,
`ssid`, `pass`. Der Schalter `sim=on|off` schaltet den Simulationsmodus. Drei
Besonderheiten:

- **WLAN** wird nur übernommen, wenn `ssid` **und** `pass` eingetragen sind.
  Ein halber Satz würde die Verbindung zerreißen.
- Nach `cfg import` (ohne Neustart) verbindet das Gerät gleich neu und stellt
die Uhr per SNTP.

Diese Einschränkung sollte man kennen: Das Zurücksetzen löscht den Inhalt der
Datei logisch, nicht physisch. Wer die Karte mit einem Recovery-Werkzeug
ausliest, könnte alte Blöcke finden. Für den Alltag (Karte entnommen oder
verloren) reicht es; wer mehr will, formatiert die Karte zwischendurch.

Eine Kopie der Musterdatei liegt auch im Projekt: `tools/spritpreis.cfg`.

## Bauen und flashen

```powershell
. .\activate-esp-idf.ps1
idf.py build
idf.py -p COMx flash monitor
```

## Bedienung am Display

| Aktion | Wirkung |
| --- | --- |
| Beruehren (irgendwo) | Anzeige einschalten |
| Ziehen im Diagramm | Zeitfenster verschieben (rechts = aelter) |
| Tippen **links** im Diagramm | Zeitfenster seitenweise aelter |
| Tippen **rechts** im Diagramm | Zeitfenster seitenweise neuer |
| Tippen **in die Mitte** des Diagramms | naechstes Zeitfenster (24h -> 3T -> 1W -> 4W) |
| Tippen auf ein Feld unten | Zeitfenster direkt waehlen |

Eigene Pfeilfelder gibt es nicht mehr - das Blaettern liegt auf der linken bzw.
rechten Haelfte des Diagramms, die Mitte schaltet das Fenster weiter. So bleibt
die ganze Fusszeile fuer die vier Fenster frei.

Ein Tipp wird erst beim **Loslassen** ausgewertet - vorher ist nicht zu
entscheiden, ob daraus ein Ziehen wird. Deswegen loest Verschieben keinen
Tipp aus.

Die Anzeige ist **fest auf E10** eingestellt (die anderen Sorten werden zwar
weiter mitgeschrieben, aber nicht mehr angezeigt). Neben dem grossen Preis
stehen **Tief** und **Hoch** des gerade sichtbaren Zeitfensters.

In der Zeile darunter steht der **guenstigste Zeitraum des sichtbaren
Fensters**: Fuer jede Tagesstunde wird ueber die Stundenwerte des Fensters der
Durchschnitt gebildet, gezeigt wird die guenstigste Stunde, z. B.
`guenstig 13-14h (24 Std)` bei 24h oder `(72 Std)` bei 3 Tagen. Im 4-Wochen-
Fenster gibt es nur Tageswerte - dort steht die typische Tagesstunde der
letzten 7 Tage, erkennbar am Zusatz `(7 T)`.

### Diagramm lesen

- **Dunkelgruener Balken**: Bereich Minimum..Maximum der Stunde bzw. des Tages.
- **Hellblaue Linie**: letzter Preis des Zeitraums.
- **Preiswerte rechts** an den Gitterlinien (Einheit `EUR/L` steht in der
  Wertezeile darueber).
- **Untere Achse**: Zeitmarken mit Abstand zu jetzt (`-6h` … `jetzt` bzw.
  `-3T` … `jetzt`) und dazwischen kleine Striche je Stunde.
- **Rote senkrechte Linie**: 12:00 Uhr mittags. Um diese Zeit darf der Preis
  erhoeht werden (MTS-K), danach sind nur Senkungen erlaubt - die Linie zeigt
  also den Beginn jedes neuen Tagespreises. In der 4-Wochen-Ansicht (Tage)
  entfaellt sie.

## Stromsparen
Die Anzeige ist nur nach einem Ereignis an und geht danach wieder aus:

| Ereignis | Wirkung |
| --- | --- |
| Touch | Anzeige an, 60 s |
| BOOT-Taste | Anzeige an, 60 s |
| Preisänderung | Anzeige an, 60 s |

Die Abfrage der Preise läuft **weiter**, auch wenn nichts zu sehen ist.
Aus ist dabei zweierlei: erst die Hintergrundbeleuchtung (das ist der große
Verbraucher), dann das Panel selbst (SLPIN). Zusätzlich schläft die CPU in
ihren Ruhezeiten automatisch (Light-Sleep), weil das Power Management aktiv
ist.

Aufgeweckt wird die CPU per **Touch-IRQ (IO36)**. Das klappt nur, wenn dieser
Pin im Ruhezustand wirklich high ist - er ist open-drain und braucht einen
Pull-up. Beim Start wird das geprüft und im Log gemeldet; Konsole: `power`.
Ist er nicht nutzbar, weckt ersatzweise die BOOT-Taste (IO0), und wenn beide
nicht gehen, bleibt es bei "nur Anzeige aus" - dann läuft die CPU weiter und
der Touch reagiert sofort.

Hinweis: Der ESP32 kann im Sleep nur **einen** Low-Pegel-Wecker auf einem
RTC-Pin (ext0) nutzen. Deshalb ist es entweder der Touch oder die Taste,
nicht beides gleichzeitig. Gedrückt werden kann die Taste immer dann, wenn
die CPU ohnehin wach ist.

### Die Uhr kommt übers Netz

Die Uhr wird per SNTP gestellt. Klappt die WLAN-Verbindung beim Start nicht
sofort (kommt vor), holt das Gerät sie im Abfrage-Takt nach und startet dann
SNTP selbst - ohne Uhr gibt es keinen Verlauf und kein Archiv. Im Log steht
dann `Zeit wird nachtraeglich geholt (SNTP)`.

## Trendpfeile

Unter dem großen Preis steht eine kompakte Zeile mit vier Feldern:

| Feld | Vergleich |
| --- | --- |
| `1T` | aktueller Preis gegen den Durchschnitt der letzten 24 h |
| `2T` | gegen den Durchschnitt der letzten 48 h |
| `1W` | gegen den Durchschnitt der letzten 7 Tage |
| `12U` | gegen den letzten 12-Uhr-Stand (der einzige Zeitpunkt, an dem eine Erhöhung möglich war) |

Pfeil **hoch + rot** = teurer als der Vergleich, **runter + grün** = billiger,
Strich = praktisch gleich (unter 1 Cent), `-,-` = noch zu wenig Verlauf.
Der Wert daneben ist die Abweichung in **Cent**.

**Wichtig zur Auswertung:** Preise dürfen nur einmal täglich um 12:00 Uhr
erhöht werden, danach sind nur Senkungen erlaubt (MTS-K). Deshalb enden alle
Vergleichsfenster **24 h vor jetzt** und treffen damit dieselbe Tagesphase.
Ein Fenster bis „jetzt" wäre nachmittags systematisch zu billig und der Pfeil
zeigte fast immer „billiger". Ausführliche Begründung: `docs/trend.md`.

## Langzeitarchiv auf der microSD

Der NVS-Verlauf hält nur 14 Tage stündlich bzw. 120 Tage täglich. Alles
Weitere landet als CSV auf der Karte (eine Datei je Monat):

```
/sdcard/spritpreis-2026-09.csv
# zeit_iso;epoch;e5_milli;e10_milli;diesel_milli;status
2026-09-23T14:35:02;1789917302;1689;1652;1589;open
```

- Preise als **Milli-Euro** (1689 = 1,689 EUR), Semikolon als Trenner -
damit lässt sich die Datei in deutschem Excel direkt in Spalten öffnen.
- Zwei Formate, umschaltbar mit `csv kompakt` / `csv lesbar`:
  ```
  lesbar : 2026-09-24T18:46:41;1790268401;2309;2249;2409;open
  kompakt: 1790268401;2309;2249;2409;open
  ```
  Die lesbare Form enthält die Zeit doppelt (Datum **und** Sekundenstempel)
  und ist damit rund 40 % größer. Beim Einlesen werden **beide** Formate
  erkannt, auch gemischt. In Excel lässt sich der Sekundenstempel umrechnen:
  `=A2/86400+25569` als Datum formatieren.
- Geschrieben wird nur bei **Preisänderung oder Tageswechsel**, also rund
10-20 Zeilen am Tag statt 288 (schont die Karte). Die Reihe bleibt trotzdem
vollständig, weil ein Wert so lange gilt, bis die nächste Zeile kommt.
- Steckt keine Karte, läuft das Gerät unverändert weiter; auf der Anzeige
steht dann `SD:--` statt `SD:ok`.
- Die Karte muss **FAT32/exFAT** formatiert sein. Das Gerät formatiert
niemals selbst - es würde dabei Daten überschreiben.
- **Nach einem Stromausfall** holt sich das Gerät den Verlauf beim Start aus
  diesen Dateien zurück (laufender Monat und die zwei davor). Der
  Arbeitsspeicher ist nach dem Trennen der Stromversorgung leer, die Karte
  nicht. Dabei wird nichts ins NVS zurückgeschrieben - das wären hunderte
  Schreibvorgänge ins Flash.
- Ist **kein WLAN** da, stellt das Gerät die Uhr aus dem jüngsten
  Archiv-Zeitstempel vor, damit die Kurve überhaupt gezeichnet werden kann.
  SNTP korrigiert die Uhr später.

## Logdatei auf der Karte

Am Gerät hängt nicht immer ein Rechner - was nachts passiert ist, wäre sonst
nirgends nachzulesen. Deshalb schreibt das Gerät dieselben Zeilen, die über
die serielle Schnittstelle gehen, zusätzlich mit:

```
/sdcard/log-2026-09.txt
```

- Enthalten sind **alle Logmeldungen** (`ESP_LOG*`) und **alle Ausgaben der
  Konsolenbefehle**. Die eingetippten Befehle selbst stehen nicht darin - aus
  der Antwort geht aber hervor, was gemacht wurde.
- Jede vollständige Zeile beginnt mit **Datum und Uhrzeit**
  (`24.09. 17:12:14  I (2617) wifi: ...`). Die Zahl in Klammern ist die
  Laufzeit in Millisekunden seit dem Start - sie bleibt zusätzlich stehen,
  weil sich damit Abstände im Ablauf ablesen lassen.
- Geschrieben wird gesammelt alle zwei Sekunden über einen eigenen Task;
  keine Logstelle wartet auf die Karte. Die Datei wird nur zum Schreiben
  geöffnet und gleich wieder geschlossen, damit die Karte jederzeit
  entnommen werden kann.
- Ein Log wird **nie gelöscht**, nur monatlich in eine neue Datei geführt.
- Ohne Karte ändert sich nichts: die Ausgabe bleibt dann nur auf der
  seriellen Schnittstelle. Prüfen mit `status` oder `sd`.
- Ohne gestellte Uhr schreibt das Gerät nach `log-vor-zeitstellung.txt`;
  sobald die Uhr steht, geht es automatisch in die Monatsdatei.

## Dateien vom Geraet auf den PC holen

Die Karte muss man zum Auswerten **nicht** herausnehmen: Das Geraet kann
Archiv und Logfile selbst auf die Konsole ausgeben.

```powershell
cd tools
.\hole_dateien.ps1                    # holt beide Dateien nach .\vom_geraet
.\hole_dateien.ps1 -Port COM5
```

Das Skript öffnet den COM-Port (ohne DTR-Reset, sonst startet das Gerät neu),
schickt `file csv` und `file log` und schreibt die Antworten in
`vom_geraet\spritpreis-<Datum>.csv` bzw. `log-<Datum>.txt`. Bei 115200 Baud
dauert das je Datei ein paar Sekunden.

Von Hand geht es auch - die Dateiausgabe ist zwischen zwei Marken eingefasst:

```
file          Zustand: welche Dateien gibt es
file csv      Archiv-CSV ausgeben
file log      Logfile ausgeben
csv           Archiv: Größe, Zeilen, Anzahl sim-Zeilen, Format
csv simweg    alle sim-Zeilen entfernen
csv kompakt   kleineres Format (ohne Datumsspalte), Datei wird umgeschrieben
csv lesbar    wieder das Format mit lesbarer Zeit
```

Beispiel: 137 Zeilen, davon 109 aus Testläufen - nach `csv simweg kompakt`
bleiben 33 Zeilen, und die Datei schrumpft von 6934 auf 1070 Byte.

```
----- Anfang /sdcard/spritpreis-2026-09.csv -----
...
----- Ende /sdcard/spritpreis-2026-09.csv (127 Zeilen) -----
```

## Was der Touch kann (und was nicht)

- Der Touch wird **im Takt abgefragt** (30 ms bei eingeschalteter Anzeige,
  50 ms bei ausgeschalteter). Der Touch-IRQ (IO36) laesst sich dafuer nicht
  nutzen: Der Anschluss hat keinen Pull-up, sein Pegel flattert (am Geraet
  gemessen: direkt nach dem Start hoch, 100 ms spaeter tief). Eine Beruehrung
  weckt deshalb die CPU nicht selbst.
- Das **Aufwecken** uebernimmt die **BOOT-Taste** (IO0) ueber den Light-Sleep.
  Ein Tipp auf den Touch weckt ebenfalls, aber erst beim naechsten Abfragetakt
  (hoechstens 50 ms). Wer ganz sicher gehen will: kurz die BOOT-Taste druecken,
dann ist das Bild sofort da.
- Ein **Tipp zählt erst beim Loslassen** — sonst würde jedes Verschieben
  zusätzlich einen Tipp auslösen. Beim Ausschalten bleibt das Bild im Speicher
  des Displays, deshalb ist es beim Aufwecken sofort da; neu gezeichnet wird
  nur, wenn sich etwas geändert hat.

## Bedingungen der API (eingehalten)

Auszug aus den Bedingungen auf `creativecommons.tankerkoenig.de` und wie das
Gerät sie einhält:

| Bedingung | Umsetzung im Code |
| --- | --- |
| API-Key erforderlich | `key <apikey>` bzw. `apikey=` in `spritpreis.cfg`, Ablage im NVS |
| **hoechstens 1 Abfrage je Minute** | Takt ist 300 s; zusaetzlich harte Sperre: `api_abstand_ok()` in `fuel_poll.c` laesst keinen zweiten Aufruf innerhalb 60 s zu (gilt auch fuer `poll` per Konsole und fuer Fehlversuche) |
| Umkreissuche max. 25 km | wird nicht benutzt - es wird nur `prices.php` mit der einen Tankstellen-ID abgefragt |
| keine Weitergabe an Mineraloelfirmen | privates Anzeigegeraet, keine Weitergabe |
| Quellenangabe (CC BY 4.0) | erscheint mit jeder Startmeldung: Urheber, Lizenz und Adresse |
| best effort, keine Garantie | Fehlversuche werden toleriert, der letzte Stand bleibt stehen |

Ergänzend: **Ohne gestellte Uhr wird gar nicht abgefragt.** Die
verschlüsselte Verbindung prüft das Zertifikat gegen die eigene Uhrzeit - mit
einem Datum von 1970 schlägt sie fehl; außerdem gäbe es ohne Uhr keine
Zeitstempel für Verlauf und Archiv.

## Konsole (seriell)

Falls die WLAN-Verbindung nicht zustande kommt, nennt das Log jetzt den
Abbruchgrund des Stacks, z. B. `Verbindung getrennt (Grund 202: Anmeldung
abgelehnt (meist falsches Passwort))`. Zusaetzlich zeigt `wifiscan`, welchen
Sicherheitsmodus ein Netz anbietet - damit laesst sich eine abweichende
Verschluesselung belegen statt vermuten. Wichtig bei gleichem Namen: ein
Repeater oder Mesh-Knoten kann dieselbe SSID auf mehreren Kanaelen
ausstrahlen; das Gerät verbindet sich mit dem Knoten, den es zuerst findet -
`wifiscan` unterscheidet sie anhand der MAC-Adresse.

Bei gleichem Netznamen auf mehreren Kanaelen (Repeater/Mesh) prueft die
Station alle Kanaele und nimmt den Knoten mit dem staerksten Signal
(`WIFI_ALL_CHANNEL_SCAN` + `WIFI_CONNECT_AP_BY_SIGNAL`). Ohne diese Angabe
gilt `WIFI_FAST_SCAN`; der bricht beim ersten Namenstreffer ab - dann kann
ein schwacher Knoten gewinnen.

```
wifi <ssid> <passwort>   Zugangsdaten speichern und verbinden
name <Text>              Anzeigename der Tankstelle (Kopfzeile der Anzeige),
                         z. B. name Star Musterstadt 1
wifiscan                 Netze in Reichweite auflisten (SSID, Signal,
                         Kanal, MAC, Sicherheitsmodus, * = konfiguriert)
wifidel                  Zugangsdaten loeschen
key <apikey>             API-Key speichern (danach Sofortabfrage)
station <uuid>           Tankstellen-ID speichern
fuel <diesel|e5|e10>     angezeigten Kraftstoff waehlen
poll                     sofort abfragen
status                   Zustand ausgeben
time                     Uhrzeit und Zeitgueltigkeit
log reset                Verlaufsspeicher loeschen
sd                       SD-Archiv: Zustand, Datei, Zaehler
sd mount                 Karte neu mounten (falls sie spaeter eingesteckt wird)
trend [diesel|e5|e10]    Trendwerte der Pfeile mit Zahlen
power                    Stromsparmodus: Zustand, Weckpin, Zaehler
power on|off             Anzeige hart ein-/ausschalten (Messung, Test)
sim                      Simulationsmodus: Zustand und Modellwerte
sim on|off               Testpreise statt der echten API (bleibt gespeichert)
sim fill [tage]          Verlauf + SD-Archiv rueckwirkend fuellen (1..9 Tage)
cfg                      Zugangsdaten-Datei auf der Karte: Zustand
cfg import               Datei jetzt einlesen (Werte ins NVS, Datei zurueck)
cfg template             Musterdatei neu schreiben
tasks                    Task-Liste mit Stack-Rest
help                     alle Befehle
```

Passwort und API-Key werden nur hier im Terminal eingegeben und liegen
ausschliesslich im NVS des Geraets.

**Bedien-Hinweis:** Im Stromsparmodus schläft die CPU. Vor dem ersten Zeichen
einer Eingabe deshalb einmal **Enter** drücken (oder das Display antippen) -
dann nimmt die Konsole den Befehl sicher an. Ohne das kann das erste Zeichen
verloren gehen.

## Aufbau des Codes

```
main/
  main.c        Start, Verdrahtung
  wifi.c/h      WLAN (STA) + SNTP
  settings.c/h  Tankstellen-ID und API-Key im NVS
  fuel_api.c/h  HTTPS-Abfrage + JSON-Auswertung
  fuel_poll.c/h Abfragetakt, Verlauf fortschreiben, Anzeige aktualisieren
  price_log.c/h Verlaufsspeicher (RAM-Fenster + NVS je Stunde/Tag)
  trend.c/h     Trendpfeile (Durchschnittsvergleich, 12-Uhr-Regel)
  sd_archive.c/h Langzeitarchiv als CSV auf der microSD
  sd_config.c/h Zugangsdaten aus /sdcard/spritpreis.cfg einlesen
  power.c/h     Stromsparen: Anzeige aus, Light-Sleep, Wecktaste
  sim.c/h       Simulationsmodus: Testpreise ohne API-Key
  ui.c/h        Anzeige und Bedienung
  display.c/h   ILI9341-Treiber (aus autofrontcam/katzenfutterautomat)
  touch.c/h     XPT2046-Touch (dito, Ziehen + Software-Bitbang)
tools/
  find_station.ps1   Tankstellen-ID suchen
  increment_build.py Build-Nummer -> include/version.h
  spritpreis.cfg     Muster der Zugangsdaten-Datei fuer die SD-Karte
docs/datenquelle.md  Recherche: API, Grenzen, Historie, Lizenz
docs/trend.md        Auswertung der Pfeile + 12-Uhr-Regel
```

## Speicherung des Verlaufs

- je Stunde ein NVS-Eintrag `h<Stundennummer>` (28 Byte),
- je Tag ein Eintrag `d<Tagesnummer>`,
- Aufbewahrung 14 Tage stuendlich, 120 Tage taeglich,
- geschrieben wird nur, wenn sich ein Wert aendert oder eine Stunde/ein Tag
  abgeschlossen ist - das schont das Flash.

Fuer die Langzeit (Jahre) kommt die CSV-Datei auf der microSD dazu, siehe
Abschnitt **Langzeitarchiv auf der microSD**.

## Offene Punkte

- **Echtes Ziehen** ist umgesetzt (Bewegungsereignisse im Touch); die
  Umrechnung der Touch-Rohwerte ist die grobe Standardformel aus dem
  CYD-Projekt - falls die Bedienung versetzt wirkt, mit `touch_set_calib_mode`
  nachmessen und in `touch.c` nachziehen.
- **Touch und microSD sind noch nicht auf echter Hardware geprueft.** Der
  Touch wurde von Hardware-SPI auf Software-Bitbang umgebaut (sonst bleibt
  kein Bus fuer die Karte). Beim ersten Flashen deshalb pruefen: Reagiert der
  Touch, und mountet die Karte (`sd`)? Wenn der Touch zickt, in `touch.c`
  `BB_HALF_US` erhoehen (langsamer takten).
- Verlauf fuer **vor** der Inbetriebnahme (Altbestand ab 2014) nur ueber einen
  PostgreSQL-Dump von Tankerkoenig auf Anfrage (siehe `docs/datenquelle.md`).
- Kein OTA - Updates laufen ueber USB. Bei Bedarf Partitionstabelle aendern.
