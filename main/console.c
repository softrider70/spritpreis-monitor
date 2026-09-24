/*
 * console.c - Kommando-Konsole auf dem Standard-UART
 *
 * Befehle:
 *   wifi <ssid> <passwort>   WLAN-Zugangsdaten speichern und verbinden
 *   wifidel                  WLAN-Zugangsdaten loeschen
 *   key <apikey>             Tankerkönig-API-Key speichern
 *   station <uuid>           Tankstellen-ID speichern
 *   name <Text>              Anzeigename fuer die Kopfzeile
 *   fuel <diesel|e5|e10>     angezeigten Kraftstoff waehlen
 *   poll                     sofort abfragen
 *   status                   Zustand ausgeben
 *   time                     Uhrzeit/Zeitgueltigkeit
 *   log reset                Verlaufsspeicher loeschen
 *   sd [mount]               SD-Archiv: Zustand anzeigen / Karte neu mounten
 *   file <log|csv>           Datei von der Karte auf die Konsole ausgeben
 *                            (PC-Skript: tools/hole_dateien.ps1)
 *   csv [simweg] [kompakt|lesbar]  Archiv aufraeumen / Format umstellen
 *   trend [diesel|e5|e10]    Trendwerte der Pfeile ausgeben
 *   sim [on|off|fill [tage]] Simulationsmodus mit erfundenen Preisen
 *   cfg [import|template]    Zugangsdaten aus /sdcard/spritpreis.cfg
 *   touch calib on|off       Touch-Rohwerte ins Log (Nachmessen)
 *   power [on|off]           Stromsparmodus: Zustand / Anzeige hart schalten
 *   tasks                    Task-Liste mit Stack-Rest (Diagnose)
 *
 * Hinweis: Passwoerter und API-Key werden nur hier im Terminal eingegeben,
 * nie ueber einen Chat. Sie landen ausschliesslich im NVS des Geraets.
 */

#include <stdio.h>
#include <string.h>
#include <strings.h>
#include <stdlib.h>
#include <time.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "esp_console.h"
#include "esp_log.h"
#include "esp_system.h"
#include "esp_heap_caps.h"

#include "config.h"
#include "console.h"
#include "fuel_poll.h"
#include "power.h"
#include "price_log.h"
#include "sd_archive.h"
#include "sd_config.h"
#include "sd_log.h"
#include "settings.h"
#include "sim.h"
#include "touch.h"
#include "trend.h"
#include "ui.h"
#include "version.h"
#include "wifi.h"

static const char *TAG = "console";

/* Alles, was die Konsole ausgibt, soll auch im Logfile auf der Karte stehen.
 * Statt jeden der rund 40 Aufrufe anzufassen, laeuft printf hier durch eine
 * eigene Funktion. Das #define steht bewusst nach allen Includes und wirkt
 * nur in dieser Datei; sd_log_printf() selbst benutzt vprintf. */
#define printf(...)  sd_log_printf(__VA_ARGS__)

/* ------------------------------------------------------------------ */
static int cmd_wifi(int argc, char **argv)
{
    if (argc != 3) {
        printf("Aufruf: wifi <ssid> <passwort>\n");
        return 1;
    }
    if (wifi_set_credentials(argv[1], argv[2]) != ESP_OK) {
        printf("Speichern fehlgeschlagen\n");
        return 1;
    }
    printf("Zugangsdaten gespeichert, verbinde mit \"%s\" ...\n", argv[1]);
    if (wifi_wait_connected(15000)) {
        printf("verbunden\n");
        time_start_sntp();
    } else {
        printf("keine Verbindung (Zugangsdaten pruefen)\n");
    }
    return 0;
}

static int cmd_wifidel(int argc, char **argv)
{
    (void)argc; (void)argv;
    wifi_clear_credentials();
    printf("Zugangsdaten geloescht\n");
    return 0;
}

static int cmd_wifiscan(int argc, char **argv)
{
    (void)argc; (void)argv;
    printf("Suche Netze in Reichweite ...\n");
    wifi_scan_print();
    return 0;
}

static int cmd_key(int argc, char **argv)
{
    if (argc != 2) {
        printf("Aufruf: key <apikey>\n");
        return 1;
    }
    settings_set_api_key(argv[1]);
    printf("API-Key gespeichert\n");
    fuel_poll_now();
    return 0;
}

