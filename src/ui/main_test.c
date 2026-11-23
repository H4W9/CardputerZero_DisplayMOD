/* Host test harness: renders the UI headless and saves screenshots.
 * Usage: displaymod-ui-test OUTDIR "D D R snap:name E snap:other ..."
 * Keys: U D L R E (enter) X (esc) A (apply).  snap:NAME writes OUTDIR/NAME.ppm */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "lvgl.h"
#include "backend.h"
#include "ui.h"

#define W 320
#define H 170

static uint32_t g_ms;
static uint16_t g_fb[W * H];
static uint32_t g_q[64];
static int g_qn, g_qi;
static int g_exit;

void ui_request_exit(void) { g_exit = 1; }
static uint32_t tick(void) { return g_ms; }

static void flush(lv_display_t *d, const lv_area_t *a, uint8_t *px)
{
    const uint16_t *s = (const uint16_t *)px;
    for (int y = a->y1; y <= a->y2; y++)
        for (int x = a->x1; x <= a->x2; x++) g_fb[y * W + x] = *s++;
    lv_display_flush_ready(d);
}

static void kp_read(lv_indev_t *i, lv_indev_data_t *data)
{
    (void)i;
    static int pressed;
    static uint32_t key;
    if (pressed) { data->key = key; data->state = LV_INDEV_STATE_RELEASED; pressed = 0; return; }
    if (g_qi < g_qn) {
        key = g_q[g_qi++];
        data->key = key;
        data->state = LV_INDEV_STATE_PRESSED;
        pressed = 1;
        return;
    }
    data->key = key;
    data->state = LV_INDEV_STATE_RELEASED;
}

static void run(int ms)
{
    for (int t = 0; t < ms; t += 10) { g_ms += 10; lv_timer_handler(); }
}

static void snap(const char *dir, const char *name)
{
    char p[256];
    snprintf(p, sizeof p, "%s/%s.ppm", dir, name);
    FILE *f = fopen(p, "wb");
    fprintf(f, "P6\n%d %d\n255\n", W, H);
    for (int i = 0; i < W * H; i++) {
        uint16_t c = g_fb[i];
        unsigned char rgb[3] = {(c >> 11) << 3, ((c >> 5) & 63) << 2, (c & 31) << 3};
        fwrite(rgb, 1, 3, f);
    }
    fclose(f);
}

int main(int argc, char **argv)
{
    if (argc < 3) return 2;
    lv_init();
    lv_tick_set_cb(tick);
    lv_display_t *d = lv_display_create(W, H);
    static uint16_t buf[W * 40];
    lv_display_set_buffers(d, buf, NULL, sizeof buf, LV_DISPLAY_RENDER_MODE_PARTIAL);
    lv_display_set_flush_cb(d, flush);

    lv_indev_t *kp = lv_indev_create();
    lv_indev_set_type(kp, LV_INDEV_TYPE_KEYPAD);
    lv_indev_set_read_cb(kp, kp_read);
    lv_group_t *g = lv_group_create();
    lv_indev_set_group(kp, g);

    dm_load();
    ui_create(g);
    run(300);

    char *script = strdup(argv[2]);
    for (char *tok = strtok(script, " "); tok; tok = strtok(NULL, " ")) {
        if (!strncmp(tok, "snap:", 5)) { run(400); snap(argv[1], tok + 5); continue; }
        uint32_t k = 0;
        switch (tok[0]) {
        case 'U': k = LV_KEY_UP; break;
        case 'D': k = LV_KEY_DOWN; break;
        case 'L': k = LV_KEY_LEFT; break;
        case 'R': k = LV_KEY_RIGHT; break;
        case 'E': k = LV_KEY_ENTER; break;
        case 'X': k = LV_KEY_ESC; break;
        case 'A': k = 'a'; break;
        case 'W': run(1500); continue;          /* wait for a busy job */
        }
        if (k) { g_q[g_qn++] = k; run(200); }
    }
    printf("done, exit=%d\n", g_exit);
    return 0;
}
