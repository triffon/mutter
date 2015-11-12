/* -*- mode: C; c-file-style: "gnu"; indent-tabs-mode: nil; -*- */

/*
 * Copyright (C) 2001 Havoc Pennington, Anders Carlsson
 * Copyright (C) 2002, 2003 Red Hat, Inc.
 * Copyright (C) 2003 Rob Adams
 * Copyright (C) 2004-2006 Elijah Newren
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License as
 * published by the Free Software Foundation; either version 2 of the
 * License, or (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful, but
 * WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, see <http://www.gnu.org/licenses/>.
 */

/**
 * SECTION:window
 * @title: MetaWindow
 * @short_description: Mutter X managed windows
 */

#include <config.h>
#include "window-private.h"
#include "boxes-private.h"
#include "edge-resistance.h"
#include "util-private.h"
#include "frame.h"
#include <meta/errors.h>
#include "workspace-private.h"
#include "stack.h"
#include "keybindings-private.h"
#include "ui.h"
#include "place.h"
#include <meta/prefs.h>
#include <meta/group.h>
#include "constraints.h"
#include <meta/meta-enum-types.h>
#include "core.h"

#include <X11/Xatom.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>

#include <meta/meta-cursor-tracker.h>
#include "meta/compositor-mutter.h"

#include "x11/window-x11.h"
#include "x11/window-props.h"
#include "x11/xprops.h"

#ifdef HAVE_WAYLAND
#include "wayland/meta-window-wayland.h"
#include "wayland/meta-wayland-surface.h"
#include "wayland/meta-wayland-private.h"
#endif

#include "backends/meta-backend-private.h"

/* Windows that unmaximize to a size bigger than that fraction of the workarea
 * will be scaled down to that size (while maintaining aspect ratio).
 * Windows that cover an area greater then this size are automaximized on map.
 */
#define MAX_UNMAXIMIZED_WINDOW_AREA .8

static int destroying_windows_disallowed = 0;

/* Each window has a "stamp" which is a non-recycled 64-bit ID. They
 * start after the end of the XID space so that, for stacking
 * we can keep a guint64 that represents one or the other
 */
static guint64 next_window_stamp = G_GUINT64_CONSTANT(0x100000000);

static void     invalidate_work_areas     (MetaWindow     *window);
static void     set_wm_state              (MetaWindow     *window);
static void     set_net_wm_state          (MetaWindow     *window);
static void     meta_window_set_above     (MetaWindow     *window,
                                           gboolean        new_value);

static void     meta_window_force_placement (MetaWindow     *window);

static void     meta_window_show          (MetaWindow     *window);
static void     meta_window_hide          (MetaWindow     *window);

static void     meta_window_save_rect         (MetaWindow    *window);

static void     ensure_mru_position_after (MetaWindow *window,
                                           MetaWindow *after_this_one);

static void meta_window_move_resize_now (MetaWindow  *window);

static void meta_window_unqueue (MetaWindow *window, guint queuebits);

static void     update_move           (MetaWindow   *window,
                                       gboolean      snap,
                                       int           x,
                                       int           y);
static gboolean update_move_timeout   (gpointer data);
static void     update_resize         (MetaWindow   *window,
                                       gboolean      snap,
                                       int           x,
                                       int           y,
                                       gboolean      force);
static gboolean update_resize_timeout (gpointer data);
static gboolean should_be_on_all_workspaces (MetaWindow *window);

static void meta_window_flush_calc_showing   (MetaWindow *window);

static gboolean queue_calc_showing_func (MetaWindow *window,
                                         void       *data);

static void meta_window_move_between_rects (MetaWindow          *window,
                                            const MetaRectangle *old_area,
                                            const MetaRectangle *new_area);

static void unmaximize_window_before_freeing (MetaWindow        *window);
static void unminimize_window_and_all_transient_parents (MetaWindow *window);

static void meta_window_propagate_focus_appearance (MetaWindow *window,
                                                    gboolean    focused);
static void meta_window_update_icon_now (MetaWindow *window,
                                         gboolean    force);

static void set_workspace_state (MetaWindow    *window,
                                 gboolean       on_all_workspaces,
                                 MetaWorkspace *workspace);

/* Idle handlers for the three queues (run with meta_later_add()). The
 * "data" parameter in each case will be a GINT_TO_POINTER of the
 * index into the queue arrays to use.
 *
 * TODO: Possibly there is still some code duplication among these, which we
 * need to sort out at some point.
 */
static gboolean idle_calc_showing (gpointer data);
static gboolean idle_move_resize (gpointer data);
static gboolean idle_update_icon (gpointer data);

G_DEFINE_ABSTRACT_TYPE (MetaWindow, meta_window, G_TYPE_OBJECT);

enum {
  PROP_0,

  PROP_TITLE,
  PROP_ICON,
  PROP_MINI_ICON,
  PROP_DECORATED,
  PROP_FULLSCREEN,
  PROP_MAXIMIZED_HORIZONTALLY,
  PROP_MAXIMIZED_VERTICALLY,
  PROP_MINIMIZED,
  PROP_WINDOW_TYPE,
  PROP_USER_TIME,
  PROP_DEMANDS_ATTENTION,
  PROP_URGENT,
  PROP_SKIP_TASKBAR,
  PROP_MUTTER_HINTS,
  PROP_APPEARS_FOCUSED,
  PROP_RESIZEABLE,
  PROP_ABOVE,
  PROP_WM_CLASS,
  PROP_GTK_APPLICATION_ID,
  PROP_GTK_UNIQUE_BUS_NAME,
  PROP_GTK_APPLICATION_OBJECT_PATH,
  PROP_GTK_WINDOW_OBJECT_PATH,
  PROP_GTK_APP_MENU_OBJECT_PATH,
  PROP_GTK_MENUBAR_OBJECT_PATH,
  PROP_ON_ALL_WORKSPACES,

  LAST_PROP,
};

static GParamSpec *obj_props[LAST_PROP];

enum
{
  WORKSPACE_CHANGED,
  FOCUS,
  RAISED,
  UNMANAGED,
  SIZE_CHANGED,
  POSITION_CHANGED,

  LAST_SIGNAL
};

static guint window_signals[LAST_SIGNAL] = { 0 };

static void
prefs_changed_callback (MetaPreference pref,
                        gpointer       data)
{
  MetaWindow *window = data;

  if (pref == META_PREF_WORKSPACES_ONLY_ON_PRIMARY)
    {
      meta_window_on_all_workspaces_changed (window);
    }
  else if (pref == META_PREF_ATTACH_MODAL_DIALOGS &&
           window->type == META_WINDOW_MODAL_DIALOG)
    {
      window->attached = meta_window_should_attach_to_parent (window);
      meta_window_recalc_features (window);
      meta_window_queue (window, META_QUEUE_MOVE_RESIZE);
    }
}

static void
meta_window_real_grab_op_began (MetaWindow *window,
                                MetaGrabOp  op)
{
}

static void
meta_window_real_grab_op_ended (MetaWindow *window,
                                MetaGrabOp  op)
{
  window->shaken_loose = FALSE;
}

static void
meta_window_real_current_workspace_changed (MetaWindow *window)
{
}

static gboolean
meta_window_real_update_struts (MetaWindow *window)
{
  return FALSE;
}

static void
meta_window_real_get_default_skip_hints (MetaWindow *window,
                                         gboolean   *skip_taskbar_out,
                                         gboolean   *skip_pager_out)
{
  *skip_taskbar_out = FALSE;
  *skip_pager_out = FALSE;
}

static gboolean
meta_window_real_update_icon (MetaWindow       *window,
                              cairo_surface_t **icon,
                              cairo_surface_t **mini_icon)
{
  *icon = NULL;
  *mini_icon = NULL;
  return FALSE;
}

static void
meta_window_finalize (GObject *object)
{
  MetaWindow *window = META_WINDOW (object);

  if (window->icon)
    cairo_surface_destroy (window->icon);

  if (window->mini_icon)
    cairo_surface_destroy (window->mini_icon);

  if (window->frame_bounds)
    cairo_region_destroy (window->frame_bounds);

  if (window->shape_region)
    cairo_region_destroy (window->shape_region);

  if (window->opaque_region)
    cairo_region_destroy (window->opaque_region);

  if (window->input_region)
    cairo_region_destroy (window->input_region);

  if (window->transient_for)
    g_object_unref (window->transient_for);

  g_free (window->sm_client_id);
  g_free (window->wm_client_machine);
  g_free (window->startup_id);
  g_free (window->role);
  g_free (window->res_class);
  g_free (window->res_name);
  g_free (window->title);
  g_free (window->desc);
  g_free (window->gtk_theme_variant);
  g_free (window->gtk_application_id);
  g_free (window->gtk_unique_bus_name);
  g_free (window->gtk_application_object_path);
  g_free (window->gtk_window_object_path);
  g_free (window->gtk_app_menu_object_path);
  g_free (window->gtk_menubar_object_path);

  G_OBJECT_CLASS (meta_window_parent_class)->finalize (object);
}

static void
meta_window_get_property(GObject         *object,
                         guint            prop_id,
                         GValue          *value,
                         GParamSpec      *pspec)
{
  MetaWindow *win = META_WINDOW (object);

  switch (prop_id)
    {
    case PROP_TITLE:
      g_value_set_string (value, win->title);
      break;
    case PROP_ICON:
      g_value_set_pointer (value, win->icon);
      break;
    case PROP_MINI_ICON:
      g_value_set_pointer (value, win->mini_icon);
      break;
    case PROP_DECORATED:
      g_value_set_boolean (value, win->decorated);
      break;
    case PROP_FULLSCREEN:
      g_value_set_boolean (value, win->fullscreen);
      break;
    case PROP_MAXIMIZED_HORIZONTALLY:
      g_value_set_boolean (value, win->maximized_horizontally);
      break;
    case PROP_MAXIMIZED_VERTICALLY:
      g_value_set_boolean (value, win->maximized_vertically);
      break;
    case PROP_MINIMIZED:
      g_value_set_boolean (value, win->minimized);
      break;
    case PROP_WINDOW_TYPE:
      g_value_set_enum (value, win->type);
      break;
    case PROP_USER_TIME:
      g_value_set_uint (value, win->net_wm_user_time);
      break;
    case PROP_DEMANDS_ATTENTION:
      g_value_set_boolean (value, win->wm_state_demands_attention);
      break;
    case PROP_URGENT:
      g_value_set_boolean (value, win->urgent);
      break;
    case PROP_SKIP_TASKBAR:
      g_value_set_boolean (value, win->skip_taskbar);
      break;
    case PROP_MUTTER_HINTS:
      g_value_set_string (value, win->mutter_hints);
      break;
    case PROP_APPEARS_FOCUSED:
      g_value_set_boolean (value, meta_window_appears_focused (win));
      break;
    case PROP_WM_CLASS:
      g_value_set_string (value, win->res_class);
      break;
    case PROP_RESIZEABLE:
      g_value_set_boolean (value, win->has_resize_func);
      break;
    case PROP_ABOVE:
      g_value_set_boolean (value, win->wm_state_above);
      break;
    case PROP_GTK_APPLICATION_ID:
      g_value_set_string (value, win->gtk_application_id);
      break;
    case PROP_GTK_UNIQUE_BUS_NAME:
      g_value_set_string (value, win->gtk_unique_bus_name);
      break;
    case PROP_GTK_APPLICATION_OBJECT_PATH:
      g_value_set_string (value, win->gtk_application_object_path);
      break;
    case PROP_GTK_WINDOW_OBJECT_PATH:
      g_value_set_string (value, win->gtk_window_object_path);
      break;
    case PROP_GTK_APP_MENU_OBJECT_PATH:
      g_value_set_string (value, win->gtk_app_menu_object_path);
      break;
    case PROP_GTK_MENUBAR_OBJECT_PATH:
      g_value_set_string (value, win->gtk_menubar_object_path);
      break;
    case PROP_ON_ALL_WORKSPACES:
      g_value_set_boolean (value, win->on_all_workspaces);
      break;
    default:
      G_OBJECT_WARN_INVALID_PROPERTY_ID (object, prop_id, pspec);
      break;
    }
}

static void
meta_window_set_property(GObject         *object,
                         guint            prop_id,
                         const GValue    *value,
                         GParamSpec      *pspec)
{
  switch (prop_id)
    {
    default:
      G_OBJECT_WARN_INVALID_PROPERTY_ID (object, prop_id, pspec);
      break;
    }
}

static void
meta_window_class_init (MetaWindowClass *klass)
{
  GObjectClass *object_class = G_OBJECT_CLASS (klass);

  object_class->finalize = meta_window_finalize;

  object_class->get_property = meta_window_get_property;
  object_class->set_property = meta_window_set_property;

  klass->grab_op_began = meta_window_real_grab_op_began;
  klass->grab_op_ended = meta_window_real_grab_op_ended;
  klass->current_workspace_changed = meta_window_real_current_workspace_changed;
  klass->update_struts = meta_window_real_update_struts;
  klass->get_default_skip_hints = meta_window_real_get_default_skip_hints;
  klass->update_icon = meta_window_real_update_icon;

  obj_props[PROP_TITLE] =
    g_param_spec_string ("title",
                         "Title",
                         "The title of the window",
                         NULL,
                         G_PARAM_READABLE | G_PARAM_STATIC_STRINGS);
  obj_props[PROP_ICON] =
    g_param_spec_pointer ("icon",
                          "Icon",
                          "96 pixel sized icon",
                          G_PARAM_READABLE | G_PARAM_STATIC_STRINGS);
  obj_props[PROP_MINI_ICON] =
    g_param_spec_pointer ("mini-icon",
                          "Mini Icon",
                          "16 pixel sized icon",
                          G_PARAM_READABLE | G_PARAM_STATIC_STRINGS);
  obj_props[PROP_DECORATED] =
    g_param_spec_boolean ("decorated",
                          "Decorated",
                          "Whether window is decorated",
                          TRUE,
                          G_PARAM_READABLE | G_PARAM_STATIC_STRINGS);
  obj_props[PROP_FULLSCREEN] =
    g_param_spec_boolean ("fullscreen",
                          "Fullscreen",
                          "Whether window is fullscreened",
                          FALSE,
                          G_PARAM_READABLE | G_PARAM_STATIC_STRINGS);
  obj_props[PROP_MAXIMIZED_HORIZONTALLY] =
    g_param_spec_boolean ("maximized-horizontally",
                          "Maximized horizontally",
                          "Whether window is maximized horizontally",
                          FALSE,
                          G_PARAM_READABLE | G_PARAM_STATIC_STRINGS);
  obj_props[PROP_MAXIMIZED_VERTICALLY] =
    g_param_spec_boolean ("maximized-vertically",
                          "Maximizing vertically",
                          "Whether window is maximized vertically",
                          FALSE,
                          G_PARAM_READABLE | G_PARAM_STATIC_STRINGS);
  obj_props[PROP_MINIMIZED] =
    g_param_spec_boolean ("minimized",
                          "Minimizing",
                          "Whether window is minimized",
                          FALSE,
                          G_PARAM_READABLE | G_PARAM_STATIC_STRINGS);
  obj_props[PROP_WINDOW_TYPE] =
    g_param_spec_enum ("window-type",
                       "Window Type",
                       "The type of the window",
                       META_TYPE_WINDOW_TYPE,
                       META_WINDOW_NORMAL,
                       G_PARAM_READABLE | G_PARAM_STATIC_STRINGS);
  obj_props[PROP_USER_TIME] =
    g_param_spec_uint ("user-time",
                       "User time",
                       "Timestamp of last user interaction",
                       0,
                       G_MAXUINT,
                       0,
                       G_PARAM_READABLE | G_PARAM_STATIC_STRINGS);
  obj_props[PROP_DEMANDS_ATTENTION] =
    g_param_spec_boolean ("demands-attention",
                          "Demands Attention",
                          "Whether the window has _NET_WM_STATE_DEMANDS_ATTENTION set",
                          FALSE,
                          G_PARAM_READABLE | G_PARAM_STATIC_STRINGS);
  obj_props[PROP_URGENT] =
    g_param_spec_boolean ("urgent",
                          "Urgent",
                          "Whether the urgent flag of WM_HINTS is set",
                          FALSE,
                          G_PARAM_READABLE | G_PARAM_STATIC_STRINGS);
  obj_props[PROP_SKIP_TASKBAR] =
    g_param_spec_boolean ("skip-taskbar",
                          "Skip taskbar",
                          "Whether the skip-taskbar flag of WM_HINTS is set",
                          FALSE,
                          G_PARAM_READABLE | G_PARAM_STATIC_STRINGS);
  obj_props[PROP_MUTTER_HINTS] =
    g_param_spec_string ("mutter-hints",
                         "_MUTTER_HINTS",
                         "Contents of the _MUTTER_HINTS property of this window",
                         NULL,
                         G_PARAM_READABLE | G_PARAM_STATIC_STRINGS);
  obj_props[PROP_APPEARS_FOCUSED] =
    g_param_spec_boolean ("appears-focused",
                          "Appears focused",
                          "Whether the window is drawn as being focused",
                          FALSE,
                          G_PARAM_READABLE | G_PARAM_STATIC_STRINGS);
  obj_props[PROP_RESIZEABLE] =
    g_param_spec_boolean ("resizeable",
                          "Resizeable",
                          "Whether the window can be resized",
                          FALSE,
                          G_PARAM_READABLE | G_PARAM_STATIC_STRINGS);
  obj_props[PROP_ABOVE] =
    g_param_spec_boolean ("above",
                          "Above",
                          "Whether the window is shown as always-on-top",
                          FALSE,
                          G_PARAM_READABLE | G_PARAM_STATIC_STRINGS);
  obj_props[PROP_WM_CLASS] =
    g_param_spec_string ("wm-class",
                         "WM_CLASS",
                         "Contents of the WM_CLASS property of this window",
                         NULL,
                         G_PARAM_READABLE | G_PARAM_STATIC_STRINGS);
  obj_props[PROP_GTK_APPLICATION_ID] =
    g_param_spec_string ("gtk-application-id",
                         "_GTK_APPLICATION_ID",
                         "Contents of the _GTK_APPLICATION_ID property of this window",
                         NULL,
                         G_PARAM_READABLE | G_PARAM_STATIC_STRINGS);
  obj_props[PROP_GTK_UNIQUE_BUS_NAME] =
    g_param_spec_string ("gtk-unique-bus-name",
                         "_GTK_UNIQUE_BUS_NAME",
                         "Contents of the _GTK_UNIQUE_BUS_NAME property of this window",
                         NULL,
                         G_PARAM_READABLE | G_PARAM_STATIC_STRINGS);
  obj_props[PROP_GTK_APPLICATION_OBJECT_PATH] =
    g_param_spec_string ("gtk-application-object-path",
                         "_GTK_APPLICATION_OBJECT_PATH",
                         "Contents of the _GTK_APPLICATION_OBJECT_PATH property of this window",
                         NULL,
                         G_PARAM_READABLE | G_PARAM_STATIC_STRINGS);
  obj_props[PROP_GTK_WINDOW_OBJECT_PATH] =
    g_param_spec_string ("gtk-window-object-path",
                         "_GTK_WINDOW_OBJECT_PATH",
                         "Contents of the _GTK_WINDOW_OBJECT_PATH property of this window",
                         NULL,
                         G_PARAM_READABLE | G_PARAM_STATIC_STRINGS);
  obj_props[PROP_GTK_APP_MENU_OBJECT_PATH] =
    g_param_spec_string ("gtk-app-menu-object-path",
                         "_GTK_APP_MENU_OBJECT_PATH",
                         "Contents of the _GTK_APP_MENU_OBJECT_PATH property of this window",
                         NULL,
                         G_PARAM_READABLE | G_PARAM_STATIC_STRINGS);
  obj_props[PROP_GTK_MENUBAR_OBJECT_PATH] =
    g_param_spec_string ("gtk-menubar-object-path",
                         "_GTK_MENUBAR_OBJECT_PATH",
                         "Contents of the _GTK_MENUBAR_OBJECT_PATH property of this window",
                         NULL,
                         G_PARAM_READABLE | G_PARAM_STATIC_STRINGS);
  obj_props[PROP_ON_ALL_WORKSPACES] =
    g_param_spec_boolean ("on-all-workspaces",
                          "On all workspaces",
                          "Whether the window is set to appear on all workspaces",
                          FALSE,
                          G_PARAM_READABLE | G_PARAM_STATIC_STRINGS);

  g_object_class_install_properties (object_class, LAST_PROP, obj_props);

  window_signals[WORKSPACE_CHANGED] =
    g_signal_new ("workspace-changed",
                  G_TYPE_FROM_CLASS (object_class),
                  G_SIGNAL_RUN_LAST,
                  0,
                  NULL, NULL, NULL,
                  G_TYPE_NONE, 0);

  window_signals[FOCUS] =
    g_signal_new ("focus",
                  G_TYPE_FROM_CLASS (object_class),
                  G_SIGNAL_RUN_LAST,
                  0,
                  NULL, NULL, NULL,
                  G_TYPE_NONE, 0);

  window_signals[RAISED] =
    g_signal_new ("raised",
                  G_TYPE_FROM_CLASS (object_class),
                  G_SIGNAL_RUN_LAST,
                  0,
                  NULL, NULL, NULL,
                  G_TYPE_NONE, 0);

  window_signals[UNMANAGED] =
    g_signal_new ("unmanaged",
                  G_TYPE_FROM_CLASS (object_class),
                  G_SIGNAL_RUN_LAST,
                  0,
                  NULL, NULL, NULL,
                  G_TYPE_NONE, 0);

  /**
   * MetaWindow::position-changed:
   * @window: a #MetaWindow
   *
   * This is emitted when the position of a window might
   * have changed. Specifically, this is emitted when the
   * position of the toplevel window has changed, or when
   * the position of the client window has changed.
   */
  window_signals[POSITION_CHANGED] =
    g_signal_new ("position-changed",
                  G_TYPE_FROM_CLASS (object_class),
                  G_SIGNAL_RUN_LAST,
                  0,
                  NULL, NULL, NULL,
                  G_TYPE_NONE, 0);

  /**
   * MetaWindow::size-changed:
   * @window: a #MetaWindow
   *
   * This is emitted when the position of a window might
   * have changed. Specifically, this is emitted when the
   * size of the toplevel window has changed, or when the
   * size of the client window has changed.
   */
  window_signals[SIZE_CHANGED] =
    g_signal_new ("size-changed",
                  G_TYPE_FROM_CLASS (object_class),
                  G_SIGNAL_RUN_LAST,
                  0,
                  NULL, NULL, NULL,
                  G_TYPE_NONE, 0);
}

static void
meta_window_init (MetaWindow *self)
{
  self->stamp = next_window_stamp++;
  meta_prefs_add_listener (prefs_changed_callback, self);
}

static gboolean
is_desktop_or_dock_foreach (MetaWindow *window,
                            void       *data)
{
  gboolean *result = data;

  *result =
    window->type == META_WINDOW_DESKTOP ||
    window->type == META_WINDOW_DOCK;
  if (*result)
    return FALSE; /* stop as soon as we find one */
  else
    return TRUE;
}

/* window is the window that's newly mapped provoking
 * the possible change
 */
static void
maybe_leave_show_desktop_mode (MetaWindow *window)
{
  gboolean is_desktop_or_dock;

  if (!window->screen->active_workspace->showing_desktop)
    return;

  /* If the window is a transient for the dock or desktop, don't
   * leave show desktop mode when the window opens. That's
   * so you can e.g. hide all windows, manipulate a file on
   * the desktop via a dialog, then unshow windows again.
   */
  is_desktop_or_dock = FALSE;
  is_desktop_or_dock_foreach (window,
                              &is_desktop_or_dock);

  meta_window_foreach_ancestor (window, is_desktop_or_dock_foreach,
                                &is_desktop_or_dock);

  if (!is_desktop_or_dock)
    {
      meta_screen_minimize_all_on_active_workspace_except (window->screen,
                                                           window);
      meta_screen_unshow_desktop (window->screen);
    }
}

gboolean
meta_window_should_attach_to_parent (MetaWindow *window)
{
  MetaWindow *parent;

  if (!meta_prefs_get_attach_modal_dialogs () ||
      window->type != META_WINDOW_MODAL_DIALOG)
    return FALSE;

  parent = meta_window_get_transient_for (window);
  if (!parent)
    return FALSE;

  switch (parent->type)
    {
    case META_WINDOW_NORMAL:
    case META_WINDOW_DIALOG:
    case META_WINDOW_MODAL_DIALOG:
      return TRUE;

    default:
      return FALSE;
    }
}

static gboolean
client_window_should_be_mapped (MetaWindow *window)
{
#ifdef HAVE_WAYLAND
  if (window->client_type == META_WINDOW_CLIENT_TYPE_WAYLAND &&
      !window->surface->buffer)
    return FALSE;
#endif

  return !window->shaded;
}

static void
sync_client_window_mapped (MetaWindow *window)
{
  gboolean should_be_mapped = client_window_should_be_mapped (window);

  g_return_if_fail (!window->override_redirect);

  if (window->mapped == should_be_mapped)
    return;

  window->mapped = should_be_mapped;

  meta_error_trap_push (window->display);
  if (should_be_mapped)
    {
      XMapWindow (window->display->xdisplay, window->xwindow);
    }
  else
    {
      XUnmapWindow (window->display->xdisplay, window->xwindow);
      window->unmaps_pending ++;
    }
  meta_error_trap_pop (window->display);
}

static void
meta_window_update_desc (MetaWindow *window)
{
  g_clear_pointer (&window->desc, g_free);

  if (window->client_type == META_WINDOW_CLIENT_TYPE_X11)
    {
      if (window->title)
        window->desc = g_strdup_printf ("0x%lx (%.10s)", window->xwindow, window->title);
      else
        window->desc = g_strdup_printf ("0x%lx", window->xwindow);
    }
  else
    {
      guint64 small_stamp = window->stamp - G_GUINT64_CONSTANT(0x100000000);

      if (window->title)
        window->desc = g_strdup_printf ("W%" G_GUINT64_FORMAT " (%.10s)", small_stamp, window->title);
      else
        window->desc = g_strdup_printf ("W%" G_GUINT64_FORMAT , small_stamp);
    }
}

static void
meta_window_main_monitor_changed (MetaWindow *window,
                                  const MetaMonitorInfo *old)
{
  META_WINDOW_GET_CLASS (window)->main_monitor_changed (window, old);

  if (old)
    g_signal_emit_by_name (window->screen, "window-left-monitor",
                           old->number, window);
  if (window->monitor)
    g_signal_emit_by_name (window->screen, "window-entered-monitor",
                           window->monitor->number, window);
}

