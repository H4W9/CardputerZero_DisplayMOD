/*
 * displaymod-mirror - DisplayMOD mirror mode for the CardputerZero.
 *
 * Copies the built-in screen (ST7789, 320x170) onto the ILI9341 EXT display
 * and uses the spare rows for a status bar (clock, battery, IP, RAM, CPU).
 * When BigTermCLI is open it hands the EXT display to the full-size
 * terminal and takes it back when BigTermCLI closes.
 *
 * Power/RAM: only rows that changed are written (fbtft deferred I/O then
 * sends only those pages over SPI). Polls fast only while the picture is
 * changing. Blanks the EXT display when the built-in backlight is off.
 *
 * Runs as the launcher user on the EXT display's tty (systemd TTYPath).
 * Settings: /etc/displaymod/displaymod.conf (MIRROR_LAYOUT, BIGTERM, BRIGHTNESS).
 * Store edition: started by displaymod-daemon on the EXT console (as the
 * service user). BigTermCLI starts its own viewer on that console; while
 * /run/displaymod/bigterm exists this only switches it to text mode.
 * Built-in screen: optionally kept off while mirroring (BUILTIN_OFF); the big
 * screen then sleeps after SLEEP minutes without a key press. Not applied while
 * BigTermCLI is open; it follows DIM_BUILTIN instead.
 * Brightness: software PWM on the LED pin (/run/displaymod/pwm), either
 * matching the built-in backlight or a fixed 10-100%. Never 0 while on.
 *
 * Test hooks (host builds): DM_SRC, DM_DST, DM_SRC_GEOM=WxHxBPP,
 * DM_DST_GEOM=WxHxBPP, DM_CONF, DM_FLAG, DM_ONCE=1, DM_FAKE_TIME="HH:MM".
 */
#define _GNU_SOURCE
#include <arpa/inet.h>
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <ifaddrs.h>
#include <linux/fb.h>
#include <linux/input.h>
#include <linux/kd.h>
#include <net/if.h>
#include <netinet/in.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#include "../common/dmdraw.h"

#define RUN_DIR "/run/displaymod"
#define CONF_PATH "/etc/displaymod/displaymod.conf"
#define BL_PATH "/sys/class/backlight/backlight/brightness"

typedef struct {
    int fd;
    uint8_t *mem;
    int w, h, bpp, stride;
    size_t len;
} fb_t;

enum { LAYOUT_BAR, LAYOUT_STRETCH, LAYOUT_PLAIN };

static volatile sig_atomic_t g_stop = 0;
static int g_layout = LAYOUT_BAR;
static int g_auto_switch = 1;
static int g_bright = -1;          /* -1 = match built-in, else 10..100 % */
static uint16_t g_border;          /* RGB565 fill around the 1:1 copy */
static int g_builtin_off = 0;      /* keep the built-in backlight off */
static int g_sleep_min = 5;        /* idle minutes before the EXT display sleeps (0 = never) */
static int g_clock12 = 0;          /* CLOCK=12: 12-hour time in the status bar */
static int g_align = 1;            /* 1:1 copy position: 0 top, 1 center, 2 bottom */

static void on_signal(int s) { (void)s; g_stop = 1; }

static uint64_t now_ms(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000u + ts.tv_nsec / 1000000u;
}


/* ---------------------------------------------------------------- config */

