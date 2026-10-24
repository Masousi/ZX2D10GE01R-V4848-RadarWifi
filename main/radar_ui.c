// ⚫ AJOUTÉ (24/10/2026) : radar Wi-Fi — écran radar rond 480x480 avec silhouettes humaines (LVGL 8.3).
//  - fond radar : disque, anneaux, réticule, balayage rotatif ;
//  - silhouette humaine (tête + tronc + bras/jambes articulés en lignes arrondies) :
//      PRÉSENCE  -> silhouette debout, bleue, qui "respire" ;
//      MOUVEMENT -> silhouette rouge qui marche + échos (silhouettes plus sombres aux positions précédentes) ;
//      VIDE      -> aucune silhouette ;
//    la silhouette s'illumine quand le balayage passe dessus ;
//    sa position = signature spectrale (angle) et intensité (distance au centre) : INDICATIVE, une seule carte
//    ESP32 ne localise pas une personne ;
//  - courbe du score (seuil = ligne pointillée), RSSI, paquets/s, sensibilité ;
//  - encodeur : rotation = sensibilité 1..10, appui = recalibrage (lus directement, sans groupe LVGL).
// ⚫ MODIFIÉ (25/10/2026) : deux vues, bascule par appui long (1,5 s) sur l'encodeur :
//  - VUE 3D (par défaut, CONFIG_RADAR_VIEW_3D) : le kit au centre d'un sol en perspective, anneaux de distance
//    gradués en mètres jusqu'à CONFIG_RADAR_RANGE_M, onde qui s'élargit depuis le kit, balayage au sol,
//    silhouettes debout sur le sol, plus petites quand elles sont loin, ombre au sol, point d'accès (box)
//    placé à sa distance estimée par le RSSI ;
//  - VUE 2D : le radar d'origine, anneaux désormais gradués en mètres.
//  Distance au kit = distance ESTIMÉE (radar_dsp.c : plus la perturbation est forte, plus la personne est
//  proche), affichée au-dessus de la silhouette ("~3,5 m"). L'angle reste indicatif.
#include <math.h>
#include <stdio.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "driver/gpio.h"
#include "esp_log.h"
#include "lvgl.h"
#include "board.h"
#include "mt8901.h"
#include "radar.h"

LV_FONT_DECLARE(font_fr_20);
LV_FONT_DECLARE(font_fr_28);

#define TAG         "RADAR_UI"

#define CX          240
#define CY          228        // centre du radar (un peu au-dessus du centre de l'écran : place pour la courbe)
#define R_SWEEP     220
#define FIG_W       56
#define FIG_H       100
#define N_ECHO      3
#define CHART_PTS   60
#define UI_MS       40

// ⚫ AJOUTÉ (25/10/2026) : vue 2D graduée en mètres : la portée correspond à R2D pixels
#define R2D         200
// ⚫ AJOUTÉ (25/10/2026) : vue 3D. Caméra en hauteur regardant le kit (origine) avec une inclinaison de P3_E,
// à P3_DC portées de distance ; coordonnées du monde en fraction de la portée (1 = anneau extérieur).
// Réglages choisis pour que l'anneau extérieur tienne dans l'écran rond (y de 148 à 380 environ).
#define P3_E_DEG    32.0f
#define P3_DC       2.0f
#define P3_F        360.0f
#define P3_CY       215
#define RING_PTS    36         // segments par anneau (rendu logiciel plein écran : rester sobre)
#define HUMAN_M     1.7f       // taille d'une personne
#define HUMAN_EXAG  1.6f       // silhouettes agrandies pour rester lisibles sur 2,1"
#define LONG_PRESS_MS 1500
#define HINT_MS     8000

#define COL_GRID    lv_color_hex(0x0b4a24)
#define COL_SWEEP   lv_color_hex(0x22ff88)
#define COL_EMPTY   lv_color_hex(0x19e36b)
#define COL_PRES    lv_color_hex(0x36c8ff)
#define COL_MOTION  lv_color_hex(0xff4d2e)
#define COL_WARN    lv_color_hex(0xffb000)
#define COL_ALERT   lv_color_hex(0xff2e63)
#define COL_TEXT    lv_color_hex(0x9fd8b4)
// ⚫ AJOUTÉ (25/10/2026)
#define COL_RING_LBL lv_color_hex(0x3f9a63)

typedef struct {
    lv_obj_t *cont;
    lv_obj_t *head;
    lv_obj_t *torso, *arm_l, *arm_r, *leg_l, *leg_r;
    lv_point_t p_torso[2], p_arm_l[3], p_arm_r[3], p_leg_l[3], p_leg_r[3];
    lv_color_t color;
    float ang_deg;             // angle à l'écran (pour l'illumination par le balayage)
    float scale;               // ⚫ AJOUTÉ (25/10/2026) : 1 = 56x100 px (vue 3D : selon la profondeur)
} figure_t;

// ⚫ MODIFIÉ (25/10/2026) : objets de la vue 2D rangés dans le conteneur s_v2d
static figure_t s_main, s_echo[N_ECHO];
static lv_obj_t *s_sweep[3];
static lv_point_t s_sweep_pts[3][2];
static lv_obj_t *s_lbl_state, *s_lbl_info, *s_lbl_msg, *s_lbl_link, *s_lbl_hint;
static lv_obj_t *s_chart;
static lv_chart_series_t *s_ser_score, *s_ser_thr;

// ⚫ AJOUTÉ (25/10/2026) : vues et objets de la vue 3D
static lv_obj_t *s_v2d, *s_v3d;
static bool s_view3d = CONFIG_RADAR_VIEW_3D;
static figure_t s_main3, s_echo3[N_ECHO];
static lv_obj_t *s_sweep3[3];
static lv_point_t s_sweep3_pts[3][2];
static lv_obj_t *s_wave3;
static lv_point_t s_wave3_pts[RING_PTS + 1];
static lv_point_t s_ring3_pts[4][RING_PTS + 1];
static lv_point_t s_radial3_pts[4][2];
static lv_obj_t *s_shadow3, *s_ap3, *s_ap3_lbl;
static lv_obj_t *s_lbl_dist;
static float s_se, s_ce;   // sin / cos de l'inclinaison de la caméra

