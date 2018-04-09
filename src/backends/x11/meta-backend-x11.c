/* -*- mode: C; c-file-style: "gnu"; indent-tabs-mode: nil; -*- */

/*
 * Copyright (C) 2014 Red Hat
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
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 59 Temple Place - Suite 330, Boston, MA
 * 02111-1307, USA.
 *
 * Written by:
 *     Jasper St. Pierre <jstpierre@mecheye.net>
 */

#include "config.h"

#include <string.h>
#include <stdlib.h>

#include "meta-backend-x11.h"

#include <clutter/x11/clutter-x11.h>

#include <X11/extensions/sync.h>
#include <X11/XKBlib.h>
#include <X11/extensions/XKBrules.h>
#include <X11/Xlib-xcb.h>
#include <xkbcommon/xkbcommon-x11.h>

#include "meta-idle-monitor-xsync.h"
#include "meta-monitor-manager-xrandr.h"
#include "backends/meta-monitor-manager-dummy.h"
#include "meta-cursor-renderer-x11.h"

#include <meta/util.h>
#include "display-private.h"
#include "compositor/compositor-private.h"

struct _MetaBackendX11Private
{
  /* The host X11 display */
  Display *xdisplay;
  xcb_connection_t *xcb;
  GSource *source;

  int xsync_event_base;
  int xsync_error_base;

  int xinput_opcode;
  int xinput_event_base;
  int xinput_error_base;
  Time latest_evtime;

  uint8_t xkb_event_base;
  uint8_t xkb_error_base;

  struct xkb_keymap *keymap;
  gchar *keymap_layouts;
  gchar *keymap_variants;
  gchar *keymap_options;
};
typedef struct _MetaBackendX11Private MetaBackendX11Private;

static void apply_keymap (MetaBackendX11 *x11);

G_DEFINE_TYPE_WITH_PRIVATE (MetaBackendX11, meta_backend_x11, META_TYPE_BACKEND);

static void
handle_alarm_notify (MetaBackend *backend,
                     XEvent      *event)
{
  int i;

  for (i = 0; i <= backend->device_id_max; i++)
    if (backend->device_monitors[i])
      meta_idle_monitor_xsync_handle_xevent (backend->device_monitors[i], (XSyncAlarmNotifyEvent*) event);
}

static void
translate_device_event (MetaBackendX11 *x11,
                        XIDeviceEvent  *device_event)
{
  MetaBackendX11Private *priv = meta_backend_x11_get_instance_private (x11);
  Window stage_window = meta_backend_x11_get_xwindow (x11);

  if (device_event->event != stage_window)
    {
      /* This codepath should only ever trigger as an X11 compositor,
       * and never under nested, as under nested all backend events
       * should be reported with respect to the stage window. */
      g_assert (!meta_is_wayland_compositor ());

      device_event->event = stage_window;

      /* As an X11 compositor, the stage window is always at 0,0, so
       * using root coordinates will give us correct stage coordinates
       * as well... */
      device_event->event_x = device_event->root_x;
      device_event->event_y = device_event->root_y;
    }

  if (!device_event->send_event && device_event->time != CurrentTime)
    {
      if (device_event->time < priv->latest_evtime)
        {
          /* Emulated pointer events received after XIRejectTouch is received
           * on a passive touch grab will contain older timestamps, update those
           * so we dont get InvalidTime at grabs.
           */
          device_event->time = priv->latest_evtime;
        }

      /* Update the internal latest evtime, for any possible later use */
      priv->latest_evtime = device_event->time;
    }
}

/* Clutter makes the assumption that there is only one X window
 * per stage, which is a valid assumption to make for a generic
 * application toolkit. As such, it will ignore any events sent
 * to the a stage that isn't its X window.
 *
 * When running as an X window manager, we need to respond to
 * events from lots of windows. Trick Clutter into translating
 * these events by pretending we got an event on the stage window.
 */