MetaWindow *
_meta_window_shared_new (MetaDisplay         *display,
                         MetaScreen          *screen,
                         MetaWindowClientType client_type,
                         MetaWaylandSurface  *surface,
                         Window               xwindow,
                         gulong               existing_wm_state,
                         MetaCompEffect       effect,
                         XWindowAttributes   *attrs)
{
  MetaWindow *window;

  g_assert (attrs != NULL);

  meta_verbose ("attrs->map_state = %d (%s)\n",
                attrs->map_state,
                (attrs->map_state == IsUnmapped) ?
                "IsUnmapped" :
                (attrs->map_state == IsViewable) ?
                "IsViewable" :
                (attrs->map_state == IsUnviewable) ?
                "IsUnviewable" :
                "(unknown)");

  if (client_type == META_WINDOW_CLIENT_TYPE_X11)
    window = g_object_new (META_TYPE_WINDOW_X11, NULL);
#ifdef HAVE_WAYLAND
  else if (client_type == META_WINDOW_CLIENT_TYPE_WAYLAND)
    window = g_object_new (META_TYPE_WINDOW_WAYLAND, NULL);
#endif
  else
    g_assert_not_reached ();

  window->constructing = TRUE;

  window->dialog_pid = -1;

  window->client_type = client_type;
  window->surface = surface;
  window->xwindow = xwindow;

  /* this is in window->screen->display, but that's too annoying to
   * type
   */
  window->display = display;
  meta_display_register_stamp (window->display, &window->stamp, window);

  window->workspace = NULL;

  window->sync_request_counter = None;
  window->sync_request_serial = 0;
  window->sync_request_timeout_id = 0;
  window->sync_request_alarm = None;

  window->screen = screen;

  meta_window_update_desc (window);

  window->override_redirect = attrs->override_redirect;

  /* avoid tons of stack updates */
  meta_stack_freeze (window->screen->stack);

  window->rect.x = attrs->x;
  window->rect.y = attrs->y;
  window->rect.width = attrs->width;
  window->rect.height = attrs->height;

  /* size_hints are the "request" */
  window->size_hints.x = attrs->x;
  window->size_hints.y = attrs->y;
  window->size_hints.width = attrs->width;
  window->size_hints.height = attrs->height;
  /* initialize the remaining size_hints as if size_hints.flags were zero */
  meta_set_normal_hints (window, NULL);

  /* And this is our unmaximized size */
  window->saved_rect = window->rect;
  window->unconstrained_rect = window->rect;

  window->depth = attrs->depth;
  window->xvisual = attrs->visual;

  window->title = NULL;
  window->icon = NULL;
  window->mini_icon = NULL;

  window->frame = NULL;
  window->has_focus = FALSE;
  window->attached_focus_window = NULL;

  window->maximized_horizontally = FALSE;
  window->maximized_vertically = FALSE;
  window->maximize_horizontally_after_placement = FALSE;
  window->maximize_vertically_after_placement = FALSE;
  window->minimize_after_placement = FALSE;
  window->fullscreen = FALSE;
  window->fullscreen_monitors[0] = -1;
  window->require_fully_onscreen = TRUE;
  window->require_on_single_monitor = TRUE;
  window->require_titlebar_visible = TRUE;
  window->on_all_workspaces = FALSE;
  window->on_all_workspaces_requested = FALSE;
  window->tile_mode = META_TILE_NONE;
  window->tile_monitor_number = -1;
  window->shaded = FALSE;
  window->initially_iconic = FALSE;
  window->minimized = FALSE;
  window->tab_unminimized = FALSE;
  window->iconic = FALSE;
  window->mapped = attrs->map_state != IsUnmapped;
  window->hidden = FALSE;
  window->known_to_compositor = FALSE;
  window->visible_to_compositor = FALSE;
  window->pending_compositor_effect = effect;
  /* if already mapped, no need to worry about focus-on-first-time-showing */
  window->showing_for_first_time = !window->mapped;
  /* if already mapped we don't want to do the placement thing;
   * override-redirect windows are placed by the app */
  window->placed = ((window->mapped && !window->hidden) || window->override_redirect);
  window->denied_focus_and_not_transient = FALSE;
  window->unmanaging = FALSE;
  window->is_in_queues = 0;
  window->keys_grabbed = FALSE;
  window->grab_on_frame = FALSE;
  window->all_keys_grabbed = FALSE;
  window->withdrawn = FALSE;
  window->initial_workspace_set = FALSE;
  window->initial_timestamp_set = FALSE;
  window->net_wm_user_time_set = FALSE;
  window->user_time_window = None;
  window->take_focus = FALSE;
  window->delete_window = FALSE;
  window->can_ping = FALSE;
  window->input = TRUE;
  window->calc_placement = FALSE;
  window->shaken_loose = FALSE;
  window->have_focus_click_grab = FALSE;
  window->disable_sync = FALSE;

  window->unmaps_pending = 0;

  window->mwm_decorated = TRUE;
  window->mwm_border_only = FALSE;
  window->mwm_has_close_func = TRUE;
  window->mwm_has_minimize_func = TRUE;
  window->mwm_has_maximize_func = TRUE;
  window->mwm_has_move_func = TRUE;
  window->mwm_has_resize_func = TRUE;

  if (client_type == META_WINDOW_CLIENT_TYPE_X11)
    window->decorated = TRUE;
  else
    window->decorated = FALSE;

  window->has_close_func = TRUE;
  window->has_minimize_func = TRUE;
  window->has_maximize_func = TRUE;
  window->has_move_func = TRUE;
  window->has_resize_func = TRUE;

  window->has_shade_func = TRUE;

  window->has_fullscreen_func = TRUE;

  window->always_sticky = FALSE;

  window->skip_taskbar = FALSE;
  window->skip_pager = FALSE;
  window->wm_state_above = FALSE;
  window->wm_state_below = FALSE;
  window->wm_state_demands_attention = FALSE;

  window->res_class = NULL;
  window->res_name = NULL;
  window->role = NULL;
  window->sm_client_id = NULL;
  window->wm_client_machine = NULL;
  window->is_remote = FALSE;
  window->startup_id = NULL;

  window->net_wm_pid = -1;

  window->xtransient_for = None;
  window->xclient_leader = None;

  window->type = META_WINDOW_NORMAL;

  window->struts = NULL;

  window->layer = META_LAYER_LAST; /* invalid value */
  window->stack_position = -1;
  window->initial_workspace = 0; /* not used */
  window->initial_timestamp = 0; /* not used */

  window->compositor_private = NULL;

  window->monitor = meta_screen_calculate_monitor_for_window (window->screen,
                                                              window);
  window->preferred_output_winsys_id = window->monitor->winsys_id;

  window->tile_match = NULL;

  /* Assign this #MetaWindow a sequence number which can be used
   * for sorting.
   */
  window->stable_sequence = ++display->window_sequence_counter;

  window->opacity = 0xFF;

  if (window->override_redirect)
    {
      window->decorated = FALSE;
      window->always_sticky = TRUE;
      window->has_close_func = FALSE;
      window->has_shade_func = FALSE;
      window->has_move_func = FALSE;
      window->has_resize_func = FALSE;
    }

  META_WINDOW_GET_CLASS (window)->manage (window);

  if (!window->override_redirect)
    meta_window_update_icon_now (window, TRUE);

  if (window->initially_iconic)
    {
      /* WM_HINTS said minimized */
      window->minimized = TRUE;
      meta_verbose ("Window %s asked to start out minimized\n", window->desc);
    }

  if (existing_wm_state == IconicState)
    {
      /* WM_STATE said minimized */
      window->minimized = TRUE;
      meta_verbose ("Window %s had preexisting WM_STATE = IconicState, minimizing\n",
                    window->desc);

      /* Assume window was previously placed, though perhaps it's
       * been iconic its whole life, we have no way of knowing.
       */
      window->placed = TRUE;
    }

  /* Apply any window attributes such as initial workspace
   * based on startup notification
   */
  meta_screen_apply_startup_properties (window->screen, window);

  /* Try to get a "launch timestamp" for the window.  If the window is
   * a transient, we'd like to be able to get a last-usage timestamp
   * from the parent window.  If the window has no parent, there isn't
   * much we can do...except record the current time so that any children
   * can use this time as a fallback.
   */
  if (!window->override_redirect && !window->net_wm_user_time_set) {
    /* First, maybe the app was launched with startup notification using an
     * obsolete version of the spec; use that timestamp if it exists.
     */
    if (window->initial_timestamp_set)
      /* NOTE: Do NOT toggle net_wm_user_time_set to true; this is just
       * being recorded as a fallback for potential transients
       */
      window->net_wm_user_time = window->initial_timestamp;
    else if (window->transient_for != NULL)
      meta_window_set_user_time (window, window->transient_for->net_wm_user_time);
    else
      /* NOTE: Do NOT toggle net_wm_user_time_set to true; this is just
       * being recorded as a fallback for potential transients
       */
      window->net_wm_user_time =
        meta_display_get_current_time_roundtrip (window->display);
  }

  window->attached = meta_window_should_attach_to_parent (window);
  if (window->attached)
    meta_window_recalc_features (window);

  if (window->type == META_WINDOW_DESKTOP ||
      window->type == META_WINDOW_DOCK)
    {
      /* Change the default, but don't enforce this if the user
       * focuses the dock/desktop and unsticks it using key shortcuts.
       * Need to set this before adding to the workspaces so the MRU
       * lists will be updated.
       */
      window->on_all_workspaces_requested = TRUE;
    }

  window->on_all_workspaces = should_be_on_all_workspaces (window);

  /* For the workspace, first honor hints,
   * if that fails put transients with parents,
   * otherwise put window on active space
   */

  if (window->initial_workspace_set)
    {
      gboolean on_all_workspaces = window->on_all_workspaces;
      MetaWorkspace *workspace = NULL;

      if (window->initial_workspace == (int) 0xFFFFFFFF)
        {
          meta_topic (META_DEBUG_PLACEMENT,
                      "Window %s is initially on all spaces\n",
                      window->desc);

	  /* need to set on_all_workspaces first so that it will be
	   * added to all the MRU lists
	   */
          window->on_all_workspaces_requested = TRUE;

          on_all_workspaces = TRUE;
        }
      else if (!on_all_workspaces)
        {
          meta_topic (META_DEBUG_PLACEMENT,
                      "Window %s is initially on space %d\n",
                      window->desc, window->initial_workspace);

          workspace = meta_screen_get_workspace_by_index (window->screen,
                                                          window->initial_workspace);
        }

      set_workspace_state (window, on_all_workspaces, workspace);
    }

  /* override-redirect windows are subtly different from other windows
   * with window->on_all_workspaces == TRUE. Other windows are part of
   * some workspace (so they can return to that if the flag is turned off),
   * but appear on other workspaces. override-redirect windows are part
   * of no workspace.
   */
  if (!window->override_redirect && window->workspace == NULL)
    {
      if (window->transient_for != NULL)
        {
          meta_topic (META_DEBUG_PLACEMENT,
                      "Putting window %s on same workspace as parent %s\n",
                      window->desc, window->transient_for->desc);

          set_workspace_state (window,
                               window->transient_for->on_all_workspaces_requested,
                               window->transient_for->workspace);
        }

      if (window->on_all_workspaces)
        {
          meta_topic (META_DEBUG_PLACEMENT,
                      "Putting window %s on all workspaces\n",
                      window->desc);

          set_workspace_state (window, TRUE, NULL);
        }
      else
        {
          meta_topic (META_DEBUG_PLACEMENT,
                      "Putting window %s on active workspace\n",
                      window->desc);

          set_workspace_state (window, FALSE, window->screen->active_workspace);
        }

      meta_window_update_struts (window);
    }

  meta_window_main_monitor_changed (window, NULL);

  /* Must add window to stack before doing move/resize, since the
   * window might have fullscreen size (i.e. should have been
   * fullscreen'd; acrobat is one such braindead case; it withdraws
   * and remaps its window whenever trying to become fullscreen...)
   * and thus constraints may try to auto-fullscreen it which also
   * means restacking it.
   */
  if (!window->override_redirect)
    meta_stack_add (window->screen->stack,
                    window);
  else
    window->layer = META_LAYER_OVERRIDE_REDIRECT; /* otherwise set by MetaStack */

  if (!window->override_redirect)
    {
      /* FIXME we have a tendency to set this then immediately
       * change it again.
       */
      set_wm_state (window);
      set_net_wm_state (window);
    }

  meta_compositor_add_window (screen->display->compositor, window);
  window->known_to_compositor = TRUE;

  /* Sync stack changes */
  meta_stack_thaw (window->screen->stack);

  /* Usually the we'll have queued a stack sync anyways, because we've
   * added a new frame window or restacked. But if an undecorated
   * window is mapped, already stacked in the right place, then we
   * might need to do this explicitly.
   */
  meta_stack_tracker_queue_sync_stack (window->screen->stack_tracker);

  /* disable show desktop mode unless we're a desktop component */
  maybe_leave_show_desktop_mode (window);

  meta_window_queue (window, META_QUEUE_CALC_SHOWING);
  /* See bug 303284; a transient of the given window can already exist, in which
   * case we think it should probably be shown.
   */
  meta_window_foreach_transient (window,
                                 queue_calc_showing_func,
                                 NULL);
  /* See bug 334899; the window may have minimized ancestors
   * which need to be shown.
   *
   * However, we shouldn't unminimize windows here when opening
   * a new display because that breaks passing _NET_WM_STATE_HIDDEN
   * between window managers when replacing them; see bug 358042.
   *
   * And we shouldn't unminimize windows if they were initially
   * iconic.
   */
  if (!window->override_redirect &&
      !display->display_opening &&
      !window->initially_iconic)
    unminimize_window_and_all_transient_parents (window);

  window->constructing = FALSE;

  meta_display_notify_window_created (display, window);

  if (window->wm_state_demands_attention)
    g_signal_emit_by_name (window->display, "window-demands-attention", window);

  return window;
}

static gboolean
detach_foreach_func (MetaWindow *window,
                     void       *data)
{
  GList **children = data;
  MetaWindow *parent;

  if (window->attached)
    {
      /* Only return the immediate children of the window being unmanaged */
      parent = meta_window_get_transient_for (window);
      if (parent->unmanaging)
        *children = g_list_prepend (*children, window);
    }

  return TRUE;
}

void
meta_window_unmanage (MetaWindow  *window,
                      guint32      timestamp)
{
  GList *tmp;

  meta_verbose ("Unmanaging %s\n", window->desc);

#ifdef HAVE_WAYLAND
  /* This needs to happen for both Wayland and XWayland clients,
   * so it can't be in MetaWindowWayland. */
  if (window->surface)
    {
      meta_wayland_surface_set_window (window->surface, NULL);
      window->surface = NULL;
    }
#endif

  if (window->visible_to_compositor)
    {
      window->visible_to_compositor = FALSE;
      meta_compositor_hide_window (window->display->compositor, window,
                                   META_COMP_EFFECT_DESTROY);
    }

  meta_compositor_remove_window (window->display->compositor, window);
  window->known_to_compositor = FALSE;

  if (destroying_windows_disallowed > 0)
    meta_bug ("Tried to destroy window %s while destruction was not allowed\n",
              window->desc);

  meta_display_unregister_stamp (window->display, window->stamp);

  window->unmanaging = TRUE;

  if (meta_prefs_get_attach_modal_dialogs ())
    {
      GList *attached_children = NULL, *iter;

      /* Detach any attached dialogs by unmapping and letting them
       * be remapped after @window is destroyed.
       */
      meta_window_foreach_transient (window,
                                     detach_foreach_func,
                                     &attached_children);
      for (iter = attached_children; iter; iter = iter->next)
        meta_window_unmanage (iter->data, timestamp);
      g_list_free (attached_children);
    }

  /* Make sure to only show window on all workspaces if requested, to
   * not confuse other window managers that may take over
   */
  if (window->screen->closing && meta_prefs_get_workspaces_only_on_primary ())
    meta_window_on_all_workspaces_changed (window);

  if (window->fullscreen)
    {
      MetaGroup *group;

      /* If the window is fullscreen, it may be forcing
       * other windows in its group to a higher layer
       */

      meta_stack_freeze (window->screen->stack);
      group = meta_window_get_group (window);
      if (group)
        meta_group_update_layers (group);
      meta_stack_thaw (window->screen->stack);
    }

  meta_display_remove_pending_pings_for_window (window->display, window);

  /* safe to do this early as group.c won't re-add to the
   * group if window->unmanaging */
  meta_window_shutdown_group (window);

  /* If we have the focus, focus some other window.
   * This is done first, so that if the unmap causes
   * an EnterNotify the EnterNotify will have final say
   * on what gets focused, maintaining sloppy focus
   * invariants.
   */
  if (meta_window_appears_focused (window))
    meta_window_propagate_focus_appearance (window, FALSE);
  if (window->has_focus)
    {
      meta_topic (META_DEBUG_FOCUS,
                  "Focusing default window since we're unmanaging %s\n",
                  window->desc);
      meta_workspace_focus_default_window (window->screen->active_workspace, NULL, timestamp);
    }
  else
    {
      meta_topic (META_DEBUG_FOCUS,
                  "Unmanaging window %s which doesn't currently have focus\n",
                  window->desc);
    }

  g_assert (window->display->focus_window != window);

  if (window->struts)
    {
      meta_free_gslist_and_elements (window->struts);
      window->struts = NULL;

      meta_topic (META_DEBUG_WORKAREA,
                  "Unmanaging window %s which has struts, so invalidating work areas\n",
                  window->desc);
      invalidate_work_areas (window);
    }

  if (window->sync_request_timeout_id)
    {
      g_source_remove (window->sync_request_timeout_id);
      window->sync_request_timeout_id = 0;
    }

  if (window->display->grab_window == window)
    meta_display_end_grab_op (window->display, timestamp);

  g_assert (window->display->grab_window != window);

  if (window->maximized_horizontally || window->maximized_vertically)
    unmaximize_window_before_freeing (window);

  meta_window_unqueue (window, META_QUEUE_CALC_SHOWING |
                               META_QUEUE_MOVE_RESIZE |
                               META_QUEUE_UPDATE_ICON);
  meta_window_free_delete_dialog (window);

  set_workspace_state (window, FALSE, NULL);

  g_assert (window->workspace == NULL);

#ifndef G_DISABLE_CHECKS
  tmp = window->screen->workspaces;
  while (tmp != NULL)
    {
      MetaWorkspace *workspace = tmp->data;

      g_assert (g_list_find (workspace->windows, window) == NULL);
      g_assert (g_list_find (workspace->mru_list, window) == NULL);

      tmp = tmp->next;
    }
#endif

  if (window->monitor)
    {
      const MetaMonitorInfo *old = window->monitor;

      window->monitor = NULL;
      meta_window_main_monitor_changed (window, old);
    }

  if (!window->override_redirect)
    meta_stack_remove (window->screen->stack, window);

  /* If an undecorated window is being withdrawn, that will change the
   * stack as presented to the compositing manager, without actually
   * changing the stacking order of X windows.
   */
  meta_stack_tracker_queue_sync_stack (window->screen->stack_tracker);

  if (window->display->autoraise_window == window)
    meta_display_remove_autoraise_callback (window->display);

  META_WINDOW_GET_CLASS (window)->unmanage (window);

  meta_prefs_remove_listener (prefs_changed_callback, window);
  meta_screen_queue_check_fullscreen (window->screen);

  g_signal_emit (window, window_signals[UNMANAGED], 0);

  g_object_unref (window);
}

static void
set_wm_state (MetaWindow *window)
{
  if (window->client_type == META_WINDOW_CLIENT_TYPE_X11)
    meta_window_x11_set_wm_state (window);
}

static void
set_net_wm_state (MetaWindow *window)
{
  if (window->client_type == META_WINDOW_CLIENT_TYPE_X11)
    meta_window_x11_set_net_wm_state (window);
}

static void
set_allowed_actions_hint (MetaWindow *window)
{
  if (window->client_type == META_WINDOW_CLIENT_TYPE_X11)
    meta_window_x11_set_allowed_actions_hint (window);
}

/**
 * meta_window_located_on_workspace:
 * @window: a #MetaWindow
 * @workspace: a #MetaWorkspace
 *
 * Returns: whether @window is displayed on @workspace, or whether it
 * will be displayed on all workspaces.
 */
gboolean
meta_window_located_on_workspace (MetaWindow    *window,
                                  MetaWorkspace *workspace)
{
  return (window->on_all_workspaces) || (window->workspace == workspace);
}

static gboolean
is_minimized_foreach (MetaWindow *window,
                      void       *data)
{
  gboolean *result = data;

  *result = window->minimized;
  if (*result)
    return FALSE; /* stop as soon as we find one */
  else
    return TRUE;
}

static gboolean
ancestor_is_minimized (MetaWindow *window)
{
  gboolean is_minimized;

  is_minimized = FALSE;

  meta_window_foreach_ancestor (window, is_minimized_foreach, &is_minimized);

  return is_minimized;
}

/**
 * meta_window_showing_on_its_workspace:
 * @window: A #MetaWindow
 *
 * Returns: %TRUE if window would be visible, if its workspace was current
 */
gboolean
meta_window_showing_on_its_workspace (MetaWindow *window)
{
  gboolean showing;
  gboolean is_desktop_or_dock;
  MetaWorkspace* workspace_of_window;

  showing = TRUE;

  /* 1. See if we're minimized */
  if (window->minimized)
    showing = FALSE;

  /* 2. See if we're in "show desktop" mode */
  is_desktop_or_dock = FALSE;
  is_desktop_or_dock_foreach (window,
                              &is_desktop_or_dock);

  meta_window_foreach_ancestor (window, is_desktop_or_dock_foreach,
                                &is_desktop_or_dock);

  if (window->on_all_workspaces)
    workspace_of_window = window->screen->active_workspace;
  else if (window->workspace)
    workspace_of_window = window->workspace;
  else /* This only seems to be needed for startup */
    workspace_of_window = NULL;

  if (showing &&
      workspace_of_window && workspace_of_window->showing_desktop &&
      !is_desktop_or_dock)
    {
      meta_verbose ("We're showing the desktop on the workspace(s) that window %s is on\n",
                    window->desc);
      showing = FALSE;
    }

  /* 3. See if an ancestor is minimized (note that
   *    ancestor's "mapped" field may not be up to date
   *    since it's being computed in this same idle queue)
   */

  if (showing)
    {
      if (ancestor_is_minimized (window))
        showing = FALSE;
    }

  return showing;
}

gboolean
meta_window_should_be_showing (MetaWindow  *window)
{
#ifdef HAVE_WAYLAND
  if (window->client_type == META_WINDOW_CLIENT_TYPE_WAYLAND &&
      !window->surface->buffer)
    return FALSE;
#endif

  /* Windows should be showing if they're located on the
   * active workspace and they're showing on their own workspace. */
  return (meta_window_located_on_workspace (window, window->screen->active_workspace) &&
          meta_window_showing_on_its_workspace (window));
}

static void
implement_showing (MetaWindow *window,
                   gboolean    showing)
{
  /* Actually show/hide the window */
  meta_verbose ("Implement showing = %d for window %s\n",
                showing, window->desc);

  if (!showing)
    {
      /* When we manage a new window, we normally delay placing it
       * until it is is first shown, but if we're previewing hidden
       * windows we might want to know where they are on the screen,
       * so we should place the window even if we're hiding it rather
       * than showing it.
       */
      if (!window->placed)
        meta_window_force_placement (window);

      meta_window_hide (window);
    }
  else
    meta_window_show (window);

  if (!window->override_redirect)
    sync_client_window_mapped (window);
}

static void
meta_window_calc_showing (MetaWindow  *window)
{
  implement_showing (window, meta_window_should_be_showing (window));
}

static guint queue_later[NUMBER_OF_QUEUES] = {0, 0, 0};
static GSList *queue_pending[NUMBER_OF_QUEUES] = {NULL, NULL, NULL};

static int
stackcmp (gconstpointer a, gconstpointer b)
{
  MetaWindow *aw = (gpointer) a;
  MetaWindow *bw = (gpointer) b;

  if (aw->screen != bw->screen)
    return 0; /* don't care how they sort with respect to each other */
  else
    return meta_stack_windows_cmp (aw->screen->stack,
                                   aw, bw);
}

static gboolean
idle_calc_showing (gpointer data)
{
  GSList *tmp;
  GSList *copy;
  GSList *should_show;
  GSList *should_hide;
  GSList *unplaced;
  GSList *displays;
  guint queue_index = GPOINTER_TO_INT (data);

  g_return_val_if_fail (queue_pending[queue_index] != NULL, FALSE);

  meta_topic (META_DEBUG_WINDOW_STATE,
              "Clearing the calc_showing queue\n");

  /* Work with a copy, for reentrancy. The allowed reentrancy isn't
   * complete; destroying a window while we're in here would result in
   * badness. But it's OK to queue/unqueue calc_showings.
   */
  copy = g_slist_copy (queue_pending[queue_index]);
  g_slist_free (queue_pending[queue_index]);
  queue_pending[queue_index] = NULL;
  queue_later[queue_index] = 0;

  destroying_windows_disallowed += 1;

  /* We map windows from top to bottom and unmap from bottom to
   * top, to avoid extra expose events. The exception is
   * for unplaced windows, which have to be mapped from bottom to
   * top so placement works.
   */
  should_show = NULL;
  should_hide = NULL;
  unplaced = NULL;
  displays = NULL;

  tmp = copy;
  while (tmp != NULL)
    {
      MetaWindow *window;

      window = tmp->data;

      if (!window->placed)
        unplaced = g_slist_prepend (unplaced, window);
      else if (meta_window_should_be_showing (window))
        should_show = g_slist_prepend (should_show, window);
      else
        should_hide = g_slist_prepend (should_hide, window);

      tmp = tmp->next;
    }

  /* bottom to top */
  unplaced = g_slist_sort (unplaced, stackcmp);
  should_hide = g_slist_sort (should_hide, stackcmp);
  /* top to bottom */
  should_show = g_slist_sort (should_show, stackcmp);
  should_show = g_slist_reverse (should_show);

  tmp = unplaced;
  while (tmp != NULL)
    {
      MetaWindow *window;

      window = tmp->data;

      meta_window_calc_showing (window);

      tmp = tmp->next;
    }

  tmp = should_show;
  while (tmp != NULL)
    {
      MetaWindow *window;

      window = tmp->data;

      implement_showing (window, TRUE);

      tmp = tmp->next;
    }

  tmp = should_hide;
  while (tmp != NULL)
    {
      MetaWindow *window;

      window = tmp->data;

      implement_showing (window, FALSE);

      tmp = tmp->next;
    }

  tmp = copy;
  while (tmp != NULL)
    {
      MetaWindow *window;

      window = tmp->data;

      /* important to set this here for reentrancy -
       * if we queue a window again while it's in "copy",
       * then queue_calc_showing will just return since
       * we are still in the calc_showing queue
       */
      window->is_in_queues &= ~META_QUEUE_CALC_SHOWING;

      tmp = tmp->next;
    }

  if (meta_prefs_get_focus_mode () != G_DESKTOP_FOCUS_MODE_CLICK)
    {
      /* When display->mouse_mode is false, we want to ignore
       * EnterNotify events unless they come from mouse motion.  To do
       * that, we set a sentinel property on the root window if we're
       * not in mouse_mode.
       */
      tmp = should_show;
      while (tmp != NULL)
        {
          MetaWindow *window = tmp->data;

          if (!window->display->mouse_mode)
            meta_display_increment_focus_sentinel (window->display);

          tmp = tmp->next;
        }
    }

  g_slist_free (copy);

  g_slist_free (unplaced);
  g_slist_free (should_show);
  g_slist_free (should_hide);
  g_slist_free (displays);

  destroying_windows_disallowed -= 1;

  return FALSE;
}

#ifdef WITH_VERBOSE_MODE
static const gchar* meta_window_queue_names[NUMBER_OF_QUEUES] =
  {"calc_showing", "move_resize", "update_icon"};
#endif

static void
meta_window_unqueue (MetaWindow *window, guint queuebits)
{
  gint queuenum;

  for (queuenum=0; queuenum<NUMBER_OF_QUEUES; queuenum++)
    {
      if ((queuebits & 1<<queuenum) /* they have asked to unqueue */
          &&
          (window->is_in_queues & 1<<queuenum)) /* it's in the queue */
        {

          meta_topic (META_DEBUG_WINDOW_STATE,
              "Removing %s from the %s queue\n",
              window->desc,
              meta_window_queue_names[queuenum]);

          /* Note that window may not actually be in the queue
           * because it may have been in "copy" inside the idle handler
           */
          queue_pending[queuenum] = g_slist_remove (queue_pending[queuenum], window);
          window->is_in_queues &= ~(1<<queuenum);

          /* Okay, so maybe we've used up all the entries in the queue.
           * In that case, we should kill the function that deals with
           * the queue, because there's nothing left for it to do.
           */
          if (queue_pending[queuenum] == NULL && queue_later[queuenum] != 0)
            {
              meta_later_remove (queue_later[queuenum]);
              queue_later[queuenum] = 0;
            }
        }
    }
}

static void
meta_window_flush_calc_showing (MetaWindow *window)
{
  if (window->is_in_queues & META_QUEUE_CALC_SHOWING)
    {
      meta_window_unqueue (window, META_QUEUE_CALC_SHOWING);
      meta_window_calc_showing (window);
    }
}

void
meta_window_queue (MetaWindow *window, guint queuebits)
{
  guint queuenum;

  /* Easier to debug by checking here rather than in the idle */
  g_return_if_fail (!window->override_redirect || (queuebits & META_QUEUE_MOVE_RESIZE) == 0);

  for (queuenum=0; queuenum<NUMBER_OF_QUEUES; queuenum++)
    {
      if (queuebits & 1<<queuenum)
        {
          /* Data which varies between queues.
           * Yes, these do look a lot like associative arrays:
           * I seem to be turning into a Perl programmer.
           */

          const MetaLaterType window_queue_later_when[NUMBER_OF_QUEUES] =
            {
              META_LATER_CALC_SHOWING, /* CALC_SHOWING */
              META_LATER_RESIZE,        /* MOVE_RESIZE */
              META_LATER_BEFORE_REDRAW  /* UPDATE_ICON */
            };

          const GSourceFunc window_queue_later_handler[NUMBER_OF_QUEUES] =
            {
              idle_calc_showing,
              idle_move_resize,
              idle_update_icon,
            };

          /* If we're about to drop the window, there's no point in putting
           * it on a queue.
           */
          if (window->unmanaging)
            break;

          /* If the window already claims to be in that queue, there's no
           * point putting it in the queue.
           */
          if (window->is_in_queues & 1<<queuenum)
            break;

          meta_topic (META_DEBUG_WINDOW_STATE,
              "Putting %s in the %s queue\n",
              window->desc,
              meta_window_queue_names[queuenum]);

          /* So, mark it as being in this queue. */
          window->is_in_queues |= 1<<queuenum;

          /* There's not a lot of point putting things into a queue if
           * nobody's on the other end pulling them out. Therefore,
           * let's check to see whether an idle handler exists to do
           * that. If not, we'll create one.
           */

          if (queue_later[queuenum] == 0)
            queue_later[queuenum] = meta_later_add
              (
                window_queue_later_when[queuenum],
                window_queue_later_handler[queuenum],
                GUINT_TO_POINTER(queuenum),
                NULL
              );

          /* And now we actually put it on the queue. */
          queue_pending[queuenum] = g_slist_prepend (queue_pending[queuenum],
                                                     window);
      }
  }
}

