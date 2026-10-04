/*
 * battery.h - Batteriespannung der 1S-LiIon-Zelle
 *
 * Die Zelle wird ueber einen Spannungsteiler an einem ADC1-Pin gemessen
 * (Aufbau und Werte: BATTERY_* in config.h). Der Wert wird geglaettet und
 * nur alle BATTERY_MESS_TAKT_MS neu bestimmt - eine Messung kostet ein paar
 * Millisekunden, so oft braucht sie niemand.
 */

#pragma once

#include <stdbool.h>
#include <stddef.h>

/* ADC einrichten (einmal beim Start aufrufen). */
void battery_init(void);

/* true, wenn die Messung eingerichtet werden konnte. */
bool battery_ok(void);

/* Zellspannung in Millivolt; 0 = keine gueltige Messung. */
int battery_millivolt(void);

/* Spannung am Messpunkt (vor dem Teiler) in Millivolt - zum Pruefen der
 * Beschaltung mit dem Multimeter. 0 = keine Messung. */
int battery_pin_millivolt(void);

/* Fertiger Text fuer die Anzeige, z. B. "3,98 V" (ohne Messung "--,-- V"). */
void battery_text(char *dst, size_t len);
