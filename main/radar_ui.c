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
#include <math.h>
#include <stdio.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "driver/gpio.h"
#include "lvgl.h"
#include "board.h"
#include "mt8901.h"
#include "radar.h"

LV_FONT_DECLARE(font_fr_20);
LV_FONT_DECLARE(font_fr_28);

#define CX          240
#define CY          228        // centre du radar (un peu au-dessus du centre de l'écran : place pour la courbe)
#define R_SWEEP     220
#define FIG_W       56
#define FIG_H       100
#define N_ECHO      3
#define CHART_PTS   60
#define UI_MS       40

#define COL_GRID    lv_color_hex(0x0b4a24)
#define COL_SWEEP   lv_color_hex(0x22ff88)
#define COL_EMPTY   lv_color_hex(0x19e36b)
#define COL_PRES    lv_color_hex(0x36c8ff)
#define COL_MOTION  lv_color_hex(0xff4d2e)
#define COL_WARN    lv_color_hex(0xffb000)
#define COL_ALERT   lv_color_hex(0xff2e63)
#define COL_TEXT    lv_color_hex(0x9fd8b4)

typedef struct {
    lv_obj_t *cont;
    lv_obj_t *head;
    lv_obj_t *torso, *arm_l, *arm_r, *leg_l, *leg_r;
    lv_point_t p_torso[2], p_arm_l[3], p_arm_r[3], p_leg_l[3], p_leg_r[3];
    lv_color_t color;
    float ang_deg;             // angle à l'écran (pour l'illumination par le balayage)
} figure_t;

static figure_t s_main, s_echo[N_ECHO];
static lv_obj_t *s_sweep[3];
static lv_point_t s_sweep_pts[3][2];
static lv_obj_t *s_lbl_state, *s_lbl_info, *s_lbl_msg, *s_lbl_link, *s_lbl_hint;
static lv_obj_t *s_chart;
static lv_chart_series_t *s_ser_score, *s_ser_thr;

static float s_sweep_deg;
static float s_walk_phase;
static float s_fig_x = CX, s_fig_y = CY;
static float s_echo_x[N_ECHO], s_echo_y[N_ECHO], s_echo_phase[N_ECHO];
static int s_echo_n;
static uint32_t s_ticks;
static int s_toast_ticks;
static int16_t s_enc_last;
static int s_btn_ticks;

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
static void joint(lv_point_t *p, int x0, int y0, float a1, float l1, float a2, float l2, int dir) {
    p[0].x = x0;
    p[0].y = y0;
    p[1].x = x0 + (lv_coord_t)lroundf(dir * l1 * sinf(a1 * (float)M_PI / 180));
    p[1].y = y0 + (lv_coord_t)lroundf(l1 * cosf(a1 * (float)M_PI / 180));
    p[2].x = p[1].x + (lv_coord_t)lroundf(dir * l2 * sinf(a2 * (float)M_PI / 180));
    p[2].y = p[1].y + (lv_coord_t)lroundf(l2 * cosf(a2 * (float)M_PI / 180));
}

// walking : cycle de marche (phase en radians) ; sinon posture debout avec respiration
static void figure_pose(figure_t *f, float phase, bool walking, int dir, float breathe) {
    const int cx = FIG_W / 2;
    int bob = walking ? (int)lroundf(2 * fabsf(sinf(phase))) : (int)lroundf(breathe);
    int neck = 24 - bob, hip = 58 - bob / 2;
    lv_obj_set_pos(f->head, cx - 10, 2 - bob);
    f->p_torso[0] = (lv_point_t){cx, neck + 4};
    f->p_torso[1] = (lv_point_t){cx, hip};
    float leg = walking ? 28 * sinf(phase) : 7;
    float arm = walking ? 26 * sinf(phase) : 10 + breathe;
    float knee_l = walking ? fmaxf(0, -30 * cosf(phase)) : 0;
    float knee_r = walking ? fmaxf(0, 30 * cosf(phase)) : 0;
    joint(f->p_leg_l, cx, hip, leg, 20, leg - knee_l, 19, dir);
    joint(f->p_leg_r, cx, hip, -leg, 20, -leg - knee_r, 19, dir);
    joint(f->p_arm_l, cx, neck + 6, -arm, 17, -arm + (walking ? 25 : 5), 15, dir);
    joint(f->p_arm_r, cx, neck + 6, arm, 17, arm + (walking ? 25 : 5), 15, walking ? dir : -dir);
    lv_line_set_points(f->torso, f->p_torso, 2);
    lv_line_set_points(f->leg_l, f->p_leg_l, 3);
    lv_line_set_points(f->leg_r, f->p_leg_r, 3);
    lv_line_set_points(f->arm_l, f->p_arm_l, 3);
    lv_line_set_points(f->arm_r, f->p_arm_r, 3);
}

