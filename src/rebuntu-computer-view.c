/*
 * Copyright (C) 2024 The GNOME project contributors
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include "rebuntu-computer-view.h"

#include <glib/gi18n.h>

#include "nautilus-file.h"
#include "nautilus-global-preferences.h"
#include "nautilus-list-base-private.h"
#include "rebuntu-device-cell.h"
#include "rebuntu-device-model.h"
#include "nautilus-scheme.h"
#include "nautilus-view-item.h"
#include "nautilus-view-model.h"

struct _RebuntuComputerView
{
    NautilusListBase parent_instance;

    GtkListView *view_ui;
    GtkWidget *device_panel;
    GtkWidget *gvfs_section;
    RebuntuDeviceModel *device_model;
    GVolumeMonitor *volume_monitor;
    GCancellable *refresh_cancel;
    gboolean disposing;
    GtkWidget *remote_panel;
};

G_DEFINE_TYPE (RebuntuComputerView, rebuntu_computer_view, NAUTILUS_TYPE_LIST_BASE)

#define get_view_item(li) \
        (NAUTILUS_VIEW_ITEM (gtk_tree_list_row_get_item (GTK_TREE_LIST_ROW (gtk_list_item_get_item (li)))))

static const NautilusViewInfo computer_view_info =
{
    .view_id = NAUTILUS_VIEW_COMPUTER_ID,
    .zoom_level_min = NAUTILUS_LIST_ZOOM_LEVEL_SMALL,
    .zoom_level_max = NAUTILUS_LIST_ZOOM_LEVEL_SMALL,
    .zoom_level_standard = NAUTILUS_LIST_ZOOM_LEVEL_SMALL,
};

static NautilusViewInfo
real_get_view_info (NautilusListBase *list_base)
{
    return computer_view_info;
}

static gint
sort_computer_items (gconstpointer a, gconstpointer b, gpointer user_data)
{
    NautilusViewItem *item_a = NAUTILUS_VIEW_ITEM ((gpointer) a);
    NautilusViewItem *item_b = NAUTILUS_VIEW_ITEM ((gpointer) b);
    return nautilus_file_compare_for_sort (nautilus_view_item_get_file (item_a),
                                           nautilus_view_item_get_file (item_b),
                                           NAUTILUS_FILE_SORT_BY_DISPLAY_NAME,
                                           FALSE, FALSE);
}

static void
real_set_zoom_level (NautilusListBase *list_base,
                     int               new_level)
{
    g_warn_if_fail (new_level == computer_view_info.zoom_level_standard);
}

/* We only care about the keyboard activation part that GtkListView provides,
 * but we don't need any special filtering here. Indeed, we ask GtkListView
 * to not activate on single click, and we get to handle double clicks before
 * GtkListView does (as one of widget subclassing's goal is to modify the parent
 * class's behavior), while claiming the click gestures, so it means GtkListView
 * will never react to a click event to emit this signal. So we should be pretty
 * safe here with regards to our custom item click handling.
 */
static void
on_list_view_item_activated (GtkListView *list_view,
                             guint        position,
                             gpointer     user_data)
{
    RebuntuComputerView *self = REBUNTU_COMPUTER_VIEW (user_data);

    nautilus_list_base_activate_selection (NAUTILUS_LIST_BASE (self), FALSE);
}

static void
real_popup_background_context_menu (NautilusListBase *self,
                                    double            x,
                                    double            y)
{
    g_signal_stop_emission_by_name (G_OBJECT (self), "popup-background-context-menu");
}

static guint
real_get_icon_size (NautilusListBase *list_base_view)
{
    return NAUTILUS_LIST_ICON_SIZE_SMALL;
}

static int
real_get_zoom_level (NautilusListBase *list_base_view)
{
    return computer_view_info.zoom_level_standard;
}

static void
real_scroll_to (NautilusListBase   *list_base_view,
                guint               position,
                GtkListScrollFlags  flags,
                GtkScrollInfo      *scroll)
{
    RebuntuComputerView *self = REBUNTU_COMPUTER_VIEW (list_base_view);

    gtk_list_view_scroll_to (self->view_ui, position, flags, scroll);
}

