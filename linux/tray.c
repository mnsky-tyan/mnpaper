/* tray.c - StatusNotifierItem + DBusMenu over sd-bus.  See tray.h. */

#include <errno.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
#include <signal.h>

#include <systemd/sd-bus.h>

#include "tray.h"

/* The core's logger (declared in paper_core.h, defined in mnpaper.c). */
extern void PAPERLOG(const char *fmt, ...);
#define L PAPERLOG

/* ------------------------------------------------------------------ ids --- */

/* Fixed item ids: the menu is rebuilt from live state on every GetLayout, but
 * ids must be stable across revisions or Plasma's cached rows go stale. */
enum {
    ID_ROOT = 0,
    ID_SHOW = 1,
    ID_SEP1 = 2,
    ID_STRENGTH = 3,
    ID_STRENGTH_BASE = 30,      /* + index 0..3 -> 31..34 */
    ID_WARMTH = 4,
    ID_WARMTH_BASE = 40,        /* + index 0..3 -> 41..44 */
    ID_SEP2 = 5,
    ID_AUTOSTART = 6,
    ID_EDITCFG = 7,
    ID_SEP3 = 8,
    ID_QUIT = 9,
    ID_ABOUT = 10,
};

static const char *STRENGTH_NAMES[4] = { "Subtle (10)", "Soft (20)",
                                         "Medium (30)", "Strong (40)" };
static const int STRENGTH_STEPS[4] = { 10, 20, 30, 40 };
static const char *WARMTH_NAMES[4] = { "Cool (10)", "Neutral (45)",
                                       "Warm (75)", "Aged (95)" };
static const int WARMTH_STEPS[4] = { 10, 45, 75, 95 };

static const struct TRAYCB *cb;
static sd_bus *bus;
static char service[96];        /* well-known name requested on the bus */
static uint32_t revision = 1;   /* menu revision: bumped on every change */

/* ------------------------------------------------------------- the icon --- */

/* A 24x24 paper swatch: warm cream base, faint darker grain, thin border.
 * Generated procedurally with the same intent as the texture, so the tray
 * icon reads as mnPaper without shipping a binary asset. */
static unsigned char ICON_PIX[24 * 24 * 4];
static int icon_ready;

/* D-Bus ARGB32 is big-endian per pixel: bytes are A, R, G, B. */
static void IconBuild(void) {
    static const unsigned lh_a = 2246822519u, lh_b = 3266489917u;
    int x, y;
    for (y = 0; y < 24; y++) {
        for (x = 0; x < 24; x++) {
            unsigned char *p = ICON_PIX + 4 * (y * 24 + x);
            unsigned h = (unsigned)(x + 1) * 73856093u ^ (unsigned)(y + 1) * 19349663u;
            h = (h ^ (h >> 15)) * lh_a;
            h ^= h >> 13;
            h *= lh_b;
            h ^= h >> 16;
            int grain = (int)(h % 17u) - 8;      /* -8..+8 shade jitter */
            int r = 245 + grain, g = 238 + grain, b = 224 + grain;
            if (x == 0 || y == 0 || x == 23 || y == 23) {   /* paper edge */
                r -= 28; g -= 30; b -= 34;
            }
            if (r < 0) r = 0;
            if (r > 255) r = 255;
            if (g < 0) g = 0;
            if (g > 255) g = 255;
            if (b < 0) b = 0;
            if (b > 255) b = 255;
            p[0] = 255;          /* A */
            p[1] = (unsigned char)r;
            p[2] = (unsigned char)g;
            p[3] = (unsigned char)b;
        }
    }
    icon_ready = 1;
}

/* ----------------------------------------------------------- menu model --- */

static int StrengthIndex(void) {
    int i, v = cb->query_intensity();
    for (i = 0; i < 4; i++) if (STRENGTH_STEPS[i] == v) return i;
    return -1;
}
static int WarmthIndex(void) {
    int i, v = cb->query_warmth();
    for (i = 0; i < 4; i++) if (WARMTH_STEPS[i] == v) return i;
    return -1;
}