static void
maybe_spoof_event_as_stage_event (MetaBackendX11 *x11,
                                  XEvent         *event)
{
  MetaBackendX11Private *priv = meta_backend_x11_get_instance_private (x11);

  if (event->type == GenericEvent &&
      event->xcookie.extension == priv->xinput_opcode)
    {
      XIEvent *input_event = (XIEvent *) event->xcookie.data;

      switch (input_event->evtype)
        {
        case XI_Motion:
        case XI_ButtonPress:
        case XI_ButtonRelease:
        case XI_KeyPress:
        case XI_KeyRelease:
        case XI_TouchBegin:
        case XI_TouchUpdate:
        case XI_TouchEnd:
          translate_device_event (x11, (XIDeviceEvent *) input_event);
          break;
        default:
          break;
        }
    }
}

static void
keymap_changed (MetaBackend *backend)
{
  MetaBackendX11 *x11 = META_BACKEND_X11 (backend);
  MetaBackendX11Private *priv = meta_backend_x11_get_instance_private (x11);

  if (priv->keymap)
    {
      xkb_keymap_unref (priv->keymap);
      priv->keymap = NULL;
    }

  g_signal_emit_by_name (backend, "keymap-changed", 0);
}

static void
handle_host_xevent (MetaBackend *backend,
                    XEvent      *event)
{
  MetaBackendX11 *x11 = META_BACKEND_X11 (backend);
  MetaBackendX11Private *priv = meta_backend_x11_get_instance_private (x11);
  gboolean bypass_clutter = FALSE;

  XGetEventData (priv->xdisplay, &event->xcookie);

  {
    MetaDisplay *display = meta_get_display ();

    if (display)
      {
        MetaCompositor *compositor = display->compositor;
        if (meta_plugin_manager_xevent_filter (compositor->plugin_mgr, event))
          bypass_clutter = TRUE;
      }
  }

  if (event->type == (priv->xsync_event_base + XSyncAlarmNotify))
    handle_alarm_notify (backend, event);

  if (event->type == priv->xkb_event_base)
    {
      XkbAnyEvent *xkb_ev = (XkbAnyEvent *) event;

      if (xkb_ev->device == META_VIRTUAL_CORE_KEYBOARD_ID)
        {
          switch (xkb_ev->xkb_type)
            {
            case XkbNewKeyboardNotify:
            case XkbMapNotify:
              keymap_changed (backend);
            default:
              break;
            }
        }
    }

  {
    MetaMonitorManager *manager = meta_backend_get_monitor_manager (backend);
    if (META_IS_MONITOR_MANAGER_XRANDR (manager) &&
        meta_monitor_manager_xrandr_handle_xevent (META_MONITOR_MANAGER_XRANDR (manager), event))
      bypass_clutter = TRUE;
  }

  if (!bypass_clutter)
    {
      maybe_spoof_event_as_stage_event (x11, event);
      clutter_x11_handle_event (event);
    }

  XFreeEventData (priv->xdisplay, &event->xcookie);
}

typedef struct {
  GSource base;
  GPollFD event_poll_fd;
  MetaBackend *backend;
} XEventSource;

static gboolean
x_event_source_prepare (GSource *source,
                        int     *timeout)
{
  XEventSource *x_source = (XEventSource *) source;
  MetaBackend *backend = x_source->backend;
  MetaBackendX11 *x11 = META_BACKEND_X11 (backend);
  MetaBackendX11Private *priv = meta_backend_x11_get_instance_private (x11);

  *timeout = -1;

  return XPending (priv->xdisplay);
}

static gboolean
x_event_source_check (GSource *source)
{
  XEventSource *x_source = (XEventSource *) source;
  MetaBackend *backend = x_source->backend;
  MetaBackendX11 *x11 = META_BACKEND_X11 (backend);
  MetaBackendX11Private *priv = meta_backend_x11_get_instance_private (x11);

  return XPending (priv->xdisplay);
}

static gboolean
x_event_source_dispatch (GSource     *source,
                         GSourceFunc  callback,
                         gpointer     user_data)
{
  XEventSource *x_source = (XEventSource *) source;
  MetaBackend *backend = x_source->backend;
  MetaBackendX11 *x11 = META_BACKEND_X11 (backend);
  MetaBackendX11Private *priv = meta_backend_x11_get_instance_private (x11);

  while (XPending (priv->xdisplay))
    {
      XEvent event;

      XNextEvent (priv->xdisplay, &event);

      handle_host_xevent (backend, &event);
    }

  return TRUE;
}

static GSourceFuncs x_event_funcs = {
  x_event_source_prepare,
  x_event_source_check,
  x_event_source_dispatch,
};

