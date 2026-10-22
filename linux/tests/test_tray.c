/* test_tray - behavioural test for the tray's bus contract.
 *
 * The tray is the part of the port a panel actually talks to, so the test
 * does the same: it starts a private dbus-daemon, runs the real tray code
 * on it, and drives it with the very messages a panel sends.
 *
 * Two properties are under test, and both fail silently:
 *
 *   - a batch the tray refuses must change nothing.  A tray that applies
 *     the first events of an over-long batch and only then reports the
 *     error leaves the menu and the settings out of sync, and the host has
 *     no way to tell.
 *   - the signals the tray pushes must carry the shape the DBusMenu spec
 *     declares, and must actually arrive.  A wrong element type is dropped
 *     by every typed consumer, and a property change that never leaves the
 *     process leaves a stale icon behind.
 *
 *   make test   (from linux/)
 */

#define _GNU_SOURCE

#include <errno.h>
#include <ftw.h>
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

/* --------------------------------------------------- the panel's side --- */

/* A second connection on the private bus, playing the part of a panel: it
 * subscribes to the signals the tray pushes and records what actually
 * arrives.  The two things under test fail SILENTLY: a wrong element type
 * in ItemsPropertiesUpdated is dropped by every typed consumer (the spec
 * says a(ia{sv})a(ias) and libdbusmenu-qt5 reads that exact shape), and a
 * PropertiesChanged that never leaves the process leaves the host with a
 * stale icon.  The only place either can be caught is the wire. */
static sd_bus *subbus;
static int got_props, got_items, got_layout;
static char items_sig[64], layout_sig[32], status_value[32];
static char props_iface[64];

/* PropertiesChanged is a signal of the standard properties interface - that
 * is where the D-Bus specification puts it, where sd-bus emits it and where
 * a host looks for it (the Plasma system tray subscribes to
 * org.freedesktop.DBus.Properties.PropertiesChanged).  The body's first
 * element names the interface whose properties changed. */
#define MATCH_PROPS \
    "type='signal',interface='org.freedesktop.DBus.Properties'," \
    "member='PropertiesChanged'"
#define MATCH_ITEMS \
    "type='signal',interface='com.canonical.dbusmenu'," \
    "member='ItemsPropertiesUpdated'"
#define MATCH_LAYOUT \
    "type='signal',interface='com.canonical.dbusmenu',member='LayoutUpdated'"

/* PropertiesChanged: sa{sv}as.  The changed properties carry the Status
 * value the host shows, so the test reads it off the wire. */
static int on_props_changed(sd_bus_message *m, void *ud, sd_bus_error *err) {
    const char *iface = NULL;
    (void)ud; (void)err;
    got_props++;
    if (sd_bus_message_read(m, "s", &iface) < 0) return 0;
    if (iface) snprintf(props_iface, sizeof props_iface, "%s", iface);
    if (sd_bus_message_enter_container(m, SD_BUS_TYPE_ARRAY, "{sv}") < 0) return 0;
    for (;;) {
        const char *key = NULL;
        int r = sd_bus_message_enter_container(m, SD_BUS_TYPE_DICT_ENTRY, "sv");
        if (r <= 0) break;
        if (sd_bus_message_read(m, "s", &key) >= 0 &&
            key && !strcmp(key, "Status")) {
            /* the value is a variant holding the status string */
            if (sd_bus_message_enter_container(m, SD_BUS_TYPE_VARIANT, "s") >= 0) {
                const char *s = NULL;
                if (sd_bus_message_read(m, "s", &s) >= 0 && s)
                    snprintf(status_value, sizeof status_value, "%s", s);
                sd_bus_message_exit_container(m);
            } else {
                sd_bus_message_skip(m, "v");
            }
        } else {
            sd_bus_message_skip(m, "v");
        }
        sd_bus_message_exit_container(m);
    }
    sd_bus_message_exit_container(m);
    /* the invalidated properties, always empty for this tray */
    if (sd_bus_message_enter_container(m, SD_BUS_TYPE_ARRAY, "s") >= 0) {
        for (;;) {
            const char *ignored = NULL;
            if (sd_bus_message_read(m, "s", &ignored) <= 0) break;
        }
        sd_bus_message_exit_container(m);
    }
    return 0;
}

/* ItemsPropertiesUpdated: the body signature is the whole point, so it is
 * read off the wire instead of being assumed. */