static int MenuChildCount(int id) {
    switch (id) {
    case ID_ROOT: return 9;
    case ID_STRENGTH: return 4;
    case ID_WARMTH: return 4;
    default: return 0;
    }
}
static int MenuChild(int id, int idx) {
    switch (id) {
    case ID_ROOT: {
        static const int root[9] = { ID_SHOW, ID_SEP1, ID_STRENGTH, ID_WARMTH,
                                     ID_SEP2, ID_AUTOSTART, ID_EDITCFG,
                                     ID_SEP3, ID_QUIT };
        return root[idx];
    }
    case ID_STRENGTH: return ID_STRENGTH_BASE + 1 + idx;
    case ID_WARMTH: return ID_WARMTH_BASE + 1 + idx;
    default: return -1;
    }
}

/* One "{sv}" property.  Returns 1 appended, 0 skipped (filtered out or
 * not applicable). */
static int PropAppend(sd_bus_message *m, int id, const char *key,
                      char **names, int nnames) {
    int want = 1, i;
    if (nnames > 0) {
        want = 0;
        for (i = 0; i < nnames; i++)
            if (!strcmp(names[i], key)) { want = 1; break; }
    }
    if (!want) return 0;

    if (!strcmp(key, "type")) {
        int sep = (id == ID_SEP1 || id == ID_SEP2 || id == ID_SEP3);
        sd_bus_message_append(m, "{sv}", "type",
                              "s", sep ? "separator" : "standard");
        return 1;
    }
    if (!strcmp(key, "label")) {
        const char *label = NULL;
        switch (id) {
        case ID_SHOW: label = "Show paper"; break;
        case ID_STRENGTH: label = "Strength"; break;
        case ID_WARMTH: label = "Warmth"; break;
        case ID_AUTOSTART: label = "Start at login"; break;
        case ID_EDITCFG: label = "Edit settings file..."; break;
        case ID_QUIT: label = "Quit"; break;
        case ID_ABOUT: label = "mnPaper (Wayland)"; break;
        default: {
            int si = id - ID_STRENGTH_BASE - 1;
            int wi = id - ID_WARMTH_BASE - 1;
            if (si >= 0 && si < 4) label = STRENGTH_NAMES[si];
            else if (wi >= 0 && wi < 4) label = WARMTH_NAMES[wi];
            break;
        }
        }
        if (!label) return 0;
        sd_bus_message_append(m, "{sv}", "label", "s", label);
        return 1;
    }
    if (!strcmp(key, "enabled")) {
        sd_bus_message_append(m, "{sv}", "enabled", "b", 1);
        return 1;
    }
    if (!strcmp(key, "visible")) {
        sd_bus_message_append(m, "{sv}", "visible", "b", 1);
        return 1;
    }
    if (!strcmp(key, "toggle-type")) {
        const char *t = NULL;
        if (id == ID_SHOW || id == ID_AUTOSTART) t = "checkmark";
        else if ((id > ID_STRENGTH_BASE && id <= ID_STRENGTH_BASE + 4) ||
                 (id > ID_WARMTH_BASE && id <= ID_WARMTH_BASE + 4)) t = "radio";
        if (!t) return 0;
        sd_bus_message_append(m, "{sv}", "toggle-type", "s", t);
        return 1;
    }
    if (!strcmp(key, "toggle-state")) {
        int on = -1;
        if (id == ID_SHOW) on = cb->query_master() ? 1 : 0;
        else if (id == ID_AUTOSTART) on = cb->query_autostart() ? 1 : 0;
        else {
            int si = id - ID_STRENGTH_BASE - 1;
            int wi = id - ID_WARMTH_BASE - 1;
            if (si >= 0 && si < 4) on = (StrengthIndex() == si);
            else if (wi >= 0 && wi < 4) on = (WarmthIndex() == wi);
        }
        sd_bus_message_append(m, "{sv}", "toggle-state", "i", on);
        return 1;
    }
    if (!strcmp(key, "children-display")) {
        if (!MenuChildCount(id)) return 0;
        sd_bus_message_append(m, "{sv}", "children-display", "s", "submenu");
        return 1;
    }
    return 0;
}

