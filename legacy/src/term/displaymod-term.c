/*
 * displaymod-term - BigTermCLI for the CardputerZero.
 *
 * Runs a shell (inside the shared "dm" tmux session) that is shown only on
 * the ILI9341 EXT display, in the same colors as the launcher's CLI app.
 * Keys are read straight from the Cardputer keyboard (grabbed, so nothing
 * leaks to the launcher or the Linux console) and sent to the session.
 * The built-in screen shows a clock/status page, or is dark if
 * "Built-in dark in BigTermCLI" is on in DisplayMOD.
 *
 * Keys match the launcher's CLI app: fn+F/X history, fn+Z/C cursor,
 * fn+K/N line start/end, shift+fn+L/M scrollback, fn+H help, hold Esc quit.
 * The CLI's viewport mode (F6) becomes "cycle text size" here, since the
 * EXT display already shows a full terminal.
 *
 * The EXT display itself is drawn by displaymod-mirror, which switches to a
 * read-only view of the session while /run/user/UID/displaymod-bigterm exists.
 */
#define _GNU_SOURCE
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <linux/fb.h>
#include <linux/input.h>
#include <poll.h>
#include <pty.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#include "../common/dmdraw.h"

#define CTL       "/usr/lib/displaymod/displaymod-ctl"
#define HELP_FILE "/usr/lib/displaymod/bigterm-help.txt"
#define KEYMAP    "/usr/share/keymaps/tca8418_keypad_m5stack_keymap.map"
#ifndef KEY_FN
#define KEY_FN 0x1d0
#endif

/* BigTermCLI fonts, same order as the DisplayMOD app and F6: Terminus (the
 * CardputerZero OS terminal font) and the console's VGA fonts; cols x rows
 * on the 320x240 display. */
typedef struct { const char *name; unsigned short cols, rows; } font_t;
static const font_t FONTS[] = {
    {"t12", 53, 20}, {"t14", 40, 17}, {"t16", 40, 15}, {"t18", 32, 13}, {"t20", 32, 12},
    {"v8", 40, 30}, {"v16", 40, 15},
};
#define NFONTS (int)(sizeof FONTS / sizeof FONTS[0])

static const font_t *font_find(const char *name)
{
    for (int i = 0; i < NFONTS; i++)
        if (!strcmp(FONTS[i].name, name)) return &FONTS[i];
    return &FONTS[0];
}

static volatile sig_atomic_t g_stop;
static void on_signal(int s) { (void)s; g_stop = 1; }

static uint64_t now_ms(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000u + ts.tv_nsec / 1000000u;
}

/* ------------------------------------------------------------- helpers */

static void conf_get(const char *key, char *out, size_t n, const char *def)
{
    snprintf(out, n, "%s", def);
    FILE *f = fopen("/etc/displaymod.conf", "r");
    if (!f) return;
    char line[128];
    size_t kl = strlen(key);
    while (fgets(line, sizeof line, f)) {
        if (strncmp(line, key, kl) || line[kl] != '=') continue;
        line[strcspn(line, "\r\n")] = 0;
        snprintf(out, n, "%s", line + kl + 1);
    }
    fclose(f);
}

static int find_fb(const char *needle, char *out, size_t n)
{
    FILE *f = fopen("/proc/fb", "r");
    if (!f) return -1;
    char line[128];
    int num = -1;
    while (fgets(line, sizeof line, f))
        if (strcasestr(line, needle) && sscanf(line, "%d", &num) == 1) break; else num = -1;
    fclose(f);
    if (num < 0) return -1;
    snprintf(out, n, "/dev/fb%d", num);
    return 0;
}

static pid_t spawn(char *const argv[], int wait_for)
{
    pid_t pid = fork();
    if (pid == 0) {
        int dn = open("/dev/null", O_RDWR);
        if (dn >= 0) { dup2(dn, 0); dup2(dn, 1); dup2(dn, 2); }
        execvp(argv[0], argv);
        _exit(127);
    }
    if (wait_for && pid > 0) {
        int st;
        waitpid(pid, &st, 0);
        return WIFEXITED(st) ? WEXITSTATUS(st) : -1;
    }
    return pid;
}

