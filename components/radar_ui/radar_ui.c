#include "radar_ui.h"

#include <math.h>
#include <stdio.h>
#include <string.h>
#include <time.h>

#include "lvgl.h"
#include "esp_lvgl_port.h"

/* ------------------------------------------------------------- geometry */
#define SCREEN_PX        412
#define CENTER_PX        (SCREEN_PX / 2)
#define PLOT_RADIUS_PX   196          /* outer ring radius (leaves a small margin) */
#define RING_COUNT       3
#define EARTH_RADIUS_NM  3440.065

/* Amber phosphor look. */
#define COLOR_SCOPE      lv_color_hex(0xFFC400)
#define COLOR_DIM        lv_color_hex(0x6E5500)

#define MAX_AIRCRAFT     64

/* Analog clock hand lengths (px) and the hour-pip radius. */
#define HAND_HOUR_LEN    96
#define HAND_MIN_LEN     140
#define HAND_SEC_LEN     162
#define PIP_RADIUS_PX    180

/* --------------------------------------------------------------- state */
static lv_obj_t *s_screen;
static lv_obj_t *s_layer;          /* parent of all per-aircraft objects */
static lv_obj_t *s_header;
static lv_obj_t *s_status;
static lv_obj_t *s_battery;
/* Analog clock drawn behind the scope: three hands pivoting at centre. */
static lv_obj_t *s_hand_hour;
static lv_obj_t *s_hand_min;
static lv_obj_t *s_hand_sec;
static lv_point_precise_t s_hour_pts[2];
static lv_point_precise_t s_min_pts[2];
static lv_point_precise_t s_sec_pts[2];

static double s_home_lat;
static double s_home_lon;
static int    s_range_nm = 50;
static char   s_zip[12];

/* Persistent storage for the leader-line endpoints. lv_line keeps the pointer
 * (it does not copy), and old lines are deleted before we overwrite a slot. */
static lv_point_precise_t s_line_pts[MAX_AIRCRAFT][2];

