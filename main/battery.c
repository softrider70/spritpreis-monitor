/*
 * battery.c - Zellspannung des 1S-LiIon-Akkus messen
 *
 * Ablauf:
 *   1. Kanal zum konfigurierten Pin bestimmen (adc_oneshot_io_to_channel) und
 *      pruefen, dass es ADC1 ist - ADC2 teilt sich der Funk, bei laufendem
 *      WLAN sind dessen Werte unbrauchbar.
 *   2. ADC mit 12-dB-Daempfung einrichten (Messbereich bis etwa 3,1 V).
 *   3. Kalibrierung aus den eFuse-Werten bauen. Ohne sie liegt der ESP32-ADC
 *      schnell um zehn Prozent daneben, und am oberen Rand wird die Kennlinie
 *      flach - die Genauigkeit kommt also nicht aus dem Teiler, sondern aus
 *      dieser Kalibrierung.
 *   4. Mehrfach messen und mitteln (Rauschen geht mit Wurzel(n) herunter),
 *      dann auf die Zellspannung hochrechnen: U_zelle = U_pin * (R1+R2)/R2.
 *
 * Der letzte Wert wird BATTERY_MESS_TAKT_MS lang zwischengespeichert, damit
 * eine Anzeige-Aktualisierung nicht jedes Mal den ADC bemueht.
 */

#include <stdio.h>
#include <string.h>

#include "esp_log.h"
#include "esp_timer.h"
#include "esp_adc/adc_oneshot.h"
#include "esp_adc/adc_cali.h"
#include "esp_adc/adc_cali_scheme.h"

#include "config.h"
#include "battery.h"

static const char *TAG = "batt";

#define BATTERY_MITTELUNG   16      /* Einzelmessungen je Wert */
#define ADC_MAX_RAW         4095    /* 12 Bit */

/* Standardwert fuer die Daempfung. In ESP-IDF 6.1 heisst die Stufe, die frueher
 * 11 dB hiess, ADC_ATTEN_DB_12. */
#define BATTERY_ATTEN       ADC_ATTEN_DB_12

static adc_oneshot_unit_handle_t s_adc = NULL;
static adc_cali_handle_t         s_cali = NULL;
static adc_channel_t             s_chan = ADC_CHANNEL_0;
static bool   s_ok = false;
static bool   s_cali_ok = false;
static int    s_pin_mv = 0;         /* zuletzt gemessene Spannung am Pin */
static int    s_zelle_mv = 0;       /* daraus hochgerechnete Zellspannung */
static int64_t s_messung_us = 0;    /* wann zuletzt gemessen wurde */
static int  s_warn_mv = BATTERY_WARN_MV;   /* Schwelle fuer die Warnung */
static bool s_schwach = false;             /* Warnung gerade aktiv? */

void battery_init(void)
{
    adc_unit_t unit = ADC_UNIT_1;
    if (adc_oneshot_io_to_channel(BATTERY_ADC_GPIO, &unit, &s_chan) != ESP_OK) {
        ESP_LOGW(TAG, "IO%d ist kein ADC-Eingang - Batteriemessung aus",
                 BATTERY_ADC_GPIO);
        return;
    }
    if (unit != ADC_UNIT_1) {
        ESP_LOGW(TAG, "IO%d gehoert zu ADC2 - der ist vom WLAN belegt, "
                      "Batteriemessung aus", BATTERY_ADC_GPIO);
        return;
    }

    adc_oneshot_unit_init_cfg_t unit_cfg = { .unit_id = unit };
    if (adc_oneshot_new_unit(&unit_cfg, &s_adc) != ESP_OK) {
        ESP_LOGW(TAG, "ADC1 nicht verfuegbar - Batteriemessung aus");
        return;
    }
    adc_oneshot_chan_cfg_t chan_cfg = {
        .atten = BATTERY_ATTEN,
        .bitwidth = ADC_BITWIDTH_DEFAULT,
    };
    if (adc_oneshot_config_channel(s_adc, s_chan, &chan_cfg) != ESP_OK) {
        ESP_LOGW(TAG, "ADC-Kanal %d nicht einstellbar", (int)s_chan);
        return;
    }

    /* Der ESP32 kennt in ESP-IDF nur die Zweipunkt-Kalibrierung (line fitting);
     * Curve Fitting gibt es erst auf neueren Chips. Die Struktur hat deshalb
     * kein Kanal-Feld. default_vref wird nur benutzt, wenn im eFuse keine
     * Kalibrierwerte stehen. */
    adc_cali_line_fitting_config_t cali_cfg = {
        .unit_id = unit,
        .atten = BATTERY_ATTEN,
        .bitwidth = ADC_BITWIDTH_DEFAULT,
        .default_vref = 1100,       /* mV, nur als Rueckfallwert */
    };
    s_cali_ok = (adc_cali_create_scheme_line_fitting(&cali_cfg, &s_cali) == ESP_OK);
    if (!s_cali_ok) {
        ESP_LOGW(TAG, "Keine ADC-Kalibrierung - die Werte werden ungenau");
    }

    s_ok = true;
    ESP_LOGI(TAG, "Batteriemessung: ADC1 Kanal %d an IO%d, Teiler %d/%d kOhm, %s",
             (int)s_chan, BATTERY_ADC_GPIO, BATTERY_TEILER_R1, BATTERY_TEILER_R2,
             s_cali_ok ? "kalibriert" : "ohne Kalibrierung");
}

