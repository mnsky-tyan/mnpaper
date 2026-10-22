/* test_tray - behavioural test for the tray's DBusMenu event path.
 *
 * The tray is the part of the port a panel actually talks to, so the test
 * does the same: it starts a private dbus-daemon, runs the real tray code
 * on it, and drives it with the very messages a panel sends.
 *
 * The property under test is the one that fails silently: a batch the tray
 * refuses must change nothing.  A tray that applies the first events of an
 * over-long batch and only then reports the error leaves the menu and the
 * settings out of sync, and the host has no way to tell.
 *
 *   make test   (from linux/)
 */

#define _GNU_SOURCE

#include <errno.h>
#include <poll.h>
#include <signal.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/wait.h>

#include <systemd/sd-bus.h>

#include "../tray.h"

static int failures;

#define CHECK(cond, msg) do {                                                  \
        if (!(cond)) {                                                         \
            printf("FAIL %s (line %d): %s\n", __func__, __LINE__, msg);        \
            failures++;                                                        \
        }                                                                      \
    } while (0)

/* The tray logs through the core's logger, which lives in the daemon's
 * translation unit; the test brings its own so the log is visible while a
 * case runs. */
void PAPERLOG(const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    vfprintf(stderr, fmt, ap);
    va_end(ap);
    fputc('\n', stderr);
}

/* The state the menu reflects, plus a count per callback, so a case can
 * tell "applied" from "not applied". */
static int st_master = 1, st_intensity = 30, st_warmth = 45, st_autostart;
static int n_show, n_intensity, n_warmth, n_autostart;

static void cb_toggle_show(void) { n_show++; st_master = !st_master; }
static void cb_set_intensity(int v) { n_intensity++; st_intensity = v; }
static void cb_set_warmth(int v) { n_warmth++; st_warmth = v; }
static void cb_toggle_autostart(void) { n_autostart++; st_autostart = !st_autostart; }
static void cb_edit_settings(void) { }
static void cb_quit(void) { }
static int cb_query_master(void) { return st_master; }
static int cb_query_intensity(void) { return st_intensity; }
static int cb_query_warmth(void) { return st_warmth; }
static int cb_query_autostart(void) { return st_autostart; }

static const struct TRAYCB g_cb = {
    cb_toggle_show, cb_set_intensity, cb_set_warmth, cb_toggle_autostart,
    cb_edit_settings, cb_quit,
    cb_query_master, cb_query_intensity, cb_query_warmth, cb_query_autostart
};

/* Menu ids the test uses: the "Show paper" toggle and the first strength
 * radio, plus one id the model does not have. */
#define ID_SHOW 1
#define ID_STRENGTH_FIRST 31
#define ID_UNKNOWN 999

/* A private session bus, so the test never touches the user's session.
 * Returns the bus daemon's pid, or -1 when there is none to start. */
static pid_t start_private_bus(char *addr, size_t addr_sz) {
    int fds[2];
    pid_t pid;
    size_t got = 0;

    if (pipe(fds) < 0) return -1;
    pid = fork();
    if (pid < 0) { close(fds[0]); close(fds[1]); return -1; }
    if (pid == 0) {
        close(fds[0]);
        dup2(fds[1], STDOUT_FILENO);
        close(fds[1]);
        execlp("dbus-daemon", "dbus-daemon", "--session", "--nofork",
               "--print-address", (char *)NULL);
        _exit(127);
    }
    close(fds[1]);
    while (got + 1 < addr_sz) {
        ssize_t n = read(fds[0], addr + got, addr_sz - 1 - got);
        if (n <= 0) break;
        got += (size_t)n;
        if (memchr(addr, '\n', got)) break;
    }
    close(fds[0]);
    if (!got) {
        kill(pid, SIGTERM);
        waitpid(pid, NULL, 0);
        return -1;
    }
    addr[got] = 0;
    addr[strcspn(addr, "\r\n")] = 0;
    setenv("DBUS_SESSION_BUS_ADDRESS", addr, 1);
    return pid;
}

/* Answer of one call, as one line: "ERR <error-name>" when the tray
 * refused, "OK <count> <id> ..." when it answered. */
static void answer(int fd, const char *line) {
    size_t n = strlen(line);
    ssize_t w = write(fd, line, n);
    (void)w;
}