static gboolean
intervening_user_event_occurred (MetaWindow *window)
{
  guint32 compare;
  MetaWindow *focus_window;

  focus_window = window->display->focus_window;

  meta_topic (META_DEBUG_STARTUP,
              "COMPARISON:\n"
              "  net_wm_user_time_set : %d\n"
              "  net_wm_user_time     : %u\n"
              "  initial_timestamp_set: %d\n"
              "  initial_timestamp    : %u\n",
              window->net_wm_user_time_set,
              window->net_wm_user_time,
              window->initial_timestamp_set,
              window->initial_timestamp);
  if (focus_window != NULL)
    {
      meta_topic (META_DEBUG_STARTUP,
                  "COMPARISON (continued):\n"
                  "  focus_window             : %s\n"
                  "  fw->net_wm_user_time_set : %d\n"
                  "  fw->net_wm_user_time     : %u\n",
                  focus_window->desc,
                  focus_window->net_wm_user_time_set,
                  focus_window->net_wm_user_time);
    }

  /* We expect the most common case for not focusing a new window
   * to be when a hint to not focus it has been set.  Since we can
   * deal with that case rapidly, we use special case it--this is
   * merely a preliminary optimization.  :)
   */
  if ( ((window->net_wm_user_time_set == TRUE) &&
       (window->net_wm_user_time == 0))
      ||
       ((window->initial_timestamp_set == TRUE) &&
       (window->initial_timestamp == 0)))
    {
      meta_topic (META_DEBUG_STARTUP,
                  "window %s explicitly requested no focus\n",
                  window->desc);
      return TRUE;
    }

  if (!(window->net_wm_user_time_set) && !(window->initial_timestamp_set))
    {
      meta_topic (META_DEBUG_STARTUP,
                  "no information about window %s found\n",
                  window->desc);
      return FALSE;
    }

  if (focus_window != NULL &&
      !focus_window->net_wm_user_time_set)
    {
      meta_topic (META_DEBUG_STARTUP,
                  "focus window, %s, doesn't have a user time set yet!\n",
                  window->desc);
      return FALSE;
    }

  /* To determine the "launch" time of an application,
   * startup-notification can set the TIMESTAMP and the
   * application (usually via its toolkit such as gtk or qt) can
   * set the _NET_WM_USER_TIME.  If both are set, we need to be
   * using the newer of the two values.
   *
   * See http://bugzilla.gnome.org/show_bug.cgi?id=573922
   */
  compare = 0;
  if (window->net_wm_user_time_set &&
      window->initial_timestamp_set)
    compare =
      XSERVER_TIME_IS_BEFORE (window->net_wm_user_time,
                              window->initial_timestamp) ?
      window->initial_timestamp : window->net_wm_user_time;
  else if (window->net_wm_user_time_set)
    compare = window->net_wm_user_time;
  else if (window->initial_timestamp_set)
    compare = window->initial_timestamp;

  if ((focus_window != NULL) &&
      XSERVER_TIME_IS_BEFORE (compare, focus_window->net_wm_user_time))
    {
      meta_topic (META_DEBUG_STARTUP,
                  "window %s focus prevented by other activity; %u < %u\n",
                  window->desc,
                  compare,
                  focus_window->net_wm_user_time);
      return TRUE;
    }
  else
    {
      meta_topic (META_DEBUG_STARTUP,
                  "new window %s with no intervening events\n",
                  window->desc);
      return FALSE;
    }
}

/* This function is an ugly hack.  It's experimental in nature and ought to be
 * replaced by a real hint from the app to the WM if we decide the experimental
 * behavior is worthwhile.  The basic idea is to get more feedback about how
 * usage scenarios of "strict" focus users and what they expect.  See #326159.
 */
static gboolean
window_is_terminal (MetaWindow *window)
{
  if (window == NULL || window->res_class == NULL)
    return FALSE;

  /*
   * Compare res_class, which is not user-settable, and thus theoretically
   * a more-reliable indication of term-ness.
   */

  /* gnome-terminal -- if you couldn't guess */
  if (strcmp (window->res_class, "Gnome-terminal") == 0)
    return TRUE;
  /* xterm, rxvt, aterm */
  else if (strcmp (window->res_class, "XTerm") == 0)
    return TRUE;
  /* konsole, KDE's terminal program */
  else if (strcmp (window->res_class, "Konsole") == 0)
    return TRUE;
  /* rxvt-unicode */
  else if (strcmp (window->res_class, "URxvt") == 0)
    return TRUE;
  /* eterm */
  else if (strcmp (window->res_class, "Eterm") == 0)
    return TRUE;
  /* KTerm -- some terminal not KDE based; so not like Konsole */
  else if (strcmp (window->res_class, "KTerm") == 0)
    return TRUE;
  /* Multi-gnome-terminal */
  else if (strcmp (window->res_class, "Multi-gnome-terminal") == 0)
    return TRUE;
  /* mlterm ("multi lingual terminal emulator on X") */
  else if (strcmp (window->res_class, "mlterm") == 0)
    return TRUE;
  /* Terminal -- XFCE Terminal */
  else if (strcmp (window->res_class, "Terminal") == 0)
    return TRUE;

  return FALSE;
}

/* This function determines what state the window should have assuming that it
 * and the focus_window have no relation
 */
static void
window_state_on_map (MetaWindow *window,
                     gboolean *takes_focus,
                     gboolean *places_on_top)
{
  gboolean intervening_events;

  intervening_events = intervening_user_event_occurred (window);

  *takes_focus = !intervening_events;
  *places_on_top = *takes_focus;

  /* don't initially focus windows that are intended to not accept
   * focus
   */
  if (!(window->input || window->take_focus))
    {
      *takes_focus = FALSE;
      return;
    }

  /* Terminal usage may be different; some users intend to launch
   * many apps in quick succession or to just view things in the new
   * window while still interacting with the terminal.  In that case,
   * apps launched from the terminal should not take focus.  This
   * isn't quite the same as not allowing focus to transfer from
   * terminals due to new window map, but the latter is a much easier
   * approximation to enforce so we do that.
   */
  if (*takes_focus &&
      meta_prefs_get_focus_new_windows () == G_DESKTOP_FOCUS_NEW_WINDOWS_STRICT &&
      !window->display->allow_terminal_deactivation &&
      window_is_terminal (window->display->focus_window) &&
      !meta_window_is_ancestor_of_transient (window->display->focus_window,
                                             window))
    {
      meta_topic (META_DEBUG_FOCUS,
                  "focus_window is terminal; not focusing new window.\n");
      *takes_focus = FALSE;
      *places_on_top = FALSE;
    }

  switch (window->type)
    {
    case META_WINDOW_UTILITY:
    case META_WINDOW_TOOLBAR:
      *takes_focus = FALSE;
      *places_on_top = FALSE;
      break;
    case META_WINDOW_DOCK:
    case META_WINDOW_DESKTOP:
    case META_WINDOW_SPLASHSCREEN:
    case META_WINDOW_MENU:
    /* override redirect types: */
    case META_WINDOW_DROPDOWN_MENU:
    case META_WINDOW_POPUP_MENU:
    case META_WINDOW_TOOLTIP:
    case META_WINDOW_NOTIFICATION:
    case META_WINDOW_COMBO:
    case META_WINDOW_DND:
    case META_WINDOW_OVERRIDE_OTHER:
      /* don't focus any of these; places_on_top may be irrelevant for some of
       * these (e.g. dock)--but you never know--the focus window might also be
       * of the same type in some weird situation...
       */
      *takes_focus = FALSE;
      break;
    case META_WINDOW_NORMAL:
    case META_WINDOW_DIALOG:
    case META_WINDOW_MODAL_DIALOG:
      /* The default is correct for these */
      break;
    }
}

static gboolean
windows_overlap (const MetaWindow *w1, const MetaWindow *w2)
{
  MetaRectangle w1rect, w2rect;
  meta_window_get_frame_rect (w1, &w1rect);
  meta_window_get_frame_rect (w2, &w2rect);
  return meta_rectangle_overlap (&w1rect, &w2rect);
}

/* Returns whether a new window would be covered by any
 * existing window on the same workspace that is set
 * to be "above" ("always on top").  A window that is not
 * set "above" would be underneath the new window anyway.
 *
 * We take "covered" to mean even partially covered, but
 * some people might prefer entirely covered.  I think it
 * is more useful to behave this way if any part of the
 * window is covered, because a partial coverage could be
 * (say) ninety per cent and almost indistinguishable from total.
 */
static gboolean
window_would_be_covered (const MetaWindow *newbie)
{
  MetaWorkspace *workspace = meta_window_get_workspace ((MetaWindow *)newbie);
  GList *tmp, *windows;

  windows = meta_workspace_list_windows (workspace);

  tmp = windows;
  while (tmp != NULL)
    {
      MetaWindow *w = tmp->data;

      if (w->wm_state_above && w != newbie)
        {
          /* We have found a window that is "above". Perhaps it overlaps. */
          if (windows_overlap (w, newbie))
            {
              g_list_free (windows); /* clean up... */
              return TRUE; /* yes, it does */
            }
        }

      tmp = tmp->next;
    }

  g_list_free (windows);
  return FALSE; /* none found */
}

static void
meta_window_force_placement (MetaWindow *window)
{
  if (window->placed)
    return;

  /* We have to recalc the placement here since other windows may
   * have been mapped/placed since we last did constrain_position
   */

  /* calc_placement is an efficiency hack to avoid
   * multiple placement calculations before we finally
   * show the window.
   */
  window->calc_placement = TRUE;
  meta_window_move_resize_now (window);
  window->calc_placement = FALSE;

  /* don't ever do the initial position constraint thing again.
   * This is toggled here so that initially-iconified windows
   * still get placed when they are ultimately shown.
   */
  window->placed = TRUE;

  /* Don't want to accidentally reuse the fact that we had been denied
   * focus in any future constraints unless we're denied focus again.
   */
  window->denied_focus_and_not_transient = FALSE;
}

static void
meta_window_show (MetaWindow *window)
{
  gboolean did_show;
  gboolean takes_focus_on_map;
  gboolean place_on_top_on_map;
  gboolean needs_stacking_adjustment;
  MetaWindow *focus_window;
  gboolean notify_demands_attention = FALSE;

  meta_topic (META_DEBUG_WINDOW_STATE,
              "Showing window %s, shaded: %d iconic: %d placed: %d\n",
              window->desc, window->shaded, window->iconic, window->placed);

  focus_window = window->display->focus_window;  /* May be NULL! */
  did_show = FALSE;
  window_state_on_map (window, &takes_focus_on_map, &place_on_top_on_map);
  needs_stacking_adjustment = FALSE;

  meta_topic (META_DEBUG_WINDOW_STATE,
              "Window %s %s focus on map, and %s place on top on map.\n",
              window->desc,
              takes_focus_on_map ? "does" : "does not",
              place_on_top_on_map ? "does" : "does not");

  /* Now, in some rare cases we should *not* put a new window on top.
   * These cases include certain types of windows showing for the first
   * time, and any window which would be covered because of another window
   * being set "above" ("always on top").
   *
   * FIXME: Although "place_on_top_on_map" and "takes_focus_on_map" are
   * generally based on the window type, there is a special case when the
   * focus window is a terminal for them both to be false; this should
   * probably rather be a term in the "if" condition below.
   */

  if ( focus_window != NULL && window->showing_for_first_time &&
      ( (!place_on_top_on_map && !takes_focus_on_map) ||
      window_would_be_covered (window) )
    ) {
      if (meta_window_is_ancestor_of_transient (focus_window, window))
        {
          guint32     timestamp;

          timestamp = meta_display_get_current_time_roundtrip (window->display);

          /* This happens for error dialogs or alerts; these need to remain on
           * top, but it would be confusing to have its ancestor remain
           * focused.
           */
          meta_topic (META_DEBUG_STARTUP,
                      "The focus window %s is an ancestor of the newly mapped "
                      "window %s which isn't being focused.  Unfocusing the "
                      "ancestor.\n",
                      focus_window->desc, window->desc);

          meta_display_focus_the_no_focus_window (window->display,
                                                  window->screen,
                                                  timestamp);
        }
      else
        {
          needs_stacking_adjustment = TRUE;
          if (!window->placed)
            window->denied_focus_and_not_transient = TRUE;
        }
    }

  if (!window->placed)
    {
      if (meta_prefs_get_auto_maximize() && window->showing_for_first_time && window->has_maximize_func)
        {
          MetaRectangle work_area;
          meta_window_get_work_area_for_monitor (window, window->monitor->number, &work_area);
          /* Automaximize windows that map with a size > MAX_UNMAXIMIZED_WINDOW_AREA of the work area */
          if (window->rect.width * window->rect.height > work_area.width * work_area.height * MAX_UNMAXIMIZED_WINDOW_AREA)
            {
              window->maximize_horizontally_after_placement = TRUE;
              window->maximize_vertically_after_placement = TRUE;
            }
        }
      meta_window_force_placement (window);
    }

  if (needs_stacking_adjustment)
    {
      gboolean overlap;

      /* This window isn't getting focus on map.  We may need to do some
       * special handing with it in regards to
       *   - the stacking of the window
       *   - the MRU position of the window
       *   - the demands attention setting of the window
       *
       * Firstly, set the flag so we don't give the window focus anyway
       * and confuse people.
       */

      takes_focus_on_map = FALSE;

      overlap = windows_overlap (window, focus_window);

      /* We want alt tab to go to the denied-focus window */
      ensure_mru_position_after (window, focus_window);

      /* We don't want the denied-focus window to obscure the focus
       * window, and if we're in both click-to-focus mode and
       * raise-on-click mode then we want to maintain the invariant
       * that MRU order == stacking order.  The need for this if
       * comes from the fact that in sloppy/mouse focus the focus
       * window may not overlap other windows and also can be
       * considered "below" them; this combination means that
       * placing the denied-focus window "below" the focus window
       * in the stack when it doesn't overlap it confusingly places
       * that new window below a lot of other windows.
       */
      if (overlap ||
          (meta_prefs_get_focus_mode () == G_DESKTOP_FOCUS_MODE_CLICK &&
           meta_prefs_get_raise_on_click ()))
        meta_window_stack_just_below (window, focus_window);

      /* If the window will be obscured by the focus window, then the
       * user might not notice the window appearing so set the
       * demands attention hint.
       *
       * We set the hint ourselves rather than calling
       * meta_window_set_demands_attention() because that would cause
       * a recalculation of overlap, and a call to set_net_wm_state()
       * which we are going to call ourselves here a few lines down.
       */
      if (overlap)
        {
          if (!window->wm_state_demands_attention)
            {
              window->wm_state_demands_attention = TRUE;
              notify_demands_attention = TRUE;
            }
        }
    }

  if (window->hidden)
    {
      meta_stack_freeze (window->screen->stack);
      window->hidden = FALSE;
      meta_stack_thaw (window->screen->stack);
      did_show = TRUE;
    }

  if (window->iconic)
    {
      window->iconic = FALSE;
      set_wm_state (window);
    }

  if (!window->visible_to_compositor)
    {
      MetaCompEffect effect = META_COMP_EFFECT_NONE;

      window->visible_to_compositor = TRUE;

      switch (window->pending_compositor_effect)
        {
        case META_COMP_EFFECT_CREATE:
        case META_COMP_EFFECT_UNMINIMIZE:
          effect = window->pending_compositor_effect;
          break;
        case META_COMP_EFFECT_NONE:
        case META_COMP_EFFECT_DESTROY:
        case META_COMP_EFFECT_MINIMIZE:
          break;
        }

      meta_compositor_show_window (window->display->compositor,
                                   window, effect);
      window->pending_compositor_effect = META_COMP_EFFECT_NONE;
    }

  /* We don't want to worry about all cases from inside
   * implement_showing(); we only want to worry about focus if this
   * window has not been shown before.
   */
  if (window->showing_for_first_time)
    {
      window->showing_for_first_time = FALSE;
      if (takes_focus_on_map)
        {
          guint32     timestamp;

          timestamp = meta_display_get_current_time_roundtrip (window->display);

          meta_window_focus (window, timestamp);
        }
      else
        {
          /* Prevent EnterNotify events in sloppy/mouse focus from
           * erroneously focusing the window that had been denied
           * focus.  FIXME: This introduces a race; I have a couple
           * ideas for a better way to accomplish the same thing, but
           * they're more involved so do it this way for now.
           */
          meta_display_increment_focus_sentinel (window->display);
        }
    }

  set_net_wm_state (window);

  if (did_show && window->struts)
    {
      meta_topic (META_DEBUG_WORKAREA,
                  "Mapped window %s with struts, so invalidating work areas\n",
                  window->desc);
      invalidate_work_areas (window);
    }

  if (did_show)
    meta_screen_queue_check_fullscreen (window->screen);

#ifdef HAVE_WAYLAND
  if (did_show && window->client_type == META_WINDOW_CLIENT_TYPE_WAYLAND)
    meta_wayland_compositor_repick (meta_wayland_compositor_get_default ());
#endif

  /*
   * Now that we have shown the window, we no longer want to consider the
   * initial timestamp in any subsequent deliberations whether to focus this
   * window or not, so clear the flag.
   *
   * See http://bugzilla.gnome.org/show_bug.cgi?id=573922
   */
  window->initial_timestamp_set = FALSE;

  if (notify_demands_attention)
    {
      g_object_notify_by_pspec (G_OBJECT (window), obj_props[PROP_DEMANDS_ATTENTION]);
      g_signal_emit_by_name (window->display, "window-demands-attention",
                             window);
    }
}

static void
meta_window_hide (MetaWindow *window)
{
  gboolean did_hide;

  meta_topic (META_DEBUG_WINDOW_STATE,
              "Hiding window %s\n", window->desc);

  if (window->visible_to_compositor)
    {
      MetaCompEffect effect = META_COMP_EFFECT_NONE;

      window->visible_to_compositor = FALSE;

      switch (window->pending_compositor_effect)
        {
        case META_COMP_EFFECT_CREATE:
        case META_COMP_EFFECT_UNMINIMIZE:
        case META_COMP_EFFECT_NONE:
          break;
        case META_COMP_EFFECT_DESTROY:
        case META_COMP_EFFECT_MINIMIZE:
          effect = window->pending_compositor_effect;
          break;
        }

      meta_compositor_hide_window (window->display->compositor, window, effect);
      window->pending_compositor_effect = META_COMP_EFFECT_NONE;
    }

  did_hide = FALSE;

  if (!window->hidden)
    {
      meta_stack_freeze (window->screen->stack);
      window->hidden = TRUE;
      meta_stack_thaw (window->screen->stack);

      did_hide = TRUE;
    }

  if (!window->iconic)
    {
      window->iconic = TRUE;
      set_wm_state (window);
    }

  set_net_wm_state (window);

  if (did_hide && window->struts)
    {
      meta_topic (META_DEBUG_WORKAREA,
                  "Unmapped window %s with struts, so invalidating work areas\n",
                  window->desc);
      invalidate_work_areas (window);
    }

  if (window->has_focus)
    {
      MetaWindow *not_this_one = NULL;
      MetaWorkspace *my_workspace = meta_window_get_workspace (window);
      guint32 timestamp = meta_display_get_current_time_roundtrip (window->display);

      /*
       * If this window is modal, passing the not_this_one window to
       * _focus_default_window() makes the focus to be given to this window's
       * ancestor. This can only be the case if the window is on the currently
       * active workspace; when it is not, we need to pass in NULL, so as to
       * focus the default window for the active workspace (this scenario
       * arises when we are switching workspaces).
       * We also pass in NULL if we are in the process of hiding all non-desktop
       * windows to avoid unexpected changes to the stacking order.
       */
      if (my_workspace == window->screen->active_workspace &&
          !my_workspace->showing_desktop)
        not_this_one = window;

      meta_workspace_focus_default_window (window->screen->active_workspace,
                                           not_this_one,
                                           timestamp);
    }

  if (did_hide)
    meta_screen_queue_check_fullscreen (window->screen);
}

static gboolean
queue_calc_showing_func (MetaWindow *window,
                         void       *data)
{
  meta_window_queue(window, META_QUEUE_CALC_SHOWING);
  return TRUE;
}

void
meta_window_minimize (MetaWindow  *window)
{
  g_return_if_fail (!window->override_redirect);

  if (!window->minimized)
    {
      window->minimized = TRUE;
      window->pending_compositor_effect = META_COMP_EFFECT_MINIMIZE;
      meta_window_queue(window, META_QUEUE_CALC_SHOWING);

      meta_window_foreach_transient (window,
                                     queue_calc_showing_func,
                                     NULL);

      if (window->has_focus)
        {
          meta_topic (META_DEBUG_FOCUS,
                      "Focusing default window due to minimization of focus window %s\n",
                      window->desc);
        }
      else
        {
          meta_topic (META_DEBUG_FOCUS,
                      "Minimizing window %s which doesn't have the focus\n",
                      window->desc);
        }

      g_object_notify_by_pspec (G_OBJECT (window), obj_props[PROP_MINIMIZED]);
    }
}

void
meta_window_unminimize (MetaWindow  *window)
{
  g_return_if_fail (!window->override_redirect);

  if (window->minimized)
    {
      window->minimized = FALSE;
      window->pending_compositor_effect = META_COMP_EFFECT_UNMINIMIZE;
      meta_window_queue(window, META_QUEUE_CALC_SHOWING);

      meta_window_foreach_transient (window,
                                     queue_calc_showing_func,
                                     NULL);

      g_object_notify_by_pspec (G_OBJECT (window), obj_props[PROP_MINIMIZED]);
    }
}

static void
ensure_size_hints_satisfied (MetaRectangle    *rect,
                             const XSizeHints *size_hints)
{
  int minw, minh, maxw, maxh;   /* min/max width/height                      */
  int basew, baseh, winc, hinc; /* base width/height, width/height increment */
  int extra_width, extra_height;

  minw  = size_hints->min_width;  minh  = size_hints->min_height;
  maxw  = size_hints->max_width;  maxh  = size_hints->max_height;
  basew = size_hints->base_width; baseh = size_hints->base_height;
  winc  = size_hints->width_inc;  hinc  = size_hints->height_inc;

  /* First, enforce min/max size constraints */
  rect->width  = CLAMP (rect->width,  minw, maxw);
  rect->height = CLAMP (rect->height, minh, maxh);

  /* Now, verify size increment constraints are satisfied, or make them be */
  extra_width  = (rect->width  - basew) % winc;
  extra_height = (rect->height - baseh) % hinc;

  rect->width  -= extra_width;
  rect->height -= extra_height;

  /* Adjusting width/height down, as done above, may violate minimum size
   * constraints, so one last fix.
   */
  if (rect->width  < minw)
    rect->width  += ((minw - rect->width)/winc  + 1)*winc;
  if (rect->height < minh)
    rect->height += ((minh - rect->height)/hinc + 1)*hinc;
}

static void
meta_window_save_rect (MetaWindow *window)
{
  if (!(META_WINDOW_MAXIMIZED (window) || META_WINDOW_TILED_SIDE_BY_SIDE (window) || window->fullscreen))
    {
      /* save size/pos as appropriate args for move_resize */
      if (!window->maximized_horizontally)
        {
          window->saved_rect.x      = window->rect.x;
          window->saved_rect.width  = window->rect.width;
        }
      if (!window->maximized_vertically)
        {
          window->saved_rect.y      = window->rect.y;
          window->saved_rect.height = window->rect.height;
        }
    }
}

void
meta_window_maximize_internal (MetaWindow        *window,
                               MetaMaximizeFlags  directions,
                               MetaRectangle     *saved_rect)
{
  /* At least one of the two directions ought to be set */
  gboolean maximize_horizontally, maximize_vertically;
  maximize_horizontally = directions & META_MAXIMIZE_HORIZONTAL;
  maximize_vertically   = directions & META_MAXIMIZE_VERTICAL;
  g_assert (maximize_horizontally || maximize_vertically);

  meta_topic (META_DEBUG_WINDOW_OPS,
              "Maximizing %s%s\n",
              window->desc,
              maximize_horizontally && maximize_vertically ? "" :
                maximize_horizontally ? " horizontally" :
                  maximize_vertically ? " vertically" : "BUGGGGG");

  if (saved_rect != NULL)
    window->saved_rect = *saved_rect;
  else
    meta_window_save_rect (window);

  if (maximize_horizontally && maximize_vertically)
    window->saved_maximize = TRUE;

  window->maximized_horizontally =
    window->maximized_horizontally || maximize_horizontally;
  window->maximized_vertically =
    window->maximized_vertically   || maximize_vertically;

  meta_window_recalc_features (window);
  set_net_wm_state (window);

  if (window->monitor->in_fullscreen)
    meta_screen_queue_check_fullscreen (window->screen);

  g_object_freeze_notify (G_OBJECT (window));
  g_object_notify_by_pspec (G_OBJECT (window), obj_props[PROP_MAXIMIZED_HORIZONTALLY]);
  g_object_notify_by_pspec (G_OBJECT (window), obj_props[PROP_MAXIMIZED_VERTICALLY]);
  g_object_thaw_notify (G_OBJECT (window));
}

void
meta_window_maximize (MetaWindow        *window,
                      MetaMaximizeFlags  directions)
{
  MetaRectangle *saved_rect = NULL;
  gboolean maximize_horizontally, maximize_vertically;

  g_return_if_fail (!window->override_redirect);

  /* At least one of the two directions ought to be set */
  maximize_horizontally = directions & META_MAXIMIZE_HORIZONTAL;
  maximize_vertically   = directions & META_MAXIMIZE_VERTICAL;
  g_assert (maximize_horizontally || maximize_vertically);

  /* Only do something if the window isn't already maximized in the
   * given direction(s).
   */
  if ((maximize_horizontally && !window->maximized_horizontally) ||
      (maximize_vertically   && !window->maximized_vertically))
    {
      if (window->shaded && maximize_vertically)
        {
          /* Shading sucks anyway; I'm not adding a timestamp argument
           * to this function just for this niche usage & corner case.
           */
          guint32 timestamp =
            meta_display_get_current_time_roundtrip (window->display);
          meta_window_unshade (window, timestamp);
        }

      /* if the window hasn't been placed yet, we'll maximize it then
       */
      if (!window->placed)
	{
	  window->maximize_horizontally_after_placement =
            window->maximize_horizontally_after_placement ||
            maximize_horizontally;
	  window->maximize_vertically_after_placement =
            window->maximize_vertically_after_placement ||
            maximize_vertically;
	  return;
	}

      if (window->tile_mode != META_TILE_NONE)
        {
          saved_rect = &window->saved_rect;

          window->maximized_vertically = FALSE;
          window->tile_mode = META_TILE_NONE;
        }

      meta_window_maximize_internal (window,
                                     directions,
                                     saved_rect);

      MetaRectangle old_frame_rect, old_buffer_rect, new_rect;

      meta_window_get_frame_rect (window, &old_frame_rect);
      meta_window_get_buffer_rect (window, &old_buffer_rect);

      meta_window_move_resize_internal (window,
                                        (META_MOVE_RESIZE_MOVE_ACTION |
                                         META_MOVE_RESIZE_RESIZE_ACTION |
                                         META_MOVE_RESIZE_STATE_CHANGED |
                                         META_MOVE_RESIZE_DONT_SYNC_COMPOSITOR),
                                        NorthWestGravity,
                                        window->unconstrained_rect);
      meta_window_get_frame_rect (window, &new_rect);

      meta_compositor_size_change_window (window->display->compositor, window,
                                          META_SIZE_CHANGE_MAXIMIZE,
                                          &old_frame_rect, &old_buffer_rect);
    }
}

/**
 * meta_window_get_maximized:
 * @window: a #MetaWindow
 *
 * Gets the current maximization state of the window, as combination
 * of the %META_MAXIMIZE_HORIZONTAL and %META_MAXIMIZE_VERTICAL flags;
 *
 * Return value: current maximization state
 */
MetaMaximizeFlags
meta_window_get_maximized (MetaWindow *window)
{
  return ((window->maximized_horizontally ? META_MAXIMIZE_HORIZONTAL : 0) |
          (window->maximized_vertically ? META_MAXIMIZE_VERTICAL : 0));
}

/**
 * meta_window_is_fullscreen:
 * @window: a #MetaWindow
 *
 * Return value: %TRUE if the window is currently fullscreen
 */
gboolean
meta_window_is_fullscreen (MetaWindow *window)
{
  return window->fullscreen;
}

/**
 * meta_window_get_all_monitors:
 * @window: The #MetaWindow
 * @length: (out): gint holding the length, may be %NULL to ignore
 *
 * Returns: (array length=length) (element-type gint) (transfer container):
 *           List of the monitor indices the window is on.
 */
gint *
meta_window_get_all_monitors (MetaWindow *window, gsize *length)
{
  GArray *monitors;
  MetaRectangle window_rect;
  int i;

  monitors = g_array_new (FALSE, FALSE, sizeof (int));
  meta_window_get_frame_rect (window, &window_rect);

  for (i = 0; i < window->screen->n_monitor_infos; i++)
    {
      MetaRectangle *monitor_rect = &window->screen->monitor_infos[i].rect;

      if (meta_rectangle_overlap (&window_rect, monitor_rect))
        g_array_append_val (monitors, i);
    }

  if (length)
    *length = monitors->len;

  i = -1;
  g_array_append_val (monitors, i);

  return (gint*) g_array_free (monitors, FALSE);
}

/**
 * meta_window_is_screen_sized:
 * @window: A #MetaWindow
 *
 * Return value: %TRUE if the window is occupies the
 *               the whole screen (all monitors).
 */
gboolean
meta_window_is_screen_sized (MetaWindow *window)
{
  MetaRectangle window_rect;
  int screen_width, screen_height;

  meta_screen_get_size (window->screen, &screen_width, &screen_height);
  meta_window_get_frame_rect (window, &window_rect);

  if (window_rect.x == 0 && window_rect.y == 0 &&
      window_rect.width == screen_width && window_rect.height == screen_height)
    return TRUE;

  return FALSE;
}

/**
 * meta_window_is_monitor_sized:
 * @window: a #MetaWindow
 *
 * Return value: %TRUE if the window is occupies an entire monitor or
 *               the whole screen.
 */
