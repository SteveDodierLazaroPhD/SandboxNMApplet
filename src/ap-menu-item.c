/* -*- Mode: C; tab-width: 4; indent-tabs-mode: t; c-basic-offset: 4 -*- */
/* ap-menu-item.c - Class to represent a Wifi access point 
 *
 * Jonathan Blandford <jrb@redhat.com>
 * Dan Williams <dcbw@redhat.com>
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License along
 * with this program; if not, write to the Free Software Foundation, Inc.,
 * 51 Franklin Street, Fifth Floor, Boston, MA 02110-1301 USA.
 *
 * Copyright (C) 2005 - 2008 Red Hat, Inc.
 */

#ifdef HAVE_CONFIG_H
#include <config.h>
#endif

#include <stdio.h>
#include <glib/gi18n.h>
#include <string.h>

#include <nm-utils.h>
#include "ap-menu-item.h"
#include "nm-access-point.h"
#include "utils.h"


G_DEFINE_TYPE (NMNetworkMenuItem, nm_network_menu_item, GTK_TYPE_IMAGE_MENU_ITEM);

static void
nm_network_menu_item_init (NMNetworkMenuItem * item)
{
	item->hbox = gtk_hbox_new (FALSE, 6);
	item->ssid = gtk_label_new (NULL);
	gtk_misc_set_alignment (GTK_MISC (item->ssid), 0.0, 0.5);

	item->detail = gtk_image_new ();

	gtk_container_add (GTK_CONTAINER (item), item->hbox);
	gtk_box_pack_start (GTK_BOX (item->hbox), item->ssid, TRUE, TRUE, 0);
	gtk_box_pack_start (GTK_BOX (item->hbox), item->detail, FALSE, FALSE, 0);

	item->strength = gtk_image_new ();
	gtk_box_pack_end (GTK_BOX (item->hbox), item->strength, FALSE, TRUE, 0);

	gtk_widget_show (item->ssid);
	gtk_widget_show (item->strength);
	gtk_widget_show (item->detail);
	gtk_widget_show (item->hbox);
}

GtkWidget*
nm_network_menu_item_new (guchar *hash,
                          guint32 hash_len,
                          gboolean has_connections)
{
	NMNetworkMenuItem * item;

	item = g_object_new (NM_TYPE_NETWORK_MENU_ITEM, NULL);
	if (item == NULL)
		return NULL;

	item->has_connections = has_connections;

	if (hash && hash_len) {
		item->hash = g_malloc0 (hash_len);
		memcpy (item->hash, hash, hash_len);
		item->hash_len = hash_len;
	}

	return GTK_WIDGET (item);
}

static void
nm_network_menu_item_class_dispose (GObject *object)
{
	NMNetworkMenuItem * item = NM_NETWORK_MENU_ITEM (object);

	if (item->destroyed) {
		G_OBJECT_CLASS (nm_network_menu_item_parent_class)->dispose (object);
		return;
	}

	gtk_widget_destroy (item->ssid);
	gtk_widget_destroy (item->strength);
	gtk_widget_destroy (item->detail);
	gtk_widget_destroy (item->hbox);

	item->destroyed = TRUE;
	g_free (item->hash);

	g_slist_foreach (item->dupes, (GFunc) g_free, NULL);
	g_slist_free (item->dupes);

	G_OBJECT_CLASS (nm_network_menu_item_parent_class)->dispose (object);
}

static void
nm_network_menu_item_class_init (NMNetworkMenuItemClass * klass)
{
	GObjectClass *object_class = G_OBJECT_CLASS (klass);

	/* virtual methods */
	object_class->dispose = nm_network_menu_item_class_dispose;
}

void
nm_network_menu_item_set_ssid (NMNetworkMenuItem *item, GByteArray *ssid)
{
	g_return_if_fail (item != NULL);
	g_return_if_fail (ssid != NULL);

	g_free (item->ssid_string);

	item->ssid_string = nm_utils_ssid_to_utf8 ((const char *) ssid->data, ssid->len);
	if (!item->ssid_string) {
		// FIXME: shouldn't happen; always coerce the SSID to _something_
		item->ssid_string = g_strdup ("<unknown>");
	}
	gtk_label_set_text (GTK_LABEL (item->ssid), item->ssid_string);
}

const char *
nm_network_menu_item_get_ssid (NMNetworkMenuItem *item)
{
	return item->ssid_string;
}

guint32
nm_network_menu_item_get_strength (NMNetworkMenuItem * item)
{
	g_return_val_if_fail (item != NULL, 0);

	return item->int_strength;
}

void
nm_network_menu_item_best_strength (NMNetworkMenuItem * item,
                                    guint8 strength,
                                    NMApplet *applet)
{
	GdkPixbuf *pixbuf = NULL;

	g_return_if_fail (item != NULL);

	strength = CLAMP (strength, 0, 100);

	/* Just do nothing if the new strength is less */
	if (strength < item->int_strength)
		return;

	item->int_strength = strength;

	if (strength > 80)
		pixbuf = gdk_pixbuf_copy (applet->wireless_100_icon);
	else if (strength > 55)
		pixbuf = gdk_pixbuf_copy (applet->wireless_75_icon);
	else if (strength > 30)
		pixbuf = gdk_pixbuf_copy (applet->wireless_50_icon);
	else if (strength > 5)
		pixbuf = gdk_pixbuf_copy (applet->wireless_25_icon);
	else
		pixbuf = gdk_pixbuf_copy (applet->wireless_00_icon);

	if (item->is_encrypted) {
		GdkPixbuf *top = applet->secure_lock_icon;

		gdk_pixbuf_composite (top, pixbuf, 0, 0, gdk_pixbuf_get_width (top),
							  gdk_pixbuf_get_height (top),
							  0, 0, 1.0, 1.0,
							  GDK_INTERP_NEAREST, 255);
	}

	gtk_image_set_from_pixbuf (GTK_IMAGE (item->strength), pixbuf);
	g_object_unref (pixbuf);
}

