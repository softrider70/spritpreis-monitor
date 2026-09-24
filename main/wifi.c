/*
 * wifi.c - WLAN (STA) + Zeitsynchronisation
 *
 * Bewusst schlank: nur eine Station, kein AP, kein Captive Portal.
 * Zugangsdaten im NVS, Verbindungsaufbau blockierend mit Timeout.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <sys/time.h>

#include "freertos/FreeRTOS.h"
#include "freertos/event_groups.h"
#include "freertos/task.h"

#include "esp_wifi.h"
#include "esp_event.h"
#include "esp_mac.h"
#include "esp_netif.h"
#include "esp_netif_sntp.h"
#include "esp_log.h"
#include "nvs.h"

#include "config.h"
#include "wifi.h"

static const char *TAG = "wifi";

#define WIFI_NVS_NAMESPACE   "spritcfg"
#define BIT_CONNECTED        BIT0

static EventGroupHandle_t s_events = NULL;
static volatile bool s_connected = false;
static char s_ssid[33] = { 0 };
static TickType_t s_last_try = 0;

/* Schwaechster akzeptierter Sicherheitsmodus.
 * Ohne diesen Wert nimmt der Stack WPA2 an, sobald ein Passwort mit
 * mindestens 8 Zeichen gesetzt ist (siehe esp_wifi_types_generic.h) - ein
 * reiner WPA-Zugangspunkt wird dann gar nicht erst gefunden.
 * WPA und alles Staerkere ja, offene und WEP-Netze nein. */
#define WIFI_SCHWELLE   WIFI_AUTH_WPA_PSK

/* Haeufige Abbruchgruende des Stacks. Namen und Nummern stammen aus
 * esp_wifi_types_generic.h (wifi_err_reason_t). */
static const char *reason_text(int r)
{
    switch (r) {
    case 15:  return "4-Way-Handshake-Zeit abgelaufen (meist falsches Passwort)";
    case 17:  return "Informationselement im Handshake passt nicht";
    case 18:  return "Gruppenschluessel ungueltig";
    case 19:  return "Paarschluessel ungueltig";
    case 23:  return "802.1X-Anmeldung abgelehnt";
    case 24:  return "Verschluesselung vom Zugangspunkt abgelehnt";
    case 29:  return "Verschluesselung und Verfahren passen nicht zusammen";
    case 200: return "Zugangspunkt nicht mehr gehoert (zu weit weg)";
    case 201: return "Zugangspunkt nicht gefunden";
    case 202: return "Anmeldung abgelehnt (meist falsches Passwort)";
    case 203: return "Verbindung abgelehnt";
    case 204: return "Handshake-Zeit abgelaufen";
    case 205: return "Verbindungsaufbau fehlgeschlagen";
    case 210: return "kein Zugangspunkt mit passender Verschluesselung";
    case 211: return "kein Zugangspunkt ueber der Sicherheitsschwelle";
    case 212: return "kein Zugangspunkt ueber der Signalstaerke-Schwelle";
    default:  return "unbekannt";
    }
}

/* Sicherheitsmodus eines gefundenen Netzes als Text. */
static const char *authmode_text(wifi_auth_mode_t m)
{
    switch (m) {
    case WIFI_AUTH_OPEN:          return "offen";
    case WIFI_AUTH_WEP:           return "WEP";
    case WIFI_AUTH_WPA_PSK:       return "WPA";
    case WIFI_AUTH_WPA2_PSK:      return "WPA2";
    case WIFI_AUTH_WPA_WPA2_PSK:  return "WPA/WPA2";
    case WIFI_AUTH_ENTERPRISE:    return "WPA2-Unternehmen";
    case WIFI_AUTH_WPA3_PSK:      return "WPA3";
    case WIFI_AUTH_WPA2_WPA3_PSK: return "WPA2/WPA3";
    case WIFI_AUTH_WAPI_PSK:      return "WAPI";
    case WIFI_AUTH_OWE:           return "OWE (offen, verschluesselt)";
    default:                      return "unbekannt";
    }
}