static float s_sweep_deg;
static float s_walk_phase;
// static float s_fig_x = CX, s_fig_y = CY;
// static float s_echo_x[N_ECHO], s_echo_y[N_ECHO], s_echo_phase[N_ECHO];
// ⚫ MODIFIÉ (25/10/2026) : positions en mètres dans le plan du sol (x = droite, z = loin), communes aux 2 vues
static float s_wx, s_wz;
static float s_echo_x[N_ECHO], s_echo_z[N_ECHO], s_echo_phase[N_ECHO];
static int s_echo_n;
static uint32_t s_ticks;
static int s_toast_ticks;
static int16_t s_enc_last;
// static int s_btn_ticks;
// ⚫ MODIFIÉ (25/10/2026) : durée d'appui mesurée en ms (appui court = recalibrage, appui long = changer de vue)
static uint32_t s_btn_down_ms;
static bool s_btn_down, s_btn_long_done;
static uint32_t s_hint_until_ms;

// -----------------------------------------------------------------------------------------------------------
// Silhouette
// -----------------------------------------------------------------------------------------------------------

static lv_obj_t *limb_create(lv_obj_t *parent, int width) {
    lv_obj_t *l = lv_line_create(parent);
    lv_obj_remove_style_all(l);
    lv_obj_set_style_line_width(l, width, 0);
    lv_obj_set_style_line_rounded(l, true, 0);
    lv_obj_clear_flag(l, LV_OBJ_FLAG_CLICKABLE | LV_OBJ_FLAG_SCROLLABLE);
    return l;
}

// ⚫ AJOUTÉ (25/10/2026) : mise à l'échelle (taille du conteneur, de la tête, épaisseur des membres)
static void figure_set_scale(figure_t *f, float s) {
    if (fabsf(s - f->scale) < 0.02f) {
        return;
    }
    f->scale = s;
    lv_obj_set_size(f->cont, (lv_coord_t)lroundf(FIG_W * s), (lv_coord_t)lroundf(FIG_H * s));
    lv_coord_t hd = (lv_coord_t)lroundf(20 * s);
    lv_obj_set_size(f->head, hd, hd);
    lv_obj_set_style_line_width(f->torso, LV_MAX(3, (int)lroundf(13 * s)), 0);
    lv_obj_set_style_line_width(f->leg_l, LV_MAX(2, (int)lroundf(8 * s)), 0);
    lv_obj_set_style_line_width(f->leg_r, LV_MAX(2, (int)lroundf(8 * s)), 0);
    lv_obj_set_style_line_width(f->arm_l, LV_MAX(2, (int)lroundf(6 * s)), 0);
    lv_obj_set_style_line_width(f->arm_r, LV_MAX(2, (int)lroundf(6 * s)), 0);
}

