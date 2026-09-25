/*
 * ui.c - Anzeige und Bedienung
 *
 * Rohes Zeichnen ueber display.c (kein LVGL). Das Diagramm wird bei jedem
 * Refresh komplett neu gezeichnet - bei 320x240 und einer Aktualisierung
 * alle paar Minuten ist das unkritisch.
 *
 * Pflicht laut Lizenz (CC BY 4.0): Quellenangabe auf der Anzeige.
 */

#include <stdio.h>
#include <string.h>
#include <time.h>

#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "esp_log.h"

#include "config.h"
#include "display.h"
#include "fuel_poll.h"
#include "power.h"
#include "price_log.h"
#include "sd_archive.h"
#include "sim.h"
#include "trend.h"
#include "ui.h"
#include "version.h"

static const char *TAG = "ui";

/* ---------------------------------------------------------------- */
/* Farben (RGB565)                                                   */
/* ---------------------------------------------------------------- */
#define C_BG        0x0861   /* Hintergrund dunkelblau-grau */
#define C_PANEL     0x10A2
#define C_TEXT      0xFFFF
#define C_TEXT_DIM  0xCE79
#define C_ACCENT    0xFD20   /* orange (Prusa-Farbe) */
#define C_GOOD      0x07E0   /* gruen  */
#define C_BAD       0xF800   /* rot    */
#define C_GRID      0x4208
#define C_BAND      0x0320   /* dunkelgruen: Balken von Min nach Max je Stunde/Tag */
#define C_LINE      0x07FF   /* hellblau: letzter Preis */
#define C_BTN       0x2945
#define C_BTN_ACT   0xFBE0
/* ---------------------------------------------------------------- */
/* Zustand                                                           */
/* ---------------------------------------------------------------- */
#define CHART_MAX   LOG_HOURS          /* groesstes Fenster (Stundenwerte) */

/* Zeitfenster: Anzahl Punkte je Stufe und ob stuendlich oder taeglich.
 * Die Stundenwerte reichen 14 Tage weit (LOG_HOURS = 336), die Tageswerte
 * 120 Tage - beide Fenster sind damit vollstaendig darstellbar. */
typedef struct {
    const char *name;
    int         punkte;
    bool        stuendlich;
} fenster_t;

static const fenster_t FENSTER[UI_RANGE_ANZAHL] = {
    { "24h",  24, true  },
    { "3T",   72, true  },
    { "1W",  168, true  },
    { "4W",   28, false },
};

static char      s_station[40] = "Tankstelle";
static int       s_fuel = FUEL_DEFAULT_INDEX;
static ui_range_t s_range = UI_RANGE_24H;
static int       s_offset = 0;          /* 0 = neuestes Fenster */
static int       s_drag_accum = 0;

/* Anstehende Vollbildmeldung (siehe ui_message) */
static char     s_msg[3][48];
static bool     s_msg_da = false;

/* Signatur des zuletzt aufgebauten Bildes. Der Aufbau eines vollen Bildes
 * dauert einige hundert Millisekunden; beim Aufwecken aus dem Stromsparmodus
 * steht das Bild aber noch im Speicher des Panels. Nur wenn sich wirklich
 * etwas geaendert hat, wird neu gezeichnet. */
static uint32_t s_sig = 0;
static bool     s_gezeichnet = false;

/* Schuetzt das Zeichnen: ui_render laeuft sowohl im UI-Task (Core 1) als auch
 * im Poll-Task (Core 0, z. B. beim Wecken wegen einer Preisaenderung). Ohne
 * Sperre koennten sich beide mitten im Bild abwechseln. */
static SemaphoreHandle_t s_ui_lock = NULL;

static log_point_t s_last[CHART_MAX];
static log_point_t s_min[CHART_MAX];
static log_point_t s_max[CHART_MAX];

/* Vorwaertsdeklaration: ui_render_chart steht vor der Definition von draw_chart */
static void draw_chart(void);

/* ---------------------------------------------------------------- */
/* Hilfsfunktionen                                                   */
/* ---------------------------------------------------------------- */
static void fmt_price(char *dst, size_t len, int milli)
{
    if (milli <= 0) {
        snprintf(dst, len, "-,--");
        return;
    }
    snprintf(dst, len, "%d,%03d", milli / 1000, milli % 1000);
}

