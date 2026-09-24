/*
 * sd_log.c - Logausgabe zusaetzlich auf die microSD-Karte
 *
 * Aufbau:
 *   - esp_log_set_vprintf() haengt sich an die Logausgabe des Systems. Eine
 *     Kopie jeder Zeile landet in einer Warteschlange.
 *   - Ein eigener Task holt die Zeilen ab und schreibt sie gesammelt in die
 *     Datei. So muss keine Logstelle auf die Karte warten - kartenzugriffe
 *     dauern Millisekunden und wuerden sonst jeden Ablauf bremsen.
 *   - Die Datei wird nur zum Schreiben geoeffnet und danach wieder
 *     geschlossen: die Karte laesst sich jederzeit entnehmen.
 *   - Aus dem Interrupt-Kontext wird nichts mitgeschrieben (die
 *     Warteschlange waere dort nicht sicher). Solche Zeilen sind selten.
 *   - Ist die Warteschlange voll, wird die Zeile verworfen und gezaehlt:
 *     Lieber eine Logzeile verlieren als den Ablauf anhalten.
 *
 * Zusammen mit dem #define in console.c landen auch die Ausgaben der
 * Konsolenbefehle in dieser Datei.
 */

#include <stdio.h>
#include <string.h>
#include <time.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"
#include "esp_log.h"

#include "config.h"
#include "sd_archive.h"
#include "sd_log.h"

static const char *TAG = "sdlog";

#define LOG_ZEILE_LEN    192     /* eine Zeile darf so lang sein */
#define LOG_WARTEN       24      /* so viele Zeilen passen in die Warteschlange */
#define LOG_SAMMELN      8       /* so viele Zeilen werden zusammen geschrieben */
#define LOG_TAKT_MS      2000    /* spaetestens nach dieser Zeit raus schreiben */
#define LOG_TASK_STACK   5120

static QueueHandle_t s_queue = NULL;
static volatile bool s_aktiv = false;
static volatile uint32_t s_verworfen = 0;
static volatile uint32_t s_geschrieben = 0;
static char s_datei[64] = "";

/* Sammelpuffer des Log-Tasks - bewusst NICHT auf dessen Stack: 8 Zeilen a
 * 192 Byte haetten den Stapel zusammen mit den Dateizugriffen gesprengt
 * (Stapelueberlauf des Tasks, am Geraet aufgetreten). */
static char s_sammel[LOG_SAMMELN][LOG_ZEILE_LEN];

/* Dateiname: eine Datei je Monat, damit sie klein bleibt und leicht zu
 * kopieren ist. Ohne gestellte Uhr gibt es kein Datum - dann sammelt eine
 * neutrale Datei die Zeilen, bis die Uhr steht. */
static void make_name(char *dst, size_t len, time_t now)
{
    if (now < APP_TIME_VALID_FROM) {
        snprintf(dst, len, "%s/log-vor-zeitstellung.txt", SD_MOUNT_POINT);
        return;
    }
    struct tm tm;
    localtime_r(&now, &tm);
    snprintf(dst, len, "%s/log-%04d-%02d.txt", SD_MOUNT_POINT,
             tm.tm_year + 1900, tm.tm_mon + 1);
}

/* Gesammelte Zeilen anhaengen. Laeuft im Log-Task, nie im Interrupt. */
static bool block_schreiben(char zeilen[][LOG_ZEILE_LEN], int anzahl)
{
    if (anzahl <= 0) {
        return true;
    }
    time_t now = time(NULL);
    const bool uhr_ok = (now >= APP_TIME_VALID_FROM);

    char name[64];
    make_name(name, sizeof(name), now);
    if (strcmp(name, s_datei) != 0) {
        strncpy(s_datei, name, sizeof(s_datei) - 1);
        s_datei[sizeof(s_datei) - 1] = 0;
    }

    sd_archive_lock();                  /* FAT vertraegt keinen zweiten Zugriff */
    FILE *f = fopen(name, "a");
    if (!f) {
        sd_archive_unlock();
        ESP_LOGW(TAG, "%s laesst sich nicht schreiben - Log nur auf der Konsole", name);
        s_aktiv = false;                /* nicht bei jeder Zeile erneut versuchen */
        return false;
    }
    fseek(f, 0, SEEK_END);
    if (ftell(f) == 0) {
        if (uhr_ok) {
            struct tm tm;
            localtime_r(&now, &tm);
            fprintf(f, "# Spritpreis-Monitor, Log ab %04d-%02d-%02d %02d:%02d\n",
                    tm.tm_year + 1900, tm.tm_mon + 1, tm.tm_mday,
                    tm.tm_hour, tm.tm_min);
        } else {
            fprintf(f, "# Spritpreis-Monitor, Log (Uhr war beim Start noch nicht gestellt)\n");
        }
    }
    for (int i = 0; i < anzahl; i++) {
        fputs(zeilen[i], f);
    }
    bool ok = (fclose(f) == 0);
    sd_archive_unlock();

    if (!ok) {
        s_aktiv = false;
        return false;
    }
    s_geschrieben += (uint32_t)anzahl;
    return true;
}

