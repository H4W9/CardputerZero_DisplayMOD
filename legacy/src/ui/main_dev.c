/* DisplayMOD settings app - device entry point (fbdev + Cardputer keyboard). */
#define _GNU_SOURCE
#include <dirent.h>
#include <fcntl.h>
#include <linux/input.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#include "lvgl.h"
#include "src/drivers/display/fb/lv_linux_fbdev.h"
#include "backend.h"
#include "ui.h"

static volatile int g_exit;
static int g_error;
static int g_kfd = -1;

void ui_request_exit(void) { g_exit = 1; }

static uint32_t tick_ms(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint32_t)(ts.tv_sec * 1000u + ts.tv_nsec / 1000000u);
}

/* ------------------------------------------------------------ display */
static void builtin_fb(char *out, size_t n)
{
    snprintf(out, n, "/dev/fb0");
    if (access("/dev/fb_lcd", F_OK) == 0) { snprintf(out, n, "/dev/fb_lcd"); return; }
    FILE *f = fopen("/proc/fb", "r");
    if (!f) return;
    char line[128];
    int num;
    while (fgets(line, sizeof line, f))
        if (strcasestr(line, "st7789") && sscanf(line, "%d", &num) == 1) { snprintf(out, n, "/dev/fb%d", num); break; }
    fclose(f);
}

/* ------------------------------------------------------------ EXT display */
static int ext_fb(char *out, size_t n)     /* the ILI9341 framebuffer, if it's up */
{
    FILE *f = fopen("/proc/fb", "r");
    if (!f) return -1;
    char line[128];
    int num = -1;
    while (fgets(line, sizeof line, f))
        if (strcasestr(line, "ili9341") && sscanf(line, "%d", &num) == 1) break; else num = -1;
    fclose(f);
    if (num < 0) return -1;
    snprintf(out, n, "/dev/fb%d", num);
    return access(out, R_OK | W_OK);
}

/* Tell the mirror (same user) to pause while we draw there. */
static char g_ui_flag[64], g_ui_ack[64];

static int ext_claim(void)
{
    snprintf(g_ui_flag, sizeof g_ui_flag, "/run/user/%u/displaymod-ui", (unsigned)getuid());
    snprintf(g_ui_ack, sizeof g_ui_ack, "/run/user/%u/displaymod-ui-ack", (unsigned)getuid());
    FILE *f = fopen(g_ui_flag, "w");
    if (!f) return -1;
    fprintf(f, "%d\n", (int)getpid());
    fclose(f);
    for (int i = 0; i < 15 && access(g_ui_ack, F_OK) != 0; i++) usleep(100000);   /* mirror paused */
    return 0;
}

static void ext_release(void)
{
    if (g_ui_flag[0]) unlink(g_ui_flag);
}

/* ------------------------------------------------------------ keyboard */
#define NB(x) (((x) + 8 * (int)sizeof(long) - 1) / (8 * (int)sizeof(long)))
#define TB(b, a) ((a)[(b) / (8 * sizeof(long))] & (1UL << ((b) % (8 * sizeof(long)))))

static int open_keyboard(void)
{
    const char *env = getenv("INPUT_DEV");
    if (env && *env) return open(env, O_RDONLY | O_NONBLOCK);
    const char *stable = "/dev/input/by-path/platform-3f804000.i2c-event";
    int fd = open(stable, O_RDONLY | O_NONBLOCK);
    if (fd >= 0) return fd;

    DIR *d = opendir("/dev/input");
    if (!d) return -1;
    struct dirent *e;
    while ((e = readdir(d))) {
        if (strncmp(e->d_name, "event", 5)) continue;
        char p[300];
        snprintf(p, sizeof p, "/dev/input/%s", e->d_name);
        fd = open(p, O_RDONLY | O_NONBLOCK);
        if (fd < 0) continue;
        unsigned long ev[NB(EV_MAX)] = {0}, keys[NB(KEY_MAX)] = {0};
        if (ioctl(fd, EVIOCGBIT(0, sizeof ev), ev) >= 0 && TB(EV_KEY, ev) && !TB(EV_REL, ev) &&
            ioctl(fd, EVIOCGBIT(EV_KEY, sizeof keys), keys) >= 0 && TB(KEY_ENTER, keys)) {
            closedir(d);
            return fd;
        }
        close(fd);
    }
    closedir(d);
    return -1;
}

