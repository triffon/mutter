/* -*- mode: C; c-file-style: "gnu"; indent-tabs-mode: nil; -*- */

/*
 * Copyright (C) 2016 Red Hat, Inc.
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

#include "config.h"

#include "tests/meta-monitor-manager-test.h"

#include "backends/meta-monitor-config-manager.h"

struct _MetaMonitorManagerTest
{
  MetaMonitorManager parent;

  gboolean is_lid_closed;

  int tiled_monitor_count;

  MetaMonitorTestSetup *test_setup;
};

G_DEFINE_TYPE (MetaMonitorManagerTest, meta_monitor_manager_test,
               META_TYPE_MONITOR_MANAGER)

static MetaMonitorTestSetup *_initial_test_setup = NULL;

void
meta_monitor_manager_test_init_test_setup (MetaMonitorTestSetup *test_setup)
{
  _initial_test_setup = test_setup;
}

void
meta_monitor_manager_test_emulate_hotplug (MetaMonitorManagerTest *manager_test,
                                           MetaMonitorTestSetup   *test_setup)
{
  MetaMonitorManager *manager = META_MONITOR_MANAGER (manager_test);
  MetaMonitorTestSetup *old_test_setup;

  old_test_setup = manager_test->test_setup;
  manager_test->test_setup = test_setup;

  meta_monitor_manager_read_current_state (manager);
  meta_monitor_manager_on_hotplug (manager);

  g_free (old_test_setup);
}

void
meta_monitor_manager_test_set_is_lid_closed (MetaMonitorManagerTest *manager_test,
                                             gboolean                is_lid_closed)
{
  manager_test->is_lid_closed = is_lid_closed;
}

int
meta_monitor_manager_test_get_tiled_monitor_count (MetaMonitorManagerTest *manager_test)
{
  return manager_test->tiled_monitor_count;
}

static void
meta_monitor_manager_test_read_current (MetaMonitorManager *manager)
{
  MetaMonitorManagerTest *manager_test = META_MONITOR_MANAGER_TEST (manager);

  manager->max_screen_width = 65535;
  manager->max_screen_height = 65535;

  g_assert (manager_test->test_setup);

  manager->modes = manager_test->test_setup->modes;
  manager->n_modes = manager_test->test_setup->n_modes;

  manager->crtcs = manager_test->test_setup->crtcs;
  manager->n_crtcs = manager_test->test_setup->n_crtcs;

  manager->outputs = manager_test->test_setup->outputs;
  manager->n_outputs = manager_test->test_setup->n_outputs;
}

static gboolean
meta_monitor_manager_test_is_lid_closed (MetaMonitorManager *manager)
{
  MetaMonitorManagerTest *manager_test = META_MONITOR_MANAGER_TEST (manager);

  return manager_test->is_lid_closed;
}

static void
meta_monitor_manager_test_ensure_initial_config (MetaMonitorManager *manager)
{
  MetaMonitorsConfig *config;

  config = meta_monitor_manager_ensure_configured (manager);

  if (manager->config_manager)
    meta_monitor_manager_update_logical_state (manager, config);
  else
    meta_monitor_manager_update_logical_state_derived (manager);
}

static void
apply_crtc_assignments (MetaMonitorManager *manager,
                        MetaCrtcInfo      **crtcs,
                        unsigned int        n_crtcs,
                        MetaOutputInfo    **outputs,
                        unsigned int        n_outputs)
{
  unsigned int i;

  for (i = 0; i < n_crtcs; i++)
    {
      MetaCrtcInfo *crtc_info = crtcs[i];
      MetaCrtc *crtc = crtc_info->crtc;
      crtc->is_dirty = TRUE;

      if (crtc_info->mode == NULL)
        {
          crtc->rect.x = 0;
          crtc->rect.y = 0;
          crtc->rect.width = 0;
          crtc->rect.height = 0;
          crtc->current_mode = NULL;
        }
      else
        {
          MetaCrtcMode *mode;
          MetaOutput *output;
          unsigned int j;
          int width, height;

          mode = crtc_info->mode;

          if (meta_monitor_transform_is_rotated (crtc_info->transform))
            {
              width = mode->height;
              height = mode->width;
            }
          else
            {
              width = mode->width;
              height = mode->height;
            }

          crtc->rect.x = crtc_info->x;
          crtc->rect.y = crtc_info->y;
          crtc->rect.width = width;
          crtc->rect.height = height;
          crtc->current_mode = mode;
          crtc->transform = crtc_info->transform;

          for (j = 0; j < crtc_info->outputs->len; j++)
            {
              output = ((MetaOutput**)crtc_info->outputs->pdata)[j];

              output->is_dirty = TRUE;
              output->crtc = crtc;
            }
        }
    }

  for (i = 0; i < n_outputs; i++)
    {
      MetaOutputInfo *output_info = outputs[i];
      MetaOutput *output = output_info->output;

      output->is_primary = output_info->is_primary;
      output->is_presentation = output_info->is_presentation;
      output->is_underscanning = output_info->is_underscanning;
    }

  /* Disable CRTCs not mentioned in the list */
  for (i = 0; i < manager->n_crtcs; i++)
    {
      MetaCrtc *crtc = &manager->crtcs[i];

      crtc->logical_monitor = NULL;

      if (crtc->is_dirty)
        {
          crtc->is_dirty = FALSE;
          continue;
        }

      crtc->rect.x = 0;
      crtc->rect.y = 0;
      crtc->rect.width = 0;
      crtc->rect.height = 0;
      crtc->current_mode = NULL;
    }

  /* Disable outputs not mentioned in the list */
  for (i = 0; i < manager->n_outputs; i++)
    {
      MetaOutput *output = &manager->outputs[i];

      if (output->is_dirty)
        {
          output->is_dirty = FALSE;
          continue;
        }

      output->crtc = NULL;
      output->is_primary = FALSE;
    }
}

