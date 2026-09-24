/*
 * fuel_api.h - Abfrage der aktuellen Spritpreise (Tankerkönig / MTS-K)
 *
 * Ein Aufruf holt die Preise EINER Tankstelle:
 *   GET https://creativecommons.tankerkoenig.de/json/prices.php?ids=<uuid>&apikey=<key>
 *
 * Antwort (verifiziert 2026-09-23):
 *  { "ok": true, "license": "CC BY 4.0 ...", "data": "MTS-K",
 *    "prices": { "<uuid>": { "status": "open", "e5": 1.234, "e10": 1.234,
 *                            "diesel": 1.234 } } }
 * Bei geschlossener Tankstelle steht "status": "closed" und die Preisfelder
 * sind false. Fehler kommen als { "ok": false, "message": "..." }.
 *
 * Preise werden als Milliardstel-Euro gefuehrt (1.234 EUR -> 1234), damit
 * im Verlaufsspeicher keine Gleitkommazahlen noetig sind.
 */

#pragma once

#include <stdbool.h>
#include "esp_err.h"
#include "config.h"

typedef struct {
    bool api_ok;                       /* Serverantwort war "ok": true */
    char status[16];                   /* "open", "closed", "no data", "?" */
    bool is_open;                      /* status == "open" */
    int  price_milli[FUEL_COUNT];      /* 0.001 EUR, 0 = unbekannt */
} fuel_prices_t;

/* Holt die Preise. Rueckgabe:
 *  ESP_OK                = Antwort ausgewertet (api_ok sagt, ob sie gueltig ist)
 *  ESP_ERR_INVALID_ARG   = Station-ID oder API-Key fehlt
 *  andere                = Netz-/TLS-/HTTP-Fehler (Preise bleiben 0) */
esp_err_t fuel_api_fetch(const char *station_id, const char *api_key, fuel_prices_t *out);

/* Anzeigename des Kraftstoffs ("Super E5", "Super E10", "Diesel") */
const char *fuel_name(int idx);

/* Kurzname fuer die Anzeige ("E5", "E10", "Diesel") */
const char *fuel_short_name(int idx);