static int cmd_station(int argc, char **argv)
{
    if (argc != 2) {
        printf("Aufruf: station <uuid>\n");
        return 1;
    }
    settings_set_station(argv[1]);
    printf("Tankstellen-ID gespeichert\n");
    fuel_poll_now();
    return 0;
}

/* Anzeigename der Tankstelle. Der Name darf Leerzeichen enthalten, deshalb
 * werden alle Woerter aneinandergehaengt ("name Star Musterstadt 1"). */
static int cmd_name(int argc, char **argv)
{
    if (argc < 2) {
        char name[SETTINGS_NAME_MAX];
        settings_get_name(name, sizeof(name));
        printf("Anzeigename  : %s\n", name[0] ? name : "(keiner - es steht nur \"Tankstelle\")");
        printf("Aufruf       : name <Text>\n");
        return 0;
    }

    char name[SETTINGS_NAME_MAX];
    name[0] = 0;
    for (int i = 1; i < argc; i++) {
        if (i > 1) {
            strncat(name, " ", sizeof(name) - strlen(name) - 1);
        }
        strncat(name, argv[i], sizeof(name) - strlen(name) - 1);
    }
    if (settings_set_name(name) != ESP_OK) {
        printf("Speichern fehlgeschlagen\n");
        return 1;
    }
    printf("Anzeigename gespeichert: %s\n", name);
    fuel_poll_now();                  /* Kopfzeile sofort auffrischen */
    return 0;
}

static int cmd_fuel(int argc, char **argv)
{
    if (argc != 2) {
        printf("Aufruf: fuel <diesel|e5|e10>\n");
        return 1;
    }
    int idx = -1;
    if (strcasecmp(argv[1], "diesel") == 0) idx = FUEL_DIESEL;
    else if (strcasecmp(argv[1], "e5") == 0) idx = FUEL_E5;
    else if (strcasecmp(argv[1], "e10") == 0) idx = FUEL_E10;
    if (idx < 0) {
        printf("unbekannt: %s\n", argv[1]);
        return 1;
    }
    ui_set_fuel(idx);
    ui_render(fuel_poll_last(), time_is_valid(), fuel_poll_net_ok());
    printf("Anzeige: %s\n", fuel_name(idx));
    return 0;
}

static int cmd_poll(int argc, char **argv)
{
    (void)argc; (void)argv;
    fuel_poll_now();
    printf("Abfrage angefordert\n");
    return 0;
}

static int cmd_status(int argc, char **argv)
{
    (void)argc; (void)argv;
    char station[SETTINGS_STATION_MAX];
    char api_key[SETTINGS_KEY_MAX];
    settings_load(station, sizeof(station), api_key, sizeof(api_key));

    printf("Projekt      : %s %s (Build %d)\n", BOARD_NAME, APP_VERSION_STRING, BUILD_NUMBER);
    printf("Tankstelle   : %s\n", station[0] ? station : "(nicht gesetzt)");
    {
        char anzeigename[SETTINGS_NAME_MAX];
        settings_get_name(anzeigename, sizeof(anzeigename));
        printf("Anzeigename  : %s\n", anzeigename[0] ? anzeigename : "(keiner)");
    }
    printf("API-Key      : %s\n", api_key[0] ? "(gesetzt)" : "(nicht gesetzt)");
    printf("WLAN         : %s, verbunden=%s\n",
           wifi_ssid()[0] ? wifi_ssid() : "(keine Daten)",
           wifi_is_connected() ? "ja" : "nein");
    printf("Uhr          : %s\n", time_is_valid() ? "gestellt" : "NICHT gestellt");

    const fuel_prices_t *p = fuel_poll_last();
    printf("Preise       : E5 %d mEUR, E10 %d mEUR, Diesel %d mEUR (status=%s)\n",
           p->price_milli[FUEL_E5], p->price_milli[FUEL_E10],
           p->price_milli[FUEL_DIESEL], p->status);
    printf("Letzte Abfrage: %s\n", fuel_poll_net_ok() ? "ok" : "fehlgeschlagen");
    printf("Anzeige      : %s, Zeitfenster %s\n",
           fuel_name(ui_fuel_index()), ui_range_name());
    printf("SD-Archiv    : %s\n", sd_archive_ready() ? "gemountet" : "nicht gemountet");
    printf("Logdatei     : %s\n", sd_log_aktiv() ? sd_log_datei() : "keine (Karte fehlt)");
    printf("Simulation   : %s\n", sim_enabled() ? "AN (Testpreise)" : "aus");
    printf("Anzeige      : %s, Licht-Sleep %s\n",
           power_display_on() ? "an" : "aus",
           power_light_sleep_ready() ? "aktiv" : "aus");
    printf("Intervall    : %d s\n", FUEL_POLL_INTERVAL_S);
    printf("Freier Heap  : %u Byte\n", (unsigned)esp_get_free_heap_size());
    return 0;
}

