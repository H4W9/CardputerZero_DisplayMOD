/*
 * displaymod-daemon - DisplayMOD service for the CardputerZero (store edition).
 *
 * Runs from displaymod.service as a systemd DynamicUser ("displaymod") with a
 * few capabilities, never as root. It
 *   - runs displaymod-ctl for local users who connect to /run/displaymod/ctl.sock
 *     (only a fixed list of commands; ctl checks every value itself),
 *   - starts and stops the mirror on the EXT display's console (/run/displaymod/view
 *     holds "TTY TOKEN" while mirroring is wanted; a new token restarts the
 *     mirror so it picks up layout changes),
 *   - hands that console to BigTermCLI ("bigterm on PID" / "bigterm off"),
 *   - pauses the mirror while the DisplayMOD app draws on the EXT display
 *     ("ui on PID" / "ui off").
 *
 * Request: arguments separated by NUL bytes, then the client shuts down its
 * write side. Reply: ctl's output, then a NUL byte and the exit code (1 byte).
 */
#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <linux/vt.h>
#include <poll.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/un.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#define RUN     "/run/displaymod"
#define SOCK    RUN "/ctl.sock"
#define VIEW    RUN "/view"
#define BTFLAG  RUN "/bigterm"
#define BTACK   RUN "/bigterm-ack"
#define MPID    RUN "/mirror.pid"
#define UIFLAG  RUN "/ui"
#define UIACK   RUN "/ui-ack"
#define LIB     "/usr/lib/displaymod"
#define CTL     LIB "/displaymod-ctl"
#define MIRROR  LIB "/displaymod-mirror"
#define MAXARGS 64

static volatile sig_atomic_t g_stop;
static pid_t g_mirror;            /* running mirror, 0 if none */
static int g_mirror_tty;
static time_t g_mirror_started;
static int g_mirror_crashed;      /* restart after a crash waits 2 s */
static char g_view_tok[96];       /* contents of VIEW the running mirror was started for */
static int g_bt_tty, g_bt_pid;    /* BigTermCLI owner of the console */
static int g_bt_prev_vt;          /* console that was active before BigTermCLI */
static uid_t g_bt_old_uid;
static gid_t g_bt_old_gid;
static mode_t g_bt_old_mode;

static void on_signal(int s) { (void)s; g_stop = 1; }

static int read_num(const char *path)
{
    FILE *f = fopen(path, "r");
    int n = 0;
    if (!f) return 0;
    if (fscanf(f, "%d", &n) != 1) n = 0;
    fclose(f);
    return n;
}

/* Run ctl with args; its output goes to fd out (or the journal if out < 0). */
static int run_ctl(char *const args[], int out)
{
    pid_t pid = fork();
    if (pid < 0) return -1;
    if (pid == 0) {
        int dn = open("/dev/null", O_RDONLY);
        if (dn >= 0) dup2(dn, 0);
        if (out >= 0) { dup2(out, 1); dup2(out, 2); }
        char *argv[MAXARGS + 2] = {"displaymod-ctl"};
        int i = 0;
        while (args[i] && i < MAXARGS) { argv[i + 1] = args[i]; i++; }
        argv[i + 1] = NULL;
        char *envp[] = {"PATH=/usr/sbin:/usr/bin:/sbin:/bin", "DM_DAEMON=1", "LANG=C.UTF-8", NULL};
        execve(CTL, argv, envp);
        _exit(127);
    }
    int st;
    while (waitpid(pid, &st, 0) < 0 && errno == EINTR) {}
    return WIFEXITED(st) ? WEXITSTATUS(st) : 1;
}

/* ---------------------------------------------------------------- mirror */

static void stop_mirror(void)
{
    if (!g_mirror) return;
    kill(g_mirror, SIGTERM);
    for (int i = 0; i < 30; i++) {
        if (waitpid(g_mirror, NULL, WNOHANG) == g_mirror) goto done;
        usleep(100000);
    }
    kill(g_mirror, SIGKILL);
    waitpid(g_mirror, NULL, 0);
done:
    g_mirror = 0;
    unlink(MPID);
}

static void read_line(const char *path, char *out, size_t n)
{
    out[0] = 0;
    FILE *f = fopen(path, "r");
    if (!f) return;
    if (!fgets(out, (int)n, f)) out[0] = 0;
    fclose(f);
    out[strcspn(out, "\n")] = 0;
}

