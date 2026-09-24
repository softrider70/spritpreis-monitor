/*
 * display.c - CYD-Display (ILI9341, 320x240), eigener roher SPI-Treiber
 *
 * Pin-Belegung und Init-Sequenz stammen aus dem verifizierten Projekt
 * autofrontcam/cyd (cyd-display-car1): CLK=14, MOSI=13, CS=15, DC=2,
 * RST=-1 (haengt am ESP32-Reset), BL=21; MADCTL 0x40; vollstaendige
 * ILI9341-Init-Sequenz. Panel ist ein 320x240-Nativcontroller (ST7796-artig),
 * KEIN 240x320 - daher MADCTL 0x40 und TFT_WIDTH=320.
 *
 * ESP-IDF 6.x hat keinen ILI9341-Treiber mehr, daher Ansteuerung ueber
 * spi_master: Kommandos mit DC=0 (Einzeltransfer), Pixeldaten mit DC=1
 * (RAMWR 0x2C). Farbdaten RGB565 BIG-ENDIAN.
 *
 * Eingesetzt fuer den RFID-Auslese-Test auf dem CYD (Tag-IDs fortlaufend
 * anzeigen).
 */

#include <string.h>
#include <stdlib.h>
#include <math.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
#include "driver/spi_master.h"
#include "driver/gpio.h"
#include "esp_log.h"
#include "esp_heap_caps.h"
#include "esp_check.h"
#include "esp_system.h"
#include "config.h"
#include "display.h"

static const char *TAG = "display";

static spi_device_handle_t s_spi = NULL;
static SemaphoreHandle_t s_lcd_mutex = NULL;   /* schuetzt SPI-Zugriffe */

/* RGB565 -> Big-Endian-Speicherformat fuer das Display */
static inline uint16_t be16(uint16_t c) { return (uint16_t)((c << 8) | (c >> 8)); }

/* ------------------------------------------------------------------ */
/* ILI9341-Kommandos (roher SPI-Treiber)                              */
/* ------------------------------------------------------------------ */
static void lcd_write(bool is_cmd, const void *data, size_t len)
{
    if (len == 0) return;
    if (s_lcd_mutex) xSemaphoreTake(s_lcd_mutex, portMAX_DELAY);
    gpio_set_level(TFT_DC, is_cmd ? 0 : 1);
    spi_transaction_t t = {
        .length = (int)(len * 8),
        .tx_buffer = data,
    };
    esp_err_t err = spi_device_transmit(s_spi, &t);
    if (s_lcd_mutex) xSemaphoreGive(s_lcd_mutex);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "SPI transmit (%s, %d B) fehlgeschlagen: %s",
                 is_cmd ? "cmd" : "data", (int)len, esp_err_to_name(err));
    }
}

static void lcd_cmd(uint8_t cmd)
{
    lcd_write(true, &cmd, 1);
}

static void lcd_cmd_data(uint8_t cmd, const uint8_t *data, size_t len)
{
    lcd_write(true, &cmd, 1);
    if (len) lcd_write(false, data, len);
}

/* Adressfenster setzen (CASET/PASET/RAMWR) */
static void lcd_set_window(int x0, int y0, int x1, int y1)
{
    uint8_t d[4];
    d[0] = (uint8_t)(x0 >> 8); d[1] = (uint8_t)x0;
    d[2] = (uint8_t)(x1 >> 8); d[3] = (uint8_t)x1;
    lcd_cmd(0x2A);                 /* CASET */
    lcd_write(false, d, 4);
    d[0] = (uint8_t)(y0 >> 8); d[1] = (uint8_t)y0;
    d[2] = (uint8_t)(y1 >> 8); d[3] = (uint8_t)y1;
    lcd_cmd(0x2B);                 /* PASET */
    lcd_write(false, d, 4);
    lcd_cmd(0x2C);                 /* RAMWR */
}

/* Pixeldaten schreiben (DC=1) */
static void lcd_draw_bitmap(const void *data, size_t len)
{
    lcd_write(false, data, len);
}

/* Schnelle Rechteck-Fuellung: Fenster einmal setzen, dann in grossen
 * Bloecken senden (statt einer Transaktion pro Zeile). */