/* Serialize one item as a "(ia{sv}av)".  wrap=1 embeds it in a VARIANT (what
 * children inside the "av" array need); wrap=0 appends the bare struct (what
 * the top-level GetLayout reply wants - the spec has no variant there).  At
 * the depth limit (depth == 0) children appear as empty variants, which the
 * spec allows and Plasma renders as stubs on the next fetch. */
static int MenuAppendItem(sd_bus_message *m, int id, int depth, int wrap) {
    int r, i, n = MenuChildCount(id);
    if (wrap) {
        r = sd_bus_message_open_container(m, SD_BUS_TYPE_VARIANT, "(ia{sv}av)");
        if (r < 0) return r;
    }
    r = sd_bus_message_open_container(m, SD_BUS_TYPE_STRUCT, "ia{sv}av");
    if (r < 0) return r;
    sd_bus_message_append(m, "i", id);

    r = sd_bus_message_open_container(m, SD_BUS_TYPE_ARRAY, "{sv}");
    if (r < 0) return r;
    {
        static const char *keys[] = { "type", "label", "enabled", "visible",
                                      "toggle-type", "toggle-state",
                                      "children-display" };
        for (i = 0; i < (int)(sizeof keys / sizeof keys[0]); i++)
            PropAppend(m, id, keys[i], NULL, 0);
    }
    sd_bus_message_close_container(m);

    r = sd_bus_message_open_container(m, SD_BUS_TYPE_ARRAY, "v");
    if (r < 0) return r;
    if (n > 0 && depth != 0) {
        for (i = 0; i < n; i++) {
            r = MenuAppendItem(m, MenuChild(id, i), depth > 0 ? depth - 1 : -1, 1);
            if (r < 0) return r;
        }
    }
    r = sd_bus_message_close_container(m);
    if (r < 0) return r;
    r = sd_bus_message_close_container(m);
    if (r < 0) return r;
    if (wrap) {
        r = sd_bus_message_close_container(m);
        if (r < 0) return r;
    }
    return r;
}

/* ------------------------------------------------------ DBusMenu methods -- */

static int MenuGetLayout(sd_bus_message *msg, void *ud, sd_bus_error *err) {
    int parent, depth, r;
    char **names = NULL;
    int nnames = 0, i;
    sd_bus_message *reply;
    (void)ud;

    r = sd_bus_message_read(msg, "ii", &parent, &depth);
    if (r < 0) return r;
    r = sd_bus_message_enter_container(msg, SD_BUS_TYPE_ARRAY, "s");
    if (r < 0) return r;
    for (;;) {
        const char *s;
        r = sd_bus_message_read(msg, "s", &s);
        if (r < 0) break;
        if (r == 0) break;
        names = realloc(names, (nnames + 1) * sizeof *names);
        if (!names) break;
        names[nnames] = strdup(s);
        if (!names[nnames]) break;
        nnames++;
    }
    sd_bus_message_exit_container(msg);

    r = sd_bus_message_new_method_return(msg, &reply);
    if (r < 0) { for (i = 0; i < nnames; i++) free(names[i]); free(names); return r; }

    r = sd_bus_message_append(reply, "u", revision);
    if (r >= 0) r = MenuAppendItem(reply, parent, depth, 0);
    if (r >= 0) r = sd_bus_send(NULL, reply, NULL);
    sd_bus_message_unref(reply);
    for (i = 0; i < nnames; i++) free(names[i]);
    free(names);
    return r < 0 ? r : 1;
}

