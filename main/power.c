/*
 * power.c - Anzeige-Stromsparmodus (Display aus + Light-Sleep)
 *
 * Aufbau:
 *   - Ein eigener Task prueft im Takt POWER_POLL_MS die BOOT-Taste und den
 *     Ablauf der Anzeige-Haltezeit. Er laeuft auf Core 1 (Display-Seite).
 *   - Der Light-Sleep wird NICHT von Hand gestartet: Mit aktiviertem Power
 *     Management schlaeft die CPU automatisch, sobald kein Task etwas zu tun
 *     hat (Tickless Idle). Weil hier alles in kurzen Abstaenden passiert,
 *     sind das viele kleine Schlafphasen - das ist gewollt, denn die
 *     Aufwachzeit selbst kostet Strom.
 *   - Wecken kann der Light-Sleep ueber zwei Pins:
 *       * Touch-IRQ (GPIO36)  - wird bei Beruehrung low gezogen
 *       * BOOT-Taste (GPIO0)  - wird beim Druecken low gezogen
 *     Der Touch-IRQ ist open-drain: Ohne Pull-up bleibt er im Ruhezustand
 *     low und waere als Wecker unbrauchbar (die CPU wuerde sofort wieder
 *     aufwachen). Deshalb wird der Pegel beim Start geprueft und das
 *     Ergebnis geloggt - notfalls bleibt es bei "nur Display aus".
 *
 * Die Preise werden weiter abgefragt, auch wenn die Anzeige aus ist.
 */

#include <string.h>
#include <time.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "driver/gpio.h"
#include "driver/uart.h"
#include "esp_pm.h"
#include "esp_sleep.h"
#include "esp_timer.h"
#include "esp_log.h"

#include "config.h"
#include "display.h"
#include "power.h"

static const char *TAG = "power";

#define POWER_TASK_STACK   3072
#define POWER_TASK_PRIO    4

static bool     s_on = true;               /* Anzeige an? */
static int64_t  s_until_us = 0;            /* bis hierhin bleibt sie an */
static uint32_t s_activity = 0;            /* Ereignisse seit dem Start */
static bool     s_light_sleep = false;     /* Light-Sleep eingerichtet? */
static bool     s_irq_usable = false;      /* Touch-IRQ als Wecker nutzbar? */
static int      s_wake_pin = -1;           /* Pin, der den Sleep beendet */
static void   (*s_wake_cb)(void) = NULL;

static void display_aus(void)
{
    display_backlight(false);
    /* Das Bild bleibt im Speicher des Panels stehen. Beim Aufwecken ist es
     * damit sofort wieder da - nur so ist die Anzeige ohne spuerbare Pause
     * zurueck, ohne dass das ganze Bild neu aufgebaut werden muss. */
    display_power(false);                  /* DISPOFF + SLPIN */
    s_on = false;
    ESP_LOGI(TAG, "Anzeige aus (Stromsparmodus)");
}

/* Anzeige einschalten. Reihenfolge ist wichtig: erst das Panel wecken, dann
 * das Bild fertig zeichnen, und ERST DANN die Hintergrundbeleuchtung an -
 * sonst sieht man den Bildaufbau als Flackern. */
static void display_ein(void)
{
    const int64_t t0 = esp_timer_get_time();
    display_power(true);                   /* SLPOUT + 120 ms + DISPON */
    s_on = true;                           /* ab jetzt darf gezeichnet werden */
    if (s_wake_cb) {
        s_wake_cb();
    }
    display_backlight(true);
    ESP_LOGI(TAG, "Anzeige an nach %d ms", (int)((esp_timer_get_time() - t0) / 1000));
}

/* Prueft die BOOT-Taste und schaltet die Anzeige nach Ablauf der Haltezeit aus. */
static void power_task(void *arg)
{
    (void)arg;
    bool taster_vorher = false;

    for (;;) {
        bool taster = (gpio_get_level(POWER_BUTTON_GPIO) == 0);   /* active low */
        if (taster && !taster_vorher) {
            ESP_LOGI(TAG, "BOOT-Taste gedrueckt");
            power_activity();
        }
        taster_vorher = taster;

        if (s_on && esp_timer_get_time() >= s_until_us) {
            display_aus();
        }
        vTaskDelay(pdMS_TO_TICKS(POWER_POLL_MS));
    }
}

void power_init(void)
{
    /* BOOT-Taste als Eingang mit Pull-up (wird im Ruhezustand high gehalten) */
    gpio_config_t taster = {
        .pin_bit_mask = (1ULL << POWER_BUTTON_GPIO),
        .mode = GPIO_MODE_INPUT,
        .pull_up_en = GPIO_PULLUP_ENABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    esp_err_t err = gpio_config(&taster);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "BOOT-Taste nicht nutzbar (%s)", esp_err_to_name(err));
    }

    /* Wakeup-Quelle waehlen.
     * Der ESP32 kann im Light-Sleep nur EINEN Low-Pegel-Wecker auf einem
     * RTC-Pin (ext0); die Mehrfach-Variante ext1 kennt auf diesem Chip nur
     * ALL_LOW oder ANY_HIGH. Deshalb: Touch-IRQ bevorzugt, und nur wenn der
     * nicht taugt (open-drain ohne Pull-up bleibt low), ubernimmt die
     * BOOT-Taste. Beide sind RTC-Pins (IO36 bzw. IO0). */
    if (gpio_get_level(TOUCH_IRQ) == 1) {
        s_wake_pin = TOUCH_IRQ;
        s_irq_usable = true;
    } else if (gpio_get_level(POWER_BUTTON_GPIO) == 1) {
        s_wake_pin = POWER_BUTTON_GPIO;
        ESP_LOGW(TAG, "Touch-IRQ (IO%d) liegt auf low - kein Pull-up? "
                      "Es weckt ersatzweise die BOOT-Taste", TOUCH_IRQ);
    } else {
        ESP_LOGW(TAG, "Weder Touch-IRQ noch BOOT-Taste sind high - es gibt "
                      "keinen Wecker, die CPU bleibt wach");
    }

    if (s_wake_pin >= 0) {
        err = esp_sleep_enable_ext0_wakeup((gpio_num_t)s_wake_pin, 0);  /* low weckt */
        if (err != ESP_OK) {
            ESP_LOGW(TAG, "Weckpin IO%d nicht gesetzt (%s)", s_wake_pin,
                     esp_err_to_name(err));
            s_wake_pin = -1;
        } else {
            ESP_LOGI(TAG, "Wecken per IO%d (Low-Pegel) - %s", s_wake_pin,
                     s_irq_usable ? "Touch" : "BOOT-Taste");
        }
    }