static GVariant *
real_get_sort_state (NautilusListBase *list_base)
{
    return g_variant_take_ref (g_variant_new ("(sb)", "invalid", FALSE));
}

static void
real_set_enable_rubberband (NautilusListBase *list_base,
                            gboolean          enabled)
{
    /* We ignore setting this value as it should always stay disabled. */
}

static void
real_set_sort_state (NautilusListBase *list_base,
                     GVariant         *value)
{
    /* No op */
}

/* UDisks2 inventory runs in a worker; no blocking D-Bus call on GTK thread.
 * The NautilusViewModel remains authoritative for file activation/selection.
 */
static void
inventory_worker (GTask *task,
                  gpointer source_object,
                  gpointer task_data,
                  GCancellable *cancellable)
{
    g_autoptr (GError) error = NULL;
    RebuntuDeviceModel *model = rebuntu_device_model_new ();

    if (g_cancellable_set_error_if_cancelled (cancellable, &error) ||
        !rebuntu_device_model_refresh (model, &error))
    {
        rebuntu_device_model_free (model);
        g_task_return_error (task, g_steal_pointer (&error));
        return;
    }

    if (g_cancellable_set_error_if_cancelled (cancellable, &error))
    {
        rebuntu_device_model_free (model);
        g_task_return_error (task, g_steal_pointer (&error));
        return;
    }

    g_task_return_pointer (task, model, (GDestroyNotify) rebuntu_device_model_free);
}

/* All UDisks2 mutations are asynchronous and subject to system Polkit policy. */
typedef struct
{
    RebuntuComputerView *view;
    char *object_path;
    gboolean mount;
} DeviceOperation;

static void
operation_free (DeviceOperation *op)
{
    g_free (op->object_path);
    g_free (op);
}

static void schedule_inventory_refresh (RebuntuComputerView *self);

static void
show_device_error (RebuntuComputerView *self, const char *message)
{
    if (self->disposing)
        return;
    GtkRoot *root = gtk_widget_get_root (GTK_WIDGET (self));
    if (!GTK_IS_WINDOW (root))
    {
        g_warning ("Rebuntu device operation: %s", message);
        return;
    }
    GtkWidget *dialog = gtk_message_dialog_new (GTK_WINDOW (root),
                                                 GTK_DIALOG_MODAL,
                                                 GTK_MESSAGE_ERROR,
                                                 GTK_BUTTONS_CLOSE,
                                                 "%s", message);
    g_signal_connect_swapped (dialog, "response", G_CALLBACK (gtk_window_destroy), dialog);
    gtk_window_present (GTK_WINDOW (dialog));
}

static void
on_device_method_finished (GObject *source, GAsyncResult *result, gpointer user_data)
{
    DeviceOperation *op = user_data;
    g_autoptr (GError) error = NULL;
    g_autoptr (GVariant) reply = g_dbus_connection_call_finish (G_DBUS_CONNECTION (source), result, &error);
    if (reply == NULL)
        show_device_error (op->view, error->message);
    if (!op->view->disposing)
        schedule_inventory_refresh (op->view);
    g_object_unref (op->view);
    operation_free (op);
}

static void
on_device_bus_ready (GObject *source, GAsyncResult *result, gpointer user_data)
{
    DeviceOperation *op = user_data;
    g_autoptr (GError) error = NULL;
    g_autoptr (GDBusConnection) bus = g_bus_get_finish (result, &error);
    if (bus == NULL)
    {
        show_device_error (op->view, error->message);
        g_object_unref (op->view);
        operation_free (op);
        return;
    }
    if (op->view->disposing)
    {
        g_object_unref (op->view);
        operation_free (op);
        return;
    }
    g_dbus_connection_call (bus,
                            "org.freedesktop.UDisks2",
                            op->object_path,
                            "org.freedesktop.UDisks2.Filesystem",
                            op->mount ? "Mount" : "Unmount",
                            g_variant_new ("(a{sv})", NULL),
                            op->mount ? G_VARIANT_TYPE ("(s)") : G_VARIANT_TYPE ("()"),
                            G_DBUS_CALL_FLAGS_NONE, 120000, NULL,
                            on_device_method_finished, op);
}