static void tmux_cmd(const char *a, const char *b, const char *c, const char *d)
{
    char *argv[] = {"tmux", (char *)a, (char *)b, (char *)c, (char *)d, NULL};
    spawn(argv, 1);
}

static void builtin_light(int on)
{
    char *argv[] = {"sudo", "-n", CTL, "builtin", on ? "on" : "off", NULL};
    spawn(argv, 1);
}

/* ------------------------------------------------------------ keyboard */

static int open_keyboard(void)
{
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
        unsigned long ev[(EV_MAX + 63) / 64] = {0}, keys[(KEY_MAX + 63) / 64] = {0};
        if (ioctl(fd, EVIOCGBIT(0, sizeof ev), ev) >= 0 && (ev[0] & (1UL << EV_KEY)) &&
            !(ev[0] & (1UL << EV_REL)) &&
            ioctl(fd, EVIOCGBIT(EV_KEY, sizeof keys), keys) >= 0 &&
            (keys[KEY_ENTER / 64] & (1UL << (KEY_ENTER % 64)))) {
            closedir(d);
            return fd;
        }
        close(fd);
    }
    closedir(d);
    return -1;
}

/* The Cardputer keyboard sends its own codes for symbols (e.g. code 39 is
 * '#', not ';'). Same table the launcher uses; the device keymap file
 * overrides it when present. */
static char g_sym[256];

static const struct { const char *name; char ch; } SYMNAMES[] = {
    {"exclam", '!'}, {"at", '@'}, {"numbersign", '#'}, {"dollar", '$'}, {"percent", '%'},
    {"asciicircum", '^'}, {"ampersand", '&'}, {"asterisk", '*'}, {"parenleft", '('},
    {"parenright", ')'}, {"asciitilde", '~'}, {"grave", '`'}, {"underscore", '_'},
    {"minus", '-'}, {"plus", '+'}, {"equal", '='}, {"bracketleft", '['},
    {"bracketright", ']'}, {"braceleft", '{'}, {"braceright", '}'}, {"semicolon", ';'},
    {"colon", ':'}, {"apostrophe", '\''}, {"quotedbl", '"'}, {"less", '<'},
    {"greater", '>'}, {"backslash", '\\'}, {"bar", '|'}, {"comma", ','},
    {"period", '.'}, {"slash", '/'}, {"question", '?'}, {"space", ' '},
};

static void load_keymap(void)
{
    static const struct { int code; char ch; } DEF[] = {
        {26, '!'}, {27, '@'}, {39, '#'}, {40, '$'}, {41, '%'}, {43, '^'}, {51, '&'},
        {52, '*'}, {53, '('}, {94, ')'}, {55, '~'}, {69, '`'}, {70, '_'}, {71, '-'},
        {72, '+'}, {73, '='}, {74, '['}, {75, ']'}, {76, '{'}, {77, '}'}, {79, ';'},
        {80, ':'}, {81, '\''}, {82, '"'}, {83, '<'}, {85, '>'}, {86, '\\'}, {89, '|'},
        {90, ','}, {91, '.'}, {92, '/'}, {93, '?'},
    };
    FILE *f = fopen(KEYMAP, "r");
    if (!f) {
        for (unsigned i = 0; i < sizeof DEF / sizeof DEF[0]; i++) g_sym[DEF[i].code] = DEF[i].ch;
        return;
    }
    char line[256], name[64];
    unsigned code;
    while (fgets(line, sizeof line, f)) {
        char *h = strchr(line, '#');
        if (h) *h = 0;
        if (sscanf(line, " keycode %u = %63s", &code, name) != 2 || code > 255) continue;
        for (unsigned i = 0; i < sizeof SYMNAMES / sizeof SYMNAMES[0]; i++)
            if (!strcmp(SYMNAMES[i].name, name)) g_sym[code] = SYMNAMES[i].ch;
    }
    fclose(f);
}