static void draw_centered(int y, const char *text, int scale, uint16_t fg)
{
    int w = (int)strlen(text) * 6 * scale;
    int x = (TFT_WIDTH - w) / 2;
    if (x < 0) {
        x = 0;
    }
    display_draw_text_scaled(x, y, text, scale, fg, C_BG);
}

/* Schaltflaeche mit Rahmen und Beschriftung */
static void draw_button(int x, int y, int w, int h, const char *label, bool aktiv)
{
    display_draw_filled_rect(x, y, w, h, aktiv ? C_BTN_ACT : C_BTN);
    display_draw_rect(x, y, w, h, C_TEXT_DIM);
    int tw = (int)strlen(label) * 6;
    int tx = x + (w - tw) / 2;
    if (tx < x + 2) {
        tx = x + 2;
    }
    display_draw_text(tx, y + (h - 7) / 2, label,
                      aktiv ? C_BG : C_TEXT, aktiv ? C_BTN_ACT : C_BTN);
}

static int fenster_punkte(void)
{
    return FENSTER[s_range].punkte;
}

static bool fenster_stuendlich(void)
{
    return FENSTER[s_range].stuendlich;
}

const char *ui_range_name(void)
{
    return FENSTER[s_range].name;
}

ui_range_t ui_range(void)
{
    return s_range;
}

/* Eine Stufe weiterschalten (Tipp in die Grafik oder auf ein Fensterfeld) */
static void fenster_weiter(void)
{
    s_range = (ui_range_t)((s_range + 1) % UI_RANGE_ANZAHL);
    s_offset = 0;                     /* neues Fenster beginnt am neuesten Wert */
}

/* Fensterbreite in Stunden; 0 = taegliche Werte (4 Wochen), dafuer gibt es
 * keine Stundenaufloesung. */
static int fenster_stunden(void)
{
    switch (s_range) {
    case UI_RANGE_24H: return 24;
    case UI_RANGE_3T:  return 72;
    case UI_RANGE_1W:  return 168;
    default:           return 0;
    }
}

/* Abstand eines Punktes zu "jetzt" als kurzer Text: "-6h", "-2T", "jetzt" */
static void fmt_abstand(char *dst, size_t len, int stunden)
{
    if (stunden <= 0) {
        snprintf(dst, len, "jetzt");
    } else if (stunden < 24) {
        snprintf(dst, len, "-%dh", stunden);
    } else {
        snprintf(dst, len, "-%dT", (stunden + 12) / 24);
    }
}

/* ---------------------------------------------------------------- */
/* Wertezeile unter dem Preis                                        */
/* ---------------------------------------------------------------- */

/* Wertezeile y=74: links die guenstigste Tagesstunde des SICHTBAREN Fensters,
 * rechts der Achsentitel. Die frueheren Trendpfeile (1T/2T/1W/12U) standen
 * hier - sie zeigten nur an, liessen sich aber nicht bedienen. */
static void draw_info_row(void)
{
    const int y = 76;
    display_draw_filled_rect(0, 74, TFT_WIDTH, 12, C_BG);

    char txt[40];
    int von = 0, bis = 0, anzahl = 0;
    const int stunden = fenster_stunden();

    if (stunden > 0) {
        if (trend_best_hour_im_fenster(s_fuel, stunden, &von, &bis, &anzahl)) {
            snprintf(txt, sizeof(txt), "guenstig %02d-%02dh (%d Std)", von, bis, anzahl);
        } else {
            snprintf(txt, sizeof(txt), "guenstig --");
        }
    } else {
        /* 4 Wochen: dafuer gibt es nur Tageswerte - dann die typische
         * Tagesstunde der letzten 7 Tage zeigen. */
        if (trend_best_hour(s_fuel, &von, &bis, &anzahl)) {
            snprintf(txt, sizeof(txt), "guenstig %02d-%02dh (%d T)", von, bis, anzahl);
        } else {
            snprintf(txt, sizeof(txt), "guenstig --");
        }
    }
    display_draw_text(4, y, txt, C_TEXT, C_BG);

    const char *einheit = "EUR/L";
    display_draw_text(TFT_WIDTH - 4 - (int)strlen(einheit) * 6, y, einheit,
                      C_TEXT_DIM, C_BG);
}