static void
on_device_mount_clicked (GtkButton *button, gpointer user_data)
{
    DeviceOperation *op = user_data;
    gtk_widget_set_sensitive (GTK_WIDGET (button), FALSE);
    DeviceOperation *request = g_new0 (DeviceOperation, 1);
    request->view = g_object_ref (op->view);
    request->object_path = g_strdup (op->object_path);
    request->mount = op->mount;
    g_bus_get (G_BUS_TYPE_SYSTEM, NULL, on_device_bus_ready, request);
}

typedef struct
{
    GWeakRef view;
    char *value;
} DeviceLink;

static DeviceLink *
device_link_new (RebuntuComputerView *view, const char *value)
{
    DeviceLink *link = g_new0 (DeviceLink, 1);
    g_weak_ref_init (&link->view, view);
    link->value = g_strdup (value);
    return link;
}

static void
device_link_free (gpointer data)
{
    DeviceLink *link = data;
    g_weak_ref_clear (&link->view);
    g_free (link->value);
    g_free (link);
}

static void
on_device_open_clicked (GtkButton *button, gpointer user_data)
{
    DeviceLink *link = user_data;
    g_autoptr (RebuntuComputerView) self = g_weak_ref_get (&link->view);
    g_autofree char *uri = g_filename_to_uri (link->value, NULL, NULL);
    g_autoptr (GError) error = NULL;
    if (uri == NULL || !g_app_info_launch_default_for_uri (uri, NULL, &error))
    {
        if (self != NULL)
            show_device_error (self, error != NULL ? error->message : _("Cannot open mount point"));
    }
}

static void
on_device_disks_clicked (GtkButton *button, gpointer user_data)
{
    DeviceLink *link = user_data;
    g_autoptr (RebuntuComputerView) self = g_weak_ref_get (&link->view);
    g_autoptr (GError) error = NULL;
    const char *argv[] = { "gnome-disks", "--block-device", link->value, NULL };
    if (!g_spawn_async (NULL, (char **) argv, NULL, G_SPAWN_SEARCH_PATH,
                        NULL, NULL, NULL, &error) && self != NULL)
        show_device_error (self, error->message);
}

static void
on_remote_open_clicked (GtkButton *button, gpointer user_data)
{
    DeviceLink *link = user_data;
    g_autoptr (RebuntuComputerView) self = g_weak_ref_get (&link->view);
    g_autoptr (GError) error = NULL;
    if (!g_app_info_launch_default_for_uri (link->value, NULL, &error) && self != NULL)
        show_device_error (self, error->message);
}

/* Show non-native GVfs mounts separately. The local block inventory remains
 * UDisks2-authoritative, so a mounted local partition is not duplicated. */
static void
render_remote_mounts (RebuntuComputerView *self)
{
    GtkWidget *child;
    if (self->remote_panel == NULL || self->volume_monitor == NULL)
        return;
    while ((child = gtk_widget_get_first_child (self->remote_panel)) != NULL)
        gtk_box_remove (GTK_BOX (self->remote_panel), child);

    GList *mounts = g_volume_monitor_get_mounts (self->volume_monitor);
    gboolean has_remote = FALSE;
    for (GList *l = mounts; l != NULL; l = l->next)
    {
        GMount *mount = G_MOUNT (l->data);
        g_autoptr (GFile) root = g_mount_get_root (mount);
        if (g_file_is_native (root))
            continue;
        g_autofree char *uri = g_file_get_uri (root);
        if (uri == NULL)
            continue;
        if (!has_remote)
        {
            GtkWidget *heading = gtk_label_new (_("Other locations (GVfs)"));
            gtk_label_set_xalign (GTK_LABEL (heading), 0);
            gtk_widget_add_css_class (heading, "heading");
            gtk_widget_set_margin_top (heading, 12);
            gtk_widget_set_margin_bottom (heading, 8);
            gtk_box_append (GTK_BOX (self->remote_panel), heading);
            has_remote = TRUE;
        }
        g_autofree char *name = g_mount_get_name (mount);
        GtkWidget *row = gtk_box_new (GTK_ORIENTATION_HORIZONTAL, 8);
        GtkWidget *icon = gtk_image_new_from_icon_name ("folder-remote-symbolic");
        GtkWidget *label = gtk_label_new (name != NULL ? name : uri);
        GtkWidget *open = gtk_button_new_with_label (_("Open"));
        gtk_label_set_xalign (GTK_LABEL (label), 0);
        gtk_widget_set_hexpand (label, TRUE);
        gtk_box_append (GTK_BOX (row), icon);
        gtk_box_append (GTK_BOX (row), label);
        g_signal_connect_data (open, "clicked", G_CALLBACK (on_remote_open_clicked),
                               device_link_new (self, uri), (GClosureNotify) device_link_free, 0);
        gtk_box_append (GTK_BOX (row), open);
        gtk_box_append (GTK_BOX (self->remote_panel), row);
    }
    g_list_free_full (mounts, g_object_unref);
}