static void
update_screen_size (MetaMonitorManager *manager,
                    MetaMonitorsConfig *config)
{
  GList *l;
  int screen_width = 0;
  int screen_height = 0;

  for (l = config->logical_monitor_configs; l; l = l->next)
    {
      MetaLogicalMonitorConfig *logical_monitor_config = l->data;
      int right_edge;
      int bottom_edge;

      right_edge = (logical_monitor_config->layout.width +
                    logical_monitor_config->layout.x);
      if (right_edge > screen_width)
        screen_width = right_edge;

      bottom_edge = (logical_monitor_config->layout.height +
                     logical_monitor_config->layout.y);
      if (bottom_edge > screen_height)
        screen_height = bottom_edge;
    }

  manager->screen_width = screen_width;
  manager->screen_height = screen_height;
}

static gboolean
meta_monitor_manager_test_apply_monitors_config (MetaMonitorManager *manager,
                                                 MetaMonitorsConfig *config,
                                                 GError            **error)
{
  GPtrArray *crtc_infos;
  GPtrArray *output_infos;

  if (!meta_monitor_config_manager_assign (manager, config,
                                           &crtc_infos,
                                           &output_infos,
                                           error))
    return FALSE;

  apply_crtc_assignments (manager,
                          (MetaCrtcInfo **) crtc_infos->pdata,
                          crtc_infos->len,
                          (MetaOutputInfo **) output_infos->pdata,
                          output_infos->len);

  g_ptr_array_free (crtc_infos, TRUE);
  g_ptr_array_free (output_infos, TRUE);

  update_screen_size (manager, config);
  meta_monitor_manager_rebuild (manager, config);

  return TRUE;
}

static void
legacy_calculate_screen_size (MetaMonitorManager *manager)
{
  unsigned int i;
  int width = 0, height = 0;

  for (i = 0; i < manager->n_crtcs; i++)
    {
      MetaCrtc *crtc = &manager->crtcs[i];

      width = MAX (width, crtc->rect.x + crtc->rect.width);
      height = MAX (height, crtc->rect.y + crtc->rect.height);
    }

  manager->screen_width = width;
  manager->screen_height = height;
}

static void
meta_monitor_manager_test_apply_configuration (MetaMonitorManager *manager,
                                               MetaCrtcInfo      **crtcs,
                                               unsigned int        n_crtcs,
                                               MetaOutputInfo    **outputs,
                                               unsigned int        n_outputs)
{
  apply_crtc_assignments (manager, crtcs, n_crtcs, outputs, n_outputs);
  legacy_calculate_screen_size (manager);
  meta_monitor_manager_rebuild_derived (manager);
}

static void
meta_monitor_manager_test_tiled_monitor_added (MetaMonitorManager *manager,
                                               MetaMonitor        *monitor)
{
  MetaMonitorManagerTest *manager_test = META_MONITOR_MANAGER_TEST (manager);

  manager_test->tiled_monitor_count++;
}

static void
meta_monitor_manager_test_tiled_monitor_removed (MetaMonitorManager *manager,
                                                 MetaMonitor        *monitor)
{
  MetaMonitorManagerTest *manager_test = META_MONITOR_MANAGER_TEST (manager);

  manager_test->tiled_monitor_count--;
}

static void
meta_monitor_manager_test_dispose (GObject *object)
{
  MetaMonitorManagerTest *manager_test = META_MONITOR_MANAGER_TEST (object);

  g_clear_pointer (&manager_test->test_setup, g_free);
}

static void
meta_monitor_manager_test_init (MetaMonitorManagerTest *manager_test)
{
  g_assert (_initial_test_setup);

  manager_test->test_setup = _initial_test_setup;
}

static void
meta_monitor_manager_test_class_init (MetaMonitorManagerTestClass *klass)
{
  GObjectClass *object_class = G_OBJECT_CLASS (klass);
  MetaMonitorManagerClass *manager_class = META_MONITOR_MANAGER_CLASS (klass);

  object_class->dispose = meta_monitor_manager_test_dispose;

  manager_class->read_current = meta_monitor_manager_test_read_current;
  manager_class->is_lid_closed = meta_monitor_manager_test_is_lid_closed;
  manager_class->ensure_initial_config = meta_monitor_manager_test_ensure_initial_config;
  manager_class->apply_monitors_config = meta_monitor_manager_test_apply_monitors_config;
  manager_class->apply_configuration = meta_monitor_manager_test_apply_configuration;
  manager_class->tiled_monitor_added = meta_monitor_manager_test_tiled_monitor_added;
  manager_class->tiled_monitor_removed = meta_monitor_manager_test_tiled_monitor_removed;
}