/* US layout for the standard codes (letters, digits, space...). */
static char us_char(int code, int shift)
{
    static const char *row1 = "1234567890", *row1s = "!@#$%^&*()";
    if (code >= KEY_1 && code <= KEY_0) return shift ? row1s[code - KEY_1] : row1[code - KEY_1];
    static const struct { int code; char c; } L[] = {
        {KEY_Q, 'q'}, {KEY_W, 'w'}, {KEY_E, 'e'}, {KEY_R, 'r'}, {KEY_T, 't'}, {KEY_Y, 'y'},
        {KEY_U, 'u'}, {KEY_I, 'i'}, {KEY_O, 'o'}, {KEY_P, 'p'}, {KEY_A, 'a'}, {KEY_S, 's'},
        {KEY_D, 'd'}, {KEY_F, 'f'}, {KEY_G, 'g'}, {KEY_H, 'h'}, {KEY_J, 'j'}, {KEY_K, 'k'},
        {KEY_L, 'l'}, {KEY_Z, 'z'}, {KEY_X, 'x'}, {KEY_C, 'c'}, {KEY_V, 'v'}, {KEY_B, 'b'},
        {KEY_N, 'n'}, {KEY_M, 'm'},
    };
    for (unsigned i = 0; i < sizeof L / sizeof L[0]; i++)
        if (L[i].code == code) return shift ? L[i].c - 32 : L[i].c;
    if (code == KEY_SPACE) return ' ';
    return 0;
}

static const char *fkey(int n)   /* xterm F1..F10 */
{
    static const char *F[] = {"\033OP", "\033OQ", "\033OR", "\033OS", "\033[15~",
                              "\033[17~", "\033[18~", "\033[19~", "\033[20~", "\033[21~"};
    return (n >= 1 && n <= 10) ? F[n - 1] : NULL;
}

/* ------------------------------------------------------- status screen */

typedef struct { int fd; uint8_t *mem; int w, h, bpp, stride; size_t len; } fb_t;

static int open_fb(fb_t *fb, const char *path)
{
    memset(fb, 0, sizeof *fb);
    fb->fd = open(path, O_RDWR);
    if (fb->fd < 0) return -1;
    struct fb_var_screeninfo v;
    struct fb_fix_screeninfo x;
    if (ioctl(fb->fd, FBIOGET_VSCREENINFO, &v) || ioctl(fb->fd, FBIOGET_FSCREENINFO, &x)) return -1;
    fb->w = v.xres; fb->h = v.yres; fb->bpp = v.bits_per_pixel;
    fb->stride = x.line_length; fb->len = x.smem_len;
    if (fb->bpp != 16 && fb->bpp != 32) return -1;
    fb->mem = mmap(NULL, fb->len, PROT_READ | PROT_WRITE, MAP_SHARED, fb->fd, 0);
    return fb->mem == MAP_FAILED ? -1 : 0;
}

static void put_row(fb_t *fb, int y, const uint16_t *px)
{
    uint8_t *p = fb->mem + (size_t)y * fb->stride;
    if (fb->bpp == 16) { memcpy(p, px, fb->w * 2); return; }
    for (int x = 0; x < fb->w; x++, p += 4) {
        uint16_t c = px[x];
        p[0] = (c & 0x1F) << 3; p[1] = ((c >> 5) & 0x3F) << 2; p[2] = (c >> 11) << 3; p[3] = 0xFF;
    }
}

