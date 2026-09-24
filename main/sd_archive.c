/*
 * sd_archive.c - Langzeitarchiv der Preise auf der microSD-Karte
 *
 * Hardware: Beim CYD haengt die Karte am VSPI-Bus.
 *   IO5 = CS, IO18 = SCK, IO19 = MISO, IO23 = MOSI
 * (Pin-Angabe aus der CYD-Doku, witnessmenow/ESP32-Cheap-Yellow-Display,
 *  PINS.md, Abschnitt "SD Card".)
 *
 * Diesen Bus bekommt die Karte allein: Der Touch, der im Projekt
 * urspruenglich ebenfalls auf SPI3 (VSPI) lief, wird jetzt per
 * Software-Bitbang getaktet (touch.c). Zwei Pin-Saetze gleichzeitig auf
 * einem Hardware-SPI-Bus sind nicht moeglich.
 *
 * Format der Datei: siehe sd_archive.h
 */

#include <stdio.h>
#include <string.h>
#include <strings.h>
#include <ctype.h>
#include <dirent.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <time.h>

#include "driver/spi_master.h"
#include "driver/sdspi_host.h"
#include "esp_vfs_fat.h"
#include "sdmmc_cmd.h"
#include "esp_log.h"

#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"

#include "config.h"
#include "price_log.h"
#include "sd_archive.h"
#include "settings.h"

static const char *TAG = "archiv";

static sdmmc_card_t *s_card = NULL;
static bool  s_mounted = false;
static SemaphoreHandle_t s_lock = NULL;   /* schuetzt alle Kartenzugriffe */
static char  s_name[64] = "";          /* aktuelle Monatsdatei */
static int32_t s_last_price[FUEL_COUNT] = { 0 };
static int32_t s_hour_written = -1;    /* Stundennummer der letzten Zeile */
static int32_t s_day_written = -1;     /* Tagesnummer der letzten Zeile */
static uint32_t s_lines = 0;           /* Zeilen in dieser Sitzung */
static int32_t  s_letzter_epoch = 0;   /* juengster Zeitstempel im Archiv */

/* Dateiname: eine Datei je Monat, damit sie klein und leicht zu kopieren
 * bleibt (rund 300 Byte pro Tag, also unter 10 kB im Monat). */
static void make_name(char *dst, size_t len, time_t now)
{
    struct tm tm;
    localtime_r(&now, &tm);
    snprintf(dst, len, "%s/spritpreis-%04d-%02d.csv", SD_MOUNT_POINT,
             tm.tm_year + 1900, tm.tm_mon + 1);
}

/* Ist die Zeile im kompakten Format? Nur dann besteht das erste Feld
 * ausschliesslich aus Ziffern ("1790268401;").
 *
 * Achtung: NICHT das erste Zeichen pruefen - das lesbare Format beginnt mit
 * dem Jahr und damit ebenfalls mit einer Ziffer. Genau dieser Fehler hat am
 * Geraet dazu gefuehrt, dass die vorhandene Datei nicht mehr gelesen wurde
 * ("Archiv enthaelt noch keine Werte"). */
static bool zeile_ist_kompakt(const char *zeile)
{
    int i = 0;
    while (i < 12 && zeile[i] && zeile[i] != ';') {
        if (!isdigit((unsigned char)zeile[i])) {
            return false;
        }
        i++;
    }
    return (i > 0 && i <= 11 && zeile[i] == ';');
}

/* Eine Datenzeile schreiben. Zwei Formate:
 *   lesbar : 2026-09-24T18:46:41;1790268401;2309;2249;2409;open
 *   kompakt: 1790268401;2309;2249;2409;open
 * Die lesbare Form hat die Zeit doppelt (ISO und epoch) und ist damit rund
 * 40 Prozent groesser. Beim Lesen werden beide erkannt. */
static int schreibe_zeile(FILE *f, time_t now, const fuel_prices_t *p)
{
    if (settings_get_csv_kompakt()) {
        return fprintf(f, "%ld;%d;%d;%d;%s\n", (long)now,
                       p->price_milli[FUEL_E5], p->price_milli[FUEL_E10],
                       p->price_milli[FUEL_DIESEL],
                       p->status[0] ? p->status : "?");
    }
    char iso[24];
    struct tm tm;
    localtime_r(&now, &tm);
    strftime(iso, sizeof(iso), "%Y-%m-%dT%H:%M:%S", &tm);
    return fprintf(f, "%s;%ld;%d;%d;%d;%s\n", iso, (long)now,
                   p->price_milli[FUEL_E5], p->price_milli[FUEL_E10],
                   p->price_milli[FUEL_DIESEL],
                   p->status[0] ? p->status : "?");
}