static void load_conf(void)
{
    const char *path = getenv("DM_CONF");
    FILE *f = fopen(path ? path : CONF_PATH, "r");
    if (!f) return;
    char line[128];
    while (fgets(line, sizeof line, f)) {
        if (!strncmp(line, "MIRROR_LAYOUT=", 14)) {
            const char *v = line + 14;
            if (!strncmp(v, "stretch", 7)) g_layout = LAYOUT_STRETCH;
            else if (!strncmp(v, "plain", 5)) g_layout = LAYOUT_PLAIN;
            else g_layout = LAYOUT_BAR;
        } else if (!strncmp(line, "BIGTERM=", 8)) {
            g_auto_switch = line[8] == '1';
        } else if (!strncmp(line, "BUILTIN_OFF=", 12)) {
            g_builtin_off = line[12] == '1';
        } else if (!strncmp(line, "SLEEP=", 6)) {
            g_sleep_min = atoi(line + 6);
        } else if (!strncmp(line, "CLOCK=", 6)) {
            g_clock12 = atoi(line + 6) == 12;
        } else if (!strncmp(line, "ALIGN=", 6)) {
            g_align = !strncmp(line + 6, "top", 3) ? 0 : !strncmp(line + 6, "bottom", 6) ? 2 : 1;
        } else if (!strncmp(line, "BORDER_COLOR=", 13)) {
            static const struct { const char *n; unsigned rgb; } C[] = {
            {"black", 0x000000},
            {"charcoal", 0x0D1117},
            {"slate", 0x30363D},
            {"navy", 0x0B1F3A},
            {"teal", 0x0E3B3A},
            {"green", 0x0F3320},
            {"maroon", 0x3A1010},
            {"purple", 0x2A1740},
            {"white", 0xFFFFFF}
            };
            g_border = 0;
            for (unsigned i = 0; i < sizeof C / sizeof C[0]; i++)
                if (!strncmp(line + 13, C[i].n, strlen(C[i].n)))
                    g_border = (uint16_t)(((C[i].rgb >> 8) & 0xF800) | ((C[i].rgb >> 5) & 0x07E0) | ((C[i].rgb >> 3) & 0x001F));
        } else if (!strncmp(line, "BRIGHTNESS=", 11)) {
            int v = atoi(line + 11);
            g_bright = (v >= 10 && v <= 100) ? v : -1;
        }
    }
    fclose(f);
}

/* ----------------------------------------------------------- framebuffer */

static int find_fb(const char *needle, char *out, size_t n)
{
    FILE *f = fopen("/proc/fb", "r");
    if (!f) return -1;
    char line[128];
    int num = -1;
    while (fgets(line, sizeof line, f)) {
        if (strcasestr(line, needle) && sscanf(line, "%d", &num) == 1) break;
        num = -1;
    }
    fclose(f);
    if (num < 0) return -1;
    snprintf(out, n, "/dev/fb%d", num);
    return 0;
}

static int open_fb(fb_t *fb, const char *path, int rw, const char *geom)
{
    memset(fb, 0, sizeof *fb);
    fb->fd = open(path, rw ? O_RDWR | O_CREAT : O_RDONLY, 0644);
    if (fb->fd < 0) { fprintf(stderr, "open %s: %s\n", path, strerror(errno)); return -1; }

    struct fb_var_screeninfo v;
    struct fb_fix_screeninfo x;
    if (ioctl(fb->fd, FBIOGET_VSCREENINFO, &v) == 0 && ioctl(fb->fd, FBIOGET_FSCREENINFO, &x) == 0) {
        fb->w = v.xres; fb->h = v.yres; fb->bpp = v.bits_per_pixel; fb->stride = x.line_length;
        fb->len = x.smem_len;
    } else if (geom && sscanf(geom, "%dx%dx%d", &fb->w, &fb->h, &fb->bpp) == 3) {
        fb->stride = fb->w * fb->bpp / 8;
        fb->len = (size_t)fb->stride * fb->h;
        if (rw && ftruncate(fb->fd, fb->len) != 0) return -1;
    } else {
        fprintf(stderr, "%s: no geometry\n", path);
        return -1;
    }
    if (fb->bpp != 16 && fb->bpp != 32) { fprintf(stderr, "%s: %d bpp unsupported\n", path, fb->bpp); return -1; }
    fb->mem = mmap(NULL, fb->len, rw ? PROT_READ | PROT_WRITE : PROT_READ, MAP_SHARED, fb->fd, 0);
    if (fb->mem == MAP_FAILED) { fprintf(stderr, "mmap %s: %s\n", path, strerror(errno)); return -1; }
    return 0;
}

/* Read one source row as RGB565. */
static void read_row565(const fb_t *fb, int y, uint16_t *out)
{
    const uint8_t *p = fb->mem + (size_t)y * fb->stride;
    if (fb->bpp == 16) { memcpy(out, p, fb->w * 2); return; }
    for (int x = 0; x < fb->w; x++, p += 4) out[x] = rgb(p[2], p[1], p[0]);
}

/* Write one destination row from RGB565. */
static void write_row565(fb_t *fb, int y, const uint16_t *in)
{
    uint8_t *p = fb->mem + (size_t)y * fb->stride;
    if (fb->bpp == 16) { memcpy(p, in, fb->w * 2); return; }
    for (int x = 0; x < fb->w; x++, p += 4) {
        uint16_t c = in[x];
        p[0] = (c & 0x1F) << 3; p[1] = ((c >> 5) & 0x3F) << 2; p[2] = (c >> 11) << 3; p[3] = 0xFF;
    }
}