gboolean
meta_window_is_monitor_sized (MetaWindow *window)
{
  if (window->fullscreen)
    return TRUE;

  if (meta_window_is_screen_sized (window))
    return TRUE;

  if (window->override_redirect)
    {
      MetaRectangle window_rect, monitor_rect;

      meta_window_get_frame_rect (window, &window_rect);
      meta_screen_get_monitor_geometry (window->screen, window->monitor->number, &monitor_rect);

      if (meta_rectangle_equal (&window_rect, &monitor_rect))
        return TRUE;
    }

  return FALSE;
}

/**
 * meta_window_is_on_primary_monitor:
 * @window: a #MetaWindow
 *
 * Return value: %TRUE if the window is on the primary monitor
 */
gboolean
meta_window_is_on_primary_monitor (MetaWindow *window)
{
  return window->monitor->is_primary;
}

/**
 * meta_window_requested_bypass_compositor:
 * @window: a #MetaWindow
 *
 * Return value: %TRUE if the window requested to bypass the compositor
 */
gboolean
meta_window_requested_bypass_compositor (MetaWindow *window)
{
  return window->bypass_compositor == _NET_WM_BYPASS_COMPOSITOR_HINT_ON;
}

/**
 * meta_window_requested_dont_bypass_compositor:
 * @window: a #MetaWindow
 *
 * Return value: %TRUE if the window requested to opt out of unredirecting
 */
gboolean
meta_window_requested_dont_bypass_compositor (MetaWindow *window)
{
  return window->bypass_compositor == _NET_WM_BYPASS_COMPOSITOR_HINT_OFF;
}

void
meta_window_tile (MetaWindow *window)
{
  MetaMaximizeFlags directions;

  /* Don't do anything if no tiling is requested */
  if (window->tile_mode == META_TILE_NONE)
    return;

  if (window->tile_mode == META_TILE_MAXIMIZED)
    directions = META_MAXIMIZE_BOTH;
  else
    directions = META_MAXIMIZE_VERTICAL;

  meta_window_maximize_internal (window, directions, NULL);
  meta_screen_update_tile_preview (window->screen, FALSE);

  meta_window_move_resize_now (window);

  if (window->frame)
    meta_frame_queue_draw (window->frame);
}

static gboolean
meta_window_can_tile_maximized (MetaWindow *window)
{
  return window->has_maximize_func;
}

gboolean
meta_window_can_tile_side_by_side (MetaWindow *window)
{
  int monitor;
  MetaRectangle tile_area;
  MetaRectangle client_rect;

  if (!meta_window_can_tile_maximized (window))
    return FALSE;

  monitor = meta_screen_get_current_monitor (window->screen);
  meta_window_get_work_area_for_monitor (window, monitor, &tile_area);

  /* Do not allow tiling in portrait orientation */
  if (tile_area.height > tile_area.width)
    return FALSE;

  tile_area.width /= 2;

  meta_window_frame_rect_to_client_rect (window, &tile_area, &client_rect);

  return client_rect.width >= window->size_hints.min_width &&
         client_rect.height >= window->size_hints.min_height;
}

static void
unmaximize_window_before_freeing (MetaWindow        *window)
{
  meta_topic (META_DEBUG_WINDOW_OPS,
              "Unmaximizing %s just before freeing\n",
              window->desc);

  window->maximized_horizontally = FALSE;
  window->maximized_vertically = FALSE;

  if (window->withdrawn)                /* See bug #137185 */
    {
      window->rect = window->saved_rect;
      set_net_wm_state (window);
    }
  else if (window->screen->closing)     /* See bug #358042 */
    {
      /* Do NOT update net_wm_state: this screen is closing,
       * it likely will be managed by another window manager
       * that will need the current _NET_WM_STATE atoms.
       * Moreover, it will need to know the unmaximized geometry,
       * therefore move_resize the window to saved_rect here
       * before closing it. */
      meta_window_move_resize_frame (window,
                                     FALSE,
                                     window->saved_rect.x,
                                     window->saved_rect.y,
                                     window->saved_rect.width,
                                     window->saved_rect.height);
    }
}

void
meta_window_unmaximize (MetaWindow        *window,
                        MetaMaximizeFlags  directions)
{
  gboolean unmaximize_horizontally, unmaximize_vertically;
  MetaRectangle new_rect;

  g_return_if_fail (!window->override_redirect);

  /* At least one of the two directions ought to be set */
  unmaximize_horizontally = directions & META_MAXIMIZE_HORIZONTAL;
  unmaximize_vertically   = directions & META_MAXIMIZE_VERTICAL;
  g_assert (unmaximize_horizontally || unmaximize_vertically);

  if (unmaximize_horizontally && unmaximize_vertically)
    window->saved_maximize = FALSE;

  /* Only do something if the window isn't already maximized in the
   * given direction(s).
   */
  if ((unmaximize_horizontally && window->maximized_horizontally) ||
      (unmaximize_vertically   && window->maximized_vertically))
    {
      MetaRectangle *desired_rect;
      MetaRectangle target_rect;
      MetaRectangle work_area;
      MetaRectangle old_frame_rect, old_buffer_rect;

      meta_window_get_work_area_for_monitor (window, window->monitor->number, &work_area);
      meta_window_get_frame_rect (window, &old_frame_rect);
      meta_window_get_buffer_rect (window, &old_buffer_rect);

      meta_topic (META_DEBUG_WINDOW_OPS,
                  "Unmaximizing %s%s\n",
                  window->desc,
                  unmaximize_horizontally && unmaximize_vertically ? "" :
                    unmaximize_horizontally ? " horizontally" :
                      unmaximize_vertically ? " vertically" : "BUGGGGG");

      window->maximized_horizontally =
        window->maximized_horizontally && !unmaximize_horizontally;
      window->maximized_vertically =
        window->maximized_vertically   && !unmaximize_vertically;

      /* recalc_features() will eventually clear the cached frame
       * extents, but we need the correct frame extents in the code below,
       * so invalidate the old frame extents manually up front.
       */
      meta_window_frame_size_changed (window);

      desired_rect = &window->saved_rect;

      /* Unmaximize to the saved_rect position in the direction(s)
       * being unmaximized.
       */
      target_rect = old_frame_rect;

      /* Avoid unmaximizing to "almost maximized" size when the previous size
       * is greater then 80% of the work area use MAX_UNMAXIMIZED_WINDOW_AREA of the work area as upper limit
       * while maintaining the aspect ratio.
       */
      if (unmaximize_horizontally && unmaximize_vertically &&
          desired_rect->width * desired_rect->height > work_area.width * work_area.height * MAX_UNMAXIMIZED_WINDOW_AREA)
        {
          if (desired_rect->width > desired_rect->height)
            {
              float aspect = (float)desired_rect->height / (float)desired_rect->width;
              desired_rect->width = MAX (work_area.width * sqrt (MAX_UNMAXIMIZED_WINDOW_AREA), window->size_hints.min_width);
              desired_rect->height = MAX (desired_rect->width * aspect, window->size_hints.min_height);
            }
          else
            {
              float aspect = (float)desired_rect->width / (float)desired_rect->height;
              desired_rect->height = MAX (work_area.height * sqrt (MAX_UNMAXIMIZED_WINDOW_AREA), window->size_hints.min_height);
              desired_rect->width = MAX (desired_rect->height * aspect, window->size_hints.min_width);
            }
        }

      if (unmaximize_horizontally)
        {
          target_rect.x     = desired_rect->x;
          target_rect.width = desired_rect->width;
        }
      if (unmaximize_vertically)
        {
          target_rect.y      = desired_rect->y;
          target_rect.height = desired_rect->height;
        }

      /* Window's size hints may have changed while maximized, making
       * saved_rect invalid.  #329152
       */
      meta_window_frame_rect_to_client_rect (window, &target_rect, &target_rect);
      ensure_size_hints_satisfied (&target_rect, &window->size_hints);
      meta_window_client_rect_to_frame_rect (window, &target_rect, &target_rect);

      meta_window_move_resize_internal (window,
                                        (META_MOVE_RESIZE_MOVE_ACTION |
                                         META_MOVE_RESIZE_RESIZE_ACTION |
                                         META_MOVE_RESIZE_STATE_CHANGED |
                                         META_MOVE_RESIZE_DONT_SYNC_COMPOSITOR),
                                        NorthWestGravity,
                                        target_rect);

      meta_window_get_frame_rect (window, &new_rect);
      meta_compositor_size_change_window (window->display->compositor, window,
                                          META_SIZE_CHANGE_UNMAXIMIZE,
                                          &old_frame_rect, &old_buffer_rect);

      /* When we unmaximize, if we're doing a mouse move also we could
       * get the window suddenly jumping to the upper left corner of
       * the workspace, since that's where it was when the grab op
       * started.  So we need to update the grab state. We have to do
       * it after the actual operation, as the window may have been moved
       * by constraints.
       */
      if (meta_grab_op_is_moving (window->display->grab_op) &&
          window->display->grab_window == window)
        {
          window->display->grab_anchor_window_pos = window->unconstrained_rect;
        }

      meta_window_recalc_features (window);
      set_net_wm_state (window);
      if (!window->monitor->in_fullscreen)
        meta_screen_queue_check_fullscreen (window->screen);
    }

  g_object_freeze_notify (G_OBJECT (window));
  g_object_notify_by_pspec (G_OBJECT (window), obj_props[PROP_MAXIMIZED_HORIZONTALLY]);
  g_object_notify_by_pspec (G_OBJECT (window), obj_props[PROP_MAXIMIZED_VERTICALLY]);
  g_object_thaw_notify (G_OBJECT (window));
}

void
meta_window_make_above (MetaWindow  *window)
{
  g_return_if_fail (!window->override_redirect);

  meta_window_set_above (window, TRUE);
  meta_window_raise (window);
}

void
meta_window_unmake_above (MetaWindow  *window)
{
  g_return_if_fail (!window->override_redirect);

  meta_window_set_above (window, FALSE);
  meta_window_raise (window);
}

static void
meta_window_set_above (MetaWindow *window,
                       gboolean    new_value)
{
  new_value = new_value != FALSE;
  if (new_value == window->wm_state_above)
    return;

  window->wm_state_above = new_value;
  meta_window_update_layer (window);
  set_net_wm_state (window);
  meta_window_frame_size_changed (window);
  g_object_notify_by_pspec (G_OBJECT (window), obj_props[PROP_ABOVE]);
}

void
meta_window_make_fullscreen_internal (MetaWindow  *window)
{
  if (!window->fullscreen)
    {
      meta_topic (META_DEBUG_WINDOW_OPS,
                  "Fullscreening %s\n", window->desc);

      if (window->shaded)
        {
          /* Shading sucks anyway; I'm not adding a timestamp argument
           * to this function just for this niche usage & corner case.
           */
          guint32 timestamp =
            meta_display_get_current_time_roundtrip (window->display);
          meta_window_unshade (window, timestamp);
        }

      meta_window_save_rect (window);

      window->fullscreen = TRUE;

      meta_stack_freeze (window->screen->stack);
      meta_window_update_layer (window);

      meta_window_raise (window);
      meta_stack_thaw (window->screen->stack);

      meta_window_recalc_features (window);
      set_net_wm_state (window);

      /* For the auto-minimize feature, if we fail to get focus */
      meta_screen_queue_check_fullscreen (window->screen);

      g_object_notify_by_pspec (G_OBJECT (window), obj_props[PROP_FULLSCREEN]);
    }
}

void
meta_window_make_fullscreen (MetaWindow  *window)
{
  g_return_if_fail (!window->override_redirect);

  if (!window->fullscreen)
    {
      MetaRectangle old_frame_rect, old_buffer_rect;

      meta_window_get_frame_rect (window, &old_frame_rect);
      meta_window_get_buffer_rect (window, &old_buffer_rect);

      meta_window_make_fullscreen_internal (window);
      meta_window_move_resize_internal (window,
                                        (META_MOVE_RESIZE_MOVE_ACTION |
                                         META_MOVE_RESIZE_RESIZE_ACTION |
                                         META_MOVE_RESIZE_STATE_CHANGED |
                                         META_MOVE_RESIZE_DONT_SYNC_COMPOSITOR),
                                        NorthWestGravity,
                                        window->unconstrained_rect);

      meta_compositor_size_change_window (window->display->compositor,
                                          window, META_SIZE_CHANGE_FULLSCREEN,
                                          &old_frame_rect, &old_buffer_rect);
    }
}

void
meta_window_unmake_fullscreen (MetaWindow  *window)
{
  g_return_if_fail (!window->override_redirect);

  if (window->fullscreen)
    {
      MetaRectangle old_frame_rect, old_buffer_rect, target_rect;

      meta_topic (META_DEBUG_WINDOW_OPS,
                  "Unfullscreening %s\n", window->desc);

      window->fullscreen = FALSE;
      target_rect = window->saved_rect;

      meta_window_frame_size_changed (window);
      meta_window_get_frame_rect (window, &old_frame_rect);
      meta_window_get_buffer_rect (window, &old_buffer_rect);

      /* Window's size hints may have changed while maximized, making
       * saved_rect invalid.  #329152
       */
      meta_window_frame_rect_to_client_rect (window, &target_rect, &target_rect);
      ensure_size_hints_satisfied (&target_rect, &window->size_hints);
      meta_window_client_rect_to_frame_rect (window, &target_rect, &target_rect);

      /* Need to update window->has_resize_func before we move_resize()
       */
      meta_window_recalc_features (window);
      set_net_wm_state (window);

      meta_window_move_resize_internal (window,
                                        (META_MOVE_RESIZE_MOVE_ACTION |
                                         META_MOVE_RESIZE_RESIZE_ACTION |
                                         META_MOVE_RESIZE_STATE_CHANGED |
                                         META_MOVE_RESIZE_DONT_SYNC_COMPOSITOR),
                                        NorthWestGravity,
                                        target_rect);

      meta_compositor_size_change_window (window->display->compositor,
                                          window, META_SIZE_CHANGE_UNFULLSCREEN,
                                          &old_frame_rect, &old_buffer_rect);

      meta_window_update_layer (window);

      g_object_notify_by_pspec (G_OBJECT (window), obj_props[PROP_FULLSCREEN]);
    }
}

void
meta_window_update_fullscreen_monitors (MetaWindow    *window,
                                        unsigned long  top,
                                        unsigned long  bottom,
                                        unsigned long  left,
                                        unsigned long  right)
{
  if ((int)top < window->screen->n_monitor_infos &&
      (int)bottom < window->screen->n_monitor_infos &&
      (int)left < window->screen->n_monitor_infos &&
      (int)right < window->screen->n_monitor_infos)
    {
      window->fullscreen_monitors[0] = top;
      window->fullscreen_monitors[1] = bottom;
      window->fullscreen_monitors[2] = left;
      window->fullscreen_monitors[3] = right;
    }
  else
    {
      window->fullscreen_monitors[0] = -1;
    }

  if (window->fullscreen)
    {
      meta_window_queue(window, META_QUEUE_MOVE_RESIZE);
    }
}

void
meta_window_shade (MetaWindow  *window,
                   guint32      timestamp)
{
  g_return_if_fail (!window->override_redirect);

  meta_topic (META_DEBUG_WINDOW_OPS,
              "Shading %s\n", window->desc);
  if (!window->shaded)
    {
      window->shaded = TRUE;

      meta_window_queue(window, META_QUEUE_MOVE_RESIZE | META_QUEUE_CALC_SHOWING);
      meta_window_frame_size_changed (window);

      /* After queuing the calc showing, since _focus flushes it,
       * and we need to focus the frame
       */
      meta_topic (META_DEBUG_FOCUS,
                  "Re-focusing window %s after shading it\n",
                  window->desc);
      meta_window_focus (window, timestamp);

      set_net_wm_state (window);
    }
}

void
meta_window_unshade (MetaWindow  *window,
                     guint32      timestamp)
{
  g_return_if_fail (!window->override_redirect);

  meta_topic (META_DEBUG_WINDOW_OPS,
              "Unshading %s\n", window->desc);
  if (window->shaded)
    {
      window->shaded = FALSE;
      meta_window_queue(window, META_QUEUE_MOVE_RESIZE | META_QUEUE_CALC_SHOWING);
      meta_window_frame_size_changed (window);

      /* focus the window */
      meta_topic (META_DEBUG_FOCUS,
                  "Focusing window %s after unshading it\n",
                  window->desc);
      meta_window_focus (window, timestamp);

      set_net_wm_state (window);
    }
}

static gboolean
unminimize_func (MetaWindow *window,
                 void       *data)
{
  meta_window_unminimize (window);
  return TRUE;
}

static void
unminimize_window_and_all_transient_parents (MetaWindow *window)
{
  meta_window_unminimize (window);
  meta_window_foreach_ancestor (window, unminimize_func, NULL);
}

void
meta_window_activate_full (MetaWindow     *window,
                           guint32         timestamp,
                           MetaClientType  source_indication,
                           MetaWorkspace  *workspace)
{
  gboolean allow_workspace_switch;
  meta_topic (META_DEBUG_FOCUS,
              "_NET_ACTIVE_WINDOW message sent for %s at time %u "
              "by client type %u.\n",
              window->desc, timestamp, source_indication);

  allow_workspace_switch = (timestamp != 0);
  if (timestamp != 0 &&
      XSERVER_TIME_IS_BEFORE (timestamp, window->display->last_user_time))
    {
      meta_topic (META_DEBUG_FOCUS,
                  "last_user_time (%u) is more recent; ignoring "
                  " _NET_ACTIVE_WINDOW message.\n",
                  window->display->last_user_time);
      meta_window_set_demands_attention(window);
      return;
    }

  if (timestamp == 0)
    timestamp = meta_display_get_current_time_roundtrip (window->display);

  meta_window_set_user_time (window, timestamp);

  /* disable show desktop mode unless we're a desktop component */
  maybe_leave_show_desktop_mode (window);

  /* Get window on current or given workspace */
  if (workspace == NULL)
    workspace = window->screen->active_workspace;

  /* For non-transient windows, we just set up a pulsing indicator,
     rather than move windows or workspaces.
     See http://bugzilla.gnome.org/show_bug.cgi?id=482354 */
  if (window->transient_for == NULL &&
      !allow_workspace_switch &&
      !meta_window_located_on_workspace (window, workspace))
    {
      meta_window_set_demands_attention (window);
      /* We've marked it as demanding, don't need to do anything else. */
      return;
    }
  else if (window->transient_for != NULL)
    {
      /* Move transients to current workspace - preference dialogs should appear over
         the source window.  */
      meta_window_change_workspace (window, workspace);
    }

  if (window->shaded)
    meta_window_unshade (window, timestamp);

  unminimize_window_and_all_transient_parents (window);

  if (meta_prefs_get_raise_on_click () ||
      source_indication == META_CLIENT_TYPE_PAGER)
    meta_window_raise (window);

  meta_topic (META_DEBUG_FOCUS,
              "Focusing window %s due to activation\n",
              window->desc);

  if (meta_window_located_on_workspace (window, workspace))
    meta_window_focus (window, timestamp);
  else
    meta_workspace_activate_with_focus (window->workspace, window, timestamp);

  meta_window_check_alive (window, timestamp);
}

/* This function exists since most of the functionality in window_activate
 * is useful for Mutter, but Mutter shouldn't need to specify a client
 * type for itself.  ;-)
 */
void
meta_window_activate (MetaWindow     *window,
                      guint32         timestamp)
{
  g_return_if_fail (!window->override_redirect);

  /* We're not really a pager, but the behavior we want is the same as if
   * we were such.  If we change the pager behavior later, we could revisit
   * this and just add extra flags to window_activate.
   */
  meta_window_activate_full (window, timestamp, META_CLIENT_TYPE_PAGER, NULL);
}

void
meta_window_activate_with_workspace (MetaWindow     *window,
                                     guint32         timestamp,
                                     MetaWorkspace  *workspace)
{
  g_return_if_fail (!window->override_redirect);

  meta_window_activate_full (window, timestamp, META_CLIENT_TYPE_APPLICATION, workspace);
}

/**
 * meta_window_updates_are_frozen:
 * @window: a #MetaWindow
 *
 * Gets whether the compositor should be updating the window contents;
 * window content updates may be frozen at client request by setting
 * an odd value in the extended _NET_WM_SYNC_REQUEST_COUNTER counter
 * by the window manager during a resize operation while waiting for
 * the client to redraw.
 *
 * Return value: %TRUE if updates are currently frozen
 */
gboolean
meta_window_updates_are_frozen (MetaWindow *window)
{
  if (window->extended_sync_request_counter &&
      window->sync_request_serial % 2 == 1)
    return TRUE;

  if (window->sync_request_serial < window->sync_request_wait_serial)
    return TRUE;

  return FALSE;
}

static gboolean
maybe_move_attached_dialog (MetaWindow *window,
                            void       *data)
{
  if (meta_window_is_attached_dialog (window))
    /* It ignores x,y for such a dialog  */
    meta_window_move_frame (window, FALSE, 0, 0);

  return FALSE;
}

/**
 * meta_window_get_monitor:
 * @window: a #MetaWindow
 *
 * Gets index of the monitor that this window is on.
 *
 * Return Value: The index of the monitor in the screens monitor list
 */
int
meta_window_get_monitor (MetaWindow *window)
{
  return window->monitor->number;
}

static MetaMonitorInfo *
find_monitor_by_winsys_id (MetaWindow *window,
                           guint       winsys_id)
{
  int i;

  for (i = 0; i < window->screen->n_monitor_infos; i++)
    {
      MetaMonitorInfo *info = &window->screen->monitor_infos[i];

      if (info->winsys_id == winsys_id)
        return info;
    }

  return NULL;
}

/* This is called when the monitor setup has changed. The window->monitor
 * reference is still "valid", but refer to the previous monitor setup */
void
meta_window_update_for_monitors_changed (MetaWindow *window)
{
  const MetaMonitorInfo *old, *new;

  if (window->override_redirect || window->type == META_WINDOW_DESKTOP)
    {
      meta_window_update_monitor (window, FALSE);
      return;
    }

  old = window->monitor;

  /* Try the preferred output first */
  new = find_monitor_by_winsys_id (window, window->preferred_output_winsys_id);

  /* Otherwise, try to find the old output on a new monitor */
  if (!new)
    new = find_monitor_by_winsys_id (window, old->winsys_id);

  /* Fall back to primary if everything else failed */
  if (!new)
    new = &window->screen->monitor_infos[window->screen->primary_monitor_index];

  if (window->tile_mode != META_TILE_NONE)
    window->tile_monitor_number = new->number;

  /* This will eventually reach meta_window_update_monitor that
   * will send leave/enter-monitor events. The old != new monitor
   * check will always fail (due to the new monitor_infos set) so
   * we will always send the events, even if the new and old monitor
   * index is the same. That is right, since the enumeration of the
   * monitors changed and the same index could be refereing
   * to a different monitor. */
  meta_window_move_between_rects (window,
                                  &old->rect,
                                  &new->rect);
}

void
meta_window_update_monitor (MetaWindow *window,
                            gboolean    user_op)
{
  const MetaMonitorInfo *old;

  old = window->monitor;
  META_WINDOW_GET_CLASS (window)->update_main_monitor (window);
  if (old != window->monitor)
    {
      meta_window_on_all_workspaces_changed (window);

      /* If workspaces only on primary and we moved back to primary due to a user action,
       * ensure that the window is now in that workspace. We do this because while
       * the window is on a non-primary monitor it is always visible, so it would be
       * very jarring if it disappeared when it crossed the monitor border.
       * The one time we want it to both change to the primary monitor and a non-active
       * workspace is when dropping the window on some other workspace thumbnail directly.
       * That should be handled by explicitly moving the window before changing the
       * workspace.
       */
      if (meta_prefs_get_workspaces_only_on_primary () && user_op &&
          meta_window_is_on_primary_monitor (window)  &&
          window->screen->active_workspace != window->workspace)
        meta_window_change_workspace (window, window->screen->active_workspace);

      meta_window_main_monitor_changed (window, old);

      /* If we're changing monitors, we need to update the has_maximize_func flag,
       * as the working area has changed. */
      meta_window_recalc_features (window);
    }
}

void
meta_window_move_resize_internal (MetaWindow          *window,
                                  MetaMoveResizeFlags  flags,
                                  int                  gravity,
                                  MetaRectangle        frame_rect)
{
  /* The rectangle here that's passed in *always* in "frame rect"
   * coordinates. That means the position of the frame's visible bounds,
   * with x and y being absolute (root window) coordinates.
   *
   * For an X11 framed window, the client window's server rectangle is
   * inset from this rectangle by the frame's visible borders, and the
   * frame window's server rectangle is outset by the invisible borders.
   *
   * For an X11 unframed window, the rectangle here directly matches
   * the server's rectangle, since the visible and invisible borders
   * are both 0.
   *
   * For an X11 CSD window, the client window's server rectangle is
   * outset from this rectagle by the client-specified frame extents.
   *
   * For a Wayland window, this rectangle can simply be sent directly
   * to the client.
   */

  gboolean did_placement;
  guint old_output_winsys_id;
  MetaRectangle unconstrained_rect;
  MetaRectangle constrained_rect;
  MetaMoveResizeResultFlags result = 0;

  g_return_if_fail (!window->override_redirect);

  /* The action has to be a move, a resize or the wayland client
   * acking our choice of size.
   */
  g_assert (flags & (META_MOVE_RESIZE_MOVE_ACTION | META_MOVE_RESIZE_RESIZE_ACTION | META_MOVE_RESIZE_WAYLAND_RESIZE));

  did_placement = !window->placed && window->calc_placement;

  /* We don't need it in the idle queue anymore. */
  meta_window_unqueue (window, META_QUEUE_MOVE_RESIZE);

  if ((flags & META_MOVE_RESIZE_RESIZE_ACTION) && (flags & META_MOVE_RESIZE_MOVE_ACTION))
    {
      /* We're both moving and resizing. Just use the passed in rect. */
      unconstrained_rect = frame_rect;
    }
  else if ((flags & META_MOVE_RESIZE_RESIZE_ACTION))
    {
      /* If this is only a resize, then ignore the position given in
       * the parameters and instead calculate the new position from
       * resizing the old rectangle with the given gravity. */
      meta_rectangle_resize_with_gravity (&window->rect,
                                          &unconstrained_rect,
                                          gravity,
                                          frame_rect.width,
                                          frame_rect.height);
    }
  else if ((flags & META_MOVE_RESIZE_MOVE_ACTION))
    {
      /* If this is only a move, then ignore the passed in size and
       * just use the existing size of the window. */
      unconstrained_rect.x = frame_rect.x;
      unconstrained_rect.y = frame_rect.y;
      unconstrained_rect.width = window->rect.width;
      unconstrained_rect.height = window->rect.height;
    }
  else if ((flags & META_MOVE_RESIZE_WAYLAND_RESIZE))
    {
      /* This is a Wayland buffer acking our size. The new rect is
       * just the existing one we have. Ignore the passed-in rect
       * completely. */
      unconstrained_rect = window->rect;
    }
  else
    g_assert_not_reached ();

  constrained_rect = unconstrained_rect;
  if (flags & (META_MOVE_RESIZE_MOVE_ACTION | META_MOVE_RESIZE_RESIZE_ACTION))
    {
      MetaRectangle old_rect;
      meta_window_get_frame_rect (window, &old_rect);

      meta_window_constrain (window,
                             flags,
                             gravity,
                             &old_rect,
                             &constrained_rect);
    }

  /* If we did placement, then we need to save the position that the window
   * was placed at to make sure that meta_window_move_resize_now places the
   * window correctly.
   */
  if (did_placement)
    {
      unconstrained_rect.x = constrained_rect.x;
      unconstrained_rect.y = constrained_rect.y;
    }

  /* Do the protocol-specific move/resize logic */
  META_WINDOW_GET_CLASS (window)->move_resize_internal (window, gravity, unconstrained_rect, constrained_rect, flags, &result);

  if (result & META_MOVE_RESIZE_RESULT_MOVED)
    g_signal_emit (window, window_signals[POSITION_CHANGED], 0);

  if (result & META_MOVE_RESIZE_RESULT_RESIZED)
    g_signal_emit (window, window_signals[SIZE_CHANGED], 0);

  if ((result & (META_MOVE_RESIZE_RESULT_MOVED | META_MOVE_RESIZE_RESULT_RESIZED)) != 0 || did_placement)
    {
      window->unconstrained_rect = unconstrained_rect;

      if (window->known_to_compositor && !(flags & META_MOVE_RESIZE_DONT_SYNC_COMPOSITOR))
        meta_compositor_sync_window_geometry (window->display->compositor,
                                              window,
                                              did_placement);
    }

  old_output_winsys_id = window->monitor->winsys_id;

  meta_window_update_monitor (window, flags & META_MOVE_RESIZE_USER_ACTION);

  if (old_output_winsys_id != window->monitor->winsys_id &&
      flags & META_MOVE_RESIZE_MOVE_ACTION && flags & META_MOVE_RESIZE_USER_ACTION)
    window->preferred_output_winsys_id = window->monitor->winsys_id;

  if ((result & META_MOVE_RESIZE_RESULT_FRAME_SHAPE_CHANGED) && window->frame_bounds)
    {
      cairo_region_destroy (window->frame_bounds);
      window->frame_bounds = NULL;
    }

  meta_window_foreach_transient (window, maybe_move_attached_dialog, NULL);

  meta_stack_update_window_tile_matches (window->screen->stack,
                                         window->screen->active_workspace);
}

