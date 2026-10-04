/*
 * config.h - Zentrale Konfiguration Spritpreis-Monitor
 *
 * Board: CYD ESP32-2432S028R (ESP32-WROOM-32, 4 MB Flash, kein PSRAM)
 *        2,8"-Display ILI9341 (320x240) + resistiver Touch XPT2046
 *
 * Aufgabe: Eine feste Tankstelle beobachten, aktuellen Preis gross anzeigen
 *          und den bisherigen Verlauf als Diagramm (Stunden-/Tagesansicht,
 *          per Touch verschiebbar).
 *
 * Datenquelle: Tankerkönig-API auf Basis der Markttransparenzstelle fuer
 *              Kraftstoffe (MTS-K, Bundeskartellamt), Lizenz CC BY 4.0.
 *              Die Lizenz verlangt eine Quellenangabe - die steht fest im
 *              Display (siehe ui.c, Zeile "Quelle: MTS-K / Tankerkoenig").
 *
 * Hinweis zur API (verifiziert 2026-09-23):
 *  - Host: creativecommons.tankerkoenig.de (der alte Host api.tankerkoenig.de
 *    existiert nicht mehr)
 *  - Nur AKTUELLE Preise. Verlauf liefert die API nicht -> das Geraet
 *    schreibt ihn selbst mit (siehe price_log.c).
 *  - Grenzen: API-Key Pflicht, max. 1 Abfrage/Minute, Umkreis max. 25 km,
 *    keine Verfuegbarkeitsgarantie (best effort).
 */

#ifndef CONFIG_H
#define CONFIG_H