static uint32_t map_key(int code)
{
    switch (code) {
    /* Same navigation keys as the launcher (F/X/Z/C). The Cardputer keyboard
     * reuses codes like KEY_SEMICOLON for other symbols, so those aren't used. */
    case KEY_UP: case KEY_F: return LV_KEY_UP;
    case KEY_DOWN: case KEY_X: return LV_KEY_DOWN;
    case KEY_LEFT: case KEY_Z: return LV_KEY_LEFT;
    case KEY_RIGHT: case KEY_C: return LV_KEY_RIGHT;
    case KEY_ENTER: case KEY_KPENTER: case KEY_SPACE: return LV_KEY_ENTER;
    case KEY_ESC: case KEY_BACKSPACE: return LV_KEY_ESC;
    case KEY_TAB: return LV_KEY_NEXT;
    case KEY_A: return 'a';            /* apply pending changes */
    case KEY_Q: return LV_KEY_ESC;
    default: return 0;
    }
}

static void kbd_read(lv_indev_t *indev, lv_indev_data_t *data)
{
    (void)indev;
    static uint32_t last_key;
    struct input_event ev;
    data->key = last_key;
    data->state = LV_INDEV_STATE_RELEASED;
    while (g_kfd >= 0 && read(g_kfd, &ev, sizeof ev) == (ssize_t)sizeof ev) {
        if (ev.type != EV_KEY || ev.value == 2) continue;   /* ignore autorepeat */
        uint32_t k = map_key(ev.code);
        if (!k) continue;
        if (g_error && k == LV_KEY_ESC) g_exit = 1;
        last_key = k;
        data->key = k;
        data->state = ev.value ? LV_INDEV_STATE_PRESSED : LV_INDEV_STATE_RELEASED;
        data->continue_reading = true;   /* deliver every press/release */
        return;
    }
}

int main(void)
{
    lv_init();
    lv_tick_set_cb(tick_ms);

    char fbpath[32], extpath[32];
    builtin_fb(fbpath, sizeof fbpath);
    lv_display_t *disps[2];
    int nd = 0;
    lv_display_t *disp = lv_linux_fbdev_create();
    if (!disp) { fprintf(stderr, "display init failed\n"); return 1; }
    lv_linux_fbdev_set_file(disp, fbpath);
    disps[nd++] = disp;

    /* The app also fills the EXT display while it's open (the mirror pauses) */
    int ext = 0;
    if (ext_fb(extpath, sizeof extpath) == 0 && ext_claim() == 0) {
        lv_display_t *d2 = lv_linux_fbdev_create();
        if (d2) {
            lv_linux_fbdev_set_file(d2, extpath);
            disps[nd++] = d2;
            ext = 1;
        } else {
            ext_release();
        }
    }
    lv_display_set_default(disp);

    g_kfd = open_keyboard();
    if (g_kfd < 0) fprintf(stderr, "displaymod-ui: no keyboard found\n");
    /* Take the keyboard for this app only, so key presses don't also reach
     * the Linux console and get drawn on the built-in screen. Released
     * automatically when the app exits. */
    else if (ioctl(g_kfd, EVIOCGRAB, 1) != 0) perror("displaymod-ui: EVIOCGRAB");
    lv_indev_t *kp = lv_indev_create();
    lv_indev_set_type(kp, LV_INDEV_TYPE_KEYPAD);
    lv_indev_set_read_cb(kp, kbd_read);

    lv_group_t *group = lv_group_create();
    lv_indev_set_group(kp, group);

    if (dm_load() != 0) {
        g_error = 1;
        ui_create_error(group, disps, nd, "DisplayMOD can't reach its helper.\n"
                               "Reinstall the package, then try again.\n\nPress Esc to exit.");
    } else
        ui_create(group, disps, nd);

    while (!g_exit) {
        uint32_t d = lv_timer_handler();
        if (d > 30) d = 30;
        usleep(d * 1000);
    }
    if (ext) ext_release();
    return 0;
}