static int MenuGetGroupProperties(sd_bus_message *msg, void *ud,
                                  sd_bus_error *err) {
    int32_t *ids = NULL;
    int nids = 0, r, i;
    sd_bus_message *reply;
    (void)ud;

    r = sd_bus_message_enter_container(msg, SD_BUS_TYPE_ARRAY, "i");
    if (r < 0) return r;
    for (;;) {
        int32_t v;
        r = sd_bus_message_read(msg, "i", &v);
        if (r <= 0) break;
        ids = realloc(ids, (nids + 1) * sizeof *ids);
        if (!ids) break;
        ids[nids++] = v;
    }
    sd_bus_message_exit_container(msg);
    /* property-name filter: we always return everything we have; the spec
     * allows a server to return more properties than asked. */

    r = sd_bus_message_new_method_return(msg, &reply);
    if (r < 0) { free(ids); return r; }
    r = sd_bus_message_open_container(reply, SD_BUS_TYPE_ARRAY, "(ia{sv})");
    if (r >= 0) {
        for (i = 0; i < nids; i++) {
            int j;
            r = sd_bus_message_open_container(reply, SD_BUS_TYPE_STRUCT, "ia{sv}");
            if (r < 0) break;
            sd_bus_message_append(reply, "i", ids[i]);
            r = sd_bus_message_open_container(reply, SD_BUS_TYPE_ARRAY, "{sv}");
            if (r < 0) break;
            {
                static const char *keys[] = { "type", "label", "enabled",
                                              "visible", "toggle-type",
                                              "toggle-state",
                                              "children-display" };
                for (j = 0; j < (int)(sizeof keys / sizeof keys[0]); j++)
                    PropAppend(reply, ids[i], keys[j], NULL, 0);
            }
            sd_bus_message_close_container(reply);
            sd_bus_message_close_container(reply);
        }
        sd_bus_message_close_container(reply);
    }
    if (r >= 0) r = sd_bus_send(NULL, reply, NULL);
    sd_bus_message_unref(reply);
    free(ids);
    return r < 0 ? r : 1;
}

static int MenuAboutToShow(sd_bus_message *msg, void *ud, sd_bus_error *err) {
    int32_t id;
    sd_bus_message *reply;
    (void)ud;
    if (sd_bus_message_read(msg, "i", &id) < 0) return -EINVAL;
    if (sd_bus_message_new_method_return(msg, &reply) < 0) return -EINVAL;
    /* We push layout changes with LayoutUpdated, so nothing to flush here. */
    {
        int r;
        sd_bus_message_append(reply, "b", 0);
        r = sd_bus_send(NULL, reply, NULL);
        sd_bus_message_unref(reply);
        return r < 0 ? r : 1;
    }
}

static int MenuEvent(sd_bus_message *msg, void *ud, sd_bus_error *err) {
    int32_t id;
    int rr;
    (void)ud; (void)err;
    const char *event, *data;
    uint32_t ts;
    int r, si, wi;
    (void)ud;

    r = sd_bus_message_read(msg, "issu", &id, &event, &data, &ts);
    if (r < 0) return r;

    if (!strcmp(event, "clicked") || !strcmp(event, "toggled")) {
        switch (id) {
        case ID_SHOW: cb->toggle_show(); break;
        case ID_AUTOSTART: cb->toggle_autostart(); break;
        case ID_EDITCFG: cb->edit_settings(); break;
        case ID_QUIT: cb->quit(); break;
        default:
            si = id - ID_STRENGTH_BASE - 1;
            wi = id - ID_WARMTH_BASE - 1;
            if (si >= 0 && si < 4) cb->set_intensity(STRENGTH_STEPS[si]);
            else if (wi >= 0 && wi < 4) cb->set_warmth(WARMTH_STEPS[wi]);
            break;
        }
    }
    /* empty result: send the reply ourselves (see SniActivate) */
    rr = sd_bus_reply_method_return(msg, NULL);
    if (rr < 0) return rr;
    return 1;
}