#ifdef __cplusplus
extern "C" {
#endif

#define BOARD_NAME          "Spritpreis-Monitor"
#define BOARD_CHIP          "ESP32-D0WD-V3 (CYD)"
#define APP_VERSION_MAJOR   0
#define APP_VERSION_MINOR   1

/* =====================================================================
 * Datenquelle / Abfrage
 * ===================================================================== */
#define FUEL_API_HOST           "creativecommons.tankerkoenig.de"
#define FUEL_API_PATH_PRICES    "/json/prices.php"     /* ?ids=<uuid>&apikey=.. */
#define FUEL_HTTP_TIMEOUT_MS    10000
#define FUEL_RESPONSE_MAX       2048                   /* reicht fuer eine Station */

/* Abfrageintervall: 300 s. Die freie API erlaubt max. 1 Abfrage/Minute,
 * es wird pro Abfrage nur EINE Station geholt -> weit unter dem Limit. */
#define FUEL_POLL_INTERVAL_S    300

/* Erststart-Werte. Leer = muss per Konsole gesetzt werden:
 *   station <uuid>   -> Tankstellen-ID (siehe tools/find_station.ps1)
 *   key <apikey>     -> API-Key (Anmeldung auf creativecommons.tankerkoenig.de)
 * Danach liegen beide Werte im NVS (Namespace "spritcfg"). */
#define FUEL_STATION_ID_DEFAULT ""
#define FUEL_API_KEY_DEFAULT    ""

/* Zeitzone (POSIX) - Deutschland mit Sommerzeit */
#define APP_TIMEZONE            "CET-1CEST,M3.5.0,M10.5.0/3"
#define APP_SNTP_SERVER         "pool.ntp.org"
/* Zeiten vor diesem Wert gelten als "Uhr noch nicht gestellt" */
#define APP_TIME_VALID_FROM     1600000000L

/* =====================================================================
 * Kraftstoffe
 * ===================================================================== */
enum {
    FUEL_E5 = 0,
    FUEL_E10,
    FUEL_DIESEL,
    FUEL_COUNT
};

#define FUEL_DEFAULT_INDEX      FUEL_E10    /* Anzeige ist fest auf E10 gelegt */

/* =====================================================================
 * Display (CYD) - Pins VERIFIZIERT (aus autofrontcam/cyd uebernommen)
 * CLK=14, MOSI=13, CS=15, DC=2, RST=-1 (haengt am ESP32-Reset), BL=21.
 * ===================================================================== */
#define TFT_SPI_HOST         SPI2_HOST
#define TFT_SCK              14
#define TFT_MOSI             13
#define TFT_MISO             12
#define TFT_CS               15
#define TFT_DC               2
#define TFT_RST              -1
#define TFT_BL               21      /* Backlight (active HIGH) */
#define TFT_BL_ON            1
#define TFT_WIDTH            320
#define TFT_HEIGHT           240
#define ILI9341_MADCTL       0x40    /* 320x240-Nativpanel (kein MV) */
#define CYD_INVERT_COLOR     0       /* 1 = INVON 0x21 (ST7789-Variante) */

/* =====================================================================
 * Touch (XPT2046, eigener SPI-Bus) - Pins VERIFIZIERT (autofrontcam/cyd)
 * MOSI=32, MISO=39, CLK=25, CS=33, IRQ=36
 * ===================================================================== */
#define TOUCH_SPI_HOST       SPI3_HOST
#define TOUCH_MOSI           32
#define TOUCH_MISO           39
#define TOUCH_SCLK           25
#define TOUCH_CS             33
#define TOUCH_IRQ            36

/* =====================================================================
 * Verlaufsspeicher (price_log.c)
 *  - stuendlich: 14 Tage (336 Werte)
 *  - taeglich:   120 Tage
 * Pro Wert: min / max / letzter Preis je Kraftstoff.
 * Persistenz: ein kleiner NVS-Eintrag je Stunde bzw. je Tag, damit das
 * Flash nur selten beschrieben wird.
 * ===================================================================== */
#define LOG_HOUR_DAYS           14
#define LOG_HOURS               (24 * LOG_HOUR_DAYS)
#define LOG_DAYS                120
#define LOG_NVS_NAMESPACE       "spritlog"

/* =====================================================================
 * microSD - Langzeitarchiv (sd_archive.c)
 *
 * Die Karte haengt beim CYD am VSPI-Bus. Pin-Angabe aus der CYD-Doku
 * (witnessmenow/ESP32-Cheap-Yellow-Display, PINS.md, Abschnitt "SD Card"):
 * IO5 = CS, IO18 = SCK, IO19 = MISO, IO23 = MOSI.
 *
 * ACHTUNG: VSPI ist derselbe Peripherie-Bus, den der Touch urspruenglich
 * benutzte (SPI3_HOST). Auf einem Hardware-SPI-Bus sind keine zwei
 * Pin-Saetze moeglich, deshalb laeuft der Touch jetzt per Software-Bitbang
 * (siehe touch.c) und die Karte bekommt den Hardware-Bus allein.
 * ===================================================================== */
#define SD_SPI_HOST             SPI3_HOST
#define SD_SCK                  18
#define SD_MISO                 19
#define SD_MOSI                 23
#define SD_CS                   5
#define SD_MOUNT_POINT          "/sdcard"
#define SD_MAX_FILES            3          /* gleichzeitig offene Dateien */

/* Konfigurationsdatei auf der Karte (Zugangsdaten einsammeln).
 * Ablauf: Geraet legt die Musterdatei mit Platzhaltern an, der Nutzer traegt
 * die Werte am PC ein, beim naechsten Start uebernimmt das Geraet sie ins NVS
 * und setzt die Datei wieder auf die Platzhalter zurueck - so liegen die
 * Geheimnisse nicht dauerhaft auf der entnehmbaren Karte. */
#define SD_CONFIG_PATH          SD_MOUNT_POINT "/spritpreis.cfg"
#define SD_CONFIG_PLATZHALTER   "EINTRAGEN"

/* =====================================================================
 * Trendpfeile (trend.c)
 *
 * Verglichen wird der aktuelle Preis mit dem DURCHSCHNITT des jeweiligen
 * Zeitraums. Jedes Vergleichsfenster endet 24 h vor jetzt, hat also
 * dieselbe Tagesphase wie der aktuelle Zeitpunkt. Grund: Preise duerfen
 * nur einmal taeglich um 12:00 Uhr erhoeht werden, danach sind nur
 * Senkungen erlaubt (MTS-K). Ein Fenster bis "jetzt" waere nachmittags
 * systematisch zu billig und der Pfeil zeigte fast immer "billiger".
 *
 * Zusatzfeld "12U": aktueller Preis gegen den letzten Preis ab dem letzten
 * 12-Uhr-Zeitpunkt - der einzige Zeitpunkt, an dem eine Erhoehung moeglich
 * war. Ausfuehrlich: docs/trend.md
 * ===================================================================== */
#define TREND_FLAT_MILLI        10         /* bis 1 Cent Unterschied = "gleich" */
#define TREND_MIN_FILL_PCT      50         /* mind. so viel % Werte im Fenster */

/* Guenstigste Tagesstunde (trend.c, trend_best_hour):
 * Fuer jeden der letzten Tage wird die Stunde mit dem niedrigsten Preis
 * bestimmt; angezeigt wird die Stunde, die am haeufigsten das Tagestief war.
 * Tage mit zu wenigen Werten zaehlen nicht (sonst waere das "Tief" kuenstlich). */
#define BEST_HOUR_DAYS          7
#define BEST_HOUR_MIN_STUNDEN   12

/* =====================================================================
 * Stromsparen (power.c)
 *
 * Das Display ist der grosse Verbraucher (Hintergrundbeleuchtung). Es ist
 * nur nach einem Ereignis an: Touch, BOOT-Taste oder Preisaenderung.
 * Danach laeuft POWER_DISPLAY_ON_MS ab und es geht wieder aus.
 *
 * Zusaetzlich wird der Light-Sleep genutzt (automatisch, sobald die CPU
 * nichts zu tun hat). Wakeup-Quellen dafuer:
 *   - Touch-IRQ (GPIO36, RTC-Pin, open-drain) -> bei Beruehrung low
 *   - BOOT-Taste (GPIO0, active low)        -> als Notfall-Wecker
 * Wichtig: Der Touch kann nur wecken, wenn der IRQ-Pin einen Pull-up hat
 * und im Ruhezustand wirklich high ist. Das wird beim Start geprueft und
 * geloggt (Konsole: power). Ist er low, bleibt nur "Display aus" - dann
 * laeuft die CPU weiter und der Touch reagiert sofort.
 * ===================================================================== */
#define POWER_DISPLAY_ON_MS     180000     /* Anzeige bleibt so lange an (3 min) */
#define POWER_BUTTON_GPIO       0          /* BOOT-Taste am CYD (active low) */
#define POWER_POLL_MS           250        /* Takt der Tastenabfrage */
#define POWER_LIGHT_SLEEP       1          /* 0 = nur Display aus */
#define POWER_CPU_MIN_MHZ       40
#define POWER_CPU_MAX_MHZ       160

/* =====================================================================
 * Lademodul wachhalten (Lastimpulse)
 *
 * Manche Lademodule (weisse Bauart mit 5-V-Ausgang, "Powerbank-Bauart")
 * schalten den Ausgang ab, wenn ueber eine Zeit zu wenig Strom fliesst.
 * Im Stromsparmodus zieht das CYD nur wenige mA - das Modul sieht "keine
 * Last" und schaltet ab. Gegenmittel: in festen Abstaenden kurz Last ziehen.
 *
 * Als Last dient im Normalfall ein **WLAN-Scan** (wifi_lastimpuls): rund
 * 2-3 s auf allen Kanaelen, das zieht deutlich Strom und braucht keine
 * Zusatzhardware. Eine einzelne Preisabfrage reicht dafuer NICHT - sie
 * dauert nur 1-2 s und liegt als Mittelwert ueber 5 Minuten im
 * Milliampere-Bereich (genau deshalb schaltet das Modul trotz Abfragen ab).
 *
 * Wer stattdessen eine Zusatzlast an einen Pin haengen will (Transistor,
 * NICHT direkt - ein GPIO darf nur etwa 20 mA treiben):
 *   IO22 -- 1k -- Basis NPN (z. B. BC547), Emitter an GND
 *   Kollektor -- 100 Ohm -- 3,3 V   -> etwa 31 mA, 0,1 W
 * Dann POWER_KEEPALIVE_GPIO auf 22 setzen. -1 = WLAN-Scan benutzen.
 *
 * WICHTIG: Der Abstand (x) muss KLEINER sein als das Abschaltfenster des
 * Moduls. Wie lang das ist, haengt vom Modul ab und muss am eigenen Geraet
 * ermittelt werden (Stoppuhr: wie lange laeuft es ohne Impuls?). Ausgangswert
 * hier: 10 Minuten. Im Betrieb laesst sich x ohne Neubauen aendern mit
 *   keepalive <sekunden> [millisekunden]
 * Der Konsolenwert gilt nur bis zum Neustart - hat sich ein Wert bewaehrt,
 * gehoert er hierher (0 = aus).
 * ===================================================================== */
#define POWER_KEEPALIVE_GPIO    (-1)   /* -1 = WLAN-Scan als Last (keine Zusatzhardware) */
#define POWER_KEEPALIVE_S       600    /* Abstand der Impulse in Sekunden (10 min), 0 = aus */
#define POWER_KEEPALIVE_MS      200    /* Dauer der Zusatzlast am Pin (beim WLAN-Scan ohne Wirkung) */

/* =====================================================================
 * Batteriespannung (1S-LiIon)
 *
 * Gemessen wird die Zellspannung ueber einen Spannungsteiler an einem
 * ADC1-Pin. Nur ADC1 taugt: ADC2 wird vom Funkmodul belegt, seine Werte
 * sind bei laufendem WLAN unbrauchbar.
 *
 *   B+ --[R1]--+-- GPIO35 (Stecker P3: GND, IO35, IO22, IO21)
 *              |
 *           [100 nF]
 *              |
 *   B- --------+-- GND des CYD      (B- MUSS dieses GND sein - vorher messen!)
 *
 * R1 = R2 = 100 kOhm -> am Pin liegt die halbe Zellspannung
 * (4,2 V Zelle -> 2,1 V), also weit im sauberen Bereich. Der Teiler zieht
 * rund 21 uA, das ist fuer den Akku unerheblich.
 *
 * NIE die Zelle direkt anschliessen: 4,2 V zerstoeren einen 3,3-V-Pin.
 * Und NICHT die 5-V-Schiene messen - dann sieht man den Booster, nicht
 * den Ladezustand.
 * ===================================================================== */
#define BATTERY_ADC_GPIO        35     /* ADC1 (Kanal wird automatisch bestimmt) */
#define BATTERY_TEILER_R1       100    /* kOhm: von B+ zum Messpunkt */
#define BATTERY_TEILER_R2       100    /* kOhm: vom Messpunkt nach GND */
#define BATTERY_MESS_TAKT_MS    10000  /* so oft wird neu gemessen (10 s) */

/* Feinkorrektur gegen das Multimeter. Angezeigt wird
 *   U_zelle = U_pin * (R1+R2)/R2 * BATTERY_KORREKTUR_MILLI / 1000
 * 1000 = keine Korrektur (Vorgabe).
 *
 * Fuer den Teiler ist das NICHT gedacht - der stimmt, wenn R1/R2 stimmen
 * (nachgerechnet: 3,763 V Zelle bei 1,883 V am Pin = 0,08 % Abweichung).
 * Der Wert gleicht nur die Streuung des ADC aus. Bestimmen:
 *   1. Zellspannung mit dem Multimeter messen (Referenz).
 *   2. `batt` auf der Konsole aufrufen und die angezeigte Zellspannung lesen.
 *   3. Faktor = 1000 * (Multimeterwert / angezeigter Wert), hier eintragen.
 * Beispiel: Multimeter 3,763 V, Anzeige 3,720 V -> 1012. */
#define BATTERY_KORREKTUR_MILLI 1000

/* =====================================================================
 * Simulation (sim.c)
 *
 * Solange kein API-Key vorliegt, laesst sich der Betrieb mit erfundenen,
 * aber regelkonformen Preisen testen: Erzeugt wird ein Tagesverlauf, der
 * die MTS-K-Regel nachbildet - Erhoehung um 12:00 Uhr, danach stundenweise
 * Senkungen bis 22:00 Uhr, dann bleibt der Preis bis zum Mittag stehen.
 * Das Muster ist deterministisch (Hash aus Datum und Stunde), also bei
 * gleicher Uhrzeit reproduzierbar.
 *
 * Wichtig: Die Simulation ist im Status sichtbar ("SIM" auf der Anzeige,
 * status="sim" in der CSV-Datei). Sie ist kein Ersatz fuer echte Daten.
 * ===================================================================== */
#define SIM_DEFAULT_ON          0      /* nach dem Flashen aus */
#define SIM_FILL_TAGE           13     /* Standard fuer "sim fill" (stuendlich) */
#define SIM_FILL_MAX_TAGE       13     /* 13*24 h passt ins 14-Tage-Fenster */
#define SIM_FILL_TAGESWERTE     30     /* zusaetzlich so viele Tage als Tageswerte */
#define SIM_FILL_STUETZPUNKTE   4      /* Stuetzpunkte je altem Tag (1/8/13/21 Uhr) */

#define SIM_BASIS_E10_MILLI     1659   /* 1,659 EUR */
#define SIM_BASIS_E5_MILLI      1719
#define SIM_BASIS_DIESEL_MILLI  1629

#define SIM_SPRUNG_MILLI        45     /* Erhoehung um 12:00 Uhr (4,5 ct) */
#define SIM_SPRUNG_VAR_MILLI    25     /* zusaetzlich 0..2,5 ct je Tag */
#define SIM_SENKUNG_MILLI       6      /* Senkung je Stunde (0,6 ct) */
#define SIM_SENKUNG_VAR_MILLI   5      /* zusaetzlich 0..0,5 ct je Tag */
#define SIM_TAGES_VAR_MILLI     60     /* Tagesniveau schwankt um +-3 ct */
#define SIM_SENKUNG_STUNDEN     10     /* Senkungen in den Stunden 13..22 */

/* =====================================================================
 * RTOS
 *  - Netz/HTTP-Abfrage -> Core 0 (PRO_CPU), weil TLS/HTTP viel CPU zieht
 *  - Display/UI        -> Core 1 (APP_CPU), bleibt dadurch fluessig
 * Stackgroessen in BYTES (ESP-IDF-Konvention).
 * ===================================================================== */
#define TASK_CORE_NET           0
#define TASK_CORE_DISPLAY       1
#define TASK_STACK_POLL         8192
#define TASK_STACK_UI           4096
#define TASK_STACK_MONITOR      3072
#define TASK_STACK_CONSOLE      4096

#ifdef __cplusplus
}
#endif

#endif /* CONFIG_H */