static void lcd_fill_rect_fast(int x, int y, int w, int h, uint16_t color)
{
    if (w <= 0 || h <= 0 || !s_spi) return;
    if (x < 0) { w += x; x = 0; }
    if (y < 0) { h += y; y = 0; }
    if (w <= 0 || h <= 0) return;
    if (x + w > TFT_WIDTH)  w = TFT_WIDTH - x;
    if (y + h > TFT_HEIGHT) h = TFT_HEIGHT - y;
    if (w <= 0 || h <= 0) return;

    const int rows = 16;
    size_t npx = (size_t)w * rows;
    uint16_t *buf = heap_caps_malloc(npx * 2, MALLOC_CAP_DMA);
    if (!buf) return;
    uint16_t c = be16(color);
    for (size_t i = 0; i < npx; i++) buf[i] = c;

    lcd_set_window(x, y, x + w - 1, y + h - 1);
    while (h > 0) {
        int n = (h < rows) ? h : rows;
        lcd_draw_bitmap(buf, (size_t)w * n * 2);
        h -= n;
    }
    heap_caps_free(buf);
}

/* Synchrones SPI: keine ausstehenden Transfers -> kein Flush noetig */
static void lcd_flush(void) { }

static void ili9341_init_panel(void)
{
    /* Kein eigener Reset-Pin (TFT_RST = -1): Panel resettet mit dem ESP32. */
    vTaskDelay(pdMS_TO_TICKS(150));
    lcd_cmd(0x01);                 /* SWRESET */
    vTaskDelay(pdMS_TO_TICKS(120));
    lcd_cmd(0x11);                 /* SLPOUT */
    vTaskDelay(pdMS_TO_TICKS(120));

    /* Vollstaendige ILI9341-Init-Sequenz (verifiziert in autofrontcam/cyd) */
    static const uint8_t init_seq[] = {
        0xEF, 3, 0x03, 0x80, 0x02,
        0xCF, 3, 0x00, 0xC1, 0x30,
        0xED, 4, 0x64, 0x03, 0x12, 0x81,
        0xE8, 3, 0x85, 0x00, 0x78,
        0xCB, 5, 0x39, 0x2C, 0x00, 0x34, 0x02,
        0xF7, 1, 0x20,
        0xEA, 2, 0x00, 0x00,
        0xC0, 1, 0x23,
        0xC1, 1, 0x11,
        0xC5, 2, 0x27, 0x2B,
        0xC7, 1, 0x1E,
        0x36, 1, ILI9341_MADCTL,
        0x3A, 1, 0x55,
        0xB1, 2, 0x00, 0x1B,
        0xB6, 3, 0x08, 0x82, 0x27,
        0xF2, 1, 0x00,
        0x26, 1, 0x01,
        0xE0, 15, 0x0F,0x31,0x2B,0x0C,0x0E,0x06,0x38,0x0F,0x44,0x49,0x09,0x06,0x15,0x12,0x0C,
        0xE1, 15, 0x00,0x0E,0x14,0x03,0x11,0x07,0x31,0xC1,0x48,0x37,0x06,0x09,0x0A,0x13,0x13,
        0x11, 0,
        0x29, 0,
    };
    size_t i = 0;
    while (i < sizeof(init_seq)) {
        uint8_t cmd = init_seq[i++];
        uint8_t cnt = init_seq[i++];
        lcd_cmd_data(cmd, &init_seq[i], cnt);
        i += cnt;
        vTaskDelay(pdMS_TO_TICKS(5));
    }

#if CYD_INVERT_COLOR
    lcd_cmd(0x21);                 /* INVON */
#endif
}