/* Zeile anhaengen; schreibt die Kopfzeile, wenn die Datei neu ist.
 * Aufruf nur mit gehaltener Sperre (sd_archive_lock). */
static bool append_line_ungesperrt(time_t now, const fuel_prices_t *p)
{
    char name[64];    make_name(name, sizeof(name), now);
    if (strcmp(name, s_name) != 0) {
        strncpy(s_name, name, sizeof(s_name) - 1);
        s_name[sizeof(s_name) - 1] = 0;
        ESP_LOGI(TAG, "Archivdatei: %s", s_name);
    }

    FILE *f = fopen(s_name, "a");
    if (!f) {
        ESP_LOGW(TAG, "%s laesst sich nicht oeffnen", s_name);
        return false;
    }

    /* Position ans Dateiende, um zu erkennen, ob die Kopfzeile fehlt */
    fseek(f, 0, SEEK_END);
    long size = ftell(f);
    if (size == 0) {
        fputs(settings_get_csv_kompakt()
                  ? "# epoch;e5_milli;e10_milli;diesel_milli;status\n"
                  : "# zeit_iso;epoch;e5_milli;e10_milli;diesel_milli;status\n", f);
    }

    const int n = schreibe_zeile(f, now, p);
    bool ok = (n > 0) && (fclose(f) == 0);
    if (!ok) {
        ESP_LOGW(TAG, "Schreiben fehlgeschlagen (Karte entfernt?)");
        s_mounted = false;                 /* ab jetzt nur noch im NVS */
        return false;
    }
    s_lines++;
    return true;
}

/* Huelle mit Sperre: Der Aufrufer muss sich nicht darum kuemmern. */
static bool append_line(time_t now, const fuel_prices_t *p)
{
    sd_archive_lock();
    bool ok = append_line_ungesperrt(now, p);
    sd_archive_unlock();
    return ok;
}

void sd_archive_lock(void)
{
    if (s_lock) {
        xSemaphoreTake(s_lock, portMAX_DELAY);
    }
}

void sd_archive_unlock(void)
{
    if (s_lock) {
        xSemaphoreGive(s_lock);
    }
}

const char *sd_archive_dateiname(void)
{
    static char name[64];
    const time_t now = time(NULL);
    if (now < APP_TIME_VALID_FROM) {
        name[0] = 0;                     /* ohne Uhr gibt es keinen Monatsnamen */
        return name;
    }
    make_name(name, sizeof(name), now);
    return name;
}

esp_err_t sd_archive_init(void)
{
    if (!s_lock) {
        s_lock = xSemaphoreCreateMutex();     /* zuerst: append_line nutzt sie */
    }
    if (s_mounted) {
        return ESP_OK;
    }

    spi_bus_config_t bus = {
        .mosi_io_num = SD_MOSI,
        .miso_io_num = SD_MISO,
        .sclk_io_num = SD_SCK,
        .quadwp_io_num = -1,
        .quadhd_io_num = -1,
        .max_transfer_sz = 4096,
    };
    esp_err_t err = spi_bus_initialize(SD_SPI_HOST, &bus, SPI_DMA_CH_AUTO);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "SPI-Bus fuer die Karte nicht verfuegbar (%s)",
                 esp_err_to_name(err));
        return err;
    }

    sdmmc_host_t host = SDSPI_HOST_DEFAULT();
    host.slot = SD_SPI_HOST;

    sdspi_device_config_t slot = SDSPI_DEVICE_CONFIG_DEFAULT();
    slot.host_id = SD_SPI_HOST;
    slot.gpio_cs = SD_CS;

    esp_vfs_fat_mount_config_t mnt = VFS_FAT_MOUNT_DEFAULT_CONFIG();
    mnt.max_files = SD_MAX_FILES;
    mnt.format_if_mount_failed = false;    /* niemals ungefragt formatieren */

    err = esp_vfs_fat_sdspi_mount(SD_MOUNT_POINT, &host, &slot, &mnt, &s_card);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "Keine SD-Karte gemountet (%s) - Archiv bleibt leer",
                 esp_err_to_name(err));
        return err;
    }

    s_mounted = true;
    uint64_t total = 0, free_bytes = 0;
    if (esp_vfs_fat_info(SD_MOUNT_POINT, &total, &free_bytes) == ESP_OK) {
        ESP_LOGI(TAG, "SD-Karte bereit: %.1f GB gesamt, %.1f GB frei",
                 (double)total / 1e9, (double)free_bytes / 1e9);
    } else {
        ESP_LOGI(TAG, "SD-Karte bereit");
    }
    return ESP_OK;
}

