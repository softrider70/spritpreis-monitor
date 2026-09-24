/*
 * sim.c - Simulationsmodus (Testbetrieb ohne API-Key)
 *
 * Preismodell (nachgebildet nach der MTS-K-Regel, siehe docs/trend.md):
 *   - Der Tag startet (00:00-12:00 Uhr) auf dem Stand des Vortages.
 *   - Um 12:00 Uhr wird erhoeht (SIM_SPRUNG_MILLI, je Tag leicht anders).
 *   - Danach sinkt der Preis stundenweise (13:00 bis 22:00 Uhr).
 *   - Ab 22:00 Uhr bleibt er stehen, bis der naechste Mittag kommt.
 * Jeder Tag bekommt zusaetzlich ein leicht anderes Preisniveau, damit
 * Verlauf und Pfeile etwas zu rechnen haben.
 *
 * Alles ist deterministisch: die "Zufallszahlen" entstehen aus einem Hash
 * von Tag und Stunde. Gleiche Uhrzeit liefert also immer denselben Preis -
 * so bleibt ein erzeugter Verlauf reproduzierbar.
 *
 * Es wird ausdruecklich KEIN Netz und KEIN API-Key gebraucht.
 */

#include <stdio.h>
#include <string.h>
#include <time.h>

#include "esp_log.h"

#include "config.h"
#include "price_log.h"
#include "sd_archive.h"
#include "settings.h"
#include "sim.h"

static const char *TAG = "sim";

/* Fuell-Wunsch (z. B. aus spritpreis.cfg), wird ausgefuehrt sobald die Uhr steht */
static int s_fill_wunsch = 0;

void sim_request_fill(int tage)
{
    if (tage > 0) {
        s_fill_wunsch = tage;
    }
}

int sim_fill_wunsch(void)
{
    return s_fill_wunsch;
}

void sim_fill_wunsch_erledigt(void)
{
    s_fill_wunsch = 0;
}

/* Kleiner, schneller Hash (xorshift-Mix) - kein Zufallsgenerator, damit
 * die Werte bei gleicher Eingabe immer gleich sind. */
static uint32_t hash32(uint32_t x)
{
    x ^= x >> 16;
    x *= 0x7feb352dU;
    x ^= x >> 15;
    x *= 0x846ca68bU;
    x ^= x >> 16;
    return x;
}

/* Laengs laufende Tagesnummer (aus dem Zeitstempel, UTC - fuer den Seed
 * genuegt das, es muss kein Kalenderdatum sein). */
static int32_t tag_nummer(time_t t)
{
    return (int32_t)(t / 86400);
}

/*
 * Preis fuer einen Kraftstoff zu einem Zeitpunkt.
 *
 * Der maßgebliche "Sprungtag" ist der Tag, an dem die letzte Erhoehung war:
 * vor 12 Uhr also der Vortag. Danach zaehlt, wie viele Senkungsstunden seither
 * vergangen sind.
 */
static int preis_milli(int fuel, time_t t)
{
    struct tm tm;
    localtime_r(&t, &tm);
    const int stunde = tm.tm_hour;
    const int32_t tag = tag_nummer(t);

    /* 12-Uhr-Sprung gehoert vor Mittag zum Vortag */
    const int32_t sprung_tag = (stunde >= 12) ? tag : tag - 1;

    /* Senkungsfortschritt: 0 um 12 Uhr, danach je Stunde einer mehr,
     * ab 22 Uhr nicht weiter (der Preis bleibt ueber Nacht stehen). */
    int senkungen;
    if (stunde <= 12) {
        senkungen = SIM_SENKUNG_STUNDEN;          /* Stand von gestern Abend */
    } else {
        senkungen = stunde - 12;
        if (senkungen > SIM_SENKUNG_STUNDEN) {
            senkungen = SIM_SENKUNG_STUNDEN;
        }
    }

    /* Tageseigene Werte deterministisch aus dem Sprungtag ableiten */
    const int sprung  = SIM_SPRUNG_MILLI  + (int)(hash32((uint32_t)sprung_tag * 2654435761u) % SIM_SPRUNG_VAR_MILLI);
    const int senkung = SIM_SENKUNG_MILLI + (int)(hash32((uint32_t)sprung_tag * 40503u + 7u) % SIM_SENKUNG_VAR_MILLI);
    const int niveau  = (int)(hash32((uint32_t)sprung_tag * 97u + 3u) % SIM_TAGES_VAR_MILLI) - SIM_TAGES_VAR_MILLI / 2;

    int basis;
    switch (fuel) {
    case FUEL_E5:     basis = SIM_BASIS_E5_MILLI;     break;
    case FUEL_DIESEL: basis = SIM_BASIS_DIESEL_MILLI; break;
    default:          basis = SIM_BASIS_E10_MILLI;    break;
    }

    int preis = basis + niveau + sprung - senkung * senkungen;

    /* Untergrenze: nicht unter das Tagesniveau fallen (sonst lauft der
     * Verlauf ueber die Zeit ins Absurde) */
    const int minimum = basis + niveau - SIM_TAGES_VAR_MILLI;
    if (preis < minimum) {
        preis = minimum;
    }
    return preis;
}