/* ------------------------------------------------------------ status bar */

static void render_bar(canvas_t *c, int info_tick)
{
    static char bat[32], ip[48], ram[32], cpu[32];
    if (info_tick || !bat[0]) {
        battery_text(bat, sizeof bat); ip_text(ip, sizeof ip);
        ram_text(ram, sizeof ram); cpu_text(cpu, sizeof cpu);
    }
    const uint16_t BG = rgb(16, 20, 24), LINE = rgb(42, 52, 64);
    const uint16_t FG = rgb(225, 230, 235), DIMC = rgb(130, 140, 150), ACC = rgb(56, 200, 190);
    for (int i = 0; i < c->w * c->h; i++) c->px[i] = BG;
    for (int x = 0; x < c->w; x++) c->px[x] = LINE;

    char hm[12], ampm[4], date[24];
    time_t t = time(NULL);
    struct tm tm;
    localtime_r(&t, &tm);
    clock_text(hm, sizeof hm, ampm, sizeof ampm, g_clock12);
    strftime(date, sizeof date, "%a %d %b", &tm);

    const char *lab[4] = {"BAT", "IP", "RAM", "CPU"};
    const char *val[4] = {bat, ip, ram, cpu};

    if (c->w >= 300) {               /* side by side: clock left, info right */
        int y0 = (c->h - 64) / 2;
        if (y0 < 2) y0 = 2;
        draw_clock(c, 8, y0 - 2, hm, ampm, FG);
        draw_small(c, 10, y0 + 46, date, DIMC);
        int x0 = c->w - 8 - 18 * DM_SMALL_W;
        for (int i = 0; i < 4; i++) {
            int y = y0 + i * DM_SMALL_H;
            draw_small(c, x0, y, lab[i], i == 0 ? ACC : DIMC);
            draw_small(c, x0 + 5 * DM_SMALL_W, y, val[i], FG);
        }
    } else {                         /* narrow (portrait): stacked */
        int y = 6;
        draw_clock(c, 8, y, hm, ampm, FG);
        draw_small(c, 10, y + 44, date, DIMC);
        for (int i = 0; i < 4; i++) {
            int yy = y + 66 + i * DM_SMALL_H;
            draw_small(c, 8, yy, lab[i], i == 0 ? ACC : DIMC);
            draw_small(c, 8 + 5 * DM_SMALL_W, yy, val[i], FG);
        }
    }
}

/* ---------------------------------------------------------------- mirror */

typedef struct {
    fb_t src, dst;
    int content_h, bar_y, bar_h, off_y;   /* off_y: top of the copy (plain layout) */
    int *xmap, *ymap;            /* first source pixel/row for each output one */
    unsigned char *xw, *yw;      /* weight of the next one, 0..32 */
    uint16_t *prev, *cur, *rowbuf, *rowbuf2, *bar, *bar_prev;
    int force;
} mirror_t;

/* Output pixel i samples the source at its centre: between source s and s+1
 * with weight w/32 on s+1. Same size maps 1:1 with no blending. */
static void scale_map(int *map, unsigned char *w, int sn, int dn)
{
    for (int i = 0; i < dn; i++) {
        if (sn == dn) { map[i] = i; w[i] = 0; continue; }
        long p = ((2L * i + 1) * sn * 32) / (2L * dn) - 16;    /* source position in 1/32 */
        if (p < 0) p = 0;
        int s = (int)(p >> 5), f = (int)(p & 31);
        if (s >= sn - 1) { s = sn - 1; f = 0; }
        map[i] = s;
        w[i] = (unsigned char)f;
    }
}

/* Blend two RGB565 pixels, w/32 of b. */
static inline uint16_t mix565(uint16_t a, uint16_t b, unsigned w)
{
    if (!w) return a;
    uint32_t x = (a | (uint32_t)a << 16) & 0x07E0F81Fu;
    uint32_t y = (b | (uint32_t)b << 16) & 0x07E0F81Fu;
    uint32_t r = ((x * (32 - w) + y * w) >> 5) & 0x07E0F81Fu;
    return (uint16_t)(r | r >> 16);
}