/* The ids the tray could not apply, read out of an "ai" reply. */
static int read_id_errors(sd_bus_message *reply, int *out, int outsz) {
    int n = 0, r;
    r = sd_bus_message_enter_container(reply, SD_BUS_TYPE_ARRAY, "i");
    if (r <= 0) return 0;
    for (;;) {
        int32_t v;
        r = sd_bus_message_read(reply, "i", &v);
        if (r <= 0) break;
        if (n < outsz) out[n++] = (int)v;
    }
    sd_bus_message_exit_container(reply);
    return n;
}

static void client_eventgroup(const char *dest, int n, int fd) {
    sd_bus *c = NULL;
    sd_bus_message *m = NULL, *reply = NULL;
    sd_bus_error err = SD_BUS_ERROR_NULL;
    char line[512];
    int iderr[64];
    int r, i, count = 0, used;

    if (sd_bus_open_user(&c) < 0) { answer(fd, "ERR noconn\n"); return; }
    sd_bus_set_method_call_timeout(c, 5u * 1000000u);
    if (sd_bus_message_new_method_call(c, &m, dest, "/StatusNotifierItem",
                                        "com.canonical.dbusmenu", "EventGroup") < 0) {
        answer(fd, "ERR nomessage\n");
        return;
    }
    sd_bus_message_open_container(m, SD_BUS_TYPE_ARRAY, "(isvu)");
    for (i = 0; i < n; i++) {
        int32_t id = ID_SHOW;
        if (i == 0) id = ID_STRENGTH_FIRST;   /* sets the strength */
        else if (i == 1) id = ID_UNKNOWN;      /* not in the model */
        sd_bus_message_open_container(m, SD_BUS_TYPE_STRUCT, "isvu");
        sd_bus_message_append(m, "i", id);
        sd_bus_message_append(m, "s", "clicked");
        sd_bus_message_open_container(m, SD_BUS_TYPE_VARIANT, "s");
        sd_bus_message_append(m, "s", "payload");
        sd_bus_message_close_container(m);
        sd_bus_message_append(m, "u", 0);
        sd_bus_message_close_container(m);
    }
    sd_bus_message_close_container(m);

    r = sd_bus_call(c, m, 0, &err, &reply);
    if (r < 0) {
        snprintf(line, sizeof line, "ERR %s\n",
                 err.name ? err.name : "unknown");
        answer(fd, line);
    } else {
        if (reply) count = read_id_errors(reply, iderr, (int)(sizeof iderr / sizeof iderr[0]));
        used = snprintf(line, sizeof line, "OK %d", count);
        for (i = 0; i < count; i++) {
            int k;
            if ((size_t)used + 16 >= sizeof line) break;
            k = snprintf(line + used, sizeof line - (size_t)used, " %d", iderr[i]);
            if (k < 0) break;
            used += k;
        }
        if ((size_t)used + 2 <= sizeof line) {
            line[used++] = '\n';
            line[used] = '\0';
        }
        answer(fd, line);
    }
    sd_bus_error_free(&err);
    if (reply) sd_bus_message_unref(reply);
    sd_bus_message_unref(m);
    sd_bus_unref(c);
}

static void client_groupprops(const char *dest, int n, int fd) {
    sd_bus *c = NULL;
    sd_bus_message *m = NULL, *reply = NULL;
    sd_bus_error err = SD_BUS_ERROR_NULL;
    char line[512];
    int r, i;

    if (sd_bus_open_user(&c) < 0) { answer(fd, "ERR noconn\n"); return; }
    sd_bus_set_method_call_timeout(c, 5u * 1000000u);
    if (sd_bus_message_new_method_call(c, &m, dest, "/StatusNotifierItem",
                                        "com.canonical.dbusmenu",
                                        "GetGroupProperties") < 0) {
        answer(fd, "ERR nomessage\n");
        return;
    }
    sd_bus_message_open_container(m, SD_BUS_TYPE_ARRAY, "i");
    for (i = 0; i < n; i++) sd_bus_message_append(m, "i", ID_SHOW);
    sd_bus_message_close_container(m);
    sd_bus_message_open_container(m, SD_BUS_TYPE_ARRAY, "s");   /* no filter */
    sd_bus_message_close_container(m);

    r = sd_bus_call(c, m, 0, &err, &reply);
    if (r < 0) {
        snprintf(line, sizeof line, "ERR %s\n",
                 err.name ? err.name : "unknown");
        answer(fd, line);
    } else {
        answer(fd, "OK 0\n");
    }
    sd_bus_error_free(&err);
    if (reply) sd_bus_message_unref(reply);
    sd_bus_message_unref(m);
    sd_bus_unref(c);
}

