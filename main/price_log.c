/*
 * price_log.c - Verlaufsspeicher (RAM-Fenster + NVS-Eintraege je Stunde/Tag)
 *
 * Aufbau:
 *   RAM:  s_hours[LOG_HOURS] und s_days[LOG_DAYS], als Ring ueber die
 *         Stunde/Tagesnummer (Slot = Nummer % Anzahl). Ein Slot gilt nur,
 *         wenn sein "stamp" zur aktuellen Nummer passt - so sind Luecken
 *         (Geraet aus) automatisch ungueltig.
 *   NVS:  je Stunde ein Blob unter dem Schluessel "h<nummer>", je Tag "d<nummer>"
 *         (14 Zeichen sind erlaubt, das passt). Beim Stundenwechsel wird der
 *         abgeschlossene Wert geschrieben, zusaetzlich bei jeder Preisaenderung.
 *   Aufraeumen: alte Schluessel werden geloescht, wenn sie aus dem Fenster fallen.
 */

#include <string.h>
#include <stdio.h>

#include "nvs.h"
#include "nvs_flash.h"
#include "esp_log.h"

#include "price_log.h"

static const char *TAG = "price_log";

typedef struct {
    int16_t min_milli;
    int16_t max_milli;
    int16_t last_milli;
} log_value_t;

typedef struct {
    int32_t     stamp;
    uint8_t     valid;
    uint8_t     pad[3];
    log_value_t fuel[FUEL_COUNT];
} bucket_t;

static bucket_t s_hours[LOG_HOURS];
static bucket_t s_days[LOG_DAYS];
static bool s_ready = false;

/* ------------------------------------------------------------------ */
/* Hilfsfunktionen                                                   */
/* ------------------------------------------------------------------ */
static void key_hour(char *dst, size_t len, int32_t stamp)
{
    snprintf(dst, len, "h%ld", (long)stamp);
}

static void key_day(char *dst, size_t len, int32_t stamp)
{
    snprintf(dst, len, "d%ld", (long)stamp);
}

static void bucket_store(const char *key, const bucket_t *b)
{
    nvs_handle_t h;
    if (nvs_open(LOG_NVS_NAMESPACE, NVS_READWRITE, &h) != ESP_OK) {
        return;
    }
    if (nvs_set_blob(h, key, b, sizeof(*b)) == ESP_OK) {
        nvs_commit(h);
    }
    nvs_close(h);
}

static void bucket_delete(const char *key)
{
    nvs_handle_t h;
    if (nvs_open(LOG_NVS_NAMESPACE, NVS_READWRITE, &h) != ESP_OK) {
        return;
    }
    if (nvs_erase_key(h, key) == ESP_OK) {
        nvs_commit(h);
    }
    nvs_close(h);
}

static bool bucket_load(const char *key, bucket_t *out)
{
    nvs_handle_t h;
    if (nvs_open(LOG_NVS_NAMESPACE, NVS_READONLY, &h) != ESP_OK) {
        return false;
    }
    size_t len = sizeof(*out);
    esp_err_t err = nvs_get_blob(h, key, out, &len);
    nvs_close(h);
    return (err == ESP_OK && len == sizeof(*out));
}

/* Wert in einen Eimer einarbeiten; true, wenn sich dadurch etwas aendert. */
static bool bucket_update(bucket_t *b, int32_t stamp, const fuel_prices_t *p)
{
    bool changed = false;

    if (!b->valid || b->stamp != stamp) {
        memset(b, 0, sizeof(*b));
        b->stamp = stamp;
        b->valid = 1;
        changed = true;
    }

    for (int f = 0; f < FUEL_COUNT; f++) {
        int v = p->price_milli[f];
        if (v <= 0) {
            continue;
        }
        log_value_t *lv = &b->fuel[f];
        if (lv->last_milli != (int16_t)v) {
            changed = true;
        }
        if (lv->min_milli == 0 || v < lv->min_milli) {
            lv->min_milli = (int16_t)v;
            changed = true;
        }
        if (lv->max_milli == 0 || v > lv->max_milli) {
            lv->max_milli = (int16_t)v;
            changed = true;
        }
        lv->last_milli = (int16_t)v;
    }
    return changed;
}

/* ------------------------------------------------------------------ */
/* Oeffentliche API                                                  */
/* ------------------------------------------------------------------ */
void price_log_init(void)
{
    memset(s_hours, 0, sizeof(s_hours));
    memset(s_days, 0, sizeof(s_days));
    s_ready = true;
    ESP_LOGI(TAG, "Verlaufsspeicher bereit (%d Stunden-, %d Tageswerte)",
             LOG_HOURS, LOG_DAYS);
}

void price_log_load(time_t now)
{
    if (!s_ready || now < APP_TIME_VALID_FROM) {
        return;
    }
    int32_t hour_now = (int32_t)(now / 3600);
    int32_t day_now = (int32_t)(now / 86400);

    for (int i = 0; i < LOG_HOURS; i++) {
        int32_t stamp = hour_now - i;
        bucket_t *slot = &s_hours[(uint32_t)stamp % LOG_HOURS];
        bucket_t tmp;
        char key[16];
        key_hour(key, sizeof(key), stamp);
        if (bucket_load(key, &tmp) && tmp.stamp == stamp && tmp.valid) {
            *slot = tmp;
        } else {
            memset(slot, 0, sizeof(*slot));
        }
    }

    for (int i = 0; i < LOG_DAYS; i++) {
        int32_t stamp = day_now - i;
        bucket_t *slot = &s_days[(uint32_t)stamp % LOG_DAYS];
        bucket_t tmp;
        char key[16];
        key_day(key, sizeof(key), stamp);
        if (bucket_load(key, &tmp) && tmp.stamp == stamp && tmp.valid) {
            *slot = tmp;
        } else {
            memset(slot, 0, sizeof(*slot));
        }
    }
    ESP_LOGI(TAG, "Verlauf geladen (Fenster %d h / %d d)", LOG_HOURS, LOG_DAYS);
}