/* UDisks2 presentation is separate from the Nautilus file selection model. */
static void
render_device_inventory (RebuntuComputerView *self)
{
    GtkWidget *child;
    const GPtrArray *devices;

    if (self->device_panel == NULL || self->device_model == NULL)
        return;

    while ((child = gtk_widget_get_first_child (self->device_panel)) != NULL)
        gtk_box_remove (GTK_BOX (self->device_panel), child);

    devices = rebuntu_device_model_get_devices (self->device_model);
    /* The GVfs computer:/// list mirrors these UDisks2 blocks. Avoid rendering
     * the same disks twice. Keep the model alive for Nautilus internals. */
    if (self->gvfs_section != NULL)
        gtk_widget_set_visible (self->gvfs_section, devices->len == 0);

    GtkWidget *heading = gtk_label_new (_("Physical devices and partitions"));
    gtk_label_set_xalign (GTK_LABEL (heading), 0);
    gtk_widget_add_css_class (heading, "heading");
    gtk_widget_set_margin_bottom (heading, 8);
    gtk_box_append (GTK_BOX (self->device_panel), heading);

    for (guint i = 0; i < devices->len; i++)
    {
        const RebuntuBlockDevice *dev = g_ptr_array_index ((GPtrArray *) devices, i);
        g_autofree char *size = g_format_size (dev->size);
        const char *device = dev->device != NULL ? dev->device : _("Block device");
        const char *label = dev->label != NULL && *dev->label ? dev->label : NULL;
        g_autofree char *title = NULL;
        g_autofree char *details = NULL;
        GtkWidget *row = gtk_box_new (GTK_ORIENTATION_HORIZONTAL, 10);
        GtkWidget *texts = gtk_box_new (GTK_ORIENTATION_VERTICAL, 3);
        GtkWidget *icon = gtk_image_new_from_icon_name (
            dev->is_partition ? "drive-harddisk-symbolic" : "drive-harddisk-solidstate-symbolic");

        gtk_widget_set_margin_start (row, dev->is_partition ? 24 : 0);
        gtk_widget_set_margin_top (row, 7);
        gtk_widget_set_margin_bottom (row, 7);
        gtk_widget_set_hexpand (row, TRUE);
        gtk_box_append (GTK_BOX (row), icon);
        gtk_widget_set_hexpand (texts, TRUE);
        gtk_widget_set_halign (texts, GTK_ALIGN_FILL);

        if (dev->is_partition)
            title = g_strdup_printf ("%s — %s", label != NULL ? label : device, size);
        else
            title = g_strdup_printf ("%s — %s", g_str_has_prefix (device, "/dev/sr") ? _("Optical drive") : _("Disk"), size);

        GtkWidget *name = gtk_label_new (title);
        gtk_label_set_xalign (GTK_LABEL (name), 0);
        gtk_label_set_ellipsize (GTK_LABEL (name), PANGO_ELLIPSIZE_END);
        gtk_box_append (GTK_BOX (texts), name);

        details = g_strdup_printf ("%s%s%s%s%s",
                                   device,
                                   dev->filesystem_type && *dev->filesystem_type ? " · " : "",
                                   dev->filesystem_type && *dev->filesystem_type ? dev->filesystem_type : "",
                                   dev->mount_point ? " · " : "",
                                   dev->mount_point ? dev->mount_point : "");
        GtkWidget *subtitle = gtk_label_new (details);
        gtk_label_set_xalign (GTK_LABEL (subtitle), 0);
        gtk_label_set_ellipsize (GTK_LABEL (subtitle), PANGO_ELLIPSIZE_END);
        gtk_widget_add_css_class (subtitle, "dim-label");
        gtk_box_append (GTK_BOX (texts), subtitle);

        if (dev->is_partition && dev->filesystem_usage_valid && dev->filesystem_total > 0)
        {
            guint64 free_bytes = MIN (dev->filesystem_free, dev->filesystem_total);
            guint64 used_bytes = dev->filesystem_total - free_bytes;
            double fraction = (double) used_bytes / (double) dev->filesystem_total;
            g_autofree char *used = g_format_size (used_bytes);
            g_autofree char *free = g_format_size (free_bytes);
            g_autofree char *usage = g_strdup_printf (
                _("%s used · %s available · %.0f%%"), used, free, fraction * 100.0);
            GtkWidget *usage_label = gtk_label_new (usage);
            gtk_label_set_xalign (GTK_LABEL (usage_label), 0);
            gtk_label_set_ellipsize (GTK_LABEL (usage_label), PANGO_ELLIPSIZE_END);
            gtk_widget_add_css_class (usage_label, "dim-label");
            gtk_box_append (GTK_BOX (texts), usage_label);
            GtkWidget *bar = gtk_progress_bar_new ();
            gtk_progress_bar_set_fraction (GTK_PROGRESS_BAR (bar), fraction);
            gtk_widget_set_halign (bar, GTK_ALIGN_START);
            gtk_widget_set_hexpand (bar, FALSE);
            gtk_widget_set_size_request (bar, 160, -1);
            gtk_widget_set_margin_top (bar, 3);
            gtk_box_append (GTK_BOX (texts), bar);
        }
        gtk_box_append (GTK_BOX (row), texts);

        /* A single always-visible actions control avoids clipping in narrow
         * windows. Popover buttons retain the existing UDisks2 callbacks. */
        GtkWidget *menu = gtk_menu_button_new ();
        gtk_menu_button_set_icon_name (GTK_MENU_BUTTON (menu), "view-more-symbolic");
        gtk_widget_set_tooltip_text (menu, _("Device actions"));
        gtk_widget_set_valign (menu, GTK_ALIGN_CENTER);
        GtkWidget *popover = gtk_popover_new ();
        GtkWidget *actions = gtk_box_new (GTK_ORIENTATION_VERTICAL, 4);
        gtk_widget_set_margin_start (actions, 8);
        gtk_widget_set_margin_end (actions, 8);
        gtk_widget_set_margin_top (actions, 8);
        gtk_widget_set_margin_bottom (actions, 8);
        if (dev->has_filesystem)
        {
            if (dev->is_mounted && dev->mount_point != NULL)
            {
                GtkWidget *open = gtk_button_new_with_label (_("Open"));
                g_signal_connect_data (open, "clicked", G_CALLBACK (on_device_open_clicked),
                                       device_link_new (self, dev->mount_point), (GClosureNotify) device_link_free, 0);
                gtk_box_append (GTK_BOX (actions), open);
            }
            gboolean protected_mount = dev->is_mounted && dev->mount_point != NULL &&
                (g_str_equal (dev->mount_point, "/") ||
                 g_str_equal (dev->mount_point, "/boot") ||
                 g_str_equal (dev->mount_point, "/boot/efi") ||
                 g_str_has_prefix (dev->mount_point, "/usr/") ||
                 g_str_equal (dev->mount_point, "/usr") ||
                 g_str_has_prefix (dev->mount_point, "/var/") ||
                 g_str_equal (dev->mount_point, "/var"));
            if (!protected_mount)
            {
                GtkWidget *mount = gtk_button_new_with_label (dev->is_mounted ? _("Unmount") : _("Mount"));
                DeviceOperation *op = g_new0 (DeviceOperation, 1);
                op->view = self;
                op->object_path = g_strdup (dev->object_path);
                op->mount = !dev->is_mounted;
                g_signal_connect_data (mount, "clicked", G_CALLBACK (on_device_mount_clicked),
                                       op, (GClosureNotify) operation_free, 0);
                gtk_box_append (GTK_BOX (actions), mount);
            }
        }
        if (dev->device != NULL && g_path_is_absolute (dev->device))
        {
            GtkWidget *disks = gtk_button_new_with_label (_("Open in Disks"));
            g_signal_connect_data (disks, "clicked", G_CALLBACK (on_device_disks_clicked),
                                   device_link_new (self, dev->device), (GClosureNotify) device_link_free, 0);
            gtk_box_append (GTK_BOX (actions), disks);
        }
        gtk_popover_set_child (GTK_POPOVER (popover), actions);
        gtk_menu_button_set_popover (GTK_MENU_BUTTON (menu), popover);
        gtk_box_append (GTK_BOX (row), menu);
        gtk_box_append (GTK_BOX (self->device_panel), row);
    }
}

