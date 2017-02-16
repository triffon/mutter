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

#include "tests/monitor-store-unit-tests.h"

#include "backends/meta-backend-private.h"
#include "backends/meta-monitor-config-store.h"
#include "backends/meta-monitor-config-manager.h"
#include "backends/meta-monitor-manager-private.h"
#include "tests/monitor-test-utils.h"

#define MAX_N_MONITORS 10
#define MAX_N_LOGICAL_MONITORS 10
#define MAX_N_CONFIGURATIONS 10

typedef struct _MonitorTestCaseMonitorMode
{
  int width;
  int height;
  float refresh_rate;
} MonitorTestCaseMonitorMode;

typedef struct _MonitorTestCaseMonitor
{
  const char *connector;
  const char *vendor;
  const char *product;
  const char *serial;
  MonitorTestCaseMonitorMode mode;
  gboolean is_underscanning;
} MonitorTestCaseMonitor;

typedef struct _MonitorTestCaseLogicalMonitor
{
  MetaRectangle layout;
  gboolean is_primary;
  gboolean is_presentation;
  MonitorTestCaseMonitor monitors[MAX_N_MONITORS];
  int n_monitors;
} MonitorTestCaseLogicalMonitor;

typedef struct _MonitorStoreTestConfiguration
{
  MonitorTestCaseLogicalMonitor logical_monitors[MAX_N_LOGICAL_MONITORS];
  int n_logical_monitors;
} MonitorStoreTestConfiguration;

typedef struct _MonitorStoreTestExpect
{
  MonitorStoreTestConfiguration configurations[MAX_N_CONFIGURATIONS];
  int n_configurations;
} MonitorStoreTestExpect;

static MetaMonitorsConfigKey *
create_config_key_from_expect (MonitorStoreTestConfiguration *expect_config)
{
  MetaMonitorsConfigKey *config_key;
  GList *monitor_specs;
  int i;

  monitor_specs = NULL;
  for (i = 0; i < expect_config->n_logical_monitors; i++)
    {
      int j;

      for (j = 0; j < expect_config->logical_monitors[i].n_monitors; j++)
        {
          MetaMonitorSpec *monitor_spec;
          MonitorTestCaseMonitor *test_monitor =
            &expect_config->logical_monitors[i].monitors[j];

          monitor_spec = g_new0 (MetaMonitorSpec, 1);

          monitor_spec->connector = g_strdup (test_monitor->connector);
          monitor_spec->vendor = g_strdup (test_monitor->vendor);
          monitor_spec->product = g_strdup (test_monitor->product);
          monitor_spec->serial = g_strdup (test_monitor->serial);

          monitor_specs = g_list_prepend (monitor_specs, monitor_spec);
        }
    }

  g_assert_nonnull (monitor_specs);

  monitor_specs = g_list_sort (monitor_specs,
                               (GCompareFunc) meta_monitor_spec_compare);

  config_key = g_new0 (MetaMonitorsConfigKey, 1);
  *config_key = (MetaMonitorsConfigKey) {
    .monitor_specs = monitor_specs
  };

  return config_key;
}

