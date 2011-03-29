/* -*- Mode: C; tab-width: 4; indent-tabs-mode: t; c-basic-offset: 4 -*- */
/* NetworkManager Wireless Applet -- Display wireless access points and allow user control
 *
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
 * (C) Copyright 2008 - 2011 Red Hat, Inc.
 * (C) Copyright 2008 Novell, Inc.
 */

#ifdef HAVE_CONFIG_H
#include <config.h>
#endif

#include <glib/gi18n.h>
#include <gtk/gtk.h>

#include <nm-device.h>
#include <nm-setting-connection.h>
#include <nm-setting-wired.h>
#include <nm-setting-8021x.h>
#include <nm-setting-pppoe.h>
#include <nm-device-ethernet.h>
#include <nm-utils.h>

#include "applet.h"
#include "applet-device-wired.h"
#include "wired-dialog.h"
#include "utils.h"

typedef struct {
	NMApplet *applet;
	NMDevice *device;
	NMConnection *connection;
} WiredMenuItemInfo;

static void
wired_menu_item_info_destroy (gpointer data)
{
	WiredMenuItemInfo *info = (WiredMenuItemInfo *) data;

	g_object_unref (G_OBJECT (info->device));
	if (info->connection)
		g_object_unref (G_OBJECT (info->connection));

	g_slice_free (WiredMenuItemInfo, data);
}

#define DEFAULT_WIRED_NAME _("Auto Ethernet")

static gboolean
wired_new_auto_connection (NMDevice *device,
                           gpointer dclass_data,
                           AppletNewAutoConnectionCallback callback,
                           gpointer callback_data)
{
	NMConnection *connection;
	NMSettingWired *s_wired = NULL;
	NMSettingConnection *s_con;
	char *uuid;

	connection = nm_connection_new ();

	s_wired = NM_SETTING_WIRED (nm_setting_wired_new ());
	nm_connection_add_setting (connection, NM_SETTING (s_wired));

	s_con = NM_SETTING_CONNECTION (nm_setting_connection_new ());
	uuid = nm_utils_uuid_generate ();
	g_object_set (s_con,
				  NM_SETTING_CONNECTION_ID, DEFAULT_WIRED_NAME,
				  NM_SETTING_CONNECTION_TYPE, nm_setting_get_name (NM_SETTING (s_wired)),
				  NM_SETTING_CONNECTION_AUTOCONNECT, TRUE,
				  NM_SETTING_CONNECTION_UUID, uuid,
				  NULL);
	g_free (uuid);

	nm_connection_add_setting (connection, NM_SETTING (s_con));

	(*callback) (connection, TRUE, FALSE, callback_data);
	return TRUE;
}

static void
wired_menu_item_activate (GtkMenuItem *item, gpointer user_data)
{
	WiredMenuItemInfo *info = (WiredMenuItemInfo *) user_data;

	applet_menu_item_activate_helper (info->device,
	                                  info->connection,
	                                  "/",
	                                  info->applet,
	                                  user_data);
}


typedef enum {
	ADD_ACTIVE = 1,
	ADD_INACTIVE = 2,
} AddActiveInactiveEnum;

static void
add_connection_items (NMDevice *device,
                      GSList *connections,
                      gboolean carrier,
                      NMConnection *active,
                      AddActiveInactiveEnum flag,
                      GtkWidget *menu,
                      NMApplet *applet)
{
	GSList *iter;
	WiredMenuItemInfo *info;

	for (iter = connections; iter; iter = g_slist_next (iter)) {
		NMConnection *connection = NM_CONNECTION (iter->data);
		GtkWidget *item;

		if (active == connection) {
			if ((flag & ADD_ACTIVE) == 0)
				continue;
		} else {
			if ((flag & ADD_INACTIVE) == 0)
				continue;
		}

		item = applet_new_menu_item_helper (connection, active, (flag & ADD_ACTIVE));
		gtk_widget_set_sensitive (item, carrier);

		info = g_slice_new0 (WiredMenuItemInfo);
		info->applet = applet;
		info->device = g_object_ref (G_OBJECT (device));
		info->connection = g_object_ref (connection);

		g_signal_connect_data (item, "activate",
		                       G_CALLBACK (wired_menu_item_activate),
		                       info,
		                       (GClosureNotify) wired_menu_item_info_destroy, 0);

		gtk_menu_shell_append (GTK_MENU_SHELL (menu), item);
	}
}