static void
inventory_ready (GObject *source_object,
                 GAsyncResult *result,
                 gpointer user_data)
{
    RebuntuComputerView *self = REBUNTU_COMPUTER_VIEW (source_object);
    g_autoptr (GError) error = NULL;
    RebuntuDeviceModel *model = g_task_propagate_pointer (G_TASK (result), &error);

    if (self->disposing ||
        g_task_get_cancellable (G_TASK (result)) != self->refresh_cancel)
    {
        if (model != NULL)
            rebuntu_device_model_free (model);
        return;
    }

    if (model == NULL)
    {
        if (!g_error_matches (error, G_IO_ERROR, G_IO_ERROR_CANCELLED))
            g_warning ("Rebuntu UDisks2 inventory: %s", error->message);
        return;
    }

    rebuntu_device_model_free (self->device_model);
    self->device_model = model;
    g_debug ("Rebuntu UDisks2 inventory: %u block devices",
             rebuntu_device_model_get_devices (model)->len);
    render_device_inventory (self);
    render_remote_mounts (self);
}

static void
schedule_inventory_refresh (RebuntuComputerView *self)
{
    if (self->disposing)
        return;

    if (self->refresh_cancel != NULL)
        g_cancellable_cancel (self->refresh_cancel);
    g_clear_object (&self->refresh_cancel);
    self->refresh_cancel = g_cancellable_new ();

    g_autoptr (GTask) task = g_task_new (self, self->refresh_cancel,
                                       inventory_ready, NULL);
    g_task_set_return_on_cancel (task, TRUE);
    g_task_run_in_thread (task, inventory_worker);
}

