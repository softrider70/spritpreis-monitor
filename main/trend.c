/*
 * trend.c - Trendberechnung fuer die Anzeige (siehe trend.h)
 *
 * Datenquelle ist der Stundenverlauf aus price_log.c (14 Tage). Es wird nur
 * gelesen, nichts geschrieben - die Funktion ist damit auch aus dem
 * Touch-Task (Core 1) aufrufbar.
 *
 * Wichtig wegen der Preiserhoehungs-Regel (nur einmal taeglich um 12:00 Uhr,
 * danach nur Senkungen): Die Vergleichsfenster werden um 24 h zurueck
 * verschoben, damit sie dieselbe Tagesphase treffen wie "jetzt". Sonst
 * vergleicht man nachmittags einen gesenkten Preis mit einem
 * Vormittagsdurchschnitt und der Pfeil zeigt fast immer "billiger".
 */

#include <string.h>
#include <time.h>

#include "config.h"
#include "price_log.h"
#include "trend.h"

static log_point_t s_pts[LOG_HOURS];    /* statisch: Task-Stacks bleiben klein */

static trend_dir_t dir_from_diff(int diff)
{
    if (diff > TREND_FLAT_MILLI) {
        return TREND_UP;
    }
    if (diff < -TREND_FLAT_MILLI) {
        return TREND_DOWN;
    }
    return TREND_FLAT;
}

/* Durchschnitt ueber die Stunden [from, to) bilden und mit now_milli
 * vergleichen. Der Wert gilt nur als gueltig, wenn mindestens
 * TREND_MIN_FILL_PCT Prozent der erwarteten Stunden vorliegen. */
static void avg_window(const log_point_t *pts, size_t n, int32_t from, int32_t to,
                       int now_milli, trend_value_t *out)
{
    memset(out, 0, sizeof(*out));
    const int32_t erwartet = to - from;
    if (erwartet <= 0) {
        return;
    }

    int64_t summe = 0;
    int treffer = 0;
    for (size_t i = 0; i < n; i++) {
        if (pts[i].stamp >= from && pts[i].stamp < to && pts[i].value > 0) {
            summe += pts[i].value;
            treffer++;
        }
    }
    if (treffer == 0 ||
        (int64_t)treffer * 100 < (int64_t)erwartet * TREND_MIN_FILL_PCT) {
        return;                          /* zu wenige Werte -> keine Aussage */
    }

    out->valid = true;
    out->samples = treffer;
    out->ref_milli = (int)(summe / treffer);
    out->diff_milli = now_milli - out->ref_milli;
    out->dir = dir_from_diff(out->diff_milli);
}

/* Stundennummer des letzten 12-Uhr-Zeitpunkts (Ortszeit) vor oder gleich
 * "jetzt". Vor 12 Uhr ist das der gestrige 12-Uhr-Stand. */
static int32_t noon_stamp(int32_t hour_now)
{
    time_t now = (time_t)hour_now * 3600;
    struct tm tm;
    localtime_r(&now, &tm);
    tm.tm_hour = 12;
    tm.tm_min = 0;
    tm.tm_sec = 0;
    tm.tm_isdst = -1;                    /* Sommerzeit selbst bestimmen */
    time_t t12 = mktime(&tm);
    if (t12 <= 0) {
        return 0;
    }
    int32_t stamp = (int32_t)(t12 / 3600);
    if (stamp > hour_now) {
        stamp -= 24;
    }
    return stamp;
}