static int MenuEventGroup(sd_bus_message *msg, void *ud, sd_bus_error *err) {
    int r;
    (void)ud;
    /* Plasma sends batched events only for keyboard activation; drain them. */
    r = sd_bus_message_enter_container(msg, SD_BUS_TYPE_ARRAY, "(isvu)");
    while (r > 0) {
        int32_t id;
        const char *event, *data;
        uint32_t ts;
        r = sd_bus_message_read(msg, "(isvu)", &id, &event, &data, &ts);
        if (r <= 0) break;
        if (!strcmp(event, "clicked") || !strcmp(event, "toggled")) {
            if (id == ID_SHOW) cb->toggle_show();
            else if (id == ID_AUTOSTART) cb->toggle_autostart();
            else if (id == ID_EDITCFG) cb->edit_settings();
            else if (id == ID_QUIT) cb->quit();
        }
        r = sd_bus_message_exit_container(msg);
    }
    sd_bus_message_exit_container(msg);
    {   /* empty result: send the reply ourselves (see SniActivate) */
        int rr = sd_bus_reply_method_return(msg, NULL);
        if (rr < 0) return rr;
    }
    return 1;
}

/* --------------------------------------------------- StatusNotifierItem --- */

static int SniGetStr(sd_bus *b, const char *path, const char *iface,
                     const char *prop, sd_bus_message *m, void *ud,
                     sd_bus_error *err) {
    const char *v = "";
    (void)b; (void)path; (void)iface; (void)ud; (void)err;
    if (!strcmp(prop, "Id")) v = "mnpaper";
    else if (!strcmp(prop, "Category")) v = "ApplicationStatus";
    else if (!strcmp(prop, "Title")) v = "mnPaper";
    else if (!strcmp(prop, "Status")) v = cb->query_master() ? "Active" : "Passive";
    else if (!strcmp(prop, "IconName")) v = "";
    else if (!strcmp(prop, "Menu")) v = "/StatusNotifierItem";
    return sd_bus_message_append(m, "s", v);
}

static int SniGetMenuPath(sd_bus *b, const char *path, const char *iface,
                          const char *prop, sd_bus_message *m, void *ud,
                          sd_bus_error *err) {
    (void)b; (void)path; (void)iface; (void)prop; (void)ud; (void)err;
    /* Menu is an OBJECT PATH ("o"), not a string - a type mismatch here
     * fails every GetAll, which makes the tray shell retry in a loop. */
    return sd_bus_message_append(m, "o", "/StatusNotifierItem");
}

static int SniGetIconPixmap(sd_bus *b, const char *path, const char *iface,
                            const char *prop, sd_bus_message *m, void *ud,
                            sd_bus_error *err) {
    int r;
    (void)b; (void)path; (void)iface; (void)prop; (void)ud; (void)err;
    if (!icon_ready) IconBuild();
    /* IconPixmap is a(iiay): an ARRAY of (w, h, bytes) icons - one element
     * here.  sd-bus enforces the container walk against the signature, so
     * the containers must follow the type exactly. */
    r = sd_bus_message_open_container(m, SD_BUS_TYPE_ARRAY, "(iiay)");
    if (r < 0) return r;
    r = sd_bus_message_open_container(m, SD_BUS_TYPE_STRUCT, "iiay");
    if (r < 0) return r;
    r = sd_bus_message_append(m, "ii", 24, 24);
    if (r < 0) return r;
    r = sd_bus_message_append_array(m, 'y', ICON_PIX, sizeof ICON_PIX);
    if (r < 0) return r;
    r = sd_bus_message_close_container(m);
    if (r < 0) return r;
    return sd_bus_message_close_container(m);
}

static int SniGetBool(sd_bus *b, const char *path, const char *iface,
                      const char *prop, sd_bus_message *m, void *ud,
                      sd_bus_error *err) {
    (void)b; (void)path; (void)iface; (void)ud; (void)err;
    return sd_bus_message_append(m, "b", strcmp(prop, "ItemIsMenu") == 0 ? 1 : 0);
}

static int SniGetInt(sd_bus *b, const char *path, const char *iface,
                     const char *prop, sd_bus_message *m, void *ud,
                     sd_bus_error *err) {
    (void)b; (void)path; (void)iface; (void)prop; (void)ud; (void)err;
    return sd_bus_message_append(m, "i", 0);
}