/* ------------------------------------------------------------------ */
/* Ereignisse                                                        */
/* ------------------------------------------------------------------ */
static void wifi_event_handler(void *arg, esp_event_base_t base, int32_t id, void *data)
{
    (void)arg;

    if (base == WIFI_EVENT && id == WIFI_EVENT_STA_START) {
        esp_wifi_connect();
    } else if (base == WIFI_EVENT && id == WIFI_EVENT_STA_DISCONNECTED) {
        const wifi_event_sta_disconnected_t *ev =
            (const wifi_event_sta_disconnected_t *)data;
        s_connected = false;
        if (s_events) {
            xEventGroupClearBits(s_events, BIT_CONNECTED);
        }
        /* Den Grund nennt ausschliesslich der Stack selbst - ohne diese Zeile
         * ist jede Ursachenanalyse im Log reine Vermutung. */
        ESP_LOGW(TAG, "Verbindung getrennt (Grund %d: %s)",
                 ev ? ev->reason : -1, reason_text(ev ? ev->reason : -1));
    } else if (base == IP_EVENT && id == IP_EVENT_STA_GOT_IP) {
        const ip_event_got_ip_t *ev = (const ip_event_got_ip_t *)data;
        ESP_LOGI(TAG, "Verbunden, IP: " IPSTR, IP2STR(&ev->ip_info.ip));
        s_connected = true;
        if (s_events) {
            xEventGroupSetBits(s_events, BIT_CONNECTED);
        }
    }
}

/* ------------------------------------------------------------------ */
/* Zugangsdaten im NVS                                               */
/* ------------------------------------------------------------------ */
static bool load_credentials(char *ssid, size_t ssid_len, char *pass, size_t pass_len)
{
    nvs_handle_t h;
    if (nvs_open(WIFI_NVS_NAMESPACE, NVS_READONLY, &h) != ESP_OK) {
        return false;
    }
    size_t len_ssid = ssid_len;
    size_t len_pass = pass_len;
    esp_err_t e1 = nvs_get_str(h, "ssid", ssid, &len_ssid);
    esp_err_t e2 = nvs_get_str(h, "pass", pass, &len_pass);
    nvs_close(h);
    if (e1 != ESP_OK) {
        ssid[0] = 0;
    }
    if (e2 != ESP_OK) {
        pass[0] = 0;
    }
    return (e1 == ESP_OK && ssid[0] != 0);
}

/* Einheitliche Konfiguration der Station - beide Verbindungswege nutzen sie,
 * damit sie nicht auseinanderlaufen (genau das fehlte hier schon einmal).
 *
 * Der Auswahlmodus ist der wichtig: ohne Angabe gilt WIFI_FAST_SCAN, denn
 * diese Konstante hat den Wert 0 und die Struktur wird mit Nullen gefuellt.
 * Dann endet der Scan beim ERSTEN Namenstreffer, ohne die Signalstaerke zu
 * vergleichen. Strahlt ein Repeater dieselbe SSID auf zwei Kanaelen aus,
 * gewinnt so der zuerst gefundene - und das ist nicht der staerkste.
 * WIFI_ALL_CHANNEL_SCAN mit WIFI_CONNECT_AP_BY_SIGNAL nimmt den staerksten. */
static void sta_config_setzen(wifi_config_t *cfg, const char *ssid, const char *pass)
{
    memset(cfg, 0, sizeof(*cfg));
    strncpy((char *)cfg->sta.ssid, ssid, sizeof(cfg->sta.ssid) - 1);
    strncpy((char *)cfg->sta.password, pass, sizeof(cfg->sta.password) - 1);
    cfg->sta.scan_method = WIFI_ALL_CHANNEL_SCAN;
    cfg->sta.sort_method = WIFI_CONNECT_AP_BY_SIGNAL;
    cfg->sta.threshold.authmode = WIFI_SCHWELLE;
    cfg->sta.pmf_cfg.capable = true;      /* Zugangspunkte mit PMF zulassen */
    cfg->sta.pmf_cfg.required = false;    /* aber nicht verlangen */
}