void trend_compute(int fuel, trend_set_t *out)
{
    if (!out) {
        return;
    }
    memset(out, 0, sizeof(*out));
    if (fuel < 0 || fuel >= FUEL_COUNT) {
        return;
    }

    size_t n = price_log_series(false, fuel, LOG_FIELD_LAST, s_pts, LOG_HOURS);
    if (n == 0) {
        return;
    }

    const log_point_t *neu = &s_pts[n - 1];    /* neuester Wert = aktueller Preis */
    out->now_milli = neu->value;
    out->has_now = (neu->value > 0);
    if (!out->has_now) {
        return;
    }

    const int32_t h = neu->stamp;
    /* Fenster enden 24 h vor jetzt -> gleiche Tagesphase wie jetzt */
    avg_window(s_pts, n, h - 48,  h - 24, out->now_milli, &out->day1);
    avg_window(s_pts, n, h - 72,  h - 24, out->now_milli, &out->day2);
    avg_window(s_pts, n, h - 192, h - 24, out->now_milli, &out->week1);

    /* Letzter 12-Uhr-Stand: erster erfasster Wert ab 12:00 Uhr. Fehlt die
     * Stunde (Geraet war aus), bleibt der naechste Wert gueltig - seit 12 Uhr
     * sind ohnehin nur Senkungen moeglich, der Wert ist also eine
     * Obergrenze. Mehr als ein Tag Abstand gilt als Luecke. */
    const int32_t stamp12 = noon_stamp(h);
    if (stamp12 > 0) {
        for (size_t i = 0; i < n; i++) {
            if (s_pts[i].stamp >= stamp12 && s_pts[i].value > 0) {
                if ((s_pts[i].stamp - stamp12) <= 24) {
                    out->noon.valid = true;
                    out->noon.samples = 1;
                    out->noon.ref_milli = s_pts[i].value;
                    out->noon.diff_milli = out->now_milli - s_pts[i].value;
                    out->noon.dir = dir_from_diff(out->noon.diff_milli);
                }
                break;
            }
        }
    }
}

const char *trend_dir_text(trend_dir_t dir)
{
    switch (dir) {
    case TREND_UP:   return "teurer";
    case TREND_DOWN: return "billiger";
    case TREND_FLAT: return "gleich";
    default:         return "keine Daten";
    }
}

/* Ein Stundenwert, in lokale Zeit zerlegt. */
typedef struct {
    int32_t tag;      /* fortlaufende lokale Tagesnummer */
    int8_t  stunde;   /* 0..23, Ortszeit */
    int16_t wert;     /* Milli-Euro */
} lokal_punkt_t;

static lokal_punkt_t s_lokal[(BEST_HOUR_DAYS + 2) * 24];

/* Stundennummer -> lokale Stunde und lokale Tagesnummer. */
static void lokal_zerlegen(int32_t stamp, int32_t *tag, int *stunde)
{
    struct tm tm;
    time_t t = (time_t)stamp * 3600;
    localtime_r(&t, &tm);
    *stunde = tm.tm_hour;

    /* Mittag desselben lokalen Tages ergibt eine eindeutige, fortlaufende
     * Tagesnummer - unabhaengig von Sommerzeit und Jahreswechsel. */
    tm.tm_hour = 12;
    tm.tm_min = 0;
    tm.tm_sec = 0;
    tm.tm_isdst = -1;
    time_t mittag = mktime(&tm);
    *tag = (int32_t)(mittag / 86400);
}

