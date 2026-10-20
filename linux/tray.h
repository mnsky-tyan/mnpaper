/* tray.h - StatusNotifierItem tray icon + DBusMenu for the Wayland port.
 *
 * Gives mnPaper a presence in the system tray (KDE Plasma, GNOME with the
 * AppIndicator extension, wlroots trays) without a GUI toolkit:
 *
 *   - left-click toggles the paper (Activate)
 *   - right-click opens the menu:
 *       Show paper            [checkmark, reflects master]
 *       ---------------------
 *       Strength >            Subtle / Soft / Medium / Strong   (radios)
 *       Warmth >              Cool / Neutral / Warm / Aged      (radios)
 *       ---------------------
 *       Start at login        [checkmark, reflects autostart]
 *       Edit settings file...
 *       ---------------------
 *       Quit
 *
 * The app drives everything through callbacks; the tray never touches the
 * Wayland state itself.  If the session has no StatusNotifierWatcher (plain
 * GNOME, bare wlroots), tray_start fails with a log line and the CLI keeps
 * working - the tray is an enhancement, never a dependency.
 */
#ifndef MNPAPER_TRAY_H
#define MNPAPER_TRAY_H

struct TRAYCB {
    void (*toggle_show)(void);       /* left click, or Show paper          */
    void (*set_intensity)(int v);    /* Strength radio (10/20/30/40)       */
    void (*set_warmth)(int v);       /* Warmth radio (10/45/75/95)         */
    void (*toggle_autostart)(void);  /* Start at login                     */
    void (*edit_settings)(void);     /* Edit settings file...              */
    void (*quit)(void);              /* Quit                               */
    int  (*query_master)(void);      /* current state, for menu checkmarks */
    int  (*query_intensity)(void);
    int  (*query_warmth)(void);
    int  (*query_autostart)(void);
};

/* Start the tray.  Returns 0 on success, -1 when the session has no tray
 * watcher (the reason is logged).  Never fatal for the daemon. */
int tray_start(const struct TRAYCB *cb);

/* Call after any state the menu reflects has changed: re-emits item
 * properties and the layout revision.  Cheap and idempotent. */
void tray_state_changed(void);

/* poll-loop integration: the sd-bus fd, the events it wants right now, and
 * the relative timeout in ms (-1 = none).  fd is -1 when the tray is off. */
int tray_fd(void);
short tray_events(void);
int tray_timeout_ms(void);

/* Drain pending D-Bus work after poll.  Never blocks. */
void tray_process(void);

#endif /* MNPAPER_TRAY_H */
