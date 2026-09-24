/*
 * touch.c - XPT2046 resistiver Touch (CYD) per Software-Bitbang
 *
 * Belegung verifiziert aus autofrontcam/cyd (cyd-display-car1):
 * MOSI=32, MISO=39, CLK=25, CS=33, IRQ=36.
 *
 * Warum Software statt Hardware-SPI?
 *   Der CYD verdrahtet Display, Touch und microSD auf drei Geraete, der
 *   ESP32 hat aber nur zwei nutzbare SPI-Peripherien (SPI2/HSPI und
 *   SPI3/VSPI). Der Touch haengt auf den Pins des VSPI - und genau diesen
 *   Bus braucht die SD-Karte fuer das Langzeitarchiv (sd_archive.c). Zwei
 *   Pin-Saetze lassen sich nicht gleichzeitig auf einen Hardware-Bus legen,
 *   deshalb wird der Touch hier getaktet (Bitbang). Er braucht wenig
 *   Bandbreite: 13 Bit je Kanal bei rund 250 kHz, also unter 0,1 ms.
 *
 * Die Rohwerte (12 bit) werden linear auf die Display-Koordinaten (320x240)
 * abgebildet; bei Bedarf kann per Kalibrier-Modus die Umrechnung
 * nachgemessen werden.
 */

#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "driver/gpio.h"
#include "esp_rom_sys.h"
#include "esp_log.h"
#include "esp_check.h"
#include "config.h"
#include "power.h"
#include "touch.h"

static const char *TAG = "touch";

/* XPT2046 Kanal-Kommandos */
#define XPT_CMD_Z1   0xB0
#define XPT_CMD_Z2   0xC0
#define XPT_CMD_X    0x90
#define XPT_CMD_Y    0xD0
#define XPT_CMD_PWD  0x80

#define TOUCH_STACK      4096
#define TOUCH_PRIO       7
#define TOUCH_PRESSURE   300      /* Druckschwelle (Rauschen ignorieren) */
#define TOUCH_TAP_WEG    18       /* bis hierhin gilt es als Tipp, nicht als Ziehen */
#define TOUCH_TAP_MAX_MS 1500     /* laenger gedrueckt = kein Tipp mehr */

/* Takt des Touch-Tasks. Am Geraet gemessen liegt der Touch-IRQ (IO36) im
 * Ruhezustand auf low - IO36 ist ein reiner Eingang ohne internen Pull-up,
 * der Ausgang des XPT2046 haengt in der Luft. Ein Interrupt kann damit nicht
 * ausgeloest werden, und ein Wecker waere er auch nicht.
 * Deshalb wird der Touch immer im Takt abgefragt. Gemeldet werden die
 * Ereignisse aber nur bei eingeschalteter Anzeige - der Touch weckt das
 * Display also nicht, sondern bedient nur, was sichtbar ist. */
#define TOUCH_TAKT_AN_MS    30
#define TOUCH_TAKT_AUS_MS   50      /* am Geraet: 100 ms verpasste kurze Tipps */

/* Halbe Taktzeit in Mikrosekunden: 2 us -> rund 250 kHz. Der XPT2046
 * vertraegt bis 2 MHz; langsam ist hier unkritisch und robuster. */
#define BB_HALF_US       2

static bool s_ok = false;
static bool s_calib = false;
static touch_cb_t s_cb = NULL;
static touch_press_cb_t s_press_cb = NULL;   /* optional: Bewegungs-/Loseereignisse */
static void *s_cb_arg = NULL;
static int s_last_x = 0;
static int s_last_y = 0;
static TaskHandle_t s_task = NULL;

/* Wegtoleranz: erst beim Loslassen entscheidet sich, ob es ein Tipp war. */
static int s_weg = 0;
static TickType_t s_down_tick = 0;

/* Der Touch-IRQ wird NICHT als Interrupt benutzt: Der Pin haengt ohne
 * Pull-up in der Luft und liefert keinen brauchbaren Pegelwechsel. Er wird
 * nur als Weckpin des Light-Sleep geprueft (siehe power.c). */

