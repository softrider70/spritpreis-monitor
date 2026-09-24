/*
 * sim.h - Simulationsmodus (Testbetrieb ohne API-Key)
 *
 * Solange kein API-Key vorhanden ist, liefert die Simulation erfundene,
 * aber plausible Preise. Sie bildet die MTS-K-Regel nach: Erhöhung um
 * 12:00 Uhr, danach stundenweise Senkungen bis 22:00 Uhr, dann bleibt der
 * Preis bis zum nächsten Mittag stehen.
 *
 * Der Wert ist deterministisch: gleiche Uhrzeit = gleicher Preis. Dadurch
 * lässt sich ein Verlauf rückwirkend erzeugen und mehrfach prüfen.
 *
 * Immer sichtbar als Simulation gekennzeichnet: "SIM" in der Statuszeile
 * der Anzeige und status="sim" in der CSV-Datei auf der Karte.
 */

#pragma once

#include <stdbool.h>
#include "fuel_api.h"

/* Ist der Simulationsmodus eingeschaltet? (Zustand liegt im NVS.) */
bool sim_enabled(void);

/* Simulationsmodus ein-/ausschalten (schreibt ins NVS). */
void sim_set_enabled(bool on);

/* Einen simulierten Preis für "jetzt" liefern. */
void sim_fetch(fuel_prices_t *out);

/* Einen simulierten Preis für einen beliebigen Zeitpunkt liefern. */
void sim_fetch_at(time_t t, fuel_prices_t *out);

/* Verlauf und Archiv rückwirkend füllen (Verlaufsspeicher + CSV).
 * Füllt die letzten `tage` Tage stündlich und zusätzlich ältere Tage als
 * Tageswerte, damit auch die Tagesansicht etwas zeigt.
 * Rückgabe: Anzahl der erzeugten Werte, 0 wenn die Uhr fehlt. */
int sim_fill(int tage);

/* Fuell-Wunsch vormerken (z. B. aus der Konfigurationsdatei). Der Wunsch wird
 * ausgefuehrt, sobald die Uhr gestellt ist - vorher gibt es keine
 * Zeitstempel. */
void sim_request_fill(int tage);
int  sim_fill_wunsch(void);
void sim_fill_wunsch_erledigt(void);

/* Zustand auf der Konsole ausgeben (Befehl "sim"). */
void sim_print_status(void);
