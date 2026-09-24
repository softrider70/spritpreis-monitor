/*
 * trend.h - Trendpfeile fuer die Anzeige
 *
 * Beantwortet die Frage "ist der Preis jetzt teuer oder billig?" fuer die
 * Zeitraeume 1 Tag, 2 Tage und 1 Woche sowie fuer den 12-Uhr-Stand.
 *
 * Aufbau der Vergleichswerte (ausfuehrlich in docs/trend.md):
 *  - Der aktuelle Preis wird gegen den DURCHSCHNITT des jeweiligen
 *    Zeitraums gestellt.
 *  - Jedes Vergleichsfenster endet 24 h vor jetzt, hat also dieselbe
 *    Tagesphase wie der aktuelle Zeitpunkt.
 *    Grund: Preise duerfen nur einmal taeglich um 12:00 Uhr erhoeht werden,
 *    danach sind nur Senkungen erlaubt (MTS-K). Ein Fenster bis "jetzt"
 *    waere nachmittags systematisch zu billig - der Pfeil zeigte dann fast
 *    immer "billiger" und damit nichts Aussagekraeftiges.
 *  - Das Feld "12U" (letzter 12-Uhr-Stand) vergleicht den aktuellen Preis
 *    mit dem ersten erfassten Preis ab dem letzten 12-Uhr-Zeitpunkt. Das
 *    ist der einzige Zeitpunkt des Tages, an dem eine Erhoehung moeglich
 *    war; der Abstand zeigt, wie viel seitdem wieder abgegeben wurde.
 */

#pragma once

#include <stdbool.h>
#include "config.h"

typedef enum {
    TREND_NO_DATA = 0,   /* zu wenige Werte im Zeitraum */
    TREND_DOWN,          /* billiger als der Vergleich */
    TREND_FLAT,          /* praktisch gleich (unter TREND_FLAT_MILLI) */
    TREND_UP             /* teurer als der Vergleich */
} trend_dir_t;

typedef struct {
    bool        valid;
    int         ref_milli;    /* Vergleichswert (Durchschnitt bzw. Preispunkt) */
    int         diff_milli;   /* aktueller Preis minus Vergleichswert */
    int         samples;      /* Anzahl der Werte im Vergleichsfenster */
    trend_dir_t dir;
} trend_value_t;

typedef struct {
    bool          has_now;    /* aktueller Preis liegt vor */
    int           now_milli;
    trend_value_t day1;       /* gegen den Durchschnitt der letzten 24 h */
    trend_value_t day2;       /* gegen den Durchschnitt der letzten 48 h */
    trend_value_t week1;      /* gegen den Durchschnitt der letzten 7 Tage */
    trend_value_t noon;       /* gegen den letzten 12-Uhr-Stand */
} trend_set_t;

/* Trendwerte fuer einen Kraftstoff berechnen (nur lesend, RAM-Puffer). */
void trend_compute(int fuel, trend_set_t *out);

/* Typische guenstigste Tagesstunde: Fuer jeden der letzten Tage wird die
 * Stunde mit dem niedrigsten Preis gesucht; zurueckgegeben wird die Stunde,
 * die am haeufigsten das Tagestief war.
 *   from_hour/to_hour : z. B. 21 und 22 fuer "guenstig 21-22 Uhr"
 *   tage              : Anzahl ausgewerteter Tage
 * Rueckgabe: true, wenn genug Tage vorliegen. */
bool trend_best_hour(int fuel, int *from_hour, int *to_hour, int *tage);

/* Guenstigste Tagesstunde bezogen auf das sichtbare Zeitfenster: ueber die
 * Stundenwerte der letzten <stunden> Stunden wird fuer jede Tagesstunde der
 * Durchschnitt gebildet; zurueckgegeben wird die Stunde mit dem niedrigsten
 * Durchschnitt. Damit gilt die Aussage genau fuer das, was im Diagramm steht.
 *   stunden : Fensterbreite in Stunden (24, 72, 168)
 *   werte   : Anzahl der ausgewerteten Stundenwerte
 * Rueckgabe: true, wenn mindestens TREND_MIN_HOURS_FENSTER Werte vorliegen. */
bool trend_best_hour_im_fenster(int fuel, int stunden, int *from_hour, int *to_hour,
                                int *werte);

/* Kurztext zur Richtung, fuer die Konsole: "teurer", "billiger", "gleich",
 * "keine Daten". */
const char *trend_dir_text(trend_dir_t dir);
