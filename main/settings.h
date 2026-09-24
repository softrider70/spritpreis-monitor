/*
 * settings.h - Einstellungen im NVS (Tankstellen-ID, API-Key)
 *
 * Namespace "spritcfg". Die WLAN-Zugangsdaten liegen im selben Namespace
 * (Keys "ssid"/"pass", siehe wifi.c).
 */

#pragma once

#include <stddef.h>
#include <stdbool.h>
#include "esp_err.h"

#define SETTINGS_STATION_MAX  40
#define SETTINGS_KEY_MAX      48
#define SETTINGS_NAME_MAX     40

/* Laedt Tankstellen-ID und API-Key (leere Zeichenkette = nicht gesetzt). */
esp_err_t settings_load(char *station, size_t station_len, char *api_key, size_t key_len);

esp_err_t settings_set_station(const char *station_id);
esp_err_t settings_set_api_key(const char *api_key);

/* Anzeigename der Tankstelle fuer die Kopfzeile (z. B. "Star Musterstadt 1").
 * Leer = es steht nur "Tankstelle" da. Der Name kommt nicht von der API -
 * die kennt nur die ID. */
esp_err_t settings_get_name(char *dst, size_t len);
esp_err_t settings_set_name(const char *name);

/* Simulationsmodus (sim.c): mit erfundenen Preisen arbeiten. */
bool settings_get_sim(void);
esp_err_t settings_set_sim(bool on);

/* Format der Archivdatei: true = kompakt ohne ISO-Zeitspalte
 * ("1790268401;2309;2249;2409;open"), false = mit lesbarer Zeit
 * ("2026-09-24T18:46:41;..."). Gelesen werden immer beide. */
bool settings_get_csv_kompakt(void);
esp_err_t settings_set_csv_kompakt(bool kompakt);