bool trend_best_hour(int fuel, int *from_hour, int *to_hour, int *tage)
{
    if (from_hour) *from_hour = 0;
    if (to_hour)   *to_hour = 0;
    if (tage)      *tage = 0;
    if (fuel < 0 || fuel >= FUEL_COUNT) {
        return false;
    }

    const size_t max_punkte = sizeof(s_lokal) / sizeof(s_lokal[0]);
    size_t n = price_log_series(false, fuel, LOG_FIELD_LAST, s_pts, max_punkte);
    if (n == 0) {
        return false;
    }

    /* Einmal in lokale Zeit umrechnen - localtime und mktime sind teuer und
     * sollen nicht in der Tagesschleife laufen. */
    for (size_t i = 0; i < n; i++) {
        int stunde;
        int32_t tag;
        lokal_zerlegen(s_pts[i].stamp, &tag, &stunde);
        s_lokal[i].tag = tag;
        s_lokal[i].stunde = (int8_t)stunde;
        s_lokal[i].wert = s_pts[i].value;
    }
    const int32_t tag_heute = s_lokal[n - 1].tag;

    int histogramm[24] = { 0 };
    int ausgewertet = 0;

    for (int d = 1; d <= BEST_HOUR_DAYS; d++) {
        const int32_t ziel = tag_heute - d;   /* laufender Tag zaehlt nicht */
        int min_wert = 0, min_stunde = -1, werte = 0;

        for (size_t i = 0; i < n; i++) {
            if (s_lokal[i].tag != ziel || s_lokal[i].wert <= 0) {
                continue;
            }
            werte++;
            if (min_stunde < 0 || s_lokal[i].wert < min_wert) {
                min_wert = s_lokal[i].wert;
                min_stunde = s_lokal[i].stunde;
            }
        }
        /* Tage mit zu wenigen Werten taugen nicht fuer ein "Tagestief" */
        if (werte < BEST_HOUR_MIN_STUNDEN || min_stunde < 0) {
            continue;
        }
        histogramm[min_stunde]++;
        ausgewertet++;
    }

    if (ausgewertet == 0) {
        return false;
    }

    int beste = 0;
    for (int h = 1; h < 24; h++) {
        if (histogramm[h] > histogramm[beste]) {
            beste = h;               /* bei Gleichstand bleibt die fruehere Stunde */
        }
    }

    if (from_hour) *from_hour = beste;
    if (to_hour)   *to_hour = (beste + 1) % 24;
    if (tage)      *tage = ausgewertet;
    return true;
}

/* Guenstigste Tagesstunde im sichtbaren Zeitfenster (siehe trend.h).
 * Anders als trend_best_hour() wird nicht die Stunde des Tagestiefs gesucht,
 * sondern der niedrigste Durchschnitt je Tagesstunde ueber die Stundenwerte
 * des Fensters - die Aussage gilt damit genau fuer das, was im Diagramm steht. */
bool trend_best_hour_im_fenster(int fuel, int stunden, int *from_hour, int *to_hour,
                                int *werte)
{
    if (from_hour) *from_hour = 0;
    if (to_hour)   *to_hour = 0;
    if (werte)     *werte = 0;
    if (fuel < 0 || fuel >= FUEL_COUNT || stunden <= 0) {
        return false;
    }

    size_t n = price_log_series(false, fuel, LOG_FIELD_LAST, s_pts, LOG_HOURS);
    if (n == 0) {
        return false;
    }
    const int32_t jetzt = s_pts[n - 1].stamp;

    int64_t summe[24] = { 0 };
    int     anzahl[24] = { 0 };
    int     gesamt = 0;

    for (size_t i = 0; i < n; i++) {
        if (s_pts[i].value <= 0) {
            continue;
        }
        const int32_t abstand = jetzt - s_pts[i].stamp;
        if (abstand < 0 || abstand >= stunden) {
            continue;                    /* ausserhalb des Fensters */
        }
        int stunde = 0;
        int32_t tag = 0;
        lokal_zerlegen(s_pts[i].stamp, &tag, &stunde);
        summe[stunde] += s_pts[i].value;
        anzahl[stunde]++;
        gesamt++;
    }

    if (gesamt < BEST_HOUR_MIN_STUNDEN) {
        return false;                    /* zu wenig Werte fuer eine Aussage */
    }

    int beste = -1;
    int64_t bester_wert = 0;
    for (int h = 0; h < 24; h++) {
        if (anzahl[h] == 0) {
            continue;
        }
        const int64_t mittel = summe[h] / anzahl[h];
        if (beste < 0 || mittel < bester_wert) {
            beste = h;
            bester_wert = mittel;
        }
    }
    if (beste < 0) {
        return false;
    }

    if (from_hour) *from_hour = beste;
    if (to_hour)   *to_hour = (beste + 1) % 24;
    if (werte)     *werte = gesamt;
    return true;
}
