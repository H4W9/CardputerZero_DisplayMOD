/* Device backend: runs "/usr/lib/displaymod/displaymod-send ..." (the DisplayMOD service) without a shell. */
#define _GNU_SOURCE
#include <fcntl.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/wait.h>
#include <unistd.h>

#include "backend.h"

#define SEND "/usr/lib/displaymod/displaymod-send"
#define MAX_KEYS 24

static dm_pair_t g_set[MAX_KEYS];
static int g_n;
static pid_t g_job;

static int run_capture(const char *const argv[], char *out, size_t n)
{
    int p[2];
    if (pipe(p) != 0) return -1;
    pid_t pid = fork();
    if (pid < 0) { close(p[0]); close(p[1]); return -1; }
    if (pid == 0) {
        dup2(p[1], 1);
        int dn = open("/dev/null", O_WRONLY);
        if (dn >= 0) dup2(dn, 2);
        close(p[0]); close(p[1]);
        execv(SEND, (char *const *)argv);
        _exit(127);
    }
    close(p[1]);
    size_t len = 0;
    ssize_t r;
    while (len + 1 < n && (r = read(p[0], out + len, n - 1 - len)) > 0) len += r;
    out[len] = 0;
    close(p[0]);
    int st;
    waitpid(pid, &st, 0);
    return WIFEXITED(st) ? WEXITSTATUS(st) : -1;
}

static void put(const char *k, const char *v)
{
    for (int i = 0; i < g_n; i++)
        if (!strcmp(g_set[i].key, k)) { snprintf(g_set[i].val, sizeof g_set[i].val, "%s", v); return; }
    if (g_n < MAX_KEYS) {
        snprintf(g_set[g_n].key, sizeof g_set[g_n].key, "%s", k);
        snprintf(g_set[g_n].val, sizeof g_set[g_n].val, "%s", v);
        g_n++;
    }
}

int dm_load(void)
{
    const char *argv[] = {SEND, "get-all", NULL};
    char buf[1024];
    if (run_capture(argv, buf, sizeof buf) != 0) return -1;
    for (char *line = strtok(buf, "\n"); line; line = strtok(NULL, "\n")) {
        char *eq = strchr(line, '=');
        if (!eq) continue;
        *eq = 0;
        put(line, eq + 1);
    }
    return 0;
}

const char *dm_get(const char *key)
{
    for (int i = 0; i < g_n; i++)
        if (!strcmp(g_set[i].key, key)) return g_set[i].val;
    return "";
}

static int spawn(const char *const argv[])
{
    if (g_job) return -1;
    pid_t pid = fork();
    if (pid < 0) return -1;
    if (pid == 0) {
        int dn = open("/dev/null", O_RDWR);
        if (dn >= 0) { dup2(dn, 0); dup2(dn, 1); dup2(dn, 2); }
        setsid();
        execv(SEND, (char *const *)argv);
        _exit(127);
    }
    g_job = pid;
    return 0;
}

int dm_set_async(const char *key, const char *val)
{
    const char *argv[] = {SEND, "set", key, val, NULL};
    if (spawn(argv) != 0) return -1;
    put(key, val);
    return 0;
}

int dm_set_many_async(const char *const *kv, int pairs)
{
    const char *argv[2 + 2 * 24 + 1] = {SEND, "set-many"};
    if (pairs > 24) pairs = 24;
    int a = 2;
    for (int i = 0; i < 2 * pairs; i++) argv[a++] = kv[i];
    argv[a] = NULL;
    if (spawn(argv) != 0) return -1;
    for (int i = 0; i < pairs; i++) put(kv[2 * i], kv[2 * i + 1]);
    return 0;
}

int dm_apply_async(void)
{
    const char *argv[] = {SEND, "apply", NULL};
    return spawn(argv);
}

int dm_uninstall_async(void)
{
    return -1;   /* store edition: removing the package needs sudo apt remove */
}

int dm_poll(int *rc)
{
    if (!g_job) { *rc = 0; return 1; }
    int st;
    pid_t r = waitpid(g_job, &st, WNOHANG);
    if (r == 0) return 0;
    g_job = 0;
    *rc = (r > 0 && WIFEXITED(st)) ? WEXITSTATUS(st) : -1;
    dm_load();
    return 1;
}

int dm_status(dm_pair_t *out, int max)
{
    const char *argv[] = {SEND, "status", NULL};
    char buf[1024];
    if (run_capture(argv, buf, sizeof buf) != 0) return 0;
    int n = 0;
    for (char *line = strtok(buf, "\n"); line && n < max; line = strtok(NULL, "\n")) {
        char *bar = strchr(line, '|');
        if (!bar) continue;
        *bar = 0;
        snprintf(out[n].key, sizeof out[n].key, "%s", line);
        snprintf(out[n].val, sizeof out[n].val, "%s", bar + 1);
        n++;
    }
    return n;
}

int dm_panel_state(void)
{
    const char *argv[] = {SEND, "panel-state", NULL};
    char buf[16];
    if (run_capture(argv, buf, sizeof buf) != 0) return 0;
    if (!strncmp(buf, "up", 2)) return 1;
    if (!strncmp(buf, "missing", 7)) return -1;
    return 0;
}

int dm_panel_up(void)
{
    const char *argv[] = {SEND, "panel-state", NULL};
    char buf[16];
    return run_capture(argv, buf, sizeof buf) == 0 && !strncmp(buf, "up", 2);
}

/* ---- BigTermCLI history: ~/.local/share/displaymod/bigterm_history ---- */
static void hist_path(char *out, size_t n, const char *suffix)
{
    const char *home = getenv("HOME");
    snprintf(out, n, "%s/.local/share/displaymod/bigterm_history%s", home ? home : "/tmp", suffix);
}

int dm_history_count(void)
{
    char p[256];
    hist_path(p, sizeof p, "");
    FILE *f = fopen(p, "r");
    if (!f) return 0;
    int n = 0, c, last = '\n';
    while ((c = fgetc(f)) != EOF) { if (c == '\n') n++; last = c; }
    if (last != '\n') n++;
    fclose(f);
    return n;
}

void dm_history_clear(void)
{
    char p[256];
    hist_path(p, sizeof p, "");
    FILE *f = fopen(p, "w");                  /* empty the saved file */
    if (f) fclose(f);
    hist_path(p, sizeof p, ".clear");         /* running shell clears at its next prompt */
    f = fopen(p, "w");
    if (f) fclose(f);
}