static int cmd_sd(int argc, char **argv)
{
    if (argc >= 2 && strcasecmp(argv[1], "mount") == 0) {
        if (sd_archive_init() == ESP_OK) {
            printf("SD-Karte gemountet\n");
            return 0;
        }
        printf("Mount fehlgeschlagen - steckt eine FAT-formatierte Karte?\n");
        return 1;
    }
    sd_archive_print_status();
    unsigned geschrieben = 0, verworfen = 0;
    sd_log_statistik(&geschrieben, &verworfen);
    printf("Logdatei     : %s\n", sd_log_aktiv() ? sd_log_datei() : "keine (Karte fehlt)");
    printf("Logzeilen    : %u geschrieben, %u verworfen\n", geschrieben, verworfen);
    return 0;
}

static int cmd_trend(int argc, char **argv)
{
    if (argc >= 2) {
        int idx = -1;
        if (strcasecmp(argv[1], "diesel") == 0) idx = FUEL_DIESEL;
        else if (strcasecmp(argv[1], "e5") == 0) idx = FUEL_E5;
        else if (strcasecmp(argv[1], "e10") == 0) idx = FUEL_E10;
        if (idx < 0) {
            printf("unbekannt: %s (moeglich: diesel, e5, e10)\n", argv[1]);
            return 1;
        }
        ui_set_fuel(idx);
    }

    int f = ui_fuel_index();
    trend_set_t ts;
    trend_compute(f, &ts);

    printf("Kraftstoff   : %s\n", fuel_name(f));
    if (!ts.has_now) {
        printf("Kein Preis im Verlauf - erst abfragen (Befehl: poll)\n");
        return 0;
    }
    printf("Preis jetzt  : %d mEUR\n", ts.now_milli);

    const struct { const char *name; const trend_value_t *v; } zeilen[4] = {
        { "1 Tag  ", &ts.day1 },
        { "2 Tage ", &ts.day2 },
        { "1 Woche", &ts.week1 },
        { "12 Uhr ", &ts.noon },
    };
    for (int i = 0; i < 4; i++) {
        const trend_value_t *v = zeilen[i].v;
        if (!v->valid) {
            printf("%s: keine Daten (noch zu wenig Verlauf)\n", zeilen[i].name);
            continue;
        }
        printf("%s: Vergleich %d mEUR, Abweichung %+d mEUR -> %s (%d Werte)\n",
               zeilen[i].name, v->ref_milli, v->diff_milli,
               trend_dir_text(v->dir), v->samples);
    }
    printf("Hinweis      : Erhoehungen sind nur um 12:00 Uhr moeglich,\n");
    printf("               danach wird nur gesenkt (MTS-K).\n");

    int von = 0, bis = 0, tage = 0;
    if (trend_best_hour(f, &von, &bis, &tage)) {
        printf("Guenstigste Stunde: %02d-%02d Uhr (aus %d Tagen)\n", von, bis, tage);
    } else {
        printf("Guenstigste Stunde: noch keine Daten\n");
    }
    return 0;
}

