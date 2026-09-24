idf_component_register(
    SRCS
        main.c
        display.c
        touch.c
        ui.c
        wifi.c
        fuel_api.c
        fuel_poll.c
        price_log.c
        trend.c
        sd_archive.c
        sd_log.c
        sd_config.c
        sim.c
        power.c
        settings.c
        console.c
    INCLUDE_DIRS
        ${CMAKE_SOURCE_DIR}/include
    REQUIRES
        nvs_flash
        esp_timer
        console
        esp_system
        esp_driver_gpio
        esp_driver_spi
        esp_driver_uart
        esp_driver_sdspi
        sdmmc
        fatfs
        esp_wifi
        esp_event
        esp_netif
        esp_http_client
        mbedtls
        lwip
        freertos
        heap
)

# Build-Nummer erhoehen + include/version.h generieren (VOR der main-Kompilierung)
# increment_build.py liegt im Projekt unter tools/, --project-dir zeigt auf das Projekt.
add_custom_target(spm_version ALL
    COMMAND ${PYTHON} ${CMAKE_SOURCE_DIR}/tools/increment_build.py --project-dir ${CMAKE_SOURCE_DIR}
    COMMENT "Erhoehe Build-Nummer und generiere version.h"
    VERBATIM
)
add_dependencies(${COMPONENT_LIB} spm_version)
# main.c.obj haengt EXPLIZIT von version.h ab -> Generierung laeuft immer vor der
# Kompilierung und main.c wird bei jeder Aenderung der Buildnummer neu gebaut
target_sources(${COMPONENT_LIB} PRIVATE ${CMAKE_SOURCE_DIR}/include/version.h)
