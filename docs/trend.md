# Trendpfeile – wie die Anzeige rechnet

Diese Datei erklärt die Auswertung unter dem großen Preis (`main/trend.c`).
Angezeigt werden vier Felder: `1T`, `2T`, `1W`, `12U`.

## Die Regel, die alles bestimmt

In Deutschland dürfen Tankstellen ihre Preise **nur einmal pro Tag um
12:00 Uhr erhöhen**; **Senkungen sind jederzeit erlaubt** (Vorgabe der
Markttransparenzstelle für Kraftstoffe, MTS-K). Angabe des Nutzers, geprüft
und Grundlage dieser Auswertung.

Folge für jede Auswertung:

- Ein Preis, der nachmittags niedriger ist als vormittags, ist **normal**.
  Er beweist keinen Trend, sondern nur die Tageszeit.
- Eine Erhöhung ist ein Ereignis mit Uhrzeit: **12:00 Uhr**.
- Ein Vergleichsfenster, das bis „jetzt“ läuft, enthält immer die Senkungen
  des laufenden Tages und ist deshalb nachmittags/abends systematisch zu
  niedrig. Der Pfeil zeigte dann fast immer `▼ billiger` – ohne Aussage.

## Deshalb: Fenster mit gleicher Tagesphase

Jedes Vergleichsfenster endet **24 Stunden vor jetzt** und hat dieselbe Länge
wie der Zeitraum, den es beschreibt. Verglichen wird der **Durchschnitt** des
Fensters mit dem aktuellen Preis.

| Feld | Fenster (Stunden vor jetzt) | Länge |
| --- | --- | --- |
| `1T` | 48 … 24 | 24 h |
| `2T` | 72 … 24 | 48 h |
| `1W` | 192 … 24 | 168 h (7 Tage) |

Beispiel um 18:00 Uhr: `1T` vergleicht den jetzigen Preis mit dem
Durchschnitt von gestern 18:00 Uhr bis heute 18:00 Uhr.

- **Pfeil hoch, rot** – aktueller Preis liegt mehr als 1 Cent über dem
  Vergleichswert. Die Tankstelle ist für diesen Zeitraum teuer.
- **Pfeil runter, grün** – mehr als 1 Cent darunter.
- **Strich** – Unterschied unter 1 Cent (`TREND_FLAT_MILLI` in `config.h`).
- **`-,-`** – zu wenige Werte im Fenster (es müssen mindestens 50 % der
  Stunden vorliegen, `TREND_MIN_FILL_PCT`). Beim frisch gestarteten Gerät ist
  das normal: die Werte wachsen mit der Laufzeit.

Der Wert neben dem Pfeil ist die Abweichung in **Cent** (auf Zehntelcent
gerundet): `+1,2` heißt 1,2 Cent teurer als der Vergleichswert.

## Das Feld `12U`

`12U` vergleicht den aktuellen Preis mit dem **ersten erfassten Preis ab dem
letzten 12-Uhr-Zeitpunkt**.

- Nach 12:00 Uhr ist das der heutige Erhöhungsstand. Da danach nur gesenkt
  werden darf, zeigt der Wert, wie viel die Tankstelle seit dem Mittag
  wieder nachgelassen hat (typisch: grüner Pfeil, einige Cent).
- Vor 12:00 Uhr ist es der gestrige 12-Uhr-Stand – also der Vergleich mit dem
  letzten Tag.
- Fehlt die Stunde 12:00 (Gerät war aus), wird der nächste erfasste Wert
  genommen. Er ist eine Obergrenze, weil seit 12 Uhr nur gesenkt werden darf.
  Ist der Abstand größer als 24 h, gilt das Feld als „keine Daten“.

`12U` ist damit das einzige Feld, das ein echtes Preisfeststellungs-Ereignis
misst. Die drei anderen zeigen, ob der Preis im Vergleich zum üblichen
Niveau des Zeitraums gerade günstig ist.

## Günstigste Tagesstunde

Unten rechts steht ein Feld wie `guenstig 21-22h`. Dafür wird für jeden der
letzten Tage (bis zu 7) die Stunde mit dem **niedrigsten** Preis gesucht und
dann gezählt, welche Stunde am häufigsten das Tagestief war. Angezeigt wird
diese häufigste Stunde als Zeitraum.

- Der laufende Tag zählt nicht mit (er ist unvollständig).
- Tage mit weniger als 12 erfassten Stunden werden übersprungen, sonst wäre
das "Tagestief" künstlich.
- `guenstig --` heißt: noch zu wenig Verlauf oder keine Karte.
- Steht eine Stunde am häufigsten, wird bei Gleichstand die frühere genommen.

**Wichtig zur Aussagekraft:** Weil nach 12:00 Uhr nur noch gesenkt werden
darf, ist das Tagestief praktisch immer spät am Abend - das Feld sagt also
vor allem: *heute ist der Preis am Abend am niedrigsten*. Tagesvergleiche
siehe die Felder `1T`, `2T`, `1W`.

## Grenzen

- Grundlage sind die **selbst mitgeschriebenen** Stundenwerte (14 Tage im
  NVS). Vorher existieren keine Daten; die API liefert keine Historie.
- Der Durchschnitt enthält alle Werte der Tankstelle, auch Zeiten, an denen
  sie geschlossen war (dann ändert sich der Preis nicht, was den Schnitt
  glättet).
- Die Pfeile beziehen sich immer auf den **angezeigten Kraftstoff**. Ein
  Tippen auf Diesel/E5/E10 rechnet die Zeile sofort neu.