static int SniGetUint(sd_bus *b, const char *path, const char *iface,
                      const char *prop, sd_bus_message *m, void *ud,
                      sd_bus_error *err) {
    (void)b; (void)path; (void)iface; (void)prop; (void)ud; (void)err;
    /* DBusMenu Version is "u"; appending "i" mismatches the vtable type. */
    return sd_bus_message_append(m, "u", 3);
}

static int SniGetToolTip(sd_bus *b, const char *path, const char *iface,
                         const char *prop, sd_bus_message *m, void *ud,
                         sd_bus_error *err) {
    int r;
    (void)b; (void)path; (void)iface; (void)prop; (void)ud; (void)err;
    if (!icon_ready) IconBuild();
    /* ToolTip is (s a(iiay) s s): the pixmap sits in an array like above. */
    r = sd_bus_message_open_container(m, 'r', "sa(iiay)ss");
    if (r < 0) return r;
    r = sd_bus_message_append(m, "s", "");
    if (r < 0) return r;
    r = sd_bus_message_open_container(m, SD_BUS_TYPE_ARRAY, "(iiay)");
    if (r < 0) return r;
    r = sd_bus_message_open_container(m, SD_BUS_TYPE_STRUCT, "iiay");
    if (r < 0) return r;
    r = sd_bus_message_append(m, "ii", 24, 24);
    if (r < 0) return r;
    r = sd_bus_message_append_array(m, 'y', ICON_PIX, sizeof ICON_PIX);
    if (r < 0) return r;
    r = sd_bus_message_close_container(m);
    if (r < 0) return r;
    r = sd_bus_message_close_container(m);
    if (r < 0) return r;
    r = sd_bus_message_append(m, "ss", "mnPaper",
                              cb->query_master()
                                ? "A soft paper texture over your screen"
                                : "Paper hidden - click Show paper");
    if (r < 0) return r;
    return sd_bus_message_close_container(m);
}

static int SniActivate(sd_bus_message *msg, void *ud, sd_bus_error *err) {
    int r;
    (void)ud; (void)err;
    cb->toggle_show();
    /* sd-bus vtable convention: a method handler that returns 0 means "no
     * matching object" (the caller gets an UnknownMethod error), and a
     * positive return means "I will send the reply myself".  These methods
     * have an empty result, so send that empty reply and return 1. */
    r = sd_bus_reply_method_return(msg, NULL);
    if (r < 0) return r;
    return 1;
}

static int SniScroll(sd_bus_message *msg, void *ud, sd_bus_error *err) {
    int32_t delta;
    const char *orient;
    (void)ud; (void)err;
    if (sd_bus_message_read(msg, "is", &delta, &orient) < 0) return -EINVAL;
    /* Scroll up/down steps through the strength presets, like a volume icon. */
    {
        int i = StrengthIndex(), v;
        if (i < 0) i = 2;
        i += delta < 0 ? -1 : 1;
        if (i < 0) i = 0;
        if (i > 3) i = 3;
        v = STRENGTH_STEPS[i];
        if (v != cb->query_intensity()) cb->set_intensity(v);
    }
    {   /* empty result: send the reply ourselves (see SniActivate) */
        int rr = sd_bus_reply_method_return(msg, NULL);
        if (rr < 0) return rr;
    }
    return 1;
}

static const sd_bus_vtable sni_vtable[] = {
    SD_BUS_VTABLE_START(0),
    SD_BUS_PROPERTY("Id", "s", SniGetStr, 0, 0),
    SD_BUS_PROPERTY("Category", "s", SniGetStr, 0, 0),
    SD_BUS_PROPERTY("Title", "s", SniGetStr, 0, 0),
    SD_BUS_PROPERTY("Status", "s", SniGetStr, 0,
                    SD_BUS_VTABLE_PROPERTY_EMITS_CHANGE),
    SD_BUS_PROPERTY("WindowId", "i", SniGetInt, 0, 0),
    SD_BUS_PROPERTY("IconName", "s", SniGetStr, 0, 0),
    SD_BUS_PROPERTY("IconPixmap", "a(iiay)", SniGetIconPixmap, 0, 0),
    SD_BUS_PROPERTY("ToolTip", "(sa(iiay)ss)", SniGetToolTip, 0, 0),
    SD_BUS_PROPERTY("Menu", "o", SniGetMenuPath, 0, 0),
    SD_BUS_PROPERTY("ItemIsMenu", "b", SniGetBool, 0, 0),
    SD_BUS_METHOD("Activate", "ii", "", SniActivate, 0),
    SD_BUS_METHOD("SecondaryActivate", "ii", "", SniActivate, 0),
    SD_BUS_METHOD("Scroll", "is", "", SniScroll, 0),
    SD_BUS_VTABLE_END,
};

