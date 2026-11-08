#include "ui.h"

#include <stdio.h>
#include <string.h>
#include <math.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"
#include "lvgl.h"
#include "esp_heap_caps.h"
#include "src/extra/others/snapshot/lv_snapshot.h"

#include "app_state.h"
#include "files.h"
#include "rgb.h"

#define C_BG       0x05070D
#define C_PANEL    0x0A1019
#define C_PANEL2   0x0E1622
#define C_LINE     0x1C2A3A
#define C_TEXT     0xEAF6FF
#define C_MUTED    0x7D8FA6
#define C_DIM      0x3E5066
#define C_CYAN     0x35E0FF
#define C_MINT     0x5CF2C2
#define C_AMBER    0xFFB25E
#define C_RED      0xFF5C7A

#define PAGE_W 172
#define PAGE_H 268
#define LIST_ROWS 6

static const char *TAG = "ui";

static lv_obj_t *s_main;
static lv_obj_t *s_viewport;
static lv_obj_t *s_track;
static lv_obj_t *s_pages[3];
static lv_obj_t *s_dots[3];
static int s_page;
static int s_file_page;

static lv_obj_t *s_status_dot;
static lv_obj_t *s_usb_dot;
static lv_obj_t *s_usb_txt;
static lv_obj_t *s_uptime;

static lv_obj_t *s_arc_glow;
static lv_obj_t *s_arc;
static lv_obj_t *s_pct;
static lv_obj_t *s_of;
static lv_obj_t *s_product;
static lv_obj_t *s_media_type;
static lv_obj_t *s_v_capacity;
static lv_obj_t *s_v_free;
static lv_obj_t *s_v_bus;
static lv_obj_t *s_v_serial;

static lv_obj_t *s_files_count;
static lv_obj_t *s_file_row[LIST_ROWS];
static lv_obj_t *s_file_name[LIST_ROWS];
static lv_obj_t *s_file_size[LIST_ROWS];
static lv_obj_t *s_file_type[LIST_ROWS];
static lv_obj_t *s_file_bar[LIST_ROWS];
static lv_obj_t *s_files_empty;
static lv_obj_t *s_files_hint;

static lv_obj_t *s_link_chip;
static lv_obj_t *s_link_state;
static lv_obj_t *s_link_sub;
static lv_obj_t *s_link_read;
static lv_obj_t *s_link_write;
static lv_obj_t *s_link_rx;
static lv_obj_t *s_link_tx;
static lv_obj_t *s_link_err;
static lv_obj_t *s_chart;
static lv_chart_series_t *s_ser_read;
static lv_chart_series_t *s_ser_write;

static lv_obj_t *s_splash_arc;

static void anim_x_cb(void *obj, int32_t v)
{
    lv_obj_set_x((lv_obj_t *)obj, v);
}

static void anim_arc_cb(void *obj, int32_t v)
{
    lv_arc_set_value((lv_obj_t *)obj, v);
}
static uint32_t s_events;
static portMUX_TYPE s_evt_mux = portMUX_INITIALIZER_UNLOCKED;
static uint32_t s_file_generation;
static uint32_t s_tick;
static uint8_t *s_screen_buf;
static volatile bool s_screen_req;
static volatile bool s_screen_ready;
static volatile uint32_t s_screen_len;
static volatile int s_pending_page = -1;

void ui_request_screen(void)
{
    if (!s_screen_buf) {
        s_screen_buf = heap_caps_malloc(172 * 320 * 2, MALLOC_CAP_8BIT);
    }
    s_screen_ready = false;
    s_screen_len = 0;
    s_screen_req = true;
}

void ui_request_page(int page)
{
    if (page >= 0 && page <= 2) s_pending_page = page;
}

bool ui_screen_ready(void)
{
    return s_screen_ready;
}

const uint8_t *ui_screen_data(uint32_t *len)
{
    if (!s_screen_ready) {
        if (len) *len = 0;
        return NULL;
    }
    if (len) *len = s_screen_len;
    return s_screen_buf;
}

void ui_screen_release(void)
{
    s_screen_ready = false;
    s_screen_len = 0;
    if (s_screen_buf) {
        heap_caps_free(s_screen_buf);
        s_screen_buf = NULL;
    }
}
static lv_obj_t *mk_label(lv_obj_t *parent, const char *txt, const lv_font_t *font,
                          uint32_t color, int x, int y)
{
    lv_obj_t *l = lv_label_create(parent);
    lv_label_set_text(l, txt);
    lv_obj_set_style_text_font(l, font, 0);
    lv_obj_set_style_text_color(l, lv_color_hex(color), 0);
    lv_obj_set_pos(l, x, y);
    return l;
}