static void
on_volume_monitor_changed (GVolumeMonitor *monitor,
                           gpointer volume_or_drive,
                           RebuntuComputerView *self)
{
    render_remote_mounts (self);
    schedule_inventory_refresh (self);
}

static void
rebuntu_computer_view_dispose (GObject *object)
{
    RebuntuComputerView *self = REBUNTU_COMPUTER_VIEW (object);
    self->disposing = TRUE;
    self->device_panel = NULL;
    self->remote_panel = NULL;
    self->gvfs_section = NULL;
    if (self->refresh_cancel != NULL)
        g_cancellable_cancel (self->refresh_cancel);
    g_clear_object (&self->refresh_cancel);
    if (self->volume_monitor != NULL)
        g_signal_handlers_disconnect_by_data (self->volume_monitor, self);
    g_clear_object (&self->volume_monitor);
    G_OBJECT_CLASS (rebuntu_computer_view_parent_class)->dispose (object);
}

static void
rebuntu_computer_view_finalize (GObject *object)
{
    RebuntuComputerView *self = REBUNTU_COMPUTER_VIEW (object);
    rebuntu_device_model_free (self->device_model);
    G_OBJECT_CLASS (rebuntu_computer_view_parent_class)->finalize (object);
}

static void
bind_cell (GtkSignalListItemFactory *factory,
           GtkListItem              *listitem,
           gpointer                  user_data)
{
    GtkWidget *cell = gtk_list_item_get_child (listitem);
    g_autoptr (NautilusViewItem) item = get_view_item (listitem);

    nautilus_view_item_set_item_ui (item, cell);
}

