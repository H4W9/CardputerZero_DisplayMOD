/* Test backend: settings kept in memory, used for host screenshots. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "backend.h"

static dm_pair_t g_set[] = {
    {"MODE", "mirror"}, {"BIGTERM", "1"}, {"MIRROR_LAYOUT", "bar"}, {"DIM_BUILTIN", "1"}, {"DETECT", "1"}, {"BRIGHTNESS", "sync"}, {"BORDER_COLOR", "black"}, {"ALIGN", "center"}, {"BUILTIN_OFF", "0"}, {"SLEEP", "5"}, {"HISTORY", "1"}, {"HIST_SIZE", "1000"}, {"HIST_DUPS", "1"},
    {"ROTATE", "90"}, {"FONT", "6x12"}, {"SPEED", "16000000"}, {"BGR", "0"},
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
        /* same lines as displaymod-ctl status (original edition) */
        {"Mode", "mirror"}, {"EXT display", "ILI9341 on fb1"}, {"Mirror", "active"},
        {"BigTermCLI", "installed"}, {"Built-in light", "80"}, {"Free RAM", "263 MB"},
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

static int g_hist = 42;
int dm_history_count(void) { return g_hist; }
void dm_history_clear(void) { g_hist = 0; }
