/* desktop recorder
 * Copyright (C) 2005 Benjamin Otte <otte@gnome.org
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Library General Public
 * License as published by the Free Software Foundation; either
 * version 2 of the License, or (at your option) any later version.
 *
 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Library General Public License for more details.
 *
 * You should have received a copy of the GNU Library General Public
 * License along with this library; if not, write to the
 * Free Software Foundation, Inc., 59 Temple Place - Suite 330,
 * Boston, MA 02111-1307, USA.
 */

#ifdef HAVE_CONFIG_H
#  include "config.h"
#endif

#include <panel-applet.h>
#include <gtk/gtklabel.h>
#include <glib/gstdio.h>
#include <libgnomevfs/gnome-vfs.h>
#include <libgnomeui/libgnomeui.h>
#include "byzanzrecorder.h"
#include "byzanzselect.h"
#include "panelstuffer.h"
#include "i18n.h"

/*** PENDING RECORDING ***/

typedef struct {
  ByzanzRecorder *	rec;
  GnomeVFSAsyncHandle *	handle;
  char *		tmp_file;
  GnomeVFSURI *		target;
} PendingRecording;

static void
byzanz_applet_show_error (GtkWindow *parent, const char *error, const char *details, ...)
{
  GtkWidget *dialog;
  gchar *msg;
  va_list args;

  g_return_if_fail (details != NULL);
  
  va_start (args, details);
  msg = g_strdup_vprintf (details, args);
  va_end (args);
  dialog = gtk_message_dialog_new (parent, GTK_DIALOG_MODAL, GTK_MESSAGE_ERROR,
      GTK_BUTTONS_CLOSE, error ? error : msg);
  if (error)
    gtk_message_dialog_format_secondary_text (GTK_MESSAGE_DIALOG (dialog), msg);
  gtk_dialog_run (GTK_DIALOG (dialog));
  gtk_widget_destroy (dialog);
  g_free (msg);
}

static void
pending_recording_destroy (PendingRecording *pending)
{
  g_assert (pending->rec == NULL);

  if (pending->tmp_file) {
    g_unlink (pending->tmp_file);
    g_free (pending->tmp_file);
  }
  gnome_vfs_uri_unref (pending->target);
  g_free (pending);
  gtk_main_quit ();
}

static int
transfer_progress_cb (GnomeVFSAsyncHandle *handle, GnomeVFSXferProgressInfo *info, gpointer data)
{
  PendingRecording *pending = data;
  char *target_uri;

  switch (info->status) {
    case GNOME_VFS_XFER_PROGRESS_STATUS_VFSERROR:
      target_uri = gnome_vfs_uri_to_string (pending->target, GNOME_VFS_URI_HIDE_PASSWORD | GNOME_VFS_URI_HIDE_FRAGMENT_IDENTIFIER);
      byzanz_applet_show_error (NULL, _("A file could not be saved."),
	  _("\"%s\" could not be saved.\nThe error that occured was: %s"), 
	    target_uri, gnome_vfs_result_to_string (info->vfs_status));
      g_free (target_uri);
      return 0;
    case GNOME_VFS_XFER_PROGRESS_STATUS_OK:
      if (info->phase == GNOME_VFS_XFER_PHASE_COMPLETED) {
	g_free (pending->tmp_file);
	pending->tmp_file = NULL;
	pending_recording_destroy (pending);
	gtk_main_quit ();
	return 0;
      }
      return 1;
    case GNOME_VFS_XFER_PROGRESS_STATUS_OVERWRITE:
    case GNOME_VFS_XFER_PROGRESS_STATUS_DUPLICATE:
    default:
      g_assert_not_reached ();
      return 0;
  }
}

