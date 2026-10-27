#include "gsr-region-selector.h"

#include <math.h>
#include <adwaita.h>
#include <glib/gi18n.h>
#include <glib/gstdio.h>

#ifdef HAVE_X11
#include <gdk/x11/gdkx.h>
#endif

/* ═══════════════════════════════════════════════════════════════════
 *  Region selector
 *
 *  One undecorated fullscreen window per monitor. Before the windows are
 *  shown, each monitor is captured with gpu-screen-recorder and the image is
 *  painted as a frozen backdrop, since compositors such as GNOME draw
 *  fullscreen windows opaquely and real transparency would show black. A dim
 *  veil goes on top; while dragging, the selected rectangle is cut out of the
 *  veil and outlined in the accent color with its pixel size.
 *
 *  If a capture fails, that overlay falls back to a translucent veil.
 * ═══════════════════════════════════════════════════════════════════ */

/* Drags smaller than this in both directions count as a click */
#define MIN_DRAG_SIZE 8

/* Give up on backdrop captures after this long and show plain overlays */
#define CAPTURE_TIMEOUT_MS 3000

typedef struct _Selector Selector;

typedef struct {
    Selector   *selector;   /* borrowed */
    GtkWindow  *window;
    GtkWidget  *area;
    GdkMonitor *monitor;    /* owned ref */

    GSubprocess     *capture;       /* owned while the backdrop is captured */
    char            *capture_path;  /* owned */
    cairo_surface_t *backdrop;      /* owned, nullable */
} Overlay;

struct _Selector {
    GTask        *task;          /* owned until completion */
    GCancellable *cancellable;   /* owned, nullable */
    gulong        cancelled_id;
    GPtrArray    *overlays;      /* Overlay*, owned */

    char         *capture_dir;   /* owned, private temp dir */
    guint         pending_captures;
    guint         capture_timeout_id;

    Overlay      *active;        /* overlay with a drag in progress */
    double        start_x, start_y;
    double        cur_x, cur_y;
};

/* ── Helpers ─────────────────────────────────────────────────────── */

static void
overlay_free(Overlay *ov)
{
    gtk_window_destroy(ov->window);
    g_object_unref(ov->monitor);
    g_clear_object(&ov->capture);
    g_free(ov->capture_path);
    g_clear_pointer(&ov->backdrop, cairo_surface_destroy);
    g_free(ov);
}

static void
selector_free(Selector *sel)
{
    if (sel->cancellable) {
        g_cancellable_disconnect(sel->cancellable, sel->cancelled_id);
        g_object_unref(sel->cancellable);
    }
    g_clear_handle_id(&sel->capture_timeout_id, g_source_remove);
    g_clear_pointer(&sel->overlays, g_ptr_array_unref);
    g_free(sel->capture_dir);
    g_free(sel);
}

/* Selection in monitor-local logical coordinates, clamped to the monitor */
static GdkRectangle
selection_rect(Selector *sel, int width, int height)
{
    double x0 = CLAMP(MIN(sel->start_x, sel->cur_x), 0, width);
    double y0 = CLAMP(MIN(sel->start_y, sel->cur_y), 0, height);
    double x1 = CLAMP(MAX(sel->start_x, sel->cur_x), 0, width);
    double y1 = CLAMP(MAX(sel->start_y, sel->cur_y), 0, height);
    return (GdkRectangle){
        .x = (int)floor(x0), .y = (int)floor(y0),
        .width = (int)ceil(x1) - (int)floor(x0),
        .height = (int)ceil(y1) - (int)floor(y0),
    };
}

/* Close the overlays first so they are gone before the caller starts
 * capturing, then hand the result back. */
static void
selector_complete(Selector *sel, const GdkRectangle *region, GError *error)
{
    GTask *task = g_steal_pointer(&sel->task);
    if (!task)
        return; /* already completed */

    /* Hide now, destroy on idle: we may be inside one of the overlays'
     * own gesture handlers. */
    sel->active = NULL;
    GPtrArray *overlays = g_steal_pointer(&sel->overlays);
    for (guint i = 0; i < overlays->len; i++) {
        Overlay *ov = g_ptr_array_index(overlays, i);
        gtk_widget_set_visible(GTK_WIDGET(ov->window), FALSE);
    }
    g_idle_add_once((GSourceOnceFunc)g_ptr_array_unref, overlays);

    if (region)
        g_task_return_pointer(task, g_memdup2(region, sizeof(*region)), g_free);
    else
        g_task_return_error(task, error);
    g_object_unref(task);
}