void touch_set_calib_mode(bool on)
{
    s_calib = on;
    ESP_LOGI(TAG, "Kalibrier-Modus %s", on ? "EIN (Rohwerte werden geloggt)" : "AUS");
}

static inline void sclk(bool level)
{
    gpio_set_level(TOUCH_SCLK, level ? 1 : 0);
}

/*
 * Einen XPT2046-Kanal lesen (SPI-Modus 0, MSB zuerst):
 *   1. CS auf low, kurze Pause
 *   2. 8 Bit Kommando senden (MOSI wird vor der steigenden Flanke gesetzt)
 *   3. 1 Nullbit + 12 Datenbits einlesen
 *   4. CS auf high - beendet die Wandlung und setzt den Chip zurueck
 */
static uint16_t read_channel(uint8_t cmd)
{
    gpio_set_level(TOUCH_CS, 0);
    esp_rom_delay_us(2);

    for (int i = 7; i >= 0; i--) {
        gpio_set_level(TOUCH_MOSI, (cmd >> i) & 1);
        esp_rom_delay_us(BB_HALF_US);
        sclk(true);
        esp_rom_delay_us(BB_HALF_US);
        sclk(false);
    }
    gpio_set_level(TOUCH_MOSI, 0);

    uint16_t bits = 0;
    for (int i = 0; i < 13; i++) {          /* 1 Nullbit + 12 Nutzbits */
        esp_rom_delay_us(BB_HALF_US);
        sclk(true);
        esp_rom_delay_us(BB_HALF_US);
        bits = (uint16_t)((bits << 1) | (gpio_get_level(TOUCH_MISO) ? 1u : 0u));
        sclk(false);
    }

    esp_rom_delay_us(2);
    gpio_set_level(TOUCH_CS, 1);
    esp_rom_delay_us(BB_HALF_US);

    return (uint16_t)(bits & 0x0FFF);
}

