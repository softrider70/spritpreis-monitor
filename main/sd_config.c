/*
 * sd_config.c - Zugangsdaten aus /sdcard/spritpreis.cfg einlesen
 *
 * Siehe sd_config.h fuer den Ablauf. Kurz:
 *   Musterdatei anlegen -> Nutzer traegt Werte ein -> Geraet uebernimmt sie
 *   ins NVS -> Datei wird wieder auf die Platzhalter zurueckgesetzt.
 *
 * Enthalten sind alle vier Werte:
 *   station  Tankstellen-ID (NVS: spritcfg/station)
 *   apikey   API-Key         (NVS: spritcfg/apikey)
 *   ssid     WLAN-Name       (NVS: spritcfg/ssid)
 *   pass     WLAN-Passwort   (NVS: spritcfg/pass)
 *
 * Bei den WLAN-Daten gilt: nur uebernehmen, wenn BEIDE Werte in der Datei
 * stehen. Ein halber Satz (nur SSID) wuerde die Verbindung zerreissen.
 *
 * Weder API-Key noch WLAN-Passwort werden geloggt - nur, ob sie gesetzt sind
 * bzw. wie lang sie sind.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

#include "esp_log.h"

#include "config.h"
#include "sd_archive.h"
#include "sd_config.h"
#include "settings.h"
#include "sim.h"
#include "wifi.h"

static const char *TAG = "cfg";

#define CFG_SSID_MAX   40
#define CFG_PASS_MAX   72

/* Musterdatei: Schluesselwoerter sind vorbelegt, nur die Werte fehlen. */
static const char *VORLAGE =
    "# spritpreis.cfg - Zugangsdaten fuer den Spritpreis-Monitor (CYD)\n"
    "#\n"
    "# So geht es:\n"
    "#   1. Diese Datei am PC oeffnen (microSD in den Kartenleser).\n"
    "#   2. Hinter den Schluesselwoertern die Werte eintragen - Werte ohne\n"
    "#      umgebende Leerzeichen, Zeilen ohne # davor sind Daten.\n"
    "#   3. Karte zurueck ins Geraet stecken und das Geraet neu starten.\n"
    "#   4. Das Geraet uebernimmt die Werte in seinen Flash und setzt diese\n"
    "#      Datei wieder auf die Platzhalter zurueck - die Geheimnisse liegen\n"
    "#      dann nicht mehr auf der Karte.\n"
    "#\n"
    "# Jeder Wert wird einzeln geprueft: steht noch ein Platzhalter da, wird er\n"
    "# ignoriert. WLAN wird nur uebernommen, wenn ssid UND pass eingetragen sind.\n"
    "#\n"
    "# Tankstellen-ID:  tools/find_station.ps1 -ApiKey <key> -Ort \"<strasse ort>\"\n"
    "# API-Key:         https://creativecommons.tankerkoenig.de/\n"
    "# Simulation:      nur noetig, solange kein API-Key da ist (on / off)\n"
    "# Verlauf fuellen:  fill=<tage> erzeugt Testwerte (1..13 Tage stuendlich,\n"
    "#                   dazu Tageswerte bis 30 Tage) - geht erst, wenn die Uhr steht\n"
    "\n"
    "station=HIER_TANKSTELLEN_ID_EINTRAGEN\n"
    "name=HIER_ANZEIGENAME_EINTRAGEN\n"
    "apikey=HIER_API_KEY_EINTRAGEN\n"
    "ssid=HIER_WLAN_NAME_EINTRAGEN\n"
    "pass=HIER_WLAN_PASSWORT_EINTRAGEN\n"
    "# sim= hier on oder off eintragen (leer lassen = nichts aendern)\n"
    "sim=\n"
    "# fill= hier z. B. 13 eintragen (leer lassen = nichts fuellen)\n"
    "fill=\n";

/* Gelesene Werte einer Datei */
typedef struct {
    char station[SETTINGS_STATION_MAX];
    char apikey[SETTINGS_KEY_MAX];
    char name[SETTINGS_NAME_MAX];
    char ssid[CFG_SSID_MAX];
    char pass[CFG_PASS_MAX];
    bool station_da;
    bool apikey_da;
    bool name_da;
    bool ssid_da;
    bool pass_da;
    bool sim_da;
    bool sim_an;
    bool sim_ungueltig;
    bool fill_da;
    int  fill_tage;
    /* Kam die Zeile ueberhaupt vor? Damit erkennt der Import eine veraltete
     * Musterdatei (z. B. ohne WLAN-Bereich) und schreibt sie neu. */
    bool zeile_station;
    bool zeile_apikey;
    bool zeile_name;
    bool zeile_ssid;
    bool zeile_pass;
    bool zeile_sim;
    bool zeile_fill;
} cfg_datei_t;