static void render_status(canvas_t *c, int help_open, int info_tick)
{
    static char bat[32], ip[48], ram[32], cpu[32];
    if (info_tick || !bat[0]) {
        battery_text(bat, sizeof bat); ip_text(ip, sizeof ip);
        ram_text(ram, sizeof ram); cpu_text(cpu, sizeof cpu);
    }
    const uint16_t BG = rgb(0x0D, 0x11, 0x17), FG = rgb(0xF0, 0xF6, 0xFC);
    const uint16_t DIMC = rgb(0x6E, 0x76, 0x81), ACC = rgb(0x39, 0xC5, 0xCF), LINE = rgb(0x30, 0x36, 0x3D);
    for (int i = 0; i < c->w * c->h; i++) c->px[i] = BG;

    char hm[12], ampm[4], date[24], clk[4];
    time_t t = time(NULL);
    struct tm tm;
    localtime_r(&t, &tm);
    conf_get("CLOCK", clk, sizeof clk, "24");           /* read live: the app changes it */
    clock_text(hm, sizeof hm, ampm, sizeof ampm, atoi(clk) == 12);
    strftime(date, sizeof date, "%a %d %b", &tm);

    draw_small(c, 10, 6, "BigTermCLI", ACC);
    draw_small(c, 10 + 11 * DM_SMALL_W, 6, help_open ? "help open" : "on EXT display", DIMC);
    for (int x = 0; x < c->w; x++) c->px[26 * c->w + x] = LINE;

    draw_clock(c, 10, 36, hm, ampm, FG);
    draw_small(c, 12, 82, date, DIMC);

    const char *lab[4] = {"BAT", "IP", "RAM", "CPU"};
    const char *val[4] = {bat, ip, ram, cpu};
    int x0 = c->w - 8 - 18 * DM_SMALL_W;
    for (int i = 0; i < 4; i++) {
        draw_small(c, x0, 36 + i * DM_SMALL_H, lab[i], i == 0 ? ACC : DIMC);
        draw_small(c, x0 + 5 * DM_SMALL_W, 36 + i * DM_SMALL_H, val[i], FG);
    }
    for (int x = 0; x < c->w; x++) c->px[(c->h - 26) * c->w + x] = LINE;
    draw_small(c, 10, c->h - 20, "fn+H help    hold Esc quit", DIMC);
}

/* ----------------------------------------------------------------- main */