static gboolean
check_done_saving_cb (gpointer data)
{
  PendingRecording *pending = data;
  GList *src, *target;
  GnomeVFSURI *src_uri;

  if (byzanz_recorder_is_active (pending->rec))
    return TRUE;
  byzanz_recorder_destroy (pending->rec);
  pending->rec = NULL;

  if (pending->target == NULL) {
    pending_recording_destroy (pending);
    return FALSE;
  }
  src_uri = gnome_vfs_uri_new (pending->tmp_file);
  src = g_list_prepend (NULL, src_uri);
  target = g_list_prepend (NULL, pending->target);
  gnome_vfs_async_xfer (&pending->handle, src, target,
      GNOME_VFS_XFER_REMOVESOURCE | GNOME_VFS_XFER_TARGET_DEFAULT_PERMS,
      GNOME_VFS_XFER_ERROR_MODE_QUERY,
      GNOME_VFS_XFER_OVERWRITE_MODE_REPLACE, /* we do overwrite confirmation in the dialog */
      GNOME_VFS_PRIORITY_DEFAULT,
      transfer_progress_cb, pending, NULL, NULL);

  g_list_free (src);
  g_list_free (target);
  gnome_vfs_uri_unref (src_uri);
  return FALSE;
}

static void
pending_recording_launch (ByzanzRecorder *rec, char *tmp_file)
{
  PendingRecording *pending;
  GtkWidget *dialog;
  
  pending = g_new0 (PendingRecording, 1);
  pending->rec = rec;
  pending->tmp_file = tmp_file;
  
  dialog = gtk_file_chooser_dialog_new (_("Save Recorded File"),
      NULL, GTK_FILE_CHOOSER_ACTION_SAVE,
      GTK_STOCK_CANCEL, GTK_RESPONSE_CANCEL,
      GTK_STOCK_SAVE, GTK_RESPONSE_ACCEPT,
      NULL);
  gtk_file_chooser_set_local_only (GTK_FILE_CHOOSER (dialog), FALSE);
  gtk_file_chooser_set_current_folder (GTK_FILE_CHOOSER (dialog), g_get_home_dir ());
  gtk_file_chooser_set_do_overwrite_confirmation (GTK_FILE_CHOOSER (dialog), TRUE);
  gtk_dialog_set_default_response (GTK_DIALOG (dialog), GTK_RESPONSE_ACCEPT);
  if (GTK_RESPONSE_ACCEPT == gtk_dialog_run (GTK_DIALOG (dialog))) {
    char *target_uri = gtk_file_chooser_get_uri (GTK_FILE_CHOOSER (dialog));
    pending->target = gnome_vfs_uri_new (target_uri);
  }
  gtk_widget_destroy (dialog);
  g_timeout_add (1000, check_done_saving_cb, pending);
  gtk_main ();
  return;
}

/*** APPLET ***/

typedef struct {
  PanelApplet *		applet;		/* the applet we manage */

  GtkWidget *		button;		/* recording button */
  GtkWidget *		label;		/* infotext label */
  GtkWidget *		progress;	/* progressbar showing cache effects */
  
  ByzanzRecorder *	rec;		/* the recorder (if recording) */
  char *		tmp_file;	/* filename that's recorded to */
  GTimeVal		start;		/* time the recording started */
  guint			update_func;	/* id of idle function that updates state */
} AppletPrivate;
#define APPLET_IS_RECORDING(applet) ((applet)->tmp_file != NULL)

static gboolean
byzanz_applet_is_recording (AppletPrivate *priv)
{
  return priv->tmp_file != NULL;
}

static void
byzanz_applet_ensure_text (AppletPrivate *priv, const char *text)
{
  const char *current;

  current = gtk_label_get_text (GTK_LABEL (priv->label));
  if (g_str_equal (current, text))
    return;
  gtk_label_set_text (GTK_LABEL (priv->label), text);
}

static gboolean
byzanz_applet_update (gpointer data)
{
  AppletPrivate *priv = data;

  if (byzanz_applet_is_recording (priv)) {
    /* applet is still actively recording the screen */
    GTimeVal tv;
    guint elapsed;
    gchar *str;
    
    g_get_current_time (&tv);
    elapsed = tv.tv_sec - priv->start.tv_sec;
    if (tv.tv_usec < priv->start.tv_usec)
      elapsed--;
    str = g_strdup_printf ("%u", elapsed);
    byzanz_applet_ensure_text (priv, str);
    g_free (str);
    gtk_toggle_button_set_active (GTK_TOGGLE_BUTTON (priv->button), TRUE);
    gtk_widget_set_sensitive (priv->button, TRUE);
  } else {
    gtk_toggle_button_set_active (GTK_TOGGLE_BUTTON (priv->button), FALSE);
    gtk_label_set_text (GTK_LABEL (priv->label), _("OFF"));
    g_source_remove (priv->update_func);
    priv->update_func = 0;
  }
  
  return TRUE;
}

