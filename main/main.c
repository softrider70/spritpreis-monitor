/*
 * main.c - Spritpreis-Monitor (CYD ESP32-2432S028R)
 *
 * Startreihenfolge:
 *   1. NVS, Display, Touch
 *   2. Konsole (damit man auch ohne Netz etwas einstellen kann)
 *   3. WLAN + Zeitsynchronisation (SNTP)
 *   4. Verlauf aus dem NVS laden (braucht eine gestellte Uhr)
 *   5. Abfragetakt starten
 *
 * Datenquelle: Tankerkönig-API (MTS-K, Bundeskartellamt), CC BY 4.0.
 */

#include <string.h>
#include <time.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "nvs_flash.h"
#include "esp_log.h"
#include "esp_netif_sntp.h"
#include "esp_timer.h"

#include "config.h"
#include "console.h"
#include "display.h"
#include "fuel_poll.h"
#include "power.h"
#include "price_log.h"
#include "sd_archive.h"
#include "sd_config.h"
#include "sd_log.h"
#include "settings.h"
#include "touch.h"
#include "ui.h"
#include "version.h"
#include "wifi.h"

static const char *TAG = "main";

/* Zeichnen und Zeitpunkt merken (das Ziehen wird gedrosselt neu gezeichnet). */
static int64_t s_letztes_zeichnen = 0;

/* Zeichnen (Zeitpunkt nicht mehr noetig, aber praktisch fuer spaetere Drosselung). */
static void anzeige_zeichnen(void)
{
    s_letztes_zeichnen = esp_timer_get_time();
    /* Diagnose: zeigt im Log, wie oft wirklich gezeichnet wird. Ist der Inhalt
     * unveraendert, baut ui_render das Bild nicht erneut auf. */
    ESP_LOGI(TAG, "Anzeige aktualisiert");
    ui_render(fuel_poll_last(), time_is_valid(), fuel_poll_net_ok());
}

/* Touch: Taps an die Anzeige, Bewegungen zum Verschieben des Diagramms.
 * Jede Beruehrung schaltet die Anzeige ein (Stromsparmodus).
 *
 * Wichtig: Erst den Zustand aendern, dann wecken. power_activity() zeichnet
 * beim Wecken selbst - und zwar genau diesen neuen Zustand. Wer danach noch
 * einmal zeichnet, sieht den zweiten Bildaufbau als Blitzen.
 *
 * Der Druck, der die Anzeige aufweckt, ist NUR zum Aufwecken da: Er darf
 * keine Funktion im Hauptprogramm ausloesen (sonst schaltet derselbe Tipp
 * gleich das Zeitfenster um oder blaettert im Diagramm). s_touch_weckte
 * merkt sich das bis zum Loslassen; erst der naechste Druck bedient wieder. */
static bool s_touch_weckte = false;

static void on_touch_tap(int x, int y, void *arg)
{
    (void)arg;

    if (s_touch_weckte) {
        /* Ende der Weckberuehrung - nichts umschalten, nichts blaettern. */
        s_touch_weckte = false;
        return;
    }
    if (!power_display_on()) {
        return;
    }
    ui_touch_tap(x, y);
    anzeige_zeichnen();
}

static void on_touch_move(int x, int y, bool pressed, void *arg)
{
    (void)arg;

    if (s_touch_weckte) {
        /* Diese Beruehrung hat nur geweckt - sie bedient noch nichts. */
        if (!pressed) {
            s_touch_weckte = false;
        }
        return;
    }

    if (pressed && !power_display_on()) {
        /* Aufsetzen bei ausgeschalteter Anzeige: weckt nur. Das Bild zeichnet
         * power_activity() selbst ueber den Wake-Callback. */
        power_activity();
        s_touch_weckte = true;
        return;
    }

    if (!power_display_on()) {
        return;                       /* nichts zu sehen, nichts zu bedienen */
    }

    const bool verschoben = ui_touch_move(x, y, pressed);
    /* Nur neu zeichnen, wenn sich das Fenster wirklich verschoben hat, und
     * dann nur das Diagramm: der Touch-Task meldet im Takt Bewegung, ein
     * voller Bildaufbau waere langsam und mehrfach sichtbar. */
    if (verschoben) {
        ui_render_chart();
    }
}