/**
 * meta_window_move_frame:
 * @window: a #MetaWindow
 * @user_op: bool to indicate whether or not this is a user operation
 * @root_x_nw: desired x pos
 * @root_y_nw: desired y pos
 *
 * Moves the window to the desired location on window's assigned
 * workspace, using the northwest edge of the frame as the reference,
 * instead of the actual window's origin, but only if a frame is present.
 * Otherwise, acts identically to meta_window_move().
 */
void
meta_window_move_frame (MetaWindow *window,
                        gboolean    user_op,
                        int         root_x_nw,
                        int         root_y_nw)
{
  MetaMoveResizeFlags flags;
  MetaRectangle rect = { root_x_nw, root_y_nw, 0, 0 };

  g_return_if_fail (!window->override_redirect);

  flags = (user_op ? META_MOVE_RESIZE_USER_ACTION : 0) | META_MOVE_RESIZE_MOVE_ACTION;
  meta_window_move_resize_internal (window, flags, NorthWestGravity, rect);
}

static void
meta_window_move_between_rects (MetaWindow  *window,
                                const MetaRectangle *old_area,
                                const MetaRectangle *new_area)
{
  int rel_x, rel_y;
  double scale_x, scale_y;

  rel_x = window->unconstrained_rect.x - old_area->x;
  rel_y = window->unconstrained_rect.y - old_area->y;
  scale_x = (double)new_area->width / old_area->width;
  scale_y = (double)new_area->height / old_area->height;

  window->unconstrained_rect.x = new_area->x + rel_x * scale_x;
  window->unconstrained_rect.y = new_area->y + rel_y * scale_y;
  window->saved_rect.x = window->unconstrained_rect.x;
  window->saved_rect.y = window->unconstrained_rect.y;

  meta_window_move_resize_now (window);
}

/**
 * meta_window_move_resize_frame:
 * @window: a #MetaWindow
 * @user_op: bool to indicate whether or not this is a user operation
 * @root_x_nw: new x
 * @root_y_nw: new y
 * @w: desired width
 * @h: desired height
 *
 * Resizes the window so that its outer bounds (including frame)
 * fit within the given rect
 */
void
meta_window_move_resize_frame (MetaWindow  *window,
                               gboolean     user_op,
                               int          root_x_nw,
                               int          root_y_nw,
                               int          w,
                               int          h)
{
  MetaMoveResizeFlags flags;
  MetaRectangle rect = { root_x_nw, root_y_nw, w, h };

  g_return_if_fail (!window->override_redirect);

  flags = (user_op ? META_MOVE_RESIZE_USER_ACTION : 0) | META_MOVE_RESIZE_MOVE_ACTION | META_MOVE_RESIZE_RESIZE_ACTION;

  meta_window_move_resize_internal (window, flags, NorthWestGravity, rect);
}

/**
 * meta_window_move_to_monitor:
 * @window: a #MetaWindow
 * @monitor: desired monitor index
 *
 * Moves the window to the monitor with index @monitor, keeping
 * the relative position of the window's top left corner.
 */
void
meta_window_move_to_monitor (MetaWindow  *window,
                             int          monitor)
{
  MetaRectangle old_area, new_area;

  if (monitor == window->monitor->number)
    return;

  meta_window_get_work_area_for_monitor (window,
                                         window->monitor->number,
                                         &old_area);
  meta_window_get_work_area_for_monitor (window,
                                         monitor,
                                         &new_area);

  if (window->tile_mode != META_TILE_NONE)
    window->tile_monitor_number = monitor;

  meta_window_move_between_rects (window, &old_area, &new_area);
  window->preferred_output_winsys_id = window->monitor->winsys_id;

  if (window->fullscreen || window->override_redirect)
    meta_screen_queue_check_fullscreen (window->screen);
}

void
meta_window_resize_frame_with_gravity (MetaWindow *window,
                                       gboolean     user_op,
                                       int          w,
                                       int          h,
                                       int          gravity)
{
  MetaMoveResizeFlags flags;
  MetaRectangle rect;

  rect.width = w;
  rect.height = h;

  flags = (user_op ? META_MOVE_RESIZE_USER_ACTION : 0) | META_MOVE_RESIZE_RESIZE_ACTION;
  meta_window_move_resize_internal (window, flags, gravity, rect);
}

static void
meta_window_move_resize_now (MetaWindow  *window)
{
  meta_window_move_resize_frame (window, FALSE,
                                 window->unconstrained_rect.x,
                                 window->unconstrained_rect.y,
                                 window->unconstrained_rect.width,
                                 window->unconstrained_rect.height);
}

static gboolean
idle_move_resize (gpointer data)
{
  GSList *tmp;
  GSList *copy;
  guint queue_index = GPOINTER_TO_INT (data);

  meta_topic (META_DEBUG_GEOMETRY, "Clearing the move_resize queue\n");

  /* Work with a copy, for reentrancy. The allowed reentrancy isn't
   * complete; destroying a window while we're in here would result in
   * badness. But it's OK to queue/unqueue move_resizes.
   */
  copy = g_slist_copy (queue_pending[queue_index]);
  g_slist_free (queue_pending[queue_index]);
  queue_pending[queue_index] = NULL;
  queue_later[queue_index] = 0;

  destroying_windows_disallowed += 1;

  tmp = copy;
  while (tmp != NULL)
    {
      MetaWindow *window;

      window = tmp->data;

      /* As a side effect, sets window->move_resize_queued = FALSE */
      meta_window_move_resize_now (window);

      tmp = tmp->next;
    }

  g_slist_free (copy);

  destroying_windows_disallowed -= 1;

  return FALSE;
}

void
meta_window_get_gravity_position (MetaWindow  *window,
                                  int          gravity,
                                  int         *root_x,
                                  int         *root_y)
{
  MetaRectangle frame_extents;
  int w, h;
  int x, y;

  w = window->rect.width;
  h = window->rect.height;

  if (gravity == StaticGravity)
    {
      frame_extents = window->rect;
      if (window->frame)
        {
          frame_extents.x = window->frame->rect.x + window->frame->child_x;
          frame_extents.y = window->frame->rect.y + window->frame->child_y;
        }
    }
  else
    {
      if (window->frame == NULL)
        frame_extents = window->rect;
      else
        frame_extents = window->frame->rect;
    }

  x = frame_extents.x;
  y = frame_extents.y;

  switch (gravity)
    {
    case NorthGravity:
    case CenterGravity:
    case SouthGravity:
      /* Find center of frame. */
      x += frame_extents.width / 2;
      /* Center client window on that point. */
      x -= w / 2;
      break;

    case SouthEastGravity:
    case EastGravity:
    case NorthEastGravity:
      /* Find right edge of frame */
      x += frame_extents.width;
      /* Align left edge of client at that point. */
      x -= w;
      break;
    default:
      break;
    }

  switch (gravity)
    {
    case WestGravity:
    case CenterGravity:
    case EastGravity:
      /* Find center of frame. */
      y += frame_extents.height / 2;
      /* Center client window there. */
      y -= h / 2;
      break;
    case SouthWestGravity:
    case SouthGravity:
    case SouthEastGravity:
      /* Find south edge of frame */
      y += frame_extents.height;
      /* Place bottom edge of client there */
      y -= h;
      break;
    default:
      break;
    }

  if (root_x)
    *root_x = x;
  if (root_y)
    *root_y = y;
}

void
meta_window_get_session_geometry (MetaWindow  *window,
                                  int         *x,
                                  int         *y,
                                  int         *width,
                                  int         *height)
{
  meta_window_get_gravity_position (window,
                                    window->size_hints.win_gravity,
                                    x, y);

  *width = (window->rect.width - window->size_hints.base_width) /
    window->size_hints.width_inc;
  *height = (window->rect.height - window->size_hints.base_height) /
    window->size_hints.height_inc;
}

/**
 * meta_window_get_buffer_rect:
 * @window: a #MetaWindow
 * @rect: (out): pointer to an allocated #MetaRectangle
 *
 * Gets the rectangle that the pixmap or buffer of @window occupies.
 *
 * For X11 windows, this is the server-side geometry of the toplevel
 * window.
 *
 * For Wayland windows, this is the bounding rectangle of the attached
 * buffer.
 */
void
meta_window_get_buffer_rect (const MetaWindow *window,
                             MetaRectangle    *rect)
{
  *rect = window->buffer_rect;
}

/**
 * meta_window_client_rect_to_frame_rect:
 * @window: a #MetaWindow
 * @client_rect: client rectangle in root coordinates
 * @frame_rect: (out): location to store the computed corresponding frame bounds.
 *
 * Converts a desired bounds of the client window into the corresponding bounds
 * of the window frame (excluding invisible borders and client side shadows.)
 */
void
meta_window_client_rect_to_frame_rect (MetaWindow    *window,
                                       MetaRectangle *client_rect,
                                       MetaRectangle *frame_rect)
{
  if (!frame_rect)
    return;

  *frame_rect = *client_rect;

  /* The support for G_MAXINT here to mean infinity is a convenience for
   * constraints.c:get_size_limits() and not something that we provide
   * in other locations or document.
   */
  if (window->frame)
    {
      MetaFrameBorders borders;
      meta_frame_calc_borders (window->frame, &borders);

      frame_rect->x -= borders.visible.left;
      frame_rect->y -= borders.visible.top;
      if (frame_rect->width != G_MAXINT)
        frame_rect->width += borders.visible.left + borders.visible.right;
      if (frame_rect->height != G_MAXINT)
        frame_rect->height += borders.visible.top  + borders.visible.bottom;
    }
  else
    {
      const GtkBorder *extents = &window->custom_frame_extents;
      frame_rect->x += extents->left;
      frame_rect->y += extents->top;
      if (frame_rect->width != G_MAXINT)
        frame_rect->width -= extents->left + extents->right;
      if (frame_rect->height != G_MAXINT)
        frame_rect->height -= extents->top + extents->bottom;
    }
}

/**
 * meta_window_frame_rect_to_client_rect:
 * @window: a #MetaWindow
 * @frame_rect: desired frame bounds for the window
 * @client_rect: (out): location to store the computed corresponding client rectangle.
 *
 * Converts a desired frame bounds for a window into the bounds of the client
 * window.
 */
void
meta_window_frame_rect_to_client_rect (MetaWindow    *window,
                                       MetaRectangle *frame_rect,
                                       MetaRectangle *client_rect)
{
  if (!client_rect)
    return;

  *client_rect = *frame_rect;

  if (window->frame)
    {
      MetaFrameBorders borders;
      meta_frame_calc_borders (window->frame, &borders);

      client_rect->x += borders.visible.left;
      client_rect->y += borders.visible.top;
      client_rect->width  -= borders.visible.left + borders.visible.right;
      client_rect->height -= borders.visible.top  + borders.visible.bottom;
    }
  else
    {
      const GtkBorder *extents = &window->custom_frame_extents;
      client_rect->x -= extents->left;
      client_rect->y -= extents->top;
      client_rect->width += extents->left + extents->right;
      client_rect->height += extents->top + extents->bottom;
    }
}

/**
 * meta_window_get_frame_rect:
 * @window: a #MetaWindow
 * @rect: (out): pointer to an allocated #MetaRectangle
 *
 * Gets the rectangle that bounds @window that is what the user thinks of
 * as the edge of the window. This doesn't include any extra reactive
 * area that we or the client adds to the window, or any area that the
 * client adds to draw a client-side shadow.
 */
void
meta_window_get_frame_rect (const MetaWindow *window,
                            MetaRectangle    *rect)
{
  *rect = window->rect;
}

/**
 * meta_window_get_client_area_rect:
 * @window: a #MetaWindow
 * @rect: (out): pointer to a cairo rectangle
 *
 * Gets the rectangle for the boundaries of the client area, relative
 * to the buffer rect. If the window is shaded, the height of the
 * rectangle is 0.
 */
void
meta_window_get_client_area_rect (const MetaWindow      *window,
                                  cairo_rectangle_int_t *rect)
{
  MetaFrameBorders borders;

  meta_frame_calc_borders (window->frame, &borders);

  rect->x = borders.total.left;
  rect->y = borders.total.top;

  rect->width = window->buffer_rect.width - borders.total.left - borders.total.right;
  if (window->shaded)
    rect->height = 0;
  else
    rect->height = window->buffer_rect.height - borders.total.top - borders.total.bottom;
}

void
meta_window_get_titlebar_rect (MetaWindow    *window,
                               MetaRectangle *rect)
{
  meta_window_get_frame_rect (window, rect);

  /* The returned rectangle is relative to the frame rect. */
  rect->x = 0;
  rect->y = 0;

  if (window->frame)
    {
      rect->height = window->frame->child_y;
    }
  else
    {
      /* Pick an arbitrary height for a titlebar. We might want to
       * eventually have CSD windows expose their borders to us. */
      rect->height = 50;
    }
}

const char*
meta_window_get_startup_id (MetaWindow *window)
{
  if (window->startup_id == NULL)
    {
      MetaGroup *group;

      group = meta_window_get_group (window);

      if (group != NULL)
        return meta_group_get_startup_id (group);
    }

  return window->startup_id;
}

static MetaWindow*
get_modal_transient (MetaWindow *window)
{
  GSList *windows;
  GSList *tmp;
  MetaWindow *modal_transient;

  /* A window can't be the transient of itself, but this is just for
   * convenience in the loop below; we manually fix things up at the
   * end if no real modal transient was found.
   */
  modal_transient = window;

  windows = meta_display_list_windows (window->display, META_LIST_DEFAULT);
  tmp = windows;
  while (tmp != NULL)
    {
      MetaWindow *transient = tmp->data;

      if (transient->transient_for == modal_transient &&
          transient->type == META_WINDOW_MODAL_DIALOG)
        {
          modal_transient = transient;
          tmp = windows;
          continue;
        }

      tmp = tmp->next;
    }

  g_slist_free (windows);

  if (window == modal_transient)
    modal_transient = NULL;

  return modal_transient;
}

/* XXX META_EFFECT_FOCUS */
void
meta_window_focus (MetaWindow  *window,
                   guint32      timestamp)
{
  MetaWindow *modal_transient;

  g_return_if_fail (!window->override_redirect);

  meta_topic (META_DEBUG_FOCUS,
              "Setting input focus to window %s, input: %d take_focus: %d\n",
              window->desc, window->input, window->take_focus);

  if (window->display->grab_window &&
      window->display->grab_window->all_keys_grabbed &&
      !window->display->grab_window->unmanaging)
    {
      meta_topic (META_DEBUG_FOCUS,
                  "Current focus window %s has global keygrab, not focusing window %s after all\n",
                  window->display->grab_window->desc, window->desc);
      return;
    }

  modal_transient = get_modal_transient (window);
  if (modal_transient != NULL &&
      !modal_transient->unmanaging)
    {
      meta_topic (META_DEBUG_FOCUS,
                  "%s has %s as a modal transient, so focusing it instead.\n",
                  window->desc, modal_transient->desc);
      if (!meta_window_located_on_workspace (modal_transient, window->screen->active_workspace))
        meta_window_change_workspace (modal_transient, window->screen->active_workspace);
      window = modal_transient;
    }

  meta_window_flush_calc_showing (window);

  if ((!window->mapped || window->hidden) && !window->shaded)
    {
      meta_topic (META_DEBUG_FOCUS,
                  "Window %s is not showing, not focusing after all\n",
                  window->desc);
      return;
    }

  META_WINDOW_GET_CLASS (window)->focus (window, timestamp);

  if (window->wm_state_demands_attention)
    meta_window_unset_demands_attention(window);

/*  meta_effect_run_focus(window, NULL, NULL); */
}

/* Workspace management. Invariants:
 *
 *  - window->workspace describes the workspace the window is on.
 *
 *  - workspace->windows is a list of windows that is located on
 *    that workspace.
 *
 *  - If the window is on_all_workspaces, then
 *    window->workspace == NULL, but workspace->windows contains
 *    the window.
 */

static void
set_workspace_state (MetaWindow    *window,
                     gboolean       on_all_workspaces,
                     MetaWorkspace *workspace)
{
  /* If we're on all workspaces, then our new workspace must be NULL. */
  if (on_all_workspaces)
    g_assert (workspace == NULL);

  /* If this is an override-redirect window, ensure that the only
   * times we're setting the workspace state is either during construction
   * to mark as on_all_workspaces, or when unmanaging to remove all the
   * workspaces. */
  if (window->override_redirect)
    g_return_if_fail ((window->constructing && on_all_workspaces) || window->unmanaging);

  if (on_all_workspaces == window->on_all_workspaces &&
      workspace == window->workspace &&
      !window->constructing)
    return;

  if (window->workspace)
    meta_workspace_remove_window (window->workspace, window);
  else if (window->on_all_workspaces)
    {
      GList *l;
      for (l = window->screen->workspaces; l != NULL; l = l->next)
        {
          MetaWorkspace *ws = l->data;
          meta_workspace_remove_window (ws, window);
        }
    }

  window->on_all_workspaces = on_all_workspaces;
  window->workspace = workspace;

  if (window->workspace)
    meta_workspace_add_window (window->workspace, window);
  else if (window->on_all_workspaces)
    {
      GList *l;
      for (l = window->screen->workspaces; l != NULL; l = l->next)
        {
          MetaWorkspace *ws = l->data;
          meta_workspace_add_window (ws, window);
        }
    }

  /* queue a move_resize since changing workspaces may change
   * the relevant struts
   */
  if (!window->override_redirect)
    meta_window_queue (window, META_QUEUE_MOVE_RESIZE);
  meta_window_queue (window, META_QUEUE_CALC_SHOWING);
  meta_window_current_workspace_changed (window);
  g_object_notify_by_pspec (G_OBJECT (window), obj_props[PROP_ON_ALL_WORKSPACES]);
  g_signal_emit (window, window_signals[WORKSPACE_CHANGED], 0);
}

static gboolean
should_be_on_all_workspaces (MetaWindow *window)
{
  return
    window->always_sticky ||
    window->on_all_workspaces_requested ||
    window->override_redirect ||
    (meta_prefs_get_workspaces_only_on_primary () &&
     !window->unmanaging &&
     !meta_window_is_on_primary_monitor (window));
}

void
meta_window_on_all_workspaces_changed (MetaWindow *window)
{
  gboolean on_all_workspaces = should_be_on_all_workspaces (window);

  if (window->on_all_workspaces == on_all_workspaces)
    return;

  MetaWorkspace *workspace;

  if (on_all_workspaces)
    {
      workspace = NULL;
    }
  else
    {
      /* We're coming out of the sticky state. Put the window on
       * the currently active workspace. */
      workspace = window->screen->active_workspace;
    }

  set_workspace_state (window, on_all_workspaces, workspace);
}

static void
meta_window_change_workspace_without_transients (MetaWindow    *window,
                                                 MetaWorkspace *workspace)
{
  /* Try to unstick the window if it's stuck. This doesn't
   * have any guarantee that we'll actually unstick the
   * window, since it could be stuck for other reasons. */
  if (window->on_all_workspaces_requested)
    meta_window_unstick (window);

  /* We failed to unstick the window. */
  if (window->on_all_workspaces)
    return;

  if (window->workspace == workspace)
    return;

  set_workspace_state (window, FALSE, workspace);
}

static gboolean
change_workspace_foreach (MetaWindow *window,
                          void       *data)
{
  meta_window_change_workspace_without_transients (window, data);
  return TRUE;
}

void
meta_window_change_workspace (MetaWindow    *window,
                              MetaWorkspace *workspace)
{
  g_return_if_fail (!window->override_redirect);

  meta_window_change_workspace_without_transients (window, workspace);

  meta_window_foreach_transient (window, change_workspace_foreach,
                                 workspace);
  meta_window_foreach_ancestor (window, change_workspace_foreach,
                                workspace);
}

static void
window_stick_impl (MetaWindow  *window)
{
  meta_verbose ("Sticking window %s current on_all_workspaces = %d\n",
                window->desc, window->on_all_workspaces);

  if (window->on_all_workspaces_requested)
    return;

  /* We don't change window->workspaces, because we revert
   * to that original workspace list if on_all_workspaces is
   * toggled back off.
   */
  window->on_all_workspaces_requested = TRUE;
  meta_window_on_all_workspaces_changed (window);
}

static void
window_unstick_impl (MetaWindow  *window)
{
  if (!window->on_all_workspaces_requested)
    return;

  /* Revert to window->workspaces */

  window->on_all_workspaces_requested = FALSE;
  meta_window_on_all_workspaces_changed (window);
}

static gboolean
stick_foreach_func (MetaWindow *window,
                    void       *data)
{
  gboolean stick;

  stick = *(gboolean*)data;
  if (stick)
    window_stick_impl (window);
  else
    window_unstick_impl (window);
  return TRUE;
}

void
meta_window_stick (MetaWindow  *window)
{
  gboolean stick = TRUE;

  g_return_if_fail (!window->override_redirect);

  window_stick_impl (window);
  meta_window_foreach_transient (window,
                                 stick_foreach_func,
                                 &stick);
}

void
meta_window_unstick (MetaWindow  *window)
{
  gboolean stick = FALSE;

  g_return_if_fail (!window->override_redirect);

  window_unstick_impl (window);
  meta_window_foreach_transient (window,
                                 stick_foreach_func,
                                 &stick);
}

void
meta_window_current_workspace_changed (MetaWindow *window)
{
  META_WINDOW_GET_CLASS (window)->current_workspace_changed (window);
}

static gboolean
find_root_ancestor (MetaWindow *window,
                    void       *data)
{
  MetaWindow **ancestor = data;

  /* Overwrite the previously "most-root" ancestor with the new one found */
  *ancestor = window;

  /* We want this to continue until meta_window_foreach_ancestor quits because
   * there are no more valid ancestors.
   */
  return TRUE;
}

/**
 * meta_window_find_root_ancestor:
 * @window: a #MetaWindow
 *
 * Follow the chain of parents of @window, skipping transient windows,
 * and return the "root" window which has no non-transient parent.
 *
 * Returns: (transfer none): The root ancestor window
 */
MetaWindow *
meta_window_find_root_ancestor (MetaWindow *window)
{
  MetaWindow *ancestor;
  ancestor = window;
  meta_window_foreach_ancestor (window, find_root_ancestor, &ancestor);
  return ancestor;
}

void
meta_window_raise (MetaWindow  *window)
{
  MetaWindow *ancestor;

  g_return_if_fail (!window->override_redirect);

  ancestor = meta_window_find_root_ancestor (window);

  meta_topic (META_DEBUG_WINDOW_OPS,
              "Raising window %s, ancestor of %s\n",
              ancestor->desc, window->desc);

  /* Raise the ancestor of the window (if the window has no ancestor,
   * then ancestor will be set to the window itself); do this because
   * it's weird to see windows from other apps stacked between a child
   * and parent window of the currently active app.  The stacking
   * constraints in stack.c then magically take care of raising all
   * the child windows appropriately.
   */
  if (window->screen->stack == ancestor->screen->stack)
    meta_stack_raise (window->screen->stack, ancestor);
  else
    {
      meta_warning (
                    "Either stacks aren't per screen or some window has a weird "
                    "transient_for hint; window->screen->stack != "
                    "ancestor->screen->stack.  window = %s, ancestor = %s.\n",
                    window->desc, ancestor->desc);
      /* We could raise the window here, but don't want to do that twice and
       * so we let the case below handle that.
       */
    }

  /* Okay, so stacking constraints misses one case: If a window has
   * two children and we want to raise one of those children, then
   * raising the ancestor isn't enough; we need to also raise the
   * correct child.  See bug 307875.
   */
  if (window != ancestor)
    meta_stack_raise (window->screen->stack, window);

  g_signal_emit (window, window_signals[RAISED], 0);
}

void
meta_window_lower (MetaWindow  *window)
{
  g_return_if_fail (!window->override_redirect);

  meta_topic (META_DEBUG_WINDOW_OPS,
              "Lowering window %s\n", window->desc);

  meta_stack_lower (window->screen->stack, window);
}

/*
 * Move window to the requested workspace; append controls whether new WS
 * should be created if one does not exist.
 */
void
meta_window_change_workspace_by_index (MetaWindow *window,
                                       gint        space_index,
                                       gboolean    append)
{
  MetaWorkspace *workspace;
  MetaScreen    *screen;

  g_return_if_fail (!window->override_redirect);

  if (space_index == -1)
    {
      meta_window_stick (window);
      return;
    }

  screen = window->screen;

  workspace =
    meta_screen_get_workspace_by_index (screen, space_index);

  if (!workspace && append)
    workspace = meta_screen_append_new_workspace (screen, FALSE, CurrentTime);

  if (workspace)
    meta_window_change_workspace (window, workspace);
}

static void
meta_window_appears_focused_changed (MetaWindow *window)
{
  set_net_wm_state (window);
  meta_window_frame_size_changed (window);

  g_object_notify_by_pspec (G_OBJECT (window), obj_props[PROP_APPEARS_FOCUSED]);

  if (window->frame)
    meta_frame_queue_draw (window->frame);
}

static gboolean
should_propagate_focus_appearance (MetaWindow *window)
{
  /* Parents of attached modal dialogs should appear focused. */
  if (meta_window_is_attached_dialog (window))
    return TRUE;

  /* Parents of these sorts of override-redirect windows should
   * appear focused. */
  switch (window->type)
    {
    case META_WINDOW_DROPDOWN_MENU:
    case META_WINDOW_POPUP_MENU:
    case META_WINDOW_COMBO:
    case META_WINDOW_TOOLTIP:
    case META_WINDOW_NOTIFICATION:
    case META_WINDOW_DND:
    case META_WINDOW_OVERRIDE_OTHER:
      return TRUE;
    default:
      break;
    }

  return FALSE;
}

/**
 * meta_window_propagate_focus_appearance:
 * @window: the window to start propagating from
 * @focused: %TRUE if @window's ancestors should appear focused,
 *   %FALSE if they should not.
 *
 * Adjusts the value of #MetaWindow:appears-focused on @window's
 * ancestors (but not @window itself). If @focused is %TRUE, each of
 * @window's ancestors will have its %attached_focus_window field set
 * to the current %focus_window. If @focused if %FALSE, each of
 * @window's ancestors will have its %attached_focus_window field
 * cleared if it is currently %focus_window.
 */
static void
meta_window_propagate_focus_appearance (MetaWindow *window,
                                        gboolean    focused)
{
  MetaWindow *child, *parent, *focus_window;

  focus_window = window->display->focus_window;

  child = window;
  parent = meta_window_get_transient_for (child);
  while (parent && (!focused || should_propagate_focus_appearance (child)))
    {
      gboolean child_focus_state_changed;

      if (focused)
        {
          if (parent->attached_focus_window == focus_window)
            break;
          child_focus_state_changed = (parent->attached_focus_window == NULL);
          parent->attached_focus_window = focus_window;
        }
      else
        {
          if (parent->attached_focus_window != focus_window)
            break;
          child_focus_state_changed = (parent->attached_focus_window != NULL);
          parent->attached_focus_window = NULL;
        }

      if (child_focus_state_changed && !parent->has_focus)
        {
          meta_window_appears_focused_changed (parent);
        }

      child = parent;
      parent = meta_window_get_transient_for (child);
    }
}

void
meta_window_set_focused_internal (MetaWindow *window,
                                  gboolean    focused)
{
  if (focused)
    {
      window->has_focus = TRUE;
      if (window->override_redirect)
        return;

      /* Move to the front of the focusing workspace's MRU list.
       * We should only be "removing" it from the MRU list if it's
       * not already there.  Note that it's possible that we might
       * be processing this FocusIn after we've changed to a
       * different workspace; we should therefore update the MRU
       * list only if the window is actually on the active
       * workspace.
       */
      if (window->screen->active_workspace &&
          meta_window_located_on_workspace (window,
                                            window->screen->active_workspace))
        {
          GList* link;
          link = g_list_find (window->screen->active_workspace->mru_list,
                              window);
          g_assert (link);

          window->screen->active_workspace->mru_list =
            g_list_remove_link (window->screen->active_workspace->mru_list,
                                link);
          g_list_free (link);

          window->screen->active_workspace->mru_list =
            g_list_prepend (window->screen->active_workspace->mru_list,
                            window);
        }

      if (window->frame)
        meta_frame_queue_draw (window->frame);

      /* move into FOCUSED_WINDOW layer */
      meta_window_update_layer (window);

      /* Ungrab click to focus button since the sync grab can interfere
       * with some things you might do inside the focused window, by
       * causing the client to get funky enter/leave events.
       *
       * The reason we usually have a passive grab on the window is
       * so that we can intercept clicks and raise the window in
       * response. For click-to-focus we don't need that since the
       * focused window is already raised. When raise_on_click is
       * FALSE we also don't need that since we don't do anything
       * when the window is clicked.
       *
       * There is dicussion in bugs 102209, 115072, and 461577
       */
      if (meta_prefs_get_focus_mode () == G_DESKTOP_FOCUS_MODE_CLICK ||
          !meta_prefs_get_raise_on_click())
        meta_display_ungrab_focus_window_button (window->display, window);

      g_signal_emit (window, window_signals[FOCUS], 0);

      if (!window->attached_focus_window)
        meta_window_appears_focused_changed (window);

      meta_window_propagate_focus_appearance (window, TRUE);
    }
  else
    {
      window->has_focus = FALSE;
      if (window->override_redirect)
        return;

      meta_window_propagate_focus_appearance (window, FALSE);

      if (!window->attached_focus_window)
        meta_window_appears_focused_changed (window);

      /* move out of FOCUSED_WINDOW layer */
      meta_window_update_layer (window);

      /* Re-grab for click to focus and raise-on-click, if necessary */
      if (meta_prefs_get_focus_mode () == G_DESKTOP_FOCUS_MODE_CLICK ||
          !meta_prefs_get_raise_on_click ())
        meta_display_grab_focus_window_button (window->display, window);
    }
}