static void
byzanz_applet_start_recording (AppletPrivate *priv)
{
  GdkWindow *window;
  GdkRectangle area;
  
  g_assert (!byzanz_applet_is_recording (priv));
  
  if (byzanz_applet_is_recording (priv))
    goto out;
  if (priv->rec) {
    if (byzanz_recorder_is_active (priv->rec))
      goto out;
    byzanz_recorder_destroy (priv->rec);
    priv->rec = NULL;
  }
  
  window = byzanz_select_method_select (0, &area); 
  if (window) {
    int fd = g_file_open_tmp ("byzanzXXXXXX", &priv->tmp_file, NULL);
    if (fd > 0) 
      priv->rec = byzanz_recorder_new_fd (fd, window, &area, TRUE);
  }
  if (priv->rec) {
    byzanz_recorder_prepare (priv->rec);
    byzanz_recorder_start (priv->rec);
    g_get_current_time (&priv->start);
    priv->update_func = g_timeout_add (1000, byzanz_applet_update, priv);
  }

out:
  byzanz_applet_update (priv);
}

static void
byzanz_applet_stop_recording (AppletPrivate *priv)
{
  char *tmp_file;
  ByzanzRecorder *rec;
  
  g_assert (byzanz_applet_is_recording (priv));
  
  byzanz_recorder_stop (priv->rec);
  tmp_file = priv->tmp_file;
  priv->tmp_file = NULL;
  rec = priv->rec;
  priv->rec = NULL;
  byzanz_applet_update (priv);
  pending_recording_launch (rec, tmp_file);
}

static void
button_clicked_cb (GtkToggleButton *button, AppletPrivate *priv)
{
  gboolean active = gtk_toggle_button_get_active (button);
  
  if (priv->rec && !active) {
    byzanz_applet_stop_recording (priv);
  } else if (!priv->rec && active) {
    byzanz_applet_start_recording (priv);
  }
}

static void
destroy_applet (GtkWidget *widget, AppletPrivate *priv)
{
  if (byzanz_applet_is_recording (priv))
    byzanz_applet_stop_recording (priv);
  g_assert (!priv->rec); 
  g_free (priv);
}

static gboolean
byzanz_applet_fill (PanelApplet *applet, const gchar *iid, gpointer data)
{
  AppletPrivate *priv;
  GtkWidget *image, *stuffer;
  
  gnome_vfs_init ();
  gnome_authentication_manager_init ();

  priv = g_new0 (AppletPrivate, 1);
  priv->applet = applet;
  g_signal_connect (applet, "destroy", G_CALLBACK (destroy_applet), priv);

  panel_applet_set_flags (applet, PANEL_APPLET_EXPAND_MINOR);
  /* create UI */
  stuffer = panel_stuffer_new (GTK_ORIENTATION_VERTICAL);
  gtk_container_add (GTK_CONTAINER (applet), stuffer);

  priv->button = gtk_toggle_button_new ();
  image = gtk_image_new_from_stock (GTK_STOCK_MEDIA_RECORD, 
      GTK_ICON_SIZE_SMALL_TOOLBAR);
  gtk_container_add (GTK_CONTAINER (priv->button), image);
  g_signal_connect (priv->button, "toggled", G_CALLBACK (button_clicked_cb), priv);
  panel_stuffer_add_full (PANEL_STUFFER (stuffer), priv->button, FALSE, TRUE);

  /* translators: the label advertises a width of 5 characters */
  priv->label = gtk_label_new (_("OFF"));
  gtk_label_set_width_chars (GTK_LABEL (priv->label), 5);
  gtk_label_set_justify (GTK_LABEL (priv->label), GTK_JUSTIFY_CENTER);
  panel_stuffer_add_full (PANEL_STUFFER (stuffer), priv->label, FALSE, FALSE);

  gtk_widget_show_all (GTK_WIDGET (applet));
  return TRUE;
}

PANEL_APPLET_BONOBO_FACTORY ("OAFIID:ByzanzApplet_Factory",
    PANEL_TYPE_APPLET, "ByzanzApplet", "0",
    byzanz_applet_fill, NULL);

