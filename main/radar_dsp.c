// ⚫ AJOUTÉ (24/10/2026) : radar Wi-Fi — traitement du signal CSI et détection.
// Algorithme (proche de la "Moving Variance Segmentation" d'ESPectre) :
//  1. chaque paquet : amplitudes des 51 sous-porteuses, divisées par leur moyenne (supprime les variations
//     de gain automatique AGC et de puissance d'émission, ne garde que la FORME du spectre) ;
//  2. fenêtre glissante de WIN paquets : variance temporelle de chaque sous-porteuse ;
//     metric = moyenne des variances x 1000. Un corps qui bouge modifie les trajets multiples -> la forme
//     du spectre fluctue -> la variance monte ;
//  3. calibrage pièce vide : seuil = percentile 95 du bruit x facteur de sensibilité ;
//  4. machine d'états avec hystérésis : MOUVEMENT (score >= 1), PRÉSENCE (activité récente ou micro-mouvements),
//     VIDE. La "direction" affichée = barycentre des bandes de sous-porteuses les plus perturbées : c'est une
//     signature spectrale, PAS une localisation (impossible avec une seule antenne / un seul lien).
#include <string.h>
#include <math.h>
#include <stdlib.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "esp_heap_caps.h"
#include "radar.h"

#define TAG "RADAR_DSP"

#define WIN            40      // paquets dans la fenêtre glissante (~0,4 s à 100 paquets/s)
#define TICK_MS        50      // période de la machine d'états
#define CAL_MAX        (CONFIG_RADAR_CALIB_SECONDS * 1000 / TICK_MS + 1)
#define MOTION_ON      1.0f    // score d'entrée en mouvement
#define MOTION_OFF     0.7f    // score de sortie (hystérésis)
#define MOTION_OFF_TICKS 10    // 0,5 s sous MOTION_OFF avant de quitter le mouvement
#define MICRO_MOTION   0.45f   // au-dessus : la présence est entretenue (personne assise qui respire, bouge un peu)
#define NOSIGNAL_US    3000000 // plus de paquet depuis 3 s
#define METRIC_FLOOR   0.02f   // seuil minimal (évite un seuil nul si le calibrage est parfaitement calme)
// ⚫ AJOUTÉ (25/10/2026) : estimation de distance (heuristique) : score 1 (juste au seuil) -> bord de la portée,
// score NEAR_SCORE et plus -> contre le kit, interpolation logarithmique entre les deux. La perturbation du CSI
// décroît avec l'éloignement du trajet kit <-> point d'accès : c'est un ORDRE DE GRANDEUR, pas une mesure.
#define NEAR_SCORE     10.0f
#define DIST_MIN_M     0.4f
// Modèle log-distance du RSSI du point d'accès : RSSI(d) = RSSI_1M - 10 * PATH_N * log10(d)
#define RSSI_1M        (-40.0f)
#define PATH_N         2.7f

static QueueHandle_t s_q;
static portMUX_TYPE s_lock = portMUX_INITIALIZER_UNLOCKED;
static radar_status_t s_st;            // protégé par s_lock

// Données propres à la tâche DSP (pas de verrou)
static float s_win[WIN][RADAR_NSC];
static int s_win_n, s_win_i;
static float s_metric_ema = -1.0f;
static float s_angle_deg;
static float s_rssi_ema;
static int s_pkt_count;
static int64_t s_last_pkt_us;

static volatile bool s_cal_request = true;   // calibrage au démarrage
static volatile int s_sensitivity = CONFIG_RADAR_DEFAULT_SENSITIVITY;
static float *s_cal_buf;
static int s_cal_n;
static bool s_calibrated;
static float s_base;                   // percentile 95 du bruit calibré

bool radar_dsp_push(const radar_sample_t *s) {
    return s_q && xQueueSend(s_q, s, 0) == pdTRUE;
}

void radar_get_status(radar_status_t *out) {
    portENTER_CRITICAL(&s_lock);
    *out = s_st;
    portEXIT_CRITICAL(&s_lock);
}

void radar_set_link(const char *ssid, int channel, bool passive, const char *msg) {
    portENTER_CRITICAL(&s_lock);
    strlcpy(s_st.ssid, ssid ? ssid : "", sizeof(s_st.ssid));
    s_st.channel = channel;
    s_st.passive = passive;
    strlcpy(s_st.link_msg, msg ? msg : "", sizeof(s_st.link_msg));
    portEXIT_CRITICAL(&s_lock);
}

void radar_request_calibration(void) {
    s_cal_request = true;
}

void radar_set_sensitivity(int s) {
    s_sensitivity = s < 1 ? 1 : (s > 10 ? 10 : s);
}

// Sensibilité 10 -> seuil = 1,2 x bruit ; 1 -> 3,45 x bruit
static float sens_factor(int s) {
    return 1.2f + (10 - s) * 0.25f;
}

