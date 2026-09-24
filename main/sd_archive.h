/*
 * sd_archive.h - Langzeitarchiv der Preise auf der microSD-Karte
 *
 * Der NVS-Verlauf (price_log.c) haelt nur 14 Tage stuendlich bzw. 120 Tage
 * taeglich und schont das Flash. Fuer "richtige" Langzeitdaten schreibt
 * dieses Modul jede Preisaenderung und jede abgeschlossene Stunde als Zeile
 * in eine CSV-Datei auf der Karte:
 *
 *   /sdcard/spritpreis-JJJJ-MM.csv     (eine Datei je Monat)
 *
 * Aufbau der Datei (Semikolon als Trenner, daher in deutschem Excel direkt
 * in Spalten):
 *   # zeit_iso;epoch;e5_milli;e10_milli;diesel_milli;status
 *   2026-09-23T14:35:02;1789917302;1689;1652;1589;open
 *
 * Preise sind Milli-Euro als Ganzzahl (1689 = 1,689 EUR) - so braucht die
 * Auswertung keine Gleitkommazahlen.
 *
 * Geschrieben wird nur, wenn sich ein Preis geaendert hat oder ein Tag
 * abgeschlossen ist: rund 10-20 Zeilen am Tag statt 288. Eine Zeile gilt so
 * lange, bis die naechste kommt - die Reihe bleibt damit vollstaendig
 * rekonstruierbar. Rueckwaerts wird nie geschrieben (ein zweiter
 * "sim fill" haengt also keine doppelten Zeilen an).
 *
 * Steckt keine Karte im Slot, laeuft das Geraet unveraendert weiter - das
 * Archiv bleibt dann einfach leer (Konsole: "sd").
 */

#pragma once

#include <stdbool.h>
#include <stdint.h>
#include <time.h>

#include "esp_err.h"
#include "fuel_api.h"

/* Karte mounten (FAT). Ohne Karte: Fehlercode, Betrieb laeuft weiter. */
esp_err_t sd_archive_init(void);

/* true, wenn die Karte beschreibbar gemountet ist. */
bool sd_archive_ready(void);

/* Aktuellen Stand anhaengen (schreibt nur bei Aenderung/Stundenwechsel). */
void sd_archive_add(time_t now, const fuel_prices_t *p);

/* Verlauf aus den Archivdateien in den Arbeitsspeicher ergaenzen. Wird nach
 * einem Neustart gebraucht: der NVS kennt nur die abgeschlossenen Werte bis
 * zum letzten Schreiben, die Karte dagegen jede Preisaenderung. */
void sd_archive_laden(void);

/* Name der aktuellen Monatsdatei ("" wenn keine Uhr gestellt ist). Fuer
 * den Konsolenbefehl "file csv". */
const char *sd_archive_dateiname(void);

/* Groesse der aktuellen Monatsdatei in Byte (0, wenn es sie nicht gibt). */
unsigned sd_archive_dateigroesse(void);

/* Zeilen und sim-Zeilen der aktuellen Monatsdatei zaehlen.
 * Rueckgabe: Zeilen gesamt, -1 wenn nicht lesbar. */
int sd_archive_zaehlen(int *zeilen, int *sim_zeilen);

/* Alle Archivdateien neu schreiben: sim-Zeilen weglassen (sim_entfernen) und
 * dabei das in den Einstellungen gewaehlte Format anwenden. Es wird zuerst in
 * eine Nebendatei geschrieben und erst danach die alte ersetzt - bei einem
 * Abbruch bleibt so die bisherige Datei erhalten.
 * Rueckgabe: Zeilen nach dem Aufraeumen, -1 wenn keine Karte steckt. */
int sd_archive_aufraeumen(bool sim_entfernen, int *zeilen_gesamt, int *sim_entfernt);

/* Zustand auf der Konsole ausgeben (Befehl "sd"). */
void sd_archive_print_status(void);

/* Sperre fuer alle Zugriffe auf die Karte. Das FAT-Dateisystem vertraegt
 * keine zwei gleichzeitigen Zugriffe - ausser dem Archiv schreibt auch die
 * Logdatei (sd_log.c) auf dieselbe Karte. Ohne Karte tun die beiden
 * Funktionen nichts. */
void sd_archive_lock(void);
void sd_archive_unlock(void);