static GSource *
x_event_source_new (MetaBackend *backend)
{
  MetaBackendX11 *x11 = META_BACKEND_X11 (backend);
  MetaBackendX11Private *priv = meta_backend_x11_get_instance_private (x11);
  GSource *source;
  XEventSource *x_source;

  source = g_source_new (&x_event_funcs, sizeof (XEventSource));
  x_source = (XEventSource *) source;
  x_source->backend = backend;
  x_source->event_poll_fd.fd = ConnectionNumber (priv->xdisplay);
  x_source->event_poll_fd.events = G_IO_IN;
  g_source_add_poll (source, &x_source->event_poll_fd);

  g_source_attach (source, NULL);
  return source;
}

static void
take_touch_grab (MetaBackend *backend)
{
  MetaBackendX11 *x11 = META_BACKEND_X11 (backend);
  MetaBackendX11Private *priv = meta_backend_x11_get_instance_private (x11);
  unsigned char mask_bits[XIMaskLen (XI_LASTEVENT)] = { 0 };
  XIEventMask mask = { META_VIRTUAL_CORE_POINTER_ID, sizeof (mask_bits), mask_bits };
  XIGrabModifiers mods = { XIAnyModifier, 0 };

  XISetMask (mask.mask, XI_TouchBegin);
  XISetMask (mask.mask, XI_TouchUpdate);
  XISetMask (mask.mask, XI_TouchEnd);

  XIGrabTouchBegin (priv->xdisplay, META_VIRTUAL_CORE_POINTER_ID,
                    DefaultRootWindow (priv->xdisplay),
                    False, &mask, 1, &mods);
}

static void
on_device_added (ClutterDeviceManager *device_manager,
                 ClutterInputDevice   *device,
                 gpointer              user_data)
{
  MetaBackendX11 *x11 = META_BACKEND_X11 (user_data);

  if (clutter_input_device_get_device_type (device) == CLUTTER_KEYBOARD_DEVICE)
    apply_keymap (x11);
}

static void
meta_backend_x11_post_init (MetaBackend *backend)
{
  MetaBackendX11 *x11 = META_BACKEND_X11 (backend);
  MetaBackendX11Private *priv = meta_backend_x11_get_instance_private (x11);
  int major, minor;

  priv->xdisplay = clutter_x11_get_default_display ();

  priv->source = x_event_source_new (backend);

  if (!XSyncQueryExtension (priv->xdisplay, &priv->xsync_event_base, &priv->xsync_error_base) ||
      !XSyncInitialize (priv->xdisplay, &major, &minor))
    meta_fatal ("Could not initialize XSync");

  {
    int major = 2, minor = 3;
    gboolean has_xi = FALSE;

    if (XQueryExtension (priv->xdisplay,
                         "XInputExtension",
                         &priv->xinput_opcode,
                         &priv->xinput_error_base,
                         &priv->xinput_event_base))
      {
        if (XIQueryVersion (priv->xdisplay, &major, &minor) == Success)
          {
            int version = (major * 10) + minor;
            if (version >= 22)
              has_xi = TRUE;
          }
      }

    if (!has_xi)
      meta_fatal ("X server doesn't have the XInput extension, version 2.2 or newer\n");
  }

  take_touch_grab (backend);

  priv->xcb = XGetXCBConnection (priv->xdisplay);
  if (!xkb_x11_setup_xkb_extension (priv->xcb,
                                    XKB_X11_MIN_MAJOR_XKB_VERSION,
                                    XKB_X11_MIN_MINOR_XKB_VERSION,
                                    XKB_X11_SETUP_XKB_EXTENSION_NO_FLAGS,
                                    NULL, NULL,
                                    &priv->xkb_event_base,
                                    &priv->xkb_error_base))
    meta_fatal ("X server doesn't have the XKB extension, version %d.%d or newer\n",
                XKB_X11_MIN_MAJOR_XKB_VERSION, XKB_X11_MIN_MINOR_XKB_VERSION);

  g_signal_connect_object (clutter_device_manager_get_default (), "device-added",
                           G_CALLBACK (on_device_added), backend, 0);

  META_BACKEND_CLASS (meta_backend_x11_parent_class)->post_init (backend);
}