static void
check_monitor_configuration (MetaMonitorConfigStore        *config_store,
                             MonitorStoreTestConfiguration *config_expect)
{
  MetaMonitorsConfigKey *config_key;
  MetaMonitorsConfig *config;
  GList *l;
  int i;

  config_key = create_config_key_from_expect (config_expect);
  config = meta_monitor_config_store_lookup (config_store, config_key);
  g_assert_nonnull (config);

  g_assert (meta_monitors_config_key_equal (config->key, config_key));
  meta_monitors_config_key_free (config_key);

  g_assert_cmpuint (g_list_length (config->logical_monitor_configs),
                    ==,
                    config_expect->n_logical_monitors);

  for (l = config->logical_monitor_configs, i = 0; l; l = l->next, i++)
    {
      MetaLogicalMonitorConfig *logical_monitor_config = l->data;
      GList *k;
      int j;

      g_assert (meta_rectangle_equal (&logical_monitor_config->layout,
                                      &config_expect->logical_monitors[i].layout));
      g_assert_cmpint (logical_monitor_config->is_primary,
                       ==,
                       config_expect->logical_monitors[i].is_primary);
      g_assert_cmpint (logical_monitor_config->is_presentation,
                       ==,
                       config_expect->logical_monitors[i].is_presentation);

      g_assert_cmpint ((int) g_list_length (logical_monitor_config->monitor_configs),
                       ==,
                       config_expect->logical_monitors[i].n_monitors);

      for (k = logical_monitor_config->monitor_configs, j = 0;
           k;
           k = k->next, j++)
        {
          MetaMonitorConfig *monitor_config = k->data;
          MonitorTestCaseMonitor *test_monitor =
            &config_expect->logical_monitors[i].monitors[j];

          g_assert_cmpstr (monitor_config->monitor_spec->connector,
                           ==,
                           test_monitor->connector);
          g_assert_cmpstr (monitor_config->monitor_spec->vendor,
                           ==,
                           test_monitor->vendor);
          g_assert_cmpstr (monitor_config->monitor_spec->product,
                           ==,
                           test_monitor->product);
          g_assert_cmpstr (monitor_config->monitor_spec->serial,
                           ==,
                           test_monitor->serial);

          g_assert_cmpint (monitor_config->mode_spec->width,
                           ==,
                           test_monitor->mode.width);
          g_assert_cmpint (monitor_config->mode_spec->height,
                           ==,
                           test_monitor->mode.height);
          g_assert_cmpfloat (monitor_config->mode_spec->refresh_rate,
                             ==,
                             test_monitor->mode.refresh_rate);
          g_assert_cmpint (monitor_config->is_underscanning,
                           ==,
                           test_monitor->is_underscanning);
        }
    }
}

static void
check_monitor_configurations (MonitorStoreTestExpect *expect)
{
  MetaBackend *backend = meta_get_backend ();
  MetaMonitorManager *monitor_manager =
    meta_backend_get_monitor_manager (backend);
  MetaMonitorConfigManager *config_manager = monitor_manager->config_manager;
  MetaMonitorConfigStore *config_store =
    meta_monitor_config_manager_get_store (config_manager);
  int i;

  g_assert_cmpint (meta_monitor_config_store_get_config_count (config_store),
                   ==,
                   expect->n_configurations);

  for (i = 0; i < expect->n_configurations; i++)
    check_monitor_configuration (config_store, &expect->configurations[i]);
}

static void
meta_test_monitor_store_single (void)
{
  MonitorStoreTestExpect expect = {
    .configurations = {
      {
        .logical_monitors = {
          {
            .layout = {
              .x = 0,
              .y = 0,
              .width = 1920,
              .height = 1080
            },
            .is_primary = TRUE,
            .is_presentation = FALSE,
            .monitors = {
              {
                .connector = "DP-1",
                .vendor = "MetaProduct's Inc.",
                .product = "MetaMonitor",
                .serial = "0x123456",
                .mode = {
                  .width = 1920,
                  .height = 1080,
                  .refresh_rate = 60.000495910644531
                }
              }
            },
            .n_monitors = 1,
          }
        },
        .n_logical_monitors = 1
      }
    },
    .n_configurations = 1
  };

  if (!is_using_monitor_config_manager ())
    {
      g_test_skip ("Not using MetaMonitorConfigManager");
      return;
    }

  set_custom_monitor_config ("single.xml");

  check_monitor_configurations (&expect);
}

