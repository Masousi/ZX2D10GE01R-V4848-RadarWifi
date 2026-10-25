// ⚫ AJOUTÉ (25/10/2026) : radar Wi-Fi — portail de configuration du Wi-Fi (repris de RadarAvion/wifi_portal.c).
// Portail : point d'accès ouvert CONFIG_RADAR_AP_SSID ("RadarWifi_Config", 192.168.4.1) annoncé à l'écran par un
// QR code ; page web pour choisir le réseau de la pièce et saisir le mot de passe ; DNS captif pour que le
// téléphone ouvre la page tout seul. Les identifiants sont enregistrés en NVS puis la carte redémarre.
// Les demandes "ouvrir le portail" / "mode passif" passent par la mémoire RTC (conservée à travers esp_restart()).
#include <string.h>
#include <stdlib.h>
#include <ctype.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"
#include "esp_wifi.h"
#include "esp_timer.h"
#include "esp_system.h"
#include "esp_attr.h"
#include "esp_http_server.h"
#include "nvs.h"
#include "lwip/sockets.h"
#include "cJSON.h"
#include "radar.h"

#define TAG "RADAR_PORTAL"

#define NVS_NS                  "radarwifi"
#define PORTAL_IDLE_RESTART_US  (5LL * 60 * 1000000)   // 5 min sans visiteur -> nouvel essai du réseau enregistré
#define SCAN_MAX                20

static wifi_ap_record_t s_scan[SCAN_MAX];
static uint16_t s_scan_n;
static volatile int64_t s_restart_at_us;

// ---------------------------------------------------------------------------------------------
// Identifiants (NVS) et demandes au prochain démarrage (RTC)
// ---------------------------------------------------------------------------------------------
bool radar_cfg_load(char *ssid, size_t ssid_cap, char *pass, size_t pass_cap) {
    ssid[0] = 0;
    pass[0] = 0;
    nvs_handle_t h;
    if (nvs_open(NVS_NS, NVS_READONLY, &h) != ESP_OK) {
        return false;   // premier démarrage : rien d'enregistré
    }
    size_t len = ssid_cap;
    if (nvs_get_str(h, "ssid", ssid, &len) != ESP_OK) {
        ssid[0] = 0;
    }
    len = pass_cap;
    if (nvs_get_str(h, "pass", pass, &len) != ESP_OK) {
        pass[0] = 0;
    }
    nvs_close(h);
    return ssid[0] != 0;
}

static esp_err_t radar_cfg_save(const char *ssid, const char *pass) {
    nvs_handle_t h;
    esp_err_t err = nvs_open(NVS_NS, NVS_READWRITE, &h);
    if (err != ESP_OK) {
        return err;
    }
    err = nvs_set_str(h, "ssid", ssid);
    if (err == ESP_OK) err = nvs_set_str(h, "pass", pass ? pass : "");
    if (err == ESP_OK) err = nvs_commit(h);
    nvs_close(h);
    ESP_LOGI(TAG, "Wi-Fi enregistré : '%s'", ssid);
    return err;
}

#define BOOT_PORTAL_MAGIC  0x52575043u   // "RWPC"
#define BOOT_PASSIVE_MAGIC 0x52575053u   // "RWPS"
static RTC_NOINIT_ATTR uint32_t s_boot_request;

// Prochain démarrage : 'P' = portail, 'S' = mode passif, 0 = normal
static void boot_request_restart(uint32_t magic) {
    s_boot_request = magic;
    esp_restart();
}

void radar_request_portal(void) {
    boot_request_restart(BOOT_PORTAL_MAGIC);
}

void radar_request_passive(void) {
    boot_request_restart(BOOT_PASSIVE_MAGIC);
}

void radar_request_retry(void) {
    boot_request_restart(0);
}

char radar_take_boot_request(void) {
    char r = s_boot_request == BOOT_PORTAL_MAGIC ? 'P' : (s_boot_request == BOOT_PASSIVE_MAGIC ? 'S' : 0);
    s_boot_request = 0;
    return r;
}