esp_err_t touch_init(void)
{
    if (s_ok) return ESP_OK;

    /* Ausgaenge: MOSI, Takt und CS - CS ruht auf high */
    gpio_config_t out = {
        .pin_bit_mask = (1ULL << TOUCH_MOSI) | (1ULL << TOUCH_SCLK) | (1ULL << TOUCH_CS),
        .mode = GPIO_MODE_OUTPUT,
        .pull_up_en = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    ESP_RETURN_ON_ERROR(gpio_config(&out), TAG, "Touch-Ausgangs-GPIO");

    /* Eingaenge: MISO (Antwort des Chips) und IRQ (Beruehrung) */
    gpio_config_t in = {
        .pin_bit_mask = (1ULL << TOUCH_MISO) | (1ULL << TOUCH_IRQ),
        .mode = GPIO_MODE_INPUT,
        .pull_up_en = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    ESP_RETURN_ON_ERROR(gpio_config(&in), TAG, "Touch-Eingangs-GPIO");

    gpio_set_level(TOUCH_CS, 1);
    sclk(false);

    uint16_t z1 = read_channel(XPT_CMD_Z1);
    read_channel(XPT_CMD_PWD);
    s_ok = true;
    ESP_LOGI(TAG, "XPT2046 initialisiert (Software-Bitbang, Z1-Diagnose: %u)",
             (unsigned)z1);
    return ESP_OK;
}

static void touch_task(void *arg)
{
    (void)arg;
    bool pressed = false;

    for (;;) {
        /* Der Touch bedient nur, was gerade sichtbar ist. Ist die Anzeige
         * aus, wird zwar weitergelesen (der Chip braucht keine Anlaufzeit),
         * aber es geht kein Ereignis nach oben - damit weckt der Touch die
         * Anzeige nicht mehr. Geweckt wird ueber die BOOT-Taste (power.c). */
        const bool an = power_display_on();

        uint16_t z1 = read_channel(XPT_CMD_Z1);
        uint16_t z2 = read_channel(XPT_CMD_Z2);
        int pressure = (z1 > 0) ? ((int)z1 - (int)z2 + 4095) : 0;
        if (pressure < 0) pressure = 0;

        if (pressure >= TOUCH_PRESSURE) {
            uint16_t x_raw = read_channel(XPT_CMD_X);
            uint16_t y_raw = read_channel(XPT_CMD_Y);
            read_channel(XPT_CMD_PWD);

            /* 12-bit -> Display-Koordinaten (Basis-Linear, ggf. kalibrieren) */
            int dx = (int)(((uint32_t)x_raw * TFT_WIDTH) / 4096);
            int dy = (int)(((uint32_t)y_raw * TFT_HEIGHT) / 4096);
            if (dx < 0) dx = 0;
            if (dx > TFT_WIDTH - 1) dx = TFT_WIDTH - 1;
            if (dy < 0) dy = 0;
            if (dy > TFT_HEIGHT - 1) dy = TFT_HEIGHT - 1;

            if (!pressed) {
                pressed = true;
                s_weg = 0;
                s_down_tick = xTaskGetTickCount();
                if (s_calib) {
                    ESP_LOGI(TAG, "KALIB: x_raw=%u y_raw=%u", (unsigned)x_raw, (unsigned)y_raw);
                }
            } else {
                /* Zurueckgelegten Weg aufsummieren: daraus entscheidet sich
                 * beim Loslassen, ob es ein Tipp oder ein Ziehen war. */
                int sx = dx - s_last_x, sy = dy - s_last_y;
                s_weg += (sx < 0 ? -sx : sx) + (sy < 0 ? -sy : sy);
            }
            s_last_x = dx;
            s_last_y = dy;
            /* Bewegungsereignis, solange gedrueckt wird (fuer Ziehen/Wischen) */
            if (an && s_press_cb) s_press_cb(dx, dy, true, s_cb_arg);
        } else if (pressed) {
            pressed = false;
            /* Ein Tipp wird erst beim Loslassen gemeldet - vorher ist nicht
             * zu wissen, ob daraus ein Ziehen wird. Sonst wuerde jedes
             * Verschieben zusaetzlich einen Tap ausloesen. */
            uint32_t dauer_ms = (uint32_t)((xTaskGetTickCount() - s_down_tick) * portTICK_PERIOD_MS);
            if (an && s_weg < TOUCH_TAP_WEG && dauer_ms < TOUCH_TAP_MAX_MS) {
                if (s_cb) s_cb(s_last_x, s_last_y, s_cb_arg);
            }
            if (an && s_press_cb) s_press_cb(s_last_x, s_last_y, false, s_cb_arg);
        }

        /* Im ausgeschalteten Zustand wird seltener abgefragt (Strom), aber
         * nicht so selten, dass ein kurzer Tipp verschwindet. */
        int takt = an ? TOUCH_TAKT_AN_MS : TOUCH_TAKT_AUS_MS;
        ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(takt));
    }
}

esp_err_t touch_task_start_full(touch_cb_t cb, touch_press_cb_t press_cb, void *arg)
{
    if (!s_ok) return ESP_ERR_INVALID_STATE;
    s_cb = cb;
    s_press_cb = press_cb;
    s_cb_arg = arg;
    if (xTaskCreatePinnedToCore(touch_task, "touch", TOUCH_STACK, NULL,
                                TOUCH_PRIO, &s_task, TASK_CORE_DISPLAY) != pdPASS) {
        return ESP_ERR_NO_MEM;
    }

    /* Der Touch-IRQ wird nicht als Interrupt genutzt (siehe oben) - hier nur
     * der Hinweis, falls er als Weckpin geprueft wird. */
    if (gpio_get_level(TOUCH_IRQ) != 1) {
        ESP_LOGI(TAG, "Touch-IRQ IO%d liegt auf low (kein Pull-up) - eine "
                      "Beruehrung weckt die CPU nicht, sie wird im Takt "
                      "abgefragt (%d ms bei ausgeschalteter Anzeige)",
                 TOUCH_IRQ, TOUCH_TAKT_AUS_MS);
    }

    ESP_LOGI(TAG, "Touch-Task gestartet (Takt %d ms an / %d ms aus)",
             TOUCH_TAKT_AN_MS, TOUCH_TAKT_AUS_MS);
    return ESP_OK;
}

esp_err_t touch_task_start(touch_cb_t cb, void *arg)
{
    return touch_task_start_full(cb, NULL, arg);
}