/* Fusszeile: die vier Zeitfenster ueber die volle Breite, das aktive
 * hervorgehoben. Geblaettert wird durch Tippen links/rechts im Diagramm -
 * eigene Pfeilfelder braucht es dafuer nicht. */
static const int RANGE_ROW_Y = 221;
#define RANGE_FELD_H   17
#define RANGE_FELD_X   4

static void draw_range_row(void)
{
    display_draw_filled_rect(0, 220, TFT_WIDTH, 20, C_BG);
    const int breite = (TFT_WIDTH - 2 * RANGE_FELD_X) / UI_RANGE_ANZAHL;
    for (int i = 0; i < UI_RANGE_ANZAHL; i++) {
        draw_button(RANGE_FELD_X + i * breite, RANGE_ROW_Y, breite - 2, RANGE_FELD_H,
                    FENSTER[i].name, (ui_range_t)i == s_range);
    }
}

void ui_render_chart(void)
{
    if (s_ui_lock) xSemaphoreTake(s_ui_lock, portMAX_DELAY);
    if (power_display_on() && !s_msg_da) {
        draw_chart();
    }
    if (s_ui_lock) xSemaphoreGive(s_ui_lock);
}

/* Tiefster und hoechster Wert im sichtbaren Fenster - dieselben Grenzen, die
 * das Diagramm als Balken zeigt. 0 bedeutet "kein Wert vorhanden". */
static void fenster_bereich(int *tief, int *hoch)
{
    *tief = 0;
    *hoch = 0;

    const bool daily = !fenster_stuendlich();
    size_t n_last = price_log_series(daily, s_fuel, LOG_FIELD_LAST, s_last, CHART_MAX);
    if (n_last == 0) {
        return;
    }
    size_t n_min = price_log_series(daily, s_fuel, LOG_FIELD_MIN, s_min, CHART_MAX);
    size_t n_max = price_log_series(daily, s_fuel, LOG_FIELD_MAX, s_max, CHART_MAX);

    const int seite = fenster_punkte();
    int max_offset = (int)n_last - seite;
    if (max_offset < 0) {
        max_offset = 0;
    }
    int offset = (s_offset > max_offset) ? max_offset : s_offset;
    int ende = (int)n_last - offset;
    if (ende <= 0) {
        ende = (int)n_last;
    }
    int start = ende - seite;
    if (start < 0) {
        start = 0;
    }

    for (int i = start; i < ende; i++) {
        int lo = (i < (int)n_min) ? s_min[i].value : s_last[i].value;
        int hi = (i < (int)n_max) ? s_max[i].value : s_last[i].value;
        if (lo > 0 && (*tief == 0 || lo < *tief)) {
            *tief = lo;
        }
        if (hi > *hoch) {
            *hoch = hi;
        }
    }
}