static void
selector_cancel(Selector *sel)
{
    selector_complete(sel, NULL,
        g_error_new_literal(G_IO_ERROR, G_IO_ERROR_CANCELLED,
                            _("Region selection was cancelled")));
}

/* Convert a monitor-local logical rectangle to the space gpu-screen-recorder
 * expects. On Wayland that is the compositor's logical space, which is what
 * GdkMonitor geometry already uses. On X11, GTK divides by GDK_SCALE, so
 * scale back up to X screen pixels. */
static GdkRectangle
to_capture_space(GdkMonitor *monitor, GdkRectangle local)
{
    GdkRectangle geo;
    gdk_monitor_get_geometry(monitor, &geo);

    GdkRectangle r = {
        .x = geo.x + local.x, .y = geo.y + local.y,
        .width = local.width, .height = local.height,
    };

#ifdef HAVE_X11
    if (GDK_IS_X11_DISPLAY(gdk_monitor_get_display(monitor))) {
        int scale = gdk_monitor_get_scale_factor(monitor);
        r.x *= scale;
        r.y *= scale;
        r.width *= scale;
        r.height *= scale;
    }
#endif
    return r;
}

/* ── Drawing ─────────────────────────────────────────────────────── */

static void
draw_label(GtkWidget *widget, cairo_t *cr, const char *text,
           double x, double y, int max_width, int max_height)
{
    PangoLayout *layout = gtk_widget_create_pango_layout(widget, text);
    int tw, th;
    pango_layout_get_pixel_size(layout, &tw, &th);

    const double pad = 6;
    double bw = tw + 2 * pad, bh = th + 2 * pad;
    x = CLAMP(x, 0, MAX(0, max_width - bw));
    y = CLAMP(y, 0, MAX(0, max_height - bh));

    cairo_set_source_rgba(cr, 0, 0, 0, 0.7);
    cairo_new_sub_path(cr);
    const double r = 6;
    cairo_arc(cr, x + bw - r, y + r,      r, -G_PI / 2, 0);
    cairo_arc(cr, x + bw - r, y + bh - r, r, 0,         G_PI / 2);
    cairo_arc(cr, x + r,      y + bh - r, r, G_PI / 2,  G_PI);
    cairo_arc(cr, x + r,      y + r,      r, G_PI,      3 * G_PI / 2);
    cairo_close_path(cr);
    cairo_fill(cr);

    cairo_set_source_rgba(cr, 1, 1, 1, 1);
    cairo_move_to(cr, x + pad, y + pad);
    pango_cairo_show_layout(cr, layout);
    g_object_unref(layout);
}