static int cmp_float(const void *a, const void *b) {
    float fa = *(const float *)a, fb = *(const float *)b;
    return (fa > fb) - (fa < fb);
}

static void process_sample(const radar_sample_t *s) {
    float mean = 0;
    for (int k = 0; k < RADAR_NSC; k++) {
        mean += s->amp[k];
    }
    mean /= RADAR_NSC;
    s_last_pkt_us = esp_timer_get_time();
    s_pkt_count++;
    s_rssi_ema = (s_rssi_ema == 0) ? s->rssi : s_rssi_ema * 0.95f + s->rssi * 0.05f;
    if (mean < 2.0f) {
        return;   // paquet quasi vide (CSI non valide)
    }
    float inv = 1.0f / mean;
    for (int k = 0; k < RADAR_NSC; k++) {
        s_win[s_win_i][k] = s->amp[k] * inv;
    }
    s_win_i = (s_win_i + 1) % WIN;
    if (s_win_n < WIN) {
        s_win_n++;
        return;
    }

    float band[RADAR_BANDS] = {0};
    float total = 0;
    for (int k = 0; k < RADAR_NSC; k++) {
        float sum = 0, sum2 = 0;
        for (int i = 0; i < WIN; i++) {
            float v = s_win[i][k];
            sum += v;
            sum2 += v * v;
        }
        float m = sum / WIN;
        float var = sum2 / WIN - m * m;
        if (var < 0) {
            var = 0;
        }
        total += var;
        band[k * RADAR_BANDS / RADAR_NSC] += var;
    }
    float metric = total / RADAR_NSC * 1000.0f;
    s_metric_ema = (s_metric_ema < 0) ? metric : s_metric_ema * 0.8f + metric * 0.2f;

    // Barycentre circulaire des bandes perturbées -> angle d'affichage (lissé)
    float x = 0, y = 0;
    for (int b = 0; b < RADAR_BANDS; b++) {
        float a = b * (2.0f * (float)M_PI / RADAR_BANDS);
        x += band[b] * cosf(a);
        y += band[b] * sinf(a);
    }
    if (x != 0 || y != 0) {
        float target = atan2f(y, x) * 180.0f / (float)M_PI;
        float d = fmodf(target - s_angle_deg + 540.0f, 360.0f) - 180.0f;
        s_angle_deg = fmodf(s_angle_deg + 0.03f * d + 360.0f, 360.0f);
    }
}