bool sim_enabled(void)
{
    return settings_get_sim();
}

void sim_set_enabled(bool on)
{
    settings_set_sim(on);
    ESP_LOGI(TAG, "Simulationsmodus %s", on ? "AN (Testpreise)" : "AUS (echte API)");
}

void sim_fetch_at(time_t t, fuel_prices_t *out)
{
    if (!out) {
        return;
    }
    memset(out, 0, sizeof(*out));
    out->api_ok = true;
    out->is_open = true;
    snprintf(out->status, sizeof(out->status), "sim");
    for (int f = 0; f < FUEL_COUNT; f++) {
        out->price_milli[f] = preis_milli(f, t);
    }
}

void sim_fetch(fuel_prices_t *out)
{
    sim_fetch_at(time(NULL), out);
}

int sim_fill(int tage)
{
    if (tage < 1) {
        tage = 1;
    }
    if (tage > SIM_FILL_MAX_TAGE) {
        tage = SIM_FILL_MAX_TAGE;
    }

    const time_t now = time(NULL);
    if (now < APP_TIME_VALID_FROM) {
        ESP_LOGW(TAG, "Uhr ist nicht gestellt - Verlauf kann nicht gefuellt werden");
        return 0;
    }

    const int32_t stunde_jetzt = (int32_t)(now / 3600);
    int geschrieben = 0;

    /* 1. Aeltere Tage nur mit wenigen Stuetzpunkten: sie landen ausschliesslich
     *    in den Tageswerten (das Stundenfenster reicht 14 Tage zuruck) und
     *    fuellen damit die Tagesansicht. Chronologisch von alt nach neu, sonst
     *    verwirft das Archiv die Zeilen (es schreibt nie rueckwaerts). */
    static const int phasen[SIM_FILL_STUETZPUNKTE] = { 1, 8, 13, 21 };
    for (int tag = SIM_FILL_TAGESWERTE - tage; tag >= 1; tag--) {
        const int32_t tages_start = stunde_jetzt - (int32_t)(tag + tage) * 24;
        for (int p = 0; p < SIM_FILL_STUETZPUNKTE; p++) {
            const time_t ts = (time_t)(tages_start + phasen[p]) * 3600;
            fuel_prices_t px;
            sim_fetch_at(ts, &px);
            price_log_add(ts, &px);
            sd_archive_add(ts, &px);
            geschrieben++;
        }
    }

    /* 2. Die letzten `tage` Tage stuendlich - das ist der Verlauf fuer das
     *    Diagramm. Chronologisch, damit der Verlaufsspeicher jeden Eintrag
     *    genau einmal abarbeitet. */
    const int32_t stunden = (int32_t)tage * 24;
    for (int32_t i = stunden; i >= 0; i--) {
        const time_t ts = (time_t)(stunde_jetzt - i) * 3600;
        fuel_prices_t p;
        sim_fetch_at(ts, &p);
        price_log_add(ts, &p);
        sd_archive_add(ts, &p);
        geschrieben++;
    }

    /* RAM-Fenster aus dem NVS neu aufbauen - so ist der Stand danach
     * garantiert genauso wie nach einem Neustart. */
    price_log_load(now);

    ESP_LOGI(TAG, "%d simulierte Werte erzeugt (%d Tage stuendlich, Tageswerte bis %d Tage)",
             geschrieben, tage, SIM_FILL_TAGESWERTE);
    return geschrieben;
}

void sim_print_status(void)
{
    printf("Simulation   : %s\n", sim_enabled() ? "AN (Testpreise)" : "aus");
    printf("Regel        : 12:00 Uhr +%d mEUR, danach %d Senkungsstunden\n",
           SIM_SPRUNG_MILLI, SIM_SENKUNG_STUNDEN);
    printf("Modellwerte  : E10 %d, E5 %d, Diesel %d mEUR (Basis)\n",
           SIM_BASIS_E10_MILLI, SIM_BASIS_E5_MILLI, SIM_BASIS_DIESEL_MILLI);

    const time_t now = time(NULL);
    if (now >= APP_TIME_VALID_FROM) {
        fuel_prices_t p;
        sim_fetch_at(now, &p);
        printf("Preis jetzt  : E10 %d, E5 %d, Diesel %d mEUR (simuliert)\n",
               p.price_milli[FUEL_E10], p.price_milli[FUEL_E5], p.price_milli[FUEL_DIESEL]);
    } else {
        printf("Preis jetzt  : Uhr nicht gestellt (Konsole: time)\n");
    }
    printf("Auffuellen   : sim fill [1..%d Tage] (zusaetzlich Tageswerte bis %d Tage)\n",
           SIM_FILL_MAX_TAGE, SIM_FILL_TAGESWERTE);
}
