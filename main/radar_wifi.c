// ⚫ AJOUTÉ (24/10/2026) : radar Wi-Fi — réception CSI.
// Deux modes, choisis selon main/wifi_credentials.h (cf. wifi_credentials.example.h) :
//  - CONNECTÉ (SSID renseigné) : station connectée au point d'accès de la pièce + ping de la passerelle à
//    CONFIG_RADAR_PING_HZ. Chaque réponse est une trame émise par le point d'accès -> un échantillon CSI
//    régulier (~100/s). Méthode de l'exemple Espressif esp-csi et d'ESPectre.
//  - PASSIF (SSID vide) : scan, puis écoute en mode promiscuous du point d'accès le plus fort (balises ~10/s).
// Dans les deux cas seules les trames émises par CE point d'accès (adresse MAC source = BSSID) sont gardées :
// mélanger plusieurs émetteurs rendrait la variance des sous-porteuses inexploitable.
#include <string.h>
#include <math.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_wifi.h"
#include "esp_event.h"
#include "esp_netif.h"
#include "esp_log.h"
#include "esp_mac.h"        // MACSTR / MAC2STR
#include "ping/ping_sock.h"
#include "lwip/ip_addr.h"
#include "radar.h"
// Généré vide par main/CMakeLists.txt s'il n'existe pas (SSID "" -> mode passif)
#include "wifi_credentials.h"

#define TAG "RADAR_WIFI"

static uint8_t s_target_mac[6];
static volatile bool s_target_valid;
static esp_ping_handle_t s_ping;
static char s_ssid[33];

static inline uint8_t csi_amp(const int8_t *buf, int idx) {
    // Chaque sous-porteuse = 2 octets signés : partie imaginaire puis partie réelle
    int im = buf[2 * idx];
    int re = buf[2 * idx + 1];
    float a = sqrtf((float)(im * im + re * re));
    return a > 255.0f ? 255 : (uint8_t)a;
}

// Appelé par la tâche Wi-Fi : rester court, aucun blocage
static void csi_rx_cb(void *ctx, wifi_csi_info_t *info) {
    if (!s_target_valid || info == NULL || info->buf == NULL || info->len < 128) {
        return;
    }
    if (memcmp(info->mac, s_target_mac, 6) != 0) {
        return;
    }
    radar_sample_t s;
    int n = 0;
    // LLTF : buffer = sous-porteuses 0..31 puis -32..-1. Utiles : 1..26 et -26..-1 ; l'indice 1 est sauté
    // car le premier mot (indices 0 et 1) peut être invalide (first_word_invalid).
    for (int i = 2; i <= 26; i++) {
        s.amp[n++] = csi_amp(info->buf, i);
    }
    for (int i = 38; i <= 63; i++) {
        s.amp[n++] = csi_amp(info->buf, i);
    }
    s.rssi = info->rx_ctrl.rssi;
    radar_dsp_push(&s);
}

// Promiscuous : la réception CSI exige un callback, les trames elles-mêmes ne servent pas
static void promisc_rx_cb(void *buf, wifi_promiscuous_pkt_type_t type) {
}

static void csi_enable(void) {
    wifi_csi_config_t cfg = {
        .lltf_en = true,
        .htltf_en = false,          // LLTF seul : même format (64 sous-porteuses) pour toutes les trames
        .stbc_htltf2_en = false,
        .ltf_merge_en = false,
        .channel_filter_en = false, // sous-porteuses indépendantes (pas de lissage)
        .manu_scale = false,
        .shift = 0,
    };
    ESP_ERROR_CHECK(esp_wifi_set_csi_config(&cfg));
    ESP_ERROR_CHECK(esp_wifi_set_csi_rx_cb(csi_rx_cb, NULL));
    ESP_ERROR_CHECK(esp_wifi_set_csi(true));
}

// ---------------------------------------------------------------------------------------------------------
// Mode connecté
// ---------------------------------------------------------------------------------------------------------

static void ping_start(uint32_t gw_addr) {
    if (s_ping) {
        esp_ping_stop(s_ping);
        esp_ping_delete_session(s_ping);
        s_ping = NULL;
    }
    esp_ping_config_t cfg = ESP_PING_DEFAULT_CONFIG();
    cfg.count = ESP_PING_COUNT_INFINITE;
    cfg.interval_ms = 1000 / CONFIG_RADAR_PING_HZ;
    cfg.timeout_ms = 1000;
    cfg.data_size = 1;
    cfg.task_stack_size = 3072;
    ip_addr_t target = {0};
    target.type = IPADDR_TYPE_V4;
    ip4_addr_set_u32(ip_2_ip4(&target), gw_addr);
    cfg.target_addr = target;
    esp_ping_callbacks_t cbs = {0};   // callbacks inutiles : seul le trafic de réponse compte
    if (esp_ping_new_session(&cfg, &cbs, &s_ping) == ESP_OK) {
        esp_ping_start(s_ping);
    } else {
        ESP_LOGE(TAG, "ping : création de session impossible");
    }
}