#if POWER_LIGHT_SLEEP
    esp_pm_config_t pm = {
        .max_freq_mhz = POWER_CPU_MAX_MHZ,
        .min_freq_mhz = POWER_CPU_MIN_MHZ,
        .light_sleep_enable = true,
    };
    err = esp_pm_configure(&pm);
    if (err == ESP_OK) {
        s_light_sleep = true;
        ESP_LOGI(TAG, "Light-Sleep aktiv (%d..%d MHz)", POWER_CPU_MIN_MHZ,
                 POWER_CPU_MAX_MHZ);
    } else {
        ESP_LOGW(TAG, "Light-Sleep nicht verfuegbar (%s) - es bleibt bei "
                      "\"nur Anzeige aus\"", esp_err_to_name(err));
    }

    /* Die Konsole muss auch im Light-Sleep erreichbar bleiben. Ohne diese
     * Zeile kommen eingehende Zeichen nicht an (die CPU schlaeft und wacht
     * nicht auf) - dann laesst sich nicht einmal "sim on" eintippen.
     *
     * Der Schwellwert bestimmt, nach wie vielen Zeichen geweckt wird; diese
     * Zeichen sind verloren, weil die UART im Schlaf nicht mitliest. Der Wert
     * 3 ist das Minimum, das die IDF zulaesst - kleiner geht nicht
     * ("wakeup_threshold out of bounds", am Geraet geprueft). Bei einer
     * Tastatur faellt der Verlust nicht auf, weil die Zeichen einzeln und
     * langsam kommen. Ein Skript oder eine Einfuege-Operation schickt die
     * ganze Zeile in unter zwei Millisekunden - dort muss zuerst ein Enter
     * als "Weckopfer" geschickt werden (macht tools/hole_dateien.ps1). */
    err = esp_sleep_enable_uart_wakeup(0);      /* Konsole = UART0 */
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "UART-Weckung nicht gesetzt (%s)", esp_err_to_name(err));
    }
    uart_set_wakeup_threshold(0, 3);
#endif

    if (xTaskCreatePinnedToCore(power_task, "power", POWER_TASK_STACK, NULL,
                                POWER_TASK_PRIO, NULL, TASK_CORE_DISPLAY) != pdPASS) {
        ESP_LOGW(TAG, "Power-Task konnte nicht gestartet werden");
    }

    /* Beim Start bleibt die Anzeige erst einmal an (Boot-Meldungen lesbar) */
    s_until_us = esp_timer_get_time() + (int64_t)POWER_DISPLAY_ON_MS * 1000;
    ESP_LOGI(TAG, "Anzeige-Haltezeit %d s, Tasterabfrage alle %d ms",
             POWER_DISPLAY_ON_MS / 1000, POWER_POLL_MS);
}

void power_set_wake_callback(void (*cb)(void))
{
    s_wake_cb = cb;
}

bool power_activity(void)
{
    s_activity++;
    s_until_us = esp_timer_get_time() + (int64_t)POWER_DISPLAY_ON_MS * 1000;
    if (!s_on) {
        display_ein();                 /* weckt und zeichnet dabei neu */
        return true;
    }
    return false;
}

bool power_display_on(void)
{
    return s_on;
}

bool power_light_sleep_ready(void)
{
    return s_light_sleep;
}

void power_force(bool on)
{
    if (on == s_on) {
        return;
    }
    if (on) {
        s_until_us = esp_timer_get_time() + (int64_t)POWER_DISPLAY_ON_MS * 1000;
        display_ein();
    } else {
        display_aus();
        s_until_us = 0;                    /* bleibt aus, bis ein Ereignis kommt */
    }
}

void power_print_status(void)
{
    int64_t rest = s_until_us - esp_timer_get_time();
    printf("Anzeige      : %s\n", s_on ? "an" : "aus");
    if (s_on && rest > 0) {
        printf("Noch an      : %d s\n", (int)(rest / 1000000));
    }
    printf("Haltezeit    : %d s nach Ereignis\n", POWER_DISPLAY_ON_MS / 1000);
    printf("Light-Sleep  : %s\n", s_light_sleep ? "aktiv" : "aus");
    printf("Wecker       : %s\n",
           (s_wake_pin < 0) ? "keiner (CPU bleibt wach)"
                            : (s_irq_usable ? "Touch (IO36)" : "BOOT-Taste (IO0)"));
    printf("Touch-IRQ    : IO%d, Pegel %d (%s)\n", TOUCH_IRQ,
           gpio_get_level(TOUCH_IRQ),
           s_irq_usable ? "als Wecker nutzbar" : "kein Wecker, siehe Log");
    printf("BOOT-Taste   : IO%d, Pegel %d\n", POWER_BUTTON_GPIO,
           gpio_get_level(POWER_BUTTON_GPIO));
    printf("Ereignisse   : %u seit dem Start\n", (unsigned)s_activity);
}
