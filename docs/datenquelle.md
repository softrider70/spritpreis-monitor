# Datenquelle (recherchiert und geprueft am 2026-09-23, Bedingungen am 2026-09-24)

## Kurzfassung

Die Spritpreise einer bestimmten Tankstelle sind **frei zugaenglich** - ueber die
API von Tankerkoenig, die die amtlichen Daten der Markttransparenzstelle fuer
Kraftstoffe (MTS-K) beim Bundeskartellamt weiterreicht. Das Projekt ist damit
machbar.

| Punkt | Befund |
| --- | --- |
| Quelle | Markttransparenzstelle fuer Kraftstoffe (MTS-K), Bundeskartellamt |
| Zugang | Tankerkoenig-API, `https://creativecommons.tankerkoenig.de/json/...` |
| Kosten | kostenlos nach Registrierung (API-Key) |
| Lizenz | CC BY 4.0 - **Quellenangabe ist Pflicht** |
| Aktualitaet | Tankstellen muessen Preisänderungen zeitnah melden |
| Grenzen | max. 1 Abfrage pro Minute, Umkreissuche max. 25 km, best effort |
| Verlauf | **API liefert nur den aktuellen Preis** - siehe unten |

## Bedingungen im Wortlaut (von creativecommons.tankerkoenig.de)

- "Als technische Einschraenkung muss ein API-Key benutzt werden, um Zugriffe
  zuordnen und Missbrauch verhindern zu koennen."
- "Des weiteren ist die Umkreissuche auf einen Radius von 25 km und die
  **Abfragefrequenz auf einen request/Minute** beschraenkt."
- "Die Bedingungen der MTS-K muessen ebenfalls eingehalten werden. Danach
  duerfen die Daten insbesondere weder unmittelbar von Mineraloelunternehmen
  und Tankstellenbetreibern ... noch von fuer die Mineraloelindustrie taetigen
  IT-Dienstleistern bezogen oder an diese weitergegeben werden."
- "Die freie API wird aus Kosten- und Ressourcengruenden auf
  best-effort-Basis betrieben, eine Dienstguete kann ... nicht garantiert
  werden."
- Bei der Registrierung muessen **Vor- und Nachname** angegeben werden
  (behoerdliche Anforderung).

## Wie das Geraet die Bedingungen einhaelt

| Bedingung | Umsetzung |
| --- | --- |
| API-Key Pflicht | `key <apikey>` bzw. `apikey=` in `spritpreis.cfg`, Ablage im NVS |
| max. 1 Abfrage/Minute | Takt 300 s; zusaetzlich harte Sperre in `fuel_poll.c` (`api_abstand_ok()`, 60 s), die auch `poll` per Konsole und Fehlversuche erfasst |
| Radius 25 km | nicht verwendet - nur `prices.php` mit der einen Tankstellen-ID |
| MTS-K Weitergabe | privates Anzeigegeraet, keine Weitergabe an Dritte |
| CC BY 4.0 Quellenangabe | Startmeldung nennt Urheber (MTS-K / Tankerkoenig), Lizenz (CC BY 4.0) und Adresse |
| best effort | Fehlversuche werden toleriert, der zuletzt bekannte Preis bleibt stehen |
| ohne Uhr keine Abfrage | `fuel_poll.c` fragt erst ab, wenn die Uhr steht (TLS prueft das Zertifikat gegen die eigene Zeit) |

## Endpunkte (selbst abgerufen und geprueft)

```
https://creativecommons.tankerkoenig.de/json/list.php?lat=..&lng=..&rad=..&sort=dist&type=all&apikey=..
https://creativecommons.tankerkoenig.de/json/prices.php?ids=<uuid>&apikey=..
https://creativecommons.tankerkoenig.de/json/detail.php?id=<uuid>&apikey=..
https://creativecommons.tankerkoenig.de/json/complaint.php     (Preis melden)
```

Wichtig: Der frueher haeufig genannte Host **`api.tankerkoenig.de` existiert
nicht mehr** (DNS-Aufloesung schlaegt fehl). Richtig ist
`creativecommons.tankerkoenig.de`.