/* ---------------------------------------------------------------- */
/* Diagramm                                                          */
/* ---------------------------------------------------------------- */
static void draw_chart(void)
{
    /* Rechts bleibt Platz fuer die Preiswerte der Gitterlinien - die linke
     * Beschriftung lag frueher im Diagramm und war nicht zuzuordnen. */
    const int x0 = 6, x1 = 276;
    const int y0 = 88, y1 = 204;
    const int breite = x1 - x0;
    const int hoehe = y1 - y0;

    /* Strichstreifen unter der Grundlinie mit loeschen. Die Stundenstriche
     * liegen ausserhalb der Diagrammflaeche (y1 .. y1+2), sonst blieben die
     * Striche des vorigen Fensters stehen, sobald sich die Punktzahl und
     * damit die Lage der Striche aendert - sichtbar beim Wechsel auf 4W,
     * denn dort sind es 28 Tageswerte statt 24 Stundenwerte. */
    display_draw_filled_rect(x0, y1, breite, 4, C_BG);
    display_draw_filled_rect(x0, y0, breite, hoehe, C_PANEL);

    bool daily = !fenster_stuendlich();
    size_t n_last = price_log_series(daily, s_fuel, LOG_FIELD_LAST, s_last, CHART_MAX);
    size_t n_min = price_log_series(daily, s_fuel, LOG_FIELD_MIN, s_min, CHART_MAX);
    size_t n_max = price_log_series(daily, s_fuel, LOG_FIELD_MAX, s_max, CHART_MAX);

    if (n_last == 0) {
        draw_centered(y0 + hoehe / 2 - 4, "noch keine Verlaufsdaten", 1, C_TEXT_DIM);
        display_draw_rect(x0, y0, breite, hoehe, C_GRID);
        return;
    }

    /* sichtbares Fenster bestimmen (offset = Werte vom neuesten Ende) */
    const int seite = fenster_punkte();
    if (s_offset < 0) {
        s_offset = 0;
    }
    int max_offset = (int)n_last - seite;
    if (max_offset < 0) {
        max_offset = 0;
    }
    if (s_offset > max_offset) {
        s_offset = max_offset;
    }

    int ende = (int)n_last - s_offset;      /* exklusiv */
    if (ende <= 0) {
        ende = (int)n_last;
    }
    int start = ende - seite;
    if (start < 0) {
        start = 0;
    }
    int anzahl = ende - start;
    if (anzahl < 1) {
        anzahl = 1;
    }

    /* Wertebereich fuer die Hoehe: Min/Max der sichtbaren Punkte */
    int vmin = 0, vmax = 0;
    for (int i = start; i < ende; i++) {
        int lo = (i < (int)n_min) ? s_min[i].value : s_last[i].value;
        int hi = (i < (int)n_max) ? s_max[i].value : s_last[i].value;
        if (lo > 0 && (vmin == 0 || lo < vmin)) vmin = lo;
        if (hi > vmin && hi > vmax) vmax = hi;
    }
    if (vmin == 0) {
        vmin = s_last[start].value;
    }
    if (vmax <= vmin) {
        vmax = vmin + 10;                    /* 1 Cent Spreizung, sonst flach */
    }
    int spanne = vmax - vmin;
    vmin -= spanne / 10;                     /* etwas Luft oben/unten */
    vmax += spanne / 10;
    spanne = vmax - vmin;

    /* Hilfslinien (Preisraster) */
    for (int g = 0; g <= 4; g++) {
        int y = y0 + (hoehe * g) / 4;
        display_draw_filled_rect(x0, y, breite, 1, C_GRID);
    }

    /* Balken min..max je Punkt + Linie "letzter Preis" */
    int slot = breite / anzahl;
    if (slot < 2) {
        slot = 2;
    }
    int prev_x = -1, prev_y = -1;
    for (int i = 0; i < anzahl; i++) {
        int idx = start + i;
        int xc = x0 + (i * breite) / anzahl;

        int lo = (idx < (int)n_min) ? s_min[idx].value : s_last[idx].value;
        int hi = (idx < (int)n_max) ? s_max[idx].value : s_last[idx].value;
        int v  = s_last[idx].value;

        if (lo > 0 && hi > 0) {
            int ylo = y1 - (int)((int64_t)(lo - vmin) * hoehe / spanne);
            int yhi = y1 - (int)((int64_t)(hi - vmin) * hoehe / spanne);
            if (yhi < y0) yhi = y0;
            if (ylo > y1) ylo = y1;
            if (ylo - yhi >= 1) {
                display_draw_filled_rect(xc, yhi, (slot > 2 ? slot - 2 : 1), ylo - yhi, C_BAND);
            }
        }
        if (v > 0) {
            int yv = y1 - (int)((int64_t)(v - vmin) * hoehe / spanne);
            if (yv < y0) yv = y0;
            if (yv > y1) yv = y1;
            if (prev_x >= 0) {
                display_draw_line(prev_x, prev_y, xc, yv, 2, C_LINE);
            }
            display_draw_filled_rect(xc - 1, yv - 1, 3, 3, C_LINE);
            prev_x = xc;
            prev_y = yv;
        }
    }

    /* Stundenstriche an der Grundlinie: im 24-h-Fenster jeder Wert, in
     * groesseren Fenstern so viele, dass sie noch unterscheidbar bleiben. */
    int schritt = anzahl / 24;
    if (schritt < 1) {
        schritt = 1;
    }
    for (int i = 0; i < anzahl; i += schritt) {
        int xc = x0 + (i * breite) / anzahl;
        display_draw_filled_rect(xc, y1, 1, 3, C_GRID);
    }
    /* Rote Linie bei 12:00 Uhr Ortszeit. Um 12 Uhr darf der Preis erhoeht
     * werden (MTS-K), danach sind nur noch Senkungen erlaubt - die Linie
     * markiert also den Beginn jedes neuen Tagespreises. Bei Tageswerten gibt
     * es keine Uhrzeit, dort entfaellt sie. */
    if (!daily) {
        for (int i = 0; i < anzahl; i++) {
            int idx = start + i;
            time_t t = (time_t)s_last[idx].stamp * 3600;
            struct tm tm;
            localtime_r(&t, &tm);
            if (tm.tm_hour != 12) {
                continue;
            }
            int xc = x0 + (i * breite) / anzahl;
            display_draw_filled_rect(xc, y0 + 1, 1, hoehe - 1, C_BAD);
        }
    }

    /* Achsenbeschriftung: Preiswerte rechts neben den Gitterlinien, darunter
     * der Zeitabstand zum neuesten Wert. Der oberste Wert traegt die Einheit
     * (siehe Wertezeile "EUR/L"). */
    char txt[24];
    for (int g = 0; g <= 4; g++) {
        int y = y0 + (hoehe * g) / 4;
        int wert = vmax - (int)((int64_t)spanne * g / 4);
        fmt_price(txt, sizeof(txt), wert);
        display_draw_text(x1 + 4, y - 3, txt, C_TEXT_DIM, C_BG);
    }

    /* Zeitachse: vier Marken mit Abstand zu jetzt ("-6h", "-2T", "jetzt").
     * Der Abstand kommt aus den Zeitstempeln, nicht aus der Punktnummer -
     * sonst wuerde eine Luecke im Verlauf falsch beschriftet. */
    const int yt = y1 + 4;
    display_draw_filled_rect(0, yt - 1, TFT_WIDTH, 10, C_BG);
    int32_t neuester = s_last[ende - 1].stamp;
    for (int i = 0; i < 4; i++) {
        int idx = start + (i * (anzahl - 1)) / 3;
        int32_t abstand = neuester - s_last[idx].stamp;
        int stunden = daily ? (int)(abstand * 24) : (int)abstand;
        fmt_abstand(txt, sizeof(txt), stunden);

        int xc = x0 + (i * (breite - 1)) / 3;
        int tx = xc - (int)strlen(txt) * 6 / 2;      /* mittig unter der Marke */
        if (tx < 0) {
            tx = 0;
        }
        if (tx > TFT_WIDTH - (int)strlen(txt) * 6) {
            tx = TFT_WIDTH - (int)strlen(txt) * 6;
        }
        display_draw_text(tx, yt, txt, C_TEXT_DIM, C_BG);
        /* Kein eigener Strich fuer die Marke: sie liegt nicht auf dem
         * Stundenraster und wirkte dadurch wie ein Strich an falscher Stelle.
         * Die Stundenstriche oben genuegen als Bezug. */
    }

    display_draw_rect(x0, y0, breite, hoehe, C_GRID);

    /* Ist-Marker: zeigt, wo der zuletzt gemessene Preis liegt. Er sitzt
     * bewusst am rechten Rand neben der Preisachse und nicht auf der Kurve -
     * so bleibt er auch dann sichtbar, wenn das Diagramm geblaettert wurde
     * und der neueste Wert gar nicht im Bild ist. */
    const fuel_prices_t *ist_p = fuel_poll_last();
    const int ist = (ist_p ? ist_p->price_milli[s_fuel] : 0);
    if (ist > 0) {
        int yv = y1 - (int)((int64_t)(ist - vmin) * hoehe / spanne);
        if (yv < y0) yv = y0;
        if (yv > y1) yv = y1;
        display_draw_filled_rect(x1 - 13, yv, 7, 1, C_ACCENT);   /* Zeiger */
        display_draw_filled_rect(x1 - 5, yv - 1, 3, 3, C_ACCENT);  /* Punkt */
    }

    ESP_LOGD(TAG, "Diagramm: %d Punkte, Fenster %d..%d, Bereich %d..%d",
             (int)n_last, start, ende - 1, vmin, vmax);
}