void price_log_add(time_t now, const fuel_prices_t *p)
{
    if (!s_ready || !p || now < APP_TIME_VALID_FROM) {
        return;
    }
    int32_t hour_now = (int32_t)(now / 3600);
    int32_t day_now = (int32_t)(now / 86400);

    bucket_t *hb = &s_hours[(uint32_t)hour_now % LOG_HOURS];
    bucket_t *db = &s_days[(uint32_t)day_now % LOG_DAYS];

    /* abgeschlossenen Vorgaenger retten, bevor der Slot ueberschrieben wird */
    if (hb->valid && hb->stamp != hour_now) {
        char key[16];
        key_hour(key, sizeof(key), hb->stamp);
        bucket_store(key, hb);
        key_hour(key, sizeof(key), hb->stamp - LOG_HOURS);
        bucket_delete(key);
    }
    if (db->valid && db->stamp != day_now) {
        char key[16];
        key_day(key, sizeof(key), db->stamp);
        bucket_store(key, db);
        key_day(key, sizeof(key), db->stamp - LOG_DAYS);
        bucket_delete(key);
    }

    bool changed_h = bucket_update(hb, hour_now, p);
    bool changed_d = bucket_update(db, day_now, p);

    if (changed_h) {
        char key[16];
        key_hour(key, sizeof(key), hour_now);
        bucket_store(key, hb);
    }
    if (changed_d) {
        char key[16];
        key_day(key, sizeof(key), day_now);
        bucket_store(key, db);
    }
}

/* Einen Wert in einen Eimer uebernehmen, ohne etwas ins NVS zu schreiben. */
static void bucket_merge(bucket_t *b, int32_t stamp, int fuel,
                         int min_milli, int max_milli, int last_milli)
{
    if (!b->valid || b->stamp != stamp) {
        memset(b, 0, sizeof(*b));
        b->stamp = stamp;
        b->valid = 1;
    }
    log_value_t *lv = &b->fuel[fuel];
    if (min_milli > 0 && (lv->min_milli == 0 || min_milli < lv->min_milli)) {
        lv->min_milli = (int16_t)min_milli;
    }
    if (max_milli > 0 && max_milli > lv->max_milli) {
        lv->max_milli = (int16_t)max_milli;
    }
    if (last_milli > 0) {
        lv->last_milli = (int16_t)last_milli;   /* Zeilen kommen chronologisch */
    }
}

void price_log_set_raw(time_t now, int fuel, int min_milli, int max_milli, int last_milli)
{
    if (!s_ready || fuel < 0 || fuel >= FUEL_COUNT || now < APP_TIME_VALID_FROM) {
        return;
    }
    bucket_merge(&s_hours[(uint32_t)(now / 3600) % LOG_HOURS],
                 (int32_t)(now / 3600), fuel, min_milli, max_milli, last_milli);
    bucket_merge(&s_days[(uint32_t)(now / 86400) % LOG_DAYS],
                 (int32_t)(now / 86400), fuel, min_milli, max_milli, last_milli);
}

size_t price_log_series(bool daily, int fuel, int field, log_point_t *out, size_t max)
{
    if (!s_ready || !out || fuel < 0 || fuel >= FUEL_COUNT || max == 0) {
        return 0;
    }    const int count = daily ? LOG_DAYS : LOG_HOURS;
    const bucket_t *arr = daily ? s_days : s_hours;

    /* Zeitstempel des Fensters: die Ringposition ergibt sich aus dem Modulo,
     * deshalb wird ueber die relative Position gerechnet. */
    int32_t stamp_now;
    if (daily) {
        stamp_now = (int32_t)(time(NULL) / 86400);
    } else {
        stamp_now = (int32_t)(time(NULL) / 3600);
    }

    /* Zuerst von neu nach alt sammeln (so bricht ein voller Puffer die ALTEN
     * Werte ab, nicht die neuen), danach umdrehen -> aeltester Wert zuerst. */
    size_t n = 0;
    for (int i = 0; i < count && n < max; i++) {
        int32_t stamp = stamp_now - i;
        const bucket_t *b = &arr[(uint32_t)stamp % count];
        if (!b->valid || b->stamp != stamp) {
            continue;
        }
        const log_value_t *lv = &b->fuel[fuel];
        int16_t v = (field == LOG_FIELD_MIN) ? lv->min_milli
                  : (field == LOG_FIELD_MAX) ? lv->max_milli
                                             : lv->last_milli;
        if (v == 0) {
            continue;
        }
        out[n].stamp = stamp;
        out[n].value = v;
        n++;
    }

    for (size_t a = 0, b2 = n; a + 1 < b2; a++, b2--) {
        log_point_t tmp = out[a];
        out[a] = out[b2 - 1];
        out[b2 - 1] = tmp;
    }
    return n;
}

void price_log_reset(void)
{
    nvs_handle_t h;
    if (nvs_open(LOG_NVS_NAMESPACE, NVS_READWRITE, &h) != ESP_OK) {
        return;
    }
    nvs_erase_all(h);
    nvs_commit(h);
    nvs_close(h);
    memset(s_hours, 0, sizeof(s_hours));
    memset(s_days, 0, sizeof(s_days));
    ESP_LOGW(TAG, "Verlauf geloescht");
}
