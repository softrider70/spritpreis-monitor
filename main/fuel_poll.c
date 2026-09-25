/*
 * fuel_poll.c - Abfragetakt und Anzeige-Aktualisierung
 *
 * Ablauf je Runde:
 *   1. WLAN pruefen (wifi_ensure_connected)
 *   2. Einstellungen aus dem NVS lesen (aendern sich per Konsole)
 *   3. Preise holen (fuel_api_fetch)
 *   4. bei Erfolg: Verlauf fortschreiben (price_log_add)
 *   5. Anzeige neu zeichnen (ui_render)
 *
 * Laeuft auf Core 0, damit TLS/HTTP nicht mit dem Display-Task (Core 1)
 * um dieselbe CPU konkurriert.
 */

#include <string.h>
#include <time.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"
#include "esp_timer.h"

#include "config.h"
#include "fuel_poll.h"
#include "power.h"
#include "price_log.h"
#include "sd_archive.h"
#include "settings.h"
#include "sim.h"
#include "ui.h"
#include "wifi.h"

static const char *TAG = "poll";

/* Bedingung der freien Tankerkoenig-API: hoechstens EIN Aufruf je Minute.
 * Der Takt liegt mit 5 Minuten deutlich darunter - aber "poll" per Konsole,
 * der "key"-Befehl und ein Neustart koennten sonst dagegen verstossen. Deshalb
 * wird der Abstand hier hart geprueft und nicht nur eingehalten. */
#define API_MIN_ABSTAND_US  (60LL * 1000000LL)

static int64_t s_letzte_abfrage_us = 0;

/* true, wenn seit der letzten Abfrage genug Zeit vergangen ist. Jeder Versuch
 * zaehlt - auch ein fehlgeschlagener, denn der Aufruf ging ja raus. */
static bool api_abstand_ok(void)
{
    const int64_t jetzt = esp_timer_get_time();
    if (s_letzte_abfrage_us != 0 && (jetzt - s_letzte_abfrage_us) < API_MIN_ABSTAND_US) {
        return false;
    }
    s_letzte_abfrage_us = jetzt;
    return true;
}

static fuel_prices_t s_last;
static volatile bool s_net_ok = false;
static volatile bool s_force = false;
static TaskHandle_t s_task = NULL;
static bool s_verlauf_geladen = false;
static bool s_sntp_gestartet = false;
static bool s_uhr_gemeldet = false;

/* Wann die zuletzt angezeigten Werte geholt wurden. Die Werte selbst bleiben
 * bei einer fehlgeschlagenen Abfrage stehen - dieses Datum aber nicht, sonst
 * wuerde die Anzeige alte Daten als frisch ausgeben. */
static volatile time_t s_messung_zeit = 0;

const fuel_prices_t *fuel_poll_last(void)
{
    return &s_last;
}

time_t fuel_poll_data_time(void)
{
    return s_messung_zeit;
}

bool fuel_poll_net_ok(void)
{
    return s_net_ok;
}

void fuel_poll_now(void)
{
    s_force = true;
    if (s_task) {
        xTaskNotifyGive(s_task);          /* wartende Schleife sofort wecken */
    }
}

