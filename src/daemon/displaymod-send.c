/*
 * displaymod-send - ask the DisplayMOD service to do something.
 *
 *   displaymod-send status
 *   displaymod-send set BRIGHTNESS 60
 *
 * Used by the DisplayMOD app, the displaymod menu and BigTermCLI in place of
 * "sudo displaymod-ctl". Prints the reply and exits with its code.
 */
#define _GNU_SOURCE
#include <stdio.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>

#define SOCK "/run/displaymod/ctl.sock"

int main(int argc, char **argv)
{
    if (argc < 2) { fprintf(stderr, "usage: displaymod-send COMMAND [ARGS...]\n"); return 2; }
    int s = socket(AF_UNIX, SOCK_STREAM, 0);
    struct sockaddr_un a = {.sun_family = AF_UNIX};
    snprintf(a.sun_path, sizeof a.sun_path, "%s", SOCK);
    if (s < 0 || connect(s, (struct sockaddr *)&a, sizeof a) != 0) {
        fprintf(stderr, "DisplayMOD service is not running (systemctl status displaymod)\n");
        return 125;
    }
    for (int i = 1; i < argc; i++)
        if (write(s, argv[i], strlen(argv[i]) + 1) < 0) return 125;
    shutdown(s, SHUT_WR);

    /* Pass the output through, holding back the 2-byte trailer (NUL, code). */
    char buf[4096 + 2], hold[2];
    int held = 0;
    ssize_t r;
    while ((r = read(s, buf + held, sizeof buf - 2)) > 0) {
        memcpy(buf, hold, held);                  /* held bytes go in front */
        size_t total = held + (size_t)r;
        size_t out = total > 2 ? total - 2 : 0;
        if (out && fwrite(buf, 1, out, stdout)) {}
        held = (int)(total - out);
        memcpy(hold, buf + out, held);
    }
    fflush(stdout);
    if (held < 2 || hold[0] != 0) return 124;   /* service went away mid-reply */
    return (unsigned char)hold[1];
}