/* Leerzeichen/Tabs und Zeilenende an beiden Enden entfernen. */
static void trimmen(char *s)
{
    if (!s) {
        return;
    }
    size_t n = strlen(s);
    while (n > 0 && (s[n - 1] == '\n' || s[n - 1] == '\r' ||
                     s[n - 1] == ' '  || s[n - 1] == '\t')) {
        s[--n] = 0;
    }
    size_t start = 0;
    while (s[start] == ' ' || s[start] == '\t') {
        start++;
    }
    if (start > 0) {
        memmove(s, s + start, n - start + 1);
    }
}

/* Ein Wert gilt als Platzhalter, wenn er leer ist, das Platzhalter-Wort
 * enthaelt oder zu kurz fuer echte Zugangsdaten ist. */
static bool ist_platzhalter(const char *wert, size_t min_len)
{
    if (!wert || !wert[0]) {
        return true;
    }
    if (strstr(wert, SD_CONFIG_PLATZHALTER) != NULL) {
        return true;
    }
    return strlen(wert) < min_len;
}

/* Datei lesen. Rueckgabe: false, wenn sie fehlt oder nicht lesbar ist. */
static bool datei_lesen(cfg_datei_t *out)
{
    memset(out, 0, sizeof(*out));

    FILE *f = fopen(SD_CONFIG_PATH, "r");
    if (!f) {
        return false;
    }

    char zeile[160];
    while (fgets(zeile, sizeof(zeile), f)) {
        char *p = zeile;
        while (*p == ' ' || *p == '\t') {
            p++;
        }
        if (*p == '#' || *p == '\n' || *p == '\r' || *p == 0) {
            continue;
        }
        char *gleich = strchr(p, '=');
        if (!gleich) {
            continue;
        }
        *gleich = 0;
        char *name = p;
        char *wert = gleich + 1;
        trimmen(name);
        trimmen(wert);

        if (strcasecmp(name, "station") == 0) {
            out->zeile_station = true;
            if (!ist_platzhalter(wert, 8)) {          /* UUID hat 36 Zeichen */
                snprintf(out->station, sizeof(out->station), "%s", wert);
                out->station_da = true;
            }
        } else if (strcasecmp(name, "apikey") == 0) {
            out->zeile_apikey = true;
            if (!ist_platzhalter(wert, 8)) {
                snprintf(out->apikey, sizeof(out->apikey), "%s", wert);
                out->apikey_da = true;
            }
        } else if (strcasecmp(name, "name") == 0) {
            /* Anzeigename fuer die Kopfzeile - darf Leerzeichen enthalten und
             * ist frei waehlbar, daher nur die Platzhalterpruefung. */
            out->zeile_name = true;
            if (!ist_platzhalter(wert, 2)) {
                snprintf(out->name, sizeof(out->name), "%s", wert);
                out->name_da = true;
            }
        } else if (strcasecmp(name, "ssid") == 0) {
            out->zeile_ssid = true;
            if (!ist_platzhalter(wert, 1)) {
                snprintf(out->ssid, sizeof(out->ssid), "%s", wert);
                out->ssid_da = true;
            }
        } else if (strcasecmp(name, "pass") == 0 ||
                   strcasecmp(name, "password") == 0) {
            out->zeile_pass = true;
            if (!ist_platzhalter(wert, 8)) {          /* WPA2 verlangt 8 Zeichen */
                snprintf(out->pass, sizeof(out->pass), "%s", wert);
                out->pass_da = true;
            }
        } else if (strcasecmp(name, "sim") == 0) {
            out->zeile_sim = true;
            /* Der Wert ist ein Schalter, hier keine Laengenpruefung */
            if (strstr(wert, SD_CONFIG_PLATZHALTER) != NULL) {
                continue;
            }
            if (strcasecmp(wert, "on") == 0 || strcasecmp(wert, "an") == 0 ||
                strcasecmp(wert, "ein") == 0 || strcasecmp(wert, "1") == 0) {
                out->sim_da = true;
                out->sim_an = true;
            } else if (strcasecmp(wert, "off") == 0 || strcasecmp(wert, "aus") == 0 ||
                       strcasecmp(wert, "0") == 0) {
                out->sim_da = true;
                out->sim_an = false;
            } else if (wert[0] != 0) {
                out->sim_ungueltig = true;            /* z. B. "yes" */
            }
        } else if (strcasecmp(name, "fill") == 0) {
            out->zeile_fill = true;
            if (!ist_platzhalter(wert, 1)) {
                int tage = atoi(wert);
                if (tage >= 1 && tage <= SIM_FILL_MAX_TAGE) {
                    out->fill_da = true;
                    out->fill_tage = tage;
                } else {
                    ESP_LOGW(TAG, "fill=%s passt nicht (1..%d)", wert, SIM_FILL_MAX_TAGE);
                }
            }
        }
    }
    fclose(f);
    return true;
}

