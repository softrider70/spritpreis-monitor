/*
 * fuel_poll.h - Abfragetakt: holt die Preise, schreibt den Verlauf, zeichnet
 */

#pragma once

#include <stdbool.h>
#include "fuel_api.h"

/* Startet den Poll-Task (Core 0). Abfrage alle FUEL_POLL_INTERVAL_S Sekunden. */
void fuel_poll_start(void);

/* Sofort eine Abfrage ausloesen (Konsole: poll). */
void fuel_poll_now(void);

/* Letzte gueltige Werte (bleiben stehen, wenn eine Abfrage fehlschlaegt). */
const fuel_prices_t *fuel_poll_last(void);

/* true, wenn die letzte Abfrage technisch erfolgreich war. */
bool fuel_poll_net_ok(void);
