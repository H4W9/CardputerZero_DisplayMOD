/* Test backend: settings kept in memory, used for host screenshots. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "backend.h"

static dm_pair_t g_set[] = {
    {"MODE", "mirror"}, {"BIGTERM", "1"}, {"MIRROR_LAYOUT", "bar"}, {"DIM_BUILTIN", "1"}, {"DETECT", "1"}, {"BRIGHTNESS", "sync"}, {"BORDER_COLOR", "black"}, {"ALIGN", "center"}, {"BUILTIN_OFF", "0"}, {"SLEEP", "5"}, {"CLOCK", "24"}, {"HISTORY", "1"}, {"HIST_SIZE", "1000"}, {"HIST_DUPS", "1"},
    {"ROTATE", "90"}, {"FONT", "t12"}, {"SPEED", "16000000"}, {"BGR", "0"},
    {"TTY", "auto"}, {"DC", "23"}, {"RST", "22"}, {"LED", "26"},
};
static int g_busy;

int dm_load(void) { return 0; }

const char *dm_get(const char *key)
{
    for (unsigned i = 0; i < sizeof g_set / sizeof g_set[0]; i++)
        if (!strcmp(g_set[i].key, key)) return g_set[i].val;
    return "";
}

int dm_set_async(const char *key, const char *val)
{
    for (unsigned i = 0; i < sizeof g_set / sizeof g_set[0]; i++)
        if (!strcmp(g_set[i].key, key)) snprintf(g_set[i].val, sizeof g_set[i].val, "%s", val);
    g_busy = 3;
    return 0;
}

int dm_apply_async(void) { g_busy = 3; return 0; }
int dm_set_many_async(const char *const *kv, int pairs)
{
    for (int i = 0; i < pairs; i++) dm_set_async(kv[2 * i], kv[2 * i + 1]);
    return 0;
}
int dm_uninstall_async(void) { g_busy = 3; return 0; }

int dm_poll(int *rc)
{
    *rc = 0;
    if (g_busy > 0) { g_busy--; return 0; }
    return 1;
}

int dm_status(dm_pair_t *out, int max)
{
    static const dm_pair_t s[] = {
        /* same lines as displaymod-ctl status (store edition) */
        {"Mode", "mirror"}, {"EXT display", "ILI9341 on fb1"}, {"Mirror", "active on tty8"},
        {"BigTermCLI", "in launcher"}, {"Built-in light", "80"}, {"Free RAM", "263 MB"},
        {"Mirror RAM", "1180 KB"}, {"Header 5V", "255"},
    };
    int n = 0;
    for (; n < max && n < (int)(sizeof s / sizeof s[0]); n++) out[n] = s[n];
    return n;
}

int dm_panel_up(void) { return strcmp(dm_get("MODE"), "off") != 0; }

int dm_panel_state(void)
{
    const char *e = getenv("DM_FAKE_PANEL");   /* up | missing | down */
    if (e && !strcmp(e, "missing")) return -1;
    if (e && !strcmp(e, "down")) return 0;
    return dm_panel_up();
}

/* newest first */
static const char *g_hdemo[] = {
    "htop", "ls -la ~/Documents", "sudo apt update && sudo apt upgrade",
    "ping -c 3 192.168.137.1", "python3 ~/scripts/weather.py --city Berlin --units metric --days 3",
    "git -C ~/src/notes pull", "df -h", "nano ~/.bashrc", "ip addr", "cd ~/Downloads",
    "uptime", "journalctl -u displaymod -n 40", "cat /proc/cpuinfo", "free -m",
};
static int g_hdel[64], g_hnd, g_hcleared;
static int hdemo_n(void) { return g_hcleared ? 0 : (int)(sizeof g_hdemo / sizeof g_hdemo[0]); }

int dm_history_load(void) { return hdemo_n() - g_hnd; }
int dm_history_count(void) { return dm_history_load(); }
void dm_history_clear(void) { g_hcleared = 1; g_hnd = 0; }

static int hdemo_at(int i)          /* index into g_hdemo of the i-th shown item */
{
    for (int k = 0; k < hdemo_n(); k++) {
        int gone = 0;
        for (int d = 0; d < g_hnd; d++) if (g_hdel[d] == k) gone = 1;
        if (!gone && i-- == 0) return k;
    }
    return -1;
}
const char *dm_history_item(int i) { int k = hdemo_at(i); return k < 0 ? "" : g_hdemo[k]; }
int dm_history_delete(int i)
{
    int k = hdemo_at(i);
    if (k < 0 || g_hnd >= 64) return -1;
    g_hdel[g_hnd++] = k;
    return 0;
}
