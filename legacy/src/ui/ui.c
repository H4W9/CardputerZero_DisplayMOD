/* DisplayMOD settings app UI (LVGL v9.2, keyboard only).
 * Drawn on the built-in screen (320x170) and, when the EXT display is up,
 * full size on it too. Keyboard focus lives on the built-in view; the EXT
 * view follows it.
 *   Up/Down  move      Left/Right  change value      Enter  toggle/open
 *   Esc/Back go back (exits on the home page)      A  apply queued changes
 *
 * Settings that only need a quick update apply at once. Settings that
 * restart the EXT display are queued and applied together (Apply / A).
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "lvgl.h"
#include "backend.h"
#include "ui.h"

#define C_BG      0x0E1115
#define C_PANEL   0x181D23
#define C_ROW     0x222931
#define C_FOCUS   0x2C5C78
#define C_ACC     0x38C8BE
#define C_FG      0xD7DEE4
#define C_DIM     0x78828C
#define C_OK_BG   0x1E5A3C
#define C_OK_FG   0x96F0AA
#define C_OFF_BG  0x3A3F46
#define C_WARN_BG 0x5A4418
#define C_WARN_FG 0xF0C850
#define C_RED_BG  0x5A2828
#define C_RED_FG  0xFFBEB4

enum page { P_HOME, P_SCREEN, P_STATUS, P_CONFIRM, P_DONE, P_WIRING, P_ABOUT, P_HISTORY, P_LEAVE };

#ifndef DM_VERSION
#define DM_VERSION "dev"
#endif
enum kind { K_TOGGLE, K_PICK, K_LINK, K_INFO, K_BUTTON };

typedef struct {
    enum kind kind;
    const char *label;
    const char *key;            /* setting key, if any */
    const char *const *vals;    /* picker values (NULL-terminated) */
    const char *const *names;   /* picker display names */
    const char *on_val, *off_val;
    int target;                 /* page for K_LINK, action for K_BUTTON */
} item_t;

static lv_group_t *g_group;
static lv_obj_t *g_list, *g_chip, *g_chip_lbl, *g_title, *g_busy, *g_hint;
static int g_exit_after;

/* Screens the app draws on: [0] built-in (keyboard focus), [1] EXT display */
#define MAX_VIEWS 2
typedef struct {
    lv_display_t *disp;
    lv_obj_t *list, *chip, *chip_lbl, *title, *hint, *busy;
} view_t;
static view_t g_v[MAX_VIEWS];
static int g_nv, g_cur;

static void use_view(int i)       /* the g_* widgets now point at this view */
{
    g_cur = i;
    g_list = g_v[i].list;
    g_chip = g_v[i].chip;
    g_chip_lbl = g_v[i].chip_lbl;
    g_title = g_v[i].title;
    g_hint = g_v[i].hint;
}

/* ------------------------------------------------- queued (pending) changes */
#define MAX_PEND 16
static char g_pk[MAX_PEND][24], g_pv[MAX_PEND][24];
static int g_np;

/* Read live by the mirror / BigTermCLI: no restart, applied at once */
static int is_live(const char *k)
{
    static const char *const L[] = {"BRIGHTNESS", "BORDER_COLOR", "BUILTIN_OFF", "SLEEP", "DIM_BUILTIN",
                                     "BIGTERM", "HISTORY", "HIST_SIZE", "HIST_DUPS", NULL};
    for (int i = 0; L[i]; i++) if (!strcmp(L[i], k)) return 1;
    return 0;
}

static int pend_idx(const char *k)
{
    for (int i = 0; i < g_np; i++) if (!strcmp(g_pk[i], k)) return i;
    return -1;
}

/* Current value as shown: queued value if there is one */
static const char *val(const char *k)
{
    int i = pend_idx(k);
    return i >= 0 ? g_pv[i] : dm_get(k);
}

static void pend_set(const char *k, const char *v)
{
    int i = pend_idx(k);
    if (!strcmp(dm_get(k), v)) {                   /* back to the saved value: drop it */
        if (i >= 0) { g_np--; memmove(g_pk[i], g_pk[i + 1], (g_np - i) * 24); memmove(g_pv[i], g_pv[i + 1], (g_np - i) * 24); }
        return;
    }
    if (i < 0) { if (g_np >= MAX_PEND) return; i = g_np++; snprintf(g_pk[i], 24, "%s", k); }
    snprintf(g_pv[i], 24, "%s", v);
}
static enum page g_page = P_HOME;
static int g_focus_idx;
static lv_timer_t *g_poll;
static int g_after_uninstall;