/**
 * meta_window_get_icon_geometry:
 * @window: a #MetaWindow
 * @rect: (out): rectangle into which to store the returned geometry.
 *
 * Gets the location of the icon corresponding to the window. The location
 * will be provided set by the task bar or other user interface element
 * displaying the icon, and is relative to the root window.
 *
 * Return value: %TRUE if the icon geometry was succesfully retrieved.
 */
gboolean
meta_window_get_icon_geometry (MetaWindow    *window,
                               MetaRectangle *rect)
{
  g_return_val_if_fail (!window->override_redirect, FALSE);

  if (window->icon_geometry_set)
    {
      if (rect)
        *rect = window->icon_geometry;

      return TRUE;
    }

  return FALSE;
}

/**
 * meta_window_set_icon_geometry:
 * @window: a #MetaWindow
 * @rect: (nullable): rectangle with the desired geometry or %NULL.
 *
 * Sets or unsets the location of the icon corresponding to the window. If
 * set, the location should correspond to a dock, task bar or other user
 * interface element displaying the icon, and is relative to the root window.
 */
void
meta_window_set_icon_geometry (MetaWindow    *window,
                               MetaRectangle *rect)
{
  if (rect)
    {
      window->icon_geometry = *rect;
      window->icon_geometry_set = TRUE;
    }
  else
    {
      window->icon_geometry_set = FALSE;
    }
}

static void
redraw_icon (MetaWindow *window)
{
  /* We could probably be smart and just redraw the icon here,
   * instead of the whole frame.
   */
  if (window->frame)
    meta_frame_queue_draw (window->frame);
}

static cairo_surface_t *
load_default_window_icon (int size)
{
  GtkIconTheme *theme = gtk_icon_theme_get_default ();
  GdkPixbuf *pixbuf;
  const char *icon_name;

  if (gtk_icon_theme_has_icon (theme, META_DEFAULT_ICON_NAME))
    icon_name = META_DEFAULT_ICON_NAME;
  else
    icon_name = "image-missing";

  pixbuf = gtk_icon_theme_load_icon (theme, icon_name, size, 0, NULL);
  return gdk_cairo_surface_create_from_pixbuf (pixbuf, 1, NULL);
}

static cairo_surface_t *
get_default_window_icon (void)
{
  static cairo_surface_t *default_icon = NULL;

  if (default_icon == NULL)
    {
      default_icon = load_default_window_icon (META_ICON_WIDTH);
      g_assert (default_icon);
    }

  return cairo_surface_reference (default_icon);
}

static cairo_surface_t *
get_default_mini_icon (void)
{
  static cairo_surface_t *default_icon = NULL;

  if (default_icon == NULL)
    {
      default_icon = load_default_window_icon (META_MINI_ICON_WIDTH);
      g_assert (default_icon);
    }

  return cairo_surface_reference (default_icon);
}

static void
meta_window_update_icon_now (MetaWindow *window,
                             gboolean    force)
{
  gboolean changed;
  cairo_surface_t *icon = NULL;
  cairo_surface_t *mini_icon;

  g_return_if_fail (!window->override_redirect);

  changed = META_WINDOW_GET_CLASS (window)->update_icon (window, &icon, &mini_icon);

  if (changed || force)
    {
      if (window->icon)
        cairo_surface_destroy (window->icon);
      if (icon)
        window->icon = icon;
      else
        window->icon = get_default_window_icon ();

      if (window->mini_icon)
        cairo_surface_destroy (window->mini_icon);
      if (mini_icon)
        window->mini_icon = mini_icon;
      else
        window->mini_icon = get_default_mini_icon ();

      g_object_freeze_notify (G_OBJECT (window));
      g_object_notify_by_pspec (G_OBJECT (window), obj_props[PROP_ICON]);
      g_object_notify_by_pspec (G_OBJECT (window), obj_props[PROP_MINI_ICON]);
      g_object_thaw_notify (G_OBJECT (window));

      redraw_icon (window);
    }

  g_assert (window->icon);
  g_assert (window->mini_icon);
}

static gboolean
idle_update_icon (gpointer data)
{
  GSList *tmp;
  GSList *copy;
  guint queue_index = GPOINTER_TO_INT (data);

  meta_topic (META_DEBUG_GEOMETRY, "Clearing the update_icon queue\n");

  /* Work with a copy, for reentrancy. The allowed reentrancy isn't
   * complete; destroying a window while we're in here would result in
   * badness. But it's OK to queue/unqueue update_icons.
   */
  copy = g_slist_copy (queue_pending[queue_index]);
  g_slist_free (queue_pending[queue_index]);
  queue_pending[queue_index] = NULL;
  queue_later[queue_index] = 0;

  destroying_windows_disallowed += 1;

  tmp = copy;
  while (tmp != NULL)
    {
      MetaWindow *window;

      window = tmp->data;

      meta_window_update_icon_now (window, FALSE);
      window->is_in_queues &= ~META_QUEUE_UPDATE_ICON;

      tmp = tmp->next;
    }

  g_slist_free (copy);

  destroying_windows_disallowed -= 1;

  return FALSE;
}

GList*
meta_window_get_workspaces (MetaWindow *window)
{
  if (window->on_all_workspaces)
    return window->screen->workspaces;
  else if (window->workspace != NULL)
    return window->workspace->list_containing_self;
  else if (window->constructing)
    return NULL;
  else
    g_assert_not_reached ();
}

static void
invalidate_work_areas (MetaWindow *window)
{
  GList *tmp;

  tmp = meta_window_get_workspaces (window);

  while (tmp != NULL)
    {
      meta_workspace_invalidate_work_area (tmp->data);
      tmp = tmp->next;
    }
}

void
meta_window_update_struts (MetaWindow *window)
{
  if (META_WINDOW_GET_CLASS (window)->update_struts (window))
    invalidate_work_areas (window);
}

static void
meta_window_type_changed (MetaWindow *window)
{
  gboolean old_decorated = window->decorated;
  GObject  *object = G_OBJECT (window);

  window->attached = meta_window_should_attach_to_parent (window);
  meta_window_recalc_features (window);

  if (!window->override_redirect)
    set_net_wm_state (window);

  /* Update frame */
  if (window->decorated)
    meta_window_ensure_frame (window);
  else
    meta_window_destroy_frame (window);

  /* update stacking constraints */
  meta_window_update_layer (window);

  meta_window_grab_keys (window);

  g_object_freeze_notify (object);

  if (old_decorated != window->decorated)
    g_object_notify_by_pspec (G_OBJECT (window), obj_props[PROP_DECORATED]);

  g_object_notify_by_pspec (G_OBJECT (window), obj_props[PROP_WINDOW_TYPE]);

  g_object_thaw_notify (object);
}

void
meta_window_set_type (MetaWindow     *window,
                      MetaWindowType  type)
{
  if (window->type == type)
    return;

  window->type = type;
  meta_window_type_changed (window);
}

void
meta_window_frame_size_changed (MetaWindow *window)
{
  if (window->frame)
    meta_frame_clear_cached_borders (window->frame);
}

static void
meta_window_get_default_skip_hints (MetaWindow *window,
                                    gboolean   *skip_taskbar_out,
                                    gboolean   *skip_pager_out)
{
  META_WINDOW_GET_CLASS (window)->get_default_skip_hints (window, skip_taskbar_out, skip_pager_out);
}

static void
meta_window_recalc_skip_features (MetaWindow *window)
{
  switch (window->type)
    {
      /* Force skip taskbar/pager on these window types */
    case META_WINDOW_DESKTOP:
    case META_WINDOW_DOCK:
    case META_WINDOW_TOOLBAR:
    case META_WINDOW_MENU:
    case META_WINDOW_UTILITY:
    case META_WINDOW_SPLASHSCREEN:
    case META_WINDOW_DROPDOWN_MENU:
    case META_WINDOW_POPUP_MENU:
    case META_WINDOW_TOOLTIP:
    case META_WINDOW_NOTIFICATION:
    case META_WINDOW_COMBO:
    case META_WINDOW_DND:
    case META_WINDOW_OVERRIDE_OTHER:
      window->skip_taskbar = TRUE;
      window->skip_pager = TRUE;
      break;

    case META_WINDOW_DIALOG:
    case META_WINDOW_MODAL_DIALOG:
      /* only skip taskbar if we have a real transient parent
         (and ignore the application hints) */
      if (window->transient_for != NULL)
        window->skip_taskbar = TRUE;
      else
        window->skip_taskbar = FALSE;
      break;

    case META_WINDOW_NORMAL:
      {
        gboolean skip_taskbar_hint, skip_pager_hint;
        meta_window_get_default_skip_hints (window, &skip_taskbar_hint, &skip_pager_hint);
        window->skip_taskbar = skip_taskbar_hint;
        window->skip_pager = skip_pager_hint;
      }
      break;
    }
}

void
meta_window_recalc_features (MetaWindow *window)
{
  gboolean old_has_close_func;
  gboolean old_has_minimize_func;
  gboolean old_has_move_func;
  gboolean old_has_resize_func;
  gboolean old_has_shade_func;
  gboolean old_always_sticky;
  gboolean old_skip_taskbar;

  old_has_close_func = window->has_close_func;
  old_has_minimize_func = window->has_minimize_func;
  old_has_move_func = window->has_move_func;
  old_has_resize_func = window->has_resize_func;
  old_has_shade_func = window->has_shade_func;
  old_always_sticky = window->always_sticky;
  old_skip_taskbar = window->skip_taskbar;

  /* Use MWM hints initially */
  if (window->client_type == META_WINDOW_CLIENT_TYPE_X11)
    window->decorated = window->mwm_decorated;
  else
    window->decorated = FALSE;
  window->border_only = window->mwm_border_only;
  window->has_close_func = window->mwm_has_close_func;
  window->has_minimize_func = window->mwm_has_minimize_func;
  window->has_maximize_func = window->mwm_has_maximize_func;
  window->has_move_func = window->mwm_has_move_func;

  window->has_resize_func = TRUE;

  /* If min_size == max_size, then don't allow resize */
  if (window->size_hints.min_width == window->size_hints.max_width &&
      window->size_hints.min_height == window->size_hints.max_height)
    window->has_resize_func = FALSE;
  else if (!window->mwm_has_resize_func)
    {
      /* We ignore mwm_has_resize_func because WM_NORMAL_HINTS is the
       * authoritative source for that info. Some apps such as mplayer or
       * xine disable resize via MWM but not WM_NORMAL_HINTS, but that
       * leads to e.g. us not fullscreening their windows.  Apps that set
       * MWM but not WM_NORMAL_HINTS are basically broken. We complain
       * about these apps but make them work.
       */

      meta_warning ("Window %s sets an MWM hint indicating it isn't resizable, but sets min size %d x %d and max size %d x %d; this doesn't make much sense.\n",
                    window->desc,
                    window->size_hints.min_width,
                    window->size_hints.min_height,
                    window->size_hints.max_width,
                    window->size_hints.max_height);
    }

  window->has_shade_func = TRUE;
  window->has_fullscreen_func = TRUE;

  window->always_sticky = FALSE;

  /* Semantic category overrides the MWM hints */
  if (window->type == META_WINDOW_TOOLBAR)
    window->decorated = FALSE;

  if (window->type == META_WINDOW_DESKTOP ||
      window->type == META_WINDOW_DOCK ||
      window->override_redirect)
    window->always_sticky = TRUE;

  if (window->override_redirect ||
      meta_window_get_frame_type (window) == META_FRAME_TYPE_LAST)
    {
      window->decorated = FALSE;
      window->has_close_func = FALSE;
      window->has_shade_func = FALSE;

      /* FIXME this keeps panels and things from using
       * NET_WM_MOVERESIZE; the problem is that some
       * panels (edge panels) have fixed possible locations,
       * and others ("floating panels") do not.
       *
       * Perhaps we should require edge panels to explicitly
       * disable movement?
       */
      window->has_move_func = FALSE;
      window->has_resize_func = FALSE;
    }

  if (window->type != META_WINDOW_NORMAL)
    {
      window->has_minimize_func = FALSE;
      window->has_maximize_func = FALSE;
      window->has_fullscreen_func = FALSE;
    }

  if (!window->has_resize_func)
    {
      window->has_maximize_func = FALSE;

      /* don't allow fullscreen if we can't resize, unless the size
       * is entire screen size (kind of broken, because we
       * actually fullscreen to monitor size not screen size)
       */
      if (window->size_hints.min_width == window->screen->rect.width &&
          window->size_hints.min_height == window->screen->rect.height)
        ; /* leave fullscreen available */
      else
        window->has_fullscreen_func = FALSE;
    }

  /* We leave fullscreen windows decorated, just push the frame outside
   * the screen. This avoids flickering to unparent them.
   *
   * Note that setting has_resize_func = FALSE here must come after the
   * above code that may disable fullscreen, because if the window
   * is not resizable purely due to fullscreen, we don't want to
   * disable fullscreen mode.
   */
  if (window->fullscreen)
    {
      window->has_shade_func = FALSE;
      window->has_move_func = FALSE;
      window->has_resize_func = FALSE;
      window->has_maximize_func = FALSE;
    }

  if (window->has_maximize_func)
    {
      MetaRectangle work_area, client_rect;

      meta_window_get_work_area_current_monitor (window, &work_area);
      meta_window_frame_rect_to_client_rect (window, &work_area, &client_rect);

      if (window->size_hints.min_width >= client_rect.width ||
          window->size_hints.min_height >= client_rect.height)
        window->has_maximize_func = FALSE;
    }

  meta_topic (META_DEBUG_WINDOW_OPS,
              "Window %s fullscreen = %d not resizable, maximizable = %d fullscreenable = %d min size %dx%d max size %dx%d\n",
              window->desc,
              window->fullscreen,
              window->has_maximize_func, window->has_fullscreen_func,
              window->size_hints.min_width,
              window->size_hints.min_height,
              window->size_hints.max_width,
              window->size_hints.max_height);

  /* no shading if not decorated */
  if (!window->decorated || window->border_only)
    window->has_shade_func = FALSE;

  meta_window_recalc_skip_features (window);

  meta_topic (META_DEBUG_WINDOW_OPS,
              "Window %s decorated = %d border_only = %d has_close = %d has_minimize = %d has_maximize = %d has_move = %d has_shade = %d skip_taskbar = %d skip_pager = %d\n",
              window->desc,
              window->decorated,
              window->border_only,
              window->has_close_func,
              window->has_minimize_func,
              window->has_maximize_func,
              window->has_move_func,
              window->has_shade_func,
              window->skip_taskbar,
              window->skip_pager);

  if (old_skip_taskbar != window->skip_taskbar)
    g_object_notify_by_pspec (G_OBJECT (window), obj_props[PROP_SKIP_TASKBAR]);

  /* FIXME:
   * Lame workaround for recalc_features being used overzealously.
   * The fix is to only recalc_features when something has
   * actually changed.
   */
  if (window->constructing                               ||
      old_has_close_func != window->has_close_func       ||
      old_has_minimize_func != window->has_minimize_func ||
      old_has_move_func != window->has_move_func         ||
      old_has_resize_func != window->has_resize_func     ||
      old_has_shade_func != window->has_shade_func       ||
      old_always_sticky != window->always_sticky)
    set_allowed_actions_hint (window);

  if (window->has_resize_func != old_has_resize_func)
    g_object_notify_by_pspec (G_OBJECT (window), obj_props[PROP_RESIZEABLE]);

  meta_window_frame_size_changed (window);

  /* FIXME perhaps should ensure if we don't have a shade func,
   * we aren't shaded, etc.
   */
}

void
meta_window_show_menu (MetaWindow         *window,
                       MetaWindowMenuType  menu,
                       int                 x,
                       int                 y)
{
  g_return_if_fail (!window->override_redirect);
  meta_compositor_show_window_menu (window->display->compositor, window, menu, x, y);
}

void
meta_window_show_menu_for_rect (MetaWindow         *window,
                                MetaWindowMenuType  menu,
                                MetaRectangle      *rect)
{
  g_return_if_fail (!window->override_redirect);
  meta_compositor_show_window_menu_for_rect (window->display->compositor, window, menu, rect);
}

void
meta_window_shove_titlebar_onscreen (MetaWindow *window)
{
  MetaRectangle  frame_rect;
  GList         *onscreen_region;
  int            horiz_amount, vert_amount;

  g_return_if_fail (!window->override_redirect);

  /* If there's no titlebar, don't bother */
  if (!window->frame)
    return;

  /* Get the basic info we need */
  meta_window_get_frame_rect (window, &frame_rect);
  onscreen_region = window->screen->active_workspace->screen_region;

  /* Extend the region (just in case the window is too big to fit on the
   * screen), then shove the window on screen, then return the region to
   * normal.
   */
  horiz_amount = frame_rect.width;
  vert_amount  = frame_rect.height;
  meta_rectangle_expand_region (onscreen_region,
                                horiz_amount,
                                horiz_amount,
                                0,
                                vert_amount);
  meta_rectangle_shove_into_region(onscreen_region,
                                   FIXED_DIRECTION_X,
                                   &frame_rect);
  meta_rectangle_expand_region (onscreen_region,
                                -horiz_amount,
                                -horiz_amount,
                                0,
                                -vert_amount);

  meta_window_move_frame (window, FALSE, frame_rect.x, frame_rect.y);
}

gboolean
meta_window_titlebar_is_onscreen (MetaWindow *window)
{
  MetaRectangle  titlebar_rect, frame_rect;
  GList         *onscreen_region;
  gboolean       is_onscreen;

  const int min_height_needed  = 8;
  const float min_width_percent  = 0.5;
  const int min_width_absolute = 50;

  /* Titlebar can't be offscreen if there is no titlebar... */
  if (!window->frame)
    return TRUE;

  /* Get the rectangle corresponding to the titlebar */
  meta_window_get_titlebar_rect (window, &titlebar_rect);

  /* Translate into screen coordinates */
  meta_window_get_frame_rect (window, &frame_rect);
  titlebar_rect.x = frame_rect.x;
  titlebar_rect.y = frame_rect.y;

  /* Run through the spanning rectangles for the screen and see if one of
   * them overlaps with the titlebar sufficiently to consider it onscreen.
   */
  is_onscreen = FALSE;
  onscreen_region = window->screen->active_workspace->screen_region;
  while (onscreen_region)
    {
      MetaRectangle *spanning_rect = onscreen_region->data;
      MetaRectangle overlap;

      meta_rectangle_intersect (&titlebar_rect, spanning_rect, &overlap);
      if (overlap.height > MIN (titlebar_rect.height, min_height_needed) &&
          overlap.width  > MIN (titlebar_rect.width * min_width_percent,
                                min_width_absolute))
        {
          is_onscreen = TRUE;
          break;
        }

      onscreen_region = onscreen_region->next;
    }

  return is_onscreen;
}

static double
timeval_to_ms (const GTimeVal *timeval)
{
  return (timeval->tv_sec * G_USEC_PER_SEC + timeval->tv_usec) / 1000.0;
}

static double
time_diff (const GTimeVal *first,
	   const GTimeVal *second)
{
  double first_ms = timeval_to_ms (first);
  double second_ms = timeval_to_ms (second);

  return first_ms - second_ms;
}

static gboolean
check_moveresize_frequency (MetaWindow *window,
			    gdouble    *remaining)
{
  GTimeVal current_time;
  const double max_resizes_per_second = 25.0;
  const double ms_between_resizes = 1000.0 / max_resizes_per_second;
  double elapsed;

  g_get_current_time (&current_time);

  /* If we are throttling via _NET_WM_SYNC_REQUEST, we don't need
   * an artificial timeout-based throttled */
  if (!window->disable_sync &&
      window->sync_request_alarm != None)
    return TRUE;

  elapsed = time_diff (&current_time, &window->display->grab_last_moveresize_time);

  if (elapsed >= 0.0 && elapsed < ms_between_resizes)
    {
      meta_topic (META_DEBUG_RESIZING,
                  "Delaying move/resize as only %g of %g ms elapsed\n",
                  elapsed, ms_between_resizes);

      if (remaining)
        *remaining = (ms_between_resizes - elapsed);

      return FALSE;
    }

  meta_topic (META_DEBUG_RESIZING,
              " Checked moveresize freq, allowing move/resize now (%g of %g seconds elapsed)\n",
              elapsed / 1000.0, 1.0 / max_resizes_per_second);

  return TRUE;
}

static gboolean
update_move_timeout (gpointer data)
{
  MetaWindow *window = data;

  update_move (window,
               window->display->grab_last_user_action_was_snap,
               window->display->grab_latest_motion_x,
               window->display->grab_latest_motion_y);

  return FALSE;
}

static void
update_move (MetaWindow  *window,
             gboolean     snap,
             int          x,
             int          y)
{
  int dx, dy;
  int new_x, new_y;
  MetaRectangle old;
  int shake_threshold;
  MetaDisplay *display = window->display;

  display->grab_latest_motion_x = x;
  display->grab_latest_motion_y = y;

  dx = x - display->grab_anchor_root_x;
  dy = y - display->grab_anchor_root_y;

  new_x = display->grab_anchor_window_pos.x + dx;
  new_y = display->grab_anchor_window_pos.y + dy;

  meta_verbose ("x,y = %d,%d anchor ptr %d,%d anchor pos %d,%d dx,dy %d,%d\n",
                x, y,
                display->grab_anchor_root_x,
                display->grab_anchor_root_y,
                display->grab_anchor_window_pos.x,
                display->grab_anchor_window_pos.y,
                dx, dy);

  /* Don't bother doing anything if no move has been specified.  (This
   * happens often, even in keyboard moving, due to the warping of the
   * pointer.
   */
  if (dx == 0 && dy == 0)
    return;

  /* Originally for detaching maximized windows, but we use this
   * for the zones at the sides of the monitor where trigger tiling
   * because it's about the right size
   */
#define DRAG_THRESHOLD_TO_SHAKE_THRESHOLD_FACTOR 6
  shake_threshold = meta_prefs_get_drag_threshold () *
    DRAG_THRESHOLD_TO_SHAKE_THRESHOLD_FACTOR;

  if (snap)
    {
      /* We don't want to tile while snapping. Also, clear any previous tile
         request. */
      window->tile_mode = META_TILE_NONE;
      window->tile_monitor_number = -1;
    }
  else if (meta_prefs_get_edge_tiling () &&
           !META_WINDOW_MAXIMIZED (window) &&
           !META_WINDOW_TILED_SIDE_BY_SIDE (window))
    {
      const MetaMonitorInfo *monitor;
      MetaRectangle work_area;

      /* For side-by-side tiling we are interested in the inside vertical
       * edges of the work area of the monitor where the pointer is located,
       * and in the outside top edge for maximized tiling.
       *
       * For maximized tiling we use the outside edge instead of the
       * inside edge, because we don't want to force users to maximize
       * windows they are placing near the top of their screens.
       *
       * The "current" idea of meta_window_get_work_area_current_monitor() and
       * meta_screen_get_current_monitor() is slightly different: the former
       * refers to the monitor which contains the largest part of the window,
       * the latter to the one where the pointer is located.
       */
      monitor = meta_screen_get_current_monitor_info_for_pos (window->screen, x, y);
      meta_window_get_work_area_for_monitor (window,
                                             monitor->number,
                                             &work_area);

      /* Check if the cursor is in a position which triggers tiling
       * and set tile_mode accordingly.
       */
      if (meta_window_can_tile_side_by_side (window) &&
          x >= monitor->rect.x && x < (work_area.x + shake_threshold))
        window->tile_mode = META_TILE_LEFT;
      else if (meta_window_can_tile_side_by_side (window) &&
               x >= work_area.x + work_area.width - shake_threshold &&
               x < (monitor->rect.x + monitor->rect.width))
        window->tile_mode = META_TILE_RIGHT;
      else if (meta_window_can_tile_maximized (window) &&
               y >= monitor->rect.y && y <= work_area.y)
        window->tile_mode = META_TILE_MAXIMIZED;
      else
        window->tile_mode = META_TILE_NONE;

      if (window->tile_mode != META_TILE_NONE)
        window->tile_monitor_number = monitor->number;
    }

  /* shake loose (unmaximize) maximized or tiled window if dragged beyond
   * the threshold in the Y direction. Tiled windows can also be pulled
   * loose via X motion.
   */

  if ((META_WINDOW_MAXIMIZED (window) && ABS (dy) >= shake_threshold) ||
      (META_WINDOW_TILED_SIDE_BY_SIDE (window) && (MAX (ABS (dx), ABS (dy)) >= shake_threshold)))
    {
      double prop;

      /* Shake loose, so that the window snaps back to maximized
       * when dragged near the top; do not snap back if tiling
       * is enabled, as top edge tiling can be used in that case
       */
      window->shaken_loose = !meta_prefs_get_edge_tiling ();
      window->tile_mode = META_TILE_NONE;

      /* move the unmaximized window to the cursor */
      prop =
        ((double)(x - display->grab_initial_window_pos.x)) /
        ((double)display->grab_initial_window_pos.width);

      display->grab_initial_window_pos.x = x - window->saved_rect.width * prop;

      /* If we started dragging the window from above the top of the window,
       * pretend like we started dragging from the middle of the titlebar
       * instead, as the "correct" anchoring looks wrong. */
      if (display->grab_anchor_root_y < display->grab_initial_window_pos.y)
        {
          MetaRectangle titlebar_rect;
          meta_window_get_titlebar_rect (window, &titlebar_rect);
          display->grab_anchor_root_y = display->grab_initial_window_pos.y + titlebar_rect.height / 2;
        }

      window->saved_rect.x = display->grab_initial_window_pos.x;
      window->saved_rect.y = display->grab_initial_window_pos.y;

      meta_window_unmaximize (window, META_MAXIMIZE_BOTH);
      return;
    }

  /* remaximize window on another monitor if window has been shaken
   * loose or it is still maximized (then move straight)
   */
  else if ((window->shaken_loose || META_WINDOW_MAXIMIZED (window)) &&
           window->tile_mode != META_TILE_LEFT && window->tile_mode != META_TILE_RIGHT)
    {
      const MetaMonitorInfo *wmonitor;
      MetaRectangle work_area;
      int monitor;

      window->tile_mode = META_TILE_NONE;
      wmonitor = window->monitor;

      for (monitor = 0; monitor < window->screen->n_monitor_infos; monitor++)
        {
          meta_window_get_work_area_for_monitor (window, monitor, &work_area);

          /* check if cursor is near the top of a monitor work area */
          if (x >= work_area.x &&
              x < (work_area.x + work_area.width) &&
              y >= work_area.y &&
              y < (work_area.y + shake_threshold))
            {
              /* move the saved rect if window will become maximized on an
               * other monitor so user isn't surprised on a later unmaximize
               */
              if (wmonitor->number != monitor)
                {
                  window->saved_rect.x = work_area.x;
                  window->saved_rect.y = work_area.y;

                  if (window->frame)
                    {
                      window->saved_rect.x += window->frame->child_x;
                      window->saved_rect.y += window->frame->child_y;
                    }

                  window->unconstrained_rect.x = window->saved_rect.x;
                  window->unconstrained_rect.y = window->saved_rect.y;

                  meta_window_unmaximize (window, META_MAXIMIZE_BOTH);

                  display->grab_initial_window_pos = work_area;
                  display->grab_anchor_root_x = x;
                  display->grab_anchor_root_y = y;
                  window->shaken_loose = FALSE;

                  meta_window_maximize (window, META_MAXIMIZE_BOTH);
                }

              return;
            }
        }
    }

  /* Delay showing the tile preview slightly to make it more unlikely to
   * trigger it unwittingly, e.g. when shaking loose the window or moving
   * it to another monitor.
   */
  meta_screen_update_tile_preview (window->screen,
                                   window->tile_mode != META_TILE_NONE);

  meta_window_get_frame_rect (window, &old);

  /* Don't allow movement in the maximized directions or while tiled */
  if (window->maximized_horizontally || META_WINDOW_TILED_SIDE_BY_SIDE (window))
    new_x = old.x;
  if (window->maximized_vertically)
    new_y = old.y;

  /* Do any edge resistance/snapping */
  meta_window_edge_resistance_for_move (window,
                                        &new_x,
                                        &new_y,
                                        update_move_timeout,
                                        snap,
                                        FALSE);

  meta_window_move_frame (window, TRUE, new_x, new_y);
}

static gboolean
update_resize_timeout (gpointer data)
{
  MetaWindow *window = data;

  update_resize (window,
                 window->display->grab_last_user_action_was_snap,
                 window->display->grab_latest_motion_x,
                 window->display->grab_latest_motion_y,
                 TRUE);
  return FALSE;
}

