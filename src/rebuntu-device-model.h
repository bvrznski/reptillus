#pragma once

#include <gio/gio.h>

G_BEGIN_DECLS

typedef struct
{
    char *object_path;
    char *device;
    char *drive_path;
    char *partition_table;
    char *filesystem_type;
    char *label;
    char *mount_point;

    guint64 size;
    guint64 filesystem_total;
    guint64 filesystem_free;
    gboolean filesystem_usage_valid;
    guint32 partition_number;

    gboolean is_partition;
    gboolean has_filesystem;
    gboolean is_mounted;
} RebuntuBlockDevice;

typedef struct _RebuntuDeviceModel RebuntuDeviceModel;

RebuntuDeviceModel *rebuntu_device_model_new (void);

gboolean rebuntu_device_model_refresh (
    RebuntuDeviceModel *model,
    GError **error);

const GPtrArray *rebuntu_device_model_get_devices (
    RebuntuDeviceModel *model);

void rebuntu_device_model_free (
    RebuntuDeviceModel *model);

G_END_DECLS