/* ------------------------------------------------------------- options */
static const char *const LAYOUT_V[] = {"bar", "stretch", "plain", NULL};
static const char *const LAYOUT_N[] = {"1:1 + status bar", "Stretch to fill", "1:1, no bar", NULL};
static const char *const HSIZE_V[] = {"100", "500", "1000", "5000", NULL};
static const char *const HSIZE_N[] = {"100", "500", "1000", "5000", NULL};
static const char *const SLEEP_V[] = {"1", "2", "5", "10", "30", "0", NULL};
static const char *const SLEEP_N[] = {"1 min", "2 min", "5 min", "10 min", "30 min", "Never", NULL};
static const char *const ALIGN_V[] = {"top", "center", "bottom", NULL};
static const char *const ALIGN_N[] = {"Top", "Center", "Bottom", NULL};
static const char *const BORDER_V[] = {"black", "charcoal", "slate", "navy", "teal", "green", "maroon", "purple", "white", NULL};
static const char *const BORDER_N[] = {"Black", "Charcoal", "Slate", "Navy", "Teal", "Green", "Maroon", "Purple", "White", NULL};
static const char *const ROT_V[] = {"90", "270", "0", "180", NULL};
static const char *const ROT_N[] = {"90\xC2\xB0 wide", "270\xC2\xB0 wide", "0\xC2\xB0 tall", "180\xC2\xB0 tall", NULL};
static const char *const FONT_V[] = {"6x12", "8x8", "8x16", NULL};      /* same order as F6 in BigTermCLI */
static const char *const FONT_N[] = {"6x12 \xE2\x80\xA2 53x20", "8x8 \xE2\x80\xA2 40x30", "8x16 \xE2\x80\xA2 40x15", NULL};
static const char *const BRIGHT_V[] = {"sync", "10", "20", "30", "40", "50", "60", "70", "80", "90", "100", NULL};
static const char *const BRIGHT_N[] = {"Match built-in", "10%", "20%", "30%", "40%", "50%", "60%", "70%", "80%", "90%", "100%", NULL};
static const char *const SPEED_V[] = {"8000000", "16000000", "24000000", "32000000", NULL};
static const char *const SPEED_N[] = {"8 MHz", "16 MHz", "24 MHz", "32 MHz", NULL};

enum { ACT_UNINSTALL = 1, ACT_CANCEL = 2, ACT_EXIT = 3, ACT_RECHECK = 4, ACT_CLEAR_HIST = 5,
       ACT_APPLY = 6, ACT_APPLY_EXIT = 7 };

static const item_t HOME[] = {
    {K_TOGGLE, "Mirror to EXT display", "MODE", NULL, NULL, "mirror", "off", 0},
    {K_TOGGLE, "BigTermCLI app", "BIGTERM", NULL, NULL, "1", "0", 0},
    {K_PICK, "Layout", "MIRROR_LAYOUT", LAYOUT_V, LAYOUT_N, NULL, NULL, 0},
    {K_PICK, "Position", "ALIGN", ALIGN_V, ALIGN_N, NULL, NULL, 0},
    {K_PICK, "Border color", "BORDER_COLOR", BORDER_V, BORDER_N, NULL, NULL, 0},
    {K_TOGGLE, "Built-in off while mirroring", "BUILTIN_OFF", NULL, NULL, "1", "0", 0},
    {K_TOGGLE, "Built-in dark in BigTermCLI", "DIM_BUILTIN", NULL, NULL, "1", "0", 0},
    {K_LINK, "BigTermCLI history", NULL, NULL, NULL, NULL, NULL, P_HISTORY},
    {K_LINK, "EXT display settings", NULL, NULL, NULL, NULL, NULL, P_SCREEN},
    {K_LINK, "Status & uninstall", NULL, NULL, NULL, NULL, NULL, P_STATUS},
    {K_LINK, "Wiring help", NULL, NULL, NULL, NULL, NULL, P_WIRING},
    {K_LINK, "About", NULL, NULL, NULL, NULL, NULL, P_ABOUT},
};
static const item_t SCREEN[] = {
    {K_PICK, "Brightness", "BRIGHTNESS", BRIGHT_V, BRIGHT_N, NULL, NULL, 0},
    {K_PICK, "Sleep (built-in off)", "SLEEP", SLEEP_V, SLEEP_N, NULL, NULL, 0},
    {K_PICK, "Rotation", "ROTATE", ROT_V, ROT_N, NULL, NULL, 0},
    {K_PICK, "BigTermCLI font", "FONT", FONT_V, FONT_N, NULL, NULL, 0},
    {K_PICK, "SPI speed", "SPEED", SPEED_V, SPEED_N, NULL, NULL, 0},
    {K_TOGGLE, "Swap red / blue", "BGR", NULL, NULL, "1", "0", 0},
    {K_TOGGLE, "Start only if detected", "DETECT", NULL, NULL, "1", "0", 0},
    {K_INFO, "Pins DC / RST / LED", NULL, NULL, NULL, NULL, NULL, 0},   /* fixed: 23 / 22 / 26 */
};
static const item_t HISTORY[] = {
    {K_TOGGLE, "Save command history", "HISTORY", NULL, NULL, "1", "0", 0},
    {K_PICK, "Keep last", "HIST_SIZE", HSIZE_V, HSIZE_N, NULL, NULL, 0},
    {K_TOGGLE, "Skip repeated commands", "HIST_DUPS", NULL, NULL, "1", "0", 0},
    {K_BUTTON, "Clear history", NULL, NULL, NULL, NULL, NULL, ACT_CLEAR_HIST},
};
static const item_t CONFIRM[] = {
    {K_INFO, "Remove DisplayMOD and", NULL, NULL, NULL, NULL, NULL, 0},
    {K_INFO, "turn the EXT display off?", NULL, NULL, NULL, NULL, NULL, 0},
    {K_BUTTON, "Yes, uninstall", NULL, NULL, NULL, NULL, NULL, ACT_UNINSTALL},
    {K_BUTTON, "Cancel", NULL, NULL, NULL, NULL, NULL, ACT_CANCEL},
};

