cmake_minimum_required(VERSION 3.24)

# ESP-IDF Projekt - Spritpreis-Monitor
#
# Beobachtet eine feste Tankstelle (Datenquelle: Tankerkönig / MTS-K,
# CC BY 4.0), zeigt den aktuellen Preis gross und den bisherigen Verlauf
# als Diagramm. Ansicht umschaltbar zwischen Stunden und Tagen, per Touch
# verschiebbar.
#
# Hardware: CYD ESP32-2432S028R (2,8", ILI9341 320x240, XPT2046-Touch).
# Display- und Touch-Treiber stammen aus dem verifizierten CYD-Aufbau
# (autofrontcam / katzenfutterautomat), Pins siehe include/config.h.

include($ENV{IDF_PATH}/tools/cmake/project.cmake)
project(spritpreis-monitor)

# Build-Nummer + version.h wird in main/CMakeLists.txt generiert
