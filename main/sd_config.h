/*
 * sd_config.h - Zugangsdaten aus einer Datei auf der microSD einlesen
 *
 * Ablauf (vom Nutzer so gewuenscht):
 *   1. Beim Start sieht das Geraet nach /sdcard/spritpreis.cfg.
 *   2. Fehlt die Datei, wird die Musterdatei angelegt - mit den Schluessel-
 *      woertern, aber leeren Platzhaltern zum Eintragen.
 *   3. Der Nutzer steckt die Karte in den PC, traegt hinter station= und
 *      apikey= die echten Werte ein und steckt sie zurueck.
 *   4. Beim naechsten Start uebernimmt das Geraet die Werte ins NVS und
 *      setzt die Datei wieder auf die Platzhalter zurueck. Damit liegen
 *      die Geheimnisse dauerhaft nur im Flash des Geraets.
 *
 * Stehen noch Platzhalter in der Datei, passiert nichts - der laufende
 * Betrieb (z. B. Simulation) wird nicht angefasst.
 *
 * Wichtig: Das Zuruecksetzen löscht den Inhalt der Datei logisch, nicht
 * physisch. Wer die Karte ausliest, koennte alte Bloecke finden. Fuer den
 * Alltag (Karte verloren/entnommen) reicht das; wer mehr will, formatiert
 * die Karte zwischendurch oder laesst sie im Geraet.
 */

#pragma once

/* Beim Start aufrufen: liest die Datei, uebernimmt Werte ins NVS und
 * schreibt die Musterdatei zurueck. Ohne Karte passiert nichts.
 * Rueckgabe: Anzahl der uebernommenen Werte (0 = nichts geaendert). */
int sd_config_import(void);

/* Zustand auf der Konsole ausgeben (Befehl "cfg"). Zeigt nur, OB Werte
 * eingetragen sind - niemals den Key selbst. */
void sd_config_print_status(void);

/* Musterdatei neu schreiben (Konsole: "cfg template"). */
void sd_config_write_template(void);