// ---------------------------------------------------------------------------------------------
// Page web du portail
// ---------------------------------------------------------------------------------------------
static const char PORTAL_HTML[] =
"<!DOCTYPE html><html lang=fr><head><meta charset=utf-8>"
"<meta name=viewport content='width=device-width,initial-scale=1'>"
"<title>Radar Wi-Fi - configuration</title><style>"
"*{box-sizing:border-box}body{margin:0;font-family:system-ui,sans-serif;background:#04140c;color:#d8ffe8;"
"display:flex;justify-content:center;padding:24px 16px}"
".card{width:100%;max-width:420px;background:#082317;border:1px solid #1d5c3b;border-radius:18px;padding:22px;"
"box-shadow:0 0 40px #00ff8822}"
"h1{margin:0 0 4px;font-size:22px;color:#5dffa8}p{margin:0 0 18px;color:#8fc9a8;font-size:14px}"
"label{display:block;margin:14px 0 6px;font-size:13px;color:#8fc9a8}"
"select,input{width:100%;padding:12px;border-radius:10px;border:1px solid #1d5c3b;background:#031009;color:#e8fff1;font-size:16px}"
"button{width:100%;margin-top:22px;padding:14px;border:0;border-radius:10px;background:#2fe08a;color:#03140b;"
"font-size:17px;font-weight:700}"
".small{font-size:12px;color:#6ea888;margin-top:6px}a{color:#5dffa8}"
"</style></head><body><div class=card>"
"<h1>&#128225; Radar Wi-Fi</h1><p>Choisissez la box Wi-Fi <b>de la pi&egrave;ce &agrave; surveiller</b> (2,4 GHz). "
"Le radar s'y connecte pour mesurer les perturbations caus&eacute;es par les personnes.</p>"
"<form method=post action=/save>"
"<label>R&eacute;seau</label><select name=ssid id=ssid><option value=''>Recherche des r&eacute;seaux&hellip;</option></select>"
"<div class=small><a href='#' onclick='scan(1);return false'>Actualiser la liste</a></div>"
"<label>Ou nom du r&eacute;seau (masqu&eacute;)</label><input name=ssid_m maxlength=32 placeholder='SSID'>"
"<label>Mot de passe</label><input name=pass type=password maxlength=64 placeholder='(vide si r&eacute;seau ouvert)'>"
"<div class=small>Le mot de passe reste dans le radar (m&eacute;moire interne), il n'est envoy&eacute; nulle part.</div>"
"<button type=submit>Enregistrer et red&eacute;marrer</button></form></div>"
"<script>"
"function scan(r){fetch('/scan'+(r?'?r=1':'')).then(x=>x.json()).then(l=>{var s=document.getElementById('ssid');"
"s.innerHTML='';if(!l.length){var o=document.createElement('option');o.value='';o.text='Aucun r\\u00e9seau trouv\\u00e9';s.add(o);}"
"l.forEach(a=>{var o=document.createElement('option');o.value=a.s;"
"o.text=a.s+'  ('+(a.r>-60?'\\u2582\\u2584\\u2586':a.r>-75?'\\u2582\\u2584':'\\u2582')+(a.o?'':' \\ud83d\\udd12')+')';s.add(o);});})}"
"scan(0);</script></body></html>";

static const char SAVED_HTML[] =
"<!DOCTYPE html><html lang=fr><head><meta charset=utf-8><meta name=viewport content='width=device-width,initial-scale=1'>"
"<title>Radar Wi-Fi</title></head><body style='font-family:system-ui;background:#04140c;color:#d8ffe8;text-align:center;padding:40px'>"
"<h2 style='color:#5dffa8'>&#10004; Enregistr&eacute;</h2><p>Le radar red&eacute;marre et se connecte au r&eacute;seau choisi.<br>"
"Vous pouvez quitter le Wi-Fi " CONFIG_RADAR_AP_SSID ".</p></body></html>";

static void do_scan(void) {
    wifi_scan_config_t sc = { .show_hidden = false };
    if (esp_wifi_scan_start(&sc, true) != ESP_OK) {
        s_scan_n = 0;
        return;
    }
    uint16_t n = SCAN_MAX;
    if (esp_wifi_scan_get_ap_records(&n, s_scan) != ESP_OK) {
        n = 0;
    }
    s_scan_n = n;
    ESP_LOGI(TAG, "%d réseaux trouvés", n);
}

static esp_err_t page_get(httpd_req_t *req) {
    httpd_resp_set_type(req, "text/html; charset=utf-8");
    return httpd_resp_send(req, PORTAL_HTML, HTTPD_RESP_USE_STRLEN);
}