static MetaIdleMonitor *
meta_backend_x11_create_idle_monitor (MetaBackend *backend,
                                      int          device_id)
{
  return g_object_new (META_TYPE_IDLE_MONITOR_XSYNC,
                       "device-id", device_id,
                       NULL);
}

static MetaMonitorManager *
meta_backend_x11_create_monitor_manager (MetaBackend *backend)
{
  /* If we're a Wayland compositor using the X11 backend,
   * we're a nested configuration, so return the dummy
   * monitor setup. */
  if (meta_is_wayland_compositor ())
    return g_object_new (META_TYPE_MONITOR_MANAGER_DUMMY, NULL);

  return g_object_new (META_TYPE_MONITOR_MANAGER_XRANDR, NULL);
}

static MetaCursorRenderer *
meta_backend_x11_create_cursor_renderer (MetaBackend *backend)
{
  return g_object_new (META_TYPE_CURSOR_RENDERER_X11, NULL);
}

static gboolean
meta_backend_x11_grab_device (MetaBackend *backend,
                              int          device_id,
                              uint32_t     timestamp)
{
  MetaBackendX11 *x11 = META_BACKEND_X11 (backend);
  MetaBackendX11Private *priv = meta_backend_x11_get_instance_private (x11);
  unsigned char mask_bits[XIMaskLen (XI_LASTEVENT)] = { 0 };
  XIEventMask mask = { XIAllMasterDevices, sizeof (mask_bits), mask_bits };
  int ret;

  if (timestamp != CurrentTime)
    timestamp = MAX (timestamp, priv->latest_evtime);

  XISetMask (mask.mask, XI_ButtonPress);
  XISetMask (mask.mask, XI_ButtonRelease);
  XISetMask (mask.mask, XI_Enter);
  XISetMask (mask.mask, XI_Leave);
  XISetMask (mask.mask, XI_Motion);
  XISetMask (mask.mask, XI_KeyPress);
  XISetMask (mask.mask, XI_KeyRelease);

  ret = XIGrabDevice (priv->xdisplay, device_id,
                      meta_backend_x11_get_xwindow (x11),
                      timestamp,
                      None,
                      XIGrabModeAsync, XIGrabModeAsync,
                      False, /* owner_events */
                      &mask);

  return (ret == Success);
}

static gboolean
meta_backend_x11_ungrab_device (MetaBackend *backend,
                                int          device_id,
                                uint32_t     timestamp)
{
  MetaBackendX11 *x11 = META_BACKEND_X11 (backend);
  MetaBackendX11Private *priv = meta_backend_x11_get_instance_private (x11);
  int ret;

  ret = XIUngrabDevice (priv->xdisplay, device_id, timestamp);

  return (ret == Success);
}

static void
meta_backend_x11_warp_pointer (MetaBackend *backend,
                               int          x,
                               int          y)
{
  MetaBackendX11 *x11 = META_BACKEND_X11 (backend);
  MetaBackendX11Private *priv = meta_backend_x11_get_instance_private (x11);

  XIWarpPointer (priv->xdisplay,
                 META_VIRTUAL_CORE_POINTER_ID,
                 None,
                 meta_backend_x11_get_xwindow (x11),
                 0, 0, 0, 0,
                 x, y);
}

static void
get_xkbrf_var_defs (Display           *xdisplay,
                    const char        *layouts,
                    const char        *variants,
                    const char        *options,
                    char             **rules_p,
                    XkbRF_VarDefsRec  *var_defs)
{
  char *rules = NULL;

  /* Get it from the X property or fallback on defaults */
  if (!XkbRF_GetNamesProp (xdisplay, &rules, var_defs) || !rules)
    {
      rules = strdup (DEFAULT_XKB_RULES_FILE);
      var_defs->model = strdup (DEFAULT_XKB_MODEL);
      var_defs->layout = NULL;
      var_defs->variant = NULL;
      var_defs->options = NULL;
    }

  /* Swap in our new options... */
  free (var_defs->layout);
  var_defs->layout = strdup (layouts);
  free (var_defs->variant);
  var_defs->variant = strdup (variants);
  free (var_defs->options);
  var_defs->options = strdup (options);

  /* Sometimes, the property is a file path, and sometimes it's
     not. Normalize it so it's always a file path. */
  if (rules[0] == '/')
    *rules_p = g_strdup (rules);
  else
    *rules_p = g_build_filename (XKB_BASE, "rules", rules, NULL);

  free (rules);
}