const guchar *
nm_network_menu_item_get_hash (NMNetworkMenuItem * item,
                               guint32 * length)
{
	g_return_val_if_fail (item != NULL, NULL);
	g_return_val_if_fail (length != NULL, NULL);

	*length = item->hash_len;
	return item->hash;
}

void
nm_network_menu_item_set_detail (NMNetworkMenuItem *item,
                                 NMAccessPoint *ap,
                                 GdkPixbuf *adhoc_icon,
                                 guint32 dev_caps)
{
	gboolean is_adhoc = FALSE;
	guint32 ap_flags, ap_wpa, ap_rsn;

	ap_flags = nm_access_point_get_flags (ap);
	ap_wpa = nm_access_point_get_wpa_flags (ap);
	ap_rsn = nm_access_point_get_rsn_flags (ap);

	if ((ap_flags & NM_802_11_AP_FLAGS_PRIVACY) || ap_wpa || ap_rsn)
		item->is_encrypted = TRUE;

	if (nm_access_point_get_mode (ap) == NM_802_11_MODE_ADHOC) {
		item->is_adhoc = is_adhoc = TRUE;
		gtk_image_set_from_pixbuf (GTK_IMAGE (item->detail), adhoc_icon);
	} else
		gtk_image_set_from_stock (GTK_IMAGE (item->detail), NULL, GTK_ICON_SIZE_MENU);

	/* Don't enable the menu item the device can't even connect to the AP */
	if (   !nm_utils_security_valid (NMU_SEC_NONE, dev_caps, TRUE, is_adhoc, ap_flags, ap_wpa, ap_rsn)
        && !nm_utils_security_valid (NMU_SEC_STATIC_WEP, dev_caps, TRUE, is_adhoc, ap_flags, ap_wpa, ap_rsn)
	    && !nm_utils_security_valid (NMU_SEC_LEAP, dev_caps, TRUE, is_adhoc, ap_flags, ap_wpa, ap_rsn)
	    && !nm_utils_security_valid (NMU_SEC_DYNAMIC_WEP, dev_caps, TRUE, is_adhoc, ap_flags, ap_wpa, ap_rsn)
	    && !nm_utils_security_valid (NMU_SEC_WPA_PSK, dev_caps, TRUE, is_adhoc, ap_flags, ap_wpa, ap_rsn)
	    && !nm_utils_security_valid (NMU_SEC_WPA2_PSK, dev_caps, TRUE, is_adhoc, ap_flags, ap_wpa, ap_rsn)
	    && !nm_utils_security_valid (NMU_SEC_WPA_ENTERPRISE, dev_caps, TRUE, is_adhoc, ap_flags, ap_wpa, ap_rsn)
	    && !nm_utils_security_valid (NMU_SEC_WPA2_ENTERPRISE, dev_caps, TRUE, is_adhoc, ap_flags, ap_wpa, ap_rsn)) {
		gtk_widget_set_sensitive (GTK_WIDGET (item), FALSE);
	}
}

gboolean
nm_network_menu_item_find_dupe (NMNetworkMenuItem *item, NMAccessPoint *ap)
{
	const char *path;
	GSList *iter;

	g_return_val_if_fail (NM_IS_NETWORK_MENU_ITEM (item), FALSE);
	g_return_val_if_fail (NM_IS_ACCESS_POINT (ap), FALSE);

	path = nm_object_get_path (NM_OBJECT (ap));
	for (iter = item->dupes; iter; iter = g_slist_next (iter)) {
		if (!strcmp (path, iter->data))
			return TRUE;
	}
	return FALSE;
}

void
nm_network_menu_item_set_active (NMNetworkMenuItem *item, gboolean active)
{
	char *markup;

	gtk_label_set_use_markup (GTK_LABEL (item->ssid), active);
	if (active) {
		markup = g_markup_printf_escaped ("<b>%s</b>", item->ssid_string);
		gtk_label_set_markup (GTK_LABEL (item->ssid), markup);
		g_free (markup);
	} else
		gtk_label_set_text (GTK_LABEL (item->ssid), item->ssid_string);
}

void
nm_network_menu_item_add_dupe (NMNetworkMenuItem *item, NMAccessPoint *ap)
{
	const char *path;

	g_return_if_fail (NM_IS_NETWORK_MENU_ITEM (item));
	g_return_if_fail (NM_IS_ACCESS_POINT (ap));

	path = nm_object_get_path (NM_OBJECT (ap));
	item->dupes = g_slist_prepend (item->dupes, g_strdup (path));
}

gboolean
nm_network_menu_item_get_has_connections (NMNetworkMenuItem *item)
{
	return item->has_connections;
}

gboolean
nm_network_menu_item_get_is_adhoc (NMNetworkMenuItem *item)
{
	return item->is_adhoc;
}

gboolean
nm_network_menu_item_get_is_encrypted (NMNetworkMenuItem *item)
{
	return item->is_encrypted;
}