bool sd_archive_ready(void)
{
    return s_mounted;
}

void sd_archive_add(time_t now, const fuel_prices_t *p)
{
    if (!s_mounted || !p || now < APP_TIME_VALID_FROM) {
        return;
    }

    /* Nie rueckwaerts schreiben: ein zweites "sim fill" oder ein doppelter
     * Aufruf wuerde sonst dieselben Zeilen noch einmal anhaengen. */
    static time_t letzter_eintrag = 0;
    if (now <= letzter_eintrag) {
        return;
    }

    int32_t hour = (int32_t)(now / 3600);
    int32_t day  = (int32_t)(now / 86400);

    /* Schreibt, wenn sich ein Preis geaendert hat, eine Stunde oder ein Tag
     * abgeschlossen ist. Innerhalb einer Stunde ohne Preisaenderung also
     * nichts - das schont die Karte. */
    bool changed = (s_hour_written < 0) || (day != s_day_written);
    for (int f = 0; f < FUEL_COUNT && !changed; f++) {
        if (p->price_milli[f] != s_last_price[f]) {
            changed = true;
        }
    }
    if (!changed) {
        return;
    }

    if (append_line(now, p)) {
        for (int f = 0; f < FUEL_COUNT; f++) {
            s_last_price[f] = p->price_milli[f];
        }
        s_hour_written = hour;
        s_day_written = day;
        letzter_eintrag = now;
    }
}

void sd_archive_print_status(void)
{
    if (!s_mounted) {
        printf("SD-Karte     : nicht gemountet (Karte steckt? Konsole: sd mount)\n");
        return;
    }
    uint64_t total = 0, free_bytes = 0;
    if (esp_vfs_fat_info(SD_MOUNT_POINT, &total, &free_bytes) == ESP_OK) {
        printf("SD-Karte     : %s, %.1f GB gesamt, %.1f GB frei\n",
               SD_MOUNT_POINT, (double)total / 1e9, (double)free_bytes / 1e9);
    } else {
        printf("SD-Karte     : %s (Groesse nicht lesbar)\n", SD_MOUNT_POINT);
    }

    time_t now = time(NULL);
    if (now >= APP_TIME_VALID_FROM) {
        char name[64];
        make_name(name, sizeof(name), now);
        struct stat st;
        if (stat(name, &st) == 0) {
            printf("Archivdatei  : %s (%ld Byte)\n", name, (long)st.st_size);
        } else {
            printf("Archivdatei  : %s (noch nicht angelegt)\n", name);
        }
    }
    printf("Zeilen       : %u in dieser Sitzung\n", (unsigned)s_lines);
}

/* Beim Einlesen wird sortiert: die Datei ist NICHT zwangslaeufig
 * chronologisch. `sim fill` hat rueckwirkende Testzeilen hinten angehaengt,
 * ein von Hand bearbeiteter oder zusammengefuehrter Bestand kann ebenfalls
 * gemischt sein. Ohne Sortierung gewaenne innerhalb einer Stunde die zuletzt
 * GELESENE Zeile statt der juengsten - der eingezeichnete Preis waere falsch.
 * 1000 Zeilen reichen fuer das groesste Fenster deutlich (4 Wochen sind rund
 * 420 Zeilen). */
#define LESEN_MAX 1000

typedef struct {
    int32_t epoch;
    int16_t e5;
    int16_t e10;
    int16_t diesel;
    int16_t pad;
} lese_zeile_t;

static lese_zeile_t s_lesen[LESEN_MAX];

static int lese_vergleich(const void *a, const void *b)
{
    const lese_zeile_t *x = (const lese_zeile_t *)a;
    const lese_zeile_t *y = (const lese_zeile_t *)b;
    if (x->epoch < y->epoch) return -1;
    if (x->epoch > y->epoch) return 1;
    return 0;
}

/* Eine Monatsdatei einlesen und die Werte in den Verlauf uebernehmen.
 * Rueckgabe: Anzahl der uebernommenen Zeilen. */