static int mirror_init(mirror_t *m)
{
    int sw = m->src.w, sh = m->src.h, dw = m->dst.w, dh = m->dst.h;
    if (g_layout == LAYOUT_STRETCH) m->content_h = dh;
    else {
        m->content_h = (sh * dw + sw / 2) / sw;
        if (m->content_h > dh) m->content_h = dh;
    }
    m->bar_y = m->content_h;
    m->bar_h = dh - m->content_h;
    if (g_layout != LAYOUT_BAR || m->bar_h < 48) m->bar_h = 0;
    if (g_layout == LAYOUT_PLAIN) m->off_y = (dh - m->content_h) * g_align / 2;

    m->xmap = malloc(sizeof(int) * dw);
    m->ymap = malloc(sizeof(int) * m->content_h);
    m->xw = malloc(dw);
    m->yw = malloc(m->content_h);
    m->prev = calloc((size_t)sw * sh, 2);
    m->cur = calloc((size_t)sw * sh, 2);
    m->rowbuf = calloc(dw > sw ? dw : sw, 2);
    m->rowbuf2 = calloc(dw > sw ? dw : sw, 2);
    if (!m->xmap || !m->ymap || !m->xw || !m->yw || !m->prev || !m->cur || !m->rowbuf || !m->rowbuf2)
        return -1;
    scale_map(m->xmap, m->xw, sw, dw);
    scale_map(m->ymap, m->yw, sh, m->content_h);
    if (m->bar_h > 0) {
        m->bar = calloc((size_t)dw * m->bar_h, 2);
        m->bar_prev = calloc((size_t)dw * m->bar_h, 2);
        if (!m->bar || !m->bar_prev) return -1;
    }
    m->force = 1;
    return 0;
}

static void clear_dst(mirror_t *m)
{
    memset(m->rowbuf, 0, m->dst.w * 2);
    for (int y = 0; y < m->dst.h; y++) write_row565(&m->dst, y, m->rowbuf);
}

/* Copy changed source rows. Returns 1 if anything changed. */
static int mirror_step(mirror_t *m)
{
    int sw = m->src.w, sh = m->src.h, dw = m->dst.w;
    static unsigned char *changed;
    static int changed_n;
    if (changed_n != sh) { free(changed); changed = calloc(sh, 1); changed_n = sh; }

    int any = 0;
    for (int y = 0; y < sh; y++) {
        uint16_t *row = m->cur + (size_t)y * sw;
        read_row565(&m->src, y, row);
        changed[y] = m->force || memcmp(row, m->prev + (size_t)y * sw, sw * 2) != 0;
        any |= changed[y];
    }
    if (!any) return 0;

    int identity_x = (sw == dw);
    for (int y = 0; y < m->content_h; y++) {
        int sy = m->ymap[y], wy = m->yw[y];
        int sy2 = wy ? sy + 1 : sy;
        if (!changed[sy] && !changed[sy2]) continue;
        const uint16_t *srow = m->cur + (size_t)sy * sw;
        if (wy) {                                   /* between two source rows */
            const uint16_t *s2 = m->cur + (size_t)sy2 * sw;
            for (int x = 0; x < sw; x++) m->rowbuf2[x] = mix565(srow[x], s2[x], wy);
            srow = m->rowbuf2;
        }
        if (identity_x) write_row565(&m->dst, m->off_y + y, srow);
        else {
            for (int x = 0; x < dw; x++) {
                int sx = m->xmap[x];
                m->rowbuf[x] = m->xw[x] ? mix565(srow[sx], srow[sx + 1], m->xw[x]) : srow[sx];
            }
            write_row565(&m->dst, m->off_y + y, m->rowbuf);
        }
    }
    for (int y = 0; y < sh; y++)
        if (changed[y]) memcpy(m->prev + (size_t)y * sw, m->cur + (size_t)y * sw, sw * 2);
    return 1;
}

static void bar_step(mirror_t *m, int info_tick)
{
    if (m->bar_h <= 0) {
        if (m->force && g_layout == LAYOUT_PLAIN) {
            for (int x = 0; x < m->dst.w; x++) m->rowbuf[x] = g_border;
            for (int y = 0; y < m->dst.h; y++)          /* border above and below */
                if (y < m->off_y || y >= m->off_y + m->content_h) write_row565(&m->dst, y, m->rowbuf);
        }
        return;
    }
    canvas_t c = { m->bar, m->dst.w, m->bar_h };
    render_bar(&c, info_tick);
    for (int y = 0; y < m->bar_h; y++) {
        uint16_t *r = m->bar + (size_t)y * m->dst.w, *p = m->bar_prev + (size_t)y * m->dst.w;
        if (m->force || memcmp(r, p, m->dst.w * 2)) {
            write_row565(&m->dst, m->bar_y + y, r);
            memcpy(p, r, m->dst.w * 2);
        }
    }
}