static void start_mirror(int tty)
{
    pid_t pid = fork();
    if (pid < 0) return;
    if (pid == 0) {
        char dev[32];
        snprintf(dev, sizeof dev, "/dev/tty%d", tty);
        setsid();
        int fd = open(dev, O_RDWR | O_NOCTTY);   /* not our controlling tty: BigTermCLI's viewer takes that */
        if (fd < 0) _exit(1);
        dup2(fd, 0); dup2(fd, 1);
        char *argv[] = {"displaymod-mirror", NULL};
        char *envp[] = {"PATH=/usr/bin:/bin", "TERM=linux", NULL};
        execve(MIRROR, argv, envp);
        _exit(127);
    }
    g_mirror = pid;
    g_mirror_tty = tty;
    g_mirror_started = time(NULL);
    FILE *f = fopen(MPID, "w");
    if (f) { fprintf(f, "%d\n", (int)pid); fclose(f); }
}

/* Make the running mirror match /run/displaymod/view. */
static void reconcile(void)
{
    if (g_mirror) {
        int st;
        pid_t r = waitpid(g_mirror, &st, WNOHANG);
        if (r == g_mirror || (r < 0 && errno == ECHILD)) { g_mirror = 0; g_mirror_crashed = 1; unlink(MPID); }
    }
    char tok[96];
    read_line(VIEW, tok, sizeof tok);
    int want = atoi(tok);
    if (g_mirror && (want != g_mirror_tty || strcmp(tok, g_view_tok))) stop_mirror();   /* settings applied */
    if (!g_mirror && want > 0 && (!g_mirror_crashed || time(NULL) - g_mirror_started >= 2)) {
        start_mirror(want);
        g_mirror_crashed = 0;
        snprintf(g_view_tok, sizeof g_view_tok, "%s", tok);
    }
}

/* ------------------------------------------------------------- BigTermCLI */

/* Make console vt the active one; returns the one that was active (0 if unknown). */
static int switch_vt(int vt)
{
    int fd = open("/dev/tty0", O_RDWR | O_NOCTTY);
    if (fd < 0) return 0;
    struct vt_stat vs;
    int prev = ioctl(fd, VT_GETSTATE, &vs) == 0 ? vs.v_active : 0;
    if (vt > 0 && vt != prev && ioctl(fd, VT_ACTIVATE, vt) == 0) ioctl(fd, VT_WAITACTIVE, vt);
    close(fd);
    return prev;
}

static void bigterm_off(void)
{
    if (!g_bt_tty) { unlink(BTFLAG); return; }
    char dev[32];
    snprintf(dev, sizeof dev, "/dev/tty%d", g_bt_tty);
    unlink(BTFLAG);
    if (chown(dev, g_bt_old_uid, g_bt_old_gid) != 0) {}
    chmod(dev, g_bt_old_mode);
    g_bt_tty = 0;
    g_bt_pid = 0;
    if (g_bt_prev_vt > 0) switch_vt(g_bt_prev_vt);   /* back to the launcher's console */
    g_bt_prev_vt = 0;
}

static int bigterm_on(uid_t uid, int pid, int out)
{
    int tty = read_num(RUN "/tty");
    if (tty <= 0 || !g_mirror) { dprintf(out, "EXT display is not running\n"); return 1; }
    if (g_bt_tty) bigterm_off();
    char dev[32];
    struct stat sb;
    snprintf(dev, sizeof dev, "/dev/tty%d", tty);
    if (stat(dev, &sb) != 0) { dprintf(out, "%s missing\n", dev); return 1; }
    g_bt_old_uid = sb.st_uid; g_bt_old_gid = sb.st_gid; g_bt_old_mode = sb.st_mode & 07777;
    if (chown(dev, uid, -1) != 0 || chmod(dev, 0620) != 0) { dprintf(out, "cannot hand over %s\n", dev); return 1; }
    g_bt_tty = tty;
    g_bt_pid = pid;
    unlink(BTACK);
    FILE *f = fopen(BTFLAG, "w");
    if (f) { fprintf(f, "%u %d\n", (unsigned)uid, pid); fclose(f); }
    for (int i = 0; i < 30 && access(BTACK, F_OK) != 0; i++) usleep(100000);   /* mirror switched to text */
    g_bt_prev_vt = switch_vt(tty);                 /* fbcon only draws the active console */
    dprintf(out, "%d\n", tty);
    return 0;
}

static int ui_on(int pid, int out)
{
    FILE *f = fopen(UIFLAG, "w");
    if (!f) { dprintf(out, "cannot pause the mirror\n"); return 1; }
    fprintf(f, "%d\n", pid);
    fclose(f);
    for (int i = 0; g_mirror && i < 20 && access(UIACK, F_OK) != 0; i++) usleep(100000);
    return 0;
}

static void ui_check(void)   /* app gone without saying so: resume the mirror */
{
    int pid = read_num(UIFLAG);
    if (pid > 0 && kill(pid, 0) != 0 && errno == ESRCH) unlink(UIFLAG);
}