static void figure_create(figure_t *f, lv_obj_t *parent) {
    f->cont = lv_obj_create(parent);
    lv_obj_remove_style_all(f->cont);
    lv_obj_set_size(f->cont, FIG_W, FIG_H);
    lv_obj_clear_flag(f->cont, LV_OBJ_FLAG_CLICKABLE | LV_OBJ_FLAG_SCROLLABLE);
    // Membres dessinés avant la tête et le tronc
    f->arm_l = limb_create(f->cont, 6);
    f->leg_l = limb_create(f->cont, 8);
    f->leg_r = limb_create(f->cont, 8);
    f->torso = limb_create(f->cont, 13);
    f->arm_r = limb_create(f->cont, 6);
    f->head = lv_obj_create(f->cont);
    lv_obj_remove_style_all(f->head);
    lv_obj_set_size(f->head, 20, 20);
    lv_obj_set_style_radius(f->head, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_bg_opa(f->head, LV_OPA_COVER, 0);
    lv_obj_clear_flag(f->head, LV_OBJ_FLAG_CLICKABLE | LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(f->cont, LV_OBJ_FLAG_HIDDEN);
    // ⚫ AJOUTÉ (25/10/2026)
    f->scale = 0;
    figure_set_scale(f, 1.0f);
}

static void figure_set_color(figure_t *f, lv_color_t c) {
    if (f->color.full == c.full) {
        return;
    }
    f->color = c;
    lv_obj_set_style_bg_color(f->head, c, 0);
    lv_obj_t *limbs[] = {f->torso, f->arm_l, f->arm_r, f->leg_l, f->leg_r};
    for (int i = 0; i < 5; i++) {
        lv_obj_set_style_line_color(limbs[i], c, 0);
    }
}

// Segment articulé : départ (x0,y0), angle depuis la verticale descendante (degrés, + = vers l'avant)
// ⚫ MODIFIÉ (25/10/2026) : coordonnées de base (silhouette 56x100) multipliées par l'échelle s
static void joint(lv_point_t *p, float x0, float y0, float a1, float l1, float a2, float l2, int dir, float s) {
    float x1 = x0 + dir * l1 * sinf(a1 * (float)M_PI / 180);
    float y1 = y0 + l1 * cosf(a1 * (float)M_PI / 180);
    float x2 = x1 + dir * l2 * sinf(a2 * (float)M_PI / 180);
    float y2 = y1 + l2 * cosf(a2 * (float)M_PI / 180);
    p[0] = (lv_point_t){(lv_coord_t)lroundf(x0 * s), (lv_coord_t)lroundf(y0 * s)};
    p[1] = (lv_point_t){(lv_coord_t)lroundf(x1 * s), (lv_coord_t)lroundf(y1 * s)};
    p[2] = (lv_point_t){(lv_coord_t)lroundf(x2 * s), (lv_coord_t)lroundf(y2 * s)};
}

// walking : cycle de marche (phase en radians) ; sinon posture debout avec respiration
static void figure_pose(figure_t *f, float phase, bool walking, int dir, float breathe) {
    const float s = f->scale;
    const int cx = FIG_W / 2;
    int bob = walking ? (int)lroundf(2 * fabsf(sinf(phase))) : (int)lroundf(breathe);
    int neck = 24 - bob, hip = 58 - bob / 2;
    lv_obj_set_pos(f->head, (lv_coord_t)lroundf((cx - 10) * s), (lv_coord_t)lroundf((2 - bob) * s));
    f->p_torso[0] = (lv_point_t){(lv_coord_t)lroundf(cx * s), (lv_coord_t)lroundf((neck + 4) * s)};
    f->p_torso[1] = (lv_point_t){(lv_coord_t)lroundf(cx * s), (lv_coord_t)lroundf(hip * s)};
    float leg = walking ? 28 * sinf(phase) : 7;
    float arm = walking ? 26 * sinf(phase) : 10 + breathe;
    float knee_l = walking ? fmaxf(0, -30 * cosf(phase)) : 0;
    float knee_r = walking ? fmaxf(0, 30 * cosf(phase)) : 0;
    joint(f->p_leg_l, cx, hip, leg, 20, leg - knee_l, 19, dir, s);
    joint(f->p_leg_r, cx, hip, -leg, 20, -leg - knee_r, 19, dir, s);
    joint(f->p_arm_l, cx, neck + 6, -arm, 17, -arm + (walking ? 25 : 5), 15, dir, s);
    joint(f->p_arm_r, cx, neck + 6, arm, 17, arm + (walking ? 25 : 5), 15, walking ? dir : -dir, s);
    lv_line_set_points(f->torso, f->p_torso, 2);
    lv_line_set_points(f->leg_l, f->p_leg_l, 3);
    lv_line_set_points(f->leg_r, f->p_leg_r, 3);
    lv_line_set_points(f->arm_l, f->p_arm_l, 3);
    lv_line_set_points(f->arm_r, f->p_arm_r, 3);
}

// Vue 2D : silhouette centrée sur (x, y)
static void figure_place(figure_t *f, float x, float y) {
    lv_obj_set_pos(f->cont, (lv_coord_t)lroundf(x) - FIG_W / 2, (lv_coord_t)lroundf(y) - FIG_H / 2);
    // f->ang_deg = fmodf(atan2f(y - CY, x - CX) * 180 / (float)M_PI + 360, 360);
    // ⚫ MODIFIÉ (25/10/2026) : angle du monde (y écran vers le bas = z vers le loin inversé)
    f->ang_deg = fmodf(atan2f(CY - y, x - CX) * 180 / (float)M_PI + 360, 360);
}

// ⚫ AJOUTÉ (25/10/2026) : vue 3D : pieds de la silhouette en (sx, sy), angle du monde ang_deg
static void figure_place_feet(figure_t *f, int sx, int sy, float ang_deg) {
    lv_coord_t w = (lv_coord_t)lroundf(FIG_W * f->scale), h = (lv_coord_t)lroundf(FIG_H * f->scale);
    lv_obj_set_pos(f->cont, sx - w / 2, sy - h);
    f->ang_deg = ang_deg;
}

static void figure_show(figure_t *f, bool show) {
    if (show == lv_obj_has_flag(f->cont, LV_OBJ_FLAG_HIDDEN)) {
        if (show) {
            lv_obj_clear_flag(f->cont, LV_OBJ_FLAG_HIDDEN);
        } else {
            lv_obj_add_flag(f->cont, LV_OBJ_FLAG_HIDDEN);
        }
    }
}

// ⚫ AJOUTÉ (25/10/2026)
static void obj_show(lv_obj_t *o, bool show) {
    if (show == lv_obj_has_flag(o, LV_OBJ_FLAG_HIDDEN)) {
        if (show) {
            lv_obj_clear_flag(o, LV_OBJ_FLAG_HIDDEN);
        } else {
            lv_obj_add_flag(o, LV_OBJ_FLAG_HIDDEN);
        }
    }
}

// Luminosité selon le retard du balayage sur la silhouette (écho radar qui s'estompe après le passage)
static lv_color_t lit(lv_color_t base, float fig_ang, float dim_floor) {
    float d = fmodf(s_sweep_deg - fig_ang + 360, 360);   // 0 = le balayage vient de passer
    float k = 1.0f - d / 360.0f;
    k = dim_floor + (1 - dim_floor) * k * k;
    return lv_color_mix(base, lv_color_black(), (lv_opa_t)(k * 255));
}

// ⚫ AJOUTÉ (25/10/2026) : "~3,5 m" sans printf flottant (LV_SPRINTF_USE_FLOAT désactivé)
static void fmt_m(char *buf, size_t n, const char *prefix, float m) {
    int d10 = (int)lroundf(m * 10);
    if (d10 % 10 == 0) {
        snprintf(buf, n, "%s%d m", prefix, d10 / 10);
    } else {
        snprintf(buf, n, "%s%d,%d m", prefix, d10 / 10, d10 % 10);
    }
}

// -----------------------------------------------------------------------------------------------------------
// Fond radar
// -----------------------------------------------------------------------------------------------------------

static lv_obj_t *circle(lv_obj_t *parent, int r, lv_color_t border, int bw, lv_color_t bg, lv_opa_t bg_opa) {
    lv_obj_t *c = lv_obj_create(parent);
    lv_obj_remove_style_all(c);
    lv_obj_set_size(c, 2 * r, 2 * r);
    lv_obj_set_pos(c, CX - r, CY - r);
    lv_obj_set_style_radius(c, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_border_color(c, border, 0);
    lv_obj_set_style_border_width(c, bw, 0);
    lv_obj_set_style_bg_color(c, bg, 0);
    lv_obj_set_style_bg_opa(c, bg_opa, 0);
    lv_obj_clear_flag(c, LV_OBJ_FLAG_CLICKABLE | LV_OBJ_FLAG_SCROLLABLE);
    return c;
}

static lv_obj_t *line(lv_obj_t *parent, lv_point_t *pts, int n, lv_color_t col, int w) {
    lv_obj_t *l = lv_line_create(parent);
    lv_obj_remove_style_all(l);
    lv_obj_set_style_line_color(l, col, 0);
    lv_obj_set_style_line_width(l, w, 0);
    lv_obj_set_style_line_rounded(l, true, 0);
    lv_obj_clear_flag(l, LV_OBJ_FLAG_CLICKABLE | LV_OBJ_FLAG_SCROLLABLE);
    lv_line_set_points(l, pts, n);
    return l;
}

static lv_obj_t *label(lv_obj_t *parent, const lv_font_t *font, lv_color_t col, lv_align_t align, int y) {
    lv_obj_t *l = lv_label_create(parent);
    lv_obj_set_style_text_font(l, font, 0);
    lv_obj_set_style_text_color(l, col, 0);
    lv_obj_set_style_text_align(l, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_align(l, align, 0, y);
    lv_label_set_text(l, "");
    return l;
}

// ⚫ AJOUTÉ (25/10/2026) : rectangle / ellipse aplatie (pilule) plein
static lv_obj_t *box(lv_obj_t *parent, int w, int h, int radius, lv_color_t bg, lv_color_t border, int bw) {
    lv_obj_t *o = lv_obj_create(parent);
    lv_obj_remove_style_all(o);
    lv_obj_set_size(o, w, h);
    lv_obj_set_style_radius(o, radius, 0);
    lv_obj_set_style_bg_color(o, bg, 0);
    lv_obj_set_style_bg_opa(o, LV_OPA_COVER, 0);
    lv_obj_set_style_border_color(o, border, 0);
    lv_obj_set_style_border_width(o, bw, 0);
    lv_obj_clear_flag(o, LV_OBJ_FLAG_CLICKABLE | LV_OBJ_FLAG_SCROLLABLE);
    return o;
}

static void sweep_update(void) {
    static const float lag[3] = {0, 7, 14};
    for (int i = 0; i < 3; i++) {
        float a = (s_sweep_deg - lag[i]) * (float)M_PI / 180;
        s_sweep_pts[i][0] = (lv_point_t){CX, CY};
        // ⚫ MODIFIÉ (25/10/2026) : sens trigonométrique (y écran inversé), comme l'angle du monde et la vue 3D
        s_sweep_pts[i][1] = (lv_point_t){CX + (lv_coord_t)lroundf(R_SWEEP * cosf(a)),
                                         CY - (lv_coord_t)lroundf(R_SWEEP * sinf(a))};
        lv_line_set_points(s_sweep[i], s_sweep_pts[i], 2);
    }
}

// -----------------------------------------------------------------------------------------------------------
// ⚫ AJOUTÉ (25/10/2026) : projection perspective de la vue 3D
// -----------------------------------------------------------------------------------------------------------

// (x, y, z) en fraction de la portée : x = droite, y = haut, z = loin. Renvoie la profondeur.
static float project(float x, float y, float z, lv_point_t *p) {
    float dy = y - P3_DC * s_se;
    float dz = z + P3_DC * s_ce;
    float depth = -dy * s_se + dz * s_ce;
    float ys = dy * s_ce + dz * s_se;
    p->x = CX + (lv_coord_t)lroundf(P3_F * x / depth);
    p->y = P3_CY - (lv_coord_t)lroundf(P3_F * ys / depth);
    return depth;
}

static void ring3(lv_point_t *pts, float r) {
    for (int k = 0; k <= RING_PTS; k++) {
        float a = k * 2 * (float)M_PI / RING_PTS;
        project(r * cosf(a), 0, r * sinf(a), &pts[k]);
    }
}

static void sweep3_update(void) {
    static const float lag[3] = {0, 7, 14};
    for (int i = 0; i < 3; i++) {
        float a = (s_sweep_deg - lag[i]) * (float)M_PI / 180;
        project(0, 0, 0, &s_sweep3_pts[i][0]);
        project(cosf(a), 0, sinf(a), &s_sweep3_pts[i][1]);
        lv_line_set_points(s_sweep3[i], s_sweep3_pts[i], 2);
    }
    // Onde qui part du kit et s'élargit jusqu'au bord en 2,4 s, en s'assombrissant
    float w = fmodf(s_ticks * UI_MS / 2400.0f, 1.0f);
    ring3(s_wave3_pts, fmaxf(w, 0.02f));
    lv_line_set_points(s_wave3, s_wave3_pts, RING_PTS + 1);
    lv_obj_set_style_line_color(s_wave3, lv_color_mix(COL_SWEEP, lv_color_black(), (lv_opa_t)(200 * (1 - w))), 0);
}

// Échelle d'une silhouette dont les pieds sont à la profondeur depth
static float human_scale(float depth, float range_m) {
    float h_px = P3_F * (HUMAN_M / range_m) / depth * HUMAN_EXAG;
    return fminf(fmaxf(h_px / FIG_H, 0.3f), 1.3f);
}

// -----------------------------------------------------------------------------------------------------------
// Encodeur / bouton (lecture directe)
// -----------------------------------------------------------------------------------------------------------

// ⚫ AJOUTÉ (25/10/2026)
static void set_view(bool v3d) {
    s_view3d = v3d;
    obj_show(s_v3d, v3d);
    obj_show(s_v2d, !v3d);
    s_hint_until_ms = lv_tick_get() + 3000;
    ESP_LOGI(TAG, "vue %s", v3d ? "3D" : "2D");
}

static void input_poll(const radar_status_t *st) {
    int16_t c = mt8901_get_count();
    int16_t d = c - s_enc_last;
    s_enc_last = c;
    if (d != 0) {
        // même sens que l'encodeur LVGL de screen.c (compteur décroissant = sens horaire)
        radar_set_sensitivity(st->sensitivity + (d > 0 ? -1 : 1));
        s_toast_ticks = 1500 / UI_MS;
    }
    // Bouton actif bas : appui puis relâchement = recalibrage
    // if (gpio_get_level(BTN_PIN) == 0) {
    //     s_btn_ticks++;
    // } else {
    //     if (s_btn_ticks * UI_MS >= 60) {
    //         radar_request_calibration();
    //     }
    //     s_btn_ticks = 0;
    // }
    // ⚫ MODIFIÉ (25/10/2026) : appui court (relâché avant 1,5 s) = recalibrage ; appui long = changer de vue
    uint32_t now = lv_tick_get();
    if (gpio_get_level(BTN_PIN) == 0) {
        if (!s_btn_down) {
            s_btn_down = true;
            s_btn_long_done = false;
            s_btn_down_ms = now;
        } else if (!s_btn_long_done && now - s_btn_down_ms >= LONG_PRESS_MS) {
            s_btn_long_done = true;
            set_view(!s_view3d);
        }
    } else if (s_btn_down) {
        s_btn_down = false;
        if (!s_btn_long_done && now - s_btn_down_ms >= 60) {
            radar_request_calibration();
        }
    }
}

// -----------------------------------------------------------------------------------------------------------
// Rafraîchissement
// -----------------------------------------------------------------------------------------------------------

// ⚫ AJOUTÉ (25/10/2026) : mise à jour de la vue 3D (sol, kit, box, silhouettes)
static void view3d_update(const radar_status_t *st, bool present, bool moving, int dir, float breathe) {
    sweep3_update();
    char buf[24];

    // Point d'accès à sa distance estimée (direction inconnue : placé en arrière-gauche du kit)
    if (s_ticks % 5 == 0) {
        bool show = st->ap_dist_m > 0 && st->state != RADAR_ST_WIFI;
        obj_show(s_ap3, show);
        obj_show(s_ap3_lbl, show);
        if (show) {
            float r = fminf(fmaxf(st->ap_dist_m / st->range_m, 0.12f), 1.0f);
            float a = 115 * (float)M_PI / 180;
            lv_point_t p;
            project(r * cosf(a), 0, r * sinf(a), &p);
            lv_obj_set_pos(s_ap3, p.x - 9, p.y - 12);
            fmt_m(buf, sizeof(buf), st->passive ? "AP ~" : "box ~", st->ap_dist_m);
            lv_label_set_text(s_ap3_lbl, buf);
            lv_obj_align(s_ap3_lbl, LV_ALIGN_TOP_MID, p.x - CX, p.y - 30);
        }
    }

    // Échos : couleur selon le balayage (positions / poses fixées lors de leur création)
    for (int i = 0; i < N_ECHO; i++) {
        figure_show(&s_echo3[i], i < s_echo_n);
        if (i < s_echo_n) {
            static const uint8_t dim[N_ECHO] = {120, 75, 40};
            lv_color_t base = lv_color_mix(COL_MOTION, lv_color_black(), dim[i]);
            figure_set_color(&s_echo3[i], lit(base, s_echo3[i].ang_deg, 0.6f));
        }
    }

    figure_show(&s_main3, present);
    obj_show(s_shadow3, present);
    obj_show(s_lbl_dist, present);
    if (present) {
        lv_point_t p;
        float depth = project(s_wx / st->range_m, 0, s_wz / st->range_m, &p);
        float ang = fmodf(atan2f(s_wz, s_wx) * 180 / (float)M_PI + 360, 360);
        figure_set_scale(&s_main3, human_scale(depth, st->range_m));
        figure_pose(&s_main3, s_walk_phase, moving, dir, breathe);
        figure_place_feet(&s_main3, p.x, p.y, ang);
        lv_color_t c = lit(moving ? COL_MOTION : COL_PRES, ang, 0.55f);
        figure_set_color(&s_main3, c);
        // Ombre au sol sous les pieds
        lv_coord_t sw = (lv_coord_t)lroundf(34 * s_main3.scale), sh = (lv_coord_t)LV_MAX(4, lroundf(10 * s_main3.scale));
        lv_obj_set_size(s_shadow3, sw, sh);
        lv_obj_set_pos(s_shadow3, p.x - sw / 2, p.y - sh / 2);
        lv_obj_set_style_bg_color(s_shadow3, lv_color_mix(moving ? COL_MOTION : COL_PRES, lv_color_black(), 90), 0);
        fmt_m(buf, sizeof(buf), "~", st->dist_m);
        lv_label_set_text(s_lbl_dist, buf);
        lv_obj_align(s_lbl_dist, LV_ALIGN_TOP_MID, p.x - CX, p.y - (lv_coord_t)lroundf(FIG_H * s_main3.scale) - 24);
    }
}

// ⚫ AJOUTÉ (25/10/2026) : vue 2D (code d'origine déplacé depuis ui_timer_cb), positions converties des mètres
static void view2d_update(const radar_status_t *st, bool present, bool moving, int dir, float breathe) {
    sweep_update();
    const float k = R2D / st->range_m;   // pixels par mètre
    for (int i = 0; i < N_ECHO; i++) {
        figure_show(&s_echo[i], i < s_echo_n);
        if (i < s_echo_n) {
            static const uint8_t dim[N_ECHO] = {120, 75, 40};
            lv_color_t base = lv_color_mix(COL_MOTION, lv_color_black(), dim[i]);
            figure_set_color(&s_echo[i], lit(base, s_echo[i].ang_deg, 0.6f));
        }
    }

    figure_show(&s_main, present);
    obj_show(s_lbl_dist, present);
    if (present) {
        float x = CX + s_wx * k, y = CY - s_wz * k;
        figure_pose(&s_main, s_walk_phase, moving, dir, breathe);
        figure_place(&s_main, x, y);
        figure_set_color(&s_main, lit(moving ? COL_MOTION : COL_PRES, s_main.ang_deg, 0.55f));
        char buf[24];
        fmt_m(buf, sizeof(buf), "~", st->dist_m);
        lv_label_set_text(s_lbl_dist, buf);
        lv_obj_align(s_lbl_dist, LV_ALIGN_TOP_MID, (lv_coord_t)lroundf(x) - CX, (lv_coord_t)lroundf(y) - FIG_H / 2 - 24);
    }
}

static void ui_timer_cb(lv_timer_t *t) {
    radar_status_t st;
    radar_get_status(&st);
    input_poll(&st);
    radar_get_status(&st);   // sensibilité éventuellement modifiée
    s_ticks++;
    if (st.range_m <= 0) {
        st.range_m = CONFIG_RADAR_RANGE_M;   // ⚫ AJOUTÉ (25/10/2026) : avant la 1re mise à jour du DSP
    }

    // ⚫ AJOUTÉ (25/10/2026) : images/s réellement affichées (rendu plein écran), dans le journal toutes les 10 s
    static uint32_t fps_t0, fps_n;
    fps_n++;
    if (lv_tick_get() - fps_t0 >= 10000) {
        ESP_LOGI(TAG, "vue %s : %u images/s", s_view3d ? "3D" : "2D", (unsigned)(fps_n / 10));
        fps_t0 = lv_tick_get();
        fps_n = 0;
    }

    s_sweep_deg = fmodf(s_sweep_deg + 5.0f, 360.0f);
    // sweep_update();
    // ⚫ MODIFIÉ (25/10/2026) : balayage mis à jour par la vue active (view2d_update / view3d_update)

    // --- Libellés d'état ---
    static radar_state_t last_state = (radar_state_t)-1;
    if (st.state != last_state) {
        last_state = st.state;
        static const char *txt[] = {"CONNEXION", "CALIBRAGE", "PIÈCE VIDE", "PRÉSENCE", "MOUVEMENT", "PAS DE SIGNAL"};
        const lv_color_t col[] = {COL_WARN, COL_WARN, COL_EMPTY, COL_PRES, COL_MOTION, COL_ALERT};
        lv_label_set_text(s_lbl_state, txt[st.state]);
        lv_obj_set_style_text_color(s_lbl_state, col[st.state], 0);
        lv_obj_set_style_line_color(s_sweep[0], st.state == RADAR_ST_MOTION ? COL_MOTION : COL_SWEEP, 0);
        // ⚫ AJOUTÉ (25/10/2026)
        lv_obj_set_style_line_color(s_sweep3[0], st.state == RADAR_ST_MOTION ? COL_MOTION : COL_SWEEP, 0);
    }

    if (s_toast_ticks > 0) {
        s_toast_ticks--;
        lv_label_set_text_fmt(s_lbl_msg, "Sensibilité\n%d / 10", st.sensitivity);
    } else if (st.state == RADAR_ST_WIFI) {
        lv_label_set_text(s_lbl_msg, st.link_msg[0] ? st.link_msg : "Attente des paquets CSI...");
    } else if (st.state == RADAR_ST_CALIB) {
        int left_delay = CONFIG_RADAR_CALIB_DELAY_SECONDS -
                         st.calib_pct * (CONFIG_RADAR_CALIB_DELAY_SECONDS + CONFIG_RADAR_CALIB_SECONDS) / 100;
        if (left_delay > 0) {
            lv_label_set_text_fmt(s_lbl_msg, "Quittez la pièce\n%d s", left_delay);
        } else {
            lv_label_set_text_fmt(s_lbl_msg, "Mesure du bruit\n%d %%", st.calib_pct);
        }
    } else if (st.state == RADAR_ST_NOSIGNAL) {
        lv_label_set_text(s_lbl_msg, "Aucun paquet CSI\ndepuis 3 s");
    } else {
        lv_label_set_text(s_lbl_msg, "");
    }

    if (s_ticks % 5 == 0) {
        lv_label_set_text_fmt(s_lbl_info, "%d dBm  ·  %d p/s  ·  sens. %d", st.rssi, st.pps, st.sensitivity);
        if (st.ssid[0]) {
            // lv_label_set_text_fmt(s_lbl_link, "%s%s  ·  canal %d", st.passive ? "passif : " : "", st.ssid, st.channel);
            // ⚫ MODIFIÉ (25/10/2026) : police 14 px ASCII (pas de "·")
            lv_label_set_text_fmt(s_lbl_link, "%s%s - canal %d", st.passive ? "passif : " : "", st.ssid, st.channel);
        } else {
            lv_label_set_text(s_lbl_link, st.passive ? "mode passif" : "");
        }
        // ⚫ AJOUTÉ (25/10/2026) : l'aide remplace la ligne réseau au démarrage et après un changement de vue
        bool hint = (int32_t)(s_hint_until_ms - lv_tick_get()) > 0;
        obj_show(s_lbl_hint, hint);
        obj_show(s_lbl_link, !hint);
    }

    // --- Courbe du score (x100, seuil = 100) ---
    if (s_ticks % 3 == 0) {
        bool live = st.state >= RADAR_ST_EMPTY && st.state <= RADAR_ST_MOTION;
        int v = live ? (int)(st.score * 100) : 0;
        lv_chart_set_next_value(s_chart, s_ser_score, v > 300 ? 300 : v);
        lv_chart_set_next_value(s_chart, s_ser_thr, 100);
        lv_chart_set_series_color(s_chart, s_ser_score, st.state == RADAR_ST_MOTION ? COL_MOTION :
                                  (st.state == RADAR_ST_PRESENCE ? COL_PRES : COL_EMPTY));
    }

    // --- Silhouettes ---
    bool moving = st.state == RADAR_ST_MOTION;
    bool present = st.state == RADAR_ST_PRESENCE || moving;
    // Position cible : angle = signature spectrale, distance = intensité (score) ; ellipse aplatie pour
    // laisser la place aux textes en haut et à la courbe en bas
    // float inten = fminf(fmaxf((st.score - 0.3f) / 2.5f, 0), 1);
    // float r = 35 + 115 * inten;
    // float a = st.angle_deg * (float)M_PI / 180;
    // float tx = CX + r * cosf(a);
    // float ty = CY + 0.4f * r * sinf(a);
    // ⚫ MODIFIÉ (25/10/2026) : position au sol en mètres : distance ESTIMÉE par le DSP (forte perturbation =
    // proche), angle = signature spectrale ; commune aux vues 2D et 3D
    float a = st.angle_deg * (float)M_PI / 180;
    float tx = st.dist_m * cosf(a);
    float tz = st.dist_m * sinf(a);
    float ox = s_wx;
    s_wx += (tx - s_wx) * (moving ? 0.06f : 0.02f);
    s_wz += (tz - s_wz) * (moving ? 0.06f : 0.02f);
    int dir = (s_wx >= ox) ? 1 : -1;

    if (moving) {
        s_walk_phase += 0.18f + 0.12f * fminf(st.score, 3);
        // un écho toutes les ~0,6 s : silhouettes figées aux positions précédentes
        if (s_ticks % (600 / UI_MS) == 0) {
            for (int i = N_ECHO - 1; i > 0; i--) {
                s_echo_x[i] = s_echo_x[i - 1];
                s_echo_z[i] = s_echo_z[i - 1];
                s_echo_phase[i] = s_echo_phase[i - 1];
            }
            s_echo_x[0] = s_wx;
            s_echo_z[0] = s_wz;
            s_echo_phase[0] = s_walk_phase;
            if (s_echo_n < N_ECHO) {
                s_echo_n++;
            }
            // ⚫ MODIFIÉ (25/10/2026) : échos placés dans les deux vues (la vue cachée reste à jour)
            const float k = R2D / st.range_m;
            for (int i = 0; i < s_echo_n; i++) {
                figure_pose(&s_echo[i], s_echo_phase[i], true, dir, 0);
                figure_place(&s_echo[i], CX + s_echo_x[i] * k, CY - s_echo_z[i] * k);
                lv_point_t p;
                float depth = project(s_echo_x[i] / st.range_m, 0, s_echo_z[i] / st.range_m, &p);
                figure_set_scale(&s_echo3[i], human_scale(depth, st.range_m));
                figure_pose(&s_echo3[i], s_echo_phase[i], true, dir, 0);
                figure_place_feet(&s_echo3[i], p.x, p.y,
                                  fmodf(atan2f(s_echo_z[i], s_echo_x[i]) * 180 / (float)M_PI + 360, 360));
            }
        }
    } else if (s_echo_n > 0 && s_ticks % (600 / UI_MS) == 0) {
        s_echo_n--;   // les échos s'effacent un par un
    }

    float breathe = 1.5f * sinf(s_ticks * UI_MS / 1000.0f * 2 * (float)M_PI / 4.0f);   // ~15 respirations/min
    // ⚫ MODIFIÉ (25/10/2026) : échos + silhouette principale dessinés par la vue active
    if (s_view3d) {
        view3d_update(&st, present, moving, dir, breathe);
    } else {
        view2d_update(&st, present, moving, dir, breathe);
    }
}

// ⚫ AJOUTÉ (25/10/2026) : vue 2D (fond radar d'origine) dans son conteneur, anneaux gradués en mètres
static void view2d_create(lv_obj_t *v) {
    // Disque + anneaux
    circle(v, 236, lv_color_hex(0x0f6b32), 2, lv_color_hex(0x02120a), LV_OPA_COVER);
    // circle(scr, 160, COL_GRID, 1, lv_color_black(), LV_OPA_TRANSP);
    // circle(scr, 84, COL_GRID, 1, lv_color_black(), LV_OPA_TRANSP);
    // ⚫ MODIFIÉ (25/10/2026) : 4 anneaux = quarts de la portée, avec leur distance
    for (int i = 1; i <= 4; i++) {
        circle(v, R2D * i / 4, COL_GRID, 1, lv_color_black(), LV_OPA_TRANSP);
        char buf[12];
        fmt_m(buf, sizeof(buf), "", CONFIG_RADAR_RANGE_M * i / 4.0f);
        lv_obj_t *l = lv_label_create(v);
        lv_obj_set_style_text_font(l, &lv_font_montserrat_14, 0);
        lv_obj_set_style_text_color(l, COL_RING_LBL, 0);
        lv_label_set_text(l, buf);
        lv_obj_set_pos(l, CX + R2D * i / 4 - 30, CY + 2);
    }
    circle(v, 4, COL_SWEEP, 0, COL_SWEEP, LV_OPA_COVER);
    static lv_point_t cross_h[2] = {{CX - 236, CY}, {CX + 236, CY}};
    static lv_point_t cross_v[2] = {{CX, CY - 228}, {CX, CY + 236}};
    line(v, cross_h, 2, COL_GRID, 1);
    line(v, cross_v, 2, COL_GRID, 1);

    // Balayage : trait principal + 2 traînées plus sombres (pas de transparence : rendu plein écran rapide)
    const lv_color_t sweep_col[3] = {COL_SWEEP, lv_color_hex(0x0f8c46), lv_color_hex(0x084a26)};
    for (int i = 2; i >= 0; i--) {
        s_sweep_pts[i][0] = (lv_point_t){CX, CY};
        s_sweep_pts[i][1] = (lv_point_t){CX + R_SWEEP, CY};
        s_sweep[i] = line(v, s_sweep_pts[i], 2, sweep_col[i], 3);
    }

    // Silhouettes : échos d'abord (dessous), puis la principale
    for (int i = N_ECHO - 1; i >= 0; i--) {
        figure_create(&s_echo[i], v);
    }
    figure_create(&s_main, v);
}

// ⚫ AJOUTÉ (25/10/2026) : vue 3D : sol en perspective, kit au centre, box, silhouettes
static void view3d_create(lv_obj_t *v) {
    s_se = sinf(P3_E_DEG * (float)M_PI / 180);
    s_ce = cosf(P3_E_DEG * (float)M_PI / 180);
    // Halo du sol (disque sombre) puis anneaux et rayons
    circle(v, 236, lv_color_hex(0x0b3a1e), 2, lv_color_hex(0x010a05), LV_OPA_COVER);
    for (int k = 0; k < 4; k++) {
        float a = k * (float)M_PI / 4;
        project(cosf(a), 0, sinf(a), &s_radial3_pts[k][0]);
        project(-cosf(a), 0, -sinf(a), &s_radial3_pts[k][1]);
        line(v, s_radial3_pts[k], 2, COL_GRID, 1);
    }
    for (int i = 1; i <= 4; i++) {
        ring3(s_ring3_pts[i - 1], i / 4.0f);
        line(v, s_ring3_pts[i - 1], RING_PTS + 1, i == 4 ? lv_color_hex(0x0f6b32) : COL_GRID, i == 4 ? 2 : 1);
        // Distance de l'anneau, côté avant-droit
        lv_point_t p;
        float a = -35 * (float)M_PI / 180;
        project(i / 4.0f * cosf(a), 0, i / 4.0f * sinf(a), &p);
        char buf[12];
        fmt_m(buf, sizeof(buf), "", CONFIG_RADAR_RANGE_M * i / 4.0f);
        lv_obj_t *l = lv_label_create(v);
        lv_obj_set_style_text_font(l, &lv_font_montserrat_14, 0);
        lv_obj_set_style_text_color(l, COL_RING_LBL, 0);
        lv_label_set_text(l, buf);
        lv_obj_set_pos(l, p.x + 3, p.y);
    }

    // Onde et balayage au sol
    ring3(s_wave3_pts, 0.02f);
    s_wave3 = line(v, s_wave3_pts, RING_PTS + 1, COL_SWEEP, 2);
    const lv_color_t sweep_col[3] = {COL_SWEEP, lv_color_hex(0x0f8c46), lv_color_hex(0x084a26)};
    for (int i = 2; i >= 0; i--) {
        project(0, 0, 0, &s_sweep3_pts[i][0]);
        project(1, 0, 0, &s_sweep3_pts[i][1]);
        s_sweep3[i] = line(v, s_sweep3_pts[i], 2, sweep_col[i], 3);
    }

    // Box / point d'accès (position mise à jour d'après le RSSI)
    s_ap3 = box(v, 18, 12, 2, lv_color_hex(0xd8dde3), lv_color_hex(0x7a8794), 1);
    s_ap3_lbl = lv_label_create(v);
    lv_obj_set_style_text_font(s_ap3_lbl, &lv_font_montserrat_14, 0);
    lv_obj_set_style_text_color(s_ap3_lbl, lv_color_hex(0xb8c2cc), 0);
    lv_label_set_text(s_ap3_lbl, "");
    obj_show(s_ap3, false);
    obj_show(s_ap3_lbl, false);

    // Échos (dessous) puis ombre et silhouette principale
    for (int i = N_ECHO - 1; i >= 0; i--) {
        figure_create(&s_echo3[i], v);
    }
    s_shadow3 = box(v, 34, 10, LV_RADIUS_CIRCLE, lv_color_hex(0x10303a), lv_color_black(), 0);
    obj_show(s_shadow3, false);
    figure_create(&s_main3, v);

    // Le kit au centre : bouton rond à écran (corps + écran lumineux), dessiné par-dessus le sol
    lv_point_t o;
    project(0, 0, 0, &o);
    lv_obj_t *base = box(v, 46, 16, LV_RADIUS_CIRCLE, lv_color_hex(0x061a10), lv_color_black(), 0);
    lv_obj_set_pos(base, o.x - 23, o.y - 6);
    lv_obj_t *body = box(v, 36, 22, 8, lv_color_hex(0x2a2f36), lv_color_hex(0x6b7785), 2);
    lv_obj_set_pos(body, o.x - 18, o.y - 20);
    lv_obj_t *screen = box(v, 30, 12, LV_RADIUS_CIRCLE, lv_color_hex(0x22c8ff), lv_color_hex(0x9be8ff), 1);
    lv_obj_set_pos(screen, o.x - 15, o.y - 26);
}

void radar_ui_init(void) {
    lv_obj_t *scr = lv_scr_act();
    lv_obj_set_style_bg_color(scr, lv_color_black(), 0);
    lv_obj_set_style_bg_opa(scr, LV_OPA_COVER, 0);
    lv_obj_clear_flag(scr, LV_OBJ_FLAG_SCROLLABLE);

    // ⚫ MODIFIÉ (25/10/2026) : le fond radar, le balayage et les silhouettes d'origine sont dans view2d_create() ;
    // chaque vue a son conteneur plein écran, une seule est visible
    for (int i = 0; i < 2; i++) {
        lv_obj_t *v = lv_obj_create(scr);
        lv_obj_remove_style_all(v);
        lv_obj_set_size(v, 480, 480);
        lv_obj_clear_flag(v, LV_OBJ_FLAG_CLICKABLE | LV_OBJ_FLAG_SCROLLABLE);
        if (i == 0) {
            s_v2d = v;
            view2d_create(v);
        } else {
            s_v3d = v;
            view3d_create(v);
        }
    }

    // Courbe du score
    s_chart = lv_chart_create(scr);
    // lv_obj_set_size(s_chart, 250, 54);
    // lv_obj_align(s_chart, LV_ALIGN_BOTTOM_MID, 0, -92);
    // ⚫ MODIFIÉ (25/10/2026) : plus basse et plus plate, sous le sol de la vue 3D
    lv_obj_set_size(s_chart, 230, 40);
    lv_obj_align(s_chart, LV_ALIGN_BOTTOM_MID, 0, -52);
    lv_obj_set_style_bg_opa(s_chart, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(s_chart, 0, 0);
    lv_obj_set_style_pad_all(s_chart, 0, 0);
    lv_obj_set_style_size(s_chart, 0, LV_PART_INDICATOR);      // pas de points
    lv_obj_set_style_line_width(s_chart, 2, LV_PART_ITEMS);
    lv_obj_clear_flag(s_chart, LV_OBJ_FLAG_CLICKABLE | LV_OBJ_FLAG_SCROLLABLE);
    lv_chart_set_type(s_chart, LV_CHART_TYPE_LINE);
    lv_chart_set_div_line_count(s_chart, 0, 0);
    lv_chart_set_point_count(s_chart, CHART_PTS);
    lv_chart_set_range(s_chart, LV_CHART_AXIS_PRIMARY_Y, 0, 300);
    lv_chart_set_update_mode(s_chart, LV_CHART_UPDATE_MODE_SHIFT);
    s_ser_thr = lv_chart_add_series(s_chart, lv_color_hex(0x5a3a10), LV_CHART_AXIS_PRIMARY_Y);
    s_ser_score = lv_chart_add_series(s_chart, COL_EMPTY, LV_CHART_AXIS_PRIMARY_Y);
    lv_chart_set_all_value(s_chart, s_ser_thr, 100);
    lv_chart_set_all_value(s_chart, s_ser_score, 0);

    // Textes
    lv_obj_t *title = label(scr, &font_fr_20, COL_EMPTY, LV_ALIGN_TOP_MID, 22);
    lv_label_set_text(title, "RADAR Wi-Fi");
    s_lbl_state = label(scr, &font_fr_28, COL_WARN, LV_ALIGN_TOP_MID, 48);
    s_lbl_info = label(scr, &font_fr_20, COL_TEXT, LV_ALIGN_TOP_MID, 88);
    s_lbl_msg = label(scr, &font_fr_28, lv_color_white(), LV_ALIGN_CENTER, -12);
    // s_lbl_link = label(scr, &font_fr_20, COL_TEXT, LV_ALIGN_BOTTOM_MID, -60);
    // s_lbl_hint = label(scr, &font_fr_20, lv_color_hex(0x4f7a60), LV_ALIGN_BOTTOM_MID, -32);
    // lv_label_set_text(s_lbl_hint, "appui : recalibrer");
    // ⚫ MODIFIÉ (25/10/2026) : réseau et aide au même endroit (l'aide s'affiche 8 s au démarrage)
    // (police 14 px : le bas de l'écran rond ne fait que ~240 px de large ; ASCII uniquement, pas de "·")
    s_lbl_link = label(scr, &lv_font_montserrat_14, COL_TEXT, LV_ALIGN_BOTTOM_MID, -26);
    s_lbl_hint = label(scr, &lv_font_montserrat_14, lv_color_hex(0x8fc4a2), LV_ALIGN_BOTTOM_MID, -26);
    lv_label_set_text(s_lbl_hint, "appui : calibrer - long : vue 2D/3D");
    // ⚫ AJOUTÉ (25/10/2026) : distance estimée au-dessus de la silhouette
    s_lbl_dist = label(scr, &font_fr_20, lv_color_white(), LV_ALIGN_TOP_MID, 0);
    obj_show(s_lbl_dist, false);

    s_enc_last = mt8901_get_count();
    // ⚫ AJOUTÉ (25/10/2026)
    set_view(s_view3d);
    s_hint_until_ms = lv_tick_get() + HINT_MS;
    lv_timer_create(ui_timer_cb, UI_MS, NULL);
}
