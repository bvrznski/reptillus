#include "rebuntu-capacity-bar.h"

#define CAPACITY_DATA_KEY "rebuntu-capacity-state"

typedef struct {
  GFile *location;
  GtkWidget *label;
  GtkWidget *progress;
  GCancellable *cancellable;
  guint generation;
} CapacityState;

typedef struct {
  GtkWidget *widget;
  guint generation;
} CapacityRequest;

static void
capacity_state_free (gpointer data)
{
  CapacityState *state = data;
  if (state->cancellable) {
    g_cancellable_cancel (state->cancellable);
    g_clear_object (&state->cancellable);
  }
  g_clear_object (&state->location);
  g_free (state);
}

static void
capacity_query_done (GObject *source, GAsyncResult *result, gpointer user_data)
{
  CapacityRequest *request = user_data;
  CapacityState *state = g_object_get_data (G_OBJECT (request->widget), CAPACITY_DATA_KEY);
  GError *error = NULL;
  GFileInfo *info = g_file_query_filesystem_info_finish (G_FILE (source), result, &error);

  if (state && request->generation == state->generation) {
    if (info && g_file_info_has_attribute (info, G_FILE_ATTRIBUTE_FILESYSTEM_SIZE) &&
        g_file_info_has_attribute (info, G_FILE_ATTRIBUTE_FILESYSTEM_FREE)) {
      guint64 total = g_file_info_get_attribute_uint64 (info, G_FILE_ATTRIBUTE_FILESYSTEM_SIZE);
      guint64 free_bytes = g_file_info_get_attribute_uint64 (info, G_FILE_ATTRIBUTE_FILESYSTEM_FREE);
      guint64 used = total > free_bytes ? total - free_bytes : 0;
      double fraction = total ? (double) used / (double) total : 0.0;
      char *free_text = g_format_size (free_bytes);
      char *total_text = g_format_size (total);
      char *text = g_strdup_printf ("%s wolne z %s", free_text, total_text);
      gtk_label_set_text (GTK_LABEL (state->label), text);
      gtk_progress_bar_set_fraction (GTK_PROGRESS_BAR (state->progress), fraction);
      gtk_widget_remove_css_class (state->progress, "capacity-warning");
      gtk_widget_remove_css_class (state->progress, "capacity-critical");
      if (fraction >= 0.95)
        gtk_widget_add_css_class (state->progress, "capacity-critical");
      else if (fraction >= 0.85)
        gtk_widget_add_css_class (state->progress, "capacity-warning");
      gtk_widget_set_visible (state->progress, total > 0);
      g_free (text);
      g_free (free_text);
      g_free (total_text);
    } else {
      gtk_label_set_text (GTK_LABEL (state->label), "Pojemność niedostępna");
      gtk_widget_set_visible (state->progress, FALSE);
    }
  }
  g_clear_object (&info);
  g_clear_error (&error);
  g_object_unref (request->widget);
  g_free (request);
}

void
rebuntu_capacity_bar_refresh (GtkWidget *widget)
{
  g_return_if_fail (GTK_IS_WIDGET (widget));
  CapacityState *state = g_object_get_data (G_OBJECT (widget), CAPACITY_DATA_KEY);
  g_return_if_fail (state != NULL);
  if (state->cancellable)
    g_cancellable_cancel (state->cancellable);
  g_clear_object (&state->cancellable);
  state->cancellable = g_cancellable_new ();
  state->generation++;
  CapacityRequest *request = g_new0 (CapacityRequest, 1);
  request->widget = g_object_ref (widget);
  request->generation = state->generation;
  g_file_query_filesystem_info_async (state->location,
      G_FILE_ATTRIBUTE_FILESYSTEM_SIZE "," G_FILE_ATTRIBUTE_FILESYSTEM_FREE,
      G_PRIORITY_DEFAULT, state->cancellable, capacity_query_done, request);
}

GtkWidget *
rebuntu_capacity_bar_new (GFile *location)
{
  g_return_val_if_fail (G_IS_FILE (location), NULL);
  GtkWidget *box = gtk_box_new (GTK_ORIENTATION_HORIZONTAL, 12);
  GtkWidget *label = gtk_label_new ("Odczytywanie pojemności…");
  GtkWidget *progress = gtk_progress_bar_new ();
  gtk_widget_add_css_class (box, "rebuntu-capacity");
  gtk_widget_add_css_class (label, "capacity-label");
  gtk_widget_add_css_class (progress, "capacity-progress");
  gtk_label_set_xalign (GTK_LABEL (label), 1.0f);
  gtk_widget_set_hexpand (label, TRUE);
  gtk_widget_set_size_request (progress, 150, -1);
  gtk_widget_set_hexpand (progress, TRUE);
  gtk_widget_set_valign (progress, GTK_ALIGN_CENTER);
  gtk_box_append (GTK_BOX (box), label);
  gtk_box_append (GTK_BOX (box), progress);
  CapacityState *state = g_new0 (CapacityState, 1);
  state->location = g_object_ref (location);
  state->label = label;
  state->progress = progress;
  g_object_set_data_full (G_OBJECT (box), CAPACITY_DATA_KEY, state, capacity_state_free);
  rebuntu_capacity_bar_refresh (box);
  return box;
}