static void bigterm_check(void)
{
    if (g_bt_tty && g_bt_pid > 0 && kill(g_bt_pid, 0) != 0 && errno == ESRCH) bigterm_off();
    if (g_bt_tty && !g_mirror) bigterm_off();
}

/* ---------------------------------------------------------------- clients */

static int allowed(const char *c)
{
    static const char *const ok[] = {"apply", "set", "set-many", "get", "get-all", "builtin",
                                     "panel-state", "status", NULL};
    for (int i = 0; ok[i]; i++) if (!strcmp(c, ok[i])) return 1;
    return 0;
}

static void handle(int c)
{
    struct ucred cr;
    socklen_t l = sizeof cr;
    if (getsockopt(c, SOL_SOCKET, SO_PEERCRED, &cr, &l) != 0) return;
    if (!(cr.uid == 0 || (cr.uid >= 1000 && cr.uid < 65534))) return;   /* people, not services */

    struct timeval tv = {3, 0};
    setsockopt(c, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof tv);
    tv.tv_sec = 10;
    setsockopt(c, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof tv);

    static char buf[2048];
    size_t len = 0;
    ssize_t r;
    while (len < sizeof buf - 1 && (r = read(c, buf + len, sizeof buf - 1 - len)) > 0) len += r;
    buf[len] = 0;

    char *args[MAXARGS + 1];
    int n = 0;
    for (size_t i = 0; i < len && n < MAXARGS; ) {
        args[n++] = buf + i;
        i += strlen(buf + i) + 1;
    }
    args[n] = NULL;

    int rc;
    if (n == 0) {
        rc = 2;
    } else if (!strcmp(args[0], "bigterm") && n >= 2 && !strcmp(args[1], "on")) {
        rc = bigterm_on(cr.uid, n >= 3 ? atoi(args[2]) : (int)cr.pid, c);
    } else if (!strcmp(args[0], "bigterm") && n >= 2 && !strcmp(args[1], "off")) {
        bigterm_off();
        rc = 0;
    } else if (!strcmp(args[0], "ui") && n >= 2 && !strcmp(args[1], "on")) {
        rc = ui_on(n >= 3 ? atoi(args[2]) : (int)cr.pid, c);
    } else if (!strcmp(args[0], "ui") && n >= 2 && !strcmp(args[1], "off")) {
        unlink(UIFLAG);
        rc = 0;
    } else if (allowed(args[0])) {
        rc = run_ctl(args, c);
        reconcile();
    } else {
        dprintf(c, "unknown command\n");
        rc = 2;
    }
    unsigned char tail[2] = {0, (unsigned char)rc};
    if (write(c, tail, 2) < 0) {}
}

int main(void)
{
    signal(SIGTERM, on_signal);
    signal(SIGINT, on_signal);
    signal(SIGPIPE, SIG_IGN);
    umask(022);
    mkdir(RUN, 0755);

    char *boot[] = {"boot", NULL};
    run_ctl(boot, 2);
    g_mirror_started = 0;
    reconcile();

    /* EXT display not found at boot: look again during the first minute, in
     * case its power or SPI bus came up late. Stops once it is found. */
    static const int retry_at[] = {3, 6, 10, 15, 20, 30, 45, 60};
    const int nretry = sizeof retry_at / sizeof retry_at[0];
    time_t boot_t = time(NULL);
    int ri = 0;

    int s = socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0);
    struct sockaddr_un a = {.sun_family = AF_UNIX};
    snprintf(a.sun_path, sizeof a.sun_path, "%s", SOCK);
    unlink(SOCK);
    if (s < 0 || bind(s, (struct sockaddr *)&a, sizeof a) != 0 || listen(s, 8) != 0) {
        perror("displaymod-daemon: socket");
        return 1;
    }
    chmod(SOCK, 0666);

    while (!g_stop) {
        struct pollfd p = {s, POLLIN, 0};
        int pr = poll(&p, 1, 1000);
        if (pr > 0 && (p.revents & POLLIN)) {
            int c = accept4(s, NULL, NULL, SOCK_CLOEXEC);
            if (c >= 0) { handle(c); close(c); }
        }
        bigterm_check();
        ui_check();
        reconcile();
        if (ri < nretry && time(NULL) - boot_t >= retry_at[ri]) {
            ri++;
            if (access(RUN "/notfound", F_OK) == 0) {
                char *retry[] = {"retry", NULL};
                run_ctl(retry, 2);
                reconcile();
            } else ri = nretry;         /* found, or mirroring is off */
        }
    }

    bigterm_off();
    stop_mirror();
    char *shut[] = {"shutdown", NULL};
    run_ctl(shut, 2);
    unlink(SOCK);
    return 0;
}