static void
add_default_connection_item (NMDevice *device,
                             gboolean carrier,
                             GtkWidget *menu,
                             NMApplet *applet)
{
	WiredMenuItemInfo *info;
	GtkWidget *item;
	
	item = gtk_check_menu_item_new_with_label (DEFAULT_WIRED_NAME);
	gtk_widget_set_sensitive (GTK_WIDGET (item), carrier);
	gtk_check_menu_item_set_draw_as_radio (GTK_CHECK_MENU_ITEM (item), TRUE);

	info = g_slice_new0 (WiredMenuItemInfo);
	info->applet = applet;
	info->device = g_object_ref (G_OBJECT (device));

	g_signal_connect_data (item, "activate",
	                       G_CALLBACK (wired_menu_item_activate),
	                       info,
	                       (GClosureNotify) wired_menu_item_info_destroy, 0);

	gtk_menu_shell_append (GTK_MENU_SHELL (menu), item);
}

static void
wired_add_menu_item (NMDevice *device,
                     guint32 n_devices,
                     NMConnection *active,
                     GtkWidget *menu,
                     NMApplet *applet)
{
	char *text;
	GtkWidget *item;
	GSList *connections, *all;
	gboolean carrier = TRUE;

	all = applet_get_all_connections (applet);
	connections = utils_filter_connections_for_device (device, all);
	g_slist_free (all);

	if (n_devices > 1) {
		char *desc = NULL;

		desc = (char *) utils_get_device_description (device);
		if (!desc)
			desc = (char *) nm_device_get_iface (device);
		g_assert (desc);

		if (g_slist_length (connections) > 1)
			text = g_strdup_printf (_("Wired Networks (%s)"), desc);
		else
			text = g_strdup_printf (_("Wired Network (%s)"), desc);
	} else {
		if (g_slist_length (connections) > 1)
			text = g_strdup (_("Wired Networks"));
		else
			text = g_strdup (_("Wired Network"));
	}

	item = applet_menu_item_create_device_item_helper (device, applet, text);
	g_free (text);

	/* Only dim the item if the device supports carrier detection AND
	 * we know it doesn't have a link.
	 */
 	if (nm_device_get_capabilities (device) & NM_DEVICE_CAP_CARRIER_DETECT)
		carrier = nm_device_ethernet_get_carrier (NM_DEVICE_ETHERNET (device));

	gtk_widget_set_sensitive (item, FALSE);
	gtk_menu_shell_append (GTK_MENU_SHELL (menu), item);
	gtk_widget_show (item);

	if (g_slist_length (connections))
		add_connection_items (device, connections, carrier, active, ADD_ACTIVE, menu, applet);

	/* Notify user of unmanaged or unavailable device */
	item = nma_menu_device_get_menu_item (device, applet, carrier ? NULL : _("disconnected"));
	if (item) {
		gtk_menu_shell_append (GTK_MENU_SHELL (menu), item);
		gtk_widget_show (item);
	}

	if (!nma_menu_device_check_unusable (device)) {
		if ((!active && g_slist_length (connections)) || (active && g_slist_length (connections) > 1))
			applet_menu_item_add_complex_separator_helper (menu, applet, _("Available"), -1);

		if (g_slist_length (connections))
			add_connection_items (device, connections, carrier, active, ADD_INACTIVE, menu, applet);
		else
			add_default_connection_item (device, carrier, menu, applet);
	}

	g_slist_free (connections);
}

static void
wired_device_state_changed (NMDevice *device,
                            NMDeviceState new_state,
                            NMDeviceState old_state,
                            NMDeviceStateReason reason,
                            NMApplet *applet)
{
	if (new_state == NM_DEVICE_STATE_ACTIVATED) {
		NMConnection *connection;
		NMSettingConnection *s_con = NULL;
		char *str = NULL;

		connection = applet_find_active_connection_for_device (device, applet, NULL);
		if (connection) {
			const char *id;
			s_con = NM_SETTING_CONNECTION (nm_connection_get_setting (connection, NM_TYPE_SETTING_CONNECTION));
			id = s_con ? nm_setting_connection_get_id (s_con) : NULL;
			if (id)
				str = g_strdup_printf (_("You are now connected to '%s'."), id);
		}

		applet_do_notify_with_pref (applet,
		                            _("Connection Established"),
		                            str ? str : _("You are now connected to the wired network."),
		                            "nm-device-wired",
		                            PREF_DISABLE_CONNECTED_NOTIFICATIONS);
		g_free (str);
	}
}

