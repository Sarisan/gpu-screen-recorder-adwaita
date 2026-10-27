#pragma once

#include <gtk/gtk.h>

G_BEGIN_DECLS

/*
 * Interactive screen-region picker.
 *
 * Covers every monitor with a dimmed fullscreen overlay and lets the user
 * drag out a rectangle on one of them. A click without dragging selects the
 * whole monitor under the pointer; Escape cancels.
 *
 * The result is in the coordinate space gpu-screen-recorder's "-region"
 * option expects: compositor logical coordinates on Wayland, X screen pixels
 * on X11.
 */

void     gsr_region_selector_select_async  (GdkDisplay          *display,
                                            GCancellable        *cancellable,
                                            GAsyncReadyCallback  callback,
                                            gpointer             user_data);

/* Returns FALSE with G_IO_ERROR_CANCELLED if the user pressed Escape. */
gboolean gsr_region_selector_select_finish (GAsyncResult        *result,
                                            GdkRectangle        *out_region,
                                            GError             **error);

G_END_DECLS
