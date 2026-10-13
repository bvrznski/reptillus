/* Rebuntu Computer view device row. SPDX-License-Identifier: GPL-3.0-or-later */
#include "rebuntu-device-cell.h"
#include "rebuntu-capacity-bar.h"
#include "rebuntu-volume-resolver.h"
#include "nautilus-file.h"
#include "nautilus-view-item.h"
#include <glib/gi18n.h>

struct _RebuntuDeviceCell
{
    NautilusViewCell parent_instance;
    GSignalGroup *item_signals;
    GtkWidget *row;
    GtkWidget *icon;
    GtkWidget *name;
    GtkWidget *subtitle;
    GtkWidget *capacity_holder;
    GtkWidget *capacity;
    GtkWidget *unmount_button;
    char *capacity_uri;
};
G_DEFINE_TYPE (RebuntuDeviceCell, rebuntu_device_cell, NAUTILUS_TYPE_VIEW_CELL)

static void
on_unmount_clicked (GtkButton *button, gpointer user_data)
{
    RebuntuDeviceCell *self = REBUNTU_DEVICE_CELL (user_data);
    gtk_widget_activate_action (GTK_WIDGET (self), "listitem.select", "(bb)", FALSE, FALSE);
    gtk_widget_activate_action (GTK_WIDGET (self), "view.unmount-volume", NULL);
}

static void
update_cell (RebuntuDeviceCell *self)
{
    g_autoptr (NautilusViewItem) item = nautilus_view_cell_get_item (NAUTILUS_VIEW_CELL (self));
    NautilusFile *file;
    g_autoptr (GIcon) icon = NULL;
    g_autofree char *activation_uri = NULL;
    g_autoptr (GFile) target = NULL;
    const char *desired_uri = NULL;

    if (item == NULL)
    {
        gtk_label_set_text (GTK_LABEL (self->name), "");
        gtk_label_set_text (GTK_LABEL (self->subtitle), "");
        gtk_widget_set_visible (self->unmount_button, FALSE);
        if (self->capacity != NULL)
        {
            gtk_box_remove (GTK_BOX (self->capacity_holder), self->capacity);
            self->capacity = NULL;
        }
        g_clear_pointer (&self->capacity_uri, g_free);
        return;
    }

    file = nautilus_view_item_get_file (item);
    gtk_label_set_text (GTK_LABEL (self->name), nautilus_file_get_display_name (file));
    icon = nautilus_file_get_gicon (file, NAUTILUS_FILE_ICON_FLAGS_NONE);
    gtk_image_set_from_gicon (GTK_IMAGE (self->icon), icon);
    gtk_widget_set_visible (self->unmount_button, nautilus_file_can_unmount (file));

    /*
     * Resolve computer:/// entries to real mounted filesystem roots.
     * Prefer GVfs standard::target-uri over activation URI heuristics.
     */
    g_autoptr (GFile) location = nautilus_file_get_location (file);

    if (location != NULL)
        target = rebuntu_volume_resolver_get_mount_root (location);

    /*
     * Fallback for Nautilus entries whose activation URI already
     * identifies a native mounted filesystem.
     */
    if (target == NULL)
    {
        activation_uri = nautilus_file_get_activation_uri (file);

        if (activation_uri != NULL &&
            g_str_has_prefix (activation_uri, "file:///"))
        {
            target = g_file_new_for_uri (activation_uri);
        }
    }

    g_autofree char *resolved_uri = NULL;

    if (target != NULL)
    {
        resolved_uri = g_file_get_uri (target);
        desired_uri = resolved_uri;

        gtk_label_set_text (
            GTK_LABEL (self->subtitle),
            _("Mounted filesystem")
        );
    }
    else
    {
        gtk_label_set_text (
            GTK_LABEL (self->subtitle),
            _("Not mounted or capacity unavailable")
        );
    }

    if (g_strcmp0 (self->capacity_uri, desired_uri) != 0)
    {
        if (self->capacity != NULL)
        {
            gtk_box_remove (GTK_BOX (self->capacity_holder), self->capacity);
            self->capacity = NULL;
        }
        g_free (self->capacity_uri);
        self->capacity_uri = g_strdup (desired_uri);
        if (target != NULL)
        {
            self->capacity = rebuntu_capacity_bar_new (target);
            gtk_box_append (GTK_BOX (self->capacity_holder), self->capacity);
        }
    }
}