/* ---------------------------------------------------------------- */
/* Oeffentliche API                                                  */
/* ---------------------------------------------------------------- */
void ui_init(void)
{
    if (!s_ui_lock) {
        s_ui_lock = xSemaphoreCreateMutex();
    }
    display_fill(C_BG);
    s_gezeichnet = false;
    ui_message("Spritpreis-Monitor", "startet ...", "");
}

void ui_set_station_label(const char *name)
{
    if (!name) {
        return;
    }
    strncpy(s_station, name, sizeof(s_station) - 1);
    s_station[sizeof(s_station) - 1] = 0;
}

static void ui_message_draw(void);

void ui_message(const char *titel, const char *zeile1, const char *zeile2)
{
    if (s_ui_lock) xSemaphoreTake(s_ui_lock, portMAX_DELAY);

    snprintf(s_msg[0], sizeof(s_msg[0]), "%s", titel ? titel : "");
    snprintf(s_msg[1], sizeof(s_msg[1]), "%s", zeile1 ? zeile1 : "");
    snprintf(s_msg[2], sizeof(s_msg[2]), "%s", zeile2 ? zeile2 : "");
    s_msg_da = true;
    s_gezeichnet = false;         /* das Bild zeigt jetzt die Meldung */

    if (power_display_on()) {
        ui_message_draw();            /* sonst beim Aufwecken nachholen */
    }
    if (s_ui_lock) xSemaphoreGive(s_ui_lock);
}

