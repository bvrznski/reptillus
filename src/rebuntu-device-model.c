/*
 * Rebuntu Device Model
 * Read-only UDisks2 inventory.
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include "rebuntu-device-model.h"
#include <sys/statvfs.h>

struct _RebuntuDeviceModel
{
    GDBusConnection *connection;
    GPtrArray *devices;
};

static void
block_device_free (gpointer data)
{
    RebuntuBlockDevice *device = data;

    if (device == NULL)
        return;

    g_free (device->object_path);
    g_free (device->device);
    g_free (device->drive_path);
    g_free (device->partition_table);
    g_free (device->filesystem_type);
    g_free (device->label);
    g_free (device->mount_point);
    g_free (device);
}

static char *
lookup_string (GVariant *properties,
               const char *key)
{
    const char *value = NULL;

    if (g_variant_lookup (properties, key, "&s", &value))
        return g_strdup (value);

    return NULL;
}

static char *
lookup_object_path (GVariant *properties,
                    const char *key)
{
    g_autoptr (GVariant) value =
        g_variant_lookup_value (
            properties,
            key,
            G_VARIANT_TYPE_OBJECT_PATH
        );

    if (value == NULL)
        return NULL;

    return g_variant_dup_string (value, NULL);
}

static char *
lookup_bytestring (GVariant *properties,
                   const char *key)
{
    g_autoptr (GVariant) value =
        g_variant_lookup_value (
            properties,
            key,
            G_VARIANT_TYPE_BYTESTRING
        );

    if (value == NULL)
        return NULL;

    return g_strdup (g_variant_get_bytestring (value));
}

static char *
lookup_mount_point (GVariant *properties)
{
    g_autoptr (GVariant) mounts =
        g_variant_lookup_value (
            properties,
            "MountPoints",
            G_VARIANT_TYPE ("aay")
        );

    if (mounts == NULL || g_variant_n_children (mounts) == 0)
        return NULL;

    g_autoptr (GVariant) first =
        g_variant_get_child_value (mounts, 0);

    return g_strdup (g_variant_get_bytestring (first));
}

static void
parse_object (RebuntuDeviceModel *model,
              const char *object_path,
              GVariant *interfaces)
{
    g_autoptr (GVariant) block =
        g_variant_lookup_value (
            interfaces,
            "org.freedesktop.UDisks2.Block",
            G_VARIANT_TYPE ("a{sv}")
        );

    if (block == NULL)
        return;

    RebuntuBlockDevice *device = g_new0 (RebuntuBlockDevice, 1);

    device->object_path = g_strdup (object_path);
    device->device = lookup_bytestring (block, "Device");
    device->drive_path = lookup_object_path (block, "Drive");
    device->filesystem_type = lookup_string (block, "IdType");
    device->label = lookup_string (block, "IdLabel");

    g_variant_lookup (block, "Size", "t", &device->size);

    g_autoptr (GVariant) partition =
        g_variant_lookup_value (
            interfaces,
            "org.freedesktop.UDisks2.Partition",
            G_VARIANT_TYPE ("a{sv}")
        );

    if (partition != NULL)
    {
        device->is_partition = TRUE;
        device->partition_table =
            lookup_object_path (partition, "Table");

        g_variant_lookup (
            partition,
            "Number",
            "u",
            &device->partition_number
        );
    }

    g_autoptr (GVariant) filesystem =
        g_variant_lookup_value (
            interfaces,
            "org.freedesktop.UDisks2.Filesystem",
            G_VARIANT_TYPE ("a{sv}")
        );

    if (filesystem != NULL)
    {
        device->has_filesystem = TRUE;
        device->mount_point = lookup_mount_point (filesystem);
        device->is_mounted = device->mount_point != NULL;
    }

    g_ptr_array_add (model->devices, device);
}


static gboolean
is_loop_device (const RebuntuBlockDevice *device)
{
    return device->device != NULL &&
           g_str_has_prefix (device->device, "/dev/loop");
}

static const char *
device_group (const RebuntuBlockDevice *device)
{
    if (device->is_partition &&
        device->partition_table != NULL)
    {
        return device->partition_table;
    }

    return device->object_path != NULL
        ? device->object_path
        : "";
}

static gint
compare_devices (gconstpointer a,
                 gconstpointer b)
{
    const RebuntuBlockDevice *left =
        *(RebuntuBlockDevice * const *) a;

    const RebuntuBlockDevice *right =
        *(RebuntuBlockDevice * const *) b;

    const char *left_group = device_group (left);
    const char *right_group = device_group (right);

    gint result = g_strcmp0 (left_group, right_group);

    if (result != 0)
        return result;

    if (left->is_partition != right->is_partition)
        return left->is_partition ? 1 : -1;

    if (left->is_partition && right->is_partition)
    {
        if (left->partition_number < right->partition_number)
            return -1;

        if (left->partition_number > right->partition_number)
            return 1;
    }

    return g_strcmp0 (left->device, right->device);
}

static void
normalize_devices (RebuntuDeviceModel *model)
{
    for (gint i = (gint) model->devices->len - 1;
         i >= 0;
         i--)
    {
        RebuntuBlockDevice *device =
            g_ptr_array_index (model->devices, i);

        if (is_loop_device (device))
            g_ptr_array_remove_index (model->devices, i);
    }

    g_ptr_array_sort (model->devices, compare_devices);

    /* Filesystem accounting is intentionally performed in the GTask worker. */
    for (guint i = 0; i < model->devices->len; i++)
    {
        RebuntuBlockDevice *device = g_ptr_array_index (model->devices, i);
        struct statvfs fs;
        if (!device->is_mounted || device->mount_point == NULL ||
            statvfs (device->mount_point, &fs) != 0 || fs.f_blocks == 0)
            continue;
        device->filesystem_total = (guint64) fs.f_blocks * fs.f_frsize;
        device->filesystem_free = (guint64) fs.f_bavail * fs.f_frsize;
        device->filesystem_usage_valid = TRUE;
    }
}