static GdkPixbuf *
wired_get_icon (NMDevice *device,
                NMDeviceState state,
                NMConnection *connection,
                char **tip,
                NMApplet *applet)
{
	NMSettingConnection *s_con;
	GdkPixbuf *pixbuf = NULL;
	const char *id;

	id = nm_device_get_iface (NM_DEVICE (device));
	if (connection) {
		s_con = NM_SETTING_CONNECTION (nm_connection_get_setting (connection, NM_TYPE_SETTING_CONNECTION));
		id = nm_setting_connection_get_id (s_con);
	}

	switch (state) {
	case NM_DEVICE_STATE_PREPARE:
		*tip = g_strdup_printf (_("Preparing wired network connection '%s'..."), id);
		break;
	case NM_DEVICE_STATE_CONFIG:
		*tip = g_strdup_printf (_("Configuring wired network connection '%s'..."), id);
		break;
	case NM_DEVICE_STATE_NEED_AUTH:
		*tip = g_strdup_printf (_("User authentication required for wired network connection '%s'..."), id);
		break;
	case NM_DEVICE_STATE_IP_CONFIG:
		*tip = g_strdup_printf (_("Requesting a wired network address for '%s'..."), id);
		break;
	case NM_DEVICE_STATE_ACTIVATED:
		pixbuf = nma_icon_check_and_load ("nm-device-wired", &applet->wired_icon, applet);
		*tip = g_strdup_printf (_("Wired network connection '%s' active"), id);
		break;
	default:
		break;
	}

	return pixbuf;
}

/* PPPoE */

typedef struct {
	SecretsRequest req;

	GtkWidget *dialog;
	GtkEntry *username_entry;
	GtkEntry *service_entry;
	GtkEntry *password_entry;
	GtkWidget *ok_button;
} NMPppoeInfo;

static void
pppoe_verify (GtkEditable *editable, gpointer user_data)
{
	NMPppoeInfo *info = (NMPppoeInfo *) user_data;
	const char *s;
	gboolean valid = TRUE;

	s = gtk_entry_get_text (info->username_entry);
	if (!s || strlen (s) < 1)
		valid = FALSE;

	if (valid) {
		s = gtk_entry_get_text (info->password_entry);
		if (!s || strlen (s) < 1)
			valid = FALSE;
	}

	gtk_widget_set_sensitive (info->ok_button, valid);
}

static void
pppoe_update_setting (NMSettingPPPOE *pppoe, NMPppoeInfo *info)
{
	const char *s;

	s = gtk_entry_get_text (info->service_entry);
	if (s && strlen (s) < 1)
		s = NULL;

	g_object_set (pppoe,
				  NM_SETTING_PPPOE_USERNAME, gtk_entry_get_text (info->username_entry),
				  NM_SETTING_PPPOE_PASSWORD, gtk_entry_get_text (info->password_entry),
				  NM_SETTING_PPPOE_SERVICE, s,
				  NULL);
}

static void
pppoe_update_ui (NMConnection *connection, NMPppoeInfo *info)
{
	NMSettingPPPOE *s_pppoe;
	const char *s;

	g_return_if_fail (NM_IS_CONNECTION (connection));
	g_return_if_fail (info != NULL);

	s_pppoe = (NMSettingPPPOE *) nm_connection_get_setting (connection, NM_TYPE_SETTING_PPPOE);
	g_return_if_fail (s_pppoe != NULL);

	s = nm_setting_pppoe_get_username (s_pppoe);
	if (s)
		gtk_entry_set_text (info->username_entry, s);

	s = nm_setting_pppoe_get_service (s_pppoe);
	if (s)
		gtk_entry_set_text (info->service_entry, s);

	s = nm_setting_pppoe_get_password (s_pppoe);
	if (s)
		gtk_entry_set_text (info->password_entry, s);
}

static void
free_pppoe_info (SecretsRequest *req)
{
	NMPppoeInfo *info = (NMPppoeInfo *) req;

	if (info->dialog) {
		gtk_widget_hide (info->dialog);
		gtk_widget_destroy (info->dialog);
	}
}