static int on_items_updated(sd_bus_message *m, void *ud, sd_bus_error *err) {
    const char *sig;
    (void)ud; (void)err;
    got_items++;
    sig = sd_bus_message_get_signature(m, 1);
    if (sig) snprintf(items_sig, sizeof items_sig, "%s", sig);
    return 0;
}

static int on_layout_updated(sd_bus_message *m, void *ud, sd_bus_error *err) {
    const char *sig;
    (void)ud; (void)err;
    got_layout++;
    sig = sd_bus_message_get_signature(m, 1);
    if (sig) snprintf(layout_sig, sizeof layout_sig, "%s", sig);
    return 0;
}

/* Pump the tray's bus and the subscriber's bus until the three signals the
 * tray pushes have arrived, or the deadline passes. */
static void wait_signals(int ms) {
    int waited = 0;
    while (!(got_props && got_items && got_layout) && waited < ms) {
        tray_process();
        if (subbus) sd_bus_process(subbus, NULL);
        usleep(5000);
        waited += 5;
    }
}

/* One item of a GetLayout reply. */
struct item {
    int32_t id;
    int nprops;
    int nchildren;
    char label[128];
};

static int read_item(sd_bus_message *m, struct item *it, struct item *first);

/* The "a{sv}" property map of an item: counts the properties, keeps the
 * label. */
static int read_props(sd_bus_message *m, struct item *it) {
    int r, n = 0;
    r = sd_bus_message_enter_container(m, SD_BUS_TYPE_ARRAY, "{sv}");
    if (r < 0) return r;
    for (;;) {
        const char *key = NULL;
        int e = sd_bus_message_enter_container(m, SD_BUS_TYPE_DICT_ENTRY, "sv");
        if (e <= 0) break;
        if (sd_bus_message_read(m, "s", &key) >= 0) {
            if (key && !strcmp(key, "label")) {
                if (sd_bus_message_enter_container(m, SD_BUS_TYPE_VARIANT, "s") >= 0) {
                    const char *s = NULL;
                    if (sd_bus_message_read(m, "s", &s) >= 0 && s)
                        snprintf(it->label, sizeof it->label, "%s", s);
                    sd_bus_message_exit_container(m);
                } else {
                    sd_bus_message_skip(m, "v");
                }
            } else {
                sd_bus_message_skip(m, "v");
            }
            n++;
        }
        sd_bus_message_exit_container(m);
    }
    sd_bus_message_exit_container(m);
    it->nprops = n;
    return 0;
}

/* The "av" child list of an item: counts the children and keeps the first
 * one, which is the row the caller looks at. */
static int read_children(sd_bus_message *m, struct item *it, struct item *first) {
    int r, n = 0;
    r = sd_bus_message_enter_container(m, SD_BUS_TYPE_ARRAY, "v");
    if (r < 0) return r;
    for (;;) {
        struct item child;
        int e = sd_bus_message_enter_container(m, SD_BUS_TYPE_VARIANT,
                                               "(ia{sv}av)");
        if (e <= 0) break;
        if (read_item(m, &child, NULL) < 0) {
            sd_bus_message_exit_container(m);
            sd_bus_message_exit_container(m);
            return -1;
        }
        sd_bus_message_exit_container(m);
        if (first && n == 0) *first = child;
        n++;
    }
    sd_bus_message_exit_container(m);
    it->nchildren = n;
    return 0;
}

/* One "(ia{sv}av)" item.  The children are read too, so the message is
 * left at the next sibling when the call returns. */