static int cmd_sim(int argc, char **argv)
{
    if (argc >= 2) {
        if (strcasecmp(argv[1], "on") == 0) {
            sim_set_enabled(true);
            printf("Simulation AN - es werden Testpreise verwendet\n");
            fuel_poll_now();
            return 0;
        }
        if (strcasecmp(argv[1], "off") == 0) {
            sim_set_enabled(false);
            printf("Simulation AUS - es gilt wieder die echte API\n");
            fuel_poll_now();
            return 0;
        }
        if (strcasecmp(argv[1], "fill") == 0) {
            int tage = (argc >= 3) ? atoi(argv[2]) : SIM_FILL_TAGE;
            int n = sim_fill(tage);
            if (n > 0) {
                printf("%d Stundenwerte erzeugt (%d Tage) - Verlauf und SD-Archiv gefuellt\n",
                       n, tage);
                printf("Anzeige: trend   zeigt die neuen Werte\n");
            } else {
                printf("Fuellen nicht moeglich - ist die Uhr gestellt? (time)\n");
            }
            return 0;
        }
        printf("Aufruf: sim [on|off|fill [tage]]\n");
        return 1;
    }
    sim_print_status();
    return 0;
}

static int cmd_touch(int argc, char **argv)
{
    if (argc >= 2 && strcasecmp(argv[1], "calib") == 0) {
        bool on = (argc < 3) || (strcasecmp(argv[2], "on") == 0);
        touch_set_calib_mode(on);
        printf("Kalibrier-Modus %s - Rohwerte stehen im Log (KALIB: ...)\n",
               on ? "EIN" : "AUS");
        return 0;
    }
    printf("Aufruf: touch calib on|off\n");
    return 1;
}

static int cmd_cfg(int argc, char **argv)
{
    if (argc >= 2) {
        if (strcasecmp(argv[1], "import") == 0) {
            int n = sd_config_import();
            if (n > 0) {
                printf("%d Wert(e) uebernommen, Datei zurueckgesetzt\n", n);
                /* Sind WLAN-Daten dabei, gleich verbinden und die Uhr stellen */
                if (wifi_has_credentials()) {
                    printf("verbinde mit \"%s\" ...\n", wifi_ssid());
                    if (wifi_wait_connected(15000)) {
                        printf("verbunden\n");
                        time_start_sntp();
                    } else {
                        printf("keine Verbindung (Zugangsdaten pruefen)\n");
                    }
                }
                fuel_poll_now();
            } else {
                printf("Nichts uebernommen (nur Platzhalter oder Werte schon im NVS)\n");
            }
            return 0;
        }
        if (strcasecmp(argv[1], "template") == 0) {
            sd_config_write_template();
            printf("Musterdatei geschrieben: %s\n", SD_CONFIG_PATH);
            return 0;
        }
        printf("Aufruf: cfg [import|template]\n");
        return 1;
    }
    sd_config_print_status();
    return 0;
}

static int cmd_power(int argc, char **argv)
{
    if (argc >= 2) {
        if (strcasecmp(argv[1], "on") == 0) {
            power_force(true);
            printf("Anzeige an\n");
            return 0;
        }
        if (strcasecmp(argv[1], "off") == 0) {
            power_force(false);
            printf("Anzeige aus (bleibt aus, bis Touch/Taste/Preisaenderung)\n");
            return 0;
        }
        printf("Aufruf: power [on|off]\n");
        return 1;
    }
    power_print_status();
    return 0;
}

static int cmd_time(int argc, char **argv)
{
    (void)argc; (void)argv;
    time_t now = time(NULL);
    struct tm tm;
    localtime_r(&now, &tm);
    char buf[32];
    strftime(buf, sizeof(buf), "%d.%m.%Y %H:%M:%S", &tm);
    printf("Systemzeit: %ld (%s), gueltig=%s\n", (long)now, buf,
           time_is_valid() ? "ja" : "nein");
    return 0;
}

static int cmd_log(int argc, char **argv)
{
    if (argc >= 2 && strcasecmp(argv[1], "reset") == 0) {
        price_log_reset();
        printf("Verlauf geloescht\n");
        return 0;
    }
    printf("Aufruf: log reset\n");
    return 1;
}

