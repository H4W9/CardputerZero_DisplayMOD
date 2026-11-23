/* dmdraw.h - shared drawing and system-info helpers for DisplayMOD
 * (status bar in displaymod-mirror, status screen in displaymod-term). */
#pragma once
#include <arpa/inet.h>
#include <dirent.h>
#include <ifaddrs.h>
#include <net/if.h>
#include <netinet/in.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "font_dm.h"

static inline uint16_t rgb(int r, int g, int b)
{
    return (uint16_t)(((r & 0xF8) << 8) | ((g & 0xFC) << 3) | (b >> 3));
}

static uint16_t blend(uint16_t bg, uint16_t fg, unsigned a)
{
    int br = bg >> 11, bgc = (bg >> 5) & 0x3F, bb = bg & 0x1F;
    int fr = fg >> 11, fgc = (fg >> 5) & 0x3F, fbb = fg & 0x1F;
    int r = br + ((fr - br) * (int)a) / 255;
    int g = bgc + ((fgc - bgc) * (int)a) / 255;
    int b = bb + ((fbb - bb) * (int)a) / 255;
    return (uint16_t)((r << 11) | (g << 5) | b);
}

typedef struct { uint16_t *px; int w, h; } canvas_t;

static void draw_glyph(canvas_t *c, int x0, int y0, const unsigned char *a, int gw, int gh, uint16_t col)
{
    for (int y = 0; y < gh; y++) {
        int yy = y0 + y;
        if (yy < 0 || yy >= c->h) continue;
        for (int x = 0; x < gw; x++) {
            int xx = x0 + x;
            unsigned v = a[y * gw + x];
            if (!v || xx < 0 || xx >= c->w) continue;
            uint16_t *d = &c->px[yy * c->w + xx];
            *d = blend(*d, col, v);
        }
    }
}

static int draw_small(canvas_t *c, int x, int y, const char *s, uint16_t col)
{
    for (; *s; s++, x += DM_SMALL_W) {
        unsigned ch = (unsigned char)*s;
        if (ch < 0x20 || ch > 0x7E) ch = '?';
        draw_glyph(c, x, y, dm_small[ch - 0x20], DM_SMALL_W, DM_SMALL_H, col);
    }
    return x;
}

static void draw_big(canvas_t *c, int x, int y, const char *s, uint16_t col)
{
    for (; *s; s++) {
        int i = (*s == ':') ? 10 : (*s >= '0' && *s <= '9') ? *s - '0' : -1;
        int colon = (*s == ':');
        if (i >= 0) draw_glyph(c, colon ? x - 4 : x, y, dm_big[i], DM_BIG_W, DM_BIG_H, col);
        x += colon ? DM_BIG_W * 2 / 3 : DM_BIG_W;
    }
}

static int read_int(const char *path, long *out)
{
    FILE *f = fopen(path, "r");
    if (!f) return -1;
    int ok = fscanf(f, "%ld", out) == 1;
    fclose(f);
    return ok ? 0 : -1;
}

static void battery_text(char *out, size_t n)
{
    snprintf(out, n, "--");
    DIR *d = opendir("/sys/class/power_supply");
    if (!d) return;
    struct dirent *e;
    while ((e = readdir(d))) {
        if (e->d_name[0] == '.') continue;
        char p[300], status[32] = "";
        long cap;
        snprintf(p, sizeof p, "/sys/class/power_supply/%s/capacity", e->d_name);
        if (read_int(p, &cap) != 0) continue;
        snprintf(p, sizeof p, "/sys/class/power_supply/%s/status", e->d_name);
        FILE *f = fopen(p, "r");
        if (f) { if (!fgets(status, sizeof status, f)) status[0] = 0; fclose(f); }
        snprintf(out, n, "%ld%%%s", cap, strncmp(status, "Charging", 8) == 0 ? " +" : "");
        break;
    }
    closedir(d);
}

static void ip_text(char *out, size_t n)
{
    snprintf(out, n, "offline");
    struct ifaddrs *ifa, *i;
    if (getifaddrs(&ifa) != 0) return;
    int best = 0;
    for (i = ifa; i; i = i->ifa_next) {
        if (!i->ifa_addr || i->ifa_addr->sa_family != AF_INET) continue;
        if (i->ifa_flags & IFF_LOOPBACK) continue;
        int score = (i->ifa_name[0] == 'w') ? 2 : 1;
        if (score <= best) continue;
        best = score;
        inet_ntop(AF_INET, &((struct sockaddr_in *)i->ifa_addr)->sin_addr, out, n);
    }
    freeifaddrs(ifa);
}

static void ram_text(char *out, size_t n)
{
    snprintf(out, n, "--");
    FILE *f = fopen("/proc/meminfo", "r");
    if (!f) return;
    char line[128];
    long kb;
    while (fgets(line, sizeof line, f))
        if (sscanf(line, "MemAvailable: %ld kB", &kb) == 1) { snprintf(out, n, "%ld MB free", kb / 1024); break; }
    fclose(f);
}

static void cpu_text(char *out, size_t n)
{
    long t = -1;
    double load = 0;
    read_int("/sys/class/thermal/thermal_zone0/temp", &t);
    FILE *f = fopen("/proc/loadavg", "r");
    if (f) { if (fscanf(f, "%lf", &load) != 1) load = 0; fclose(f); }
    if (t >= 0) snprintf(out, n, "%ldC  load %.2f", t / 1000, load);
    else snprintf(out, n, "load %.2f", load);
}