// Liste JSON des réseaux : [{"s":ssid,"r":rssi,"o":ouvert}] (doublons supprimés, triés par signal par le pilote)
static esp_err_t scan_get(httpd_req_t *req) {
    char q[8];
    if (httpd_req_get_url_query_str(req, q, sizeof(q)) == ESP_OK && strstr(q, "r=1")) {
        do_scan();
    }
    cJSON *arr = cJSON_CreateArray();
    for (int i = 0; i < s_scan_n; i++) {
        const char *ssid = (const char *)s_scan[i].ssid;
        if (!ssid[0]) {
            continue;
        }
        bool dup = false;
        for (int j = 0; j < i; j++) {
            if (strcmp(ssid, (const char *)s_scan[j].ssid) == 0) {
                dup = true;
                break;
            }
        }
        if (dup) {
            continue;
        }
        cJSON *o = cJSON_CreateObject();
        cJSON_AddStringToObject(o, "s", ssid);
        cJSON_AddNumberToObject(o, "r", s_scan[i].rssi);
        cJSON_AddBoolToObject(o, "o", s_scan[i].authmode == WIFI_AUTH_OPEN);
        cJSON_AddItemToArray(arr, o);
    }
    char *txt = cJSON_PrintUnformatted(arr);
    cJSON_Delete(arr);
    httpd_resp_set_type(req, "application/json");
    esp_err_t err = httpd_resp_send(req, txt ? txt : "[]", HTTPD_RESP_USE_STRLEN);
    free(txt);
    return err;
}

// Décodage application/x-www-form-urlencoded en place ('+' -> espace, %XX -> octet)
static void url_decode(char *s) {
    char *o = s;
    for (; *s; s++) {
        if (*s == '+') {
            *o++ = ' ';
        } else if (*s == '%' && isxdigit((unsigned char)s[1]) && isxdigit((unsigned char)s[2])) {
            char hex[3] = { s[1], s[2], 0 };
            *o++ = (char)strtol(hex, NULL, 16);
            s += 2;
        } else {
            *o++ = *s;
        }
    }
    *o = 0;
}

static void form_field(const char *body, const char *key, char *dst, size_t cap) {
    char raw[200] = {0};
    dst[0] = 0;
    if (httpd_query_key_value(body, key, raw, sizeof(raw)) == ESP_OK) {
        url_decode(raw);
        strlcpy(dst, raw, cap);
    }
}

static esp_err_t save_post(httpd_req_t *req) {
    char body[600];
    int total = req->content_len;
    if (total <= 0 || total >= (int)sizeof(body)) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Formulaire invalide");
        return ESP_FAIL;
    }
    int got = 0;
    while (got < total) {
        int r = httpd_req_recv(req, body + got, total - got);
        if (r == HTTPD_SOCK_ERR_TIMEOUT) {
            continue;
        }
        if (r <= 0) {
            return ESP_FAIL;
        }
        got += r;
    }
    body[got] = 0;

    char ssid[33], ssid_m[33], pass[65];
    form_field(body, "ssid", ssid, sizeof(ssid));
    form_field(body, "ssid_m", ssid_m, sizeof(ssid_m));
    form_field(body, "pass", pass, sizeof(pass));
    if (ssid_m[0]) {
        strlcpy(ssid, ssid_m, sizeof(ssid));   // nom saisi à la main prioritaire sur la liste
    }
    if (!ssid[0]) {
        httpd_resp_set_status(req, "302 Found");
        httpd_resp_set_hdr(req, "Location", "/");
        return httpd_resp_send(req, NULL, 0);
    }
    if (radar_cfg_save(ssid, pass) != ESP_OK) {
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Enregistrement impossible");
        return ESP_FAIL;
    }
    radar_set_portal(true, "Enregistré ! Redémarrage...", true);
    httpd_resp_set_type(req, "text/html; charset=utf-8");
    httpd_resp_send(req, SAVED_HTML, HTTPD_RESP_USE_STRLEN);
    s_restart_at_us = esp_timer_get_time() + 1500000;   // laisse le temps à la réponse de partir
    return ESP_OK;
}

// Portail captif : toute autre URL (détection Android/iOS/Windows...) est redirigée vers la page
static esp_err_t redirect_404(httpd_req_t *req, httpd_err_code_t err) {
    httpd_resp_set_status(req, "302 Found");
    httpd_resp_set_hdr(req, "Location", "http://" RADAR_AP_IP "/");
    httpd_resp_send(req, "Redirection", HTTPD_RESP_USE_STRLEN);
    return ESP_OK;
}

static void http_start(void) {
    httpd_handle_t server = NULL;
    httpd_config_t cfg = HTTPD_DEFAULT_CONFIG();
    cfg.stack_size = 8192;
    cfg.lru_purge_enable = true;
    ESP_ERROR_CHECK(httpd_start(&server, &cfg));
    const httpd_uri_t uris[] = {
        { .uri = "/", .method = HTTP_GET, .handler = page_get },
        { .uri = "/scan", .method = HTTP_GET, .handler = scan_get },
        { .uri = "/save", .method = HTTP_POST, .handler = save_post },
    };
    for (int i = 0; i < sizeof(uris) / sizeof(uris[0]); i++) {
        httpd_register_uri_handler(server, &uris[i]);
    }
    httpd_register_err_handler(server, HTTPD_404_NOT_FOUND, redirect_404);
}

