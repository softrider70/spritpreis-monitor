# AGENTS.md - Projektkontext Spritpreis-Monitor

> Wird von Copilot/Agenten automatisch gelesen, wenn dieses Projekt als
> Workspace geoeffnet wird. Angelegt 2026-09-23.

## Was ist das?

ESP32-Geraet, das **eine feste Tankstelle** beobachtet: aktueller Preis gross
auf einem 2,8"-Display, darunter der Verlauf als Diagramm (Stunden-/Tages-
ansicht, verschiebbar). Ziel: besseres Gefuehl dafuer, wann tanken lohnt.

- Pfad: `C:\Users\<name>\Downloads\GitHub\VS-Projekte\CascadeProjects\spritpreis-monitor`
- Hardware: **CYD ESP32-2432S028R** (2,8", ILI9341 320x240, XPT2046-Touch,
  ESP32-WROOM-32, 4 MB, kein PSRAM)
- Abgeleitet von `katzenfutterautomat` / `autofrontcam` (Display- und
  Touch-Treiber wurden uebernommen, Pins sind dort verifiziert)
- Zielgeraet: **eine feste Tankstelle** (CYD); Name und Adresse bleiben privat

## Datenquelle (geprueft 2026-09-23)

- Amtliche Preise der **MTS-K** (Bundeskartellamt) ueber die freie
  Tankerkoenig-API, Lizenz **CC BY 4.0** -> Quellenangabe ist Pflicht und
  steht fest in der Anzeige.
- Host: **`creativecommons.tankerkoenig.de`** - der alte Host
  `api.tankerkoenig.de` existiert NICHT mehr (DNS leer).
- `prices.php?ids=<uuid>&apikey=..` liefert nur den AKTUELLEN Preis.
  Fehlerantworten kommen mit **HTTP 200** und `ok:false` -> immer `ok` pruefen.
- Grenzen: API-Key Pflicht (Registrierung mit Vor-/Nachnamen), max.
  1 Abfrage/Minute, Umkreis max. 25 km, best effort.
- Verlauf: nur selbst mitschreiben (so macht es das Projekt); Altbestand ab
  2014 gibt es nur als PostgreSQL-Dump auf Anfrage.
- Details: `docs/datenquelle.md`

## Aufbau

- `main/fuel_api.c` holt und parst die Preise (HTTPS mit
  `esp_crt_bundle_attach`, kein cJSON - in IDF 6.1 nicht vorhanden).
- `main/price_log.c` fuehrt den Verlauf: RAM-Fenster (14 Tage stuendlich,
  120 Tage taeglich) plus je ein kleiner NVS-Eintrag `h<nummer>`/`d<nummer>`.
  Geschrieben wird nur bei Aenderung oder Stundengewinn.
- `main/trend.c` rechnet die Trendwerte `1T`/`2T`/`1W`/`12U` (nur noch fuer
  die Konsole, nicht mehr auf der Anzeige). Vergleichsfenster enden 24 h vor
  jetzt (gleiche Tagesphase), weil Preise nur um 12:00 Uhr erhoeht werden
  duerfen. Siehe `docs/trend.md`.
- `main/sd_archive.c` schreibt jede Preisaenderung/Stunde als CSV auf die
  microSD (`/sdcard/spritpreis-JJJJ-MM.csv`, Milli-Euro, Semikolon) und liest
  sie beim Start wieder ein (`sd_archive_laden`) - der Arbeitsspeicher ist
  nach dem Trennen der Stromversorgung leer. Ohne Karte laeuft alles
  unveraendert weiter.
- `main/power.c` haelt die Anzeige aus, wenn nichts passiert (Hintergrund-
  beleuchtung aus + Panel-Sleep), und nutzt den Light-Sleep. Wecker ist der
  Touch-IRQ (IO36) per ext0 (Low-Pegel), ersatzweise die BOOT-Taste (IO0).
  Wecktaste und Display-Haltezeit: `POWER_*` in `config.h`.
  Der ESP32 kann nur EINEN ext0-Pin - deshalb Touch oder Taste, nicht beide.
  Beim Ausschalten wird das Bild NICHT geloescht: es bleibt im Speicher des
  Panels, damit das Aufwecken ohne Bildaufbau geht. Deshalb zeichnet
  `ui_render` nur bei geaenderter Signatur (Preis, Minute, Fenster, Offset).
- Anzeige ist fest auf **E10** gelegt (`FUEL_DEFAULT_INDEX`); der
  Kraftstoff-Schalter am Display ist entfallen, die Konsole kann noch
  umschalten (`fuel`).
- `main/sd_log.c` schreibt dieselben Zeilen, die ueber die serielle
  Schnittstelle gehen, zusaetzlich nach `/sdcard/log-JJJJ-MM.txt` (eigener
  Task, gesammelt alle 2 s). Konsolenausgaben laufen in `console.c` ueber das
  Makro `printf(...) -> sd_log_printf(...)` mit. Alle Kartenzugriffe laufen
  ueber `sd_archive_lock()` - das FAT vertraegt keinen zweiten Zugriff.
- `main/fuel_poll.c` ist der Takt (5 min), Core 0. Er fragt erst ab, wenn die
  Uhr steht, und haelt den Mindestabstand von 60 s zur API ein.