static void
free_xkbrf_var_defs (XkbRF_VarDefsRec *var_defs)
{
  free (var_defs->model);
  free (var_defs->layout);
  free (var_defs->variant);
  free (var_defs->options);
}

static void
free_xkb_component_names (XkbComponentNamesRec *p)
{
  free (p->keymap);
  free (p->keycodes);
  free (p->types);
  free (p->compat);
  free (p->symbols);
  free (p->geometry);
}

static void
upload_xkb_description (Display              *xdisplay,
                        const gchar          *rules_file_path,
                        XkbRF_VarDefsRec     *var_defs,
                        XkbComponentNamesRec *comp_names)
{
  XkbDescRec *xkb_desc;
  gchar *rules_file;

  /* Upload it to the X server using the same method as setxkbmap */
  xkb_desc = XkbGetKeyboardByName (xdisplay,
                                   XkbUseCoreKbd,
                                   comp_names,
                                   XkbGBN_AllComponentsMask,
                                   XkbGBN_AllComponentsMask &
                                   (~XkbGBN_GeometryMask), True);
  if (!xkb_desc)
    {
      g_warning ("Couldn't upload new XKB keyboard description");
      return;
    }

  XkbFreeKeyboard (xkb_desc, 0, True);

  rules_file = g_path_get_basename (rules_file_path);

  if (!XkbRF_SetNamesProp (xdisplay, rules_file, var_defs))
    g_warning ("Couldn't update the XKB root window property");

  g_free (rules_file);
}

static void
apply_keymap (MetaBackendX11 *x11)
{
  MetaBackendX11Private *priv = meta_backend_x11_get_instance_private (x11);
  XkbRF_RulesRec *xkb_rules;
  XkbRF_VarDefsRec xkb_var_defs = { 0 };
  gchar *rules_file_path;

  if (!priv->keymap_layouts ||
      !priv->keymap_variants ||
      !priv->keymap_options)
    return;

  get_xkbrf_var_defs (priv->xdisplay,
                      priv->keymap_layouts,
                      priv->keymap_variants,
                      priv->keymap_options,
                      &rules_file_path,
                      &xkb_var_defs);

  xkb_rules = XkbRF_Load (rules_file_path, NULL, True, True);
  if (xkb_rules)
    {
      XkbComponentNamesRec xkb_comp_names = { 0 };

      XkbRF_GetComponents (xkb_rules, &xkb_var_defs, &xkb_comp_names);
      upload_xkb_description (priv->xdisplay, rules_file_path, &xkb_var_defs, &xkb_comp_names);

      free_xkb_component_names (&xkb_comp_names);
      XkbRF_Free (xkb_rules, True);
    }
  else
    {
      g_warning ("Couldn't load XKB rules");
    }

  free_xkbrf_var_defs (&xkb_var_defs);
  g_free (rules_file_path);
}

static void
meta_backend_x11_set_keymap (MetaBackend *backend,
                             const char  *layouts,
                             const char  *variants,
                             const char  *options)
{
  MetaBackendX11 *x11 = META_BACKEND_X11 (backend);
  MetaBackendX11Private *priv = meta_backend_x11_get_instance_private (x11);

  g_free (priv->keymap_layouts);
  priv->keymap_layouts = g_strdup (layouts);
  g_free (priv->keymap_variants);
  priv->keymap_variants = g_strdup (variants);
  g_free (priv->keymap_options);
  priv->keymap_options = g_strdup (options);

  apply_keymap (x11);
}

static struct xkb_keymap *
meta_backend_x11_get_keymap (MetaBackend *backend)
{
  MetaBackendX11 *x11 = META_BACKEND_X11 (backend);
  MetaBackendX11Private *priv = meta_backend_x11_get_instance_private (x11);

  if (priv->keymap == NULL)
    {
      struct xkb_context *context = xkb_context_new (XKB_CONTEXT_NO_FLAGS);
      priv->keymap = xkb_x11_keymap_new_from_device (context,
                                                     priv->xcb,
                                                     xkb_x11_get_core_keyboard_device_id (priv->xcb),
                                                     XKB_KEYMAP_COMPILE_NO_FLAGS);
      xkb_context_unref (context);
    }

  return priv->keymap;
}