/* Send one call from a forked child - the call blocks until the tray
 * answers, and the tray is served from this process - and collect the
 * answer.  The tray's bus is pumped while the child waits. */
static int call_tray(const char *dest, const char *method, int n,
                     char *out, size_t outsz) {
    int fds[2];
    pid_t pid;
    size_t got = 0;
    int waited = 0;

    if (pipe(fds) < 0) return -1;
    pid = fork();
    if (pid < 0) { close(fds[0]); close(fds[1]); return -1; }
    if (pid == 0) {
        close(fds[0]);
        if (strcmp(method, "EventGroup") == 0)
            client_eventgroup(dest, n, fds[1]);
        else
            client_groupprops(dest, n, fds[1]);
        close(fds[1]);
        _exit(0);
    }
    close(fds[1]);
    for (;;) {
        struct pollfd pfd;
        int r;
        pfd.fd = fds[0];
        pfd.events = POLLIN;
        pfd.revents = 0;
        r = poll(&pfd, 1, 20);
        if (r > 0) break;
        if (r < 0 && errno != EINTR) break;
        tray_process();     /* serve the call the child is waiting for */
        if (++waited > 1500) break;    /* 30 s: the child's own call times out first */
    }
    while (got + 1 < outsz) {
        ssize_t k = read(fds[0], out + got, outsz - 1 - got);
        if (k <= 0) break;
        got += (size_t)k;
    }
    out[got] = 0;
    close(fds[0]);
    waitpid(pid, NULL, 0);
    return (int)got;
}

static void reset_counters(void) {
    n_show = n_intensity = n_warmth = n_autostart = 0;
    st_master = 1;
    st_intensity = 30;
    st_warmth = 45;
    st_autostart = 0;
}

int main(void) {
    char addr[512], out[1024], dest[128];
    pid_t bus_pid;

    bus_pid = start_private_bus(addr, sizeof addr);
    if (bus_pid < 0) {
        printf("SKIP: no private session bus (dbus-daemon unavailable)\n");
        return 0;
    }
    if (tray_start(&g_cb) < 0) {
        CHECK(0, "tray: tray_start failed on the private bus");
        kill(bus_pid, SIGTERM);
        waitpid(bus_pid, NULL, 0);
        return 1;
    }
    /* the tray ignores SIGCHLD for its fire-and-forget child; the test
     * waits for its own children, so the default comes back */
    signal(SIGCHLD, SIG_DFL);
    snprintf(dest, sizeof dest, "org.kde.StatusNotifierItem-%d-1",
             (int)getpid());

    /* a batch at the bound is applied in full */
    reset_counters();
    call_tray(dest, "EventGroup", 256, out, sizeof out);
    CHECK(strncmp(out, "OK", 2) == 0, "eventgroup: a batch at the bound is answered");
    CHECK(n_intensity == 1, "eventgroup: the strength event was applied");
    CHECK(n_show == 254, "eventgroup: every show event was applied");
    CHECK(st_intensity == 10, "eventgroup: the strength radio took effect");
    CHECK(strstr(out, "999") != NULL, "eventgroup: the unknown id is reported");

    /* a batch over the bound is refused and changes nothing */
    reset_counters();
    call_tray(dest, "EventGroup", 257, out, sizeof out);
    CHECK(strstr(out, "org.freedesktop.DBus.Error.LimitsExceeded") != NULL,
          "eventgroup: a batch over the bound is refused");
    CHECK(n_show == 0 && n_intensity == 0 && n_warmth == 0 && n_autostart == 0,
          "eventgroup: a refused batch applies nothing");
    CHECK(st_intensity == 30 && st_warmth == 45 && st_master == 1,
          "eventgroup: a refused batch leaves the settings alone");

    /* the sibling call refuses over the bound too, and answers at it */
    call_tray(dest, "GetGroupProperties", 257, out, sizeof out);
    CHECK(strstr(out, "org.freedesktop.DBus.Error.LimitsExceeded") != NULL,
          "groupprops: a batch over the bound is refused");
    call_tray(dest, "GetGroupProperties", 256, out, sizeof out);
    CHECK(strncmp(out, "OK", 2) == 0,
          "groupprops: a batch at the bound is answered");

    kill(bus_pid, SIGTERM);
    waitpid(bus_pid, NULL, 0);
    printf("%s: %d failure(s)\n", failures ? "RESULT" : "RESULT 0", failures);
    return failures ? 1 : 0;
}
