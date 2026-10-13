#pragma once
#include <gtk/gtk.h>
G_BEGIN_DECLS
/* Returns a GtkBox containing a label and a capacity progress bar.
 * The returned widget has a floating reference like other GTK widgets. */
GtkWidget *rebuntu_capacity_bar_new (GFile *location);
/* Re-query the filesystem asynchronously. */
void rebuntu_capacity_bar_refresh (GtkWidget *widget);
G_END_DECLS