static void
get_pppoe_secrets_cb (GtkDialog *dialog, gint response, gpointer user_data)
{
	SecretsRequest *req = user_data;
	NMPppoeInfo *info = (NMPppoeInfo *) req;
	NMSetting *setting;
	GHashTable *settings = NULL;
	GHashTable *secrets;
	GError *error = NULL;

	if (response != GTK_RESPONSE_OK) {
		g_set_error (&error,
		             NM_SECRET_AGENT_ERROR,
		             NM_SECRET_AGENT_ERROR_USER_CANCELED,
		             "%s.%d (%s): canceled",
		             __FILE__, __LINE__, __func__);
		goto done;
	}

	setting = nm_connection_get_setting (req->connection, NM_TYPE_SETTING_PPPOE);
	pppoe_update_setting (NM_SETTING_PPPOE (setting), info);

	secrets = nm_setting_to_hash (setting, NM_SETTING_HASH_FLAG_ONLY_SECRETS);
	if (!secrets) {
		g_set_error (&error,
		             NM_SECRET_AGENT_ERROR,
		             NM_SECRET_AGENT_ERROR_INTERNAL_ERROR,
					 "%s.%d (%s): failed to hash setting " NM_SETTING_PPPOE_SETTING_NAME,
					 __FILE__, __LINE__, __func__);
	} else {
		/* Returned secrets are a{sa{sv}}; this is the outer a{s...} hash that
		 * will contain all the individual settings hashes.
		 */
		settings = g_hash_table_new_full (g_str_hash, g_str_equal, NULL, (GDestroyNotify) g_hash_table_destroy);
		g_hash_table_insert (settings, NM_SETTING_PPPOE_SETTING_NAME, secrets);
	}

done:
	applet_secrets_request_complete (req, settings, error);
	applet_secrets_request_free (req);

	if (settings)
		g_hash_table_destroy (settings);
}

static void
show_password_toggled (GtkToggleButton *button, gpointer user_data)
{
	NMPppoeInfo *info = (NMPppoeInfo *) user_data;

	if (gtk_toggle_button_get_active (button))
		gtk_entry_set_visibility (GTK_ENTRY (info->password_entry), TRUE);
	else
		gtk_entry_set_visibility (GTK_ENTRY (info->password_entry), FALSE);
}

static gboolean
pppoe_get_secrets (SecretsRequest *req, GError **error)
{
	NMPppoeInfo *info = (NMPppoeInfo *) req;
	GtkWidget *w;
	GtkBuilder* builder;
	GError *tmp_error = NULL;

	builder = gtk_builder_new ();

	if (!gtk_builder_add_from_file (builder, UIDIR "/ce-page-dsl.ui", &tmp_error)) {
		g_set_error (error,
		             NM_SECRET_AGENT_ERROR,
		             NM_SECRET_AGENT_ERROR_INTERNAL_ERROR,
					 "%s.%d (%s): couldn't display secrets UI: %s",
		             __FILE__, __LINE__, __func__, tmp_error->message);
		g_error_free (tmp_error);
		return FALSE;
	}

	applet_secrets_request_set_free_func (req, free_pppoe_info);

	info->username_entry = GTK_ENTRY (gtk_builder_get_object (builder, "dsl_username"));
	g_signal_connect (info->username_entry, "changed", G_CALLBACK (pppoe_verify), info);

	info->service_entry = GTK_ENTRY (gtk_builder_get_object (builder, "dsl_service"));

	info->password_entry = GTK_ENTRY (gtk_builder_get_object (builder, "dsl_password"));
	g_signal_connect (info->password_entry, "changed", G_CALLBACK (pppoe_verify), info);

	/* Create the dialog */
	info->dialog = gtk_dialog_new ();
	gtk_window_set_title (GTK_WINDOW (info->dialog), _("DSL authentication"));
	gtk_window_set_modal (GTK_WINDOW (info->dialog), TRUE);

	w = gtk_dialog_add_button (GTK_DIALOG (info->dialog), GTK_STOCK_CANCEL, GTK_RESPONSE_REJECT);
	w = gtk_dialog_add_button (GTK_DIALOG (info->dialog), GTK_STOCK_OK, GTK_RESPONSE_OK);
	info->ok_button = w;

	gtk_box_pack_start (GTK_BOX (gtk_dialog_get_content_area (GTK_DIALOG (info->dialog))),
	                    GTK_WIDGET (gtk_builder_get_object (builder, "DslPage")),
	                    TRUE, TRUE, 0);

	pppoe_update_ui (req->connection, info);

	w = GTK_WIDGET (gtk_builder_get_object (builder, "dsl_show_password"));
	g_signal_connect (w, "toggled", G_CALLBACK (show_password_toggled), info);

	g_signal_connect (info->dialog, "response", G_CALLBACK (get_pppoe_secrets_cb), info);

	gtk_window_set_position (GTK_WINDOW (info->dialog), GTK_WIN_POS_CENTER_ALWAYS);
	gtk_widget_realize (info->dialog);
	gtk_window_present (GTK_WINDOW (info->dialog));

	return TRUE;
}

/* 802.1x */

typedef struct {
	SecretsRequest req;
	GtkWidget *dialog;
} NM8021xInfo;

static void
free_8021x_info (SecretsRequest *req)
{
	NM8021xInfo *info = (NM8021xInfo *) req;

	if (info->dialog) {
		gtk_widget_hide (info->dialog);
		gtk_widget_destroy (info->dialog);
	}
}

