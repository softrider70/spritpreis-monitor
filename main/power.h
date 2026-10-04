/*
 * power.h - Anzeige-Stromsparmodus (Display aus + Light-Sleep)
 *
 * Das CYD soll möglichst wenig Strom brauchen. Die Anzeige ist nur nach
 * einem Ereignis an:
 *   - Touch (Berührung),
 *   - BOOT-Taste,
 *   - Preisänderung (dann für POWER_DISPLAY_ON_MS, Standard 60 s).
 *
 * Danach geht das Display aus (Hintergrundbeleuchtung + Panel-Sleep). Läuft
 * der Light-Sleep, schläft die CPU zusätzlich in ihren Ruhezeiten.
 *
 * Was wie viel bringt (Erfahrungswerte, am Gerät zu prüfen):
 *   Display + Backlight an  ~ 80-100 mA
 *   nur Backlight aus       ~ 25-35 mA   (CPU läuft, Touch reagiert sofort)
 *   zusätzlich Light-Sleep  ~ wenige mA  (Aufwachen per Touch-IRQ/Taste)
 */

#pragma once

#include <stdbool.h>
#include <stdint.h>

/* Taster, Wakeup-Quellen und Power Management einrichten. */
void power_init(void);

/* Wird gerufen, wenn das Display geweckt wurde - der Aufrufer zeichnet dann
 * die Anzeige neu (z. B. ui_render). */
void power_set_wake_callback(void (*cb)(void));

/* Liefert die Last fuer den Wachhalte-Impuls (siehe POWER_KEEPALIVE_*).
 * Ohne Zusatzlast am Pin wird hier wifi_lastimpuls gesetzt: ein WLAN-Scan
 * zieht ohne Zusatzhardware deutlich Strom. Rueckgabe true = Impuls lief. */
void power_set_lastimpuls_callback(bool (*cb)(void));

/* Wird gerufen, wenn sich die Spannungsanzeige aendern soll (sichtbar=false
 * laesst das Feld leer - so entsteht das Blinken der Akku-Warnung). */
void power_set_blink_callback(void (*cb)(bool sichtbar));

/* Ereignis melden: Haltezeit neu starten und die Anzeige einschalten, falls
 * sie aus war. Rueckgabe true = sie wurde geweckt und dabei schon neu
 * gezeichnet - der Aufrufer soll dann NICHT noch einmal zeichnen (sonst
 * sieht man den zweiten Bildaufbau als Blitzen). */
bool power_activity(void);

/* true, wenn die Anzeige gerade eingeschaltet ist. */
bool power_display_on(void);

/* display_power(): true, wenn der Light-Sleep eingerichtet werden konnte. */
bool power_light_sleep_ready(void);

/* Zustandszeile fuer die Konsole. */
void power_print_status(void);

/* Anzeige von Hand schalten (Konsole: power on|off). */
void power_force(bool on);

/* Wachhalte-Impulse fuer das Lademodul (config.h, POWER_KEEPALIVE_*).
 *
 * Manche Lademodule schalten ihren Ausgang ab, wenn die Last zu klein wird.
 * Gegenmittel: in festem Abstand kurz eine Zusatzlast am Pin
 * POWER_KEEPALIVE_GPIO einschalten. Die Impulse laufen in festem Rhythmus,
 * unabhaengig davon, ob die Anzeige gerade an ist - haengen sie am
 * Ausschalten der Anzeige, kommen sie bei haeufigen Preisaenderungen nie
 * zustande.
 *
 * set(): sekunden = 0 schaltet ab, millisekunden <= 0 laesst die Dauer wie
 * sie ist. Der Wert liegt nur im Arbeitsspeicher - nach dem Neustart gilt
 * wieder config.h. */
void power_keepalive_set(int sekunden, int millisekunden);