static void
rebuntu_device_cell_dispose (GObject *object)
{
    RebuntuDeviceCell *self = REBUNTU_DEVICE_CELL (object);
    g_signal_group_set_target (self->item_signals, NULL);
    if (self->row != NULL)
    {
        gtk_widget_unparent (self->row);
        self->row = NULL;
    }
    self->capacity = NULL;
    G_OBJECT_CLASS (rebuntu_device_cell_parent_class)->dispose (object);
}

static void
rebuntu_device_cell_finalize (GObject *object)
{
    RebuntuDeviceCell *self = REBUNTU_DEVICE_CELL (object);
    g_clear_object (&self->item_signals);
    g_free (self->capacity_uri);
    G_OBJECT_CLASS (rebuntu_device_cell_parent_class)->finalize (object);
}

static void
rebuntu_device_cell_class_init (RebuntuDeviceCellClass *klass)
{
    GObjectClass *object_class = G_OBJECT_CLASS (klass);
    GtkWidgetClass *widget_class = GTK_WIDGET_CLASS (klass);
    object_class->dispose = rebuntu_device_cell_dispose;
    object_class->finalize = rebuntu_device_cell_finalize;
    gtk_widget_class_set_layout_manager_type (widget_class, GTK_TYPE_BIN_LAYOUT);
}

static void
rebuntu_device_cell_init (RebuntuDeviceCell *self)
{
    GtkWidget *text_box;
    self->row = gtk_box_new (GTK_ORIENTATION_HORIZONTAL, 12);
    gtk_widget_set_parent (self->row, GTK_WIDGET (self));
    gtk_widget_set_margin_top (self->row, 8);
    gtk_widget_set_margin_bottom (self->row, 8);
    gtk_widget_set_margin_start (self->row, 12);
    gtk_widget_set_margin_end (self->row, 12);
    self->icon = gtk_image_new ();
    gtk_image_set_pixel_size (GTK_IMAGE (self->icon), 32);
    gtk_box_append (GTK_BOX (self->row), self->icon);

    text_box = gtk_box_new (GTK_ORIENTATION_VERTICAL, 4);
    gtk_widget_set_hexpand (text_box, TRUE);
    self->name = gtk_label_new (NULL);
    gtk_label_set_xalign (GTK_LABEL (self->name), 0);
    gtk_label_set_ellipsize (GTK_LABEL (self->name), PANGO_ELLIPSIZE_END);
    gtk_box_append (GTK_BOX (text_box), self->name);
    self->subtitle = gtk_label_new (NULL);
    gtk_label_set_xalign (GTK_LABEL (self->subtitle), 0);
    gtk_widget_add_css_class (self->subtitle, "dim-label");
    gtk_widget_add_css_class (self->subtitle, "caption");
    gtk_box_append (GTK_BOX (text_box), self->subtitle);
    self->capacity_holder = gtk_box_new (GTK_ORIENTATION_VERTICAL, 0);
    gtk_box_append (GTK_BOX (text_box), self->capacity_holder);
    gtk_box_append (GTK_BOX (self->row), text_box);

    self->unmount_button = gtk_button_new_from_icon_name ("media-eject-symbolic");
    gtk_widget_add_css_class (self->unmount_button, "flat");
    gtk_widget_set_valign (self->unmount_button, GTK_ALIGN_CENTER);
    gtk_widget_set_tooltip_text (self->unmount_button, _("Unmount"));
    gtk_widget_set_visible (self->unmount_button, FALSE);
    g_signal_connect (self->unmount_button, "clicked", G_CALLBACK (on_unmount_clicked), self);
    gtk_box_append (GTK_BOX (self->row), self->unmount_button);

    self->item_signals = g_signal_group_new (NAUTILUS_TYPE_VIEW_ITEM);
    g_signal_group_connect_swapped (self->item_signals, "file-changed", G_CALLBACK (update_cell), self);
    g_signal_group_connect_swapped (self->item_signals, "notify::loading", G_CALLBACK (update_cell), self);
    g_signal_connect_swapped (self->item_signals, "bind", G_CALLBACK (update_cell), self);
    g_object_bind_property (self, "item", self->item_signals, "target", G_BINDING_SYNC_CREATE);
}

NautilusViewCell *
rebuntu_device_cell_new (NautilusListBase *view)
{
    return NAUTILUS_VIEW_CELL (g_object_new (REBUNTU_TYPE_DEVICE_CELL, "view", view, NULL));
}