static void
get_8021x_secrets_cb (GtkDialog *dialog, gint response, gpointer user_data)
{
	SecretsRequest *req = user_data;
	NM8021xInfo *info = (NM8021xInfo *) req;
	NMConnection *connection = NULL;
	NMSetting *setting;
	GError *error = NULL;

	if (response != GTK_RESPONSE_OK) {
		g_set_error (&error,
		             NM_SECRET_AGENT_ERROR,
		             NM_SECRET_AGENT_ERROR_USER_CANCELED,
		             "%s.%d (%s): canceled",
		             __FILE__, __LINE__, __func__);
		goto done;
	}

	connection = nma_wired_dialog_get_connection (info->dialog);
	if (!connection) {
		g_set_error (&error,
		             NM_SECRET_AGENT_ERROR,
		             NM_SECRET_AGENT_ERROR_INTERNAL_ERROR,
		             "%s.%d (%s): couldn't get connection from wired dialog.",
		             __FILE__, __LINE__, __func__);
		goto done;
	}

	setting = nm_connection_get_setting (connection, NM_TYPE_SETTING_802_1X);
	if (setting) {
		nm_connection_add_setting (req->connection, g_object_ref (setting));
	} else {
		g_set_error (&error,
		             NM_SECRET_AGENT_ERROR,
		             NM_SECRET_AGENT_ERROR_INTERNAL_ERROR,
					 "%s.%d (%s): requested setting '802-1x' didn't"
					 " exist in the connection.",
					 __FILE__, __LINE__, __func__);
	}

done:
	applet_secrets_request_complete_setting (req, NM_SETTING_802_1X_SETTING_NAME, error);
	applet_secrets_request_free (req);
	g_clear_error (&error);
}

static gboolean
nm_8021x_get_secrets (SecretsRequest *req, GError **error)
{
	NM8021xInfo *info = (NM8021xInfo *) req;

	applet_secrets_request_set_free_func (req, free_8021x_info);

	info->dialog = nma_wired_dialog_new (g_object_ref (req->connection));
	if (!info->dialog) {
		g_set_error (error,
		             NM_SECRET_AGENT_ERROR,
		             NM_SECRET_AGENT_ERROR_INTERNAL_ERROR,
		             "%s.%d (%s): couldn't display secrets UI",
		             __FILE__, __LINE__, __func__);
		return FALSE;
	}

	g_signal_connect (info->dialog, "response", G_CALLBACK (get_8021x_secrets_cb), info);

	gtk_window_set_position (GTK_WINDOW (info->dialog), GTK_WIN_POS_CENTER_ALWAYS);
	gtk_widget_realize (info->dialog);
	gtk_window_present (GTK_WINDOW (info->dialog));

	return TRUE;
}

static gboolean
wired_get_secrets (SecretsRequest *req, GError **error)
{
	NMSettingConnection *s_con;
	const char *ctype;

	s_con = NM_SETTING_CONNECTION (nm_connection_get_setting (req->connection, NM_TYPE_SETTING_CONNECTION));
	if (!s_con) {
		g_set_error (error,
		             NM_SECRET_AGENT_ERROR,
		             NM_SECRET_AGENT_ERROR_INVALID_CONNECTION,
		             "%s.%d (%s): Invalid connection",
		             __FILE__, __LINE__, __func__);
		return FALSE;
	}

	ctype = nm_setting_connection_get_connection_type (s_con);
	if (!strcmp (ctype, NM_SETTING_WIRED_SETTING_NAME))
		return nm_8021x_get_secrets (req, error);
	else if (!strcmp (ctype, NM_SETTING_PPPOE_SETTING_NAME))
		return pppoe_get_secrets (req, error);
	else {
		g_set_error (error,
		             NM_SECRET_AGENT_ERROR,
		             NM_SECRET_AGENT_ERROR_INTERNAL_ERROR,
		             "%s.%d (%s): unhandled wired connection type '%s'",
		             __FILE__, __LINE__, __func__, ctype);
	}

	return FALSE;
}

NMADeviceClass *
applet_device_wired_get_class (NMApplet *applet)
{
	NMADeviceClass *dclass;

	dclass = g_slice_new0 (NMADeviceClass);
	if (!dclass)
		return NULL;

	dclass->new_auto_connection = wired_new_auto_connection;
	dclass->add_menu_item = wired_add_menu_item;
	dclass->device_state_changed = wired_device_state_changed;
	dclass->get_icon = wired_get_icon;
	dclass->get_secrets = wired_get_secrets;
	dclass->secrets_request_size = MAX (sizeof (NM8021xInfo), sizeof (NMPppoeInfo));

	return dclass;
}