/* Wiring help: display pin -> header pin (CardputerZero signal) */
static const char *const WIRING[][2] = {
    {"Power off before wiring!", ""},
    {"VCC", "pin 6   5V out"},
    {"GND", "pin 4   GND"},
    {"CS", "pin 13  G7 (CE1)"},
    {"RESET", "pin 5   G22"},
    {"DC", "pin 3   G23"},
    {"SDI (MOSI)", "pin 9   G10"},
    {"SCK", "pin 7   G11"},
    {"LED", "pin 1   G26"},
    {"SDO (MISO)", "pin 11  G9  (detect)"},
    {"Touch / SD pins", "leave unconnected"},
    {"Pin 2 (5V in)", "do not use"},
};

/* About page. '#' = heading, '-' = bullet, '' = spacer */
static const char *const ABOUT[] = {
    "#DisplayMOD " DM_VERSION,
    "Second screen for the M5Stack CardputerZero",
    "2.8\" ILI9341 SPI display (EXT display)",
    "by H4W9",
    "",
    "#Mirror",
    "-1:1 copy of the built-in screen",
    "-Status bar: clock, battery, IP, RAM, CPU",
    "-Layouts: status bar, stretch, no bar",
    "-No-bar position and border color",
    "-This app fills the EXT display while open",
    "",
    "#BigTermCLI",
    "-Full terminal on the EXT display",
    "-Same keys and colors as the CLI app",
    "-Command history kept across reboots",
    "-Starts in your home folder",
    "",
    "#Power",
    "-Brightness follows the built-in screen",
    "-Built-in screen can stay off",
    "-EXT display sleeps when idle",
    "-Stays off if no EXT display is found",
    "",
    "#Keys",
    "F / X  up / down     Z / C  change",
    "Enter  select        Esc    back / exit",
    "A      apply queued changes",
    "",
    "#More",
    "Terminal version: displaymod",
    "Remove: sudo apt remove displaymod",
};

static void build_page(void);
static void flush_live(void);
static void rebuild_async(void *p) { (void)p; build_page(); }