static void
overlay_draw(GtkDrawingArea *area, cairo_t *cr, int width, int height,
             gpointer user_data)
{
    Overlay *ov = user_data;
    Selector *sel = ov->selector;

    /* Frozen backdrop, or fully transparent if the capture failed */
    cairo_save(cr);
    cairo_set_operator(cr, CAIRO_OPERATOR_SOURCE);
    if (ov->backdrop) {
        cairo_scale(cr,
            (double)width / cairo_image_surface_get_width(ov->backdrop),
            (double)height / cairo_image_surface_get_height(ov->backdrop));
        cairo_set_source_surface(cr, ov->backdrop, 0, 0);
    } else {
        cairo_set_source_rgba(cr, 0, 0, 0, 0);
    }
    cairo_paint(cr);
    cairo_restore(cr);

    /* Dim veil over everything except the selection */
    gboolean selecting = sel->active == ov;
    GdkRectangle r = selecting ? selection_rect(sel, width, height)
                               : (GdkRectangle){ 0, 0, 0, 0 };
    cairo_set_operator(cr, CAIRO_OPERATOR_OVER);
    cairo_set_fill_rule(cr, CAIRO_FILL_RULE_EVEN_ODD);
    cairo_rectangle(cr, 0, 0, width, height);
    if (selecting)
        cairo_rectangle(cr, r.x, r.y, r.width, r.height);
    cairo_set_source_rgba(cr, 0, 0, 0, 0.4);
    cairo_fill(cr);

    if (!selecting) {
        const char *hint =
            _("Drag to select an area · Click to select the whole monitor · Esc to cancel");
        PangoLayout *layout = gtk_widget_create_pango_layout(GTK_WIDGET(area), hint);
        int tw, th;
        pango_layout_get_pixel_size(layout, &tw, &th);
        g_object_unref(layout);
        draw_label(GTK_WIDGET(area), cr, hint,
                   (width - tw) / 2.0 - 6, height * 0.1, width, height);
        return;
    }

    g_autoptr(GdkRGBA) accent =
        adw_style_manager_get_accent_color_rgba(adw_style_manager_get_default());
    gdk_cairo_set_source_rgba(cr, accent);
    cairo_set_line_width(cr, 2);
    cairo_rectangle(cr, r.x + 1, r.y + 1, MAX(0, r.width - 2), MAX(0, r.height - 2));
    cairo_stroke(cr);

    /* Size in device pixels, i.e. what ends up in the video */
    double scale = gdk_monitor_get_scale(ov->monitor);
    g_autofree char *size = g_strdup_printf("%d × %d",
        (int)round(r.width * scale), (int)round(r.height * scale));
    draw_label(GTK_WIDGET(area), cr, size, r.x, r.y + r.height + 8, width, height);
}

/* ── Input ───────────────────────────────────────────────────────── */

static void
on_drag_begin(GtkGestureDrag *gesture G_GNUC_UNUSED,
              double x, double y, gpointer user_data)
{
    Overlay *ov = user_data;
    Selector *sel = ov->selector;

    Overlay *prev = sel->active;
    sel->active = ov;
    sel->start_x = sel->cur_x = x;
    sel->start_y = sel->cur_y = y;

    if (prev && prev != ov)
        gtk_widget_queue_draw(prev->area);
    gtk_widget_queue_draw(ov->area);
}

static void
on_drag_update(GtkGestureDrag *gesture G_GNUC_UNUSED,
               double offset_x, double offset_y, gpointer user_data)
{
    Overlay *ov = user_data;
    Selector *sel = ov->selector;
    if (sel->active != ov)
        return;

    sel->cur_x = sel->start_x + offset_x;
    sel->cur_y = sel->start_y + offset_y;
    gtk_widget_queue_draw(ov->area);
}

static void
on_drag_end(GtkGestureDrag *gesture G_GNUC_UNUSED,
            double offset_x, double offset_y, gpointer user_data)
{
    Overlay *ov = user_data;
    Selector *sel = ov->selector;
    if (sel->active != ov)
        return;

    sel->cur_x = sel->start_x + offset_x;
    sel->cur_y = sel->start_y + offset_y;

    int width = gtk_widget_get_width(ov->area);
    int height = gtk_widget_get_height(ov->area);
    GdkRectangle local;
    if (fabs(offset_x) < MIN_DRAG_SIZE && fabs(offset_y) < MIN_DRAG_SIZE)
        local = (GdkRectangle){ 0, 0, width, height }; /* click: whole monitor */
    else
        local = selection_rect(sel, width, height);

    GdkRectangle region = to_capture_space(ov->monitor, local);
    selector_complete(sel, &region, NULL);
}

static gboolean
on_key_pressed(GtkEventControllerKey *controller G_GNUC_UNUSED,
               guint keyval, guint keycode G_GNUC_UNUSED,
               GdkModifierType state G_GNUC_UNUSED, gpointer user_data)
{
    if (keyval != GDK_KEY_Escape)
        return FALSE;
    selector_cancel(user_data);
    return TRUE;
}

static void
on_cancellable_cancelled(GCancellable *cancellable G_GNUC_UNUSED,
                         gpointer      user_data)
{
    selector_cancel(user_data);
}

