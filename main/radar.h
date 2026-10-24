// ⚫ AJOUTÉ (24/10/2026) : radar Wi-Fi CSI (Channel State Information) — types et API partagés.
// Chaîne de traitement :
//   radar_wifi.c : connexion Wi-Fi (ou écoute passive d'un point d'accès voisin), réception CSI,
//                  génération de trafic (ping de la passerelle) -> file d'échantillons d'amplitude ;
//   radar_dsp.c  : variance glissante des sous-porteuses, calibrage, machine d'états vide/présence/mouvement ;
//   radar_ui.c   : écran radar 480x480 avec silhouettes humaines (LVGL) ;
//   radar_led.c  : anneau de 13 LED reflétant l'état.
// Inspiré de ESPectre (variance glissante / "MVS"), WaveSight (auto-calibrage, sorties LED)
// et de l'exemple ESP-IDF esp-csi (ping de la passerelle pour obtenir un flux CSI régulier).
#pragma once

#include <stdbool.h>
#include <stdint.h>

// Sous-porteuses utiles du LLTF (20 MHz) : indices 2..26 et 38..63 du buffer CSI
// (0 = DC, 27..37 = bandes de garde, 1 parfois invalide : first_word_invalid).
#define RADAR_NSC 51

// Nombre de bandes de sous-porteuses utilisées pour la "direction" indicative affichée
#define RADAR_BANDS 8

typedef struct {
    uint8_t amp[RADAR_NSC];   // amplitude de chaque sous-porteuse (sqrt(I²+Q²), 0..181)
    int8_t rssi;              // dBm
} radar_sample_t;

typedef enum {
    RADAR_ST_WIFI = 0,    // connexion / recherche Wi-Fi en cours
    RADAR_ST_CALIB,       // calibrage (pièce vide)
    RADAR_ST_EMPTY,       // personne
    RADAR_ST_PRESENCE,    // présence (immobile ou peu de mouvement)
    RADAR_ST_MOTION,      // mouvement
    RADAR_ST_NOSIGNAL,    // plus de paquets CSI
} radar_state_t;

typedef struct {
    radar_state_t state;
    float score;          // perturbation / seuil (>= 1 : mouvement)
    float metric;         // variance moyenne normalisée (x1000)
    float threshold;      // seuil courant (même unité que metric)
    float angle_deg;      // signature spectrale -> angle d'affichage (INDICATIF, pas une vraie direction)
    // ⚫ AJOUTÉ (25/10/2026) : distances (mètres) pour l'affichage 2D / 3D
    float range_m;        // portée affichée (CONFIG_RADAR_RANGE_M)
    float dist_m;         // distance ESTIMÉE de la personne : plus la perturbation est forte, plus elle est proche
    float ap_dist_m;      // distance estimée du point d'accès d'après le RSSI (modèle log-distance), 0 = inconnue
    int sensitivity;      // 1..10
    int calib_pct;        // 0..100 pendant le calibrage
    int pps;              // paquets CSI par seconde
    int rssi;             // dBm (moyenne)
    int channel;
    bool passive;         // true : écoute passive d'un AP voisin, false : connecté (STA + ping)
    char ssid[33];
    char link_msg[48];    // message affiché pendant RADAR_ST_WIFI
    uint32_t motion_events;
} radar_status_t;

// radar_wifi.c
void radar_wifi_start(void);

// radar_dsp.c
void radar_dsp_start(void);
bool radar_dsp_push(const radar_sample_t *s);         // appelé depuis le callback CSI (non bloquant)
void radar_get_status(radar_status_t *out);           // copie thread-safe de l'état
void radar_set_link(const char *ssid, int channel, bool passive, const char *msg);
void radar_request_calibration(void);
void radar_set_sensitivity(int s);

// radar_ui.c (à appeler dans la tâche LVGL)
void radar_ui_init(void);

// radar_led.c
void radar_led_start(void);