void ui_message_clear(void)
{
    s_msg_da = false;
    s_gezeichnet = false;         /* Preisansicht muss neu aufgebaut werden */
}

/* Vollbildmeldung zeichnen (Inhalt steht in s_msg) */
static void ui_message_draw(void)
{
    display_fill(C_BG);
    draw_centered(70, s_msg[0], 2, C_ACCENT);
    if (s_msg[1][0]) {
        draw_centered(120, s_msg[1], 1, C_TEXT);
    }
    if (s_msg[2][0]) {
        draw_centered(140, s_msg[2], 1, C_TEXT_DIM);
    }
    /* Quellenangabe: CC BY 4.0 verlangt Urheber, Lizenz und einen Hinweis auf
     * die Lizenz. Die Anzeige hat keinen Browser, deshalb steht die Adresse
     * als Text da. Sie erscheint mit jeder Startmeldung. */
    display_draw_text(6, 220, "Daten: MTS-K / Tankerkoenig", C_TEXT_DIM, C_BG);
    display_draw_text(6, 230, "CC BY 4.0 - creativecommons.tankerkoenig.de",
                      C_TEXT_DIM, C_BG);
}

/* Kennzeichen des Inhalts: aendert sich dieser Wert nicht, ist das Bild auf
 * dem Panel noch aktuell und muss nicht erneut aufgebaut werden.
 *
 * Die Uhrzeit geht nur in 5-Minuten-Schritten ein: Mit der Minute als
 * Kennzeichen wurde beim Aufwecken fast immer neu gezeichnet und das Bild
 * brauchte dadurch ueber 400 ms (am Geraet gemessen: "Anzeige an nach 472 ms").
 * Jetzt reichen die rund 120 ms fuer das Panel selber. */
static uint32_t bild_signatur(const fuel_prices_t *p, bool zeit_ok, bool netz_ok)
{
    uint32_t h = (uint32_t)(p ? p->price_milli[s_fuel] : 0);
    h = h * 31u + (p ? (p->is_open ? 1u : 2u) : 0u);
    h = h * 31u + (zeit_ok ? (uint32_t)(time(NULL) / 300) : 0u);
    h = h * 31u + (netz_ok ? 3u : 4u);
    h = h * 31u + (sim_enabled() ? 5u : 6u);
    h = h * 31u + (uint32_t)s_fuel;
    h = h * 31u + (uint32_t)s_range;
    h = h * 31u + (uint32_t)s_offset;
    /* Der Zeitpunkt der letzten Messung gehoert dazu: sonst bliebe "Messung
     * 22:40" stehen, obwohl laengst neue Werte da sind. Geaendert wird das
     * Bild dadurch nur einmal je Abfrage (alle 5 Minuten). */
    h = h * 31u + (uint32_t)(fuel_poll_data_time() / 60);
    return h;
}