static void
unbind_cell (GtkSignalListItemFactory *factory,
             GtkListItem              *listitem,
             gpointer                  user_data)
{
    g_autoptr (NautilusViewItem) item = get_view_item (listitem);

    /* item may be NULL when row has just been destroyed. */
    if (item != NULL)
    {
        nautilus_view_item_set_item_ui (item, NULL);
    }
}

static void
setup_cell (GtkSignalListItemFactory *factory,
            GtkListItem              *listitem,
            gpointer                  user_data)
{
    RebuntuComputerView *self = REBUNTU_COMPUTER_VIEW (user_data);
    NautilusViewCell *cell;
    GtkExpression *expression;

    cell = rebuntu_device_cell_new (NAUTILUS_LIST_BASE (self));
    gtk_list_item_set_child (listitem, GTK_WIDGET (cell));
    setup_cell_common (G_OBJECT (listitem), cell);
    setup_cell_hover (cell);

    g_object_bind_property (self, "icon-size",
                            cell, "icon-size",
                            G_BINDING_SYNC_CREATE);

    /* Use file display name as accessible label. Explaining in pseudo-code:
     * listitem:accessible-name :- listitem:item:item:file:display-name */
    expression = gtk_property_expression_new (GTK_TYPE_LIST_ITEM, NULL, "item");
    expression = gtk_property_expression_new (GTK_TYPE_TREE_LIST_ROW, expression, "item");
    expression = gtk_property_expression_new (NAUTILUS_TYPE_VIEW_ITEM, expression, "file");
    expression = gtk_property_expression_new (NAUTILUS_TYPE_FILE, expression, "display-name");
    gtk_expression_bind (expression, listitem, "accessible-label", listitem);
}

static void
on_model_changed (RebuntuComputerView *self)
{
    NautilusViewModel *model = nautilus_list_base_get_model (NAUTILUS_LIST_BASE (self));

    if (model != NULL)
    {
        g_autoptr (GtkCustomSorter) sorter = gtk_custom_sorter_new (sort_computer_items, self, NULL);

        nautilus_view_model_set_sorter (model, GTK_SORTER (sorter));
    }

    gtk_list_view_set_model (self->view_ui, GTK_SELECTION_MODEL (model));
}

static GtkListView *
create_view_ui (RebuntuComputerView *self)
{
    g_autoptr (GtkListItemFactory) factory = gtk_signal_list_item_factory_new ();
    GtkListView *list_view;

    g_signal_connect (factory, "setup", G_CALLBACK (setup_cell), self);
    g_signal_connect (factory, "bind", G_CALLBACK (bind_cell), self);
    g_signal_connect (factory, "unbind", G_CALLBACK (unbind_cell), self);

    list_view = GTK_LIST_VIEW (gtk_list_view_new (NULL, g_steal_pointer (&factory)));

    /* We don't use the built-in child activation feature for clicks because it
     * doesn't fill all our needs nor does it match our expected behavior.
     * Instead, we roll our own event handling and double/single click mode.
     * However, GtkListView:single-click-activate has other effects besides
     * activation, as it affects the selection behavior as well (e.g. selects on
     * hover). Setting it to FALSE gives us the expected behavior. */
    gtk_list_view_set_single_click_activate (list_view, FALSE);
    gtk_list_view_set_enable_rubberband (list_view, FALSE);
    gtk_list_view_set_tab_behavior (list_view, GTK_LIST_TAB_ITEM);

    gtk_accessible_update_property (GTK_ACCESSIBLE (list_view),
                                    GTK_ACCESSIBLE_PROPERTY_LABEL,
                                    _("Content View"),
                                    GTK_ACCESSIBLE_PROPERTY_ROLE_DESCRIPTION,
                                    _("View of the current location"),
                                    -1);

    /* While we don't want to use GTK's click activation, we'll let it handle
     * the key activation part (with Enter). */
    g_signal_connect (list_view, "activate", G_CALLBACK (on_list_view_item_activated), self);

    return list_view;
}