static void dsp_task(void *arg) {
    radar_sample_t s;
    int64_t next_tick = esp_timer_get_time();
    int64_t next_sec = next_tick + 1000000;
    int64_t last_activity = INT64_MIN / 2;
    int64_t cal_start = 0;
    const int64_t cal_delay_us = (int64_t)CONFIG_RADAR_CALIB_DELAY_SECONDS * 1000000;
    const int64_t cal_len_us = (int64_t)CONFIG_RADAR_CALIB_SECONDS * 1000000;
    const int64_t hold_us = (int64_t)CONFIG_RADAR_PRESENCE_HOLD_SECONDS * 1000000;
    bool motion = false;
    int low_ticks = 0;
    // ⚫ AJOUTÉ (25/10/2026) : distance estimée, lissée (part du bord de la zone)
    const float range_m = CONFIG_RADAR_RANGE_M;
    float dist_m = range_m;
    int pps = 0;
    uint32_t events = 0;

    for (;;) {
        int64_t now = esp_timer_get_time();
        TickType_t wait = (next_tick > now) ? pdMS_TO_TICKS((next_tick - now) / 1000) : 0;
        if (xQueueReceive(s_q, &s, wait) == pdTRUE) {
            process_sample(&s);
            // vide la file sans repasser par l'attente
            while (xQueueReceive(s_q, &s, 0) == pdTRUE) {
                process_sample(&s);
            }
        }
        now = esp_timer_get_time();
        if (now < next_tick) {
            continue;
        }
        next_tick += TICK_MS * 1000;
        if (next_tick < now) {
            next_tick = now + TICK_MS * 1000;
        }

        if (now >= next_sec) {
            pps = s_pkt_count;
            s_pkt_count = 0;
            next_sec += 1000000;
        }

        radar_state_t state;
        int calib_pct = 0;
        float threshold = s_calibrated ? fmaxf(s_base, METRIC_FLOOR) * sens_factor(s_sensitivity) : 0;
        float score = 0;
        bool window_ready = (s_win_n >= WIN);

        if (s_cal_request && window_ready) {
            s_cal_request = false;
            s_cal_n = 0;
            cal_start = now;
            s_calibrated = false;
            motion = false;
            last_activity = INT64_MIN / 2;
            ESP_LOGI(TAG, "calibrage : quittez la pièce (%d s), mesure %d s",
                     CONFIG_RADAR_CALIB_DELAY_SECONDS, CONFIG_RADAR_CALIB_SECONDS);
        }

        if (!window_ready) {
            state = (s_last_pkt_us == 0) ? RADAR_ST_WIFI : RADAR_ST_CALIB;
        } else if (now - s_last_pkt_us > NOSIGNAL_US) {
            state = RADAR_ST_NOSIGNAL;
        } else if (!s_calibrated) {
            state = RADAR_ST_CALIB;
            int64_t el = now - cal_start;
            calib_pct = (int)(el * 100 / (cal_delay_us + cal_len_us));
            if (el >= cal_delay_us && s_cal_n < CAL_MAX) {
                s_cal_buf[s_cal_n++] = s_metric_ema;
            }
            if (el >= cal_delay_us + cal_len_us && s_cal_n > 0) {
                qsort(s_cal_buf, s_cal_n, sizeof(float), cmp_float);
                s_base = s_cal_buf[(s_cal_n * 95) / 100 < s_cal_n ? (s_cal_n * 95) / 100 : s_cal_n - 1];
                s_calibrated = true;
                calib_pct = 100;
                ESP_LOGI(TAG, "calibré : bruit p50=%.3f p95=%.3f (%d mesures)", s_cal_buf[s_cal_n / 2], s_base, s_cal_n);
            }
        } else {
            score = s_metric_ema / threshold;
            if (!motion && score >= MOTION_ON) {
                motion = true;
                low_ticks = 0;
                events++;
            } else if (motion) {
                low_ticks = (score < MOTION_OFF) ? low_ticks + 1 : 0;
                if (low_ticks >= MOTION_OFF_TICKS) {
                    motion = false;
                }
            }
            bool presence = (now - last_activity) < hold_us;
            if (motion || (presence && score >= MICRO_MOTION)) {
                last_activity = now;
                presence = true;
            }
            state = motion ? RADAR_ST_MOTION : (presence ? RADAR_ST_PRESENCE : RADAR_ST_EMPTY);
            // ⚫ AJOUTÉ (25/10/2026) : distance mise à jour seulement pendant un mouvement (score >= 1) ;
            // une personne immobile garde la dernière distance connue
            if (score >= MOTION_ON) {
                float target = range_m * (1.0f - logf(score) / logf(NEAR_SCORE));
                target = fminf(fmaxf(target, DIST_MIN_M), range_m);
                dist_m += (target - dist_m) * 0.1f;
            }
        }

        portENTER_CRITICAL(&s_lock);
        s_st.state = state;
        s_st.score = score;
        s_st.metric = s_metric_ema < 0 ? 0 : s_metric_ema;
        s_st.threshold = threshold;
        s_st.angle_deg = s_angle_deg;
        s_st.sensitivity = s_sensitivity;
        s_st.calib_pct = calib_pct > 100 ? 100 : calib_pct;
        s_st.pps = pps;
        s_st.rssi = (int)lroundf(s_rssi_ema);
        s_st.motion_events = events;
        // ⚫ AJOUTÉ (25/10/2026)
        s_st.range_m = range_m;
        s_st.dist_m = dist_m;
        s_st.ap_dist_m = (s_rssi_ema < 0) ? powf(10.0f, (RSSI_1M - s_rssi_ema) / (10.0f * PATH_N)) : 0;
        portEXIT_CRITICAL(&s_lock);

        static int log_div;
        if (++log_div >= 1000 / TICK_MS) {
            log_div = 0;
            static const char *names[] = {"WIFI", "CALIB", "VIDE", "PRESENCE", "MOUVEMENT", "SANS_SIGNAL"};
            // ESP_LOGI(TAG, "%s metric=%.3f seuil=%.3f score=%.2f pps=%d rssi=%d angle=%.0f",
            //          names[state], s_st.metric, threshold, score, pps, s_st.rssi, s_angle_deg);
            // ⚫ MODIFIÉ (25/10/2026) : + distance estimée de la personne et du point d'accès
            ESP_LOGI(TAG, "%s metric=%.3f seuil=%.3f score=%.2f pps=%d rssi=%d angle=%.0f dist=%.1fm ap=%.1fm",
                     names[state], s_st.metric, threshold, score, pps, s_st.rssi, s_angle_deg,
                     dist_m, s_st.ap_dist_m);
        }
    }
}

void radar_dsp_start(void) {
    s_q = xQueueCreate(64, sizeof(radar_sample_t));
    s_cal_buf = heap_caps_malloc(CAL_MAX * sizeof(float), MALLOC_CAP_DEFAULT);
    assert(s_q && s_cal_buf);
    s_st.state = RADAR_ST_WIFI;
    s_st.sensitivity = s_sensitivity;
    strlcpy(s_st.link_msg, "Démarrage Wi-Fi...", sizeof(s_st.link_msg));
    // Cœur 0 (avec le Wi-Fi), LVGL est seul sur le cœur 1
    xTaskCreatePinnedToCore(dsp_task, "radar_dsp", 4096, NULL, 4, NULL, 0);
}
