/*
 * sd_log.h - Logausgabe zusaetzlich auf die microSD-Karte
 *
 * Am Geraet haengt nicht immer ein Rechner. Was in der Nacht passiert ist
 * (WLAN weg, Preisabfrage fehlgeschlagen, Uhr aus dem Archiv gestellt),
 * waere sonst nirgends nachzulesen. Dieses Modul schreibt dieselben Zeilen,
 * die ueber die serielle Schnittstelle gehen, zusaetzlich in eine Datei:
 *
 *   /sdcard/log-JJJJ-MM.txt        (eine Datei je Monat, wird angehaengt)
 *
 * Ohne Karte aendert sich nichts - die Ausgabe bleibt dann nur auf der
 * seriellen Schnittstelle.
 */

#pragma once

#include <stdarg.h>
#include <stdbool.h>

#include "esp_err.h"

/* Logausgabe auf die Karte umleiten (zusaetzlich zur Konsole).
 * Muss nach sd_archive_init() gerufen werden - vorher gibt es kein
 * Dateisystem. Ohne Karte passiert nichts. */
esp_err_t sd_log_init(void);

/* true, wenn in eine Datei geschrieben wird. */
bool sd_log_aktiv(void);

/* Eine Zeile auf der Konsole ausgeben und mitprotokollieren. Wird von
 * console.c als printf-Ersatz benutzt (siehe #define dort). */
int sd_log_printf(const char *fmt, ...);

/* Dateiname der laufenden Logdatei ("" wenn nicht aktiv). */
const char *sd_log_datei(void);

/* Zaehler seit dem Start: geschriebene und verworfene Zeilen. Damit laesst
 * sich pruefen, ob wirklich mitgeschrieben wird, ohne die Karte zu ziehen. */
void sd_log_statistik(unsigned *geschrieben, unsigned *verworfen);