static void
update_resize (MetaWindow *window,
               gboolean    snap,
               int x, int y,
               gboolean force)
{
  int dx, dy;
  int new_w, new_h;
  int gravity;
  MetaRectangle old;
  double remaining = 0;

  window->display->grab_latest_motion_x = x;
  window->display->grab_latest_motion_y = y;

  dx = x - window->display->grab_anchor_root_x;
  dy = y - window->display->grab_anchor_root_y;

  /* Attached modal dialogs are special in that size
   * changes apply to both sides, so that the dialog
   * remains centered to the parent.
   */
  if (meta_window_is_attached_dialog (window))
    {
      dx *= 2;
      dy *= 2;
    }

  new_w = window->display->grab_anchor_window_pos.width;
  new_h = window->display->grab_anchor_window_pos.height;

  /* Don't bother doing anything if no move has been specified.  (This
   * happens often, even in keyboard resizing, due to the warping of the
   * pointer.
   */
  if (dx == 0 && dy == 0)
    return;

  if (window->display->grab_op == META_GRAB_OP_KEYBOARD_RESIZING_UNKNOWN)
    {
      MetaGrabOp op = META_GRAB_OP_WINDOW_BASE | META_GRAB_OP_WINDOW_FLAG_KEYBOARD;

      if (dx > 0)
        op |= META_GRAB_OP_WINDOW_DIR_EAST;
      else if (dx < 0)
        op |= META_GRAB_OP_WINDOW_DIR_WEST;

      if (dy > 0)
        op |= META_GRAB_OP_WINDOW_DIR_SOUTH;
      else if (dy < 0)
        op |= META_GRAB_OP_WINDOW_DIR_NORTH;

      window->display->grab_op = op;

      meta_window_update_keyboard_resize (window, TRUE);
    }

  if (window->display->grab_op & META_GRAB_OP_WINDOW_DIR_EAST)
    new_w += dx;
  else if (window->display->grab_op & META_GRAB_OP_WINDOW_DIR_WEST)
    new_w -= dx;

  if (window->display->grab_op & META_GRAB_OP_WINDOW_DIR_SOUTH)
    new_h += dy;
  else if (window->display->grab_op & META_GRAB_OP_WINDOW_DIR_NORTH)
    new_h -= dy;

  /* If we're waiting for a request for _NET_WM_SYNC_REQUEST, we'll
   * resize the window when the window responds, or when we time
   * the response out.
   */
  if (window->sync_request_timeout_id != 0)
    return;

  if (!check_moveresize_frequency (window, &remaining) && !force)
    {
      /* we are ignoring an event here, so we schedule a
       * compensation event when we would otherwise not ignore
       * an event. Otherwise we can become stuck if the user never
       * generates another event.
       */
      if (!window->display->grab_resize_timeout_id)
	{
	  window->display->grab_resize_timeout_id =
	    g_timeout_add ((int)remaining, update_resize_timeout, window);
	  g_source_set_name_by_id (window->display->grab_resize_timeout_id,
                                   "[mutter] update_resize_timeout");
	}

      return;
    }

  /* Remove any scheduled compensation events */
  if (window->display->grab_resize_timeout_id)
    {
      g_source_remove (window->display->grab_resize_timeout_id);
      window->display->grab_resize_timeout_id = 0;
    }

  meta_window_get_frame_rect (window, &old);

  /* One sided resizing ought to actually be one-sided, despite the fact that
   * aspect ratio windows don't interact nicely with the above stuff.  So,
   * to avoid some nasty flicker, we enforce that.
   */

  if ((window->display->grab_op & (META_GRAB_OP_WINDOW_DIR_WEST | META_GRAB_OP_WINDOW_DIR_EAST)) == 0)
    new_w = old.width;

  if ((window->display->grab_op & (META_GRAB_OP_WINDOW_DIR_NORTH | META_GRAB_OP_WINDOW_DIR_SOUTH)) == 0)
    new_h = old.height;

  /* compute gravity of client during operation */
  gravity = meta_resize_gravity_from_grab_op (window->display->grab_op);
  g_assert (gravity >= 0);

  /* Do any edge resistance/snapping */
  meta_window_edge_resistance_for_resize (window,
                                          &new_w,
                                          &new_h,
                                          gravity,
                                          update_resize_timeout,
                                          snap,
                                          FALSE);

  meta_window_resize_frame_with_gravity (window, TRUE, new_w, new_h, gravity);

  /* Store the latest resize time, if we actually resized. */
  if (window->rect.width != old.width || window->rect.height != old.height)
    g_get_current_time (&window->display->grab_last_moveresize_time);
}

static void
update_tile_mode (MetaWindow *window)
{
  switch (window->tile_mode)
    {
      case META_TILE_LEFT:
      case META_TILE_RIGHT:
          if (!META_WINDOW_TILED_SIDE_BY_SIDE (window))
              window->tile_mode = META_TILE_NONE;
          break;
      case META_TILE_MAXIMIZED:
          if (!META_WINDOW_MAXIMIZED (window))
              window->tile_mode = META_TILE_NONE;
          break;
    }
}

void
meta_window_update_resize (MetaWindow *window,
                           gboolean    snap,
                           int x, int y,
                           gboolean force)
{
  update_resize (window, snap, x, y, force);
}

static void
end_grab_op (MetaWindow *window,
             const ClutterEvent *event)
{
  ClutterModifierType modifiers;
  gfloat x, y;

  clutter_event_get_coords (event, &x, &y);
  modifiers = clutter_event_get_state (event);
  meta_display_check_threshold_reached (window->display, x, y);

  /* If the user was snap moving then ignore the button
   * release because they may have let go of shift before
   * releasing the mouse button and they almost certainly do
   * not want a non-snapped movement to occur from the button
   * release.
   */
  if (!window->display->grab_last_user_action_was_snap)
    {
      if (meta_grab_op_is_moving (window->display->grab_op))
        {
          if (window->tile_mode != META_TILE_NONE)
            meta_window_tile (window);
          else
            update_move (window,
                         modifiers & CLUTTER_SHIFT_MASK,
                         x, y);
        }
      else if (meta_grab_op_is_resizing (window->display->grab_op))
        {
          update_resize (window,
                         modifiers & CLUTTER_SHIFT_MASK,
                         x, y,
                         TRUE);

          /* If a tiled window has been dragged free with a
           * mouse resize without snapping back to the tiled
           * state, it will end up with an inconsistent tile
           * mode on mouse release; cleaning the mode earlier
           * would break the ability to snap back to the tiled
           * state, so we wait until mouse release.
           */
          update_tile_mode (window);
        }
    }
  meta_display_end_grab_op (window->display, clutter_event_get_time (event));
}

gboolean
meta_window_handle_mouse_grab_op_event  (MetaWindow         *window,
                                         const ClutterEvent *event)
{
  ClutterEventSequence *sequence = clutter_event_get_event_sequence (event);
  ClutterModifierType modifier_state;
  gfloat x, y;

  switch (event->type)
    {
    case CLUTTER_BUTTON_PRESS:
      {
        ClutterModifierType grab_mods = meta_display_get_window_grab_modifiers (window->display);

        /* This is the keybinding or menu case where we've
         * been dragging around the window without the button
         * pressed. */

        if ((meta_grab_op_is_mouse (window->display->grab_op) &&
             (event->button.modifier_state & grab_mods) == grab_mods &&
             window->display->grab_button != (int) event->button.button) ||
            meta_grab_op_is_keyboard (window->display->grab_op))
          {
            end_grab_op (window, event);
            return FALSE;
          }
        return TRUE;
      }

    case CLUTTER_TOUCH_END:
      if (meta_display_is_pointer_emulating_sequence (window->display, sequence))
        end_grab_op (window, event);

      return TRUE;

    case CLUTTER_BUTTON_RELEASE:
      if (event->button.button == 1 ||
          event->button.button == (unsigned int) meta_prefs_get_mouse_button_resize ())
        end_grab_op (window, event);

      return TRUE;

    case CLUTTER_TOUCH_BEGIN:
      /* This will only catch the keybinding and menu cases, just deal with this
       * like a CLUTTER_TOUCH_UPDATE rather than a CLUTTER_BUTTON_PRESS, and
       * wait until CLUTTER_TOUCH_END to undo the grab, just so the window
       * doesn't warp below the finger and remain there.
       */
    case CLUTTER_TOUCH_UPDATE:
      if (!meta_display_is_pointer_emulating_sequence (window->display, sequence))
        return FALSE;

      /* Fall through */
    case CLUTTER_MOTION:
      modifier_state = clutter_event_get_state (event);
      clutter_event_get_coords (event, &x, &y);

      meta_display_check_threshold_reached (window->display, x, y);
      if (meta_grab_op_is_moving (window->display->grab_op))
        {
          update_move (window,
                       modifier_state & CLUTTER_SHIFT_MASK,
                       x, y);
        }
      else if (meta_grab_op_is_resizing (window->display->grab_op))
        {
          update_resize (window,
                         modifier_state & CLUTTER_SHIFT_MASK,
                         x, y,
                         FALSE);
        }
      return TRUE;

    default:
      return FALSE;
    }
}

static void
get_work_area_monitor (MetaWindow    *window,
                       MetaRectangle *area,
                       int            which_monitor)
{
  GList *tmp;

  g_assert (which_monitor >= 0);

  /* Initialize to the whole monitor */
  *area = window->screen->monitor_infos[which_monitor].rect;

  tmp = meta_window_get_workspaces (window);
  while (tmp != NULL)
    {
      MetaRectangle workspace_work_area;
      meta_workspace_get_work_area_for_monitor (tmp->data,
                                                which_monitor,
                                                &workspace_work_area);
      meta_rectangle_intersect (area,
                                &workspace_work_area,
                                area);
      tmp = tmp->next;
    }

  meta_topic (META_DEBUG_WORKAREA,
              "Window %s monitor %d has work area %d,%d %d x %d\n",
              window->desc, which_monitor,
              area->x, area->y, area->width, area->height);
}

/**
 * meta_window_get_work_area_current_monitor:
 * @window: a #MetaWindow
 * @area: (out): a location to store the work area
 *
 * Get the work area for the monitor @window is currently on.
 */
void
meta_window_get_work_area_current_monitor (MetaWindow    *window,
                                           MetaRectangle *area)
{
  meta_window_get_work_area_for_monitor (window,
                                         window->monitor->number,
                                         area);
}

/**
 * meta_window_get_work_area_for_monitor:
 * @window: a #MetaWindow
 * @which_monitor: a moniotr to get the work area for
 * @area: (out): a location to store the work area
 *
 * Get the work area for @window, given the monitor index
 * @which_monitor.
 */
void
meta_window_get_work_area_for_monitor (MetaWindow    *window,
                                       int            which_monitor,
                                       MetaRectangle *area)
{
  g_return_if_fail (which_monitor >= 0);

  get_work_area_monitor (window,
                         area,
                         which_monitor);
}

/**
 * meta_window_get_work_area_all_monitors:
 * @window: a #MetaWindow
 * @area: (out): a location to store the work area
 *
 * Get the work area for all monitors for @window.
 */
void
meta_window_get_work_area_all_monitors (MetaWindow    *window,
                                        MetaRectangle *area)
{
  GList *tmp;

  /* Initialize to the whole screen */
  *area = window->screen->rect;

  tmp = meta_window_get_workspaces (window);
  while (tmp != NULL)
    {
      MetaRectangle workspace_work_area;
      meta_workspace_get_work_area_all_monitors (tmp->data,
                                                 &workspace_work_area);
      meta_rectangle_intersect (area,
                                &workspace_work_area,
                                area);
      tmp = tmp->next;
    }

  meta_topic (META_DEBUG_WORKAREA,
              "Window %s has whole-screen work area %d,%d %d x %d\n",
              window->desc, area->x, area->y, area->width, area->height);
}

int
meta_window_get_current_tile_monitor_number (MetaWindow *window)
{
  int tile_monitor_number = window->tile_monitor_number;

  if (tile_monitor_number < 0)
    {
      meta_warning ("%s called with an invalid monitor number; using 0 instead\n", G_STRFUNC);
      tile_monitor_number = 0;
    }

  return tile_monitor_number;
}

void
meta_window_get_current_tile_area (MetaWindow    *window,
                                   MetaRectangle *tile_area)
{
  int tile_monitor_number;

  g_return_if_fail (window->tile_mode != META_TILE_NONE);

  tile_monitor_number = meta_window_get_current_tile_monitor_number (window);

  meta_window_get_work_area_for_monitor (window, tile_monitor_number, tile_area);

  if (window->tile_mode == META_TILE_LEFT  ||
      window->tile_mode == META_TILE_RIGHT)
    tile_area->width /= 2;

  if (window->tile_mode == META_TILE_RIGHT)
    tile_area->x += tile_area->width;
}

gboolean
meta_window_same_application (MetaWindow *window,
                              MetaWindow *other_window)
{
  MetaGroup *group       = meta_window_get_group (window);
  MetaGroup *other_group = meta_window_get_group (other_window);

  return
    group!=NULL &&
    other_group!=NULL &&
    group==other_group;
}

/**
 * meta_window_is_client_decorated:
 *
 * Check if if the window has decorations drawn by the client.
 * (window->decorated refers only to whether we should add decorations)
 */
gboolean
meta_window_is_client_decorated (MetaWindow *window)
{
  if (window->client_type == META_WINDOW_CLIENT_TYPE_WAYLAND)
    {
      /* Assume all Wayland clients draw decorations - not strictly
       * true but good enough for current purposes.
       */
      return TRUE;
    }
  else
    {
      /* Currently the implementation here is hackish -
       * has_custom_frame_extents() is set if _GTK_FRAME_EXTENTS is set
       * to any value even 0. GTK+ always sets _GTK_FRAME_EXTENTS for
       * client-side-decorated window, even if the value is 0 because
       * the window is maxized and has no invisible borders or shadows.
       */
      return window->has_custom_frame_extents;
    }
}

/**
 * meta_window_foreach_transient:
 * @window: a #MetaWindow
 * @func: (scope call) (closure user_data): Called for each window which is a transient of @window (transitively)
 * @user_data: User data
 *
 * Call @func for every window which is either transient for @window, or is
 * a transient of a window which is in turn transient for @window.
 * The order of window enumeration is not defined.
 *
 * Iteration will stop if @func at any point returns %FALSE.
 */
void
meta_window_foreach_transient (MetaWindow            *window,
                               MetaWindowForeachFunc  func,
                               void                  *user_data)
{
  GSList *windows;
  GSList *tmp;

  windows = meta_display_list_windows (window->display, META_LIST_DEFAULT);

  tmp = windows;
  while (tmp != NULL)
    {
      MetaWindow *transient = tmp->data;

      if (meta_window_is_ancestor_of_transient (window, transient))
        {
          if (!(* func) (transient, user_data))
            break;
        }

      tmp = tmp->next;
    }

  g_slist_free (windows);
}

/**
 * meta_window_foreach_ancestor:
 * @window: a #MetaWindow
 * @func: (scope call) (closure user_data): Called for each window which is a transient parent of @window
 * @user_data: User data
 *
 * If @window is transient, call @func with the window for which it's transient,
 * repeatedly until either we find a non-transient window, or @func returns %FALSE.
 */
void
meta_window_foreach_ancestor (MetaWindow            *window,
                              MetaWindowForeachFunc  func,
                              void                  *user_data)
{
  MetaWindow *w;

  w = window;
  do
    {
      if (w->transient_for == NULL)
        break;

      w = w->transient_for;
    }
  while (w && (* func) (w, user_data));
}

typedef struct
{
  MetaWindow *ancestor;
  gboolean found;
} FindAncestorData;

static gboolean
find_ancestor_func (MetaWindow *window,
                    void       *data)
{
  FindAncestorData *d = data;

  if (window == d->ancestor)
    {
      d->found = TRUE;
      return FALSE;
    }

  return TRUE;
}

/**
 * meta_window_is_ancestor_of_transient:
 * @window: a #MetaWindow
 * @transient: a #MetaWindow
 *
 * The function determines whether @window is an ancestor of @transient; it does
 * so by traversing the @transient's ancestors until it either locates @window
 * or reaches an ancestor that is not transient.
 *
 * Return Value: %TRUE if window is an ancestor of transient.
 */
gboolean
meta_window_is_ancestor_of_transient (MetaWindow *window,
                                      MetaWindow *transient)
{
  FindAncestorData d;

  d.ancestor = window;
  d.found = FALSE;

  meta_window_foreach_ancestor (transient, find_ancestor_func, &d);

  return d.found;
}

/* Warp pointer to location appropriate for grab,
 * return root coordinates where pointer ended up.
 */
static gboolean
warp_grab_pointer (MetaWindow          *window,
                   MetaGrabOp           grab_op,
                   int                 *x,
                   int                 *y)
{
  MetaRectangle  rect;
  MetaDisplay   *display;

  display = window->display;

  /* We may not have done begin_grab_op yet, i.e. may not be in a grab
   */

  meta_window_get_frame_rect (window, &rect);

  if (grab_op & META_GRAB_OP_WINDOW_DIR_WEST)
    *x = 0;
  else if (grab_op & META_GRAB_OP_WINDOW_DIR_EAST)
    *x = rect.width - 1;
  else
    *x = rect.width / 2;

  if (grab_op & META_GRAB_OP_WINDOW_DIR_NORTH)
    *y = 0;
  else if (grab_op & META_GRAB_OP_WINDOW_DIR_SOUTH)
    *y = rect.height - 1;
  else
    *y = rect.height / 2;

  *x += rect.x;
  *y += rect.y;

  /* Avoid weird bouncing at the screen edge; see bug 154706 */
  *x = CLAMP (*x, 0, window->screen->rect.width-1);
  *y = CLAMP (*y, 0, window->screen->rect.height-1);

  meta_error_trap_push (display);

  meta_topic (META_DEBUG_WINDOW_OPS,
              "Warping pointer to %d,%d with window at %d,%d\n",
              *x, *y, rect.x, rect.y);

  /* Need to update the grab positions so that the MotionNotify and other
   * events generated by the XWarpPointer() call below don't cause complete
   * funkiness.  See bug 124582 and bug 122670.
   */
  display->grab_anchor_root_x = *x;
  display->grab_anchor_root_y = *y;
  display->grab_latest_motion_x = *x;
  display->grab_latest_motion_y = *y;
  meta_window_get_frame_rect (window,
                              &display->grab_anchor_window_pos);

  {
    MetaBackend *backend = meta_get_backend ();
    meta_backend_warp_pointer (backend, *x, *y);
  }

  if (meta_error_trap_pop_with_return (display) != Success)
    {
      meta_verbose ("Failed to warp pointer for window %s\n",
                    window->desc);
      return FALSE;
    }

  return TRUE;
}

void
meta_window_begin_grab_op (MetaWindow *window,
                           MetaGrabOp  op,
                           gboolean    frame_action,
                           guint32     timestamp)
{
  int x, y;

  warp_grab_pointer (window,
                     op, &x, &y);

  meta_display_begin_grab_op (window->display,
                              window->screen,
                              window,
                              op,
                              FALSE,
                              frame_action,
                              0 /* button */,
                              0,
                              timestamp,
                              x, y);
}

void
meta_window_update_keyboard_resize (MetaWindow *window,
                                    gboolean    update_cursor)
{
  int x, y;

  warp_grab_pointer (window,
                     window->display->grab_op,
                     &x, &y);

  if (update_cursor)
    meta_display_update_cursor (window->display);
}

void
meta_window_update_keyboard_move (MetaWindow *window)
{
  int x, y;

  warp_grab_pointer (window,
                     window->display->grab_op,
                     &x, &y);
}

void
meta_window_update_layer (MetaWindow *window)
{
  MetaGroup *group;

  meta_stack_freeze (window->screen->stack);
  group = meta_window_get_group (window);
  if (group)
    meta_group_update_layers (group);
  else
    meta_stack_update_layer (window->screen->stack, window);
  meta_stack_thaw (window->screen->stack);
}

/* ensure_mru_position_after ensures that window appears after
 * below_this_one in the active_workspace's mru_list (i.e. it treats
 * window as having been less recently used than below_this_one)
 */
static void
ensure_mru_position_after (MetaWindow *window,
                           MetaWindow *after_this_one)
{
  /* This is sort of slow since it runs through the entire list more
   * than once (especially considering the fact that we expect the
   * windows of interest to be the first two elements in the list),
   * but it doesn't matter while we're only using it on new window
   * map.
   */

  GList* active_mru_list;
  GList* window_position;
  GList* after_this_one_position;

  active_mru_list         = window->screen->active_workspace->mru_list;
  window_position         = g_list_find (active_mru_list, window);
  after_this_one_position = g_list_find (active_mru_list, after_this_one);

  /* after_this_one_position is NULL when we switch workspaces, but in
   * that case we don't need to do any MRU shuffling so we can simply
   * return.
   */
  if (after_this_one_position == NULL)
    return;

  if (g_list_length (window_position) > g_list_length (after_this_one_position))
    {
      window->screen->active_workspace->mru_list =
        g_list_delete_link (window->screen->active_workspace->mru_list,
                            window_position);

      window->screen->active_workspace->mru_list =
        g_list_insert_before (window->screen->active_workspace->mru_list,
                              after_this_one_position->next,
                              window);
    }
}

void
meta_window_stack_just_below (MetaWindow *window,
                              MetaWindow *below_this_one)
{
  g_return_if_fail (window         != NULL);
  g_return_if_fail (below_this_one != NULL);

  if (window->stack_position > below_this_one->stack_position)
    {
      meta_topic (META_DEBUG_STACK,
                  "Setting stack position of window %s to %d (making it below window %s).\n",
                  window->desc,
                  below_this_one->stack_position,
                  below_this_one->desc);
      meta_window_set_stack_position (window, below_this_one->stack_position);
    }
  else
    {
      meta_topic (META_DEBUG_STACK,
                  "Window %s  was already below window %s.\n",
                  window->desc, below_this_one->desc);
    }
}

/**
 * meta_window_get_user_time:
 * @window: a #MetaWindow
 *
 * The user time represents a timestamp for the last time the user
 * interacted with this window.  Note this property is only available
 * for non-override-redirect windows.
 *
 * The property is set by Mutter initially upon window creation,
 * and updated thereafter on input events (key and button presses) seen by Mutter,
 * client updates to the _NET_WM_USER_TIME property (if later than the current time)
 * and when focusing the window.
 *
 * Returns: The last time the user interacted with this window.
 */
guint32
meta_window_get_user_time (MetaWindow *window)
{
  return window->net_wm_user_time;
}

void
meta_window_set_user_time (MetaWindow *window,
                           guint32     timestamp)
{
  /* FIXME: If Soeren's suggestion in bug 151984 is implemented, it will allow
   * us to sanity check the timestamp here and ensure it doesn't correspond to
   * a future time.
   */

  g_return_if_fail (!window->override_redirect);

  /* Only update the time if this timestamp is newer... */
  if (window->net_wm_user_time_set &&
      XSERVER_TIME_IS_BEFORE (timestamp, window->net_wm_user_time))
    {
      meta_topic (META_DEBUG_STARTUP,
                  "Window %s _NET_WM_USER_TIME not updated to %u, because it "
                  "is less than %u\n",
                  window->desc, timestamp, window->net_wm_user_time);
    }
  else
    {
      meta_topic (META_DEBUG_STARTUP,
                  "Window %s has _NET_WM_USER_TIME of %u\n",
                  window->desc, timestamp);
      window->net_wm_user_time_set = TRUE;
      window->net_wm_user_time = timestamp;
      if (XSERVER_TIME_IS_BEFORE (window->display->last_user_time, timestamp))
        window->display->last_user_time = timestamp;

      /* If this is a terminal, user interaction with it means the user likely
       * doesn't want to have focus transferred for now due to new windows.
       */
      if (meta_prefs_get_focus_new_windows () == G_DESKTOP_FOCUS_NEW_WINDOWS_STRICT &&
          window_is_terminal (window))
        window->display->allow_terminal_deactivation = FALSE;
    }

  g_object_notify_by_pspec (G_OBJECT (window), obj_props[PROP_USER_TIME]);
}

/**
 * meta_window_get_stable_sequence:
 * @window: A #MetaWindow
 *
 * The stable sequence number is a monotonicially increasing
 * unique integer assigned to each #MetaWindow upon creation.
 *
 * This number can be useful for sorting windows in a stable
 * fashion.
 *
 * Returns: Internal sequence number for this window
 */
guint32
meta_window_get_stable_sequence (MetaWindow *window)
{
  g_return_val_if_fail (META_IS_WINDOW (window), 0);

  return window->stable_sequence;
}

/* Sets the demands_attention hint on a window, but only
 * if it's at least partially obscured (see #305882).
 */
void
meta_window_set_demands_attention (MetaWindow *window)
{
  MetaRectangle candidate_rect, other_rect;
  GList *stack = window->screen->stack->sorted;
  MetaWindow *other_window;
  gboolean obscured = FALSE;

  MetaWorkspace *workspace = window->screen->active_workspace;

  if (window->wm_state_demands_attention)
    return;

  if (!meta_window_located_on_workspace (window, workspace))
    {
      /* windows on other workspaces are necessarily obscured */
      obscured = TRUE;
    }
  else if (window->minimized)
    {
      obscured = TRUE;
    }
  else
    {
      meta_window_get_frame_rect (window, &candidate_rect);

      /* The stack is sorted with the top windows first. */

      while (stack != NULL && stack->data != window)
        {
          other_window = stack->data;
          stack = stack->next;

          if (meta_window_located_on_workspace (other_window, workspace))
            {
              meta_window_get_frame_rect (other_window, &other_rect);

              if (meta_rectangle_overlap (&candidate_rect, &other_rect))
                {
                  obscured = TRUE;
                  break;
                }
            }
        }
    }

  if (obscured)
    {
      meta_topic (META_DEBUG_WINDOW_OPS,
                  "Marking %s as needing attention\n",
                  window->desc);

      window->wm_state_demands_attention = TRUE;
      set_net_wm_state (window);
      g_object_notify_by_pspec (G_OBJECT (window), obj_props[PROP_DEMANDS_ATTENTION]);
      g_signal_emit_by_name (window->display, "window-demands-attention",
                             window);
    }
  else
    {
      /* If the window's in full view, there's no point setting the flag. */

      meta_topic (META_DEBUG_WINDOW_OPS,
                 "Not marking %s as needing attention because "
                 "it's in full view\n",
                 window->desc);
    }
}

void
meta_window_unset_demands_attention (MetaWindow *window)
{
  meta_topic (META_DEBUG_WINDOW_OPS,
      "Marking %s as not needing attention\n", window->desc);

  if (window->wm_state_demands_attention)
    {
      window->wm_state_demands_attention = FALSE;
      set_net_wm_state (window);
      g_object_notify_by_pspec (G_OBJECT (window), obj_props[PROP_DEMANDS_ATTENTION]);
    }
}

/**
 * meta_window_get_frame: (skip)
 * @window: a #MetaWindow
 *
 */
MetaFrame *
meta_window_get_frame (MetaWindow *window)
{
  return window->frame;
}

/**
 * meta_window_appears_focused:
 * @window: a #MetaWindow
 *
 * Determines if the window should be drawn with a focused appearance. This is
 * true for focused windows but also true for windows with a focused modal
 * dialog attached.
 *
 * Return value: %TRUE if the window should be drawn with a focused frame
 */
gboolean
meta_window_appears_focused (MetaWindow *window)
{
  return window->has_focus || (window->attached_focus_window != NULL);
}

gboolean
meta_window_has_focus (MetaWindow *window)
{
  return window->has_focus;
}

gboolean
meta_window_is_shaded (MetaWindow *window)
{
  return window->shaded;
}

/**
 * meta_window_is_override_redirect:
 * @window: A #MetaWindow
 *
 * Returns: %TRUE if this window isn't managed by mutter; it will
 * control its own positioning and mutter won't draw decorations
 * among other things.  In X terminology this is "override redirect".
 */
gboolean
meta_window_is_override_redirect (MetaWindow *window)
{
  return window->override_redirect;
}

/**
 * meta_window_is_skip_taskbar:
 * @window: A #MetaWindow
 *
 * Gets whether this window should be ignored by task lists.
 *
 * Return value: %TRUE if the skip bar hint is set.
 */
gboolean
meta_window_is_skip_taskbar (MetaWindow *window)
{
  g_return_val_if_fail (META_IS_WINDOW (window), FALSE);

  return window->skip_taskbar;
}

/**
 * meta_window_get_screen:
 * @window: a #MetaWindow
 *
 * Gets the #MetaScreen that the window is on.
 *
 * Return value: (transfer none): the #MetaScreen for the window
 */
MetaScreen *
meta_window_get_screen (MetaWindow *window)
{
  return window->screen;
}

/**
 * meta_window_get_display:
 * @window: A #MetaWindow
 *
 * Returns: (transfer none): The display for @window
 */
MetaDisplay *
meta_window_get_display (MetaWindow *window)
{
  return window->display;
}

/**
 * meta_window_get_xwindow: (skip)
 * @window: a #MetaWindow
 *
 */
Window
meta_window_get_xwindow (MetaWindow *window)
{
  return window->xwindow;
}

MetaWindowType
meta_window_get_window_type (MetaWindow *window)
{
  return window->type;
}

/**
 * meta_window_get_workspace:
 * @window: a #MetaWindow
 *
 * Gets the #MetaWorkspace that the window is currently displayed on.
 * If the window is on all workspaces, returns the currently active
 * workspace.
 *
 * Return value: (transfer none): the #MetaWorkspace for the window
 */
MetaWorkspace *
meta_window_get_workspace (MetaWindow *window)
{
  if (window->on_all_workspaces)
    return window->screen->active_workspace;
  else
    return window->workspace;
}

gboolean
meta_window_is_on_all_workspaces (MetaWindow *window)
{
  return window->on_all_workspaces;
}

gboolean
meta_window_is_hidden (MetaWindow *window)
{
  return window->hidden;
}

const char *
meta_window_get_description (MetaWindow *window)
{
  if (!window)
    return NULL;

  return window->desc;
}