static void
meta_test_monitor_store_vertical (void)
{
  MonitorStoreTestExpect expect = {
    .configurations = {
      {
        .logical_monitors = {
          {
            .layout = {
              .x = 0,
              .y = 0,
              .width = 1024,
              .height = 768
            },
            .is_primary = TRUE,
            .is_presentation = FALSE,
            .monitors = {
              {
                .connector = "DP-1",
                .vendor = "MetaProduct's Inc.",
                .product = "MetaMonitor",
                .serial = "0x123456",
                .mode = {
                  .width = 1024,
                  .height = 768,
                  .refresh_rate = 60.000495910644531
                }
              }
            },
            .n_monitors = 1,
          },
          {
            .layout = {
              .x = 0,
              .y = 768,
              .width = 800,
              .height = 600
            },
            .is_primary = FALSE,
            .is_presentation = FALSE,
            .monitors = {
              {
                .connector = "DP-2",
                .vendor = "MetaProduct's Inc.",
                .product = "MetaMonitor",
                .serial = "0x123456",
                .mode = {
                  .width = 800,
                  .height = 600,
                  .refresh_rate = 60.000495910644531
                }
              }
            },
            .n_monitors = 1,
          }
        },
        .n_logical_monitors = 2
      }
    },
    .n_configurations = 1
  };

  if (!is_using_monitor_config_manager ())
    {
      g_test_skip ("Not using MetaMonitorConfigManager");
      return;
    }

  set_custom_monitor_config ("vertical.xml");

  check_monitor_configurations (&expect);
}

static void
meta_test_monitor_store_primary (void)
{
  MonitorStoreTestExpect expect = {
    .configurations = {
      {
        .logical_monitors = {
          {
            .layout = {
              .x = 0,
              .y = 0,
              .width = 1024,
              .height = 768
            },
            .is_primary = FALSE,
            .is_presentation = FALSE,
            .monitors = {
              {
                .connector = "DP-1",
                .vendor = "MetaProduct's Inc.",
                .product = "MetaMonitor",
                .serial = "0x123456",
                .mode = {
                  .width = 1024,
                  .height = 768,
                  .refresh_rate = 60.000495910644531
                }
              }
            },
            .n_monitors = 1,
          },
          {
            .layout = {
              .x = 1024,
              .y = 0,
              .width = 800,
              .height = 600
            },
            .is_primary = TRUE,
            .is_presentation = FALSE,
            .monitors = {
              {
                .connector = "DP-2",
                .vendor = "MetaProduct's Inc.",
                .product = "MetaMonitor",
                .serial = "0x123456",
                .mode = {
                  .width = 800,
                  .height = 600,
                  .refresh_rate = 60.000495910644531
                }
              }
            },
            .n_monitors = 1,
          }
        },
        .n_logical_monitors = 2
      }
    },
    .n_configurations = 1
  };

  if (!is_using_monitor_config_manager ())
    {
      g_test_skip ("Not using MetaMonitorConfigManager");
      return;
    }

  set_custom_monitor_config ("primary.xml");

  check_monitor_configurations (&expect);
}

static void
meta_test_monitor_store_underscanning (void)
{
  MonitorStoreTestExpect expect = {
    .configurations = {
      {
        .logical_monitors = {
          {
            .layout = {
              .x = 0,
              .y = 0,
              .width = 1024,
              .height = 768
            },
            .is_primary = TRUE,
            .is_presentation = FALSE,
            .monitors = {
              {
                .connector = "DP-1",
                .vendor = "MetaProduct's Inc.",
                .product = "MetaMonitor",
                .serial = "0x123456",
                .is_underscanning = TRUE,
                .mode = {
                  .width = 1024,
                  .height = 768,
                  .refresh_rate = 60.000495910644531
                }
              }
            },
            .n_monitors = 1,
          },
        },
        .n_logical_monitors = 1
      }
    },
    .n_configurations = 1
  };

  if (!is_using_monitor_config_manager ())
    {
      g_test_skip ("Not using MetaMonitorConfigManager");
      return;
    }

  set_custom_monitor_config ("underscanning.xml");

  check_monitor_configurations (&expect);
}

void
init_monitor_store_tests (void)
{
  g_test_add_func ("/backends/monitor-store/single",
                   meta_test_monitor_store_single);
  g_test_add_func ("/backends/monitor-store/vertical",
                   meta_test_monitor_store_vertical);
  g_test_add_func ("/backends/monitor-store/primary",
                   meta_test_monitor_store_primary);
  g_test_add_func ("/backends/monitor-store/underscanning",
                   meta_test_monitor_store_underscanning);
}