void sd_config_write_template(void)
{
    if (!sd_archive_ready()) {
        ESP_LOGW(TAG, "Keine Karte - Musterdatei nicht angelegt");
        return;
    }
    FILE *f = fopen(SD_CONFIG_PATH, "w");
    if (!f) {
        ESP_LOGW(TAG, "%s laesst sich nicht schreiben", SD_CONFIG_PATH);
        return;
    }
    const size_t n = fwrite(VORLAGE, 1, strlen(VORLAGE), f);
    if (fclose(f) != 0 || n != strlen(VORLAGE)) {
        ESP_LOGW(TAG, "Musterdatei nur teilweise geschrieben");
        return;
    }
    ESP_LOGI(TAG, "Musterdatei angelegt: %s (Werte eintragen, dann neu starten)",
             SD_CONFIG_PATH);
}

int sd_config_import(void)
{
    if (!sd_archive_ready()) {
        return 0;                     /* ohne Karte gibt es nichts zu lesen */
    }

    cfg_datei_t cfg;
    if (!datei_lesen(&cfg)) {
        /* Erster Start mit Karte: Vorlage anlegen, damit nur noch die
         * Werte eingetragen werden muessen. */
        sd_config_write_template();
        return 0;
    }

    if (!cfg.station_da && !cfg.name_da && !cfg.apikey_da && !cfg.ssid_da &&
        !cfg.pass_da && !cfg.sim_da && !cfg.fill_da) {
        /* Keine Werte drin (nur Platzhalter). Wenn dabei Schluessel fehlen -
         * etwa eine alte Musterdatei ohne WLAN-Bereich oder ohne den neuen
         * Namen -, wird die Datei auf die aktuelle Fassung gebracht. Werte
         * koennen dabei nicht verloren gehen, weil keine da sind. */
        const bool komplett = cfg.zeile_station && cfg.zeile_name &&
                              cfg.zeile_apikey && cfg.zeile_ssid &&
                              cfg.zeile_pass && cfg.zeile_sim && cfg.zeile_fill;
        if (!komplett) {
            sd_config_write_template();
            ESP_LOGI(TAG, "Musterdatei aktualisiert (fehlende Schluessel ergaenzt)");
        } else {
            ESP_LOGI(TAG, "Datei enthaelt nur Platzhalter - nichts zu uebernehmen");
        }
        return 0;
    }
    if (cfg.sim_ungueltig) {
        ESP_LOGW(TAG, "sim= hat einen unbekannten Wert - bitte on oder off");
    }

    int anzahl = 0;

    /* Tankstellen-ID und API-Key: nur schreiben, wenn sie sich unterscheiden
     * (schont das Flash). */
    char nvs_station[SETTINGS_STATION_MAX];
    char nvs_key[SETTINGS_KEY_MAX];
    settings_load(nvs_station, sizeof(nvs_station), nvs_key, sizeof(nvs_key));

    if (cfg.station_da && strcmp(cfg.station, nvs_station) != 0) {
        settings_set_station(cfg.station);
        ESP_LOGI(TAG, "Tankstellen-ID aus der Datei uebernommen");
        anzahl++;
    }
    if (cfg.apikey_da && strcmp(cfg.apikey, nvs_key) != 0) {
        settings_set_api_key(cfg.apikey);
        ESP_LOGI(TAG, "API-Key aus der Datei uebernommen (%u Zeichen)",
                 (unsigned)strlen(cfg.apikey));
        anzahl++;
    }

    /* Anzeigename fuer die Kopfzeile. */
    if (cfg.name_da) {
        char nvs_name[SETTINGS_NAME_MAX];
        settings_get_name(nvs_name, sizeof(nvs_name));
        if (strcmp(cfg.name, nvs_name) != 0) {
            settings_set_name(cfg.name);
            ESP_LOGI(TAG, "Anzeigename aus der Datei uebernommen: %s", cfg.name);
            anzahl++;
        }
    }

    /* WLAN: nur als vollstaendiges Paar uebernehmen. */
    if (cfg.ssid_da && cfg.pass_da) {
        if (cfg.ssid[0] && strcmp(cfg.ssid, wifi_ssid()) != 0) {
            wifi_set_credentials(cfg.ssid, cfg.pass);
            ESP_LOGI(TAG, "WLAN-Zugangsdaten uebernommen (%s, Passwort %u Zeichen)",
                     cfg.ssid, (unsigned)strlen(cfg.pass));
            anzahl++;
        } else {
            ESP_LOGI(TAG, "WLAN-Name ist bereits gespeichert - unveraendert gelassen");
        }
    } else if (cfg.ssid_da != cfg.pass_da) {
        ESP_LOGW(TAG, "WLAN unvollstaendig (ssid und pass noetig) - nicht uebernommen");
    }

    /* Simulationsmodus: nur schreiben, wenn er sich wirklich aendert. */
    if (cfg.sim_da && sim_enabled() != cfg.sim_an) {
        sim_set_enabled(cfg.sim_an);
        ESP_LOGI(TAG, "Simulation %s (aus der Datei)", cfg.sim_an ? "AN" : "AUS");
        anzahl++;
    }

    // Verlauf fuellen vormerken: geht erst, wenn die Uhr gestellt ist
    if (cfg.fill_da) {
        sim_request_fill(cfg.fill_tage);
        ESP_LOGI(TAG, "Verlauf wird gefuellt, sobald die Uhr steht (%d Tage)",
                 cfg.fill_tage);
        anzahl++;
    }

    /* Datei zuruecksetzen: Werte sind jetzt im Flash, die Karte braucht sie
     * nicht mehr. Das Muster bleibt zum spaeteren Aendern stehen. */
    sd_config_write_template();
    if (anzahl > 0) {
        ESP_LOGI(TAG, "%d Wert(e) uebernommen, Datei zurueckgesetzt", anzahl);
    } else {
        ESP_LOGI(TAG, "Keine neuen Werte, Datei zurueckgesetzt");
    }
    return anzahl;
}

