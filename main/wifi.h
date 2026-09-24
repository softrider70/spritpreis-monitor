/*
 * wifi.h - WLAN-Verbindung + Zeit (SNTP) fuer den Spritpreis-Monitor
 *
 * Die Zugangsdaten liegen im NVS (Namespace "spritcfg", Keys "ssid"/"pass")
 * und werden beim Start automatisch verwendet. Setzen per Konsole:
 *   wifi <ssid> <passwort>
 */

#pragma once

#include <stdbool.h>
#include <stddef.h>
#include "esp_err.h"

/* Initialisiert Netif/Event/WLAN und verbindet mit den gespeicherten Daten.
 * Ohne gespeicherte Zugangsdaten: ESP_ERR_NOT_FOUND (kein Fehlerfall). */
esp_err_t wifi_start(void);

/* Wartet bis zu ms Millisekunden auf eine IP-Adresse. true = verbunden. */
bool wifi_wait_connected(uint32_t ms);

/* true, wenn gerade eine IP-Adresse anliegt. */
bool wifi_is_connected(void);

/* Holt die Verbindung zurueck, falls sie weg ist (max. ein Versuch je 30 s).
 * Wird vom Poll-Task aufgerufen. */
void wifi_ensure_connected(void);

/* Listet die Netze in Reichweite mit Sicherheitsmodus auf (Diagnose).
 * Anzeige im Log/auf der Konsole. */
void wifi_scan_print(void);

/* Gespeicherten Netzwerknamen ("" = keiner). */
const char *wifi_ssid(void);

/* Zugangsdaten speichern (NVS) und sofort neu verbinden. */
esp_err_t wifi_set_credentials(const char *ssid, const char *pass);

/* true, wenn Name und Passwort gespeichert sind. */
bool wifi_has_credentials(void);

/* Zugangsdaten loeschen. */
esp_err_t wifi_clear_credentials(void);

/* Startet SNTP (Zeitzone aus config.h) - nur noetig, wenn WLAN steht. */
esp_err_t time_start_sntp(void);

/* true, wenn die Uhr plausibel gestellt ist (fuer Stunden-/Tageseinteilung). */
bool time_is_valid(void);