esp_err_t wifi_set_credentials(const char *ssid, const char *pass)
{
    if (!ssid || !pass) {
        return ESP_ERR_INVALID_ARG;
    }
    nvs_handle_t h;
    esp_err_t err = nvs_open(WIFI_NVS_NAMESPACE, NVS_READWRITE, &h);
    if (err != ESP_OK) {
        return err;
    }
    nvs_set_str(h, "ssid", ssid);
    nvs_set_str(h, "pass", pass);
    err = nvs_commit(h);
    nvs_close(h);

    strncpy(s_ssid, ssid, sizeof(s_ssid) - 1);
    s_ssid[sizeof(s_ssid) - 1] = 0;
    ESP_LOGI(TAG, "Zugangsdaten gespeichert fuer SSID \"%s\"", s_ssid);

    /* sofort neu verbinden */
    wifi_config_t cfg;
    sta_config_setzen(&cfg, ssid, pass);
    if (s_events) {
        esp_wifi_disconnect();
        esp_wifi_set_config(WIFI_IF_STA, &cfg);
        esp_wifi_connect();
    }
    return ESP_OK;
}

bool wifi_has_credentials(void)
{
    char ssid[40];
    char pass[72];
    if (!load_credentials(ssid, sizeof(ssid), pass, sizeof(pass))) {
        return false;
    }
    return (ssid[0] != 0 && pass[0] != 0);
}

esp_err_t wifi_clear_credentials(void)
{
    nvs_handle_t h;
    esp_err_t err = nvs_open(WIFI_NVS_NAMESPACE, NVS_READWRITE, &h);
    if (err != ESP_OK) {
        return err;
    }
    nvs_erase_key(h, "ssid");
    nvs_erase_key(h, "pass");
    err = nvs_commit(h);
    nvs_close(h);
    s_ssid[0] = 0;
    ESP_LOGW(TAG, "Zugangsdaten geloescht");
    return err;
}

const char *wifi_ssid(void)
{
    return s_ssid;
}

/* ------------------------------------------------------------------ */
/* Start / Status                                                    */
/* ------------------------------------------------------------------ */
esp_err_t wifi_start(void)
{
    char pass[65] = { 0 };
    char ssid[33] = { 0 };

    s_events = xEventGroupCreate();

    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());
    esp_netif_create_default_wifi_sta();

    wifi_init_config_t init = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&init));

    ESP_ERROR_CHECK(esp_event_handler_instance_register(WIFI_EVENT, ESP_EVENT_ANY_ID,
                                                        &wifi_event_handler, NULL, NULL));
    ESP_ERROR_CHECK(esp_event_handler_instance_register(IP_EVENT, IP_EVENT_STA_GOT_IP,
                                                        &wifi_event_handler, NULL, NULL));

    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
    ESP_ERROR_CHECK(esp_wifi_set_storage(WIFI_STORAGE_RAM));

    if (!load_credentials(ssid, sizeof(ssid), pass, sizeof(pass))) {
        ESP_LOGW(TAG, "Keine WLAN-Daten gespeichert - bitte \"wifi <ssid> <passwort>\" eingeben");
        return ESP_ERR_NOT_FOUND;
    }

    strncpy(s_ssid, ssid, sizeof(s_ssid) - 1);
    ESP_LOGI(TAG, "Verbinde mit \"%s\" ...", s_ssid);

    wifi_config_t cfg;
    sta_config_setzen(&cfg, ssid, pass);
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_STA, &cfg));
    ESP_ERROR_CHECK(esp_wifi_start());
    s_last_try = xTaskGetTickCount();

    return ESP_OK;
}

bool wifi_wait_connected(uint32_t ms)
{
    if (!s_events) {
        return false;
    }
    EventBits_t bits = xEventGroupWaitBits(s_events, BIT_CONNECTED, pdFALSE, pdTRUE,
                                           pdMS_TO_TICKS(ms));
    return (bits & BIT_CONNECTED) != 0;
}

bool wifi_is_connected(void)
{
    return s_connected;
}

void wifi_ensure_connected(void)
{
    if (s_connected || s_ssid[0] == 0) {
        return;
    }
    /* hoechstens alle 30 s ein neuer Versuch */
    TickType_t jetzt = xTaskGetTickCount();
    if ((jetzt - s_last_try) < pdMS_TO_TICKS(30000)) {
        return;
    }
    s_last_try = jetzt;
    ESP_LOGI(TAG, "Neuer Verbindungsversuch");
    esp_wifi_connect();
}

