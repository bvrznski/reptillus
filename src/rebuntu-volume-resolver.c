#include "rebuntu-volume-resolver.h"

GFile *
rebuntu_volume_resolver_get_mount_root (GFile *computer_file)
{
    g_return_val_if_fail (G_IS_FILE (computer_file), NULL);

    g_autoptr (GError) error = NULL;

    g_autoptr (GFileInfo) info =
        g_file_query_info (
            computer_file,
            G_FILE_ATTRIBUTE_STANDARD_TARGET_URI,
            G_FILE_QUERY_INFO_NONE,
            NULL,
            &error
        );

    if (info == NULL)
        return NULL;

    const char *target_uri =
        g_file_info_get_attribute_string (
            info,
            G_FILE_ATTRIBUTE_STANDARD_TARGET_URI
        );

    if (target_uri == NULL)
        return NULL;

    g_autoptr (GFile) target = g_file_new_for_uri (target_uri);

    if (!g_file_is_native (target))
        return NULL;

    return g_steal_pointer (&target);
}