/* ----------------------------------------------------------- scope build */
static void make_ring(int radius_px)
{
    lv_obj_t *ring = lv_obj_create(s_screen);
    lv_obj_remove_style_all(ring);
    lv_obj_set_size(ring, radius_px * 2, radius_px * 2);
    lv_obj_align(ring, LV_ALIGN_CENTER, 0, 0);
    lv_obj_set_style_radius(ring, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_bg_opa(ring, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_color(ring, COLOR_SCOPE, 0);
    lv_obj_set_style_border_width(ring, 2, 0);
    lv_obj_set_style_border_opa(ring, LV_OPA_COVER, 0);
    lv_obj_clear_flag(ring, LV_OBJ_FLAG_SCROLLABLE);
}

/* Point a clock hand: 0 deg = 12 o'clock, increasing clockwise. */
static void set_hand(lv_obj_t *hand, lv_point_precise_t *pts, float deg, int length)
{
    float r = deg * (float)M_PI / 180.0f;
    pts[0].x = CENTER_PX;
    pts[0].y = CENTER_PX;
    pts[1].x = (lv_value_precise_t)(CENTER_PX + sinf(r) * length);
    pts[1].y = (lv_value_precise_t)(CENTER_PX - cosf(r) * length);
    lv_line_set_points(hand, pts, 2);   /* re-points and invalidates the area */
}

/* Refresh the analog clock hands. Runs in the LVGL task once per second. */
static void clock_timer_cb(lv_timer_t *timer)
{
    LV_UNUSED(timer);
    time_t now = time(NULL);
    bool synced = (now >= 1700000000);   /* after ~2023 -> SNTP has run */

    /* Hide the hands until the time is known. */
    lv_obj_t *hands[] = { s_hand_hour, s_hand_min, s_hand_sec };
    for (int i = 0; i < 3; i++) {
        if (synced) {
            lv_obj_clear_flag(hands[i], LV_OBJ_FLAG_HIDDEN);
        } else {
            lv_obj_add_flag(hands[i], LV_OBJ_FLAG_HIDDEN);
        }
    }
    if (!synced) {
        return;
    }

    struct tm tm;
    localtime_r(&now, &tm);
    float hour_deg = (tm.tm_hour % 12) * 30.0f + tm.tm_min * 0.5f;
    float min_deg  = tm.tm_min * 6.0f + tm.tm_sec * 0.1f;
    float sec_deg  = tm.tm_sec * 6.0f;

    set_hand(s_hand_hour, s_hour_pts, hour_deg, HAND_HOUR_LEN);
    set_hand(s_hand_min,  s_min_pts,  min_deg,  HAND_MIN_LEN);
    set_hand(s_hand_sec,  s_sec_pts,  sec_deg,  HAND_SEC_LEN);
}

/* Create one clock hand line (behind the scope). */
static lv_obj_t *make_hand(int width)
{
    lv_obj_t *hand = lv_line_create(s_screen);
    lv_obj_set_style_line_color(hand, COLOR_DIM, 0);
    lv_obj_set_style_line_width(hand, width, 0);
    lv_obj_set_style_line_rounded(hand, true, 0);
    lv_obj_add_flag(hand, LV_OBJ_FLAG_HIDDEN);
    return hand;
}

/* Small dim hour pip at clock position @hour (0..11). */
static void make_pip(int hour)
{
    float r = hour * 30.0f * (float)M_PI / 180.0f;
    int size = (hour % 3 == 0) ? 6 : 3;   /* quarter marks are larger */
    int x = (int)lroundf(CENTER_PX + sinf(r) * PIP_RADIUS_PX);
    int y = (int)lroundf(CENTER_PX - cosf(r) * PIP_RADIUS_PX);

    lv_obj_t *pip = lv_obj_create(s_screen);
    lv_obj_remove_style_all(pip);
    lv_obj_set_size(pip, size, size);
    lv_obj_set_pos(pip, x - size / 2, y - size / 2);
    lv_obj_set_style_radius(pip, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_bg_color(pip, COLOR_DIM, 0);
    lv_obj_set_style_bg_opa(pip, LV_OPA_COVER, 0);
}

void radar_ui_create(const char *zip, double home_lat, double home_lon, int range_nm)
{
    s_home_lat = home_lat;
    s_home_lon = home_lon;
    s_range_nm = range_nm > 0 ? range_nm : 50;
    strlcpy(s_zip, zip ? zip : "", sizeof(s_zip));

    lvgl_port_lock(0);

    s_screen = lv_screen_active();
    lv_obj_set_style_bg_color(s_screen, lv_color_black(), 0);
    lv_obj_set_style_bg_opa(s_screen, LV_OPA_COVER, 0);
    lv_obj_clear_flag(s_screen, LV_OBJ_FLAG_SCROLLABLE);

    /* Analog clock, created first so its hands and pips sit behind the scope. */
    for (int h = 0; h < 12; h++) {
        make_pip(h);
    }
    s_hand_hour = make_hand(5);
    s_hand_min  = make_hand(3);
    s_hand_sec  = make_hand(2);
    lv_timer_create(clock_timer_cb, 1000, NULL);

    /* Concentric range rings. */
    for (int i = 1; i <= RING_COUNT; i++) {
        make_ring(PLOT_RADIUS_PX * i / RING_COUNT);
    }

    /* Centre (home) mark. */
    lv_obj_t *home = lv_obj_create(s_screen);
    lv_obj_remove_style_all(home);
    lv_obj_set_size(home, 6, 6);
    lv_obj_align(home, LV_ALIGN_CENTER, 0, 0);
    lv_obj_set_style_radius(home, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_bg_color(home, COLOR_SCOPE, 0);
    lv_obj_set_style_bg_opa(home, LV_OPA_COVER, 0);

    /* Header (top) and status (bottom). */
    s_header = lv_label_create(s_screen);
    lv_obj_set_style_text_font(s_header, &lv_font_unscii_8, 0);
    lv_obj_set_style_text_color(s_header, COLOR_SCOPE, 0);
    lv_obj_align(s_header, LV_ALIGN_TOP_MID, 0, 26);
    lv_label_set_text_fmt(s_header, "%s  %dNM  AC:0", s_zip, s_range_nm);

    s_status = lv_label_create(s_screen);
    lv_obj_set_style_text_font(s_status, &lv_font_unscii_8, 0);
    lv_obj_set_style_text_color(s_status, COLOR_DIM, 0);
    lv_obj_align(s_status, LV_ALIGN_BOTTOM_MID, 0, -26);
    lv_label_set_text(s_status, "");

    /* Battery indicator (below the header); hidden until a battery is seen. */
    s_battery = lv_label_create(s_screen);
    lv_obj_set_style_text_font(s_battery, &lv_font_montserrat_14, 0);
    lv_obj_set_style_text_color(s_battery, COLOR_SCOPE, 0);
    lv_obj_align(s_battery, LV_ALIGN_TOP_MID, 0, 42);
    lv_obj_add_flag(s_battery, LV_OBJ_FLAG_HIDDEN);

    /* Layer that holds the dynamic aircraft objects. */
    s_layer = lv_obj_create(s_screen);
    lv_obj_remove_style_all(s_layer);
    lv_obj_set_size(s_layer, SCREEN_PX, SCREEN_PX);
    lv_obj_align(s_layer, LV_ALIGN_CENTER, 0, 0);
    lv_obj_clear_flag(s_layer, LV_OBJ_FLAG_SCROLLABLE);

    lvgl_port_unlock();
}

/* ------------------------------------------------------- aircraft plotting */
/* Project an aircraft position to screen pixels relative to home.
 * Returns false if it falls outside the current range. */
static bool project(const aircraft_t *ac, int *px, int *py)
{
    double hlat_rad = s_home_lat * M_PI / 180.0;
    double east_nm  = (ac->lon - s_home_lon) * M_PI / 180.0 * cos(hlat_rad) * EARTH_RADIUS_NM;
    double north_nm = (ac->lat - s_home_lat) * M_PI / 180.0 * EARTH_RADIUS_NM;

    double scale = (double)PLOT_RADIUS_PX / s_range_nm;
    double x = CENTER_PX + east_nm * scale;
    double y = CENTER_PX - north_nm * scale;   /* screen y grows downward */

    double dist = sqrt(east_nm * east_nm + north_nm * north_nm);
    if (dist > s_range_nm) {
        return false;
    }
    *px = (int)lround(x);
    *py = (int)lround(y);
    return true;
}

static void plot_aircraft(int slot, const aircraft_t *ac, int px, int py)
{
    /* Velocity leader line, pointing along the track. */
    float len = ac->ground_speed * 0.06f;
    if (len < 10.0f) len = 10.0f;
    if (len > 30.0f) len = 30.0f;
    double t = ac->track * M_PI / 180.0;
    int ex = px + (int)lround(sin(t) * len);
    int ey = py - (int)lround(cos(t) * len);

    s_line_pts[slot][0].x = px;
    s_line_pts[slot][0].y = py;
    s_line_pts[slot][1].x = ex;
    s_line_pts[slot][1].y = ey;

    lv_obj_t *line = lv_line_create(s_layer);
    lv_line_set_points(line, s_line_pts[slot], 2);
    lv_obj_set_style_line_color(line, COLOR_SCOPE, 0);
    lv_obj_set_style_line_width(line, 2, 0);

    /* Aircraft dot. */
    lv_obj_t *dot = lv_obj_create(s_layer);
    lv_obj_remove_style_all(dot);
    lv_obj_set_size(dot, 7, 7);
    lv_obj_set_pos(dot, px - 3, py - 3);
    lv_obj_set_style_radius(dot, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_bg_color(dot, COLOR_SCOPE, 0);
    lv_obj_set_style_bg_opa(dot, LV_OPA_COVER, 0);

    /* 4-line data block: callsign / type / flight level / ground speed. */
    char fl[16];
    if (ac->on_ground) {
        strcpy(fl, "GND");
    } else if (ac->altitude_ft > 0) {
        snprintf(fl, sizeof(fl), "FL%03d", ac->altitude_ft / 100);
    } else {
        strcpy(fl, "----");
    }

    lv_obj_t *label = lv_label_create(s_layer);
    lv_obj_set_style_text_font(label, &lv_font_unscii_8, 0);
    lv_obj_set_style_text_color(label, COLOR_SCOPE, 0);
    lv_obj_set_style_text_line_space(label, 1, 0);
    lv_label_set_text_fmt(label, "%s\n%s\n%s\n%d",
                          ac->callsign,
                          ac->type[0] ? ac->type : "----",
                          fl,
                          (int)(ac->ground_speed + 0.5f));
    lv_obj_set_pos(label, px + 9, py - 5);
}

void radar_ui_update(const aircraft_t *list, size_t count)
{
    lvgl_port_lock(0);

    lv_obj_clean(s_layer);

    int shown = 0;
    for (size_t i = 0; i < count && shown < MAX_AIRCRAFT; i++) {
        int px, py;
        if (project(&list[i], &px, &py)) {
            plot_aircraft(shown, &list[i], px, py);
            shown++;
        }
    }

    lv_label_set_text_fmt(s_header, "%s  %dNM  AC:%d", s_zip, s_range_nm, shown);

    lvgl_port_unlock();
}

void radar_ui_set_status(const char *status)
{
    if (!s_status) {
        return;
    }
    lvgl_port_lock(0);
    lv_label_set_text(s_status, status ? status : "");
    lvgl_port_unlock();
}

void radar_ui_set_battery(int percent, bool present)
{
    if (!s_battery) {
        return;
    }
    lvgl_port_lock(0);
    if (!present) {
        lv_obj_add_flag(s_battery, LV_OBJ_FLAG_HIDDEN);
    } else {
        const char *sym = LV_SYMBOL_BATTERY_FULL;
        if (percent < 20)      sym = LV_SYMBOL_BATTERY_EMPTY;
        else if (percent < 45) sym = LV_SYMBOL_BATTERY_1;
        else if (percent < 70) sym = LV_SYMBOL_BATTERY_2;
        else if (percent < 95) sym = LV_SYMBOL_BATTERY_3;
        lv_label_set_text_fmt(s_battery, "%s %d%%", sym, percent);
        lv_obj_clear_flag(s_battery, LV_OBJ_FLAG_HIDDEN);
    }
    lvgl_port_unlock();
}
