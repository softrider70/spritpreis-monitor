/*
 * price_log.h - Verlaufsspeicher (Stunden- und Tageswerte)
 *
 * Die API liefert nur den aktuellen Preis, deshalb schreibt das Geraet den
 * Verlauf selbst mit: bei jeder Abfrage (Standard 5 min) werden pro
 * Kraftstoff Minimum, Maximum und letzter Preis der laufenden Stunde bzw.
 * des laufenden Tages fortgeschrieben.
 *
 * Ablage: RAM-Fenster + je Stunde/Tag ein kleiner NVS-Eintrag (28 Byte).
 * Vorteil: sehr wenige Flash-Schreibvorgaenge, Verlauf ueberlebt Reboots.
 *
 * Aufbewahrung: LOG_HOURS Stundenwerte (14 Tage), LOG_DAYS Tageswerte (120 Tage).
 */

#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <time.h>

#include "config.h"
#include "fuel_api.h"

/* Feldauswahl fuer price_log_series() */
enum {
    LOG_FIELD_MIN = 0,
    LOG_FIELD_MAX,
    LOG_FIELD_LAST
};

typedef struct {
    int32_t stamp;    /* Stunden- (epoch/3600) bzw. Tagesnummer (epoch/86400) */
    int16_t value;    /* Milli-Euro */
} log_point_t;

/* NVS oeffnen (Namespace aus config.h). */
void price_log_init(void);

/* Fenster um "jetzt" aus dem NVS in den RAM laden (nach der Zeit-Synchronisation). */
void price_log_load(time_t now);

/* Einen Messwert einarbeiten (schreibt nur, wenn sich etwas geaendert hat). */
void price_log_add(time_t now, const fuel_prices_t *p);

/* Einen fertigen Stunden-/Tageswert nur in den Arbeitsspeicher setzen - fuer
 * das Nachladen aus dem Archiv auf der Karte. Geschrieben wird dabei nichts
 * ins NVS: bei 30 Tagen waeren das hunderte Schreibvorgaenge ins Flash.
 * Mehrere Werte derselben Stunde werden zusammengefasst (kleinstes/groesstes
 * Minimum/Maximum, letzter Wert gewinnt). */
void price_log_set_raw(time_t now, int fuel, int min_milli, int max_milli, int last_milli);

/* Reihe fuer das Diagramm: aeltester Wert zuerst, nur gueltige Punkte.
 * daily=true -> Tageswerte, sonst Stundenwerte.
 * Rueckgabe: Anzahl der gefuellten Punkte (<= max). */
size_t price_log_series(bool daily, int fuel, int field, log_point_t *out, size_t max);

/* Alle gespeicherten Werte loeschen (Konsole: log reset). */
void price_log_reset(void);