/* -------------------------------------------------------------- BigTermCLI */

/* BigTermCLI's viewer draws on this console in text mode; tell the daemon
 * once the switch is done. */
static void start_term(void)
{
    ioctl(0, KDSETMODE, KD_TEXT);
    close(open(RUN_DIR "/bigterm-ack", O_CREAT | O_WRONLY, 0644));
}

static void stop_term(void)
{
    unlink(RUN_DIR "/bigterm-ack");
    ioctl(0, KDSETMODE, KD_GRAPHICS);
}

static int builtin_backlight_on(void)
{
    long v;
    if (read_int(BL_PATH, &v) != 0) return 1;   /* unknown: assume on */
    return v > 0;
}

static void builtin_write(long v)
{
    FILE *f = fopen(BL_PATH, "w");
    if (f) { fprintf(f, "%ld", v); fclose(f); }
}

/* The DisplayMOD app draws on the EXT display itself while it is open.
 * The flag file holds its pid; a stale flag (app gone) is ignored. */
static int ui_active(const char *path)
{
    FILE *f = fopen(path, "r");
    int pid = 0;
    if (!f) return 0;
    if (fscanf(f, "%d", &pid) != 1) pid = 0;
    fclose(f);
    return pid > 0 && (kill(pid, 0) == 0 || errno == EPERM);
}

/* Keyboard, read without grabbing, only to notice activity. */
static int open_keyboard_ro(void)
{
    int fd = open("/dev/input/by-path/platform-3f804000.i2c-event", O_RDONLY | O_NONBLOCK);
    if (fd >= 0) return fd;
    DIR *d = opendir("/dev/input");
    if (!d) return -1;
    struct dirent *e;
    while ((e = readdir(d))) {
        if (strncmp(e->d_name, "event", 5)) continue;
        char p[300];
        snprintf(p, sizeof p, "/dev/input/%s", e->d_name);
        unsigned long ev[(EV_MAX + 63) / 64] = {0};
        fd = open(p, O_RDONLY | O_NONBLOCK);
        if (fd < 0) continue;
        if (ioctl(fd, EVIOCGBIT(0, sizeof ev), ev) >= 0 && (ev[0] & (1UL << EV_KEY)) && !(ev[0] & (1UL << EV_REL))) {
            closedir(d);
            return fd;
        }
        close(fd);
    }
    closedir(d);
    return -1;
}

/* Built-in backlight as a percentage (100 if unknown). */
static int builtin_percent(void)
{
    long v, mx;
    if (read_int(BL_PATH, &v) != 0 || read_int("/sys/class/backlight/backlight/max_brightness", &mx) != 0 || mx <= 0)
        return 100;
    return (int)(v * 100 / mx);
}

/* EXT display backlight via PWM, 0..100 %. Writes only on change. */
static void set_backlight(int pct)
{
    static char dir[128];
    static long period;
    static int last = -2;
    if (!dir[0]) {
        FILE *f = fopen("/run/displaymod/pwm", "r");
        if (!f) return;                               /* driver handles on/off */
        if (!fgets(dir, sizeof dir, f)) dir[0] = 0;
        fclose(f);
        dir[strcspn(dir, "\n")] = 0;
        char p[160];
        snprintf(p, sizeof p, "%s/period", dir);
        if (read_int(p, &period) != 0 || period <= 0) { dir[0] = 0; return; }
    }
    if (pct == last) return;
    last = pct;
    char p[160];
    snprintf(p, sizeof p, "%s/duty_cycle", dir);
    FILE *f = fopen(p, "w");
    if (f) { fprintf(f, "%ld", period * pct / 100); fclose(f); }
}

/* Re-read the settings file when it changes (brightness and border color
 * apply live). Returns 1 if it was reloaded. */
static int reload_conf_if_changed(void)
{
    static time_t mtime;
    struct stat st;
    const char *path = getenv("DM_CONF");
    if (stat(path ? path : CONF_PATH, &st) == 0 && st.st_mtime != mtime) {
        mtime = st.st_mtime;
        load_conf();
        return 1;
    }
    return 0;
}