/* ------------------------------------------------------------------ */
/* Oeffentliche API                                                    */
/* ------------------------------------------------------------------ */
esp_err_t display_init(void)
{
    if (s_spi) return ESP_OK;      /* bereits initialisiert */

    s_lcd_mutex = xSemaphoreCreateMutex();

    /* BL und DC als GPIO konfigurieren (CS uebernimmt der SPI-Treiber) */
    gpio_config_t io = {
        .pin_bit_mask = (1ULL << TFT_BL) | (1ULL << TFT_DC),
        .mode = GPIO_MODE_OUTPUT,
        .pull_up_en = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    ESP_RETURN_ON_ERROR(gpio_config(&io), TAG, "GPIO-Konfiguration fehlgeschlagen");
    gpio_set_level(TFT_BL, !TFT_BL_ON);
    gpio_set_level(TFT_DC, 1);

    spi_bus_config_t buscfg = {
        .sclk_io_num = TFT_SCK,
        .mosi_io_num = TFT_MOSI,
        .miso_io_num = TFT_MISO,
        .quadwp_io_num = -1,
        .quadhd_io_num = -1,
        .max_transfer_sz = TFT_WIDTH * 2 * 16,
    };
    ESP_RETURN_ON_ERROR(spi_bus_initialize(TFT_SPI_HOST, &buscfg, SPI_DMA_CH_AUTO),
                        TAG, "SPI-Bus init fehlgeschlagen");

    spi_device_interface_config_t dev_cfg = {
        .clock_speed_hz = 40 * 1000 * 1000,   /* 40 MHz (verifiziert) */
        .mode = 0,
        .spics_io_num = TFT_CS,
        .queue_size = 7,
    };
    ESP_RETURN_ON_ERROR(spi_bus_add_device(TFT_SPI_HOST, &dev_cfg, &s_spi),
                        TAG, "SPI-Geraet fehlgeschlagen");

    ili9341_init_panel();
    ESP_LOGI(TAG, "ILI9341 initialisiert (%dx%d), freier Heap: %lu B",
             TFT_WIDTH, TFT_HEIGHT, (unsigned long)esp_get_free_heap_size());
    display_fill(0x0000);
    return ESP_OK;
}

void display_backlight(bool on)
{
    gpio_set_level(TFT_BL, on ? TFT_BL_ON : !TFT_BL_ON);
}

void display_power(bool on)
{
    if (!s_spi) return;
    if (on) {
        lcd_cmd(0x11);                     /* SLPOUT */
        vTaskDelay(pdMS_TO_TICKS(120));    /* Datenblatt: 120 ms warten */
        lcd_cmd(0x29);                     /* DISPON */
    } else {
        lcd_cmd(0x28);                     /* DISPOFF */
        lcd_cmd(0x10);                     /* SLPIN */
    }
}

void display_fill(uint16_t color)
{
    if (!s_spi) return;
    uint16_t *row = heap_caps_malloc(TFT_WIDTH * 2, MALLOC_CAP_DMA);
    if (!row) {
        ESP_LOGE(TAG, "display_fill: kein DMA-Puffer (Heap %lu)",
                 (unsigned long)esp_get_free_heap_size());
        return;
    }
    uint16_t c = be16(color);
    for (int x = 0; x < TFT_WIDTH; x++) row[x] = c;

    lcd_set_window(0, 0, TFT_WIDTH - 1, TFT_HEIGHT - 1);
    for (int y = 0; y < TFT_HEIGHT; y++) {
        lcd_draw_bitmap(row, TFT_WIDTH * 2);
    }
    lcd_flush();
    heap_caps_free(row);
}

/* ------------------------------------------------------------------ */
/* 5x7-Font (Public Domain)                                            */
/* ------------------------------------------------------------------ */
static const uint8_t font5x7[96][5] = {
    {0x00,0x00,0x00,0x00,0x00}, /*   */
    {0x00,0x00,0x5F,0x00,0x00}, /* ! */
    {0x00,0x07,0x00,0x07,0x00}, /* " */
    {0x14,0x7F,0x14,0x7F,0x14}, /* # */
    {0x24,0x2A,0x7F,0x2A,0x12}, /* $ */
    {0x23,0x13,0x08,0x64,0x62}, /* % */
    {0x36,0x49,0x55,0x22,0x50}, /* & */
    {0x00,0x05,0x03,0x00,0x00}, /* ' */
    {0x00,0x1C,0x22,0x41,0x00}, /* ( */
    {0x00,0x41,0x22,0x1C,0x00}, /* ) */
    {0x14,0x08,0x3E,0x08,0x14}, /* * */
    {0x08,0x08,0x3E,0x08,0x08}, /* + */
    {0x00,0x50,0x30,0x00,0x00}, /* , */
    {0x08,0x08,0x08,0x08,0x08}, /* - */
    {0x00,0x60,0x60,0x00,0x00}, /* . */
    {0x20,0x10,0x08,0x04,0x02}, /* / */
    {0x3E,0x51,0x49,0x45,0x3E}, /* 0 */
    {0x00,0x42,0x7F,0x40,0x00}, /* 1 */
    {0x42,0x61,0x51,0x49,0x46}, /* 2 */
    {0x21,0x41,0x45,0x4B,0x31}, /* 3 */
    {0x18,0x14,0x12,0x7F,0x10}, /* 4 */
    {0x27,0x45,0x45,0x45,0x39}, /* 5 */
    {0x3C,0x4A,0x49,0x49,0x30}, /* 6 */
    {0x01,0x71,0x09,0x05,0x03}, /* 7 */
    {0x36,0x49,0x49,0x49,0x36}, /* 8 */
    {0x06,0x49,0x49,0x29,0x1E}, /* 9 */
    {0x00,0x36,0x36,0x00,0x00}, /* : */
    {0x00,0x56,0x36,0x00,0x00}, /* ; */
    {0x08,0x14,0x22,0x41,0x00}, /* < */
    {0x14,0x14,0x14,0x14,0x14}, /* = */
    {0x00,0x41,0x22,0x14,0x08}, /* > */
    {0x02,0x01,0x51,0x09,0x06}, /* ? */
    {0x32,0x49,0x79,0x41,0x3E}, /* @ */
    {0x7E,0x11,0x11,0x11,0x7E}, /* A */
    {0x7F,0x49,0x49,0x49,0x36}, /* B */
    {0x3E,0x41,0x41,0x41,0x22}, /* C */
    {0x7F,0x41,0x41,0x22,0x1C}, /* D */
    {0x7F,0x49,0x49,0x49,0x41}, /* E */
    {0x7F,0x09,0x09,0x09,0x01}, /* F */
    {0x3E,0x41,0x49,0x49,0x7A}, /* G */
    {0x7F,0x08,0x08,0x08,0x7F}, /* H */
    {0x00,0x41,0x7F,0x41,0x00}, /* I */
    {0x20,0x40,0x41,0x3F,0x01}, /* J */
    {0x7F,0x08,0x14,0x22,0x41}, /* K */
    {0x7F,0x40,0x40,0x40,0x40}, /* L */
    {0x7F,0x02,0x0C,0x02,0x7F}, /* M */
    {0x7F,0x04,0x08,0x10,0x7F}, /* N */
    {0x3E,0x41,0x41,0x41,0x3E}, /* O */
    {0x7F,0x09,0x09,0x09,0x06}, /* P */
    {0x3E,0x41,0x51,0x21,0x5E}, /* Q */
    {0x7F,0x09,0x19,0x29,0x46}, /* R */
    {0x46,0x49,0x49,0x49,0x31}, /* S */
    {0x01,0x01,0x7F,0x01,0x01}, /* T */
    {0x3F,0x40,0x40,0x40,0x3F}, /* U */
    {0x1F,0x20,0x40,0x20,0x1F}, /* V */
    {0x3F,0x40,0x38,0x40,0x3F}, /* W */
    {0x63,0x14,0x08,0x14,0x63}, /* X */
    {0x07,0x08,0x70,0x08,0x07}, /* Y */
    {0x61,0x51,0x49,0x45,0x43}, /* Z */
    {0x00,0x7F,0x41,0x41,0x00}, /* [ */
    {0x02,0x04,0x08,0x10,0x20}, /* \ */
    {0x00,0x41,0x41,0x7F,0x00}, /* ] */
    {0x04,0x02,0x01,0x02,0x04}, /* ^ */
    {0x40,0x40,0x40,0x40,0x40}, /* _ */
    {0x00,0x01,0x02,0x04,0x00}, /* ` */
    {0x20,0x54,0x54,0x54,0x78}, /* a */
    {0x7F,0x48,0x44,0x44,0x38}, /* b */
    {0x38,0x44,0x44,0x44,0x20}, /* c */
    {0x38,0x44,0x44,0x48,0x7F}, /* d */
    {0x38,0x54,0x54,0x54,0x18}, /* e */
    {0x08,0x7E,0x09,0x01,0x02}, /* f */
    {0x0C,0x52,0x52,0x52,0x3E}, /* g */
    {0x7F,0x08,0x04,0x04,0x78}, /* h */
    {0x00,0x44,0x7D,0x40,0x00}, /* i */
    {0x20,0x40,0x44,0x3D,0x00}, /* j */
    {0x7F,0x10,0x28,0x44,0x00}, /* k */
    {0x00,0x41,0x7F,0x40,0x00}, /* l */
    {0x7C,0x04,0x18,0x04,0x78}, /* m */
    {0x7C,0x08,0x04,0x04,0x78}, /* n */
    {0x38,0x44,0x44,0x44,0x38}, /* o */
    {0x7C,0x14,0x14,0x14,0x08}, /* p */
    {0x08,0x14,0x14,0x18,0x7C}, /* q */
    {0x7C,0x08,0x04,0x04,0x08}, /* r */
    {0x48,0x54,0x54,0x54,0x20}, /* s */
    {0x04,0x3F,0x44,0x40,0x20}, /* t */
    {0x3C,0x40,0x40,0x20,0x7C}, /* u */
    {0x1C,0x20,0x40,0x20,0x1C}, /* v */
    {0x3C,0x40,0x30,0x40,0x3C}, /* w */
    {0x44,0x28,0x10,0x28,0x44}, /* x */
    {0x0C,0x50,0x50,0x50,0x3C}, /* y */
    {0x44,0x64,0x54,0x4C,0x44}, /* z */
    {0x00,0x08,0x36,0x41,0x00}, /* { */
    {0x00,0x00,0x7F,0x00,0x00}, /* | */
    {0x00,0x41,0x36,0x08,0x00}, /* } */
    {0x10,0x08,0x08,0x10,0x08}, /* ~ */
    {0x00,0x06,0x09,0x09,0x06}, /* DEL */
};

void display_draw_text(int x, int y, const char *text, uint16_t color, uint16_t bg)
{
    if (!s_spi || !text) return;

    int len = (int)strlen(text);
    int total_w = len * 6;   /* 5px Glyphe + 1px Abstand */
    if (total_w <= 0) return;

    uint16_t *row = heap_caps_malloc((size_t)total_w * 2, MALLOC_CAP_DMA);
    if (!row) {
        ESP_LOGE(TAG, "display_draw_text: kein DMA-Puffer");
        return;
    }

    uint16_t col_be = be16(color);
    uint16_t bg_be  = be16(bg);

    for (int ry = 0; ry < 7; ry++) {
        for (int i = 0; i < len; i++) {
            unsigned char ch = (unsigned char)text[i];
            if (ch < 32 || ch > 126) ch = '?';
            for (int c = 0; c < 5; c++) {
                uint8_t col = font5x7[ch - 32][c];
                row[i * 6 + c] = ((col >> ry) & 1) ? col_be : bg_be;
            }
            row[i * 6 + 5] = bg_be;
        }
        int yy = y + ry;
        if (yy < 0 || yy >= TFT_HEIGHT) continue;
        int xx0 = (x < 0) ? 0 : x;
        int ww = total_w;
        if (xx0 + ww > TFT_WIDTH) ww = TFT_WIDTH - xx0;
        if (ww <= 0) continue;
        lcd_set_window(xx0, yy, xx0 + ww - 1, yy);
        lcd_draw_bitmap(row + (xx0 - x), (size_t)ww * 2);
    }
    lcd_flush();
    heap_caps_free(row);
}

void display_draw_text_scaled(int x, int y, const char *text, int scale,
                              uint16_t color, uint16_t bg)
{
    if (!s_spi || !text || scale < 1) return;
    if (scale > 4) scale = 4;

    int len = (int)strlen(text);
    int total_w = len * 6 * scale;
    if (total_w <= 0) return;

    uint16_t *row = heap_caps_malloc((size_t)total_w * 2, MALLOC_CAP_DMA);
    if (!row) {
        ESP_LOGE(TAG, "display_draw_text_scaled: kein DMA-Puffer");
        return;
    }

    uint16_t col_be = be16(color);
    uint16_t bg_be  = be16(bg);

    for (int ry = 0; ry < 7; ry++) {
        /* Eine Font-Zeile inkl. Skalierung in den Zeilenpuffer bauen */
        for (int i = 0; i < len; i++) {
            unsigned char ch = (unsigned char)text[i];
            if (ch < 32 || ch > 126) ch = '?';
            for (int c = 0; c < 5; c++) {
                uint8_t col = font5x7[ch - 32][c];
                uint16_t px = ((col >> ry) & 1) ? col_be : bg_be;
                for (int s = 0; s < scale; s++) {
                    row[(i * 6 * scale) + (c * scale) + s] = px;
                }
            }
            for (int s = 0; s < scale; s++) {
                row[(i * 6 * scale) + (5 * scale) + s] = bg_be;
            }
        }
        for (int s = 0; s < scale; s++) {
            int yy = y + ry * scale + s;
            if (yy < 0 || yy >= TFT_HEIGHT) continue;
            int xx0 = (x < 0) ? 0 : x;
            int ww = total_w;
            if (xx0 + ww > TFT_WIDTH) ww = TFT_WIDTH - xx0;
            if (ww <= 0) continue;
            lcd_set_window(xx0, yy, xx0 + ww - 1, yy);
            lcd_draw_bitmap(row + (xx0 - x), (size_t)ww * 2);
        }
    }
    lcd_flush();
    heap_caps_free(row);
}

void display_draw_filled_rect(int x, int y, int w, int h, uint16_t color)
{
    if (!s_spi) return;
    lcd_fill_rect_fast(x, y, w, h, color);
    lcd_flush();
}

void display_draw_rect(int x, int y, int w, int h, uint16_t color)
{
    display_draw_filled_rect(x, y, w, 1, color);
    display_draw_filled_rect(x, y + h - 1, w, 1, color);
    display_draw_filled_rect(x, y, 1, h, color);
    display_draw_filled_rect(x + w - 1, y, 1, h, color);
}

void display_draw_line(int x0, int y0, int x1, int y1, int width, uint16_t color)
{
    if (!s_spi) return;
    if (width < 1) width = 1;

    float dxv = (float)(x1 - x0), dyv = (float)(y1 - y0);
    float len = sqrtf(dxv * dxv + dyv * dyv);
    if (len < 0.5f) return;
    float nx = -dyv / len;
    float ny =  dxv / len;
    float half = (float)width / 2.0f;

    float px[4], py[4];
    px[0] = x0 + nx * half;  py[0] = y0 + ny * half;
    px[1] = x1 + nx * half;  py[1] = y1 + ny * half;
    px[2] = x1 - nx * half;  py[2] = y1 - ny * half;
    px[3] = x0 - nx * half;  py[3] = y0 - ny * half;

    int ymin = (int)floorf(fminf(fminf(py[0], py[1]), fminf(py[2], py[3])));
    int ymax = (int)ceilf(fmaxf(fmaxf(py[0], py[1]), fmaxf(py[2], py[3])));
    if (ymin < 0) ymin = 0;
    if (ymax > TFT_HEIGHT - 1) ymax = TFT_HEIGHT - 1;
    if (ymin > ymax) return;

    uint16_t *row = heap_caps_malloc((size_t)TFT_WIDTH * 2, MALLOC_CAP_DMA);
    if (!row) return;
    uint16_t c = be16(color);

    for (int y = ymin; y <= ymax; y++) {
        float fy = (float)y;
        float xmin = 1e9f, xmax = -1e9f;
        for (int e = 0; e < 4; e++) {
            int f = (e + 1) & 3;
            float ax = px[e], ay = py[e], bx = px[f], by = py[f];
            if ((ay <= fy && by > fy) || (by <= fy && ay > fy)) {
                float t = (fy - ay) / (by - ay);
                float x = ax + t * (bx - ax);
                if (x < xmin) xmin = x;
                if (x > xmax) xmax = x;
            }
        }
        if (xmax < xmin) continue;
        int xi0 = (int)ceilf(xmin - 0.5f);
        int xi1 = (int)floorf(xmax + 0.5f);
        if (xi0 < 0) xi0 = 0;
        if (xi1 > TFT_WIDTH - 1) xi1 = TFT_WIDTH - 1;
        if (xi1 < xi0) continue;
        int n = xi1 - xi0 + 1;
        for (int i = 0; i < n; i++) row[xi0 + i] = c;
        lcd_set_window(xi0, y, xi1, y);
        lcd_draw_bitmap(row + xi0, (size_t)n * 2);
    }
    lcd_flush();
    heap_caps_free(row);
}

void display_test_pattern(void)
{
    static const uint16_t colors[] = { 0xF800, 0x07E0, 0x001F, 0x0000 };
    for (size_t i = 0; i < 4; i++) {
        display_fill(colors[i]);
        vTaskDelay(pdMS_TO_TICKS(800));
    }
    display_fill(0x0000);
}