/* ── Setup ───────────────────────────────────────────────────────── */

static void
ensure_css(GdkDisplay *display)
{
    static GdkDisplay *installed_for = NULL;
    if (installed_for == display)
        return;
    installed_for = display;

    g_autoptr(GtkCssProvider) provider = gtk_css_provider_new();
    gtk_css_provider_load_from_string(provider,
        "window.gsr-region-selector { background: none; }");
    gtk_style_context_add_provider_for_display(display,
        GTK_STYLE_PROVIDER(provider), GTK_STYLE_PROVIDER_PRIORITY_APPLICATION);
}

static Overlay *
overlay_new(Selector *sel, GdkMonitor *monitor)
{
    Overlay *ov = g_new0(Overlay, 1);
    ov->selector = sel;
    ov->monitor = g_object_ref(monitor);

    ov->window = GTK_WINDOW(gtk_window_new());
    gtk_window_set_title(ov->window, _("Select Region"));
    gtk_window_set_decorated(ov->window, FALSE);
    gtk_widget_add_css_class(GTK_WIDGET(ov->window), "gsr-region-selector");

    ov->area = gtk_drawing_area_new();
    gtk_widget_set_cursor_from_name(ov->area, "crosshair");
    gtk_drawing_area_set_draw_func(GTK_DRAWING_AREA(ov->area), overlay_draw, ov, NULL);
    gtk_window_set_child(ov->window, ov->area);

    GtkGesture *drag = gtk_gesture_drag_new();
    gtk_gesture_single_set_button(GTK_GESTURE_SINGLE(drag), GDK_BUTTON_PRIMARY);
    g_signal_connect(drag, "drag-begin", G_CALLBACK(on_drag_begin), ov);
    g_signal_connect(drag, "drag-update", G_CALLBACK(on_drag_update), ov);
    g_signal_connect(drag, "drag-end", G_CALLBACK(on_drag_end), ov);
    gtk_widget_add_controller(ov->area, GTK_EVENT_CONTROLLER(drag));

    GtkEventController *keys = gtk_event_controller_key_new();
    g_signal_connect(keys, "key-pressed", G_CALLBACK(on_key_pressed), sel);
    gtk_widget_add_controller(GTK_WIDGET(ov->window), keys);

    return ov;
}

/* ── Backdrop capture ────────────────────────────────────────────── */

static cairo_surface_t *
load_backdrop(const char *path)
{
    g_autoptr(GdkTexture) texture = gdk_texture_new_from_filename(path, NULL);
    if (!texture)
        return NULL;

    int w = gdk_texture_get_width(texture);
    int h = gdk_texture_get_height(texture);
    cairo_surface_t *surface = cairo_image_surface_create(CAIRO_FORMAT_ARGB32, w, h);
    /* GDK_MEMORY_DEFAULT matches CAIRO_FORMAT_ARGB32 on either endianness */
    gdk_texture_download(texture, cairo_image_surface_get_data(surface),
                         (gsize)cairo_image_surface_get_stride(surface));
    cairo_surface_mark_dirty(surface);
    return surface;
}

static void
selector_show(Selector *sel)
{
    g_clear_handle_id(&sel->capture_timeout_id, g_source_remove);

    for (guint i = 0; i < sel->overlays->len; i++) {
        Overlay *ov = g_ptr_array_index(sel->overlays, i);
        if (ov->capture_path) {
            g_unlink(ov->capture_path);
            g_clear_pointer(&ov->capture_path, g_free);
        }
        gtk_window_fullscreen_on_monitor(ov->window, ov->monitor);
        gtk_window_present(ov->window);
    }
    if (sel->capture_dir)
        g_rmdir(sel->capture_dir);

    if (sel->cancellable)
        sel->cancelled_id = g_cancellable_connect(sel->cancellable,
            G_CALLBACK(on_cancellable_cancelled), sel, NULL);
}