static void sd_log_task(void *arg)
{
    (void)arg;
    char eine[LOG_ZEILE_LEN];
    int anzahl = 0;
    TickType_t letzter_schreibvorgang = xTaskGetTickCount();

    for (;;) {
        bool bekommen = (xQueueReceive(s_queue, eine, pdMS_TO_TICKS(LOG_TAKT_MS)) == pdTRUE);
        if (bekommen) {
            snprintf(s_sammel[anzahl], LOG_ZEILE_LEN, "%s", eine);
            anzahl++;
        }

        TickType_t jetzt = xTaskGetTickCount();
        bool zeit_um = (jetzt - letzter_schreibvorgang) >= pdMS_TO_TICKS(LOG_TAKT_MS);
        if (anzahl > 0 && (anzahl >= LOG_SAMMELN || zeit_um || !bekommen)) {
            if (s_aktiv) {
                block_schreiben(s_sammel, anzahl);
            }
            anzahl = 0;
            letzter_schreibvorgang = jetzt;
        }
    }
}

/* Eine fertige Zeile in die Warteschlange legen (ohne Warten). */
static void zeile_anfuegen(const char *zeile)
{
    if (!s_aktiv || !s_queue || !zeile || !zeile[0]) {
        return;
    }
    if (xPortInIsrContext()) {
        return;                          /* im Interrupt nicht mitschreiben */
    }
    char kopie[LOG_ZEILE_LEN];
    snprintf(kopie, sizeof(kopie), "%s", zeile);
    if (xQueueSend(s_queue, kopie, 0) != pdTRUE) {
        s_verworfen++;
    }
}

/* Uhrzeit vor die Zeile setzen, damit die Datei auch ohne die Klammerzahl
 * (Millisekunden seit dem Start) lesbar ist. Ohne gestellte Uhr bleibt die
 * Zeile unveraendert.
 *
 * Wichtig: Nur vollstaendige Zeilen bekommen einen Stempel. Einige Teile des
 * Systems (z. B. die WLAN-Bibliothek) schreiben eine Zeile in mehreren
 * Stuecken; mit Stempel auf jedem Stueck stand die Uhrzeit mitten im Text
 * ("wifi:24.09. 17:12:14  mode : sta ..."). */
static void mit_zeit(const char *zeile, char *dst, size_t len)
{
    const size_t l = strlen(zeile);
    const time_t now = time(NULL);
    if (l == 0 || zeile[l - 1] != '\n' || now < APP_TIME_VALID_FROM) {
        snprintf(dst, len, "%s", zeile);
        return;
    }
    struct tm tm;
    localtime_r(&now, &tm);
    snprintf(dst, len, "%02d.%02d. %02d:%02d:%02d  %s",
             tm.tm_mday, tm.tm_mon + 1, tm.tm_hour, tm.tm_min, tm.tm_sec, zeile);
}

/* Hook an der Logausgabe: erst auf die Konsole, dann in die Datei. */
static int log_hook(const char *fmt, va_list args)
{
    va_list kopie;
    va_copy(kopie, args);
    int n = vprintf(fmt, args);          /* unveraendert auf die serielle Schnittstelle */

    if (s_aktiv) {
        char roh[LOG_ZEILE_LEN];
        char zeile[LOG_ZEILE_LEN];
        vsnprintf(roh, sizeof(roh), fmt, kopie);
        mit_zeit(roh, zeile, sizeof(zeile));
        zeile_anfuegen(zeile);
    }
    va_end(kopie);
    return n;
}

int sd_log_printf(const char *fmt, ...)
{
    va_list args;
    va_start(args, fmt);

    va_list kopie;
    va_copy(kopie, args);
    int n = vprintf(fmt, args);          /* Ausgabe auf die Konsole */

    if (s_aktiv) {
        char roh[LOG_ZEILE_LEN];
        char zeile[LOG_ZEILE_LEN];
        vsnprintf(roh, sizeof(roh), fmt, kopie);
        mit_zeit(roh, zeile, sizeof(zeile));
        zeile_anfuegen(zeile);
    }
    va_end(kopie);
    va_end(args);
    return n;
}

bool sd_log_aktiv(void)
{
    return s_aktiv;
}

const char *sd_log_datei(void)
{
    return s_datei;
}

void sd_log_statistik(unsigned *geschrieben, unsigned *verworfen)
{
    if (geschrieben) {
        *geschrieben = (unsigned)s_geschrieben;
    }
    if (verworfen) {
        *verworfen = (unsigned)s_verworfen;
    }
}

esp_err_t sd_log_init(void)
{
    if (!sd_archive_ready()) {
        ESP_LOGW(TAG, "Keine Karte - Log bleibt auf der seriellen Schnittstelle");
        return ESP_ERR_INVALID_STATE;
    }

    s_queue = xQueueCreate(LOG_WARTEN, LOG_ZEILE_LEN);
    if (!s_queue) {
        return ESP_ERR_NO_MEM;
    }
    if (xTaskCreate(sd_log_task, "sdlog", LOG_TASK_STACK, NULL, 3, NULL) != pdPASS) {
        vQueueDelete(s_queue);
        s_queue = NULL;
        return ESP_ERR_NO_MEM;
    }

    s_aktiv = true;
    esp_log_set_vprintf(log_hook);        /* ab jetzt zusaetzlich in die Datei */

    char name[64];
    make_name(name, sizeof(name), time(NULL));
    ESP_LOGI(TAG, "Log wird mitgeschrieben: %s", name);
    return ESP_OK;
}