static void wifi_event_handler(void *arg, esp_event_base_t base, int32_t id, void *data) {
    if (base == WIFI_EVENT && id == WIFI_EVENT_STA_START) {
        radar_set_link(s_ssid, 0, false, "Connexion Wi-Fi...");
        esp_wifi_connect();
    } else if (base == WIFI_EVENT && id == WIFI_EVENT_STA_DISCONNECTED) {
        const wifi_event_sta_disconnected_t *ev = data;
        ESP_LOGW(TAG, "déconnecté (raison %d), nouvelle tentative", ev->reason);
        s_target_valid = false;
        if (s_ping) {
            esp_ping_stop(s_ping);
        }
        radar_set_link(s_ssid, 0, false, "Wi-Fi perdu, reconnexion...");
        vTaskDelay(pdMS_TO_TICKS(1000));
        esp_wifi_connect();
    } else if (base == IP_EVENT && id == IP_EVENT_STA_GOT_IP) {
        const ip_event_got_ip_t *ev = data;
        wifi_ap_record_t ap;
        if (esp_wifi_sta_get_ap_info(&ap) == ESP_OK) {
            memcpy(s_target_mac, ap.bssid, 6);
            s_target_valid = true;
            radar_set_link(s_ssid, ap.primary, false, NULL);
            ESP_LOGI(TAG, "connecté à %s canal %d, IP " IPSTR ", passerelle " IPSTR ", AP " MACSTR,
                     s_ssid, ap.primary, IP2STR(&ev->ip_info.ip), IP2STR(&ev->ip_info.gw), MAC2STR(ap.bssid));
        }
        ping_start(ev->ip_info.gw.addr);
    }
}

static void wifi_start_sta(void) {
    esp_netif_create_default_wifi_sta();
    wifi_init_config_t init = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&init));
    // Pas d'écriture NVS par le Wi-Fi : une écriture flash suspend le cache et fait "glisser" l'écran RGB
    ESP_ERROR_CHECK(esp_wifi_set_storage(WIFI_STORAGE_RAM));
    ESP_ERROR_CHECK(esp_event_handler_register(WIFI_EVENT, ESP_EVENT_ANY_ID, wifi_event_handler, NULL));
    ESP_ERROR_CHECK(esp_event_handler_register(IP_EVENT, IP_EVENT_STA_GOT_IP, wifi_event_handler, NULL));

    wifi_config_t cfg = {0};
    strlcpy((char *)cfg.sta.ssid, RADAR_WIFI_SSID, sizeof(cfg.sta.ssid));
    strlcpy((char *)cfg.sta.password, RADAR_WIFI_PASSWORD, sizeof(cfg.sta.password));
    cfg.sta.threshold.authmode = strlen(RADAR_WIFI_PASSWORD) ? WIFI_AUTH_WPA_PSK : WIFI_AUTH_OPEN;
    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_STA, &cfg));
    ESP_ERROR_CHECK(esp_wifi_start());
    // Économie d'énergie désactivée : sinon la radio dort entre deux balises et le flux CSI s'effondre
    ESP_ERROR_CHECK(esp_wifi_set_ps(WIFI_PS_NONE));
    csi_enable();
}

// ---------------------------------------------------------------------------------------------------------
// Mode passif
// ---------------------------------------------------------------------------------------------------------

static void passive_task(void *arg) {
    wifi_init_config_t init = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&init));
    ESP_ERROR_CHECK(esp_wifi_set_storage(WIFI_STORAGE_RAM));
    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
    ESP_ERROR_CHECK(esp_wifi_start());
    ESP_ERROR_CHECK(esp_wifi_set_ps(WIFI_PS_NONE));

    wifi_ap_record_t best = {0};
    for (;;) {
        radar_set_link("", 0, true, "Recherche Wi-Fi...");
        uint16_t n = 0;
        if (esp_wifi_scan_start(NULL, true) == ESP_OK && esp_wifi_scan_get_ap_num(&n) == ESP_OK && n > 0) {
            if (n > 20) {
                n = 20;
            }
            // ⚫ 24/10/2026 : static (20 x 80 octets) : sur la pile, débordement de radar_scan au démarrage
            static wifi_ap_record_t recs[20];
            esp_wifi_scan_get_ap_records(&n, recs);
            for (int i = 0; i < n; i++) {
                if (i == 0 || recs[i].rssi > best.rssi) {
                    best = recs[i];
                }
            }
            break;
        }
        ESP_LOGW(TAG, "aucun point d'accès trouvé, nouveau scan dans 5 s");
        vTaskDelay(pdMS_TO_TICKS(5000));
    }

    strlcpy(s_ssid, (const char *)best.ssid, sizeof(s_ssid));
    ESP_LOGI(TAG, "mode passif : %s canal %d (%d dBm) " MACSTR, s_ssid, best.primary, best.rssi, MAC2STR(best.bssid));
    memcpy(s_target_mac, best.bssid, 6);

    wifi_promiscuous_filter_t filter = {
        .filter_mask = WIFI_PROMIS_FILTER_MASK_MGMT | WIFI_PROMIS_FILTER_MASK_DATA,
    };
    ESP_ERROR_CHECK(esp_wifi_set_promiscuous_filter(&filter));
    ESP_ERROR_CHECK(esp_wifi_set_promiscuous_rx_cb(promisc_rx_cb));
    ESP_ERROR_CHECK(esp_wifi_set_promiscuous(true));
    ESP_ERROR_CHECK(esp_wifi_set_channel(best.primary, WIFI_SECOND_CHAN_NONE));
    csi_enable();
    s_target_valid = true;
    radar_set_link(s_ssid, best.primary, true, NULL);
    vTaskDelete(NULL);
}

void radar_wifi_start(void) {
    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());
    if (strlen(RADAR_WIFI_SSID) > 0) {
        strlcpy(s_ssid, RADAR_WIFI_SSID, sizeof(s_ssid));
        ESP_LOGI(TAG, "mode connecté : %s, ping %d Hz", s_ssid, CONFIG_RADAR_PING_HZ);
        wifi_start_sta();
    } else {
        ESP_LOGI(TAG, "pas d'identifiants (main/wifi_credentials.h) : mode passif");
        xTaskCreatePinnedToCore(passive_task, "radar_scan", 6144, NULL, 4, NULL, 0);
    }
}