static void
on_capture_done(GObject *source, GAsyncResult *result, gpointer user_data)
{
    Overlay *ov = user_data;
    Selector *sel = ov->selector;

    if (g_subprocess_wait_check_finish(G_SUBPROCESS(source), result, NULL))
        ov->backdrop = load_backdrop(ov->capture_path);
    g_clear_object(&ov->capture);

    if (--sel->pending_captures == 0)
        selector_show(sel);
}

static gboolean
on_capture_timeout(gpointer user_data)
{
    Selector *sel = user_data;
    sel->capture_timeout_id = 0;

    /* Each forced exit still runs on_capture_done, which shows the overlays
     * once the last one is accounted for. */
    for (guint i = 0; i < sel->overlays->len; i++) {
        Overlay *ov = g_ptr_array_index(sel->overlays, i);
        if (ov->capture)
            g_subprocess_force_exit(ov->capture);
    }
    return G_SOURCE_REMOVE;
}

/* Start capturing one monitor with gpu-screen-recorder; matched by connector
 * name (e.g. "DP-2"), which is also what gpu-screen-recorder calls it. */
static void
capture_start(Selector *sel, Overlay *ov, guint index)
{
    const char *connector = gdk_monitor_get_connector(ov->monitor);
    if (!connector || !sel->capture_dir)
        return;

    g_autofree char *name = g_strdup_printf("monitor-%u.jpg", index);
    ov->capture_path = g_build_filename(sel->capture_dir, name, NULL);

    ov->capture = g_subprocess_new(
        G_SUBPROCESS_FLAGS_STDOUT_SILENCE | G_SUBPROCESS_FLAGS_STDERR_SILENCE, NULL,
        "gpu-screen-recorder", "-w", connector, "-cursor", "no", "-v", "no",
        "-q", "very_high", "-o", ov->capture_path, NULL);
    if (!ov->capture)
        return;

    sel->pending_captures++;
    g_subprocess_wait_check_async(ov->capture, NULL, on_capture_done, ov);
}

/* ── Public API ──────────────────────────────────────────────────── */

void
gsr_region_selector_select_async(GdkDisplay          *display,
                                 GCancellable        *cancellable,
                                 GAsyncReadyCallback  callback,
                                 gpointer             user_data)
{
    g_return_if_fail(GDK_IS_DISPLAY(display));

    GTask *task = g_task_new(NULL, cancellable, callback, user_data);
    g_task_set_source_tag(task, gsr_region_selector_select_async);

    GListModel *monitors = gdk_display_get_monitors(display);
    guint n = g_list_model_get_n_items(monitors);
    if (n == 0) {
        g_task_return_new_error(task, G_IO_ERROR, G_IO_ERROR_NOT_FOUND,
                                _("No monitors found"));
        g_object_unref(task);
        return;
    }

    ensure_css(display);

    Selector *sel = g_new0(Selector, 1);
    sel->task = task;
    sel->overlays = g_ptr_array_new_with_free_func((GDestroyNotify)overlay_free);
    g_task_set_task_data(task, sel, (GDestroyNotify)selector_free);

    /* Cancellation is only hooked up once the overlays are shown, so it
     * can't tear them down while captures still reference them. */
    if (cancellable)
        sel->cancellable = g_object_ref(cancellable);

    sel->capture_dir = g_dir_make_tmp("gsr-region-XXXXXX", NULL);

    for (guint i = 0; i < n; i++) {
        g_autoptr(GdkMonitor) monitor = g_list_model_get_item(monitors, i);
        Overlay *ov = overlay_new(sel, monitor);
        g_ptr_array_add(sel->overlays, ov);
        capture_start(sel, ov, i);
    }

    if (sel->pending_captures == 0)
        selector_show(sel);
    else
        sel->capture_timeout_id = g_timeout_add(CAPTURE_TIMEOUT_MS, on_capture_timeout, sel);
}

gboolean
gsr_region_selector_select_finish(GAsyncResult  *result,
                                  GdkRectangle  *out_region,
                                  GError       **error)
{
    g_return_val_if_fail(g_task_is_valid(result, NULL), FALSE);

    g_autofree GdkRectangle *region = g_task_propagate_pointer(G_TASK(result), error);
    if (!region)
        return FALSE;
    if (out_region)
        *out_region = *region;
    return TRUE;
}