static const sd_bus_vtable menu_vtable[] = {
    SD_BUS_VTABLE_START(0),
    SD_BUS_PROPERTY("Version", "u", SniGetUint, 0, 0),   /* DBusMenu v3 */
    SD_BUS_PROPERTY("Status", "s", SniGetStr, 0, 0),
    SD_BUS_METHOD("GetLayout", "iias", "u(ia{sv}av)", MenuGetLayout, 0),
    SD_BUS_METHOD("GetGroupProperties", "aias", "a(ia{sv})",
                  MenuGetGroupProperties, 0),
    SD_BUS_METHOD("AboutToShow", "i", "b", MenuAboutToShow, 0),
    SD_BUS_METHOD("Event", "issu", "", MenuEvent, 0),
    SD_BUS_METHOD("EventGroup", "a(isvu)", "", MenuEventGroup, 0),
    SD_BUS_VTABLE_END,
};

/* -------------------------------------------------------------- lifecycle -- */

int tray_start(const struct TRAYCB *c) {
    sd_bus_error err = SD_BUS_ERROR_NULL;
    sd_bus_message *reply = NULL;
    int r;

    cb = c;
    if (!cb) return -1;

    r = sd_bus_open_user(&bus);
    if (r < 0) {
        L("tray: no session bus (%s) - tray disabled, CLI still works",
          strerror(-r));
        return -1;
    }

    r = sd_bus_add_object_vtable(bus, NULL, "/StatusNotifierItem",
                                 "org.kde.StatusNotifierItem",
                                 sni_vtable, NULL);
    if (r < 0) goto fail;
    r = sd_bus_add_object_vtable(bus, NULL, "/StatusNotifierItem",
                                 "com.canonical.dbusmenu",
                                 menu_vtable, NULL);
    if (r < 0) goto fail;

    snprintf(service, sizeof service, "org.kde.StatusNotifierItem-%d-1",
             (int)getpid());
    r = sd_bus_request_name(bus, service, SD_BUS_NAME_REPLACE_EXISTING);
    if (r < 0) {
        /* fall back to the unique name, which the watcher accepts too */
        const char *uniq = NULL;
        sd_bus_get_unique_name(bus, &uniq);
        snprintf(service, sizeof service, "%s", uniq ? uniq : "");
        r = sd_bus_request_name(bus, service, 0);
        if (r < 0) goto fail;
    }

    r = sd_bus_call_method(bus, "org.kde.StatusNotifierWatcher",
                           "/StatusNotifierWatcher",
                           "org.kde.StatusNotifierWatcher",
                           "RegisterStatusNotifierItem", &err, &reply, "s",
                           service);
    if (r < 0) {
        L("tray: no tray watcher on this session (%s) - tray disabled, "
          "CLI still works", err.message ? err.message : strerror(-r));
        sd_bus_error_free(&err);
        sd_bus_unref(bus);
        bus = NULL;
        return -1;
    }
    sd_bus_message_unref(reply);
    sd_bus_error_free(&err);

    signal(SIGCHLD, SIG_IGN);   /* the Edit-config child is fire and forget */
    L("tray: registered as %s", service);
    return 0;

fail:
    L("tray: registration failed (%s) - tray disabled, CLI still works",
      strerror(-r));
    sd_bus_error_free(&err);
    sd_bus_unref(bus);
    bus = NULL;
    return -1;
}