/* Wird von power.c gerufen, wenn das Display aufgeweckt wurde */
static void on_display_wake(void)
{
    anzeige_zeichnen();
}

static void nvs_init(void)
{
    esp_err_t err = nvs_flash_init();
    if (err == ESP_ERR_NVS_NO_FREE_PAGES || err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_LOGW(TAG, "NVS wird neu formatiert");
        ESP_ERROR_CHECK(nvs_flash_erase());
        err = nvs_flash_init();
    }
    ESP_ERROR_CHECK(err);
}

void app_main(void)
{
    ESP_LOGI(TAG, "%s %s (Build %d vom %s)", BOARD_NAME, APP_VERSION_STRING,
             BUILD_NUMBER, BUILD_TIMESTAMP);
    ESP_LOGI(TAG, "Chip: %s", BOARD_CHIP);

    nvs_init();

    /* --- Anzeige --- */
    ESP_ERROR_CHECK(display_init());
    display_backlight(true);
    ui_init();

    /* --- Touch --- */
    if (touch_init() == ESP_OK) {
        touch_task_start_full(on_touch_tap, on_touch_move, NULL);
    } else {
        ESP_LOGW(TAG, "Touch nicht verfuegbar - Anzeige laeuft ohne Bedienung");
    }

    price_log_init();

    /* --- Langzeitarchiv auf der microSD (Karte darf fehlen) --- */
    if (sd_archive_init() != ESP_OK) {
        ESP_LOGW(TAG, "Kein Archiv - Geraet laeuft mit dem NVS-Verlauf weiter");
    } else {
        /* Zuerst das Mitprotokollieren einschalten, damit auch die folgenden
         * Zeilen auf der Karte landen, dann den Verlauf holen. */
        sd_log_init();
        /* Verlauf aus dem Archiv holen, damit die Kurve nach dem Trennen der
         * Stromversorgung wieder steht (der Arbeitsspeicher ist dann leer). */
        sd_archive_laden();
    }

    /* --- Zugangsdaten aus /sdcard/spritpreis.cfg uebernehmen --- */
    int uebernommen = sd_config_import();
    if (uebernommen > 0) {
        ESP_LOGI(TAG, "%d Wert(e) aus der Konfigurationsdatei uebernommen", uebernommen);
    }

    /* --- Stromsparen: Taster, Wakeup-Pins, Light-Sleep --- */
    power_set_wake_callback(on_display_wake);
    power_init();

    console_start();

    /* --- Netz und Zeit --- */
    bool zeit_ok = false;
    if (wifi_start() == ESP_OK) {
        if (wifi_wait_connected(15000)) {
            time_start_sntp();
            if (esp_netif_sntp_sync_wait(pdMS_TO_TICKS(15000)) == ESP_OK) {
                zeit_ok = time_is_valid();
            } else {
                ESP_LOGW(TAG, "Keine Zeit vom SNTP-Server");
            }
        } else {
            ESP_LOGW(TAG, "Kein WLAN - Anzeige laeuft ohne neue Preise");
        }
    }

    /* --- Verlauf laden ---
     * Das passiert im Poll-Task, sobald die Uhr gueltig ist: SNTP braucht
     * manchmal laenger, als der Start hier abwartet (sonst waeren Diagramm
     * und Trendpfeile nach jedem Start leer). */
    if (!zeit_ok) {
        ESP_LOGW(TAG, "Uhr noch nicht gestellt - Verlauf wird nachgeladen, "
                      "sobald die Zeit da ist");
    }

    /* --- Abfragetakt --- */
    fuel_poll_start();

    /* Letzte Meldung beim Start: die Build-Nummer. Damit ist im Log und auf
     * der Konsole sofort zu sehen, welcher Stand wirklich laeuft - beim
     * Flashen mehrerer Staende hintereinander sonst kaum zu unterscheiden. */
    ESP_LOGI(TAG, "%s bereit - Build %d", BOARD_NAME, BUILD_NUMBER);
}
