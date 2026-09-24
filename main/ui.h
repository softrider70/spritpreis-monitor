/*
 * ui.h - Anzeige des Spritpreis-Monitors
 *
 * Aufbau des Bildschirms (320x240):
 *   Kopfzeile   : Stationsname (bzw. "SIMULATION - Testpreise")
 *   Statuszeile : Kraftstoff, offen/geschlossen, online/offline, SD-Zustand,
 *                 rechts die Uhrzeit
 *   Preiszeile  : aktueller Preis gross; links daneben das Tief, rechts das
 *                 Hoch des sichtbaren Zeitfensters
 *   Wertezeile  : typische guenstigste Tagesstunde, rechts der Achsentitel
 *   Diagramm    : Verlauf - Balken von Minimum bis Maximum je Stunde/Tag,
 *                 Linie = letzter Preis. Rechts stehen die Preiswerte der
 *                 Gitterlinien.
 *   Zeitachse   : Marken mit Abstand zu jetzt ("-6h", "-2T", "jetzt")
 *   Fusszeile   : "<" und ">" zum Blaettern, dazwischen die vier
 *                 Zeitfenster (das aktive ist hervorgehoben)
 *
 * Bedienung:
 *   Tippen auf "<" / ">"        -> Zeitfenster aelter / neuer
 *   Tippen auf ein Fensterfeld  -> Fenster waehlen
 *   Tippen in die Grafikmitte   -> naechstes Zeitfenster
 *   Ziehen im Diagramm          -> Zeitfenster verschieben
 *
 * Die Anzeige ist fest auf E10 gelegt; umschalten laesst sich der Kraftstoff
 * nur noch ueber die Konsole ("fuel").
 *
 * Hinweis zur Lizenz (CC BY 4.0): die Quellenangabe steht in der
 * Startmeldung (ui_message) und wird dort mit jedem Start gezeigt.
 */

#pragma once

#include <stdbool.h>
#include "fuel_api.h"

/* Zeitfenster der Kurve. Reihenfolge = Reihenfolge beim Weiterschalten. */
typedef enum {
    UI_RANGE_24H = 0,       /* stuendlich, letzte 24 Stunden */
    UI_RANGE_3T,            /* stuendlich, letzte 3 Tage */
    UI_RANGE_1W,            /* stuendlich, letzte 7 Tage */
    UI_RANGE_4W,            /* taeglich, letzte 28 Tage */
    UI_RANGE_ANZAHL
} ui_range_t;

/* Gewaehltes Zeitfenster und sein Kurzname ("24h", "3T", "1W", "4W") */
ui_range_t ui_range(void);
const char *ui_range_name(void);

/* Display vorbereiten (setzt auch die Quellenangabe) */
void ui_init(void);

/* Name der Tankstelle in der Kopfzeile setzen */
void ui_set_station_label(const char *name);

/* Vollbildmeldung (z. B. "Kein WLAN", "API-Key fehlt"). Sie bleibt stehen
 * und wird erst mit ui_message_clear() wieder durch die Preisansicht ersetzt. */
void ui_message(const char *titel, const char *zeile1, const char *zeile2);

/* Meldung wegnehmen (nach erfolgreicher Abfrage). */
void ui_message_clear(void);

/* Hauptanzeige zeichnen */
void ui_render(const fuel_prices_t *p, bool zeit_ok, bool netz_ok);

/* Nur den Diagrammbereich neu zeichnen. Beim Verschieben des Zeitfensters
 * aendert sich ausschliesslich dort etwas - ein voller Bildaufbau waere
 * langsam und mehrfach sichtbar (Flackern). */
void ui_render_chart(void);

/* Touch-Ereignisse */
void ui_touch_tap(int x, int y);

/* Bewegung/Ziehen. Rueckgabe: true, wenn sich das Zeitfenster wirklich
 * verschoben hat - nur dann lohnt ein Neuzeichnen (sonst wuerde bei jedem
 * Bewegungssignal das ganze Bild neu aufgebaut und sichtbar flackern). */
bool ui_touch_move(int x, int y, bool pressed);

/* Aktuelle Auswahl (fuer die Konsole/Statusausgabe) */
int ui_fuel_index(void);

/* Angezeigten Kraftstoff setzen (Konsole: fuel ...) */
void ui_set_fuel(int idx);
