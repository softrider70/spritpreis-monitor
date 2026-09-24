/*
 * touch.h - XPT2046 resistiver Touch (CYD) - API
 *
 * Der CYD (ESP32-2432S028R) hat einen XPT2046 auf separatem SPI-Bus SPI3
 * (Pins in config.h: TOUCH_*). touch_init() initialisiert den Bus,
 * touch_task_start() pollt zyklisch und ruft den Callback mit Display-
 * Koordinaten auf (nur bei echtem Druck, inkl. Entprellung).
 */

#pragma once

#include <stdbool.h>
#include "esp_err.h"

/* Callback: liefert einen "Tap" (x,y) in Display-Koordinaten */
typedef void (*touch_cb_t)(int x, int y, void *arg);

/* Callback fuer Ziehen: wird beim Aufsetzen, bei jeder Bewegung und beim
 * Loslassen gerufen (pressed=false beim Loslassen). Damit laesst sich z. B.
 * ein Diagramm verschieben. */
typedef void (*touch_press_cb_t)(int x, int y, bool pressed, void *arg);

/* Initialisiert den XPT2046-SPI-Bus (einmal). */
esp_err_t touch_init(void);

/* Startet den Poll-Task (nur Taps). */
esp_err_t touch_task_start(touch_cb_t cb, void *arg);

/* Startet den Poll-Task mit zusaetzlichen Bewegungsereignissen. */
esp_err_t touch_task_start_full(touch_cb_t cb, touch_press_cb_t press_cb, void *arg);

/* Kalibrier-Modus: loggt Rohwerte (zur Bestimmung der Umrechnung). */
void touch_set_calib_mode(bool on);