int main(void)
{
    char dim[8], boff[8], viewer_tty[32] = "";
    conf_get("DIM_BUILTIN", dim, sizeof dim, "1");
    conf_get("BUILTIN_OFF", boff, sizeof boff, "0");
    load_keymap();

    signal(SIGTERM, on_signal);
    signal(SIGINT, on_signal);
    signal(SIGHUP, on_signal);
    signal(SIGPIPE, SIG_IGN);

    /* Built-in screen */
    char fb0path[32] = "/dev/fb0", big[32];
    find_fb("st7789", fb0path, sizeof fb0path);
    fb_t fb0;
    int have_fb0 = open_fb(&fb0, fb0path) == 0;
    uint16_t *cur = NULL, *prev = NULL;
    canvas_t cv = {0};
    if (have_fb0) {
        cur = calloc((size_t)fb0.w * fb0.h, 2);
        prev = calloc((size_t)fb0.w * fb0.h, 2);
        cv = (canvas_t){cur, fb0.w, fb0.h};
    }

    int panel = find_fb("ili9341", big, sizeof big) == 0;
    const char *once = getenv("DM_STATUS_ONCE");      /* test hook: render to a raw file */
    if (once) {
        static uint16_t px[320 * 170];
        canvas_t tc = {px, 320, 170};
        render_status(&tc, 0, 1);
        FILE *o = fopen(once, "wb");
        if (o) { fwrite(px, 2, 320 * 170, o); fclose(o); }
        return 0;
    }
    if (!panel) {
        if (have_fb0) {
            for (int i = 0; i < fb0.w * fb0.h; i++) cur[i] = rgb(0x0D, 0x11, 0x17);
            draw_small(&cv, 10, 60, "BigTermCLI needs the EXT display.", rgb(0xF0, 0xF6, 0xFC));
            draw_small(&cv, 10, 80, "Open DisplayMOD to check it.", rgb(0x6E, 0x76, 0x81));
            for (int y = 0; y < fb0.h; y++) put_row(&fb0, y, cur + y * fb0.w);
        }
        sleep(3);
        return 1;
    }

    FILE *tf = fopen("/run/displaymod/tty", "r");
    if (tf) { int n; if (fscanf(tf, "%d", &n) == 1) snprintf(viewer_tty, sizeof viewer_tty, "/dev/tty%d", n); fclose(tf); }

    /* Keyboard: take it for this app only */
    int kfd = open_keyboard();
    if (kfd < 0) { fprintf(stderr, "displaymod-term: no keyboard\n"); return 1; }
    ioctl(kfd, EVIOCGRAB, 1);

    /* Shell in the shared tmux session; we only send keys, the EXT display shows it */
    char font[8];
    conf_get("FONT", font, sizeof font, "t12");
    const font_t *fo = font_find(font);
    struct winsize ws = {.ws_row = fo->rows, .ws_col = fo->cols};
    int pty;
    pid_t child = forkpty(&pty, NULL, NULL, &ws);
    if (child < 0) { perror("forkpty"); return 1; }
    if (child == 0) {
        setenv("TERM", "xterm-256color", 1);
        execlp("tmux", "tmux", "new-session", "-A", "-s", "dm", "/usr/lib/displaymod/bigterm-shell",
               ";", "set-option", "status", "off", (char *)NULL);
        _exit(127);
    }
    fcntl(pty, F_SETFL, O_NONBLOCK);

    /* Hand the EXT display to the session */
    char flag[64];
    snprintf(flag, sizeof flag, "/run/user/%u/displaymod-bigterm", (unsigned)getuid());
    close(open(flag, O_CREAT | O_WRONLY, 0644));

    /* "Built-in off while mirroring" does not apply here: the built-in
     * follows DIM_BUILTIN only. The mirror stops forcing it off once the
     * flag above exists, and forces it off again after we leave. */
    int dimmed = 0, lightctl = strcmp(boff, "1") != 0;
    long lvl = 0;
    uint64_t relight_at = 0;
    if (!strcmp(dim, "1")) {
        dimmed = 1;
        if (lightctl) builtin_light(0);
        else { FILE *b = fopen("/sys/class/backlight/backlight/brightness", "w"); if (b) { fputs("0", b); fclose(b); } }
    } else if (!lightctl) {
        char lf[64];
        snprintf(lf, sizeof lf, "/run/user/%u/displaymod-builtin-level", (unsigned)getuid());
        FILE *f = fopen(lf, "r");
        if (f) { if (fscanf(f, "%ld", &lvl) != 1) lvl = 0; fclose(f); }
        if (lvl <= 0) read_int("/sys/class/backlight/backlight/max_brightness", &lvl);
        FILE *b = fopen("/sys/class/backlight/backlight/brightness", "w");
        if (b) { fprintf(b, "%ld", lvl); fclose(b); }
        relight_at = now_ms() + 700;   /* once more, after the mirror has seen the flag */
    }

    int shift = 0, ctrl = 0, alt = 0, fn = 0, help = 0, scrolled = 0;
    uint64_t esc_down = 0, last_draw = 0, last_info = 0;
    /* Holding a key repeats it with the launcher's timing: after 500 ms, every 50 ms.
     * The keyboard driver sends no repeats, so they are made here. */
    int rep_code = 0;
    uint64_t rep_next = 0;
    int force = 1;

    while (!g_stop) {
        struct pollfd pf[2] = {{kfd, POLLIN, 0}, {pty, POLLIN, 0}};
        poll(pf, 2, rep_code ? 20 : esc_down ? 100 : 500);
        uint64_t t = now_ms();

        if (relight_at && t >= relight_at) {
            long cur_bl = 0;
            relight_at = 0;
            if (read_int("/sys/class/backlight/backlight/brightness", &cur_bl) == 0 && cur_bl == 0) {
                FILE *b = fopen("/sys/class/backlight/backlight/brightness", "w");
                if (b) { fprintf(b, "%ld", lvl); fclose(b); }
                force = 1;
            }
        }

        /* Drain tmux client output (not shown here) and notice when it ends */
        char junk[4096];
        ssize_t r;
        while ((r = read(pty, junk, sizeof junk)) > 0) {}
        if (r == 0 || (r < 0 && errno == EIO)) break;
        int st;
        pid_t w;
        while ((w = waitpid(-1, &st, WNOHANG)) > 0)
            if (w == child) g_stop = 1;

        struct input_event ev;
        for (;;) {
            if (read(kfd, &ev, sizeof ev) == (ssize_t)sizeof ev) {
                if (ev.type != EV_KEY || ev.value == 2) continue;   /* repeats are made below */
                switch (ev.code) {
                case KEY_LEFTSHIFT: case KEY_RIGHTSHIFT: case KEY_LEFTCTRL: case KEY_RIGHTCTRL:
                case KEY_LEFTALT: case KEY_RIGHTALT: case KEY_FN: break;
                default:
                    if (ev.value) { rep_code = ev.code; rep_next = t + 500; }
                    else if (ev.code == rep_code) rep_code = 0;
                }
            } else if (rep_code && t >= rep_next) {
                ev.type = EV_KEY;                 /* the held key again */
                ev.code = rep_code;
                ev.value = 2;
                rep_next = t + 50;
            } else break;
            int down = ev.value != 0, code = ev.code;
            switch (code) {
            case KEY_LEFTSHIFT: case KEY_RIGHTSHIFT: shift = down; continue;
            case KEY_LEFTCTRL: case KEY_RIGHTCTRL: ctrl = down; continue;
            case KEY_LEFTALT: case KEY_RIGHTALT: alt = down; continue;
            case KEY_FN: fn = down; continue;
            }
            if (code == KEY_ESC) esc_down = (ev.value == 1) ? t : (ev.value == 0 ? 0 : esc_down);
            if (!down) continue;

            /* help on the EXT display */
            int help_key = code == KEY_HELP || (fn && code == KEY_H);
            if (help || help_key) {
                if (ev.value != 1) continue;
                if (!help && help_key && viewer_tty[0]) {
                    char *argv[] = {"tmux", "display-popup", "-c", viewer_tty, "-w", "96%", "-h", "96%",
                                    "-b", "rounded", "-T", " BigTermCLI help ",
                                    "-s", "bg=#10151C,fg=#F0F6FC", "-S", "fg=#39C5CF",
                                    "-E", "cat " HELP_FILE "; exec sleep 86400", NULL};
                    spawn(argv, 0);
                    help = 1;
                } else if (help && (help_key || code == KEY_ESC)) {
                    char *argv[] = {"tmux", "display-popup", "-C", "-c", viewer_tty, NULL};
                    spawn(argv, 1);
                    help = 0;
                    if (code == KEY_ESC) esc_down = 0;
                }
                force = 1;
                continue;
            }

            /* Scrollback (shift+fn+L/M, or shift / ctrl+alt with PgUp/PgDn), as in the CLI app */
            int sb_mods = shift || (ctrl && alt);
            int sb_up = sb_mods && ((fn && code == KEY_L) || code == KEY_PAGEUP);
            int sb_dn = sb_mods && ((fn && code == KEY_M) || code == KEY_PAGEDOWN);
            if (sb_up) { tmux_cmd("copy-mode", "-e", "-u", "-tdm"); scrolled = 1; continue; }
            if (sb_dn) { if (scrolled) tmux_cmd("send-keys", "-tdm", "-X", "page-down"); continue; }

            /* F6 (fn+6): the CLI's viewport toggle -> the next font */
            if (ev.value == 1 && (code == KEY_F6 || (fn && code == KEY_6))) {
                char cur[8];
                conf_get("FONT", cur, sizeof cur, "t12");
                const char *next = FONTS[(font_find(cur) - FONTS + 1) % NFONTS].name;
                char *argv[] = {"sudo", "-n", CTL, "set", "FONT", (char *)next, NULL};
                spawn(argv, 1);
                continue;
            }

            /* Any other key leaves scrollback first, like the CLI app */
            if (scrolled) { tmux_cmd("send-keys", "-tdm", "-X", "cancel"); scrolled = 0; }

            const char *seq = NULL;
            char buf[4] = {0};
            if (fn) {
                switch (code) {
                case KEY_F: seq = "\033[A"; break;
                case KEY_X: seq = "\033[B"; break;
                case KEY_C: seq = "\033[C"; break;
                case KEY_Z: seq = "\033[D"; break;
                case KEY_K: seq = "\033[H"; break;
                case KEY_N: seq = "\033[F"; break;
                case KEY_L: seq = "\033[5~"; break;   /* page up / down to the program */
                case KEY_M: seq = "\033[6~"; break;
                case KEY_BACKSPACE: seq = "\033[3~"; break;
                default:
                    if (code >= KEY_1 && code <= KEY_0) seq = fkey(code - KEY_1 + 1);
                }
            }
            if (!seq) {
                switch (code) {
                case KEY_UP: seq = "\033[A"; break;
                case KEY_DOWN: seq = "\033[B"; break;
                case KEY_RIGHT: seq = "\033[C"; break;
                case KEY_LEFT: seq = "\033[D"; break;
                case KEY_HOME: seq = "\033[H"; break;
                case KEY_END: seq = "\033[F"; break;
                case KEY_PAGEUP: seq = "\033[5~"; break;
                case KEY_PAGEDOWN: seq = "\033[6~"; break;
                case KEY_DELETE: seq = "\033[3~"; break;
                case KEY_ENTER: case KEY_KPENTER: seq = ev.value == 1 ? "\r" : NULL; break;
                case KEY_BACKSPACE: seq = "\177"; break;
                case KEY_TAB: seq = "\t"; break;
                case KEY_ESC: seq = ev.value == 1 ? "\033" : NULL; break;
                default:
                    if (code >= KEY_F1 && code <= KEY_F10) { seq = fkey(code - KEY_F1 + 1); break; }
                    {
                        char ch = (code < 256 && g_sym[code]) ? g_sym[code] : us_char(code, shift);
                        if (!ch) break;
                        if (ctrl && ch >= '@' && ch <= '~') ch &= 0x1F;
                        int n = 0;
                        if (alt) buf[n++] = '\033';
                        buf[n++] = ch;
                        seq = buf;
                    }
                }
            }
            if (seq && write(pty, seq, strlen(seq)) < 0) {}
        }

        /* hold Esc to quit */
        if (esc_down && t - esc_down >= 1500) break;

        /* built-in status page, redrawn only when something changed */
        if (have_fb0 && !dimmed && (force || t - last_draw >= 1000)) {
            last_draw = t;
            int info = t - last_info >= 5000;
            if (info) last_info = t;
            render_status(&cv, help, info);
            for (int y = 0; y < fb0.h; y++) {
                const uint16_t *a = cur + y * fb0.w, *b = prev + y * fb0.w;
                if (force || memcmp(a, b, fb0.w * 2)) put_row(&fb0, y, a);
            }
            memcpy(prev, cur, (size_t)fb0.w * fb0.h * 2);
            force = 0;
        }
    }

    /* Leave: close help, detach (session keeps running), give screens back */
    if (help) { char *argv[] = {"tmux", "display-popup", "-C", "-c", viewer_tty, NULL}; spawn(argv, 1); }
    kill(child, SIGTERM);
    waitpid(child, NULL, 0);
    unlink(flag);
    if (dimmed && lightctl) builtin_light(1);
    ioctl(kfd, EVIOCGRAB, 0);
    return 0;
}