static void figure_place(figure_t *f, float x, float y) {
    lv_obj_set_pos(f->cont, (lv_coord_t)lroundf(x) - FIG_W / 2, (lv_coord_t)lroundf(y) - FIG_H / 2);
    f->ang_deg = fmodf(atan2f(y - CY, x - CX) * 180 / (float)M_PI + 360, 360);
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

// Luminosité selon le retard du balayage sur la silhouette (écho radar qui s'estompe après le passage)
static lv_color_t lit(lv_color_t base, float fig_ang, float dim_floor) {
    float d = fmodf(s_sweep_deg - fig_ang + 360, 360);   // 0 = le balayage vient de passer
    float k = 1.0f - d / 360.0f;
    k = dim_floor + (1 - dim_floor) * k * k;
    return lv_color_mix(base, lv_color_black(), (lv_opa_t)(k * 255));
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

static void sweep_update(void) {
    static const float lag[3] = {0, 7, 14};
    for (int i = 0; i < 3; i++) {
        float a = (s_sweep_deg - lag[i]) * (float)M_PI / 180;
        s_sweep_pts[i][0] = (lv_point_t){CX, CY};
        s_sweep_pts[i][1] = (lv_point_t){CX + (lv_coord_t)lroundf(R_SWEEP * cosf(a)),
                                         CY + (lv_coord_t)lroundf(R_SWEEP * sinf(a))};
        lv_line_set_points(s_sweep[i], s_sweep_pts[i], 2);
    }
}

// -----------------------------------------------------------------------------------------------------------
// Encodeur / bouton (lecture directe)
// -----------------------------------------------------------------------------------------------------------

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
    if (gpio_get_level(BTN_PIN) == 0) {
        s_btn_ticks++;
    } else {
        if (s_btn_ticks * UI_MS >= 60) {
            radar_request_calibration();
        }
        s_btn_ticks = 0;
    }
}

// -----------------------------------------------------------------------------------------------------------
// Rafraîchissement
// -----------------------------------------------------------------------------------------------------------

static void ui_timer_cb(lv_timer_t *t) {
    radar_status_t st;
    radar_get_status(&st);
    input_poll(&st);
    radar_get_status(&st);   // sensibilité éventuellement modifiée
    s_ticks++;

    s_sweep_deg = fmodf(s_sweep_deg + 5.0f, 360.0f);
    sweep_update();

    // --- Libellés d'état ---
    static radar_state_t last_state = (radar_state_t)-1;
    if (st.state != last_state) {
        last_state = st.state;
        static const char *txt[] = {"CONNEXION", "CALIBRAGE", "PIÈCE VIDE", "PRÉSENCE", "MOUVEMENT", "PAS DE SIGNAL"};
        const lv_color_t col[] = {COL_WARN, COL_WARN, COL_EMPTY, COL_PRES, COL_MOTION, COL_ALERT};
        lv_label_set_text(s_lbl_state, txt[st.state]);
        lv_obj_set_style_text_color(s_lbl_state, col[st.state], 0);
        lv_obj_set_style_line_color(s_sweep[0], st.state == RADAR_ST_MOTION ? COL_MOTION : COL_SWEEP, 0);
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
            lv_label_set_text_fmt(s_lbl_link, "%s%s  ·  canal %d", st.passive ? "passif : " : "", st.ssid, st.channel);
        } else {
            lv_label_set_text(s_lbl_link, st.passive ? "mode passif" : "");
        }
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
    float inten = fminf(fmaxf((st.score - 0.3f) / 2.5f, 0), 1);
    float r = 35 + 115 * inten;
    float a = st.angle_deg * (float)M_PI / 180;
    float tx = CX + r * cosf(a);
    float ty = CY + 0.4f * r * sinf(a);
    float ox = s_fig_x;
    s_fig_x += (tx - s_fig_x) * (moving ? 0.06f : 0.02f);
    s_fig_y += (ty - s_fig_y) * (moving ? 0.06f : 0.02f);
    int dir = (s_fig_x >= ox) ? 1 : -1;

    if (moving) {
        s_walk_phase += 0.18f + 0.12f * fminf(st.score, 3);
        // un écho toutes les ~0,6 s : silhouettes figées aux positions précédentes
        if (s_ticks % (600 / UI_MS) == 0) {
            for (int i = N_ECHO - 1; i > 0; i--) {
                s_echo_x[i] = s_echo_x[i - 1];
                s_echo_y[i] = s_echo_y[i - 1];
                s_echo_phase[i] = s_echo_phase[i - 1];
            }
            s_echo_x[0] = s_fig_x;
            s_echo_y[0] = s_fig_y;
            s_echo_phase[0] = s_walk_phase;
            if (s_echo_n < N_ECHO) {
                s_echo_n++;
            }
            for (int i = 0; i < s_echo_n; i++) {
                figure_pose(&s_echo[i], s_echo_phase[i], true, dir, 0);
                figure_place(&s_echo[i], s_echo_x[i], s_echo_y[i]);
            }
        }
    } else if (s_echo_n > 0 && s_ticks % (600 / UI_MS) == 0) {
        s_echo_n--;   // les échos s'effacent un par un
    }
    for (int i = 0; i < N_ECHO; i++) {
        figure_show(&s_echo[i], i < s_echo_n);
        if (i < s_echo_n) {
            static const uint8_t dim[N_ECHO] = {120, 75, 40};
            lv_color_t base = lv_color_mix(COL_MOTION, lv_color_black(), dim[i]);
            figure_set_color(&s_echo[i], lit(base, s_echo[i].ang_deg, 0.6f));
        }
    }

    figure_show(&s_main, present);
    if (present) {
        float breathe = 1.5f * sinf(s_ticks * UI_MS / 1000.0f * 2 * (float)M_PI / 4.0f);   // ~15 respirations/min
        figure_pose(&s_main, s_walk_phase, moving, dir, breathe);
        figure_place(&s_main, s_fig_x, s_fig_y);
        figure_set_color(&s_main, lit(moving ? COL_MOTION : COL_PRES, s_main.ang_deg, 0.55f));
    }
}

void radar_ui_init(void) {
    lv_obj_t *scr = lv_scr_act();
    lv_obj_set_style_bg_color(scr, lv_color_black(), 0);
    lv_obj_set_style_bg_opa(scr, LV_OPA_COVER, 0);
    lv_obj_clear_flag(scr, LV_OBJ_FLAG_SCROLLABLE);

    // Disque + anneaux
    circle(scr, 236, lv_color_hex(0x0f6b32), 2, lv_color_hex(0x02120a), LV_OPA_COVER);
    circle(scr, 160, COL_GRID, 1, lv_color_black(), LV_OPA_TRANSP);
    circle(scr, 84, COL_GRID, 1, lv_color_black(), LV_OPA_TRANSP);
    circle(scr, 4, COL_SWEEP, 0, COL_SWEEP, LV_OPA_COVER);
    static lv_point_t cross_h[2] = {{CX - 236, CY}, {CX + 236, CY}};
    static lv_point_t cross_v[2] = {{CX, CY - 228}, {CX, CY + 236}};
    line(scr, cross_h, 2, COL_GRID, 1);
    line(scr, cross_v, 2, COL_GRID, 1);

    // Balayage : trait principal + 2 traînées plus sombres (pas de transparence : rendu plein écran rapide)
    const lv_color_t sweep_col[3] = {COL_SWEEP, lv_color_hex(0x0f8c46), lv_color_hex(0x084a26)};
    for (int i = 2; i >= 0; i--) {
        s_sweep_pts[i][0] = (lv_point_t){CX, CY};
        s_sweep_pts[i][1] = (lv_point_t){CX + R_SWEEP, CY};
        s_sweep[i] = line(scr, s_sweep_pts[i], 2, sweep_col[i], 3);
    }

    // Courbe du score
    s_chart = lv_chart_create(scr);
    lv_obj_set_size(s_chart, 250, 54);
    lv_obj_align(s_chart, LV_ALIGN_BOTTOM_MID, 0, -92);
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

    // Silhouettes : échos d'abord (dessous), puis la principale
    for (int i = N_ECHO - 1; i >= 0; i--) {
        figure_create(&s_echo[i], scr);
    }
    figure_create(&s_main, scr);

    // Textes
    lv_obj_t *title = label(scr, &font_fr_20, COL_EMPTY, LV_ALIGN_TOP_MID, 22);
    lv_label_set_text(title, "RADAR Wi-Fi");
    s_lbl_state = label(scr, &font_fr_28, COL_WARN, LV_ALIGN_TOP_MID, 48);
    s_lbl_info = label(scr, &font_fr_20, COL_TEXT, LV_ALIGN_TOP_MID, 88);
    s_lbl_msg = label(scr, &font_fr_28, lv_color_white(), LV_ALIGN_CENTER, -12);
    s_lbl_link = label(scr, &font_fr_20, COL_TEXT, LV_ALIGN_BOTTOM_MID, -60);
    s_lbl_hint = label(scr, &font_fr_20, lv_color_hex(0x4f7a60), LV_ALIGN_BOTTOM_MID, -32);
    lv_label_set_text(s_lbl_hint, "appui : recalibrer");

    s_enc_last = mt8901_get_count();
    lv_timer_create(ui_timer_cb, UI_MS, NULL);
}