- `main/ui.c` zeichnet (roher SPI-Treiber, kein LVGL), Core 1 via Touch-Task.
  Zeilen: Kopf, Status (inkl. `SD:ok`) + Uhr, grosser Preis mit Tief/Hoch,
  guenstigster Zeitraum **des sichtbaren Fensters** + Achsentitel, Diagramm mit
  Preiswerten rechts, Zeitmarken/Stundenstrichen unten und roter 12-Uhr-Linie,
  Fusszeile mit den vier Fensterfeldern.
  **Blaettern im Diagramm per Tipp links/rechts, Mitte schaltet das Fenster
  weiter** - eigene Pfeilfelder gibt es nicht mehr.
- `main/touch.c` taktet den XPT2046 per Bitbang und wird im Takt abgefragt
  (30 ms bei Anzeige an, 50 ms aus). Der Touch-IRQ (IO36) ist **nicht**
  nutzbar - kein Pull-up, der Pegel flattert (siehe oben).
  Ein **Tipp wird erst beim Loslassen** gemeldet - sonst wuerde jedes
  Verschieben zusaetzlich einen Tipp ausloesen.
- `main/settings.c` haelt Tankstellen-ID und API-Key im NVS (`spritcfg`),
  `main/wifi.c` die WLAN-Daten im selben Namespace.

## Hardware-Konflikt SPI (wichtig)

Der CYD hat Display, Touch und microSD, der ESP32 aber nur zwei nutzbare
SPI-Peripherien (SPI2/HSPI, SPI3/VSPI).

- Display: SPI2 (HSPI), Pins 14/13/12/15/2/21
- microSD: SPI3 (VSPI), Pins 5/18/19/23 - Pin-Angabe aus der CYD-Doku
  (witnessmenow/ESP32-Cheap-Yellow-Display, `PINS.md`, Abschnitt "SD Card")
- Touch: gleiche Pins wie VSPI-Geraet, deshalb **Software-Bitbang**
  (`main/touch.c`, `BB_HALF_US` = 2 us). Nicht auf Hardware-SPI zurueckbauen,
  sonst ist der Karten-Bus belegt.

## Konventionen

- Kommentare, Logs und Doku auf **Deutsch**.
- Pins und Parameter zentral in `include/config.h`.
- Preise werden als **Milli-Euro in `int`** gefuehrt (kein float im Verlauf).
- Task-Stacks in BYTES, Aufteilung Core 0 = Netz/HTTP, Core 1 = Display/UI.
- Build-Zaehler: `tools/increment_build.py` -> `include/version.h`.

## Bauen und flashen (verifiziert)

```powershell
. .\activate-esp-idf.ps1
idf.py build
idf.py -p COMx flash monitor
```

## Erste Einrichtung auf dem Geraet

```
wifi <ssid> <passwort>
station <uuid>     # ID mit tools/find_station.ps1 suchen
key <apikey>       # von creativecommons.tankerkoenig.de
poll
status
sd                 # Zustand des SD-Archivs
power              # Stromsparmodus und Weckpin
```

Ohne API-Key laeuft der Testbetrieb mit erfundenen Preisen aus `main/sim.c`:

```
sim on             # Testpreise statt der API (bleibt im NVS gespeichert)
sim fill 8         # 8 Tage Verlauf + SD-Archiv rueckwirkend erzeugen
trend              # Zahlen zu den Pfeilen
```

Zugangsdaten kommen entweder per Konsole (`wifi`, `station`, `key`) oder aus
`/sdcard/spritpreis.cfg` (`main/sd_config.c`): Musterdatei mit Platzhaltern
(alle Werte: station, apikey, ssid, pass, sim), Nutzer traegt Werte ein,
Geraet uebernimmt sie ins NVS und setzt die Datei zurueck.
WLAN nur als vollstaendiges Paar (ssid+pass), `sim=on|off` schaltet die
Simulation. Konsole: `cfg`, `cfg import`, `cfg template`.

Ohne API-Key: `sim on` und `sim fill 8` - dann kommen die Preise aus
`main/sim.c` (Testmuster mit 12-Uhr-Sprung, Status "sim").

## Status

- [x] Projektgeruest, Display-/Touch-Treiber uebernommen, Build laeuft
- [x] API-Abfrage, Verlaufsspeicher, Anzeige, Touch-Bedienung, Konsole
- [x] Trendpfeile 1T/2T/1W/12U (Durchschnittsvergleich, 12-Uhr-Regel)
- [x] Anzeige ueberarbeitet: vier Zeitfenster (24h/3T/1W/4W), beschriftete
      Achsen, Tief/Hoch neben dem Preis, Pfeilzeile entfallen
- [x] Touch weckt per IRQ sofort (Tipp erst beim Loslassen)
- [x] Verlauf wird beim Start aus dem SD-Archiv ergaenzt (Stromausfall)
- [x] Langzeitarchiv als CSV auf der microSD
- [x] Stromsparen: Anzeige nach Touch/Taste/Preisaenderung 180 s an (power.c)
- [x] Simulationsmodus fuer den Betrieb ohne API-Key (sim.c, `sim on`/`sim fill`)
- [x] Auf dem echten CYD geflasht, Anzeige und Touch geprueft (Build 91)
- [x] Touch-IRQ (IO36) geprueft: kein Pull-up, Pegel flattert - Wecken laeuft
      ueber die BOOT-Taste (Konsole: `power`)
- [ ] Tankstellen-ID und API-Key der eigenen Tankstelle eintragen (Nutzer)
      -> bis dahin laeuft der Testbetrieb mit `sim on`
- [ ] Verlauf ein paar Tage laufen lassen und Diagramm ansehen

## Messwerte am echten Geraet

- Akkulaufzeit: 150 mAh bei Anzeige-Stromsparmodus -> 5 bis 7 h
  (Nutzermessung 2026-09-24).
