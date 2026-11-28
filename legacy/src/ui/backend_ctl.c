/* Device backend: runs "sudo -n /usr/lib/displaymod/displaymod-ctl ..." without a shell. */
#define _GNU_SOURCE
#include <fcntl.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/wait.h>
#include <unistd.h>

#include "backend.h"

#define CTL "/usr/lib/displaymod/displaymod-ctl"
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
        execv("/usr/bin/sudo", (char *const *)argv);
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
    const char *argv[] = {"sudo", "-n", CTL, "get-all", NULL};
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
        execv("/usr/bin/sudo", (char *const *)argv);
        _exit(127);
    }
    g_job = pid;
    return 0;
}

int dm_set_async(const char *key, const char *val)
{
    const char *argv[] = {"sudo", "-n", CTL, "set", key, val, NULL};
    if (spawn(argv) != 0) return -1;
    put(key, val);
    return 0;
}

int dm_set_many_async(const char *const *kv, int pairs)
{
    const char *argv[4 + 2 * 24 + 1] = {"sudo", "-n", CTL, "set-many"};
    if (pairs > 24) pairs = 24;
    int a = 4;
    for (int i = 0; i < 2 * pairs; i++) argv[a++] = kv[i];
    argv[a] = NULL;
    if (spawn(argv) != 0) return -1;
    for (int i = 0; i < pairs; i++) put(kv[2 * i], kv[2 * i + 1]);
    return 0;
}

int dm_apply_async(void)
{
    const char *argv[] = {"sudo", "-n", CTL, "apply", NULL};
    return spawn(argv);
}

int dm_uninstall_async(void)
{
    const char *argv[] = {"sudo", "-n", CTL, "uninstall", NULL};
    return spawn(argv);
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
    const char *argv[] = {"sudo", "-n", CTL, "status", NULL};
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
    const char *argv[] = {"sudo", "-n", CTL, "panel-state", NULL};
    char buf[16];
    if (run_capture(argv, buf, sizeof buf) != 0) return 0;
    if (!strncmp(buf, "up", 2)) return 1;
    if (!strncmp(buf, "missing", 7)) return -1;
    return 0;
}

int dm_panel_up(void)
{
    const char *argv[] = {"sudo", "-n", CTL, "panel-state", NULL};
    char buf[16];
    return run_capture(argv, buf, sizeof buf) == 0 && !strncmp(buf, "up", 2);
}

/* ---- BigTermCLI history: ~/.local/share/displaymod/bigterm_history ---- */
static void hist_path(char *out, size_t n, const char *suffix)
{
    const char *home = getenv("HOME");
    snprintf(out, n, "%s/.local/share/displaymod/bigterm_history%s", home ? home : "/tmp", suffix);
}

/* The file as lines. Bash writes "#<seconds>" before a command when
 * HISTTIMEFORMAT is set; those belong to the command after them. */
static char **g_hl;              /* all lines */
static int g_hn;
static int *g_he, g_en;          /* line index of each command, oldest first */

static int is_stamp(const char *s)
{
    if (s[0] != '#' || !s[1]) return 0;
    for (s++; *s; s++) if (*s < '0' || *s > '9') return 0;
    return 1;
}

int dm_history_load(void)
{
    for (int i = 0; i < g_hn; i++) free(g_hl[i]);
    free(g_hl); free(g_he);
    g_hl = NULL; g_he = NULL; g_hn = g_en = 0;
    char p[256];
    hist_path(p, sizeof p, "");
    FILE *f = fopen(p, "r");
    if (!f) return 0;
    char *line = NULL;
    size_t cap = 0;
    ssize_t len;
    int max = 0;
    while ((len = getline(&line, &cap, f)) >= 0) {
        if (len && line[len - 1] == '\n') line[--len] = 0;
        if (g_hn == max) {
            max = max ? max * 2 : 256;
            g_hl = realloc(g_hl, max * sizeof *g_hl);
            g_he = realloc(g_he, max * sizeof *g_he);
        }
        if (!is_stamp(line)) g_he[g_en++] = g_hn;
        g_hl[g_hn++] = strdup(line);
    }
    free(line);
    fclose(f);
    return g_en;
}

const char *dm_history_item(int i)
{
    return i >= 0 && i < g_en ? g_hl[g_he[g_en - 1 - i]] : "";
}

int dm_history_delete(int i)
{
    if (i < 0 || i >= g_en) return -1;
    int cmd = g_he[g_en - 1 - i];
    int from = cmd > 0 && is_stamp(g_hl[cmd - 1]) ? cmd - 1 : cmd;
    char p[256], tmp[264];
    hist_path(p, sizeof p, "");
    snprintf(tmp, sizeof tmp, "%s.new", p);
    FILE *f = fopen(tmp, "w");
    if (!f) return -1;
    for (int l = 0; l < g_hn; l++)
        if (l < from || l > cmd) fprintf(f, "%s\n", g_hl[l]);
    if (fclose(f) != 0 || rename(tmp, p) != 0) { unlink(tmp); return -1; }
    hist_path(p, sizeof p, ".reload");        /* running shell reloads at its next prompt */
    f = fopen(p, "w");
    if (f) fclose(f);
    dm_history_load();
    return 0;
}

int dm_history_count(void)
{
    return dm_history_load();
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