/* Netze in Reichweite auflisten. Zeigt den Sicherheitsmodus, damit ein
 * abweichender Modus (offen, WEP, nur WPA) als Ursache belegt und nicht
 * geraten werden muss. */
void wifi_scan_print(void)
{
    if (!s_events) {
        ESP_LOGW(TAG, "Scan nicht moeglich: WLAN ist nicht gestartet");
        return;
    }
    if (s_connected) {
        ESP_LOGW(TAG, "Hinweis: die bestehende Verbindung kann beim Scan kurz aussetzen");
    }

    wifi_scan_config_t scan = { 0 };
    scan.show_hidden = true;
    esp_err_t err = esp_wifi_scan_start(&scan, true);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "Scan fehlgeschlagen: %s", esp_err_to_name(err));
        return;
    }

    uint16_t anzahl = 20;
    wifi_ap_record_t *liste = calloc(anzahl, sizeof(wifi_ap_record_t));
    if (!liste) {
        ESP_LOGW(TAG, "Kein Speicher fuer die Scan-Liste");
        return;
    }
    err = esp_wifi_scan_get_ap_records(&anzahl, liste);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "Scan-Liste nicht lesbar: %s", esp_err_to_name(err));
        free(liste);
        return;
    }

    ESP_LOGI(TAG, "Scan: %u Netz(e), Schwelle: %s und staerker",
             (unsigned)anzahl, authmode_text(WIFI_SCHWELLE));
    for (uint16_t i = 0; i < anzahl; i++) {
        const wifi_ap_record_t *ap = &liste[i];
        const char *name = (const char *)ap->ssid;
        bool gesucht = (s_ssid[0] != 0 && strcmp(name, s_ssid) == 0);
        bool zu_offen = (ap->authmode == WIFI_AUTH_OPEN || ap->authmode == WIFI_AUTH_WEP);

        ESP_LOGI(TAG, "  %s%-24s %4d dBm  Kanal %2u  " MACSTR "  %s%s",
                 gesucht ? "* " : "  ",
                 name[0] ? name : "(versteckt)",
                 ap->rssi, ap->primary,
                 MAC2STR(ap->bssid),
                 authmode_text(ap->authmode),
                 zu_offen ? "  <- unverschluesselt, wird abgelehnt" : "");
    }

    if (s_ssid[0] == 0) {
        ESP_LOGW(TAG, "Kein Netz konfiguriert - "
                      "\"wifi <ssid> <passwort>\" oder /sdcard/spritpreis.cfg");
    } else {
        bool gefunden = false;
        for (uint16_t i = 0; i < anzahl; i++) {
            if (strcmp((const char *)liste[i].ssid, s_ssid) == 0) {
                gefunden = true;
            }
        }
        if (!gefunden) {
            ESP_LOGW(TAG, "Konfiguriertes Netz \"%s\" war nicht dabei", s_ssid);
        }
    }
    free(liste);
}

/* ------------------------------------------------------------------ */
/* Zeit                                                              */
/* ------------------------------------------------------------------ */
esp_err_t time_start_sntp(void)
{
    setenv("TZ", APP_TIMEZONE, 1);
    tzset();

    esp_sntp_config_t cfg = ESP_NETIF_SNTP_DEFAULT_CONFIG(APP_SNTP_SERVER);
    cfg.start = true;
    cfg.wait_for_sync = false;
    esp_err_t err = esp_netif_sntp_init(&cfg);
    if (err == ESP_ERR_INVALID_STATE) {
        /* Schon initialisiert - das passiert, wenn die Zeit nachtraeglich
         * geholt wird (z. B. WLAN kam erst spaeter). Dann nur erneut
         * starten, ein zweites init ist nicht erlaubt. */
        err = esp_netif_sntp_start();
        if (err == ESP_OK) {
            ESP_LOGI(TAG, "SNTP erneut gestartet (%s)", APP_SNTP_SERVER);
            return ESP_OK;
        }
    }
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "SNTP-Start fehlgeschlagen: %s", esp_err_to_name(err));
        return err;
    }
    ESP_LOGI(TAG, "SNTP gestartet (%s)", APP_SNTP_SERVER);
    return ESP_OK;
}

bool time_is_valid(void)
{
    return time(NULL) > APP_TIME_VALID_FROM;
}