Antwort von `prices.php` (echte Antwort, Demo-Key aus der Doku):

```json
{ "ok": true,
  "license": "CC BY 4.0 - https://creativecommons.tankerkoenig.de",
  "data": "MTS-K",
  "prices": { "<uuid>": { "status": "open", "e5": 1.234, "e10": 1.234, "diesel": 1.234 } } }
```

- Bei geschlossener Tankstelle: `"status": "closed"`, Preisfelder `false`.
- Fehler (z. B. fehlender Key): `{"status":"error","ok":false,"message":"parameter error"}`
  - **mit HTTP 200**, deshalb muss der Code `ok` pruefen und nicht nur den Statuscode.
- Der Demo-Key aus der Doku liefert nur ein Testdatenset (feste Berliner
  Beispielwerte) - fuer echte Preise braucht es einen eigenen Key.

## Zertifikat / ESP32

`creativecommons.tankerkoenig.de` nutzt ein Let's-Encrypt-Zertifikat
(Aussteller `YR1`, Wurzel ISRG Root X1) und spricht HTTP/1.1. Auf dem ESP32
reicht deshalb das eingebaute Wurzelzertifikat-Bundle
(`esp_crt_bundle_attach`), kein Zertifikat muss in die Firmware.

## API-Key besorgen

1. `https://creativecommons.tankerkoenig.de/` aufrufen, kostenlos registrieren.
2. Die Anmeldung verlangt inzwischen **Vor- und Nachnamen** (behoerdliche
   Anforderung), ausserdem eine E-Mail-Adresse.
3. Der Key kommt per E-Mail und wird auf dem Geraet einmalig gesetzt:
   `key <apikey>` in der Konsole.

Bedingungen der MTS-K: Die Daten duerfen nicht von Mineraloelfirmen,
Tankstellenbetreibern oder deren IT-Dienstleistern bezogen oder an diese
weitergegeben werden. Fuer ein privates Anzeigegeraet ist das unkritisch.

Abfragefrequenz: Die freie API erlaubt **einen Aufruf je Minute**. Das Geraet
fragt alle 5 Minuten ab und sperrt zusaetzlich jeden zweiten Aufruf innerhalb
von 60 Sekunden (`api_abstand_ok()` in `fuel_poll.c`).

## Verlauf (der Haken)

Die freie API liefert **nur den aktuellen Preis**, keine Historie. Deshalb
schreibt dieses Geraet den Verlauf selbst mit:

- alle 5 Minuten abfragen,
- pro Stunde **Minimum, Maximum und letzter Preis** je Kraftstoff speichern,
- Aufbewahrung: 14 Tage stuendlich, 120 Tage taeglich (NVS im Geraet).

Ergebnis: Nach wenigen Tagen ist der Tagesverlauf sichtbar, nach ein bis zwei
Wochen auch das uebliche Muster (morgens teuer, abends guenstiger).

Alternativen fuer Altbestand (falls spaeter gewuenscht):

- Tankerkoenig gibt die gesammelten Daten **seit Juni 2014** heraus, aber laut
  eigener Doku als **PostgreSQL-Dump auf Anfrage** (`info@tankerkoenig.de`);
  andere Formate wie CSV/JSON nur gegen Gebuehr. Der alte CSV-Link
  (`dev.azure.com/tankerkoenig/tankerkoenig-data`) liefert **404**.
- Der Nachbau eines fremden Dienstes (z. B. `benzinpreis.de`-Preisarchiv) ist
  nicht noetig und waere rechtlich unklarer als der amtliche Weg.

## Vergleichbare Projekte

- **SpritTracker** (`sprittracker.de`): selbst gehostet, fragt die API
  hoechstens alle 5 Minuten ab (bis zu 10 Stationen je Abfrage) und speichert
  **nur Preisänderungen** - gleiche Strategie wie hier.
- Tankerkoenig hat eigene Apps und auf der Website einen Verlauf
  (`/preisentwicklung`), aber keinen dokumentierten Verlaufs-Endpunkt fuer
  einzelne Stationen.