void ui_render(const fuel_prices_t *p, bool zeit_ok, bool netz_ok)
{
    if (s_ui_lock) xSemaphoreTake(s_ui_lock, portMAX_DELAY);

    /* Ist die Anzeige im Stromsparmodus aus, wird nicht gezeichnet - die
     * SPI-Transfers waeren reine Verschwendung. Beim Aufwachen zeichnet
     * power.c ueber den Wake-Callback neu. */
    if (!power_display_on()) {
        if (s_ui_lock) xSemaphoreGive(s_ui_lock);
        return;
    }

    /* Steht eine Meldung an (z. B. "API-Key fehlt"), wird sie gezeigt - auch
     * nach dem Aufwecken aus dem Stromsparmodus. */
    if (s_msg_da) {
        ui_message_draw();
        if (s_ui_lock) xSemaphoreGive(s_ui_lock);
        return;
    }

    /* Nichts geaendert? Dann das vorhandene Bild stehen lassen. */
    const uint32_t sig = bild_signatur(p, zeit_ok, netz_ok);
    if (s_gezeichnet && sig == s_sig) {
        ESP_LOGD(TAG, "Anzeige unveraendert - nicht neu gezeichnet");
        if (s_ui_lock) xSemaphoreGive(s_ui_lock);
        return;
    }
    s_sig = sig;
    s_gezeichnet = true;

    /* Kopfzeile. Links der Name der Tankstelle, mit Abstand daneben die
     * Build-Nummer - damit ist am Geraet sofort zu sehen, welcher Stand
     * laeuft. Ganz rechts steht, wo im Verlauf man sich befindet: "jetzt"
     * oder z. B. "-2h" / "-3T", wenn im Diagramm geblaettert wurde. */
    char station[40];
    snprintf(station, sizeof(station), "%s", s_station);

    char build[12];
    snprintf(build, sizeof(build), "B%d", BUILD_NUMBER);

    char pos[16];
    const int verschub_stunden = s_offset * (fenster_stuendlich() ? 1 : 24);
    fmt_abstand(pos, sizeof(pos), verschub_stunden);
    const int pos_x = TFT_WIDTH - 4 - (int)strlen(pos) * 6;

    /* Der Name darf die Build-Nummer und die Positionsangabe rechts nicht
     * ueberfahren - notfalls wird er gekuerzt (die Build-Nummer muss
     * sichtbar bleiben). */
    int platz = (pos_x - 6 - 4 - 10 - (int)strlen(build) * 6) / 6;
    if (platz < 4) {
        platz = 4;
    }
    if ((int)strlen(station) > platz) {
        station[platz] = 0;
    }

    display_draw_filled_rect(0, 0, TFT_WIDTH, 10, C_BG);
    display_draw_text(pos_x, 2, pos, s_offset > 0 ? C_ACCENT : C_TEXT_DIM, C_BG);
    display_draw_text(4, 2, station, C_TEXT, C_BG);
    display_draw_text(4 + (int)strlen(station) * 6 + 10, 2, build, C_TEXT_DIM, C_BG);

    /* Statuszeile */
    char status[48];
    char zeit[8] = "--:--";
    if (zeit_ok) {
        time_t now = time(NULL);
        struct tm tm;
        localtime_r(&now, &tm);
        strftime(zeit, sizeof(zeit), "%H:%M", &tm);
    }
    snprintf(status, sizeof(status), "%s %s %s  SD:%s", fuel_short_name(s_fuel),
             sim_enabled() ? "SIM"
                           : (p ? (p->is_open ? "offen" : "geschlossen") : "?"),
             sim_enabled() ? "simuliert" : (netz_ok ? "online" : "offline"),
             sd_archive_ready() ? "ok" : "--");
    display_draw_filled_rect(0, 10, TFT_WIDTH - 40, 10, C_BG);
    display_draw_text(4, 12, status, C_TEXT_DIM, C_BG);

    /* Zeitpunkt der letzten Messung. Zusammen mit der Uhr rechts daneben sagt
     * er, wie frisch die Werte sind - gruen, solange die Messung im laufenden
     * Takt (5 min) liegt. */
    const time_t messung = fuel_poll_data_time();
    if (messung > 0 && zeit_ok) {
        struct tm mt;
        localtime_r(&messung, &mt);
        char letzte[24];
        strftime(letzte, sizeof(letzte), "Messung %H:%M", &mt);
        const bool frisch = (time(NULL) - messung) <= 6 * 60;
        display_draw_text(TFT_WIDTH - 38 - (int)strlen(letzte) * 6 - 6, 12,
                          letzte, frisch ? C_GOOD : C_TEXT_DIM, C_BG);
    }

    display_draw_text(TFT_WIDTH - 38, 12, zeit, C_TEXT, C_BG);

    /* Grosser Preis, links daneben das Tief, rechts das Hoch des sichtbaren
     * Zeitfensters - dieselben Grenzen, die das Diagramm zeigt. */
    char preis[12];
    fmt_price(preis, sizeof(preis), p ? p->price_milli[s_fuel] : 0);
    display_draw_filled_rect(0, 24, TFT_WIDTH, 50, C_BG);
    draw_centered(30, preis, 4, p && p->is_open ? C_ACCENT : C_TEXT_DIM);
    draw_centered(62, "EUR je Liter", 1, C_TEXT_DIM);

    int tief = 0, hoch = 0;
    fenster_bereich(&tief, &hoch);
    char wert[16];
    display_draw_text(4, 34, "Tief", C_TEXT_DIM, C_BG);
    fmt_price(wert, sizeof(wert), tief);
    display_draw_text_scaled(4, 46, wert, 2, C_TEXT, C_BG);
    int hx = TFT_WIDTH - 4 - (int)strlen("Hoch") * 6;
    display_draw_text(hx, 34, "Hoch", C_TEXT_DIM, C_BG);
    fmt_price(wert, sizeof(wert), hoch);
    display_draw_text_scaled(TFT_WIDTH - 4 - (int)strlen(wert) * 6 * 2, 46, wert,
                             2, C_TEXT, C_BG);

    /* Diagramm und die Zeilen darunter (setzen ihren Bereich selbst) */
    draw_chart();
    draw_info_row();
    draw_range_row();

    if (s_ui_lock) xSemaphoreGive(s_ui_lock);
}