static int datei_lesen(const char *name)
{
    sd_archive_lock();
    FILE *f = fopen(name, "r");
    if (!f) {
        sd_archive_unlock();
        return 0;
    }
    char zeile[192];
    int gesammelt = 0;
    bool ueberlauf = false;

    while (fgets(zeile, sizeof(zeile), f)) {
        if (zeile[0] == '#' || zeile[0] == '\n' || zeile[0] == '\r') {
            continue;
        }
        long epoch = 0;
        int e5 = 0, e10 = 0, diesel = 0;
        /* Zwei Formate: kompakt beginnt mit der Zahl (epoch), lesbar mit dem
         * Datum. Erkannt wird am ersten Feld - eindeutig und ohne
         * Verwechslung. */
        int treffer;
        if (zeile_ist_kompakt(zeile)) {
            treffer = sscanf(zeile, "%ld;%d;%d;%d", &epoch, &e5, &e10, &diesel);
        } else {
            treffer = sscanf(zeile, "%*[^;];%ld;%d;%d;%d", &epoch, &e5, &e10, &diesel);
        }
        if (treffer != 4 || epoch <= 0) {
            continue;
        }
        if (epoch > s_letzter_epoch) {
            s_letzter_epoch = (int32_t)epoch;
        }
        if (gesammelt >= LESEN_MAX) {
            ueberlauf = true;
            continue;                    /* nur die ersten 1000 Zeilen */
        }
        s_lesen[gesammelt].epoch = (int32_t)epoch;
        s_lesen[gesammelt].e5 = (int16_t)e5;
        s_lesen[gesammelt].e10 = (int16_t)e10;
        s_lesen[gesammelt].diesel = (int16_t)diesel;
        gesammelt++;
    }
    fclose(f);
    sd_archive_unlock();

    if (gesammelt == 0) {
        return 0;
    }
    if (ueberlauf) {
        ESP_LOGW(TAG, "%s: mehr als %d Zeilen - der Rest wird nicht gelesen",
                 name, LESEN_MAX);
    }

    qsort(s_lesen, (size_t)gesammelt, sizeof(s_lesen[0]), lese_vergleich);

    for (int i = 0; i < gesammelt; i++) {
        /* Eine Archivzeile gilt, bis die naechste kommt - Minimum und Maximum
         * sind also der Wert selbst, mehrere Zeilen derselben Stunde werden
         * in price_log_set_raw() zusammengefasst. Durch die Sortierung ist
         * der zuletzt uebernommene Wert der juengste. */
        const time_t t = (time_t)s_lesen[i].epoch;
        price_log_set_raw(t, FUEL_E5, s_lesen[i].e5, s_lesen[i].e5, s_lesen[i].e5);
        price_log_set_raw(t, FUEL_E10, s_lesen[i].e10, s_lesen[i].e10, s_lesen[i].e10);
        price_log_set_raw(t, FUEL_DIESEL, s_lesen[i].diesel, s_lesen[i].diesel,
                          s_lesen[i].diesel);
    }
    return gesammelt;
}

/* Alle Archivdateien im Mountpunkt sammeln, chronologisch sortiert.
 * Rueckgabe: Anzahl (maximal max). */
static int dateien_sammeln(char namen[][64], int max)
{
    int anzahl = 0;
    DIR *d = opendir(SD_MOUNT_POINT);
    if (!d) {
        return 0;
    }
    struct dirent *eintrag;
    while ((eintrag = readdir(d)) != NULL && anzahl < max) {
        if (strncmp(eintrag->d_name, "spritpreis-", 11) != 0) {
            continue;
        }
        size_t l = strlen(eintrag->d_name);
        if (l < 4 || strcasecmp(eintrag->d_name + l - 4, ".csv") != 0) {
            continue;
        }
        /* Laenge begrenzen: d_name kann bis zu 255 Zeichen haben, der Puffer
         * hier ist kleiner (sonst warnt der Compiler zu Recht). */
        snprintf(namen[anzahl], 64, "%s/%.48s", SD_MOUNT_POINT, eintrag->d_name);
        anzahl++;
    }
    closedir(d);

    /* Namen sortieren: spritpreis-JJJJ-MM.csv ist damit chronologisch. */
    for (int i = 1; i < anzahl; i++) {
        char tmp[64];
        snprintf(tmp, sizeof(tmp), "%s", namen[i]);
        int j = i - 1;
        while (j >= 0 && strcmp(namen[j], tmp) > 0) {
            snprintf(namen[j + 1], 64, "%s", namen[j]);
            j--;
        }
        snprintf(namen[j + 1], 64, "%s", tmp);
    }
    return anzahl;
}

