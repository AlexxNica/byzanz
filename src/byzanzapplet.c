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
#include "byzanzrecorder.h"

typedef struct {
  PanelApplet *applet;
  ByzanzRecorder *rec;
} AppletPrivate;

static void
button_clicked_cb (GtkToggleButton *button, AppletPrivate *priv)
{
  gboolean active = gtk_toggle_button_get_active (button);
  
  if (priv->rec && !active) {
    byzanz_recorder_stop (priv->rec);
    byzanz_recorder_destroy (priv->rec);
    priv->rec = NULL;
  } else if (!priv->rec && active) {
    priv->rec = byzanz_recorder_new ("/root/test.gif", 0, 0, G_MAXINT / 2, G_MAXINT / 2, TRUE);
    if (priv->rec) {
      byzanz_recorder_prepare (priv->rec);
      byzanz_recorder_start (priv->rec);
    } else {
      gtk_toggle_button_set_active (button, FALSE);
    }
  }

}

static void
destroy_applet (GtkWidget *widget, AppletPrivate *priv)
{
  g_free (priv);
}

static gboolean
byzanz_applet_fill (PanelApplet *applet, const gchar *iid, gpointer data)
{
  AppletPrivate *priv;
  GtkWidget *button, *image;
  
  priv = g_new0 (AppletPrivate, 1);
  priv->applet = applet;
  g_signal_connect (applet, "destroy", G_CALLBACK (destroy_applet), priv);

  button = gtk_toggle_button_new ();
  image = gtk_image_new_from_stock (GTK_STOCK_MEDIA_RECORD, 
      GTK_ICON_SIZE_SMALL_TOOLBAR);
  gtk_container_add (GTK_CONTAINER (button), image);
  g_signal_connect (button, "toggled", G_CALLBACK (button_clicked_cb), priv);
  gtk_container_add (GTK_CONTAINER (applet), button);
  gtk_widget_show_all (GTK_WIDGET (applet));
  return TRUE;
}

PANEL_APPLET_BONOBO_FACTORY ("OAFIID:ByzanzApplet_Factory",
    PANEL_TYPE_APPLET, "ByzanzApplet", "0",
    byzanz_applet_fill, NULL);

