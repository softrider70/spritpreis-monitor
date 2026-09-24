/*
 * console.h - serielle Kommando-Konsole (esp_console)
 */

#pragma once

#include "esp_err.h"

/* Startet die REPL auf dem Standard-UART. */
esp_err_t console_start(void);
