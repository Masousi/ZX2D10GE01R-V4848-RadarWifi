// ⚫ AJOUTÉ (24/10/2026) : radar Wi-Fi — anneau de 13 LED WS2812 (initialisé par main.c).
//  connexion / calibrage : LED ambre qui tourne (calibrage : remplissage selon l'avancement)
//  vide      : comète verte qui tourne, synchronisée à l'œil avec le balayage de l'écran
//  présence  : bleu qui respire
//  mouvement : rouge, nombre de LED allumées proportionnel à l'intensité
//  pas de signal : magenta clignotant
#include <math.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "ws2812.h"
#include "radar.h"

#define LED_MS 30

static void put(int i, float r, float g, float b, float k) {
    const float max = CONFIG_RADAR_LED_BRIGHTNESS;
    k = fminf(fmaxf(k, 0), 1);
    ws2812_set(i, (uint8_t)(r * k * max), (uint8_t)(g * k * max), (uint8_t)(b * k * max));
}

static void led_task(void *arg) {
    const int n = ws2812_count();
    uint32_t t = 0;
    radar_status_t st;
    for (;;) {
        radar_get_status(&st);
        t++;
        ws2812_clear();
        float pos = fmodf(t * LED_MS / 2700.0f, 1.0f) * n;   // un tour en 2,7 s (comme le balayage)
        switch (st.state) {
        case RADAR_ST_WIFI:
            put((int)pos % n, 1, 0.55f, 0, 0.8f);
            break;
        case RADAR_ST_CALIB: {
            int lit = st.calib_pct * n / 100;
            for (int i = 0; i < lit; i++) {
                put(i, 1, 0.55f, 0, 0.35f);
            }
            put((int)pos % n, 1, 0.55f, 0, 1);
            break;
        }
        case RADAR_ST_EMPTY:
            for (int k = 0; k < 4; k++) {
                put(((int)pos - k + n) % n, 0, 1, 0.3f, 0.6f / (1 + 2 * k));
            }
            break;
        case RADAR_ST_PRESENCE: {
            float br = 0.25f + 0.35f * (0.5f + 0.5f * sinf(t * LED_MS / 1000.0f * 2 * (float)M_PI / 4.0f));
            for (int i = 0; i < n; i++) {
                put(i, 0.1f, 0.6f, 1, br);
            }
            break;
        }
        case RADAR_ST_MOTION: {
            int lit = (int)fminf(fmaxf(st.score * 4, 1), n);
            float flick = 0.75f + 0.25f * ((t / 3) % 2);
            for (int i = 0; i < lit; i++) {
                put(i, 1, 0.12f, 0.05f, flick);
            }
            break;
        }
        case RADAR_ST_NOSIGNAL:
            if ((t * LED_MS / 500) % 2) {
                for (int i = 0; i < n; i++) {
                    put(i, 1, 0, 0.6f, 0.4f);
                }
            }
            break;
        }
        ws2812_show();
        vTaskDelay(pdMS_TO_TICKS(LED_MS));
    }
}

void radar_led_start(void) {
    if (ws2812_count() > 0) {
        xTaskCreatePinnedToCore(led_task, "radar_led", 3072, NULL, 3, NULL, 0);
    }
}