static void
rebuntu_computer_view_class_init (RebuntuComputerViewClass *klass)
{
    GObjectClass *object_class = G_OBJECT_CLASS (klass);
    NautilusListBaseClass *list_base_view_class = NAUTILUS_LIST_BASE_CLASS (klass);

    object_class->dispose = rebuntu_computer_view_dispose;
    object_class->finalize = rebuntu_computer_view_finalize;

    list_base_view_class->get_icon_size = real_get_icon_size;
    list_base_view_class->get_sort_state = real_get_sort_state;
    list_base_view_class->get_view_info = real_get_view_info;
    list_base_view_class->get_zoom_level = real_get_zoom_level;
    list_base_view_class->popup_background_context_menu = real_popup_background_context_menu;
    list_base_view_class->scroll_to = real_scroll_to;
    list_base_view_class->set_enable_rubberband = real_set_enable_rubberband;
    list_base_view_class->set_sort_state = real_set_sort_state;
    list_base_view_class->set_zoom_level = real_set_zoom_level;
}

static void
rebuntu_computer_view_init (RebuntuComputerView *self)
{
    GtkWidget *scrolled_window = nautilus_list_base_get_scrolled_window (NAUTILUS_LIST_BASE (self));

    self->volume_monitor = g_volume_monitor_get ();
    g_signal_connect (self->volume_monitor, "drive-connected",
                      G_CALLBACK (on_volume_monitor_changed), self);
    g_signal_connect (self->volume_monitor, "drive-disconnected",
                      G_CALLBACK (on_volume_monitor_changed), self);
    g_signal_connect (self->volume_monitor, "volume-added",
                      G_CALLBACK (on_volume_monitor_changed), self);
    g_signal_connect (self->volume_monitor, "volume-removed",
                      G_CALLBACK (on_volume_monitor_changed), self);
    g_signal_connect (self->volume_monitor, "mount-added",
                      G_CALLBACK (on_volume_monitor_changed), self);
    g_signal_connect (self->volume_monitor, "mount-removed",
                      G_CALLBACK (on_volume_monitor_changed), self);
    g_signal_connect (self->volume_monitor, "mount-changed",
                      G_CALLBACK (on_volume_monitor_changed), self);

    gtk_widget_add_css_class (GTK_WIDGET (self), "rebuntu-computer-view");

    self->view_ui = create_view_ui (self);
    nautilus_list_base_setup_gestures (NAUTILUS_LIST_BASE (self));

    g_signal_connect_swapped (self, "notify::model", G_CALLBACK (on_model_changed), self);

    GtkWidget *content = gtk_box_new (GTK_ORIENTATION_VERTICAL, 0);
    self->device_panel = gtk_box_new (GTK_ORIENTATION_VERTICAL, 0);
    gtk_widget_set_margin_start (self->device_panel, 12);
    gtk_widget_set_margin_end (self->device_panel, 12);
    gtk_widget_set_margin_top (self->device_panel, 12);
    gtk_widget_set_margin_bottom (self->device_panel, 12);
    gtk_box_append (GTK_BOX (content), self->device_panel);
    self->remote_panel = gtk_box_new (GTK_ORIENTATION_VERTICAL, 6);
    gtk_widget_set_margin_start (self->remote_panel, 12);
    gtk_widget_set_margin_end (self->remote_panel, 12);
    gtk_box_append (GTK_BOX (content), self->remote_panel);
    render_remote_mounts (self);
    self->gvfs_section = GTK_WIDGET (self->view_ui);
    gtk_widget_set_vexpand (self->gvfs_section, TRUE);
    gtk_box_append (GTK_BOX (content), self->gvfs_section);
    gtk_scrolled_window_set_child (GTK_SCROLLED_WINDOW (scrolled_window), content);
    nautilus_list_base_set_zoom_level (NAUTILUS_LIST_BASE (self), computer_view_info.zoom_level_standard);
    schedule_inventory_refresh (self);
}

RebuntuComputerView *
rebuntu_computer_view_new (void)
{
    return g_object_new (REBUNTU_TYPE_COMPUTER_VIEW, NULL);
}