int main(void)
{
    char src_path[32] = "/dev/fb0", dst_path[32] = "";
    const char *e;

    load_conf();
    if ((e = getenv("DM_SRC"))) snprintf(src_path, sizeof src_path, "%s", e);
    else find_fb("st7789", src_path, sizeof src_path);
    if ((e = getenv("DM_DST"))) snprintf(dst_path, sizeof dst_path, "%s", e);
    else if (find_fb("ili9341", dst_path, sizeof dst_path) != 0) {
        fprintf(stderr, "displaymod-mirror: EXT display not found\n");
        return 1;
    }

    mirror_t m;
    memset(&m, 0, sizeof m);
    if (open_fb(&m.src, src_path, 0, getenv("DM_SRC_GEOM")) || open_fb(&m.dst, dst_path, 1, getenv("DM_DST_GEOM")))
        return 1;
    if (mirror_init(&m)) { fprintf(stderr, "out of memory\n"); return 1; }

    char flag[64];
    if ((e = getenv("DM_FLAG"))) snprintf(flag, sizeof flag, "%s", e);
    else snprintf(flag, sizeof flag, "%s", RUN_DIR "/bigterm");
    const char *ui_flag = RUN_DIR "/ui", *ui_ack = RUN_DIR "/ui-ack";
    int paused = 0;

    signal(SIGTERM, on_signal);
    signal(SIGINT, on_signal);
    signal(SIGHUP, on_signal);

    if (getenv("DM_ONCE")) {
        mirror_step(&m);
        bar_step(&m, 1);
        return 0;
    }

    ioctl(0, KDSETMODE, KD_GRAPHICS);
    clear_dst(&m);
    fprintf(stderr, "displaymod-mirror: %s %dx%d -> %s %dx%d, bar %d rows\n",
            src_path, m.src.w, m.src.h, dst_path, m.dst.w, m.dst.h, m.bar_h);

    int term = 0;
    int blanked = 0, last_pct = 100;
    uint64_t blanked_at = 0;
    uint64_t last_change = now_ms(), last_sec = 0, last_info = 0, last_flag = 0;

    /* "Built-in off while mirroring": remember the level we turned off so it
     * can be restored, and share it with BigTermCLI's status page. */
    int kfd = open_keyboard_ro();
    uint64_t last_kopen = now_ms();
    if (kfd < 0) fprintf(stderr, "displaymod-mirror: keyboard not found yet, will retry\n");
    long seen_level = 0;               /* last built-in level seen while it was on */
    long saved_level = 0, bl_max = 0;
    int forced_off = 0;
    uint64_t last_input = now_ms();
    char level_file[64];
    snprintf(level_file, sizeof level_file, "%s", RUN_DIR "/builtin-level");
    if (read_int("/sys/class/backlight/backlight/max_brightness", &bl_max) != 0 || bl_max <= 0) bl_max = 100;

    while (!g_stop) {
        uint64_t t = now_ms();

        /* BigTermCLI hand-off, checked ~3 times a second */
        if (t - last_flag >= 300) {
            last_flag = t;
            int want_ui = ui_active(ui_flag);
            if (want_ui && !paused) {                  /* DisplayMOD app took the EXT display */
                if (blanked) { ioctl(m.dst.fd, FBIOBLANK, FB_BLANK_UNBLANK); blanked = 0; }
                paused = 1;
                close(open(ui_ack, O_CREAT | O_WRONLY, 0644));
            } else if (paused && !want_ui) {           /* app closed: repaint everything */
                paused = 0;
                unlink(ui_ack);
                m.force = 1;
                last_input = t;
            }
            int want_term = g_auto_switch && access(flag, F_OK) == 0;
            if (want_term && !term) {
                if (blanked) { ioctl(m.dst.fd, FBIOBLANK, FB_BLANK_UNBLANK); blanked = 0; }
                start_term();
                term = 1;
            } else if (term && !want_term) {
                stop_term();
                term = 0;
                m.force = 1;
                last_input = t;   /* don't fall asleep right after */
            }
        }
        if (term || paused) {
            /* BigTermCLI may darken the built-in screen: keep the last level */
            if (t - last_sec >= 1000) {
                last_sec = t;
                reload_conf_if_changed();
                if (paused && !term) {               /* DisplayMOD app open: "Built-in off" still applies live */
                    long b;
                    if (g_builtin_off) {
                        if (read_int(BL_PATH, &b) == 0 && b > 0) {
                            saved_level = b;
                            FILE *lf = fopen(level_file, "w");
                            if (lf) { fprintf(lf, "%ld", b); fclose(lf); }
                            builtin_write(0);
                            forced_off = 1;
                        }
                    } else if (forced_off) {         /* switched off in the app: light it again now */
                        builtin_write(saved_level > 0 ? saved_level : bl_max);
                        unlink(level_file);
                        forced_off = 0;
                    }
                }
                set_backlight(g_bright > 0 ? g_bright : last_pct);
            }
            usleep(100000);
            continue;
        }

        /* Key presses (shared, not grabbed) keep the EXT display awake */
        if (kfd < 0 && t - last_kopen >= 5000) {        /* keyboard missing: try again */
            last_kopen = t;
            kfd = open_keyboard_ro();
            if (kfd >= 0) fprintf(stderr, "displaymod-mirror: keyboard found\n");
        }
        if (kfd >= 0) {
            struct input_event ev;
            ssize_t r;
            while ((r = read(kfd, &ev, sizeof ev)) == (ssize_t)sizeof ev)
                if (ev.type == EV_KEY) last_input = t;
            if (r < 0 && errno != EAGAIN && errno != EINTR) {   /* device went away */
                fprintf(stderr, "displaymod-mirror: keyboard lost, will retry\n");
                close(kfd);
                kfd = -1;
            }
        }

        /* Follow the built-in backlight (4x a second when keeping it off,
         * so the launcher waking it is undone quickly) */
        if (t - last_sec >= (g_builtin_off ? 250u : 1000u)) {
            last_sec = t;
            if (reload_conf_if_changed()) m.force = 1;   /* repaint border */
            int on, pct;
            if (g_builtin_off) {
                long b;
                /* BigTermCLI may be starting: it sets the built-in itself (DIM_BUILTIN) */
                if (access(flag, F_OK) != 0 && read_int(BL_PATH, &b) == 0 && b > 0) {
                    saved_level = b;
                    last_input = t;                  /* the launcher lit it: someone pressed a key */
                    FILE *lf = fopen(level_file, "w");
                    if (lf) { fprintf(lf, "%ld", b); fclose(lf); }
                    builtin_write(0);
                    forced_off = 1;
                }
                on = !(g_sleep_min > 0 && t - last_input > (uint64_t)g_sleep_min * 60000u);
                pct = saved_level > 0 ? (int)(saved_level * 100 / bl_max) : 100;
            } else {
                if (forced_off) {                        /* setting switched off: give it back */
                    builtin_write(saved_level > 0 ? saved_level : bl_max);
                    unlink(level_file);
                    forced_off = 0;
                }
                long cur;
                if (read_int(BL_PATH, &cur) == 0 && cur > 0) seen_level = cur;
                else if (blanked && last_input > blanked_at && seen_level > 0) {
                    builtin_write(seen_level);       /* key while both screens are dark: wake them */
                    fprintf(stderr, "displaymod-mirror: key pressed, waking the built-in screen\n");
                }
                on = builtin_backlight_on();
                pct = builtin_percent();
            }
            if (!on && !blanked) {
                ioctl(m.dst.fd, FBIOBLANK, FB_BLANK_POWERDOWN);
                blanked = 1;
                blanked_at = t;
                fprintf(stderr, "displaymod-mirror: EXT display asleep\n");
            } else if (on && blanked) {
                ioctl(m.dst.fd, FBIOBLANK, FB_BLANK_UNBLANK);
                blanked = 0;
                m.force = 1;
                fprintf(stderr, "displaymod-mirror: EXT display awake\n");
            }
            if (on) {
                if (pct < 10) pct = 10;               /* never fully dark while on */
                last_pct = pct;
                set_backlight(g_bright > 0 ? g_bright : pct);
            } else {
                set_backlight(0);
            }
            if (!blanked) {
                int info = (t - last_info >= 5000);
                if (info) last_info = t;
                bar_step(&m, info);
            }
        }
        if (blanked) { usleep(250000); continue; }

        if (mirror_step(&m)) last_change = t;
        if (m.force) { bar_step(&m, 0); m.force = 0; }

        /* 25 fps while things move, 5 fps after 2 s of no change */
        usleep(t - last_change < 2000 ? 40000 : 200000);
    }

    if (term) stop_term();
    if (forced_off) { builtin_write(saved_level > 0 ? saved_level : bl_max); unlink(level_file); }
    ioctl(0, KDSETMODE, KD_TEXT);
    return 0;
}