// ---------------------------------------------------------------------------------------------
// DNS captif : répond 192.168.4.1 à toutes les requêtes de type A
// ---------------------------------------------------------------------------------------------
static void dns_task(void *arg) {
    int sock = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    struct sockaddr_in addr = {
        .sin_family = AF_INET,
        .sin_port = htons(53),
        .sin_addr.s_addr = htonl(INADDR_ANY),
    };
    if (sock < 0 || bind(sock, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
        ESP_LOGE(TAG, "DNS captif : socket impossible");
        vTaskDelete(NULL);
        return;
    }
    uint8_t buf[512];
    for (;;) {
        struct sockaddr_in src;
        socklen_t sl = sizeof(src);
        int n = recvfrom(sock, buf, sizeof(buf), 0, (struct sockaddr *)&src, &sl);
        if (n < 12 || (buf[2] & 0x80) || ((buf[4] << 8) | buf[5]) == 0) {
            continue;   // trop court, déjà une réponse, ou aucune question
        }
        int p = 12;
        while (p < n && buf[p]) {
            p += buf[p] + 1;   // labels du nom demandé
        }
        p++;
        if (p + 4 > n) {
            continue;
        }
        uint16_t qtype = (buf[p] << 8) | buf[p + 1];
        int len = p + 4;   // on ne garde que la 1re question
        buf[2] = 0x81;     // réponse, récursion demandée
        buf[3] = 0x80;     // récursion disponible, pas d'erreur
        buf[4] = 0; buf[5] = 1;
        buf[6] = 0; buf[7] = 0;
        memset(&buf[8], 0, 4);
        if (qtype == 1 && len + 16 <= (int)sizeof(buf)) {
            static const uint8_t ans[16] = { 0xC0, 0x0C, 0, 1, 0, 1, 0, 0, 0, 60, 0, 4, 192, 168, 4, 1 };
            memcpy(buf + len, ans, sizeof(ans));
            len += sizeof(ans);
            buf[7] = 1;
        }
        sendto(sock, buf, len, 0, (struct sockaddr *)&src, sl);
    }
}

// Surveille le portail : redémarrage après enregistrement, ou nouvel essai du réseau enregistré après 5 min
static void portal_watch_task(void *arg) {
    bool has_saved = (bool)(intptr_t)arg;
    int64_t last_visit = esp_timer_get_time();
    for (;;) {
        vTaskDelay(pdMS_TO_TICKS(200));
        int64_t now = esp_timer_get_time();
        if (s_restart_at_us && now >= s_restart_at_us) {
            esp_restart();
        }
        radar_status_t st;
        radar_get_status(&st);
        if (st.portal_clients > 0) {
            last_visit = now;
        }
        if (has_saved && now - last_visit > PORTAL_IDLE_RESTART_US) {
            ESP_LOGW(TAG, "Portail inutilisé depuis 5 min : nouvel essai du réseau enregistré");
            esp_restart();
        }
    }
}

// Wi-Fi déjà initialisé (radar_wifi.c). Ne bloque pas : le portail tourne dans ses propres tâches.
void wifi_portal_start(const char *reason, bool has_saved_ssid) {
    ESP_LOGW(TAG, "Portail de configuration : %s", reason);
    radar_set_portal(true, reason, has_saved_ssid);

    esp_wifi_disconnect();
    esp_wifi_stop();

    wifi_config_t ap = {0};
    strlcpy((char *)ap.ap.ssid, CONFIG_RADAR_AP_SSID, sizeof(ap.ap.ssid));
    ap.ap.ssid_len = strlen(CONFIG_RADAR_AP_SSID);
    ap.ap.channel = 1;
    ap.ap.max_connection = 4;
    ap.ap.authmode = WIFI_AUTH_OPEN;
    // APSTA : le côté station sert uniquement à lister les réseaux pour la page web
    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_APSTA));
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_AP, &ap));
    ESP_ERROR_CHECK(esp_wifi_start());
    do_scan();

    http_start();
    xTaskCreatePinnedToCore(dns_task, "dns", 4096, NULL, 4, NULL, 0);
    xTaskCreatePinnedToCore(portal_watch_task, "portal", 3072, (void *)(intptr_t)has_saved_ssid, 3, NULL, 0);
    ESP_LOGI(TAG, "Point d'accès '%s' prêt : http://%s", CONFIG_RADAR_AP_SSID, RADAR_AP_IP);
}