/* Zustand eines Schluessels fuer die Ausgabe: "Wert", "Platzhalter", "fehlt".
 * Der Wert selbst wird nie ausgegeben. */
static const char *zustand(bool da, bool vorhanden)
{
    if (!vorhanden) {
        return "fehlt";
    }
    return da ? "Wert eingetragen" : "nur Platzhalter";
}

void sd_config_print_status(void)
{
    printf("Datei        : %s\n", SD_CONFIG_PATH);

    cfg_datei_t cfg;
    bool vorhanden = false;
    if (!sd_archive_ready()) {
        printf("Karte        : nicht gemountet (sd mount)\n");
    } else {
        vorhanden = datei_lesen(&cfg);
        if (!vorhanden) {
            printf("Zustand      : Datei fehlt (wird beim naechsten Start angelegt)\n");
        } else {
            printf("In der Datei : station=%s, apikey=%s\n",
                   zustand(cfg.station_da, true), zustand(cfg.apikey_da, true));
            printf("               ssid=%s, pass=%s\n",
                   zustand(cfg.ssid_da, true), zustand(cfg.pass_da, true));
            printf("               sim=%s\n",
                   cfg.sim_ungueltig ? "unbekannter Wert (on/off)"
                                     : (cfg.sim_da ? (cfg.sim_an ? "on" : "off") : "leer"));
            printf("               fill=%s\n",
                   cfg.fill_da ? "wird gefuellt" : "leer");
            if (cfg.ssid_da != cfg.pass_da) {
                printf("               (WLAN wird nur als Paar uebernommen)\n");
            }
        }
    }

    char nvs_station[SETTINGS_STATION_MAX];
    char nvs_key[SETTINGS_KEY_MAX];
    settings_load(nvs_station, sizeof(nvs_station), nvs_key, sizeof(nvs_key));
    printf("Im NVS       : station=%s, apikey=%s\n",
           nvs_station[0] ? nvs_station : "(leer)",
           nvs_key[0] ? "gesetzt" : "(leer)");
    printf("               ssid=%s, WLAN-Passwort=%s\n",
           wifi_ssid()[0] ? wifi_ssid() : "(leer)",
           wifi_has_credentials() ? "gesetzt" : "(leer)");
    printf("               Simulation=%s\n", sim_enabled() ? "AN" : "aus");
    printf("Einlesen     : cfg import   (Werte ins NVS, Datei danach zurueck)\n");
}
