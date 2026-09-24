/*
 * display.h - CYD-Display (ILI9341, 320x240) - API
 *
 * Roh-SPI-Treiber (kein LVGL). Pin-Belegung/Init siehe config.h (TFT_*)
 * und display.c. Farben als RGB565.
 */

#pragma once

#include <stdint.h>
#include <stdbool.h>
#include "esp_err.h"

/* Initialisiert SPI-Bus + ILI9341-Panel + Backlight-GPIO */
esp_err_t display_init(void);

/* Backlight an/aus */
void display_backlight(bool on);

/* Panel in den Sleep schicken bzw. aufwecken (SLPIN/SLPOUT).
 * Der Bildspeicher bleibt dabei erhalten, nach dem Aufwecken wird die
 * Anzeige aber sicherheitshalber neu gezeichnet. */
void display_power(bool on);

/* Gesamten Bildschirm mit Farbe (RGB565) fuellen */
void display_fill(uint16_t color);

/* Text (5x7) zeichnen, color/bg als RGB565 */
void display_draw_text(int x, int y, const char *text, uint16_t color, uint16_t bg);

/* Text skaliert (scale 1..4) - fuer grosse Anzeigen (z.B. Tag-ID) */
void display_draw_text_scaled(int x, int y, const char *text, int scale,
                              uint16_t color, uint16_t bg);

/* Rechtecke (RGB565) */
void display_draw_rect(int x, int y, int w, int h, uint16_t color);
void display_draw_filled_rect(int x, int y, int w, int h, uint16_t color);

/* Linie mit Breite (scanline-gefuellt) */
void display_draw_line(int x0, int y0, int x1, int y1, int width, uint16_t color);

/* Diagnose-Selbsttest: Rot -> Gruen -> Blau -> Schwarz (je ~0,8 s) */
void display_test_pattern(void);