/* Datei von der Karte auf die serielle Schnittstelle ausgeben - damit lassen
 * sich Archiv und Logfile ohne Kartenleser am PC sichern (tools/hole_dateien.ps1).
 *
 * Wichtig: hier wird bewusst fprintf(stdout, ...) benutzt und NICHT printf.
 * printf laeuft in dieser Datei ueber sd_log_printf und wuerde jede
 * ausgegebene Zeile erneut ins Logfile schreiben - die Datei fuellte sich
 * damit selbst.
 *
 * Die Sperre bleibt waehrend der ganzen Ausgabe gehalten: Bei 115200 Baud
 * dauert das je nach Dateigroesse ein paar Sekunden, in denen das Archiv
 * nicht schreiben kann. Das ist vertretbar - der Abfragetakt liegt bei
 * fuenf Minuten. */
#define FILE_PUFFER 200

static int cmd_file(int argc, char **argv)
{
    const char *log_name = sd_log_aktiv() ? sd_log_datei() : "";
    const char *csv_name = sd_archive_dateiname();

    if (argc < 2) {
        printf("Aufruf: file <log|csv>\n");
        printf("  log -> %s\n", log_name[0] ? log_name : "(keine Karte)");
        printf("  csv -> %s\n", csv_name[0] ? csv_name : "(keine Karte)");
        return 0;
    }

    const char *name = NULL;
    if (strcasecmp(argv[1], "csv") == 0) {
        name = csv_name;
    } else if (strcasecmp(argv[1], "log") == 0) {
        name = log_name;
    } else {
        printf("unbekannt: %s (moeglich: log, csv)\n", argv[1]);
        return 1;
    }
    if (!name[0]) {
        fprintf(stdout, "Keine Datei bekannt - steckt die Karte?\n");
        fflush(stdout);
        return 1;
    }

    char zeile[FILE_PUFFER];
    unsigned anzahl = 0;

    sd_archive_lock();
    FILE *f = fopen(name, "r");
    fprintf(stdout, "----- Anfang %s -----\n", name);
    if (f) {
        while (fgets(zeile, sizeof(zeile), f)) {
            fprintf(stdout, "%s", zeile);
            anzahl++;
        }
        fclose(f);
    } else {
        fprintf(stdout, "(nicht lesbar - Karte entfernt?)\n");
    }
    fprintf(stdout, "----- Ende %s (%u Zeilen) -----\n", name, anzahl);
    fflush(stdout);
    sd_archive_unlock();
    return 0;
}

/* Archiv aufraeumen: sim-Zeilen entfernen und/oder das Format wechseln.
 * Beides schreibt die Datei neu (Nebendatei, dann umbenennen). */
static int cmd_csv(int argc, char **argv)
{
    bool sim_weg = false;
    bool kompakt_an = false;
    bool kompakt_aus = false;
    bool aufraeumen = false;

    for (int i = 1; i < argc; i++) {
        if (strcasecmp(argv[i], "simweg") == 0) {
            sim_weg = true;
            aufraeumen = true;
        } else if (strcasecmp(argv[i], "kompakt") == 0) {
            kompakt_an = true;
            aufraeumen = true;
        } else if (strcasecmp(argv[i], "lesbar") == 0) {
            kompakt_aus = true;
            aufraeumen = true;
        } else {
            printf("unbekannt: %s\n", argv[i]);
            printf("Aufruf: csv [simweg] [kompakt|lesbar]\n");
            return 1;
        }
    }

    if (!aufraeumen) {
        int zeilen = 0, sim = 0;
        sd_archive_zaehlen(&zeilen, &sim);
        printf("Datei        : %s\n",
               sd_archive_dateiname()[0] ? sd_archive_dateiname() : "(keine Karte)");
        printf("Groesse      : %u Byte\n", sd_archive_dateigroesse());
        printf("Zeilen       : %d, davon %d sim\n", zeilen, sim);
        printf("Format       : %s\n",
               settings_get_csv_kompakt() ? "kompakt (epoch;e5;e10;diesel;status)"
                                          : "lesbar (mit ISO-Zeit)");
        printf("Aufruf       : csv [simweg] [kompakt|lesbar]\n");
        return 0;
    }

    if (kompakt_an)  settings_set_csv_kompakt(true);
    if (kompakt_aus) settings_set_csv_kompakt(false);

    int gesamt = 0, entfernt = 0;
    if (sd_archive_aufraeumen(sim_weg, &gesamt, &entfernt) < 0) {
        printf("Kein Archiv - steckt die Karte?\n");
        return 1;
    }
    printf("Aufgeraeumt  : %d Zeilen behalten, %d sim-Zeilen entfernt\n",
           gesamt, entfernt);
    printf("Groesse      : %u Byte\n", sd_archive_dateigroesse());
    printf("Format       : %s\n", settings_get_csv_kompakt() ? "kompakt" : "lesbar");
    return 0;
}