static int read_item(sd_bus_message *m, struct item *it, struct item *first) {
    int r;
    memset(it, 0, sizeof *it);
    it->id = -1;
    r = sd_bus_message_enter_container(m, SD_BUS_TYPE_STRUCT, "ia{sv}av");
    if (r <= 0) return r < 0 ? r : -1;
    r = sd_bus_message_read(m, "i", &it->id);
    if (r >= 0) r = read_props(m, it);
    if (r >= 0) r = read_children(m, it, first);
    sd_bus_message_exit_container(m);
    return r < 0 ? r : 0;
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

/* The left click: the SNI Activate method. */
static void client_activate(const char *dest, int fd) {
    sd_bus *c = NULL;
    sd_bus_message *m = NULL, *reply = NULL;
    sd_bus_error err = SD_BUS_ERROR_NULL;
    char line[256];
    int r;

    if (sd_bus_open_user(&c) < 0) { answer(fd, "ERR noconn\n"); return; }
    sd_bus_set_method_call_timeout(c, 5u * 1000000u);
    if (sd_bus_message_new_method_call(c, &m, dest, "/StatusNotifierItem",
                                        "org.kde.StatusNotifierItem",
                                        "Activate") < 0) {
        answer(fd, "ERR nomessage\n");
        return;
    }
    sd_bus_message_append(m, "ii", 0, 0);
    r = sd_bus_call(c, m, 0, &err, &reply);
    if (r < 0) {
        snprintf(line, sizeof line, "ERR %s\n",
                 err.name ? err.name : "unknown");
        answer(fd, line);
    } else {
        answer(fd, "OK\n");
    }
    sd_bus_error_free(&err);
    if (reply) sd_bus_message_unref(reply);
    sd_bus_message_unref(m);
    sd_bus_unref(c);
}

/* One menu click: the DBusMenu Event method with the "clicked" event. */
static void client_event(const char *dest, int id, int fd) {
    sd_bus *c = NULL;
    sd_bus_message *m = NULL, *reply = NULL;
    sd_bus_error err = SD_BUS_ERROR_NULL;
    char line[256];
    int r;

    if (sd_bus_open_user(&c) < 0) { answer(fd, "ERR noconn\n"); return; }
    sd_bus_set_method_call_timeout(c, 5u * 1000000u);
    if (sd_bus_message_new_method_call(c, &m, dest, "/StatusNotifierItem",
                                        "com.canonical.dbusmenu", "Event") < 0) {
        answer(fd, "ERR nomessage\n");
        return;
    }
    sd_bus_message_append(m, "i", id);
    sd_bus_message_append(m, "s", "clicked");
    sd_bus_message_open_container(m, SD_BUS_TYPE_VARIANT, "s");
    sd_bus_message_append(m, "s", "payload");
    sd_bus_message_close_container(m);
    sd_bus_message_append(m, "u", 0);
    r = sd_bus_call(c, m, 0, &err, &reply);
    if (r < 0) {
        snprintf(line, sizeof line, "ERR %s\n",
                 err.name ? err.name : "unknown");
        answer(fd, line);
    } else {
        answer(fd, "OK\n");
    }
    sd_bus_error_free(&err);
    if (reply) sd_bus_message_unref(reply);
    sd_bus_message_unref(m);
    sd_bus_unref(c);
}

/* The menu itself: the DBusMenu GetLayout reply, root id, one level deep.
 * The answer carries the facts the parent asserts on, so the reply is
 * parsed where it arrives. */
static void client_getlayout(const char *dest, int fd) {
    sd_bus *c = NULL;
    sd_bus_message *m = NULL, *reply = NULL;
    sd_bus_error err = SD_BUS_ERROR_NULL;
    char line[512];
    uint32_t rev = 0;
    struct item root, first;
    int r;

    if (sd_bus_open_user(&c) < 0) { answer(fd, "ERR noconn\n"); return; }
    sd_bus_set_method_call_timeout(c, 5u * 1000000u);
    if (sd_bus_message_new_method_call(c, &m, dest, "/StatusNotifierItem",
                                        "com.canonical.dbusmenu",
                                        "GetLayout") < 0) {
        answer(fd, "ERR nomessage\n");
        return;
    }
    sd_bus_message_append(m, "ii", 0, 1);        /* root, one level deep */
    sd_bus_message_open_container(m, SD_BUS_TYPE_ARRAY, "s");  /* no filter */
    sd_bus_message_close_container(m);

    r = sd_bus_call(c, m, 0, &err, &reply);
    if (r < 0) {
        snprintf(line, sizeof line, "ERR %s\n",
                 err.name ? err.name : "unknown");
        answer(fd, line);
    } else if (sd_bus_message_read(reply, "u", &rev) < 0 ||
               read_item(reply, &root, &first) < 0) {
        answer(fd, "ERR badreply\n");
    } else {
        snprintf(line, sizeof line, "OK %u %d %d %d %d \"%s\"\n", rev,
                 (int)root.id, root.nprops, root.nchildren, (int)first.id,
                 first.label[0] ? first.label : "-");
        answer(fd, line);
    }
    sd_bus_error_free(&err);
    if (reply) sd_bus_message_unref(reply);
    sd_bus_message_unref(m);
    sd_bus_unref(c);
}

/* One property read: what the host asks the item before it routes a click.
 * ItemIsMenu decides whether a left click is an Activate or a menu, so a
 * wrong answer there makes the left click silently do nothing. */
static void client_prop(const char *dest, const char *prop, int fd) {
    sd_bus *c = NULL;
    sd_bus_message *m = NULL, *reply = NULL;
    sd_bus_error err = SD_BUS_ERROR_NULL;
    char line[256];
    int r;

    if (sd_bus_open_user(&c) < 0) { answer(fd, "ERR noconn\n"); return; }
    sd_bus_set_method_call_timeout(c, 5u * 1000000u);
    if (sd_bus_message_new_method_call(c, &m, dest, "/StatusNotifierItem",
                                        "org.freedesktop.DBus.Properties",
                                        "Get") < 0) {
        answer(fd, "ERR nomessage\n");
        return;
    }
    sd_bus_message_append(m, "s", "org.kde.StatusNotifierItem");
    sd_bus_message_append(m, "s", prop);
    r = sd_bus_call(c, m, 0, &err, &reply);
    if (r < 0) {
        snprintf(line, sizeof line, "ERR %s\n",
                 err.name ? err.name : "unknown");
        answer(fd, line);
    } else if (sd_bus_message_enter_container(reply, SD_BUS_TYPE_VARIANT, "b") >= 0) {
        int b = -1;
        if (sd_bus_message_read(reply, "b", &b) < 0) b = -1;
        sd_bus_message_exit_container(reply);
        snprintf(line, sizeof line, "OK %d\n", b);
        answer(fd, line);
    } else {
        answer(fd, "ERR badreply\n");
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
        /* the parent's bus connections are not the child's business: a
         * child that read from one of them would take a signal the parent
         * is waiting for away from it */
        if (subbus) { int f = sd_bus_get_fd(subbus); if (f >= 0) close(f); }
        { int f = tray_fd(); if (f >= 0) close(f); }
        if (strcmp(method, "EventGroup") == 0)
            client_eventgroup(dest, n, fds[1]);
        else if (strcmp(method, "GetGroupProperties") == 0)
            client_groupprops(dest, n, fds[1]);
        else if (strcmp(method, "Activate") == 0)
            client_activate(dest, fds[1]);
        else if (strcmp(method, "Event") == 0)
            client_event(dest, n, fds[1]);
        else if (strcmp(method, "GetLayout") == 0)
            client_getlayout(dest, fds[1]);
        else if (strcmp(method, "ItemIsMenu") == 0)
            client_prop(dest, "ItemIsMenu", fds[1]);
        else
            answer(fds[1], "ERR unknown\n");
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

/* A throwaway XDG environment: the test must not read the user's real
 * settings and must not put anything into the live daemon's runtime
 * directory. */
static char xdg_dir[320];

static void private_xdg(void) {
    const char *tmp = getenv("TMPDIR");
    if (!tmp || !*tmp) tmp = "/tmp";
    snprintf(xdg_dir, sizeof xdg_dir, "%s/mnpaper-tray-test-XXXXXX", tmp);
    if (!mkdtemp(xdg_dir)) { xdg_dir[0] = 0; return; }
    setenv("XDG_RUNTIME_DIR", xdg_dir, 1);
    setenv("XDG_CONFIG_HOME", xdg_dir, 1);
}

/* The throwaway tree is not empty when the test is done: the private bus
 * puts its machine-id and its lock file under $XDG_RUNTIME_DIR, so the
 * whole tree goes, not just the directory. */
static int remove_entry(const char *path, const struct stat *sb, int typeflag,
                        struct FTW *ftwbuf) {
    (void)sb; (void)typeflag; (void)ftwbuf;
    return remove(path);
}

static void private_xdg_cleanup(void) {
    if (!xdg_dir[0]) return;
    nftw(xdg_dir, remove_entry, 8, FTW_DEPTH | FTW_PHYS);
    xdg_dir[0] = 0;
}

int main(void) {
    char addr[512], out[1024], dest[128];
    pid_t bus_pid;

    private_xdg();
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

    /* the panel's side of the wire: a second connection that subscribes to
     * the signals the tray pushes */
    if (sd_bus_open_user(&subbus) < 0) {
        CHECK(0, "subscriber: no second connection to the private bus");
        kill(bus_pid, SIGTERM);
        waitpid(bus_pid, NULL, 0);
        return 1;
    }
    sd_bus_add_match(subbus, NULL, MATCH_PROPS, on_props_changed, NULL);
    sd_bus_add_match(subbus, NULL, MATCH_ITEMS, on_items_updated, NULL);
    sd_bus_add_match(subbus, NULL, MATCH_LAYOUT, on_layout_updated, NULL);

    /* the left click: the host asks the item what it is before it routes
     * the click - an item that claims to be a menu never gets an Activate */
    reset_counters();
    call_tray(dest, "ItemIsMenu", 0, out, sizeof out);
    CHECK(strncmp(out, "OK 0", 4) == 0,
          "sni: the item is no menu, so a left click activates it");

    /* the left click: Activate */
    reset_counters();
    call_tray(dest, "Activate", 0, out, sizeof out);
    CHECK(strncmp(out, "OK", 2) == 0, "activate: the call is answered");
    CHECK(n_show == 1, "activate: the paper was toggled");
    CHECK(st_master == 0, "activate: the paper is off after the toggle");

    /* a menu click: Event with the "clicked" event */
    reset_counters();
    call_tray(dest, "Event", ID_SHOW, out, sizeof out);
    CHECK(strncmp(out, "OK", 2) == 0, "event: the call is answered");
    CHECK(n_show == 1, "event: a 'clicked' event toggles the paper");
    CHECK(st_master == 0, "event: the toggle took effect");

    reset_counters();
    call_tray(dest, "Event", ID_STRENGTH_FIRST, out, sizeof out);
    CHECK(strncmp(out, "OK", 2) == 0, "event: a radio row is answered");
    CHECK(n_intensity == 1, "event: a 'clicked' event on a radio sets the strength");
    CHECK(st_intensity == 10, "event: the radio took effect");

    /* the menu itself */
    {
        unsigned rev = 0;
        int rootid = -1, nprops = -1, nchildren = -1, firstid = -1;
        char label[128] = "";
        reset_counters();
        call_tray(dest, "GetLayout", 0, out, sizeof out);
        CHECK(sscanf(out, "OK %u %d %d %d %d \"%127[^\"]\"", &rev, &rootid,
                     &nprops, &nchildren, &firstid, label) == 6,
              "layout: the reply carries the root item");
        CHECK(rootid == 0, "layout: the root item is id 0");
        CHECK(nprops > 0, "layout: the root item carries its properties");
        CHECK(nchildren == 9, "layout: the root carries every row of the menu");
        CHECK(firstid == ID_SHOW, "layout: the first row is the Show paper toggle");
        CHECK(strcmp(label, "Show paper") == 0,
              "layout: the first row carries its label");
        CHECK(rev >= 1, "layout: the reply carries the menu revision");
    }

    /* the signals a host depends on: the tray's only push channel, and
     * both defects fail silently - a wrong element type is dropped by
     * every typed consumer, and a PropertiesChanged that never leaves the
     * process leaves a stale icon behind */
    reset_counters();
    got_props = got_items = got_layout = 0;
    items_sig[0] = layout_sig[0] = status_value[0] = props_iface[0] = 0;
    tray_state_changed();
    wait_signals(5000);
    CHECK(got_props == 1, "signals: the tray announced its property change");
    CHECK(strcmp(props_iface, "org.kde.StatusNotifierItem") == 0,
          "signals: the PropertiesChanged names the changed interface");
    CHECK(strcmp(status_value, "Active") == 0,
          "signals: the PropertiesChanged carried the Status property");
    CHECK(got_items == 1, "signals: the tray announced its item properties");
    CHECK(strcmp(items_sig, "a(ia{sv})a(ias)") == 0,
          "signals: ItemsPropertiesUpdated carries the body the spec declares");
    CHECK(got_layout == 1, "signals: the tray announced the new layout");
    CHECK(strcmp(layout_sig, "ui") == 0,
          "signals: LayoutUpdated carries the body the spec declares");

    kill(bus_pid, SIGTERM);
    waitpid(bus_pid, NULL, 0);
    if (subbus) { sd_bus_unref(subbus); subbus = NULL; }
    private_xdg_cleanup();
    printf("%s: %d failure(s)\n", failures ? "RESULT" : "RESULT 0", failures);
    return failures ? 1 : 0;
}