RebuntuDeviceModel *
rebuntu_device_model_new (void)
{
    RebuntuDeviceModel *model = g_new0 (RebuntuDeviceModel, 1);

    model->devices =
        g_ptr_array_new_with_free_func (block_device_free);

    return model;
}

gboolean
rebuntu_device_model_refresh (RebuntuDeviceModel *model,
                              GError **error)
{
    g_return_val_if_fail (model != NULL, FALSE);

    if (model->connection == NULL)
    {
        model->connection =
            g_bus_get_sync (G_BUS_TYPE_SYSTEM, NULL, error);

        if (model->connection == NULL)
            return FALSE;
    }

    g_autoptr (GVariant) result =
        g_dbus_connection_call_sync (
            model->connection,
            "org.freedesktop.UDisks2",
            "/org/freedesktop/UDisks2",
            "org.freedesktop.DBus.ObjectManager",
            "GetManagedObjects",
            NULL,
            G_VARIANT_TYPE ("(a{oa{sa{sv}}})"),
            G_DBUS_CALL_FLAGS_NONE,
            10000,
            NULL,
            error
        );

    if (result == NULL)
        return FALSE;

    g_autoptr (GVariant) objects =
        g_variant_get_child_value (result, 0);

    g_autoptr (GPtrArray) fresh =
        g_ptr_array_new_with_free_func (block_device_free);

    GPtrArray *previous = model->devices;
    model->devices = fresh;

    GVariantIter iterator;
    const char *path;
    GVariant *interfaces;

    g_variant_iter_init (&iterator, objects);

    while (g_variant_iter_next (
               &iterator,
               "{&o@a{sa{sv}}}",
               &path,
               &interfaces))
    {
        parse_object (model, path, interfaces);
        g_variant_unref (interfaces);
    }

    normalize_devices (model);

    normalize_devices (model);

    normalize_devices (model);

    g_ptr_array_unref (previous);
    g_steal_pointer (&fresh);

    return TRUE;
}

const GPtrArray *
rebuntu_device_model_get_devices (RebuntuDeviceModel *model)
{
    g_return_val_if_fail (model != NULL, NULL);

    return model->devices;
}

void
rebuntu_device_model_free (RebuntuDeviceModel *model)
{
    if (model == NULL)
        return;

    g_clear_object (&model->connection);
    g_clear_pointer (&model->devices, g_ptr_array_unref);
    g_free (model);
}