static void poll_task(void *arg)
{
    (void)arg;
    s_task = xTaskGetCurrentTaskHandle();
    char station[SETTINGS_STATION_MAX];
    char api_key[SETTINGS_KEY_MAX];
    char name[SETTINGS_NAME_MAX];

    for (;;) {
        wifi_ensure_connected();
        settings_load(station, sizeof(station), api_key, sizeof(api_key));
        settings_get_name(name, sizeof(name));

        /* SNTP nur beim Start zu beginnen reicht nicht: Ist das WLAN erst
         * spaeter verbunden (oder war die Verbindung weg), fehlt die Uhr
         * sonst dauerhaft - und ohne Uhr gibt es weder Verlauf noch Archiv.
         * Deshalb hier nachholen, sobald eine Verbindung steht. */
        if (!s_sntp_gestartet && wifi_is_connected() && !time_is_valid()) {
            time_start_sntp();
            s_sntp_gestartet = true;
            ESP_LOGI(TAG, "Zeit wird nachtraeglich geholt (SNTP)");
        }

        /* Wird die Uhr erst spaeter gestellt (SNTP braucht manchmal laenger,
         * als der Start abwartet), muss der Verlauf aus dem NVS nachtraeglich
         * ins RAM - sonst bleiben Diagramm und Trendpfeile leer, obwohl
         * Daten vorhanden sind. */
        if (!s_verlauf_geladen && time_is_valid()) {
            price_log_load(time(NULL));
            /* Der NVS-Verlauf endet dort, wo zuletzt geschrieben wurde. Nach
             * einem Stromausfall fehlen die Werte dazwischen - die stehen im
             * Archiv auf der Karte und werden hier ergaenzt. */
            sd_archive_laden();
            s_verlauf_geladen = true;
            ESP_LOGI(TAG, "Uhr gestellt - Verlauf aus dem NVS und dem Archiv geladen");
            power_activity();      /* Anzeige auffrischen, damit die Daten sichtbar sind */
        }

        /* Fuellwunsch aus der Konfigurationsdatei (fill=<tage>): geht erst mit
         * gueltiger Uhr, weil sonst keine Zeitstempel moeglich sind. */
        if (time_is_valid() && sim_fill_wunsch() > 0) {
            const int tage = sim_fill_wunsch();
            sim_fill_wunsch_erledigt();   /* nur einmal */
            ESP_LOGI(TAG, "Verlauf wird gefuellt (%d Tage)", tage);
            sim_fill(tage);               /* laedt den Verlauf am Ende selbst neu */
            s_verlauf_geladen = true;
            power_activity();             /* Ergebnis gleich zeigen */
        }

        /* Simulation braucht weder Tankstellen-ID noch API-Key noch Netz. */
        const bool sim = sim_enabled();
        bool bereit = true;
        s_net_ok = false;

        if (sim) {
            /* nichts zu pruefen */
        } else if (station[0] == 0) {
            ESP_LOGW(TAG, "Keine Tankstellen-ID gesetzt (Konsole: station <uuid>)");
            ui_message("Tankstelle fehlt",
                       "Konsole: station <uuid>",
                       "ID holen mit tools/find_station.ps1");
            bereit = false;
        } else if (api_key[0] == 0) {
            ESP_LOGW(TAG, "Kein API-Key gesetzt (Konsole: key <apikey>)");
            ui_message("API-Key fehlt",
                       "Konsole: key <apikey>  oder: sim on",
                       "Key: creativecommons.tankerkoenig.de");
            bereit = false;
        } else if (!time_is_valid()) {
            /* Ohne gestellte Uhr wird nicht abgefragt: Die verschluesselte
             * Verbindung prueft das Zertifikat gegen die eigene Uhrzeit, mit
             * einem Datum von 1970 schlaegt sie fehl. Ausserdem gibt es ohne
             * Uhr keine Zeitstempel fuer Verlauf und Archiv. */
            if (!s_uhr_gemeldet) {
                s_uhr_gemeldet = true;
                ESP_LOGW(TAG, "Uhr noch nicht gestellt - es wird noch nicht "
                              "abgefragt (kommt mit SNTP bzw. aus dem Archiv)");
            }
            ui_message("Uhr fehlt", "Warte auf die Zeit (WLAN/SNTP)",
                       "Preise kommen, sobald die Uhr steht");
            bereit = false;
        } else if (!wifi_is_connected()) {
            ESP_LOGW(TAG, "Kein WLAN - bitte Zugangsdaten pruefen");
            ui_message("Kein WLAN", "Konsole: wifi <ssid> <passwort>", "");
            bereit = false;
        }

        bool uebersprungen = false;
        /* Kopfzeile: der Anzeigename gilt unabhaengig davon, ob gerade Daten
         * geholt werden koennen - er steht auch ohne Netz richtig da. */
        ui_set_station_label(sim ? "SIMULATION - Testpreise"
                                 : (name[0] ? name : "Tankstelle"));

        if (bereit) {
            fuel_prices_t p = { 0 };
            esp_err_t err = ESP_FAIL;
            if (sim) {
                /* Testpreise brauchen weder Uhr noch Netz noch die API */
                sim_fetch(&p);
                err = ESP_OK;
            } else if (!api_abstand_ok()) {
                /* Grenze der freien API: nur eine Abfrage je Minute. Der
                 * letzte bekannte Stand bleibt stehen. */
                ESP_LOGW(TAG, "Abstand zur letzten Abfrage zu kurz - die freie "
                              "API erlaubt nur eine Abfrage je Minute");
                uebersprungen = true;
            } else {
                err = fuel_api_fetch(station, api_key, &p);
            }
            s_net_ok = uebersprungen ? s_net_ok : (err == ESP_OK);
            if (!uebersprungen) {
                s_uhr_gemeldet = false;      /* naechstes Mal wieder melden */
                if (err == ESP_OK) {
                    ui_message_clear();
                    bool geaendert = false;
                    for (int f = 0; f < FUEL_COUNT; f++) {
                        if (p.price_milli[f] != s_last.price_milli[f]) {
                            geaendert = true;
                        }
                    }
                    s_last = p;
                    s_messung_zeit = time(NULL);   /* Zeitpunkt dieser Messung */
                    price_log_add(time(NULL), &p);
                    sd_archive_add(time(NULL), &p);   /* Langzeitarchiv auf der Karte */
                    if (geaendert) {
                        /* Preis hat sich bewegt -> Anzeige fuer POWER_DISPLAY_ON_MS
                         * einschalten (der Nutzer sieht die Aenderung ohne Tippen). */
                        ESP_LOGI(TAG, "Preis geaendert - Anzeige %d s aktiv",
                                 POWER_DISPLAY_ON_MS / 1000);
                        power_activity();
                    }
                }
            }
            ui_render(&s_last, time_is_valid(), s_net_ok);
        } else {
            /* Nichts zu holen (ID/Key/WLAN/Uhr fehlt): Zustand auch anzeigen */
            ui_render(&s_last, time_is_valid(), false);
        }

        /* Bis zum naechsten Takt warten - oder bis "poll" sofort anfordert.
         * Bewusst keine 100-ms-Schleife: so kann die CPU dazwischen in den
         * Light-Sleep gehen (Stromsparen). */
        if (!s_force) {
            ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(FUEL_POLL_INTERVAL_S * 1000));
        }
        if (s_force) {
            ESP_LOGI(TAG, "Sofortabfrage angefordert");
            s_force = false;
        }
    }
}

void fuel_poll_start(void)
{
    if (xTaskCreatePinnedToCore(poll_task, "fuel_poll", TASK_STACK_POLL, NULL,
                                5, NULL, TASK_CORE_NET) != pdPASS) {
        ESP_LOGE(TAG, "Poll-Task konnte nicht gestartet werden");
        return;
    }
    ESP_LOGI(TAG, "Poll-Task gestartet (alle %d s)", FUEL_POLL_INTERVAL_S);
}