bool battery_ok(void)
{
    return s_ok;
}

static int roh_zu_millivolt(int raw)
{
    int mv = 0;
    if (s_cali_ok && adc_cali_raw_to_voltage(s_cali, raw, &mv) == ESP_OK) {
        return mv;
    }
    /* Notnagel ohne Kalibrierung: gerade Kennlinie ueber den Messbereich. */
    return raw * 3100 / ADC_MAX_RAW;
}

static void messen(void)
{
    int summe = 0, anzahl = 0;
    for (int i = 0; i < BATTERY_MITTELUNG; i++) {
        int raw = 0;
        if (adc_oneshot_read(s_adc, s_chan, &raw) != ESP_OK) {
            continue;
        }
        summe += roh_zu_millivolt(raw);
        anzahl++;
    }
    if (anzahl == 0) {
        return;
    }
    s_pin_mv = summe / anzahl;
    /* Teiler: U_zelle = U_pin * (R1 + R2) / R2 */
    s_zelle_mv = (int)((int32_t)s_pin_mv * (BATTERY_TEILER_R1 + BATTERY_TEILER_R2)
                       / BATTERY_TEILER_R2);
    /* Feinkorrektur gegen das Multimeter (config.h). Der Teiler selbst ist
     * damit nicht gemeint - der stimmt, wenn R1 und R2 stimmen. */
    s_zelle_mv = (int)((int32_t)s_zelle_mv * BATTERY_KORREKTUR_MILLI / 1000);
    s_messung_us = esp_timer_get_time();
}

static void bei_bedarf_messen(void)
{
    if (!s_ok) {
        return;
    }
    if (s_messung_us == 0 ||
        (esp_timer_get_time() - s_messung_us) > (int64_t)BATTERY_MESS_TAKT_MS * 1000) {
        messen();
    }
}

int battery_millivolt(void)
{
    bei_bedarf_messen();
    return s_zelle_mv;
}

int battery_pin_millivolt(void)
{
    bei_bedarf_messen();
    return s_pin_mv;
}

void battery_text(char *dst, size_t len)
{
    if (!dst || len == 0) {
        return;
    }
    const int mv = battery_millivolt();
    if (mv <= 0) {
        snprintf(dst, len, "--,-- V");
        return;
    }
    /* Auf 10 mV runden, nicht abschneiden: 3749 mV soll "3,75 V" ergeben.
     * Erst runden, dann aufteilen - sonst wird aus 3999 mV "3,100 V". */
    const int mv_ger = ((mv + 5) / 10) * 10;
    snprintf(dst, len, "%d,%02d V", mv_ger / 1000, (mv_ger % 1000) / 10);
}

bool battery_schwach(void)
{
    const int mv = battery_millivolt();
    if (s_warn_mv <= 0 || mv <= 0) {
        s_schwach = false;
        return false;
    }
    if (!s_schwach && mv <= s_warn_mv) {
        s_schwach = true;
        ESP_LOGW(TAG, "Akku schwach: %d mV (Schwelle %d mV) - bitte laden",
                 mv, s_warn_mv);
    } else if (s_schwach && mv >= s_warn_mv + BATTERY_WARN_HYSTERESE_MV) {
        s_schwach = false;
        ESP_LOGI(TAG, "Akku wieder ueber der Warnschwelle: %d mV", mv);
    }
    return s_schwach;
}

int battery_warn_mv(void)
{
    return s_warn_mv;
}

void battery_warn_set(int mv)
{
    if (mv < 0) {
        mv = 0;
    }
    s_warn_mv = mv;
    s_schwach = false;              /* mit der neuen Schwelle neu bewerten */
    ESP_LOGI(TAG, "Warnschwelle auf %d mV gesetzt%s", s_warn_mv,
             (s_warn_mv == 0) ? " (Warnung aus)" : "");
}