void ui_touch_tap(int x, int y)
{
    /* Fusszeile: die Felder waehlen das Zeitfenster direkt. Ein Tipp neben die
     * Felder tut nichts. */
    if (y >= 218) {
        const int breite = (TFT_WIDTH - 2 * RANGE_FELD_X) / UI_RANGE_ANZAHL;
        if (x >= RANGE_FELD_X) {
            int i = (x - RANGE_FELD_X) / breite;
            if (i >= 0 && i < UI_RANGE_ANZAHL) {
                s_range = (ui_range_t)i;
                s_offset = 0;
            }
        }
        return;
    }
    if (y < 88 || y > 204) {
        return;                          /* ausserhalb des Diagramms */
    }

    /* Im Diagramm: links blaettert aelter, rechts neuer, in der Mitte wird das
     * Zeitfenster weitergeschaltet. Die frueheren Pfeilfelder in der Fusszeile
     * sind damit ueberfluessig. */
    const int drittel = TFT_WIDTH / 3;
    if (x < drittel) {
        s_offset += fenster_punkte() / 2;          /* aeltere Werte */
    } else if (x > 2 * drittel) {
        s_offset -= fenster_punkte() / 2;          /* neuere Werte */
        if (s_offset < 0) {
            s_offset = 0;
        }
    } else {
        fenster_weiter();
    }
}

bool ui_touch_move(int x, int y, bool pressed)
{
    static int last_x = -1;

    if (!pressed) {
        s_drag_accum = 0;
        last_x = -1;                     /* naechste Beruehrung faengt neu an */
        return false;
    }
    if (last_x < 0) {
        /* Erster Wert dieser Beruehrung ist der Bezugspunkt - ohne ihn wuerde
         * die Strecke seit der letzten Beruehrung als Ziehweg gelten und das
         * Diagramm springen (z. B. direkt nach dem Aufwecken). */
        last_x = x;
        return false;
    }
    if (y < 84 || y > 210) {
        last_x = x;
        return false;
    }
    int dx = x - last_x;
    last_x = x;
        s_drag_accum += dx;

    bool geaendert = false;

    /* Verschieben: finger nach rechts -> aeltere Werte */
    while (s_drag_accum >= 24) {
        s_drag_accum -= 24;
        s_offset++;
        geaendert = true;
    }
    while (s_drag_accum <= -24) {
        s_drag_accum += 24;
        s_offset--;
        geaendert = true;
        if (s_offset < 0) {
            s_offset = 0;
            s_drag_accum = 0;
            geaendert = false;      /* war schon am Anfang, nichts passiert */
        }
    }
    return geaendert;
}

int ui_fuel_index(void)
{
    return s_fuel;
}

void ui_set_fuel(int idx)
{
    if (idx >= 0 && idx < FUEL_COUNT) {
        s_fuel = idx;
    }
}