static void
meta_backend_x11_lock_layout_group (MetaBackend *backend,
                                    guint        idx)
{
  MetaBackendX11 *x11 = META_BACKEND_X11 (backend);
  MetaBackendX11Private *priv = meta_backend_x11_get_instance_private (x11);

  XkbLockGroup (priv->xdisplay, XkbUseCoreKbd, idx);
}

static void
meta_backend_x11_update_screen_size (MetaBackend *backend,
                                     int width, int height)
{
  if (meta_is_wayland_compositor ())
    {
      /* For a nested wayland session, we want to go through Clutter to update the
       * toplevel window size, rather than doing it directly.
       */
      META_BACKEND_CLASS (meta_backend_x11_parent_class)->update_screen_size (backend, width, height);
    }
  else
    {
      MetaBackendX11 *x11 = META_BACKEND_X11 (backend);
      MetaBackendX11Private *priv = meta_backend_x11_get_instance_private (x11);
      Window xwin = meta_backend_x11_get_xwindow (x11);
      XResizeWindow (priv->xdisplay, xwin, width, height);
    }
}

static void
meta_backend_x11_select_stage_events (MetaBackend *backend)
{
  MetaBackendX11 *x11 = META_BACKEND_X11 (backend);
  MetaBackendX11Private *priv = meta_backend_x11_get_instance_private (x11);
  Window xwin = meta_backend_x11_get_xwindow (x11);
  unsigned char mask_bits[XIMaskLen (XI_LASTEVENT)] = { 0 };
  XIEventMask mask = { XIAllMasterDevices, sizeof (mask_bits), mask_bits };

  XISetMask (mask.mask, XI_KeyPress);
  XISetMask (mask.mask, XI_KeyRelease);
  XISetMask (mask.mask, XI_ButtonPress);
  XISetMask (mask.mask, XI_ButtonRelease);
  XISetMask (mask.mask, XI_Enter);
  XISetMask (mask.mask, XI_Leave);
  XISetMask (mask.mask, XI_FocusIn);
  XISetMask (mask.mask, XI_FocusOut);
  XISetMask (mask.mask, XI_Motion);
  XIClearMask (mask.mask, XI_TouchBegin);
  XIClearMask (mask.mask, XI_TouchEnd);
  XIClearMask (mask.mask, XI_TouchUpdate);
  XISelectEvents (priv->xdisplay, xwin, &mask, 1);
}

static void
meta_backend_x11_class_init (MetaBackendX11Class *klass)
{
  MetaBackendClass *backend_class = META_BACKEND_CLASS (klass);

  backend_class->post_init = meta_backend_x11_post_init;
  backend_class->create_idle_monitor = meta_backend_x11_create_idle_monitor;
  backend_class->create_monitor_manager = meta_backend_x11_create_monitor_manager;
  backend_class->create_cursor_renderer = meta_backend_x11_create_cursor_renderer;
  backend_class->grab_device = meta_backend_x11_grab_device;
  backend_class->ungrab_device = meta_backend_x11_ungrab_device;
  backend_class->warp_pointer = meta_backend_x11_warp_pointer;
  backend_class->set_keymap = meta_backend_x11_set_keymap;
  backend_class->get_keymap = meta_backend_x11_get_keymap;
  backend_class->lock_layout_group = meta_backend_x11_lock_layout_group;
  backend_class->update_screen_size = meta_backend_x11_update_screen_size;
  backend_class->select_stage_events = meta_backend_x11_select_stage_events;
}

static void
meta_backend_x11_init (MetaBackendX11 *x11)
{
  /* We do X11 event retrieval ourselves */
  clutter_x11_disable_event_retrieval ();
}

Display *
meta_backend_x11_get_xdisplay (MetaBackendX11 *x11)
{
  MetaBackendX11Private *priv = meta_backend_x11_get_instance_private (x11);

  return priv->xdisplay;
}

Window
meta_backend_x11_get_xwindow (MetaBackendX11 *x11)
{
  ClutterActor *stage = meta_backend_get_stage (META_BACKEND (x11));
  return clutter_x11_get_stage_window (CLUTTER_STAGE (stage));
}