void sd_archive_laden(void)
{
    if (!s_mounted) {
        ESP_LOGW(TAG, "Archiv nicht verfuegbar - Verlauf bleibt wie im NVS");
        return;
    }

    /* Nach einem Stromausfall ist der Arbeitsspeicher leer; der NVS-Verlauf
     * endet dort, wo zuletzt geschrieben wurde. Die Karte kennt dagegen jede
     * Preisaenderung.
     *
     * Die Dateien werden ueber das Verzeichnis gesucht und NICHT aus der Uhr
     * abgeleitet: Bei einem Neustart ohne Netz steht die Uhr noch nicht, und
     * dann berechnet das Geraet Monatsnamen von 1969/1970 und findet nichts
     * (genau das ist am Geraet aufgetreten, obwohl die Datei 6 kB enthielt).
     * Einlesen lohnt nur die drei juengsten Dateien - weiter zurueck reicht
     * das groesste Fenster (4 Wochen) ohnehin nicht. */
    char namen[12][64];
    const int anzahl = dateien_sammeln(namen, 12);

    const int von = (anzahl > 3) ? anzahl - 3 : 0;
    int werte = 0;
    for (int i = von; i < anzahl; i++) {
        int n = datei_lesen(namen[i]);
        if (n > 0) {
            werte += n;
            ESP_LOGI(TAG, "%s: %d Zeilen uebernommen", namen[i], n);
        }
    }

    if (werte > 0) {
        ESP_LOGI(TAG, "Verlauf aus dem Archiv ergaenzt (%d Zeilen)", werte);
    } else {
        ESP_LOGI(TAG, "Archiv enthaelt noch keine Werte (%d Datei(en) gefunden)", anzahl);
    }

    /* Ohne Netz gibt es keine Uhr - und ohne Uhr laesst sich der Verlauf nicht
     * darstellen (die Grafik rechnet mit "jetzt"). Der juengste Zeitstempel
     * im Archiv ist die beste Naeherung: damit ist die Kurve sofort wieder da,
     * und SNTP stellt die Uhr spaeter genau. */
    if (time(NULL) < APP_TIME_VALID_FROM && s_letzter_epoch > APP_TIME_VALID_FROM) {
        struct timeval tv = { .tv_sec = s_letzter_epoch, .tv_usec = 0 };
        if (settimeofday(&tv, NULL) == 0) {
            ESP_LOGW(TAG, "Uhr ohne Netz aus dem Archiv vorgestellt "
                          "(SNTP korrigiert sie, sobald WLAN da ist)");
        }
    }
}

/* Eine Datei neu schreiben: sim-Zeilen weglassen und das Format aus den
 * Einstellungen verwenden.
 *
 * Geschrieben wird zuerst in eine Nebendatei; erst wenn die vollstaendig ist,
 * wird die alte ersetzt. Bei einem Abbruch (Strom weg, Karte gezogen) bleibt
 * damit die bisherige Datei erhalten - ein halb geschriebenes Archiv waere
 * schlimmer als ein zu grosses.
 *
 * Rueckgabe: Anzahl behaltener Zeilen, -1 bei Fehler. */
