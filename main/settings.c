/*
 * settings.c - Tankstellen-ID und API-Key im NVS
 */

#include <string.h>

#include "nvs.h"
#include "esp_log.h"

#include "config.h"
#include "settings.h"

static const char *TAG = "settings";
#define SETTINGS_NS     "spritcfg"
#define KEY_STATION     "station"
#define KEY_API         "apikey"
#define KEY_SIM         "sim"
#define KEY_NAME        "name"
#define KEY_CSVKOMPAKT  "csvfmt"

esp_err_t settings_load(char *station, size_t station_len, char *api_key, size_t key_len)
{
    if (!station || !api_key) {
        return ESP_ERR_INVALID_ARG;
    }
    station[0] = 0;
    api_key[0] = 0;

    nvs_handle_t h;
    if (nvs_open(SETTINGS_NS, NVS_READONLY, &h) != ESP_OK) {
        return ESP_OK;                 /* noch nichts gespeichert */
    }
    size_t len = station_len;
    if (nvs_get_str(h, KEY_STATION, station, &len) != ESP_OK) {
        station[0] = 0;
    }
    len = key_len;
    if (nvs_get_str(h, KEY_API, api_key, &len) != ESP_OK) {
        api_key[0] = 0;
    }
    nvs_close(h);
    return ESP_OK;
}

static esp_err_t store_str(const char *key, const char *value)
{
    nvs_handle_t h;
    esp_err_t err = nvs_open(SETTINGS_NS, NVS_READWRITE, &h);
    if (err != ESP_OK) {
        return err;
    }
    err = nvs_set_str(h, key, value);
    if (err == ESP_OK) {
        err = nvs_commit(h);
    }
    nvs_close(h);
    return err;
}

esp_err_t settings_set_station(const char *station_id)
{
    if (!station_id) {
        return ESP_ERR_INVALID_ARG;
    }
    ESP_LOGI(TAG, "Tankstellen-ID gespeichert: %s", station_id);
    return store_str(KEY_STATION, station_id);
}

esp_err_t settings_set_api_key(const char *api_key)
{
    if (!api_key) {
        return ESP_ERR_INVALID_ARG;
    }
    /* Key nur gekuerzt loggen, nicht vollstaendig */
    ESP_LOGI(TAG, "API-Key gespeichert (%u Zeichen)", (unsigned)strlen(api_key));
    return store_str(KEY_API, api_key);
}

esp_err_t settings_get_name(char *dst, size_t len)
{
    if (!dst || len == 0) {
        return ESP_ERR_INVALID_ARG;
    }
    dst[0] = 0;

    nvs_handle_t h;
    if (nvs_open(SETTINGS_NS, NVS_READONLY, &h) != ESP_OK) {
        return ESP_OK;                 /* noch nichts gespeichert */
    }
    size_t laenge = len;
    if (nvs_get_str(h, KEY_NAME, dst, &laenge) != ESP_OK) {
        dst[0] = 0;
    }
    nvs_close(h);
    return ESP_OK;
}

esp_err_t settings_set_name(const char *name)
{
    if (!name) {
        return ESP_ERR_INVALID_ARG;
    }
    ESP_LOGI(TAG, "Tankstellenname gespeichert: \"%s\"", name);
    return store_str(KEY_NAME, name);
}

bool settings_get_sim(void)
{
    nvs_handle_t h;
    if (nvs_open(SETTINGS_NS, NVS_READONLY, &h) != ESP_OK) {
        return SIM_DEFAULT_ON;
    }
    uint8_t v = SIM_DEFAULT_ON;
    if (nvs_get_u8(h, KEY_SIM, &v) != ESP_OK) {
        v = SIM_DEFAULT_ON;
    }
    nvs_close(h);
    return v != 0;
}

esp_err_t settings_set_sim(bool on)
{
    nvs_handle_t h;
    esp_err_t err = nvs_open(SETTINGS_NS, NVS_READWRITE, &h);
    if (err != ESP_OK) {
        return err;
    }
    err = nvs_set_u8(h, KEY_SIM, on ? 1 : 0);
    if (err == ESP_OK) {
        err = nvs_commit(h);
    }
    nvs_close(h);
    ESP_LOGI(TAG, "Simulation %s", on ? "eingeschaltet" : "ausgeschaltet");
    return err;
}

bool settings_get_csv_kompakt(void)
{
    nvs_handle_t h;
    if (nvs_open(SETTINGS_NS, NVS_READONLY, &h) != ESP_OK) {
        return false;
    }
    uint8_t v = 0;
    if (nvs_get_u8(h, KEY_CSVKOMPAKT, &v) != ESP_OK) {
        v = 0;
    }
    nvs_close(h);
    return v != 0;
}

esp_err_t settings_set_csv_kompakt(bool kompakt)
{
    nvs_handle_t h;
    esp_err_t err = nvs_open(SETTINGS_NS, NVS_READWRITE, &h);
    if (err != ESP_OK) {
        return err;
    }
    err = nvs_set_u8(h, KEY_CSVKOMPAKT, kompakt ? 1 : 0);
    if (err == ESP_OK) {
        err = nvs_commit(h);
    }
    nvs_close(h);
    ESP_LOGI(TAG, "Archivformat: %s", kompakt ? "kompakt (ohne ISO-Zeit)"
                                              : "mit lesbarer Zeit");
    return err;
}