/* -------------------------------------------------------------- styles */
static void style_box(lv_obj_t *o, uint32_t bg, int radius)
{
    lv_obj_remove_style_all(o);
    lv_obj_set_style_bg_color(o, lv_color_hex(bg), 0);
    lv_obj_set_style_bg_opa(o, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(o, radius, 0);
}

static lv_obj_t *text(lv_obj_t *parent, const char *s, const lv_font_t *f, uint32_t col)
{
    lv_obj_t *l = lv_label_create(parent);
    lv_label_set_text(l, s);
    lv_obj_set_style_text_font(l, f, 0);
    lv_obj_set_style_text_color(l, lv_color_hex(col), 0);
    return l;
}

/* ------------------------------------------------------------ helpers */
static int pick_index(const item_t *it)
{
    const char *cur = val(it->key);
    for (int i = 0; it->vals[i]; i++)
        if (!strcmp(it->vals[i], cur)) return i;
    return 0;
}

static int pick_count(const item_t *it)
{
    int n = 0;
    while (it->vals[n]) n++;
    return n;
}

static void update_chip(int st)
{
    const char *txt = LV_SYMBOL_OK " EXT DISPLAY";
    uint32_t bg = C_OK_BG, fg = C_OK_FG;
    if (st < 0) { txt = LV_SYMBOL_CLOSE " NO EXT DISPLAY"; bg = C_WARN_BG; fg = C_WARN_FG; }
    else if (st == 0) { txt = "EXT DISPLAY OFF"; bg = C_OFF_BG; fg = C_FG; }
    lv_label_set_text(g_chip_lbl, txt);
    lv_obj_set_style_bg_color(g_chip, lv_color_hex(bg), 0);
    lv_obj_set_style_text_color(g_chip_lbl, lv_color_hex(fg), 0);
}

/* ------------------------------------------------------------- busy UI */
static void poll_cb(lv_timer_t *t)
{
    (void)t;
    int rc;
    if (!dm_poll(&rc)) return;
    lv_timer_delete(g_poll);
    g_poll = NULL;
    if (g_busy) {
        for (int i = 0; i < g_nv; i++)
            if (g_v[i].busy) { lv_obj_delete(g_v[i].busy); g_v[i].busy = NULL; }
        g_busy = NULL;
    }
    if (g_after_uninstall) { g_page = P_DONE; g_after_uninstall = 0; }
    if (g_exit_after) { ui_request_exit(); return; }
    flush_live();
    build_page();
}

static void show_busy(const char *msg)
{
    for (int i = 0; i < g_nv; i++) {
        lv_obj_t *b = lv_obj_create(lv_display_get_layer_top(g_v[i].disp));
        style_box(b, 0x000000, 0);
        lv_obj_set_style_bg_opa(b, LV_OPA_70, 0);
        lv_obj_set_size(b, LV_PCT(100), LV_PCT(100));
        lv_obj_t *sp = lv_spinner_create(b);
        lv_obj_set_size(sp, 34, 34);
        lv_obj_set_style_arc_width(sp, 4, LV_PART_MAIN);
        lv_obj_set_style_arc_width(sp, 4, LV_PART_INDICATOR);
        lv_obj_set_style_arc_color(sp, lv_color_hex(C_ACC), LV_PART_INDICATOR);
        lv_obj_align(sp, LV_ALIGN_CENTER, 0, -12);
        lv_obj_t *l = text(b, msg, &lv_font_montserrat_12, C_FG);
        lv_obj_align(l, LV_ALIGN_CENTER, 0, 24);
        g_v[i].busy = b;
    }
    g_busy = g_v[0].busy;
    g_poll = lv_timer_create(poll_cb, 150, NULL);
}

static void quiet_poll(void)
{
    if (!g_poll) g_poll = lv_timer_create(poll_cb, 150, NULL);
}

/* Quick settings changed while the helper was busy wait here and are
 * sent as soon as it's free. */
static void flush_live(void)
{
    const char *kv[2 * MAX_PEND];
    static char k[MAX_PEND][24], v[MAX_PEND][24];
    int n = 0, had = g_np > 0;
    for (int i = 0; i < g_np;) {
        if (!is_live(g_pk[i])) { i++; continue; }
        memcpy(k[n], g_pk[i], 24); memcpy(v[n], g_pv[i], 24);
        kv[2 * n] = k[n]; kv[2 * n + 1] = v[n]; n++;
        g_np--; memmove(g_pk[i], g_pk[i + 1], (g_np - i) * 24); memmove(g_pv[i], g_pv[i + 1], (g_np - i) * 24);
    }
    if (n && dm_set_many_async(kv, n) == 0) quiet_poll();
    if (had && !g_np && g_focus_idx > 0) g_focus_idx--;    /* Apply row went away */
}

/* A value was changed on screen */
static void change(const char *key, const char *v)
{
    if (g_busy) return;
    int had = g_np > 0;
    if (is_live(key) && pend_idx(key) < 0 && dm_set_async(key, v) == 0) quiet_poll();
    else pend_set(key, v);
    if (g_page == P_HOME || g_page == P_SCREEN) {          /* Apply row at the top */
        if (!had && g_np) g_focus_idx++;
        else if (had && !g_np && g_focus_idx > 0) g_focus_idx--;
    }
    lv_async_call(rebuild_async, NULL);
}

static void apply_pending(int then_exit)
{
    if (g_busy || !g_np) { if (then_exit) ui_request_exit(); return; }
    const char *kv[2 * MAX_PEND];
    for (int i = 0; i < g_np; i++) { kv[2 * i] = g_pk[i]; kv[2 * i + 1] = g_pv[i]; }
    if (dm_set_many_async(kv, g_np) != 0) return;
    char msg[40];
    snprintf(msg, sizeof msg, "Applying %d change%s...", g_np, g_np == 1 ? "" : "s");
    g_np = 0;
    if (g_focus_idx > 0) g_focus_idx--;                    /* Apply row goes away */
    g_exit_after = then_exit;
    show_busy(msg);
}

/* ------------------------------------------------------------- events */
/* The EXT view has no keyboard focus of its own: highlight the same row */
static void mirror_focus(int idx)
{
    for (int v = 1; v < g_nv; v++) {
        lv_obj_t *list = g_v[v].list;
        uint32_t n = lv_obj_get_child_count(list);
        for (uint32_t i = 0; i < n; i++) {
            lv_obj_t *c = lv_obj_get_child(list, i);
            if (!lv_obj_has_flag(c, LV_OBJ_FLAG_USER_1)) continue;
            if ((int)(intptr_t)lv_obj_get_user_data(c) == idx) {
                lv_obj_add_state(c, LV_STATE_FOCUSED);
                lv_obj_scroll_to_view(c, LV_ANIM_ON);
            } else {
                lv_obj_remove_state(c, LV_STATE_FOCUSED);
            }
        }
    }
}

static void row_focused(lv_event_t *e)
{
    lv_obj_t *row = lv_event_get_target(e);
    lv_obj_scroll_to_view(row, LV_ANIM_ON);
    g_focus_idx = (int)(intptr_t)lv_obj_get_user_data(row);
    mirror_focus(g_focus_idx);
}

static void go(enum page p)
{
    g_page = p;
    g_focus_idx = 0;
    lv_async_call(rebuild_async, NULL);
}

static void row_key(lv_event_t *e)
{
    if (g_busy) return;
    const item_t *it = lv_event_get_user_data(e);
    uint32_t k = lv_event_get_key(e);

    switch (k) {
    case LV_KEY_UP: case ';': case 'w':
        lv_group_focus_prev(g_group); return;
    case LV_KEY_DOWN: case '.': case 's':
        lv_group_focus_next(g_group); return;
    case LV_KEY_ESC: case LV_KEY_BACKSPACE:
        if (g_page == P_HOME) { if (g_np) go(P_LEAVE); else ui_request_exit(); return; }
        go(g_page == P_CONFIRM ? P_STATUS : P_HOME);
        return;
    case 'a': case 'A':
        apply_pending(0);
        return;
    default: break;
    }
    if (!it) return;

    int left = (k == LV_KEY_LEFT || k == ',' || k == 'a');
    int right = (k == LV_KEY_RIGHT || k == '/' || k == 'd');
    int enter = (k == LV_KEY_ENTER || k == ' ');

    switch (it->kind) {
    case K_TOGGLE:
        if (enter || left || right) {
            int on = !strcmp(val(it->key), it->on_val);
            if ((left && !on) || (right && on)) return;
            change(it->key, on ? it->off_val : it->on_val);
        }
        break;
    case K_PICK:
        if (left || right || enter) {
            int n = pick_count(it), i = pick_index(it);
            i = left ? (i + n - 1) % n : (i + 1) % n;
            change(it->key, it->vals[i]);
        }
        break;
    case K_LINK:
        if (enter || right) go(it->target);
        break;
    case K_BUTTON:
        if (!enter) break;
        if (it->target == ACT_EXIT) ui_request_exit();
        else if (it->target == ACT_APPLY) apply_pending(0);
        else if (it->target == ACT_APPLY_EXIT) apply_pending(1);
        else if (it->target == ACT_CLEAR_HIST) { dm_history_clear(); lv_async_call(rebuild_async, NULL); }
        else if (it->target == ACT_RECHECK && !g_busy && dm_apply_async() == 0)
            show_busy("Looking for the EXT display...");
        else if (it->target == ACT_CANCEL) go(P_STATUS);
        else if (it->target == ACT_UNINSTALL && !g_busy && dm_uninstall_async() == 0) {
            g_after_uninstall = 1;
            show_busy("Removing DisplayMOD...");
        }
        break;
    default: break;
    }
}

/* --------------------------------------------------------------- rows */
static lv_obj_t *make_row(const item_t *it, int idx, int focusable)
{
    lv_obj_t *row = lv_obj_create(g_list);
    style_box(row, C_ROW, 5);
    lv_obj_set_style_bg_color(row, lv_color_hex(C_FOCUS), LV_STATE_FOCUSED);
    lv_obj_set_size(row, LV_PCT(100), 22);
    lv_obj_set_style_pad_hor(row, 8, 0);
    lv_obj_remove_flag(row, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_user_data(row, (void *)(intptr_t)idx);

    lv_obj_t *l = text(row, it->label, &lv_font_montserrat_12, C_FG);
    lv_obj_align(l, LV_ALIGN_LEFT_MID, 0, 0);

    if (it->kind == K_TOGGLE) {
        lv_obj_t *sw = lv_switch_create(row);
        lv_obj_set_size(sw, 28, 14);
        lv_obj_align(sw, LV_ALIGN_RIGHT_MID, 0, 0);
        lv_obj_remove_flag(sw, LV_OBJ_FLAG_CLICK_FOCUSABLE);
        lv_obj_set_style_bg_color(sw, lv_color_hex(0x464E58), LV_PART_MAIN);
        lv_obj_set_style_bg_color(sw, lv_color_hex(C_ACC), LV_PART_INDICATOR | LV_STATE_CHECKED);
        lv_obj_set_style_pad_all(sw, -2, LV_PART_KNOB);
        if (!strcmp(val(it->key), it->on_val)) lv_obj_add_state(sw, LV_STATE_CHECKED);
        if (pend_idx(it->key) >= 0) {             /* queued: amber until applied */
            lv_obj_set_style_bg_color(sw, lv_color_hex(C_WARN_FG), LV_PART_INDICATOR | LV_STATE_CHECKED);
            lv_obj_set_style_bg_color(sw, lv_color_hex(C_WARN_BG), LV_PART_MAIN);
        }
    } else if (it->kind == K_PICK) {
        char buf[64];
        snprintf(buf, sizeof buf, LV_SYMBOL_LEFT "  %s  " LV_SYMBOL_RIGHT, it->names[pick_index(it)]);
        lv_obj_t *v = text(row, buf, &lv_font_montserrat_12, pend_idx(it->key) >= 0 ? C_WARN_FG : 0xBEC6CE);
        lv_obj_align(v, LV_ALIGN_RIGHT_MID, 0, 0);
    } else if (it->kind == K_LINK) {
        lv_obj_t *v = text(row, LV_SYMBOL_RIGHT, &lv_font_montserrat_12, C_DIM);
        lv_obj_align(v, LV_ALIGN_RIGHT_MID, 0, 0);
    } else if (it->kind == K_INFO && it->key == NULL && g_page == P_SCREEN) {
        char buf[48];
        snprintf(buf, sizeof buf, "%s / %s / %s", dm_get("DC"), dm_get("RST"), dm_get("LED"));
        lv_obj_t *v = text(row, buf, &lv_font_montserrat_12, C_DIM);
        lv_obj_align(v, LV_ALIGN_RIGHT_MID, 0, 0);
    } else if (it->kind == K_BUTTON) {
        lv_obj_align(l, LV_ALIGN_CENTER, 0, 0);
        if (it->target == ACT_UNINSTALL) {
            lv_obj_set_style_bg_color(row, lv_color_hex(C_RED_BG), 0);
            lv_obj_set_style_bg_color(row, lv_color_hex(0x8A3030), LV_STATE_FOCUSED);
            lv_obj_set_style_text_color(l, lv_color_hex(C_RED_FG), 0);
        }
    }
    if (it->kind == K_INFO) {
        lv_obj_set_style_bg_opa(row, LV_OPA_TRANSP, 0);
        lv_obj_set_height(row, 18);
    }

    if (focusable && g_cur == 0) {
        lv_group_add_obj(g_group, row);
        lv_obj_add_event_cb(row, row_key, LV_EVENT_KEY, (void *)it);
        lv_obj_add_event_cb(row, row_focused, LV_EVENT_FOCUSED, NULL);
    } else if (focusable) {
        lv_obj_add_flag(row, LV_OBJ_FLAG_USER_1);    /* follows the built-in view's focus */
    }
    return row;
}

static void status_rows(const dm_pair_t *st, int n)
{
    for (int i = 0; i < n; i++) {
        lv_obj_t *row = lv_obj_create(g_list);
        lv_obj_remove_style_all(row);
        lv_obj_set_size(row, LV_PCT(100), 15);
        lv_obj_t *k = text(row, st[i].key, &lv_font_montserrat_12, C_DIM);
        lv_obj_align(k, LV_ALIGN_LEFT_MID, 8, 0);
        lv_obj_t *v = text(row, st[i].val, &lv_font_montserrat_12, C_FG);
        lv_obj_align(v, LV_ALIGN_LEFT_MID, 112, 0);
    }
    static const item_t UNINSTALL = {K_LINK, "Uninstall DisplayMOD...", NULL, NULL, NULL, NULL, NULL, P_CONFIRM};
    static const item_t BACK = {K_LINK, "Back", NULL, NULL, NULL, NULL, NULL, P_HOME};
    static const item_t RECHECK = {K_BUTTON, "Check for screen again", NULL, NULL, NULL, NULL, NULL, ACT_RECHECK};
    make_row(&RECHECK, 0, 1);
    lv_obj_t *u = make_row(&UNINSTALL, 1, 1);
    lv_obj_set_style_bg_color(u, lv_color_hex(C_RED_BG), 0);
    lv_obj_set_style_bg_color(u, lv_color_hex(0x8A3030), LV_STATE_FOCUSED);
    make_row(&BACK, 2, 1);
}

static void about_rows(void)
{
    for (unsigned i = 0; i < sizeof ABOUT / sizeof ABOUT[0]; i++) {
        static const item_t info = {K_INFO, "", NULL, NULL, NULL, NULL, NULL, 0};
        const char *s = ABOUT[i];
        lv_obj_t *row = make_row(&info, i, 1);   /* focusable so it scrolls */
        lv_obj_set_style_bg_opa(row, LV_OPA_COVER, LV_STATE_FOCUSED);
        lv_obj_t *l = lv_obj_get_child(row, 0);
        if (s[0] == '#') {
            lv_label_set_text(l, s + 1);
            lv_obj_set_style_text_color(l, lv_color_hex(C_ACC), 0);
            lv_obj_set_style_text_font(l, &lv_font_montserrat_14, 0);
        } else if (s[0] == '-') {
            lv_label_set_text_fmt(l, LV_SYMBOL_OK "  %s", s + 1);
        } else {
            lv_label_set_text(l, s);
            lv_obj_set_style_text_color(l, lv_color_hex(0xBEC6CE), 0);
            if (!s[0]) lv_obj_set_height(row, 6);
        }
    }
}

static void wiring_rows(void)
{
    for (unsigned i = 0; i < sizeof WIRING / sizeof WIRING[0]; i++) {
        static const item_t info = {K_INFO, "", NULL, NULL, NULL, NULL, NULL, 0};
        lv_obj_t *row = make_row(&info, i, 1);   /* focusable so it scrolls */
        lv_obj_set_style_bg_opa(row, LV_OPA_COVER, LV_STATE_FOCUSED);
        lv_obj_t *k = lv_obj_get_child(row, 0);
        lv_label_set_text(k, WIRING[i][0]);
        if (i == 0) lv_obj_set_style_text_color(k, lv_color_hex(0xF0C850), 0);
        lv_obj_t *v = text(row, WIRING[i][1], &lv_font_montserrat_12, C_ACC);
        lv_obj_align(v, LV_ALIGN_LEFT_MID, 104, 0);
    }
}

/* Position and border color only mean something in the "1:1, no bar" layout */
static int shown(const item_t *it)
{
    if (it->key && (!strcmp(it->key, "ALIGN") || !strcmp(it->key, "BORDER_COLOR")))
        return !strcmp(val("MIRROR_LAYOUT"), "plain");
    return 1;
}

/* ---------------------------------------------------------------- page */
/* Build one view of the page (g_* point at it). */
static void build_view(enum page pg, int chip, const dm_pair_t *st, int stn)
{
    lv_obj_clean(g_list);
    lv_obj_scroll_to_y(g_list, 0, LV_ANIM_OFF);
    update_chip(chip);

    const item_t *items = NULL;
    int n = 0, fi = 0;
    lv_label_set_text(g_hint, g_np ? LV_SYMBOL_UP LV_SYMBOL_DOWN " move   " LV_SYMBOL_LEFT LV_SYMBOL_RIGHT
                                     " change   A apply   ESC back"
                                   : LV_SYMBOL_UP LV_SYMBOL_DOWN " move   " LV_SYMBOL_LEFT LV_SYMBOL_RIGHT
                                     " change   ENTER select   ESC back");
    if (g_np && (pg == P_HOME || pg == P_SCREEN)) {
        static char lbl[32];
        snprintf(lbl, sizeof lbl, "Apply changes (%d)", g_np);
        static item_t ap = {K_BUTTON, "", NULL, NULL, NULL, NULL, NULL, ACT_APPLY};
        ap.label = lbl;
        lv_obj_t *r = make_row(&ap, fi++, 1);
        lv_obj_set_style_bg_color(r, lv_color_hex(C_WARN_BG), 0);
        lv_obj_set_style_bg_color(r, lv_color_hex(0x8A6A20), LV_STATE_FOCUSED);
        lv_obj_set_style_text_color(lv_obj_get_child(r, 0), lv_color_hex(C_WARN_FG), 0);
    }
    switch (pg) {
    case P_HOME:    lv_label_set_text(g_title, "DisplayMOD"); items = HOME; n = 12; break;
    case P_SCREEN:  lv_label_set_text(g_title, "EXT display"); items = SCREEN; n = 8; break;
    case P_CONFIRM: lv_label_set_text(g_title, "Uninstall"); items = CONFIRM; n = 4; break;
    case P_STATUS:  lv_label_set_text(g_title, "Status"); status_rows(st, stn); break;
    case P_WIRING:  lv_label_set_text(g_title, "Wiring help"); wiring_rows(); break;
    case P_ABOUT:   lv_label_set_text(g_title, "About"); about_rows(); break;
    case P_LEAVE: {
        lv_label_set_text(g_title, "Unapplied changes");
        static char msg[48];
        snprintf(msg, sizeof msg, "%d change%s not applied yet.", g_np, g_np == 1 ? " is" : "s are");
        static item_t m = {K_INFO, "", NULL, NULL, NULL, NULL, NULL, 0};
        static const item_t a = {K_BUTTON, "Apply and exit", NULL, NULL, NULL, NULL, NULL, ACT_APPLY_EXIT};
        static const item_t d = {K_BUTTON, "Exit without applying", NULL, NULL, NULL, NULL, NULL, ACT_EXIT};
        static const item_t b = {K_LINK, "Back", NULL, NULL, NULL, NULL, NULL, P_HOME};
        m.label = msg;
        make_row(&m, 0, 0); make_row(&a, 0, 1); make_row(&d, 1, 1); make_row(&b, 2, 1);
        break;
    }
    case P_HISTORY: {
        lv_label_set_text(g_title, "BigTermCLI history");
        static char cnt[48];
        snprintf(cnt, sizeof cnt, "Saved commands: %d", dm_history_count());
        static item_t info = {K_INFO, "", NULL, NULL, NULL, NULL, NULL, 0};
        info.label = cnt;
        make_row(&info, 0, 0);
        items = HISTORY; n = 4;
        break;
    }
    case P_DONE: {
        lv_label_set_text(g_title, "Removed");
        static const item_t a = {K_INFO, "DisplayMOD was removed.", NULL, NULL, NULL, NULL, NULL, 0};
        static const item_t b = {K_INFO, "Press Esc to return to the launcher.", NULL, NULL, NULL, NULL, NULL, 0};
        static const item_t x = {K_BUTTON, "Exit", NULL, NULL, NULL, NULL, NULL, ACT_EXIT};
        make_row(&a, 0, 0); make_row(&b, 1, 0); make_row(&x, 2, 1);
        break;
    }
    }
    for (int i = 0; i < n; i++) {
        if (!shown(&items[i])) continue;
        int focusable = items[i].kind != K_INFO || g_page == P_SCREEN;
        make_row(&items[i], fi, focusable);
        if (focusable) fi++;
    }
}

static void build_page(void)
{
    enum page pg = g_page;
    int chip = dm_panel_state();
    dm_pair_t st[DM_MAX_STATUS];
    int stn = pg == P_STATUS ? dm_status(st, DM_MAX_STATUS) : 0;
    lv_group_remove_all_objs(g_group);
    for (int v = g_nv - 1; v >= 0; v--) {          /* built-in view last: g_* end up on it */
        use_view(v);
        build_view(pg, chip, st, stn);
    }
    if (pg == P_DONE) g_page = P_HOME;            /* Esc now exits */
    /* restore focus */
    uint32_t cnt = lv_group_get_obj_count(g_group);
    if (cnt) {
        if (g_focus_idx >= (int)cnt) g_focus_idx = cnt - 1;
        lv_group_focus_obj(lv_group_get_obj_by_index(g_group, g_focus_idx));
    }
    if (pg == P_STATUS)                            /* show info first (focus scrolled to the button) */
        for (int v = 0; v < g_nv; v++) lv_obj_scroll_to_y(g_v[v].list, 0, LV_ANIM_OFF);
}

static void create_view(int i, lv_display_t *disp)
{
    int w = lv_display_get_horizontal_resolution(disp), h = lv_display_get_vertical_resolution(disp);
    g_v[i].disp = disp;
    lv_obj_t *scr = lv_display_get_screen_active(disp);
    style_box(scr, C_BG, 0);

    lv_obj_t *hdr = lv_obj_create(scr);
    style_box(hdr, C_PANEL, 0);
    lv_obj_set_size(hdr, w, 22);
    lv_obj_align(hdr, LV_ALIGN_TOP_LEFT, 0, 0);
    g_title = text(hdr, "DisplayMOD", &lv_font_montserrat_14, C_FG);
    lv_obj_align(g_title, LV_ALIGN_LEFT_MID, 8, 0);
    g_chip = lv_obj_create(hdr);
    style_box(g_chip, C_OK_BG, 7);
    lv_obj_set_size(g_chip, LV_SIZE_CONTENT, 14);
    lv_obj_set_style_pad_hor(g_chip, 6, 0);
    lv_obj_align(g_chip, LV_ALIGN_RIGHT_MID, -6, 0);
    g_chip_lbl = text(g_chip, "", &lv_font_montserrat_10, C_OK_FG);
    lv_obj_center(g_chip_lbl);

    g_list = lv_obj_create(scr);
    lv_obj_remove_style_all(g_list);
    lv_obj_set_size(g_list, w, h - 22 - 14);
    lv_obj_align(g_list, LV_ALIGN_TOP_LEFT, 0, 22);
    lv_obj_set_flex_flow(g_list, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_all(g_list, 4, 0);
    lv_obj_set_style_pad_row(g_list, 3, 0);
    lv_obj_set_scrollbar_mode(g_list, LV_SCROLLBAR_MODE_OFF);

    lv_obj_t *ftr = lv_obj_create(scr);
    style_box(ftr, C_PANEL, 0);
    lv_obj_set_size(ftr, w, 14);
    lv_obj_align(ftr, LV_ALIGN_BOTTOM_LEFT, 0, 0);
    g_hint = text(ftr, "", &lv_font_montserrat_10, C_DIM);
    lv_obj_align(g_hint, LV_ALIGN_LEFT_MID, 6, 0);

    g_v[i].list = g_list; g_v[i].chip = g_chip; g_v[i].chip_lbl = g_chip_lbl;
    g_v[i].title = g_title; g_v[i].hint = g_hint;
}

void ui_create(lv_group_t *group, lv_display_t *const *disps, int n)
{
    g_group = group;
    lv_group_set_wrap(group, true);
    if (n > MAX_VIEWS) n = MAX_VIEWS;
    g_nv = n;
    for (int i = n - 1; i >= 0; i--) create_view(i, disps[i]);
    use_view(0);
    build_page();
}

/* Page used when the helper can't be reached (sudo rule missing). */
void ui_create_error(lv_group_t *group, lv_display_t *const *disps, int n, const char *msg)
{
    (void)group;
    for (int i = 0; i < n; i++) {
        lv_obj_t *scr = lv_display_get_screen_active(disps[i]);
        style_box(scr, C_BG, 0);
        lv_obj_t *l = text(scr, msg, &lv_font_montserrat_12, C_FG);
        lv_obj_set_width(l, lv_display_get_horizontal_resolution(disps[i]) - 20);
        lv_label_set_long_mode(l, LV_LABEL_LONG_WRAP);
        lv_obj_align(l, LV_ALIGN_CENTER, 0, 0);
    }
}
