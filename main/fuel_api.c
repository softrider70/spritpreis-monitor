/*
 * fuel_api.c - HTTPS-Abfrage der Tankerkönig-API (MTS-K-Daten)
 *
 * TLS: Die API nutzt ein Let's-Encrypt-Zertifikat; auf dem ESP32 wird das
 * eingebaute Wurzelzertifikat-Bundle verwendet (kein Zertifikat fest
 * einkompiliert, damit ein Zertifikatswechsel nicht die Firmware bricht).
 */

#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <math.h>

#include "esp_http_client.h"
#include "esp_crt_bundle.h"
#include "esp_log.h"

#include "fuel_api.h"

static const char *TAG = "fuel_api";

static const char *const FUEL_LONG[FUEL_COUNT]  = { "Super E5", "Super E10", "Diesel" };
static const char *const FUEL_SHORT[FUEL_COUNT] = { "E5",       "E10",       "Diesel" };

const char *fuel_name(int idx)
{
    return (idx >= 0 && idx < FUEL_COUNT) ? FUEL_LONG[idx] : "?";
}

const char *fuel_short_name(int idx)
{
    return (idx >= 0 && idx < FUEL_COUNT) ? FUEL_SHORT[idx] : "?";
}

/* ------------------------------------------------------------------ */
/* Kleiner JSON-Auswerter                                             */
/*                                                                    */
/* ESP-IDF 6.1 bringt kein cJSON mehr mit, und die Antwort dieser API  */
/* ist klein und fest aufgebaut. Deshalb wird gezielt nach den noetigen */
/* Feldern gesucht, statt einen vollstaendigen JSON-Parser einzubinden. */
/* Die Rohantwort steht im Log (Level DEBUG), damit sich das pruefen     */
/* laesst.                                                              */
/* ------------------------------------------------------------------ */

/* Sucht "key" und liefert den Zeiger auf den Wert (hinter dem Doppelpunkt). */
static const char *json_value(const char *json, const char *ende, const char *key)
{
    char muster[40];
    int n = snprintf(muster, sizeof(muster), "\"%s\"", key);
    if (n <= 0 || n >= (int)sizeof(muster)) {
        return NULL;
    }
    const char *p = json;
    while (p && p < ende) {
        p = strstr(p, muster);
        if (!p || p >= ende) {
            return NULL;
        }
        p += n;
        while (p < ende && (*p == ' ' || *p == '\t' || *p == '\n' || *p == '\r')) {
            p++;
        }
        if (p < ende && *p == ':') {
            p++;
            while (p < ende && (*p == ' ' || *p == '\t' || *p == '\n' || *p == '\r')) {
                p++;
            }
            return (p < ende) ? p : NULL;
        }
        /* Kein Doppelpunkt (Text kommt als Wert vor) -> weitersuchen. */
    }
    return NULL;
}

/* Zahl (EUR) in Milli-Euro umrechnen; false/fehlend -> 0 */
static int json_number_milli(const char *p, const char *ende)
{
    if (!p || p >= ende) {
        return 0;
    }
    if (*p != '-' && (*p < '0' || *p > '9')) {
        return 0;                    /* z. B. false bei geschlossener Tankstelle */
    }
    char *rest = NULL;
    double v = strtod(p, &rest);
    if (rest == p || v <= 0.0) {
        return 0;
    }
    return (int)lround(v * 1000.0);
}

/* String-Wert (z. B. "open") kopieren */
static void json_string(const char *p, const char *ende, char *dst, size_t dst_len)
{
    if (!p || p >= ende || *p != '"' || dst_len == 0) {
        return;
    }
    p++;
    size_t i = 0;
    while (p < ende && *p != '"' && i + 1 < dst_len) {
        dst[i++] = *p++;
    }
    dst[i] = 0;
}

esp_err_t fuel_api_fetch(const char *station_id, const char *api_key, fuel_prices_t *out)
{
    if (!out) {
        return ESP_ERR_INVALID_ARG;
    }
    memset(out, 0, sizeof(*out));
    strncpy(out->status, "?", sizeof(out->status) - 1);

    if (!station_id || station_id[0] == 0 || !api_key || api_key[0] == 0) {
        return ESP_ERR_INVALID_ARG;
    }

    char url[320];
    snprintf(url, sizeof(url), "https://%s%s?ids=%s&apikey=%s",
             FUEL_API_HOST, FUEL_API_PATH_PRICES, station_id, api_key);

    esp_http_client_config_t cfg = {
        .url = url,
        .timeout_ms = FUEL_HTTP_TIMEOUT_MS,
        .crt_bundle_attach = esp_crt_bundle_attach,
        .buffer_size = 1024,
    };

    esp_http_client_handle_t client = esp_http_client_init(&cfg);
    if (!client) {
        ESP_LOGE(TAG, "HTTP-Client konnte nicht angelegt werden");
        return ESP_FAIL;
    }
    esp_http_client_set_header(client, "Accept", "application/json");

    esp_err_t err = esp_http_client_open(client, 0);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "Verbindung fehlgeschlagen: %s", esp_err_to_name(err));
        esp_http_client_cleanup(client);
        return err;
    }

    esp_http_client_fetch_headers(client);
    int http_status = esp_http_client_get_status_code(client);

    char *buf = malloc(FUEL_RESPONSE_MAX);
    if (!buf) {
        esp_http_client_close(client);
        esp_http_client_cleanup(client);
        return ESP_ERR_NO_MEM;
    }
    int len = esp_http_client_read(client, buf, FUEL_RESPONSE_MAX - 1);
    esp_http_client_close(client);
    esp_http_client_cleanup(client);

    if (len <= 0) {
        ESP_LOGW(TAG, "Leere Antwort (HTTP %d)", http_status);
        free(buf);
        return ESP_ERR_INVALID_RESPONSE;
    }
    buf[len] = 0;

    ESP_LOGD(TAG, "Antwort (%d Byte, HTTP %d): %s", len, http_status, buf);

    const char *ende = buf + len;
    const char *ok_wert = json_value(buf, ende, "ok");
    out->api_ok = (ok_wert && strncmp(ok_wert, "true", 4) == 0);

    const char *prices = json_value(buf, ende, "prices");
    const char *station = prices ? json_value(prices, ende, station_id) : NULL;

    if (station) {
        json_string(json_value(station, ende, "status"), ende,
                    out->status, sizeof(out->status));
        out->is_open = (strcmp(out->status, "open") == 0);
        out->price_milli[FUEL_E5]     = json_number_milli(json_value(station, ende, "e5"), ende);
        out->price_milli[FUEL_E10]    = json_number_milli(json_value(station, ende, "e10"), ende);
        out->price_milli[FUEL_DIESEL] = json_number_milli(json_value(station, ende, "diesel"), ende);
    } else {
        char msg[80] = { 0 };
        json_string(json_value(buf, ende, "message"), ende, msg, sizeof(msg));
        if (msg[0]) {
            ESP_LOGW(TAG, "API meldet: %s", msg);
        }
    }
    free(buf);

    ESP_LOGI(TAG, "HTTP %d, status=%s, Diesel=%d mEUR, E5=%d mEUR, E10=%d mEUR",
             http_status, out->status, out->price_milli[FUEL_DIESEL],
             out->price_milli[FUEL_E5], out->price_milli[FUEL_E10]);

    return out->api_ok ? ESP_OK : ESP_FAIL;
}