static int datei_aufraeumen(const char *name, bool sim_entfernen, int *entfernt)
{
    char tmp[96];
    snprintf(tmp, sizeof(tmp), "%.88s.tmp", name);
    if (entfernt) {
        *entfernt = 0;
    }

    sd_archive_lock();
    FILE *ein = fopen(name, "r");
    if (!ein) {
        sd_archive_unlock();
        return -1;
    }
    FILE *aus = fopen(tmp, "w");
    if (!aus) {
        fclose(ein);
        sd_archive_unlock();
        return -1;
    }

    fputs(settings_get_csv_kompakt()
              ? "# epoch;e5_milli;e10_milli;diesel_milli;status\n"
              : "# zeit_iso;epoch;e5_milli;e10_milli;diesel_milli;status\n", aus);

    char zeile[192];
    int behalten = 0;
    int weg = 0;

    while (fgets(zeile, sizeof(zeile), ein)) {
        if (zeile[0] == '#' || zeile[0] == '\n' || zeile[0] == '\r') {
            continue;
        }
        long epoch = 0;
        int e5 = 0, e10 = 0, diesel = 0;
        int treffer;
        if (zeile_ist_kompakt(zeile)) {
            treffer = sscanf(zeile, "%ld;%d;%d;%d", &epoch, &e5, &e10, &diesel);
        } else {
            treffer = sscanf(zeile, "%*[^;];%ld;%d;%d;%d", &epoch, &e5, &e10, &diesel);
        }
        if (treffer != 4 || epoch <= 0) {
            continue;                    /* unlesbare Zeile faellt weg */
        }

        /* Status steht im letzten Feld */
        fuel_prices_t p = { 0 };
        p.price_milli[FUEL_E5] = e5;
        p.price_milli[FUEL_E10] = e10;
        p.price_milli[FUEL_DIESEL] = diesel;
        snprintf(p.status, sizeof(p.status), "%s", "?");
        char *letzte = strrchr(zeile, ';');
        if (letzte) {
            char *s = letzte + 1;
            size_t l = strlen(s);
            while (l > 0 && (s[l - 1] == '\n' || s[l - 1] == '\r')) {
                s[--l] = 0;
            }
            snprintf(p.status, sizeof(p.status), "%s", s);
        }

        if (sim_entfernen && strcmp(p.status, "sim") == 0) {
            weg++;
            continue;
        }

        schreibe_zeile(aus, (time_t)epoch, &p);
        behalten++;
    }
    fclose(ein);

    bool ok = (fclose(aus) == 0);
    if (!ok) {
        remove(tmp);
        sd_archive_unlock();
        ESP_LOGW(TAG, "%s konnte nicht neu geschrieben werden", name);
        return -1;
    }

    remove(name);                        /* erst jetzt die alte Fassung weg */
    if (rename(tmp, name) != 0) {
        ESP_LOGW(TAG, "%s liess sich nicht ersetzen", name);
        sd_archive_unlock();
        return -1;
    }
    sd_archive_unlock();

    ESP_LOGI(TAG, "%s: %d Zeilen behalten, %d sim-Zeilen entfernt", name,
             behalten, weg);
    if (entfernt) {
        *entfernt = weg;
    }
    return behalten;
}

int sd_archive_aufraeumen(bool sim_entfernen, int *zeilen_gesamt, int *sim_entfernt)
{
    if (zeilen_gesamt) *zeilen_gesamt = 0;
    if (sim_entfernt)  *sim_entfernt = 0;
    if (!s_mounted) {
        return -1;
    }

    char namen[12][64];
    const int anzahl = dateien_sammeln(namen, 12);
    int gesamt = 0;
    int weg_gesamt = 0;

    for (int i = 0; i < anzahl; i++) {
        int weg = 0;
        int behalten = datei_aufraeumen(namen[i], sim_entfernen, &weg);
        if (behalten < 0) {
            continue;
        }
        gesamt += behalten;
        weg_gesamt += weg;
    }

    if (zeilen_gesamt) *zeilen_gesamt = gesamt;
    if (sim_entfernt)  *sim_entfernt = weg_gesamt;
    return gesamt;
}

/* Groesse der aktuellen Monatsdatei in Byte (0, wenn es sie nicht gibt). */
unsigned sd_archive_dateigroesse(void)
{
    const char *name = sd_archive_dateiname();
    if (!name[0]) {
        return 0;
    }
    struct stat st;
    if (stat(name, &st) != 0) {
        return 0;
    }
    return (unsigned)st.st_size;
}

/* Zeilen und sim-Zeilen der aktuellen Monatsdatei zaehlen. */
int sd_archive_zaehlen(int *zeilen, int *sim_zeilen)
{
    if (zeilen)     *zeilen = 0;
    if (sim_zeilen) *sim_zeilen = 0;
    if (!s_mounted) {
        return -1;
    }
    const char *name = sd_archive_dateiname();
    if (!name[0]) {
        return -1;
    }

    sd_archive_lock();
    FILE *f = fopen(name, "r");
    if (!f) {
        sd_archive_unlock();
        return -1;
    }
    char zeile[192];
    int n = 0, sim = 0;
    while (fgets(zeile, sizeof(zeile), f)) {
        if (zeile[0] == '#' || zeile[0] == '\n' || zeile[0] == '\r') {
            continue;
        }
        n++;
        char *letzte = strrchr(zeile, ';');
        if (letzte && strncmp(letzte + 1, "sim", 3) == 0) {
            sim++;
        }
    }
    fclose(f);
    sd_archive_unlock();

    if (zeilen)     *zeilen = n;
    if (sim_zeilen) *sim_zeilen = sim;
    return n;
}