static int cmd_tasks(int argc, char **argv)
{
    (void)argc; (void)argv;
    char *buf = malloc(2048);
    if (!buf) {
        printf("kein Speicher\n");
        return 1;
    }
    printf("%-16s %-10s %-5s %-7s %s\n", "Name", "State", "Prio", "Stack", "Core");
    vTaskList(buf);
    printf("%s", buf);
    free(buf);
    return 0;
}

/* ------------------------------------------------------------------ */
static void register_cmd(const char *name, const char *help, esp_console_cmd_func_t func)
{
    const esp_console_cmd_t cmd = {
        .command = name,
        .help = help,
        .hint = NULL,
        .func = func,
        .argtable = NULL,
    };
    ESP_ERROR_CHECK(esp_console_cmd_register(&cmd));
}

esp_err_t console_start(void)
{
    esp_console_repl_t *repl = NULL;
    esp_console_repl_config_t repl_cfg = ESP_CONSOLE_REPL_CONFIG_DEFAULT();
    repl_cfg.prompt = "sprit>";
    repl_cfg.max_cmdline_length = 128;
    repl_cfg.task_stack_size = TASK_STACK_CONSOLE;

    esp_console_dev_uart_config_t uart_cfg = ESP_CONSOLE_DEV_UART_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_console_new_repl_uart(&uart_cfg, &repl_cfg, &repl));

    esp_console_register_help_command();
    register_cmd("wifi",    "wifi <ssid> <passwort>  - WLAN-Zugangsdaten speichern", cmd_wifi);
    register_cmd("wifidel", "wifidel                  - WLAN-Zugangsdaten loeschen", cmd_wifidel);
    register_cmd("wifiscan", "wifiscan                 - Netze in Reichweite auflisten", cmd_wifiscan);
    register_cmd("key",     "key <apikey>             - Tankerkoenig-API-Key speichern", cmd_key);
    register_cmd("station", "station <uuid>           - Tankstellen-ID speichern", cmd_station);
    register_cmd("name",    "name <Text>              - Anzeigename (Kopfzeile)", cmd_name);
    register_cmd("fuel",    "fuel <diesel|e5|e10>     - angezeigten Kraftstoff waehlen", cmd_fuel);
    register_cmd("poll",    "poll                     - sofort abfragen", cmd_poll);
    register_cmd("status",  "status                   - Zustand ausgeben", cmd_status);
    register_cmd("time",    "time                     - Uhrzeit und Zeitgueltigkeit", cmd_time);
    register_cmd("log",     "log reset                - Verlaufsspeicher loeschen", cmd_log);
    register_cmd("file",    "file <log|csv>           - Datei von der Karte ausgeben", cmd_file);
    register_cmd("csv",     "csv [simweg] [kompakt|lesbar] - Archiv aufraeumen/Format", cmd_csv);
    register_cmd("sd",      "sd [mount]               - SD-Archiv: Zustand / Karte mounten", cmd_sd);
    register_cmd("trend",   "trend [diesel|e5|e10]    - Trendwerte der Pfeile", cmd_trend);
    register_cmd("power",   "power [on|off]           - Stromsparmodus / Anzeige schalten", cmd_power);
    register_cmd("sim",     "sim [on|off|fill [tage]] - Simulationsmodus (Testpreise)", cmd_sim);
    register_cmd("cfg",     "cfg [import|template]    - Zugangsdaten aus SD-Datei", cmd_cfg);
    register_cmd("touch",   "touch calib on|off       - Touch-Rohwerte zum Nachmessen", cmd_touch);
    register_cmd("tasks",   "tasks                    - Task-Liste (Diagnose)", cmd_tasks);

    ESP_ERROR_CHECK(esp_console_start_repl(repl));
    ESP_LOGI(TAG, "Konsole bereit (help zeigt alle Befehle)");
    return ESP_OK;
}