/**
 * meta_window_get_wm_class:
 * @window: a #MetaWindow
 *
 * Return the current value of the name part of WM_CLASS X property.
 */
const char *
meta_window_get_wm_class (MetaWindow *window)
{
  if (!window)
    return NULL;

  return window->res_class;
}

/**
 * meta_window_get_wm_class_instance:
 * @window: a #MetaWindow
 *
 * Return the current value of the instance part of WM_CLASS X property.
 */
const char *
meta_window_get_wm_class_instance (MetaWindow *window)
{
  if (!window)
    return NULL;

  return window->res_name;
}

/**
 * meta_window_get_gtk_theme_variant:
 * @window: a #MetaWindow
 *
 * Return value: (transfer none): the theme variant or %NULL
 **/
const char *
meta_window_get_gtk_theme_variant (MetaWindow *window)
{
  return window->gtk_theme_variant;
}

/**
 * meta_window_get_gtk_application_id:
 * @window: a #MetaWindow
 *
 * Return value: (transfer none): the application ID
 **/
const char *
meta_window_get_gtk_application_id (MetaWindow *window)
{
  return window->gtk_application_id;
}

/**
 * meta_window_get_gtk_unique_bus_name:
 * @window: a #MetaWindow
 *
 * Return value: (transfer none): the unique name
 **/
const char *
meta_window_get_gtk_unique_bus_name (MetaWindow *window)
{
  return window->gtk_unique_bus_name;
}

/**
 * meta_window_get_gtk_application_object_path:
 * @window: a #MetaWindow
 *
 * Return value: (transfer none): the object path
 **/
const char *
meta_window_get_gtk_application_object_path (MetaWindow *window)
{
  return window->gtk_application_object_path;
}

/**
 * meta_window_get_gtk_window_object_path:
 * @window: a #MetaWindow
 *
 * Return value: (transfer none): the object path
 **/
const char *
meta_window_get_gtk_window_object_path (MetaWindow *window)
{
  return window->gtk_window_object_path;
}

/**
 * meta_window_get_gtk_app_menu_object_path:
 * @window: a #MetaWindow
 *
 * Return value: (transfer none): the object path
 **/
const char *
meta_window_get_gtk_app_menu_object_path (MetaWindow *window)
{
  return window->gtk_app_menu_object_path;
}

/**
 * meta_window_get_gtk_menubar_object_path:
 * @window: a #MetaWindow
 *
 * Return value: (transfer none): the object path
 **/
const char *
meta_window_get_gtk_menubar_object_path (MetaWindow *window)
{
  return window->gtk_menubar_object_path;
}

/**
 * meta_window_get_compositor_private:
 * @window: a #MetaWindow
 *
 * Gets the compositor's wrapper object for @window.
 *
 * Return value: (transfer none): the wrapper object.
 **/
GObject *
meta_window_get_compositor_private (MetaWindow *window)
{
  if (!window)
    return NULL;
  return window->compositor_private;
}

void
meta_window_set_compositor_private (MetaWindow *window, GObject *priv)
{
  if (!window)
    return;
  window->compositor_private = priv;
}

const char *
meta_window_get_role (MetaWindow *window)
{
  if (!window)
    return NULL;

  return window->role;
}

/**
 * meta_window_get_title:
 * @window: a #MetaWindow
 *
 * Returns: the current title of the window.
 */
const char *
meta_window_get_title (MetaWindow *window)
{
  g_return_val_if_fail (META_IS_WINDOW (window), NULL);

  return window->title;
}

MetaStackLayer
meta_window_get_layer (MetaWindow *window)
{
  return window->layer;
}

/**
 * meta_window_get_transient_for:
 * @window: a #MetaWindow
 *
 * Returns the #MetaWindow for the window that is pointed to by the
 * WM_TRANSIENT_FOR hint on this window (see XGetTransientForHint()
 * or XSetTransientForHint()). Metacity keeps transient windows above their
 * parents. A typical usage of this hint is for a dialog that wants to stay
 * above its associated window.
 *
 * Return value: (transfer none): the window this window is transient for, or
 * %NULL if the WM_TRANSIENT_FOR hint is unset or does not point to a toplevel
 * window that Metacity knows about.
 */
MetaWindow *
meta_window_get_transient_for (MetaWindow *window)
{
  g_return_val_if_fail (META_IS_WINDOW (window), NULL);

  if (window->transient_for)
    return window->transient_for;
  else if (window->xtransient_for)
    return meta_display_lookup_x_window (window->display,
                                         window->xtransient_for);
  else
    return NULL;
}

/**
 * meta_window_get_pid:
 * @window: a #MetaWindow
 *
 * Returns pid of the process that created this window, if known (obtained from
 * the _NET_WM_PID property).
 *
 * Return value: the pid, or -1 if not known.
 */
int
meta_window_get_pid (MetaWindow *window)
{
  g_return_val_if_fail (META_IS_WINDOW (window), -1);

  return window->net_wm_pid;
}

/**
 * meta_window_get_client_machine:
 * @window: a #MetaWindow
 *
 * Returns name of the client machine from which this windows was created,
 * if known (obtained from the WM_CLIENT_MACHINE property).
 *
 * Return value: (transfer none): the machine name, or NULL; the string is
 * owned by the window manager and should not be freed or modified by the
 * caller.
 */
const char *
meta_window_get_client_machine (MetaWindow *window)
{
  g_return_val_if_fail (META_IS_WINDOW (window), NULL);

  return window->wm_client_machine;
}

/**
 * meta_window_is_remote:
 * @window: a #MetaWindow
 *
 * Returns: %TRUE if this window originates from a host
 * different from the one running mutter.
 */
gboolean
meta_window_is_remote (MetaWindow *window)
{
  return window->is_remote;
}

/**
 * meta_window_get_mutter_hints:
 * @window: a #MetaWindow
 *
 * Gets the current value of the _MUTTER_HINTS property.
 *
 * The purpose of the hints is to allow fine-tuning of the Window Manager and
 * Compositor behaviour on per-window basis, and is intended primarily for
 * hints that are plugin-specific.
 *
 * The property is a list of colon-separated key=value pairs. The key names for
 * any plugin-specific hints must be suitably namespaced to allow for shared
 * use; 'mutter-' key prefix is reserved for internal use, and must not be used
 * by plugins.
 *
 * Return value: (transfer none): the _MUTTER_HINTS string, or %NULL if no hints
 * are set.
 */
const char *
meta_window_get_mutter_hints (MetaWindow *window)
{
  g_return_val_if_fail (META_IS_WINDOW (window), NULL);

  return window->mutter_hints;
}

/**
 * meta_window_get_frame_type:
 * @window: a #MetaWindow
 *
 * Gets the type of window decorations that should be used for this window.
 *
 * Return value: the frame type
 */
MetaFrameType
meta_window_get_frame_type (MetaWindow *window)
{
  MetaFrameType base_type = META_FRAME_TYPE_LAST;

  switch (window->type)
    {
    case META_WINDOW_NORMAL:
      base_type = META_FRAME_TYPE_NORMAL;
      break;

    case META_WINDOW_DIALOG:
      base_type = META_FRAME_TYPE_DIALOG;
      break;

    case META_WINDOW_MODAL_DIALOG:
      if (meta_window_is_attached_dialog (window))
        base_type = META_FRAME_TYPE_ATTACHED;
      else
        base_type = META_FRAME_TYPE_MODAL_DIALOG;
      break;

    case META_WINDOW_MENU:
      base_type = META_FRAME_TYPE_MENU;
      break;

    case META_WINDOW_UTILITY:
      base_type = META_FRAME_TYPE_UTILITY;
      break;

    case META_WINDOW_DESKTOP:
    case META_WINDOW_DOCK:
    case META_WINDOW_TOOLBAR:
    case META_WINDOW_SPLASHSCREEN:
    case META_WINDOW_DROPDOWN_MENU:
    case META_WINDOW_POPUP_MENU:
    case META_WINDOW_TOOLTIP:
    case META_WINDOW_NOTIFICATION:
    case META_WINDOW_COMBO:
    case META_WINDOW_DND:
    case META_WINDOW_OVERRIDE_OTHER:
      /* No frame */
      base_type = META_FRAME_TYPE_LAST;
      break;
    }

  if (base_type == META_FRAME_TYPE_LAST)
    {
      /* can't add border if undecorated */
      return META_FRAME_TYPE_LAST;
    }
  else if (window->border_only ||
           (window->hide_titlebar_when_maximized && META_WINDOW_MAXIMIZED (window)) ||
           (window->hide_titlebar_when_maximized && META_WINDOW_TILED_SIDE_BY_SIDE (window)))
    {
      /* override base frame type */
      return META_FRAME_TYPE_BORDER;
    }
  else
    {
      return base_type;
    }
}

/**
 * meta_window_get_frame_bounds:
 * @window: a #MetaWindow
 *
 * Gets a region representing the outer bounds of the window's frame.
 *
 * Return value: (transfer none) (nullable): a #cairo_region_t
 *  holding the outer bounds of the window, or %NULL if the window
 *  doesn't have a frame.
 */
cairo_region_t *
meta_window_get_frame_bounds (MetaWindow *window)
{
  if (!window->frame_bounds)
    {
      if (window->frame)
        window->frame_bounds = meta_frame_get_frame_bounds (window->frame);
    }

  return window->frame_bounds;
}

/**
 * meta_window_is_attached_dialog:
 * @window: a #MetaWindow
 *
 * Tests if @window is should be attached to its parent window.
 * (If the "attach_modal_dialogs" option is not enabled, this will
 * always return %FALSE.)
 *
 * Return value: whether @window should be attached to its parent
 */
gboolean
meta_window_is_attached_dialog (MetaWindow *window)
{
  return window->attached;
}

/**
 * meta_window_get_tile_match:
 * @window: a #MetaWindow
 *
 * Returns the matching tiled window on the same monitor as @window. This is
 * the topmost tiled window in a complementary tile mode that is:
 *
 *  - on the same monitor;
 *  - on the same workspace;
 *  - spanning the remaining monitor width;
 *  - there is no 3rd window stacked between both tiled windows that's
 *    partially visible in the common edge.
 *
 * Return value: (transfer none) (nullable): the matching tiled window or
 * %NULL if it doesn't exist.
 */
MetaWindow *
meta_window_get_tile_match (MetaWindow *window)
{
  return window->tile_match;
}

void
meta_window_compute_tile_match (MetaWindow *window)
{
  MetaWindow *match;
  MetaStack *stack;
  MetaTileMode match_tile_mode = META_TILE_NONE;

  window->tile_match = NULL;

  if (window->shaded || window->minimized)
    return;

  if (META_WINDOW_TILED_LEFT (window))
    match_tile_mode = META_TILE_RIGHT;
  else if (META_WINDOW_TILED_RIGHT (window))
    match_tile_mode = META_TILE_LEFT;
  else
    return;

  stack = window->screen->stack;

  for (match = meta_stack_get_top (stack);
       match;
       match = meta_stack_get_below (stack, match, FALSE))
    {
      if (!match->shaded &&
          !match->minimized &&
          match->tile_mode == match_tile_mode &&
          match->monitor == window->monitor &&
          meta_window_get_workspace (match) == meta_window_get_workspace (window))
        break;
    }

  if (match)
    {
      MetaWindow *above, *bottommost, *topmost;
      MetaRectangle above_rect, bottommost_rect, topmost_rect;

      if (meta_stack_windows_cmp (window->screen->stack, match, window) > 0)
        {
          topmost = match;
          bottommost = window;
        }
      else
        {
          topmost = window;
          bottommost = match;
        }

      meta_window_get_frame_rect (bottommost, &bottommost_rect);
      meta_window_get_frame_rect (topmost, &topmost_rect);
      /*
       * If there's a window stacked in between which is partially visible
       * behind the topmost tile we don't consider the tiles to match.
       */
      for (above = meta_stack_get_above (stack, bottommost, FALSE);
           above && above != topmost;
           above = meta_stack_get_above (stack, above, FALSE))
        {
          if (above->minimized ||
              above->monitor != window->monitor ||
              meta_window_get_workspace (above) != meta_window_get_workspace (window))
            continue;

          meta_window_get_frame_rect (above, &above_rect);

          if (meta_rectangle_overlap (&above_rect, &bottommost_rect) &&
              meta_rectangle_overlap (&above_rect, &topmost_rect))
            return;
        }

      window->tile_match = match;
    }
}

void
meta_window_set_title (MetaWindow *window,
                       const char *title)
{
  g_free (window->title);
  window->title = g_strdup (title);

  if (window->frame)
    meta_frame_update_title (window->frame);

  meta_window_update_desc (window);

  g_object_notify_by_pspec (G_OBJECT (window), obj_props[PROP_TITLE]);
}

void
meta_window_set_wm_class (MetaWindow *window,
                          const char *wm_class,
                          const char *wm_instance)
{
  g_free (window->res_class);
  g_free (window->res_name);

  window->res_name = g_strdup (wm_instance);
  window->res_class = g_strdup (wm_class);

  g_object_notify_by_pspec (G_OBJECT (window), obj_props[PROP_WM_CLASS]);
}

void
meta_window_set_gtk_dbus_properties (MetaWindow *window,
                                     const char *application_id,
                                     const char *unique_bus_name,
                                     const char *appmenu_path,
                                     const char *menubar_path,
                                     const char *application_object_path,
                                     const char *window_object_path)
{
  g_object_freeze_notify (G_OBJECT (window));

  g_free (window->gtk_application_id);
  window->gtk_application_id = g_strdup (application_id);
  g_object_notify_by_pspec (G_OBJECT (window), obj_props[PROP_GTK_APPLICATION_ID]);

  g_free (window->gtk_unique_bus_name);
  window->gtk_unique_bus_name = g_strdup (unique_bus_name);
  g_object_notify_by_pspec (G_OBJECT (window), obj_props[PROP_GTK_UNIQUE_BUS_NAME]);

  g_free (window->gtk_app_menu_object_path);
  window->gtk_app_menu_object_path = g_strdup (appmenu_path);
  g_object_notify_by_pspec (G_OBJECT (window), obj_props[PROP_GTK_APP_MENU_OBJECT_PATH]);

  g_free (window->gtk_menubar_object_path);
  window->gtk_menubar_object_path = g_strdup (menubar_path);
  g_object_notify_by_pspec (G_OBJECT (window), obj_props[PROP_GTK_MENUBAR_OBJECT_PATH]);

  g_free (window->gtk_application_object_path);
  window->gtk_application_object_path = g_strdup (application_object_path);
  g_object_notify_by_pspec (G_OBJECT (window), obj_props[PROP_GTK_APPLICATION_OBJECT_PATH]);

  g_free (window->gtk_window_object_path);
  window->gtk_window_object_path = g_strdup (window_object_path);
  g_object_notify_by_pspec (G_OBJECT (window), obj_props[PROP_GTK_WINDOW_OBJECT_PATH]);

  g_object_thaw_notify (G_OBJECT (window));
}

void
meta_window_set_transient_for (MetaWindow *window,
                               MetaWindow *parent)
{
  if (meta_window_appears_focused (window) && window->transient_for != NULL)
    meta_window_propagate_focus_appearance (window, FALSE);

  /* may now be a dialog */
  if (window->client_type == META_WINDOW_CLIENT_TYPE_X11)
    meta_window_x11_recalc_window_type (window);

  if (!window->constructing)
    {
      /* If the window attaches, detaches, or changes attached
       * parents, we need to destroy the MetaWindow and let a new one
       * be created (which happens as a side effect of
       * meta_window_unmanage()). The condition below is correct
       * because we know window->transient_for has changed.
       */
      if (window->attached || meta_window_should_attach_to_parent (window))
        {
          guint32 timestamp;

          timestamp = meta_display_get_current_time_roundtrip (window->display);
          meta_window_unmanage (window, timestamp);
          return;
        }
    }

  /* update stacking constraints */
  if (!window->override_redirect)
    meta_stack_update_transient (window->screen->stack, window);

  /* We know this won't create a reference cycle because we check for loops */
  g_clear_object (&window->transient_for);
  window->transient_for = parent ? g_object_ref (parent) : NULL;

  /* possibly change its group. We treat being a window's transient as
   * equivalent to making it your group leader, to work around shortcomings
   * in programs such as xmms-- see #328211.
   */
  if (window->xtransient_for != None &&
      window->xgroup_leader != None &&
      window->xtransient_for != window->xgroup_leader)
    meta_window_group_leader_changed (window);

  if (!window->constructing && !window->override_redirect)
    meta_window_queue (window, META_QUEUE_MOVE_RESIZE);

  if (meta_window_appears_focused (window) && window->transient_for != NULL)
    meta_window_propagate_focus_appearance (window, TRUE);
}

void
meta_window_set_opacity (MetaWindow *window,
                         guint8      opacity)
{
  window->opacity = opacity;

  meta_compositor_window_opacity_changed (window->display->compositor, window);
}

static void
reset_ignored_crossing_serials (MetaDisplay *display)
{
  int i;

  i = 0;
  while (i < N_IGNORED_CROSSING_SERIALS)
    {
      display->ignored_crossing_serials[i] = 0;
      ++i;
    }
}

typedef struct
{
  MetaWindow *window;
  int pointer_x;
  int pointer_y;
} MetaFocusData;

static void
mouse_mode_focus (MetaWindow  *window,
                  guint32      timestamp)
{
  MetaDisplay *display = window->display;

  if (window->type != META_WINDOW_DESKTOP)
    {
      meta_topic (META_DEBUG_FOCUS,
                  "Focusing %s at time %u.\n", window->desc, timestamp);

      meta_window_focus (window, timestamp);

      if (meta_prefs_get_auto_raise ())
        meta_display_queue_autoraise_callback (display, window);
      else
        meta_topic (META_DEBUG_FOCUS, "Auto raise is disabled\n");
    }
  else
    {
      /* In mouse focus mode, we defocus when the mouse *enters*
       * the DESKTOP window, instead of defocusing on LeaveNotify.
       * This is because having the mouse enter override-redirect
       * child windows unfortunately causes LeaveNotify events that
       * we can't distinguish from the mouse actually leaving the
       * toplevel window as we expect.  But, since we filter out
       * EnterNotify events on override-redirect windows, this
       * alternative mechanism works great.
       */
      if (meta_prefs_get_focus_mode() == G_DESKTOP_FOCUS_MODE_MOUSE &&
          display->focus_window != NULL)
        {
          meta_topic (META_DEBUG_FOCUS,
                      "Unsetting focus from %s due to mouse entering "
                      "the DESKTOP window\n",
                      display->focus_window->desc);
          meta_display_focus_the_no_focus_window (display,
                                                  window->screen,
                                                  timestamp);
        }
    }
}

static gboolean
window_has_pointer_wayland (MetaWindow *window)
{
  ClutterDeviceManager *dm;
  ClutterInputDevice *dev;
  ClutterActor *pointer_actor, *window_actor;

  dm = clutter_device_manager_get_default ();
  dev = clutter_device_manager_get_core_device (dm, CLUTTER_POINTER_DEVICE);
  pointer_actor = clutter_input_device_get_pointer_actor (dev);
  window_actor = CLUTTER_ACTOR (meta_window_get_compositor_private (window));

  return pointer_actor && clutter_actor_contains (window_actor, pointer_actor);
}

static gboolean
window_has_pointer_x11 (MetaWindow *window)
{
  MetaDisplay *display = window->display;
  MetaScreen *screen = window->screen;
  Window root, child;
  double root_x, root_y, x, y;
  XIButtonState buttons;
  XIModifierState mods;
  XIGroupState group;

  meta_error_trap_push (display);
  XIQueryPointer (display->xdisplay,
                  META_VIRTUAL_CORE_POINTER_ID,
                  screen->xroot,
                  &root, &child,
                  &root_x, &root_y, &x, &y,
                  &buttons, &mods, &group);
  meta_error_trap_pop (display);
  free (buttons.mask);

  return meta_display_lookup_x_window (display, child) == window;
}

gboolean
meta_window_has_pointer (MetaWindow *window)
{
  if (meta_is_wayland_compositor ())
    return window_has_pointer_wayland (window);
  else
    return window_has_pointer_x11 (window);
}

static gboolean
window_focus_on_pointer_rest_callback (gpointer data)
{
  MetaFocusData *focus_data = data;
  MetaWindow *window = focus_data->window;
  MetaDisplay *display = window->display;
  MetaScreen *screen = window->screen;
  MetaCursorTracker *tracker = meta_cursor_tracker_get_for_screen (screen);
  int root_x, root_y;
  guint32 timestamp;

  if (meta_prefs_get_focus_mode () == G_DESKTOP_FOCUS_MODE_CLICK)
    goto out;

  meta_cursor_tracker_get_pointer (tracker, &root_x, &root_y, NULL);

  if (root_x != focus_data->pointer_x ||
      root_y != focus_data->pointer_y)
    {
      focus_data->pointer_x = root_x;
      focus_data->pointer_y = root_y;
      return G_SOURCE_CONTINUE;
    }

  if (!meta_window_has_pointer (window))
    goto out;

  timestamp = meta_display_get_current_time_roundtrip (display);
  mouse_mode_focus (window, timestamp);

 out:
  display->focus_timeout_id = 0;
  return G_SOURCE_REMOVE;
}

/* The interval, in milliseconds, we use in focus-follows-mouse
 * mode to check whether the pointer has stopped moving after a
 * crossing event.
 */
#define FOCUS_TIMEOUT_DELAY 25

static void
queue_focus_callback (MetaDisplay *display,
                      MetaWindow  *window,
                      int          pointer_x,
                      int          pointer_y)
{
  MetaFocusData *focus_data;

  focus_data = g_new (MetaFocusData, 1);
  focus_data->window = window;
  focus_data->pointer_x = pointer_x;
  focus_data->pointer_y = pointer_y;

  if (display->focus_timeout_id != 0)
    g_source_remove (display->focus_timeout_id);

  display->focus_timeout_id =
    g_timeout_add_full (G_PRIORITY_DEFAULT,
                        FOCUS_TIMEOUT_DELAY,
                        window_focus_on_pointer_rest_callback,
                        focus_data,
                        g_free);
  g_source_set_name_by_id (display->focus_timeout_id,
                           "[mutter] window_focus_on_pointer_rest_callback");
}

void
meta_window_handle_enter (MetaWindow  *window,
                          guint32      timestamp,
                          guint        root_x,
                          guint        root_y)
{
  MetaDisplay *display = window->display;

  switch (meta_prefs_get_focus_mode ())
    {
    case G_DESKTOP_FOCUS_MODE_SLOPPY:
    case G_DESKTOP_FOCUS_MODE_MOUSE:
      display->mouse_mode = TRUE;
      if (window->type != META_WINDOW_DOCK)
        {
          if (meta_prefs_get_focus_change_on_pointer_rest())
            queue_focus_callback (display, window, root_x, root_y);
          else
            mouse_mode_focus (window, timestamp);

          /* stop ignoring stuff */
          reset_ignored_crossing_serials (display);
        }
      break;
    case G_DESKTOP_FOCUS_MODE_CLICK:
      break;
    }

  if (window->type == META_WINDOW_DOCK)
    meta_window_raise (window);
}

void
meta_window_handle_leave (MetaWindow *window)
{
  if (window->type == META_WINDOW_DOCK && !window->has_focus)
    meta_window_lower (window);
}

void
meta_window_handle_ungrabbed_event (MetaWindow         *window,
                                    const ClutterEvent *event)
{
  MetaDisplay *display = window->display;
  gboolean unmodified;
  gboolean is_window_grab;

  if (window->frame && meta_ui_frame_handle_event (window->frame->ui_frame, event))
    return;

  if (event->type != CLUTTER_BUTTON_PRESS)
    return;

  if (display->grab_op != META_GRAB_OP_NONE)
    return;

  /* Some windows might not ask for input, in which case we might be here
   * because we selected for ButtonPress on the root window. In that case,
   * we have to take special care not to act for an override-redirect window.
   */
  if (window->override_redirect)
    return;

  /* We have three passive button grabs:
   * - on any button, without modifiers => focuses and maybe raises the window
   * - on resize button, with modifiers => start an interactive resizing
   *   (normally <Super>middle)
   * - on move button, with modifiers => start an interactive move
   *   (normally <Super>left)
   * - on menu button, with modifiers => show the window menu
   *   (normally <Super>right)
   *
   * We may get here because we actually have a button
   * grab on the window, or because we're a wayland
   * compositor and thus we see all the events, so we
   * need to check if the event is interesting.
   * We want an event that is not modified for a window.
   *
   * We may have other events on the window, for example
   * a click on a frame button, but that's not for us to
   * care about. Just let the event through.
   */

  ClutterModifierType grab_mods = meta_display_get_window_grab_modifiers (display);
  unmodified = (event->button.modifier_state & grab_mods) == 0;
  is_window_grab = (event->button.modifier_state & grab_mods) == grab_mods;

  if (unmodified)
    {
      if (meta_prefs_get_raise_on_click ())
        meta_window_raise (window);
      else
        meta_topic (META_DEBUG_FOCUS,
                    "Not raising window on click due to don't-raise-on-click option\n");

      /* Don't focus panels--they must explicitly request focus.
       * See bug 160470
       */
      if (window->type != META_WINDOW_DOCK)
        {
          meta_topic (META_DEBUG_FOCUS,
                      "Focusing %s due to unmodified button %u press (display.c)\n",
                      window->desc, event->button.button);
          meta_window_focus (window, event->any.time);
        }
      else
        /* However, do allow terminals to lose focus due to new
         * window mappings after the user clicks on a panel.
         */
        display->allow_terminal_deactivation = TRUE;

      meta_verbose ("Allowing events time %u\n",
                    (unsigned int)event->button.time);
    }
  else if (is_window_grab && (int) event->button.button == meta_prefs_get_mouse_button_resize ())
    {
      if (window->has_resize_func)
        {
          gboolean north, south;
          gboolean west, east;
          MetaRectangle frame_rect;
          MetaGrabOp op = META_GRAB_OP_WINDOW_BASE;

          meta_window_get_frame_rect (window, &frame_rect);

          west = event->button.x < (frame_rect.x + 1 * frame_rect.width / 3);
          east = event->button.x > (frame_rect.x + 2 * frame_rect.width / 3);
          north = event->button.y < (frame_rect.y + 1 * frame_rect.height / 3);
          south = event->button.y > (frame_rect.y + 2 * frame_rect.height / 3);

          if (west)
            op |= META_GRAB_OP_WINDOW_DIR_WEST;
          if (east)
            op |= META_GRAB_OP_WINDOW_DIR_EAST;
          if (north)
            op |= META_GRAB_OP_WINDOW_DIR_NORTH;
          if (south)
            op |= META_GRAB_OP_WINDOW_DIR_SOUTH;

          if (op != META_GRAB_OP_WINDOW_BASE)
            meta_display_begin_grab_op (display,
                                        window->screen,
                                        window,
                                        op,
                                        TRUE,
                                        FALSE,
                                        event->button.button,
                                        0,
                                        event->any.time,
                                        event->button.x,
                                        event->button.y);
        }
    }
  else if (is_window_grab && (int) event->button.button == meta_prefs_get_mouse_button_menu ())
    {
      if (meta_prefs_get_raise_on_click ())
        meta_window_raise (window);
      meta_window_show_menu (window,
                             META_WINDOW_MENU_WM,
                             event->button.x,
                             event->button.y);
    }
  else if (is_window_grab && (int) event->button.button == 1)
    {
      if (window->has_move_func)
        {
          meta_display_begin_grab_op (display,
                                      window->screen,
                                      window,
                                      META_GRAB_OP_MOVING,
                                      TRUE,
                                      FALSE,
                                      event->button.button,
                                      0,
                                      event->any.time,
                                      event->button.x,
                                      event->button.y);
        }
    }
}

gboolean
meta_window_can_maximize (MetaWindow *window)
{
  return window->has_maximize_func;
}

gboolean
meta_window_can_minimize (MetaWindow *window)
{
  return window->has_minimize_func;
}

gboolean
meta_window_can_shade (MetaWindow *window)
{
  return window->has_shade_func;
}

gboolean
meta_window_can_close (MetaWindow *window)
{
  return window->has_close_func;
}

gboolean
meta_window_is_always_on_all_workspaces (MetaWindow *window)
{
  return window->always_sticky;
}

gboolean
meta_window_is_above (MetaWindow *window)
{
  return window->wm_state_above;
}

gboolean
meta_window_allows_move (MetaWindow *window)
{
  return META_WINDOW_ALLOWS_MOVE (window);
}

gboolean
meta_window_allows_resize (MetaWindow *window)
{
  return META_WINDOW_ALLOWS_RESIZE (window);
}

void
meta_window_set_urgent (MetaWindow *window,
                        gboolean    urgent)
{
  if (window->urgent == urgent)
    return;

  window->urgent = urgent;
  g_object_notify_by_pspec (G_OBJECT (window), obj_props[PROP_URGENT]);

  if (urgent)
    g_signal_emit_by_name (window->display, "window-marked-urgent", window);
}

void
meta_window_grab_op_began (MetaWindow *window,
                           MetaGrabOp  op)
{
  META_WINDOW_GET_CLASS (window)->grab_op_began (window, op);
}

void
meta_window_grab_op_ended (MetaWindow *window,
                           MetaGrabOp  op)
{
  META_WINDOW_GET_CLASS (window)->grab_op_ended (window, op);
}

void
meta_window_emit_size_changed (MetaWindow *window)
{
  g_signal_emit (window, window_signals[SIZE_CHANGED], 0);
}
