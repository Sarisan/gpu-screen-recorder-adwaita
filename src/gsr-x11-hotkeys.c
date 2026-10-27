#include "gsr-x11-hotkeys.h"

#include <stdlib.h>
#include <string.h>

#include <X11/Xlib.h>
#include <X11/Xutil.h>
#include <glib-unix.h>
#include <glib.h>

/* ── Internal types ──────────────────────────────────────────────── */

#define MAX_GRABBED_COMBOS 16

struct _GsrX11Hotkeys {
    Display              *display;      /* owned; separate from GDK's */
    Window                root;
    unsigned int          numlockmask;

    GsrX11HotkeyCombo    combos[MAX_GRABBED_COMBOS];
    int                   num_combos;

    GsrX11HotkeyCallback callback;
    void                 *userdata;

    GSource              *source;       /* GLib source watching the X fd */
    guint                 source_id;
};

/* ── NumLock detection ───────────────────────────────────────────── */

static unsigned int
detect_numlock_mask(Display *display)
{
    unsigned int mask = 0;
    KeyCode numlock_keycode = XKeysymToKeycode(display, XK_Num_Lock);
    XModifierKeymap *modmap = XGetModifierMapping(display);
    if (modmap) {
        for (int i = 0; i < 8; i++) {
            for (int j = 0; j < modmap->max_keypermod; j++) {
                if (modmap->modifiermap[i * modmap->max_keypermod + j] == numlock_keycode)
                    mask = (1u << i);
            }
        }
        XFreeModifiermap(modmap);
    }
    return mask;
}

/* ── X error handling ────────────────────────────────────────────── */

static bool x_grab_failed = false;

static int
xerror_grab(Display *dpy G_GNUC_UNUSED, XErrorEvent *ev G_GNUC_UNUSED)
{
    x_grab_failed = true;
    return 0;
}

/* ── Strip NumLock/CapsLock from key state ───────────────────────── */

static unsigned int
key_state_without_locks(unsigned int state)
{
    return state & ~(Mod2Mask | LockMask);
}

/* ── GSource callbacks for X fd polling ──────────────────────────── */

typedef struct {
    GSource        source;
    GsrX11Hotkeys *hotkeys;
} X11Source;

static gboolean
x11_source_prepare(GSource *source, gint *timeout)
{
    *timeout = -1;
    /* Events may already be queued in Xlib without the fd being readable */
    return XPending(((X11Source *)source)->hotkeys->display) > 0;
}

static gboolean
x11_source_check(GSource *source)
{
    return XPending(((X11Source *)source)->hotkeys->display) > 0;
}

static gboolean
x11_source_dispatch(GSource *source,
                    GSourceFunc callback G_GNUC_UNUSED,
                    gpointer user_data G_GNUC_UNUSED)
{
    GsrX11Hotkeys *self = ((X11Source *)source)->hotkeys;

    while (XPending(self->display)) {
        XEvent ev;
        XNextEvent(self->display, &ev);

        if (ev.type != KeyRelease)
            continue;

        KeySym keysym = XLookupKeysym(&ev.xkey, 0);
        unsigned int state = key_state_without_locks(ev.xkey.state);

        for (int i = 0; i < self->num_combos; i++) {
            if (keysym == self->combos[i].keysym &&
                state == self->combos[i].modifiers)
            {
                self->callback(self->combos[i].modifiers,
                              self->combos[i].keysym,
                              self->userdata);
                break;
            }
        }
    }

    return G_SOURCE_CONTINUE;
}

static GSourceFuncs x11_source_funcs = {
    .prepare  = x11_source_prepare,
    .check    = x11_source_check,
    .dispatch = x11_source_dispatch,
    .finalize = NULL,
};

/* ── Public API ──────────────────────────────────────────────────── */

GsrX11Hotkeys *
gsr_x11_hotkeys_new(const char *display_name,
                     GsrX11HotkeyCallback callback,
                     void *userdata)
{
    if (!callback)
        return NULL;

    /* Use our own connection: reading events from GDK's Display would
     * steal events GTK needs and leave windows unpainted or frozen. Key
     * grabs are delivered to the connection that made them. */
    Display *display = XOpenDisplay(display_name);
    if (!display)
        return NULL;

    GsrX11Hotkeys *self = calloc(1, sizeof(GsrX11Hotkeys));
    if (!self) {
        XCloseDisplay(display);
        return NULL;
    }

    self->display      = display;
    self->root         = DefaultRootWindow(display);
    self->numlockmask  = detect_numlock_mask(display);
    self->callback     = callback;
    self->userdata     = userdata;
    self->num_combos   = 0;

    /* Create a GSource that polls the X connection fd */
    int x_fd = ConnectionNumber(display);
    self->source = g_source_new(&x11_source_funcs, sizeof(X11Source));
    ((X11Source *)self->source)->hotkeys = self;
    g_source_add_unix_fd(self->source, x_fd, G_IO_IN | G_IO_HUP | G_IO_ERR);
    self->source_id = g_source_attach(self->source, NULL);

    return self;
}

void
gsr_x11_hotkeys_free(GsrX11Hotkeys *self)
{
    if (!self)
        return;

    gsr_x11_hotkeys_ungrab_all(self);

    if (self->source) {
        g_source_destroy(self->source);
        g_clear_pointer(&self->source, g_source_unref);
    }

    XCloseDisplay(self->display);
    free(self);
}

void
gsr_x11_hotkeys_ungrab_all(GsrX11Hotkeys *self)
{
    if (!self)
        return;

    unsigned int modifiers[] = { 0, LockMask, self->numlockmask,
                                 self->numlockmask | LockMask };

    for (int i = 0; i < self->num_combos; i++) {
        KeyCode kc = XKeysymToKeycode(self->display, self->combos[i].keysym);
        for (int m = 0; m < 4; m++) {
            XUngrabKey(self->display, kc,
                       self->combos[i].modifiers | modifiers[m],
                       self->root);
        }
    }
    XSync(self->display, False);
    self->num_combos = 0;
}

bool
gsr_x11_hotkeys_grab(GsrX11Hotkeys *self, GsrX11HotkeyCombo combo)
{
    if (!self || self->num_combos >= MAX_GRABBED_COMBOS)
        return false;

    if (combo.keysym == None && combo.modifiers == 0)
        return true;  /* nothing to grab */

    unsigned int modifiers[] = { 0, LockMask, self->numlockmask,
                                 self->numlockmask | LockMask };

    XSync(self->display, False);
    x_grab_failed = false;
    XErrorHandler prev = XSetErrorHandler(xerror_grab);

    KeyCode kc = XKeysymToKeycode(self->display, combo.keysym);
    for (int m = 0; m < 4; m++) {
        XGrabKey(self->display, kc,
                 combo.modifiers | modifiers[m],
                 self->root, False,
                 GrabModeAsync, GrabModeAsync);
    }

    XSync(self->display, False);
    XSetErrorHandler(prev);

    if (x_grab_failed) {
        /* Undo partial grabs */
        for (int m = 0; m < 4; m++) {
            XUngrabKey(self->display, kc,
                       combo.modifiers | modifiers[m],
                       self->root);
        }
        XSync(self->display, False);
        return false;
    }

    self->combos[self->num_combos++] = combo;
    return true;
}