static lv_obj_t *mk_rect(lv_obj_t *parent, int x, int y, int w, int h,
                         uint32_t color, int radius, lv_opa_t opa)
{
    lv_obj_t *o = lv_obj_create(parent);
    lv_obj_remove_style_all(o);
    lv_obj_clear_flag(o, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_pos(o, x, y);
    lv_obj_set_size(o, w, h);
    lv_obj_set_style_bg_color(o, lv_color_hex(color), 0);
    lv_obj_set_style_bg_opa(o, opa, 0);
    lv_obj_set_style_radius(o, radius, 0);
    return o;
}

static lv_obj_t *mk_chip(lv_obj_t *parent, int x, int y, int w, const char *txt, uint32_t color)
{
    lv_obj_t *o = mk_rect(parent, x, y, w, 16, C_PANEL2, 8, LV_OPA_COVER);
    lv_obj_set_style_border_width(o, 1, 0);
    lv_obj_set_style_border_color(o, lv_color_hex(color), 0);
    lv_obj_set_style_border_opa(o, LV_OPA_60, 0);
    lv_obj_t *l = mk_label(o, txt, &lv_font_montserrat_10, color, 0, 1);
    lv_obj_align(l, LV_ALIGN_CENTER, 0, 0);
    return o;
}

static void set_chip(lv_obj_t *chip, const char *txt, uint32_t color)
{
    if (!chip) return;
    lv_obj_set_style_border_color(chip, lv_color_hex(color), 0);
    lv_obj_t *l = lv_obj_get_child(chip, 0);
    if (l) {
        lv_label_set_text(l, txt);
        lv_obj_set_style_text_color(l, lv_color_hex(color), 0);
    }
}

static void fmt_bytes(uint64_t bytes, char *out, size_t n)
{
    if (bytes >= (uint64_t)1024 * 1024 * 1024) {
        snprintf(out, n, "%.1f GB", (double)bytes / (1024.0 * 1024.0 * 1024.0));
    } else if (bytes >= (uint64_t)1024 * 1024) {
        snprintf(out, n, "%.1f MB", (double)bytes / (1024.0 * 1024.0));
    } else {
        snprintf(out, n, "%llu KB", (unsigned long long)(bytes / 1024));
    }
}

static void fmt_rate(float mbps, char *out, size_t n)
{
    if (mbps < 0.05f) snprintf(out, n, "0.0 MB/s");
    else snprintf(out, n, "%.1f MB/s", mbps);
}

static void fmt_uptime(uint32_t sec, char *out, size_t n)
{
    if (sec >= 3600) snprintf(out, n, "%lu:%02lu:%02lu", (unsigned long)(sec / 3600), (unsigned long)((sec / 60) % 60), (unsigned long)(sec % 60));
    else snprintf(out, n, "%02lu:%02lu", (unsigned long)(sec / 60), (unsigned long)(sec % 60));
}

static void page_set(int idx, bool anim)
{
    if (idx < 0 || idx > 2) return;
    s_page = idx;
    int target = -idx * PAGE_W;
    if (anim) {
        lv_anim_t a;
        lv_anim_init(&a);
        lv_anim_set_var(&a, s_track);
        lv_anim_set_values(&a, lv_obj_get_x(s_track), target);
        lv_anim_set_time(&a, 260);
        lv_anim_set_path_cb(&a, lv_anim_path_ease_out);
        lv_anim_set_exec_cb(&a, anim_x_cb);
        lv_anim_start(&a);
    } else {
        lv_obj_set_x(s_track, target);
    }
    for (int i = 0; i < 3; i++) {
        lv_obj_set_style_bg_color(s_dots[i], lv_color_hex(i == idx ? C_CYAN : C_LINE), 0);
        lv_obj_set_size(s_dots[i], i == idx ? 5 : 3, i == idx ? 5 : 3);
        lv_obj_set_style_radius(s_dots[i], LV_RADIUS_CIRCLE, 0);
    }
    if (idx == 1) files_request_scan();
}

void ui_post_event(uint32_t evt)
{
    taskENTER_CRITICAL(&s_evt_mux);
    s_events |= evt;
    taskEXIT_CRITICAL(&s_evt_mux);
}

void ui_mark_dirty(void)
{
    s_file_generation = 0;
}

static void update_files(bool force)
{
    uint32_t gen = files_generation();
    if (!force && gen == s_file_generation) return;
    s_file_generation = gen;

    static file_entry_t entries[FILES_MAX];
    int dirs = 0;
    int count = files_snapshot(entries, FILES_MAX, &dirs);
    if (count < 0) count = 0;

    int pages = (count + LIST_ROWS - 1) / LIST_ROWS;
    if (pages < 1) pages = 1;
    if (s_file_page >= pages) s_file_page = pages - 1;
    if (s_file_page < 0) s_file_page = 0;

    lv_label_set_text_fmt(s_files_count, "%d ITEM%s", count, count == 1 ? "" : "S");
    if (count == 0) {
        lv_obj_clear_flag(s_files_empty, LV_OBJ_FLAG_HIDDEN);
        lv_obj_add_flag(s_files_hint, LV_OBJ_FLAG_HIDDEN);
    } else {
        lv_obj_add_flag(s_files_empty, LV_OBJ_FLAG_HIDDEN);
        lv_obj_clear_flag(s_files_hint, LV_OBJ_FLAG_HIDDEN);
        lv_label_set_text_fmt(s_files_hint, "%s  %d/%d",
                              s_file_page + 1 < pages ? "BOOT: NEXT" : "BOOT: LINK",
                              s_file_page + 1, pages);
    }

    for (int i = 0; i < LIST_ROWS; i++) {
        int idx = s_file_page * LIST_ROWS + i;
        if (idx >= count) {
            lv_obj_add_flag(s_file_row[i], LV_OBJ_FLAG_HIDDEN);
            continue;
        }
        lv_obj_clear_flag(s_file_row[i], LV_OBJ_FLAG_HIDDEN);
        const file_entry_t *e = &entries[idx];
        lv_label_set_text(s_file_name[i], e->name);
        lv_label_set_text(s_file_type[i], e->is_dir ? "DIR" : "FILE");
        char sz[16];
        if (e->is_dir) snprintf(sz, sizeof(sz), "--");
        else if (e->size >= 1024 * 1024) snprintf(sz, sizeof(sz), "%.1fM", (double)e->size / (1024 * 1024));
        else if (e->size >= 1024) snprintf(sz, sizeof(sz), "%.0fK", (double)e->size / 1024);
        else snprintf(sz, sizeof(sz), "%llu", (unsigned long long)e->size);
        lv_label_set_text(s_file_size[i], sz);
        lv_obj_set_style_bg_color(s_file_bar[i], lv_color_hex(e->is_dir ? C_CYAN : C_DIM), 0);
        lv_obj_set_style_text_color(s_file_type[i], lv_color_hex(e->is_dir ? C_CYAN : C_MUTED), 0);
    }
}

static void update_chart(const app_state_t *st)
{
    if (!s_chart) return;
    int start = (int)st->hist_index;
    int maxv = 0;
    for (int i = 0; i < STATS_HISTORY; i++) {
        int idx = (start + i) % STATS_HISTORY;
        s_ser_read->y_points[i] = st->hist_read[idx];
        s_ser_write->y_points[i] = st->hist_write[idx];
        if (st->hist_read[idx] > maxv) maxv = st->hist_read[idx];
        if (st->hist_write[idx] > maxv) maxv = st->hist_write[idx];
    }
    if (maxv == 0) {
        lv_chart_set_series_color(s_chart, s_ser_read, lv_color_hex(C_LINE));
        lv_chart_set_series_color(s_chart, s_ser_write, lv_color_hex(C_LINE));
    } else {
        lv_chart_set_series_color(s_chart, s_ser_read, lv_color_hex(C_CYAN));
        lv_chart_set_series_color(s_chart, s_ser_write, lv_color_hex(C_AMBER));
    }
    lv_chart_refresh(s_chart);
}

static void refresh_ui(void)
{
    app_state_t st;
    state_snapshot(&st);

    bool host = st.host_active;
    bool card = st.card_present;
    uint64_t total = st.total_bytes ? st.total_bytes : st.info.capacity_bytes;
    uint64_t freeb = st.total_bytes ? st.free_bytes : 0;
    /* Usage is only meaningful with a mounted filesystem; a card that is
     * present but unreadable must not render as "100% used". */
    uint64_t used = (st.fs_mounted && total > freeb) ? total - freeb : 0;
    int pct = (st.fs_mounted && total) ? (int)((used * 100) / total) : 0;
    if (pct > 100) pct = 100;

    lv_arc_set_value(s_arc, pct * 10);
    lv_arc_set_value(s_arc_glow, pct * 10);
    lv_label_set_text_fmt(s_pct, "%d%%", pct);

    char b1[32], b2[32];
    fmt_bytes(total, b1, sizeof(b1));
    lv_label_set_text_fmt(s_of, "OF %s", card ? b1 : "--");

    if (card) {
        lv_label_set_text(s_product, st.info.product[0] ? st.info.product : "TF MEDIA");
        const char *ctype = st.info.capacity_bytes > (uint64_t)32 * 1024 * 1024 * 1024 ? "SDXC"
                          : st.info.capacity_bytes > (uint64_t)2 * 1024 * 1024 * 1024 ? "SDHC" : "SDSC";
        const char *fs;
        uint32_t fs_color;
        if (st.fs_mounted) {
            fs = st.info.fs_type[0] ? st.info.fs_type : "FAT";
            fs_color = C_MUTED;
        } else if (st.info.fault) {
            fs = "FAULT";
            fs_color = C_RED;
        } else {
            fs = "NO FS";
            fs_color = C_AMBER;
        }
        lv_label_set_text_fmt(s_media_type, "%s  /  %s", ctype, fs);
        lv_obj_set_style_text_color(s_media_type, lv_color_hex(fs_color), 0);
    } else {
        lv_label_set_text(s_product, "NO MEDIA");
        lv_label_set_text(s_media_type, "INSERT TF CARD");
        lv_obj_set_style_text_color(s_media_type, lv_color_hex(C_MUTED), 0);
    }

    fmt_bytes(total, b1, sizeof(b1));
    fmt_bytes(freeb, b2, sizeof(b2));
    lv_label_set_text(s_v_capacity, b1);
    lv_label_set_text(s_v_free, st.fs_mounted ? b2 : "--");
    lv_label_set_text(s_v_bus, "20 MHz");
    lv_label_set_text_fmt(s_v_serial, "0x%08lX", (unsigned long)st.info.cid_serial);

    uint32_t dot_color = host ? C_MINT : st.fs_mounted ? C_CYAN
                       : card ? (st.info.fault ? C_RED : C_AMBER) : C_AMBER;
    lv_obj_set_style_bg_color(s_status_dot, lv_color_hex(dot_color), 0);
    lv_obj_set_style_bg_color(s_usb_dot, lv_color_hex(host ? C_MINT : C_DIM), 0);
    lv_label_set_text(s_usb_txt, host ? "ON" : "USB");
    lv_obj_set_style_text_color(s_usb_txt, lv_color_hex(host ? C_MINT : C_MUTED), 0);

    char up[16];
    fmt_uptime(st.uptime_s, up, sizeof(up));
    lv_label_set_text(s_uptime, up);

    if (host) {
        lv_label_set_text(s_link_state, "ONLINE");
        lv_obj_set_style_text_color(s_link_state, lv_color_hex(C_MINT), 0);
        lv_label_set_text(s_link_sub, "CARD LOCKED BY PC");
        set_chip(s_link_chip, "LINK", C_MINT);
    } else if (card && st.fs_mounted) {
        lv_label_set_text(s_link_state, "READY");
        lv_obj_set_style_text_color(s_link_state, lv_color_hex(C_TEXT), 0);
        lv_label_set_text(s_link_sub, "NBD /dev/nbd0");
        set_chip(s_link_chip, "IDLE", C_DIM);
    } else if (card) {
        lv_label_set_text(s_link_state, st.info.fault ? "FAULT" : "NO FS");
        lv_obj_set_style_text_color(s_link_state,
                                    lv_color_hex(st.info.fault ? C_RED : C_AMBER), 0);
        lv_label_set_text(s_link_sub, st.info.fault ? "DATA PATH BROKEN" : "FAT32 ONLY");
        set_chip(s_link_chip, st.info.fault ? "FAULT" : "RAW",
                 st.info.fault ? C_RED : C_AMBER);
    } else {
        lv_label_set_text(s_link_state, "NO CARD");
        lv_obj_set_style_text_color(s_link_state, lv_color_hex(C_AMBER), 0);
        lv_label_set_text(s_link_sub, "INSERT MEDIA");
        set_chip(s_link_chip, "HOLD", C_AMBER);
    }

    char rate[24];
    fmt_rate(st.read_mbps, rate, sizeof(rate));
    lv_label_set_text(s_link_read, rate);
    fmt_rate(st.write_mbps, rate, sizeof(rate));
    lv_label_set_text(s_link_write, rate);
    fmt_bytes(st.session_read, rate, sizeof(rate));
    lv_label_set_text(s_link_rx, rate);
    fmt_bytes(st.session_write, rate, sizeof(rate));
    lv_label_set_text(s_link_tx, rate);
    lv_label_set_text_fmt(s_link_err, "ERRORS  %u", (unsigned)st.errors);

    update_chart(&st);
    update_files(false);
}

static void handle_event(uint32_t evt)
{
    if (evt & UI_EVT_RESCAN) {
        files_request_scan();
        s_file_generation = 0;
    }
    if (evt & UI_EVT_NEXT) {
        if (s_page == 1) {
            static file_entry_t entries[FILES_MAX];
            int count = files_snapshot(entries, FILES_MAX, NULL);
            int pages = (count + LIST_ROWS - 1) / LIST_ROWS;
            if (s_file_page + 1 < pages) {
                s_file_page++;
                s_file_generation = 0;
                update_files(true);
            } else {
                page_set(2, true);
            }
        } else if (s_page == 2) {
            page_set(0, true);
        } else {
            page_set(1, true);
        }
    }
}

static void ui_timer_cb(lv_timer_t *t)
{
    (void)t;
    s_tick++;
    uint32_t evt;
    taskENTER_CRITICAL(&s_evt_mux);
    evt = s_events;
    s_events = 0;
    taskEXIT_CRITICAL(&s_evt_mux);
    if (evt) handle_event(evt);

    int pending = s_pending_page;
    if (pending >= 0) {
        s_pending_page = -1;
        page_set(pending, false);
    }

    if (s_screen_req) {
        s_screen_req = false;
        lv_img_dsc_t dsc;
        if (s_screen_buf &&
            lv_snapshot_take_to_buf(lv_scr_act(), LV_IMG_CF_TRUE_COLOR, &dsc,
                                    s_screen_buf, 172 * 320 * 2) == LV_RES_OK) {
            s_screen_len = (uint32_t)dsc.header.w * dsc.header.h * 2;
            s_screen_ready = true;
        } else {
            s_screen_len = 0;
            s_screen_ready = true;
        }
    }

    if ((s_tick % 5) == 0) state_stats_tick();
    if ((s_tick % 2) == 0) refresh_ui();
}

static void build_status(lv_obj_t *scr)
{
    lv_obj_t *bar = mk_rect(scr, 0, 0, PAGE_W, 26, C_BG, 0, LV_OPA_COVER);
    lv_obj_set_style_border_side(bar, LV_BORDER_SIDE_BOTTOM, 0);
    lv_obj_set_style_border_width(bar, 1, 0);
    lv_obj_set_style_border_color(bar, lv_color_hex(C_LINE), 0);
    lv_obj_set_style_border_opa(bar, LV_OPA_60, 0);

    s_status_dot = mk_rect(bar, 9, 11, 4, 4, C_CYAN, LV_RADIUS_CIRCLE, LV_OPA_COVER);
    mk_label(bar, "TFR", &lv_font_montserrat_12, C_TEXT, 17, 6);
    s_usb_txt = mk_label(bar, "USB", &lv_font_montserrat_10, C_MUTED, 72, 8);
    lv_obj_set_width(s_usb_txt, 28);
    s_usb_dot = mk_rect(bar, 102, 12, 3, 3, C_DIM, LV_RADIUS_CIRCLE, LV_OPA_COVER);
    s_uptime = mk_label(bar, "00:00", &lv_font_montserrat_10, C_DIM, 122, 8);
    lv_obj_set_width(s_uptime, 42);
    lv_obj_set_style_text_align(s_uptime, LV_TEXT_ALIGN_RIGHT, 0);
}

static void build_footer(lv_obj_t *scr)
{
    lv_obj_t *bar = mk_rect(scr, 0, 294, PAGE_W, 26, C_BG, 0, LV_OPA_COVER);
    lv_obj_set_style_border_side(bar, LV_BORDER_SIDE_TOP, 0);
    lv_obj_set_style_border_width(bar, 1, 0);
    lv_obj_set_style_border_color(bar, lv_color_hex(C_LINE), 0);
    lv_obj_set_style_border_opa(bar, LV_OPA_60, 0);
    mk_label(bar, "BOOT", &lv_font_montserrat_10, C_DIM, 8, 8);
    mk_label(bar, "v1.0", &lv_font_montserrat_10, C_DIM, 144, 8);
    for (int i = 0; i < 3; i++) {
        s_dots[i] = mk_rect(bar, 78 + i * 9, 11, 3, 3, i == 0 ? C_CYAN : C_LINE, LV_RADIUS_CIRCLE, LV_OPA_COVER);
    }
}

static void build_page_card(lv_obj_t *p)
{
    mk_label(p, "MEDIA", &lv_font_montserrat_12, C_MUTED, 8, 4);
    mk_chip(p, 128, 2, 36, "LIVE", C_CYAN);

    lv_obj_t *gauge = lv_obj_create(p);
    lv_obj_remove_style_all(gauge);
    lv_obj_clear_flag(gauge, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_pos(gauge, 24, 20);
    lv_obj_set_size(gauge, 124, 124);

    s_arc_glow = lv_arc_create(gauge);
    lv_obj_set_size(s_arc_glow, 124, 124);
    lv_obj_set_pos(s_arc_glow, 0, 0);
    lv_obj_remove_style(s_arc_glow, NULL, LV_PART_KNOB);
    lv_obj_set_style_arc_width(s_arc_glow, 12, LV_PART_MAIN);
    lv_obj_set_style_arc_width(s_arc_glow, 12, LV_PART_INDICATOR);
    lv_obj_set_style_arc_color(s_arc_glow, lv_color_hex(C_LINE), LV_PART_MAIN);
    lv_obj_set_style_arc_color(s_arc_glow, lv_color_hex(C_CYAN), LV_PART_INDICATOR);
    lv_obj_set_style_arc_opa(s_arc_glow, LV_OPA_30, LV_PART_INDICATOR);
    lv_obj_set_style_arc_rounded(s_arc_glow, true, LV_PART_INDICATOR);
    lv_arc_set_bg_angles(s_arc_glow, 135, 45);
    lv_arc_set_range(s_arc_glow, 0, 1000);

    s_arc = lv_arc_create(gauge);
    lv_obj_set_size(s_arc, 112, 112);
    lv_obj_set_pos(s_arc, 6, 6);
    lv_obj_remove_style(s_arc, NULL, LV_PART_KNOB);
    lv_obj_set_style_arc_width(s_arc, 7, LV_PART_MAIN);
    lv_obj_set_style_arc_width(s_arc, 7, LV_PART_INDICATOR);
    lv_obj_set_style_arc_color(s_arc, lv_color_hex(C_LINE), LV_PART_MAIN);
    lv_obj_set_style_arc_color(s_arc, lv_color_hex(C_CYAN), LV_PART_INDICATOR);
    lv_obj_set_style_arc_rounded(s_arc, true, LV_PART_INDICATOR);
    lv_arc_set_bg_angles(s_arc, 135, 45);
    lv_arc_set_range(s_arc, 0, 1000);

    lv_obj_t *used = mk_label(gauge, "USED", &lv_font_montserrat_10, C_MUTED, 0, 34);
    lv_obj_set_width(used, 124);
    lv_obj_set_style_text_align(used, LV_TEXT_ALIGN_CENTER, 0);
    s_pct = mk_label(gauge, "0%", &lv_font_montserrat_40, C_TEXT, 0, 43);
    lv_obj_set_width(s_pct, 124);
    lv_obj_set_style_text_align(s_pct, LV_TEXT_ALIGN_CENTER, 0);
    s_of = mk_label(gauge, "OF --", &lv_font_montserrat_12, C_MUTED, 0, 92);
    lv_obj_set_width(s_of, 124);
    lv_obj_set_style_text_align(s_of, LV_TEXT_ALIGN_CENTER, 0);

    s_product = mk_label(p, "TF MEDIA", &lv_font_montserrat_14, C_TEXT, 8, 148);
    lv_obj_set_width(s_product, 156);
    lv_obj_set_style_text_align(s_product, LV_TEXT_ALIGN_CENTER, 0);
    lv_label_set_long_mode(s_product, LV_LABEL_LONG_DOT);
    s_media_type = mk_label(p, "SDHC / FAT32", &lv_font_montserrat_12, C_MUTED, 8, 168);
    lv_obj_set_width(s_media_type, 156);
    lv_obj_set_style_text_align(s_media_type, LV_TEXT_ALIGN_CENTER, 0);

    mk_rect(p, 10, 188, 152, 1, C_LINE, 0, LV_OPA_70);

    mk_label(p, "CAPACITY", &lv_font_montserrat_12, C_MUTED, 10, 194);
    s_v_capacity = mk_label(p, "--", &lv_font_montserrat_12, C_TEXT, 100, 194);
    lv_obj_set_width(s_v_capacity, 62);
    lv_obj_set_style_text_align(s_v_capacity, LV_TEXT_ALIGN_RIGHT, 0);

    mk_label(p, "FREE", &lv_font_montserrat_12, C_MUTED, 10, 211);
    s_v_free = mk_label(p, "--", &lv_font_montserrat_12, C_TEXT, 100, 211);
    lv_obj_set_width(s_v_free, 62);
    lv_obj_set_style_text_align(s_v_free, LV_TEXT_ALIGN_RIGHT, 0);

    mk_label(p, "BUS", &lv_font_montserrat_12, C_MUTED, 10, 228);
    s_v_bus = mk_label(p, "20 MHz", &lv_font_montserrat_12, C_TEXT, 100, 228);
    lv_obj_set_width(s_v_bus, 62);
    lv_obj_set_style_text_align(s_v_bus, LV_TEXT_ALIGN_RIGHT, 0);

    mk_label(p, "SERIAL", &lv_font_montserrat_12, C_MUTED, 10, 245);
    s_v_serial = mk_label(p, "0x00000000", &lv_font_montserrat_12, C_TEXT, 70, 245);
    lv_obj_set_width(s_v_serial, 92);
    lv_obj_set_style_text_align(s_v_serial, LV_TEXT_ALIGN_RIGHT, 0);
}

static void build_page_files(lv_obj_t *p)
{
    mk_label(p, "FILES", &lv_font_montserrat_12, C_MUTED, 8, 4);
    mk_chip(p, 132, 2, 32, "ROOT", C_CYAN);
    s_files_count = mk_label(p, "0 ITEMS", &lv_font_montserrat_10, C_DIM, 8, 22);

    for (int i = 0; i < LIST_ROWS; i++) {
        lv_obj_t *row = lv_obj_create(p);
        lv_obj_remove_style_all(row);
        lv_obj_clear_flag(row, LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_set_pos(row, 0, 32 + i * 36);
        lv_obj_set_size(row, PAGE_W, 36);
        s_file_row[i] = row;
        s_file_bar[i] = mk_rect(row, 8, 9, 2, 18, C_DIM, 1, LV_OPA_COVER);
        s_file_type[i] = mk_label(row, "FILE", &lv_font_montserrat_10, C_MUTED, 16, 9);
        s_file_name[i] = mk_label(row, "--", &lv_font_montserrat_12, C_TEXT, 50, 7);
        lv_obj_set_width(s_file_name[i], 86);
        lv_label_set_long_mode(s_file_name[i], LV_LABEL_LONG_DOT);
        s_file_size[i] = mk_label(row, "--", &lv_font_montserrat_10, C_MUTED, 136, 9);
        lv_obj_set_width(s_file_size[i], 28);
        lv_obj_set_style_text_align(s_file_size[i], LV_TEXT_ALIGN_RIGHT, 0);
        lv_obj_t *line = mk_rect(row, 8, 35, 156, 1, C_LINE, 0, LV_OPA_40);
        (void)line;
    }

    s_files_empty = mk_label(p, "EMPTY\nNO FILES IN ROOT", &lv_font_montserrat_12, C_DIM, 8, 96);
    lv_obj_set_width(s_files_empty, 156);
    lv_obj_set_style_text_align(s_files_empty, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_add_flag(s_files_empty, LV_OBJ_FLAG_HIDDEN);
    s_files_hint = mk_label(p, "BOOT: NEXT", &lv_font_montserrat_10, C_DIM, 8, 250);
    lv_obj_set_width(s_files_hint, 156);
    lv_obj_set_style_text_align(s_files_hint, LV_TEXT_ALIGN_CENTER, 0);
}

static void build_metric(lv_obj_t *p, int x, int y, const char *label, lv_obj_t **value)
{
    lv_obj_t *box = mk_rect(p, x, y, 76, 34, C_PANEL, 6, LV_OPA_COVER);
    lv_obj_set_style_border_width(box, 1, 0);
    lv_obj_set_style_border_color(box, lv_color_hex(C_LINE), 0);
    lv_obj_set_style_border_opa(box, LV_OPA_60, 0);
    mk_label(box, label, &lv_font_montserrat_10, C_MUTED, 7, 4);
    *value = mk_label(box, "--", &lv_font_montserrat_14, C_TEXT, 7, 15);
}

static void build_page_link(lv_obj_t *p)
{
    mk_label(p, "USB LINK", &lv_font_montserrat_12, C_MUTED, 8, 4);
    s_link_chip = mk_chip(p, 128, 2, 36, "IDLE", C_DIM);

    s_link_state = mk_label(p, "READY", &lv_font_montserrat_28, C_TEXT, 8, 28);
    lv_obj_set_width(s_link_state, 156);
    lv_obj_set_style_text_align(s_link_state, LV_TEXT_ALIGN_CENTER, 0);
    s_link_sub = mk_label(p, "NBD /dev/nbd0", &lv_font_montserrat_12, C_MUTED, 8, 62);
    lv_obj_set_width(s_link_sub, 156);
    lv_obj_set_style_text_align(s_link_sub, LV_TEXT_ALIGN_CENTER, 0);

    lv_obj_t *badge = mk_rect(p, 64, 82, 44, 44, C_PANEL, 10, LV_OPA_COVER);
    lv_obj_set_style_border_width(badge, 1, 0);
    lv_obj_set_style_border_color(badge, lv_color_hex(C_CYAN), 0);
    lv_obj_set_style_border_opa(badge, LV_OPA_50, 0);
    lv_obj_t *usb = mk_label(badge, "USB", &lv_font_montserrat_12, C_CYAN, 0, 6);
    lv_obj_set_width(usb, 44);
    lv_obj_set_style_text_align(usb, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_t *cdc = mk_label(badge, "CDC", &lv_font_montserrat_10, C_DIM, 0, 24);
    lv_obj_set_width(cdc, 44);
    lv_obj_set_style_text_align(cdc, LV_TEXT_ALIGN_CENTER, 0);

    build_metric(p, 8, 138, "READ", &s_link_read);
    build_metric(p, 88, 138, "WRITE", &s_link_write);
    build_metric(p, 8, 176, "SESSION RX", &s_link_rx);
    build_metric(p, 88, 176, "SESSION TX", &s_link_tx);

    mk_label(p, "I/O 30s", &lv_font_montserrat_10, C_DIM, 8, 208);
    s_chart = lv_chart_create(p);
    lv_obj_set_pos(s_chart, 8, 218);
    lv_obj_set_size(s_chart, 156, 30);
    lv_obj_set_style_bg_opa(s_chart, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(s_chart, 0, 0);
    lv_obj_set_style_pad_all(s_chart, 0, 0);
    lv_chart_set_type(s_chart, LV_CHART_TYPE_LINE);
    lv_chart_set_point_count(s_chart, STATS_HISTORY);
    lv_chart_set_range(s_chart, LV_CHART_AXIS_PRIMARY_Y, 0, 100);
    lv_chart_set_div_line_count(s_chart, 0, 0);
    s_ser_read = lv_chart_add_series(s_chart, lv_color_hex(C_CYAN), LV_CHART_AXIS_PRIMARY_Y);
    s_ser_write = lv_chart_add_series(s_chart, lv_color_hex(C_AMBER), LV_CHART_AXIS_PRIMARY_Y);
    lv_chart_set_all_value(s_chart, s_ser_read, 0);
    lv_chart_set_all_value(s_chart, s_ser_write, 0);
    s_link_err = mk_label(p, "ERRORS  0", &lv_font_montserrat_10, C_DIM, 8, 252);
}

static void build_main(void)
{
    s_main = lv_obj_create(NULL);
    lv_obj_set_style_bg_color(s_main, lv_color_hex(C_BG), 0);
    lv_obj_set_style_bg_opa(s_main, LV_OPA_COVER, 0);
    lv_obj_clear_flag(s_main, LV_OBJ_FLAG_SCROLLABLE);

    build_status(s_main);

    s_viewport = lv_obj_create(s_main);
    lv_obj_remove_style_all(s_viewport);
    lv_obj_clear_flag(s_viewport, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_pos(s_viewport, 0, 26);
    lv_obj_set_size(s_viewport, PAGE_W, PAGE_H);
    lv_obj_set_style_clip_corner(s_viewport, true, 0);

    s_track = lv_obj_create(s_viewport);
    lv_obj_remove_style_all(s_track);
    lv_obj_clear_flag(s_track, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_pos(s_track, 0, 0);
    lv_obj_set_size(s_track, PAGE_W * 3, PAGE_H);

    for (int i = 0; i < 3; i++) {
        lv_obj_t *p = lv_obj_create(s_track);
        lv_obj_remove_style_all(p);
        lv_obj_clear_flag(p, LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_set_pos(p, i * PAGE_W, 0);
        lv_obj_set_size(p, PAGE_W, PAGE_H);
        s_pages[i] = p;
    }
    build_page_card(s_pages[0]);
    build_page_files(s_pages[1]);
    build_page_link(s_pages[2]);

    build_footer(s_main);
    page_set(0, false);
}

static void build_splash(void)
{
    lv_obj_t *scr = lv_scr_act();
    lv_obj_set_style_bg_color(scr, lv_color_hex(C_BG), 0);
    lv_obj_set_style_bg_opa(scr, LV_OPA_COVER, 0);
    lv_obj_clear_flag(scr, LV_OBJ_FLAG_SCROLLABLE);

    s_splash_arc = lv_arc_create(scr);
    lv_obj_set_size(s_splash_arc, 120, 120);
    lv_obj_set_pos(s_splash_arc, 26, 64);
    lv_obj_remove_style(s_splash_arc, NULL, LV_PART_KNOB);
    lv_obj_set_style_arc_width(s_splash_arc, 3, LV_PART_MAIN);
    lv_obj_set_style_arc_width(s_splash_arc, 3, LV_PART_INDICATOR);
    lv_obj_set_style_arc_color(s_splash_arc, lv_color_hex(C_LINE), LV_PART_MAIN);
    lv_obj_set_style_arc_color(s_splash_arc, lv_color_hex(C_CYAN), LV_PART_INDICATOR);
    lv_obj_set_style_arc_rounded(s_splash_arc, true, LV_PART_INDICATOR);
    lv_arc_set_bg_angles(s_splash_arc, 135, 45);
    lv_arc_set_range(s_splash_arc, 0, 1000);

    lv_obj_t *title = mk_label(scr, "TFR", &lv_font_montserrat_40, C_TEXT, 0, 210);
    lv_obj_set_width(title, PAGE_W);
    lv_obj_set_style_text_align(title, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_style_text_letter_space(title, 4, 0);
    lv_obj_t *sub = mk_label(scr, "VISUAL CARD READER", &lv_font_montserrat_10, C_CYAN, 0, 258);
    lv_obj_set_width(sub, PAGE_W);
    lv_obj_set_style_text_align(sub, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_style_text_letter_space(sub, 1, 0);
    lv_obj_t *cap = mk_label(scr, "MOUNTING MEDIA", &lv_font_montserrat_10, C_DIM, 0, 278);
    lv_obj_set_width(cap, PAGE_W);
    lv_obj_set_style_text_align(cap, LV_TEXT_ALIGN_CENTER, 0);

    lv_anim_t a;
    lv_anim_init(&a);
    lv_anim_set_var(&a, s_splash_arc);
    lv_anim_set_values(&a, 0, 1000);
    lv_anim_set_time(&a, 1200);
    lv_anim_set_path_cb(&a, lv_anim_path_ease_in_out);
    lv_anim_set_exec_cb(&a, anim_arc_cb);
    lv_anim_start(&a);
}

void ui_init(void)
{
    build_main();
    build_splash();
    lv_scr_load_anim(s_main, LV_SCR_LOAD_ANIM_FADE_ON, 450, 1250, true);
    lv_timer_create(ui_timer_cb, 100, NULL);
    ESP_LOGI(TAG, "UI ready");
}