void tray_state_changed(void) {
    sd_bus_message *sig = NULL;
    int r;
    if (!bus) return;
    revision++;

    r = sd_bus_emit_properties_changed(bus, "/StatusNotifierItem",
                                       "org.kde.StatusNotifierItem", "Status",
                                       "ToolTip", NULL);
    (void)r;

    /* ItemsPropertiesUpdated: the toggles and radios whose state can move. */
    r = sd_bus_message_new_signal(bus, &sig, "/StatusNotifierItem",
                                  "com.canonical.dbusmenu",
                                  "ItemsPropertiesUpdated");
    if (r < 0) return;
    r = sd_bus_message_open_container(sig, SD_BUS_TYPE_ARRAY, "(ia{sv})");
    if (r >= 0) {
        /* worst case: show + autostart + 4 strength + 4 warmth radios */
        int ids[10], states[10], n = 0, i, si = StrengthIndex(), wi = WarmthIndex();
        ids[n] = ID_SHOW;        states[n++] = cb->query_master() ? 1 : 0;
        ids[n] = ID_AUTOSTART;   states[n++] = cb->query_autostart() ? 1 : 0;
        for (i = 0; i < 4; i++) {
            if (si >= 0) { ids[n] = ID_STRENGTH_BASE + 1 + i; states[n++] = (si == i); }
            if (wi >= 0) { ids[n] = ID_WARMTH_BASE + 1 + i;  states[n++] = (wi == i); }
        }
        for (i = 0; i < n; i++) {
            sd_bus_message_open_container(sig, SD_BUS_TYPE_STRUCT, "ia{sv}");
            sd_bus_message_append(sig, "i", ids[i]);
            sd_bus_message_open_container(sig, SD_BUS_TYPE_ARRAY, "{sv}");
            sd_bus_message_append(sig, "{sv}", "toggle-state", "i", states[i]);
            sd_bus_message_close_container(sig);
            sd_bus_message_close_container(sig);
        }
        sd_bus_message_close_container(sig);
    }
    r = sd_bus_message_open_container(sig, SD_BUS_TYPE_ARRAY, "(ias)");
    if (r >= 0) sd_bus_message_close_container(sig);
    r = sd_bus_send(NULL, sig, NULL);
    sd_bus_message_unref(sig);
    (void)r;

    r = sd_bus_emit_signal(bus, "/StatusNotifierItem", "com.canonical.dbusmenu",
                           "LayoutUpdated", "ui", revision, 0);
    (void)r;
}

int tray_fd(void) {
    if (!bus) return -1;
    return sd_bus_get_fd(bus);
}

short tray_events(void) {
    if (!bus) return 0;
    return (short)sd_bus_get_events(bus);
}

int tray_timeout_ms(void) {
    uint64_t t;
    uint64_t now;
    struct timespec ts;
    if (!bus) return -1;
    if (sd_bus_get_timeout(bus, &t) < 0 || t == UINT64_MAX) return -1;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    now = (uint64_t)ts.tv_sec * 1000000ull + (uint64_t)ts.tv_nsec / 1000u;
    if (t <= now) return 0;
    if ((t - now) / 1000ull > 1000000ull) return -1;
    return (int)((t - now) / 1000ull);
}

void tray_process(void) {
    int r;
    if (!bus) return;
    for (;;) {
        r = sd_bus_process(bus, NULL);
        if (r < 0) {
            /* A dropped session bus (logout) is not fatal for the overlay. */
            if (r == -ENOTCONN || r == -ECONNRESET || r == -EPIPE) {
                L("tray: session bus gone - tray disabled, overlay keeps running");
                sd_bus_unref(bus);
                bus = NULL;
            }
            return;
        }
        if (r == 0) break;
    }
    r = sd_bus_flush(bus);
    if (r == -ENOTCONN || r == -ECONNRESET || r == -EPIPE) {
        L("tray: session bus gone - tray disabled, overlay keeps running");
        sd_bus_unref(bus);
        bus = NULL;
    }
}
