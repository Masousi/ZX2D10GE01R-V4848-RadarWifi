# ⚫ MODIFIÉ (24/10/2026) : démos LVGL supprimées, seuls les sources de main/ sont compilés
# ⚫ AJOUTÉ (24/10/2026) : identifiants Wi-Fi du radar hors dépôt. S'il n'existe pas, main/wifi_credentials.h est
# créé avec un SSID vide (= mode passif) ; le modifier ensuite suffit (dépendance suivie par la compilation).
if(NOT EXISTS ${CMAKE_CURRENT_LIST_DIR}/wifi_credentials.h)
    file(WRITE ${CMAKE_CURRENT_LIST_DIR}/wifi_credentials.h [=[
// Identifiants Wi-Fi du radar (fichier local, ignoré par git) : cf. wifi_credentials.example.h
// SSID vide = mode passif (écoute du point d'accès voisin le plus fort).
#pragma once

#define RADAR_WIFI_SSID     ""
#define RADAR_WIFI_PASSWORD ""
]=])
endif()

idf_component_register(
    SRC_DIRS "."
    INCLUDE_DIRS "."
    # REQUIRES esp_lcd driver lvgl esp_timer button mt8901 ws2812
    # ⚫ MODIFIÉ (24/10/2026) : + Wi-Fi / réseau (radar CSI, ping de la passerelle) et NVS
    # REQUIRES esp_lcd driver lvgl esp_timer button mt8901 ws2812 esp_wifi esp_netif esp_event nvs_flash lwip
    # ⚫ MODIFIÉ (25/10/2026) : + serveur web et JSON du portail de configuration Wi-Fi
    REQUIRES esp_lcd driver lvgl esp_timer button mt8901 ws2812 esp_wifi esp_netif esp_event nvs_flash lwip
             esp_http_server json
)

target_compile_options(${COMPONENT_LIB} PRIVATE "-Wno-format")
