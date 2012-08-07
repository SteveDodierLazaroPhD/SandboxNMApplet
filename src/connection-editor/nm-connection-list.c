/* -*- Mode: C; tab-width: 4; indent-tabs-mode: t; c-basic-offset: 4 -*- */
/* NetworkManager Connection editor -- Connection editor for NetworkManager
 *
 * Rodrigo Moya <rodrigo@gnome-db.org>
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
 * (C) Copyright 2007 - 2012 Red Hat, Inc.
 */

#include <config.h>
#include <string.h>
#include <sys/types.h>
#include <unistd.h>

#include <gtk/gtk.h>
#include <gdk/gdkx.h>
#include <glib/gi18n.h>

#include <nm-setting-connection.h>
#include <nm-connection.h>
#include <nm-setting.h>
#include <nm-setting-connection.h>
#include <nm-setting-wired.h>
#include <nm-setting-wireless.h>
#include <nm-setting-vpn.h>
#include <nm-setting-gsm.h>
#include <nm-setting-cdma.h>
#include <nm-setting-pppoe.h>
#include <nm-setting-ppp.h>
#include <nm-setting-serial.h>
#include <nm-setting-wimax.h>
#include <nm-utils.h>
#include <nm-remote-settings.h>

#include "ce-page.h"
#include "page-wired.h"
#include "page-wireless.h"
#include "page-mobile.h"
#include "page-dsl.h"
#include "page-vpn.h"
#include "page-wimax.h"
#include "nm-connection-editor.h"
#include "nm-connection-list.h"
#include "vpn-helpers.h"
#include "ce-polkit-button.h"
#include "new-connection.h"

G_DEFINE_TYPE (NMConnectionList, nm_connection_list, G_TYPE_OBJECT)

enum {
	LIST_DONE,
	EDITING_DONE,
	LIST_LAST_SIGNAL
};

static guint list_signals[LIST_LAST_SIGNAL] = { 0 };

#define COL_ICON       0
#define COL_ID         1
#define COL_LAST_USED  2
#define COL_TIMESTAMP  3
#define COL_CONNECTION 4
#define COL_GTYPE      5
#define COL_ORDER      6

static void
error_dialog (GtkWindow *parent, const char *heading, const char *format, ...)
{
	GtkWidget *dialog;
	va_list args;
	char *message;

	dialog = gtk_message_dialog_new (parent,
	                                 GTK_DIALOG_DESTROY_WITH_PARENT,
	                                 GTK_MESSAGE_ERROR,
	                                 GTK_BUTTONS_CLOSE,
	                                 "%s", heading);

	va_start (args, format);
	message = g_strdup_vprintf (format, args);
	va_end (args);

	gtk_message_dialog_format_secondary_text (GTK_MESSAGE_DIALOG (dialog), "%s", message);
	g_free (message);

	gtk_widget_show_all (dialog);
	gtk_window_present (GTK_WINDOW (dialog));
	gtk_dialog_run (GTK_DIALOG (dialog));
	gtk_widget_destroy (dialog);
}

static NMRemoteConnection *
get_active_connection (GtkTreeView *treeview)
{
	GtkTreeSelection *selection;
	GList *selected_rows;
	GtkTreeModel *model = NULL;
	GtkTreeIter iter;
	NMRemoteConnection *connection = NULL;

	selection = gtk_tree_view_get_selection (treeview);
	selected_rows = gtk_tree_selection_get_selected_rows (selection, &model);
	if (!selected_rows)
		return NULL;

	if (gtk_tree_model_get_iter (model, &iter, (GtkTreePath *) selected_rows->data))
		gtk_tree_model_get (model, &iter, COL_CONNECTION, &connection, -1);

	/* free memory */
	g_list_foreach (selected_rows, (GFunc) gtk_tree_path_free, NULL);
	g_list_free (selected_rows);

	/* gtk_tree_model_get() will have reffed connection, but we don't
	 * need that since we know the model will continue to hold a ref.
	 */
	if (connection)
		g_object_unref (connection);

	return connection;
}

static gboolean
get_iter_for_connection (NMConnectionList *list,
                         NMRemoteConnection *connection,
                         GtkTreeIter *iter)
{
	GtkTreeIter types_iter;

	if (!gtk_tree_model_get_iter_first (list->model, &types_iter))
		return FALSE;

	do {
		if (!gtk_tree_model_iter_children (list->model, iter, &types_iter))
			continue;

		do {
			NMRemoteConnection *candidate = NULL;

			gtk_tree_model_get (list->model, iter,
			                    COL_CONNECTION, &candidate,
			                    -1);
			if (candidate == connection) {
				g_object_unref (candidate);
				return TRUE;
			}
			g_object_unref (candidate);
		} while (gtk_tree_model_iter_next (list->model, iter));
	} while (gtk_tree_model_iter_next (list->model, &types_iter));

	return FALSE;
}

static char *
format_last_used (guint64 timestamp)
{
	GTimeVal now_tv;
	GDate *now, *last;
	char *last_used = NULL;

	if (!timestamp)
		return g_strdup (_("never"));

	g_get_current_time (&now_tv);
	now = g_date_new ();
	g_date_set_time_val (now, &now_tv);

	last = g_date_new ();
	g_date_set_time_t (last, (time_t) timestamp);

	/* timestamp is now or in the future */
	if (now_tv.tv_sec <= timestamp) {
		last_used = g_strdup (_("now"));
		goto out;
	}

	if (g_date_compare (now, last) <= 0) {
		guint minutes, hours;

		/* Same day */

		minutes = (now_tv.tv_sec - timestamp) / 60;
		if (minutes == 0) {
			last_used = g_strdup (_("now"));
			goto out;
		}

		hours = (now_tv.tv_sec - timestamp) / 3600;
		if (hours == 0) {
			/* less than an hour ago */
			last_used = g_strdup_printf (ngettext ("%d minute ago", "%d minutes ago", minutes), minutes);
			goto out;
		}

		last_used = g_strdup_printf (ngettext ("%d hour ago", "%d hours ago", hours), hours);
	} else {
		guint days, months, years;

		days = g_date_get_julian (now) - g_date_get_julian (last);
		if (days == 0) {
			last_used = g_strdup ("today");
			goto out;
		}

		months = days / 30;
		if (months == 0) {
			last_used = g_strdup_printf (ngettext ("%d day ago", "%d days ago", days), days);
			goto out;
		}

		years = days / 365;
		if (years == 0) {
			last_used = g_strdup_printf (ngettext ("%d month ago", "%d months ago", months), months);
			goto out;
		}

		last_used = g_strdup_printf (ngettext ("%d year ago", "%d years ago", years), years);
	}

out:
	g_date_free (now);
	g_date_free (last);
	return last_used;
}

static void
update_connection_row (NMConnectionList *self,
                       GtkTreeIter *iter,
                       NMRemoteConnection *connection)
{
	NMSettingConnection *s_con;
	char *last_used, *id;

	s_con = nm_connection_get_setting_connection (NM_CONNECTION (connection));
	g_assert (s_con);

	last_used = format_last_used (nm_setting_connection_get_timestamp (s_con));
	id = g_markup_escape_text (nm_setting_connection_get_id (s_con), -1);
	gtk_tree_store_set (GTK_TREE_STORE (self->model), iter,
	                    COL_ID, id,
	                    COL_LAST_USED, last_used,
	                    COL_TIMESTAMP, nm_setting_connection_get_timestamp (s_con),
	                    COL_CONNECTION, connection,
	                    -1);
	g_free (last_used);
	g_free (id);

	gtk_tree_model_filter_refilter (self->filter);
}


/**********************************************/
/* Connection deleting */

typedef void (*DeleteResultFunc) (NMConnectionList *list,
                                  GError *error,
                                  gpointer user_data);

typedef struct {
	NMConnectionList *list;
	NMRemoteConnection *original;
	NMConnectionEditor *editor;
	DeleteResultFunc callback;
	gpointer callback_data;
} DeleteInfo;

static void
delete_cb (NMRemoteConnection *remote,
           GError *error,
           gpointer user_data)
{
	DeleteInfo *info = user_data;

	if (info->editor)
		nm_connection_editor_set_busy (info->editor, FALSE);

	info->callback (info->list, error, info->callback_data);
	g_free (info);
}

static void
delete_connection (NMConnectionList *list,
                   NMRemoteConnection *connection,
                   DeleteResultFunc callback,
                   gpointer user_data)
{
	DeleteInfo *info;
	NMConnectionEditor *editor;

	editor = g_hash_table_lookup (list->editors, connection);

	info = g_malloc0 (sizeof (DeleteInfo));
	info->list = list;
	info->callback = callback;
	info->callback_data = user_data;
	info->editor = editor;

	if (editor)
		nm_connection_editor_set_busy (editor, TRUE);

	nm_remote_connection_delete (connection, delete_cb, info);
}

/**********************************************/
/* Connection adding */

typedef void (*AddResultFunc) (NMConnectionList *list,
                               GError *error,
                               gpointer user_data);

typedef struct {
	NMConnectionList *list;
	NMConnectionEditor *editor;
	AddResultFunc callback;
	gpointer callback_data;
} AddInfo;

static void
add_cb (NMRemoteSettings *settings,
        NMRemoteConnection *connection,
        GError *error,
        gpointer user_data)
{
	AddInfo *info = user_data;

	nm_connection_editor_set_busy (info->editor, FALSE);
	info->callback (info->list, error, info->callback_data);
	g_free (info);
}

static void
add_connection (NMConnectionList *self,
                NMConnectionEditor *editor,
                AddResultFunc callback,
                gpointer callback_data)
{
	AddInfo *info;

	info = g_malloc0 (sizeof (AddInfo));
	info->list = self;
	info->editor = editor;
	info->callback = callback;
	info->callback_data = callback_data;

	nm_connection_editor_set_busy (editor, TRUE);

	nm_remote_settings_add_connection (self->settings,
	                                   nm_connection_editor_get_connection (editor),
	                                   add_cb,
	                                   info);
}

/**********************************************/
/* Connection updating */

typedef void (*UpdateResultFunc) (NMConnectionList *list,
                                  NMRemoteConnection *connection,
                                  GError *error,
                                  gpointer user_data);

typedef struct {
	NMConnectionList *list;
	NMConnectionEditor *editor;
	NMRemoteConnection *connection;
	UpdateResultFunc callback;
	gpointer callback_data;
} UpdateInfo;

static void
update_complete (UpdateInfo *info, GError *error)
{
	info->callback (info->list, info->connection, error, info->callback_data);
	g_object_unref (info->connection);
	g_free (info);
}

static void
update_cb (NMRemoteConnection *connection, GError *error, gpointer user_data)
{
	UpdateInfo *info = user_data;

	nm_connection_editor_set_busy (info->editor, FALSE);

	/* Clear secrets so they don't lay around in memory; they'll get requested
	 * again anyway next time the connection is edited.
	 */
	nm_connection_clear_secrets (NM_CONNECTION (connection));

	update_complete (info, error);
}

static void
update_connection (NMConnectionList *list,
                   NMConnectionEditor *editor,
                   NMRemoteConnection *connection,
                   UpdateResultFunc callback,
                   gpointer user_data)
{
	UpdateInfo *info;
	GHashTable *new_settings;
	GError *error = NULL;

	info = g_malloc0 (sizeof (UpdateInfo));
	info->list = list;
	info->editor = editor;
	info->connection = g_object_ref (connection);
	info->callback = callback;
	info->callback_data = user_data;

	/* Connections need the certificates filled because the
	 * applet private values that we use to store the path to certificates
	 * and private keys don't go through D-Bus; they are private of course!
	 */
	new_settings = nm_connection_to_hash (NM_CONNECTION (connection), NM_SETTING_HASH_FLAG_ALL);
	if (!nm_connection_replace_settings (NM_CONNECTION (connection), new_settings, &error)) {
		update_complete (info, error);
		g_error_free (error);
		return;
	}

	nm_connection_editor_set_busy (editor, TRUE);
	nm_remote_connection_commit_changes (connection, update_cb, info);
}

/**********************************************/
/* dialog/UI handling stuff */

static void
add_finished_cb (NMConnectionList *list, GError *error, gpointer user_data)
{
	NMConnectionEditor *editor = NM_CONNECTION_EDITOR (user_data);
	GtkWindow *parent;

	if (error) {
		parent = nm_connection_editor_get_window (editor);
		error_dialog (parent, _("Connection add failed"), "%s", error->message);
	}

	g_hash_table_remove (list->editors, nm_connection_editor_get_connection (editor));
}


static void
add_response_cb (NMConnectionEditor *editor, gint response, GError *error, gpointer user_data)
{
	NMConnectionList *list = user_data;
	GError *add_error = NULL;

	/* if the dialog is busy waiting for authorization or something,
	 * don't destroy it until authorization returns.
	 */
	if (nm_connection_editor_get_busy (editor))
		return;

	if (response == GTK_RESPONSE_OK) {
		/* Verify and commit user changes */
		if (nm_connection_editor_update_connection (editor, &add_error)) {
			/* Yay we can try to add the connection; it'll get removed from
			 * list->editors when the add finishes.
			 */
			add_connection (list, editor, add_finished_cb, editor);
			return;
		} else {
			error_dialog (GTK_WINDOW (editor->window),
			              _("Error saving connection"),
			              _("The property '%s' / '%s' is invalid: %d"),
			              g_type_name (nm_connection_lookup_setting_type_by_quark (add_error->domain)),
			              (add_error && add_error->message) ? add_error->message : "(unknown)",
			              add_error ? add_error->code : -1);
			g_clear_error (&add_error);
		}
	} else if (response == GTK_RESPONSE_NONE) {
		const char *message = _("An unknown error occurred.");

		if (error && error->message)
			message = error->message;
		error_dialog (GTK_WINDOW (editor->window),
		              _("Error initializing editor"),
		              "%s", message);
	}

	g_hash_table_remove (list->editors, nm_connection_editor_get_connection (editor));
	g_signal_emit (list, list_signals[EDITING_DONE], 0, 0);
}


static void
really_add_connection (NMConnection *connection,
                       gboolean canceled,
                       GError *error,
                       gpointer user_data)
{
	NMConnectionList *list = user_data;
	NMConnectionEditor *editor;
	GError *editor_error = NULL;
	const char *message = _("The connection editor dialog could not be initialized due to an unknown error.");

	if (canceled) {
		g_signal_emit (list, list_signals[EDITING_DONE], 0, 0);
		return;
	}

	if (!connection) {
		error_dialog (GTK_WINDOW (list->dialog),
		              _("Could not create new connection"),
		              "%s",
		              (error && error->message) ? error->message : message);
		g_signal_emit (list, list_signals[EDITING_DONE], 0, 0);
		return;
	}

	editor = nm_connection_editor_new (connection, list->nm_client, &error);
	if (!editor) {
		g_object_unref (connection);

		error_dialog (GTK_WINDOW (list->dialog),
		              _("Could not edit new connection"),
		              "%s",
		              (editor_error && editor_error->message) ? editor_error->message : message);
		g_clear_error (&editor_error);
		g_signal_emit (list, list_signals[EDITING_DONE], 0, 0);
		return;
	}

	g_signal_connect (editor, "done", G_CALLBACK (add_response_cb), list);
	g_hash_table_insert (list->editors, connection, editor);

	nm_connection_editor_run (editor);
}

static void
add_clicked (GtkButton *button, gpointer user_data)
{
	NMConnectionList *list = user_data;

	new_connection_dialog (GTK_WINDOW (list->dialog),
	                       list->settings,
	                       NULL,
	                       really_add_connection,
	                       list);
}

typedef struct {
	NMConnectionList *list;
	NMConnectionEditor *editor;
} EditInfo;

static void
connection_updated_cb (NMConnectionList *list,
                       NMRemoteConnection *connection,
                       GError *error,
                       gpointer user_data)
{
	EditInfo *info = user_data;
	GtkTreeIter iter;

	if (error) {
		/* Log the error and do nothing.  We don't want to destroy the dialog
		 * because that's not really useful.  If there's a hard error, the user
		 * will just have to cancel.  This better handles the case where
		 * PolicyKit authentication is required, but the user accidentally gets
		 * their password wrong.  Which used to close the dialog, and that's
		 * completely unhelpful.  Instead just let them hit 'Save' again.
		 */
		g_warning ("Error updating connection '%s': (%d) %s",
		           nm_connection_get_id (NM_CONNECTION (connection)),
		           error->code,
		           error->message);
		return;
	}

	/* Success */
	if (get_iter_for_connection (list, connection, &iter))
		update_connection_row (list, &iter, connection);

	/* This callback might be triggered long after it's caller was called,
	 * if for example we've had to get PolicyKit authentication to perform
	 * the update.  So only signal we're done with editing when all that is
	 * complete.
	 */
	g_signal_emit (info->list, list_signals[EDITING_DONE], 0, 0);

	g_hash_table_remove (list->editors, connection);
	g_free (info);
}

static void
edit_done_cb (NMConnectionEditor *editor, gint response, GError *error, gpointer user_data)
{
	EditInfo *info = user_data;
	const char *message = _("An unknown error occurred.");
	NMConnection *connection;
	GError *edit_error = NULL;

	/* if the dialog is busy waiting for authorization or something,
	 * don't destroy it until authorization returns.
	 */
	if (nm_connection_editor_get_busy (editor))
		return;

	connection = nm_connection_editor_get_connection (editor);
	g_assert (connection);

	switch (response) {
	case GTK_RESPONSE_OK:
		/* Verify and commit user changes */
		if (nm_connection_editor_update_connection (editor, &edit_error)) {
			/* Save the connection to backing storage */
			update_connection (info->list,
			                   editor,
			                   NM_REMOTE_CONNECTION (connection),
			                   connection_updated_cb,
			                   info);
		} else {
			g_warning ("%s: invalid connection after update: bug in the "
			           "'%s' / '%s' invalid: %d",
			           __func__,
			           g_type_name (nm_connection_lookup_setting_type_by_quark (edit_error->domain)),
			           edit_error->message, edit_error->code);
			connection_updated_cb (info->list,
			                       NM_REMOTE_CONNECTION (connection),
			                       edit_error,
			                       NULL);
			g_error_free (edit_error);
		}
		break;
	case GTK_RESPONSE_NONE:
		/* Show an error dialog if the editor initialization failed */
		if (error && error->message)
			message = error->message;
		error_dialog (GTK_WINDOW (editor->window), _("Error initializing editor"), "%s", message);
		/* fall through */
	case GTK_RESPONSE_CANCEL:
	default:
		g_hash_table_remove (info->list->editors, connection);
		g_signal_emit (info->list, list_signals[EDITING_DONE], 0, 0);
		g_free (info);
		break;
	}
}

static void
edit_connection (NMConnectionList *list, NMConnection *connection)
{
	NMConnectionEditor *editor;
	EditInfo *edit_info;
	GError *error = NULL;
	const char *message = _("The connection editor dialog could not be initialized due to an unknown error.");

	g_return_if_fail (connection != NULL);

	/* Don't allow two editors for the same connection */
	editor = (NMConnectionEditor *) g_hash_table_lookup (list->editors, connection);
	if (editor) {
		nm_connection_editor_present (editor);
		return;
	}

	editor = nm_connection_editor_new (NM_CONNECTION (connection), list->nm_client, &error);
	if (!editor) {
		error_dialog (GTK_WINDOW (list->dialog),
		              _("Could not edit connection"),
		              "%s",
		              (error && error->message) ? error->message : message);
		return;
	}

	edit_info = g_malloc0 (sizeof (EditInfo));
	edit_info->list = list;
	edit_info->editor = editor;

	g_signal_connect (editor, "done", G_CALLBACK (edit_done_cb), edit_info);
	g_hash_table_insert (list->editors, connection, editor);

	nm_connection_editor_run (editor);
}

static void
do_edit (NMConnectionList *list)
{
	edit_connection (list, NM_CONNECTION (get_active_connection (list->connection_list)));
}

static void
delete_result_cb (NMConnectionList *list,
                  GError *error,
                  gpointer user_data)
{
	GtkWindow *parent = GTK_WINDOW (user_data);

	if (error)
		error_dialog (parent, _("Connection delete failed"), "%s", error->message);
}

static void
delete_clicked (GtkButton *button, gpointer user_data)
{
	NMConnectionList *list = user_data;
	NMRemoteConnection *connection;
	NMConnectionEditor *editor;
	NMSettingConnection *s_con;
	GtkWidget *dialog;
	const char *id;
	guint result;

	connection = get_active_connection (list->connection_list);
	g_return_if_fail (connection != NULL);

	editor = g_hash_table_lookup (list->editors, connection);
	if (editor && nm_connection_editor_get_busy (editor)) {
		/* Editor already has an operation in progress, raise it */
		nm_connection_editor_present (editor);
		return;
	}

	s_con = nm_connection_get_setting_connection (NM_CONNECTION (connection));
	g_assert (s_con);
	id = nm_setting_connection_get_id (s_con);

	dialog = gtk_message_dialog_new (GTK_WINDOW (list->dialog),
	                                 GTK_DIALOG_DESTROY_WITH_PARENT,
	                                 GTK_MESSAGE_QUESTION,
	                                 GTK_BUTTONS_NONE,
	                                 _("Are you sure you wish to delete the connection %s?"),
	                                 id);
	gtk_dialog_add_buttons (GTK_DIALOG (dialog),
	                        GTK_STOCK_CANCEL, GTK_RESPONSE_CANCEL,
	                        GTK_STOCK_DELETE, GTK_RESPONSE_YES,
	                        NULL);
	gtk_window_set_transient_for (GTK_WINDOW (dialog), GTK_WINDOW (list->dialog));

	result = gtk_dialog_run (GTK_DIALOG (dialog));
	gtk_widget_destroy (dialog);

	if (result == GTK_RESPONSE_YES) {
		delete_connection (list,
		                   connection,
		                   delete_result_cb,
		                   GTK_WINDOW (list->dialog));
	}
}

static void
pk_button_selection_changed_cb (GtkTreeSelection *selection, gpointer user_data)
{
	CEPolkitButton *button = user_data;
	NMConnectionList *list = g_object_get_data (G_OBJECT (button), "NMConnectionList");
	GtkTreeIter iter;
	GtkTreeModel *model;
	NMRemoteConnection *connection;
	NMSettingConnection *s_con;
	gboolean sensitive = FALSE;

	if (gtk_tree_selection_get_selected (selection, &model, &iter)) {
		connection = get_active_connection (list->connection_list);
		if (connection) {
			s_con = nm_connection_get_setting_connection (NM_CONNECTION (connection));
			g_assert (s_con);
	
			sensitive = !nm_setting_connection_get_read_only (s_con);
		}
	}

	ce_polkit_button_set_master_sensitive (button, sensitive);
}

static void
connection_double_clicked_cb (GtkTreeView *tree_view,
                              GtkTreePath *path,
                              GtkTreeViewColumn *column,
                              gpointer user_data)
{
	GtkButton *button = user_data;

	if (ce_polkit_button_get_actionable (CE_POLKIT_BUTTON (button)))
		gtk_button_clicked (button);
}

static void
dialog_response_cb (GtkDialog *dialog, guint response, gpointer user_data)
{
	gtk_widget_hide (GTK_WIDGET (dialog));
}

static void
nm_connection_list_init (NMConnectionList *list)
{
}

static void
dispose (GObject *object)
{
	NMConnectionList *list = NM_CONNECTION_LIST (object);

	if (list->dialog)
		gtk_widget_hide (list->dialog);

	if (list->editors)
		g_hash_table_destroy (list->editors);

	if (list->gui)
		g_object_unref (list->gui);
	if (list->nm_client)
		g_object_unref (list->nm_client);

	if (list->settings)
		g_object_unref (list->settings);

	G_OBJECT_CLASS (nm_connection_list_parent_class)->dispose (object);
}

static void
nm_connection_list_class_init (NMConnectionListClass *klass)
{
	GObjectClass *object_class = G_OBJECT_CLASS (klass);

	/* virtual methods */
	object_class->dispose = dispose;

	/* Signals */
	list_signals[LIST_DONE] =
		g_signal_new ("done",
					  G_OBJECT_CLASS_TYPE (object_class),
					  G_SIGNAL_RUN_FIRST,
					  G_STRUCT_OFFSET (NMConnectionListClass, done),
					  NULL, NULL,
					  g_cclosure_marshal_VOID__INT,
					  G_TYPE_NONE, 1, G_TYPE_INT);

	list_signals[EDITING_DONE] =
		g_signal_new ("editing-done",
		              G_OBJECT_CLASS_TYPE (object_class),
		              G_SIGNAL_RUN_FIRST,
		              G_STRUCT_OFFSET (NMConnectionListClass, done),
		              NULL, NULL,
		              g_cclosure_marshal_VOID__INT,
		              G_TYPE_NONE, 1, G_TYPE_INT);
}

static void
column_header_clicked_cb (GtkTreeViewColumn *treeviewcolumn, gpointer user_data)
{
	gint sort_col_id = GPOINTER_TO_INT (user_data);

	gtk_tree_view_column_set_sort_column_id (treeviewcolumn, sort_col_id);
}

static gint
sort_connection_types (GtkTreeModel *model, GtkTreeIter *a, GtkTreeIter *b, gpointer user_data)
{
	GtkTreeSortable *sortable = user_data;
	int order_a, order_b;
	GtkSortType order;

	gtk_tree_model_get (model, a, COL_ORDER, &order_a, -1);
	gtk_tree_model_get (model, b, COL_ORDER, &order_b, -1);

	/* The connection types should stay in the same order regardless of whether
	 * the table is sorted ascending or descending.
	 */
	gtk_tree_sortable_get_sort_column_id (sortable, NULL, &order);
	if (order == GTK_SORT_ASCENDING)
		return order_a - order_b;
	else
		return order_b - order_a;
}

static gint
id_sort_func (GtkTreeModel *model, GtkTreeIter *a, GtkTreeIter *b, gpointer user_data)
{
	NMConnection *conn_a, *conn_b;
	gint ret;

	gtk_tree_model_get (model, a, COL_CONNECTION, &conn_a, -1);
	gtk_tree_model_get (model, b, COL_CONNECTION, &conn_b, -1);

	if (!conn_a || !conn_b) {
		g_assert (!conn_a && !conn_b);
		return sort_connection_types (model, a, b, user_data);
	}

	ret = strcmp (nm_connection_get_id (conn_a), nm_connection_get_id (conn_b));
	g_object_unref (conn_a);
	g_object_unref (conn_b);

	return ret;
}

static gint
timestamp_sort_func (GtkTreeModel *model, GtkTreeIter *a, GtkTreeIter *b, gpointer user_data)
{
	NMConnection *conn_a, *conn_b;
	guint64 time_a, time_b;

	gtk_tree_model_get (model, a,
	                    COL_CONNECTION, &conn_a,
	                    COL_TIMESTAMP, &time_a,
	                    -1);
	gtk_tree_model_get (model, b,
	                    COL_CONNECTION, &conn_b,
	                    COL_TIMESTAMP, &time_b,
	                    -1);

	if (!conn_a || !conn_b) {
		g_assert (!conn_a && !conn_b);
		return sort_connection_types (model, a, b, user_data);
	}

	g_object_unref (conn_a);
	g_object_unref (conn_b);

	return time_b - time_a;
}

static void
name_column_data_func (GtkTreeViewColumn *tree_column,
                       GtkCellRenderer *cell,
                       GtkTreeModel *tree_model,
                       GtkTreeIter *iter,
                       gpointer data)
{
	GdkPixbuf *icon;

	gtk_tree_model_get (tree_model, iter,
	                    COL_ICON, &icon,
	                    -1);
	g_object_set (G_OBJECT (cell),
	              "pixbuf", icon,
	              "visible", icon != NULL,
	              NULL);
	if (icon)
		g_object_unref (icon);
}

static gboolean
tree_model_visible_func (GtkTreeModel *model,
                         GtkTreeIter *iter,
                         gpointer user_data)
{
	NMConnection *connection;

	gtk_tree_model_get (model, iter, COL_CONNECTION, &connection, -1);
	if (connection) {
		g_object_unref (connection);
		return TRUE;
	} else {
		/* Top-level type nodes are visible iff they have children */
		return gtk_tree_model_iter_has_child  (model, iter);
	}
}

static void
initialize_treeview (NMConnectionList *self)
{
	GtkCellRenderer *renderer;
	GtkTreeViewColumn *column;
	GtkTreeSelection *selection;
	ConnectionTypeData *types;
	GtkTreeIter iter;
	char *id;
	int i;

	/* Model */
	self->model = GTK_TREE_MODEL (gtk_tree_store_new (7, GDK_TYPE_PIXBUF, G_TYPE_STRING, G_TYPE_STRING, G_TYPE_UINT64, G_TYPE_OBJECT, G_TYPE_GTYPE, G_TYPE_INT));

	/* Filter */
	self->filter = GTK_TREE_MODEL_FILTER (gtk_tree_model_filter_new (self->model, NULL));
	gtk_tree_model_filter_set_visible_func (self->filter,
	                                        tree_model_visible_func,
	                                        self, NULL);

	/* Sortable */
	self->sortable = GTK_TREE_SORTABLE (gtk_tree_model_sort_new_with_model (GTK_TREE_MODEL (self->filter)));
	gtk_tree_sortable_set_default_sort_func (self->sortable, NULL, NULL, NULL);
	gtk_tree_sortable_set_sort_func (self->sortable, COL_TIMESTAMP, timestamp_sort_func,
	                                 self->sortable, NULL);
	gtk_tree_sortable_set_sort_func (self->sortable, COL_ID, id_sort_func,
	                                 self->sortable, NULL);
	gtk_tree_sortable_set_sort_column_id (self->sortable, COL_TIMESTAMP, GTK_SORT_DESCENDING);

	gtk_tree_view_set_model (self->connection_list, GTK_TREE_MODEL (self->sortable));

	/* Name column */
	column = gtk_tree_view_column_new ();
	gtk_tree_view_column_set_title (column, _("Name"));
	gtk_tree_view_column_set_spacing (column, 4);

	renderer = gtk_cell_renderer_pixbuf_new ();
	gtk_tree_view_column_pack_start (column, renderer, FALSE);
	gtk_tree_view_column_set_cell_data_func (column, renderer, name_column_data_func, NULL, NULL);

	renderer = gtk_cell_renderer_text_new ();
	gtk_tree_view_column_pack_start (column, renderer, TRUE);
	gtk_tree_view_column_add_attribute (column, renderer, "markup", COL_ID);

	gtk_tree_view_column_set_expand (column, TRUE);
	gtk_tree_view_column_set_sort_column_id (column, COL_ID);
	g_signal_connect (column, "clicked", G_CALLBACK (column_header_clicked_cb), GINT_TO_POINTER (COL_ID));
	gtk_tree_view_append_column (self->connection_list, column);

	/* Last Used column */
	renderer = g_object_new (GTK_TYPE_CELL_RENDERER_TEXT,
	                         "foreground", "SlateGray",
	                         NULL);
	column = gtk_tree_view_column_new_with_attributes (_("Last Used"),
	                                                   renderer,
	                                                   "text", COL_LAST_USED,
	                                                   NULL);
	gtk_tree_view_column_set_sort_column_id (column, COL_TIMESTAMP);
	g_signal_connect (column, "clicked", G_CALLBACK (column_header_clicked_cb), GINT_TO_POINTER (COL_TIMESTAMP));
	gtk_tree_view_append_column (self->connection_list, column);

	/* Selection */
	selection = gtk_tree_view_get_selection (self->connection_list);
	gtk_tree_selection_set_mode (selection, GTK_SELECTION_SINGLE);

	/* Fill in connection types */
	types = get_connection_type_list ();
	for (i = 0; types[i].name; i++) {
		id = g_strdup_printf ("<b>%s</b>", types[i].name);
		gtk_tree_store_append (GTK_TREE_STORE (self->model), &iter, NULL);
		gtk_tree_store_set (GTK_TREE_STORE (self->model), &iter,
		                    COL_ICON, types[i].icon,
		                    COL_ID, id,
		                    COL_GTYPE, types[i].setting_type,
		                    COL_ORDER, i,
		                    -1);
		g_free (id);
	}
}

static void
add_connection_buttons (NMConnectionList *self)
{
	GtkWidget *button, *box;
	GtkTreeSelection *selection;

	selection = gtk_tree_view_get_selection (self->connection_list);

	/* Add */
	button = GTK_WIDGET (gtk_builder_get_object (self->gui, "connection_add"));
	g_signal_connect (button, "clicked", G_CALLBACK (add_clicked), self);

	box = GTK_WIDGET (gtk_builder_get_object (self->gui, "connection_button_box"));

	/* Edit */
	button = ce_polkit_button_new (_("_Edit"),
	                               _("Edit the selected connection"),
	                               _("_Edit..."),
	                               _("Authenticate to edit the selected connection"),
	                               GTK_STOCK_EDIT,
	                               self->nm_client,
	                               NM_CLIENT_PERMISSION_SETTINGS_MODIFY_SYSTEM);
	g_object_set_data (G_OBJECT (button), "NMConnectionList", self);
	gtk_button_set_use_underline (GTK_BUTTON (button), TRUE);
	gtk_box_pack_end (GTK_BOX (box), button, TRUE, TRUE, 0);

	g_signal_connect_swapped (button, "clicked", G_CALLBACK (do_edit), self);
	g_signal_connect (self->connection_list, "row-activated", G_CALLBACK (connection_double_clicked_cb), button);
	g_signal_connect (selection, "changed", G_CALLBACK (pk_button_selection_changed_cb), button);
	pk_button_selection_changed_cb (selection, button);

	/* Delete */
	button = ce_polkit_button_new (_("_Delete"),
	                               _("Delete the selected connection"),
	                               _("_Delete..."),
	                               _("Authenticate to delete the selected connection"),
	                               GTK_STOCK_DELETE,
	                               self->nm_client,
	                               NM_CLIENT_PERMISSION_SETTINGS_MODIFY_SYSTEM);
	g_object_set_data (G_OBJECT (button), "NMConnectionList", self);
	gtk_button_set_use_underline (GTK_BUTTON (button), TRUE);
	gtk_box_pack_end (GTK_BOX (box), button, TRUE, TRUE, 0);

	g_signal_connect (button, "clicked", G_CALLBACK (delete_clicked), self);
	g_signal_connect (selection, "changed", G_CALLBACK (pk_button_selection_changed_cb), button);
	pk_button_selection_changed_cb (selection, button);

	gtk_widget_show_all (box);
}

static void
connection_removed (NMRemoteConnection *connection, gpointer user_data)
{
	NMConnectionList *self = NM_CONNECTION_LIST (user_data);
	GtkTreeIter iter, parent_iter;

	if (get_iter_for_connection (self, connection, &iter)) {
		gtk_tree_model_iter_parent (self->model, &parent_iter, &iter);
		gtk_tree_store_remove (GTK_TREE_STORE (self->model), &iter);
	}
	gtk_tree_model_filter_refilter (self->filter);
}

static void
connection_updated (NMRemoteConnection *connection, gpointer user_data)
{
	NMConnectionList *self = NM_CONNECTION_LIST (user_data);
	GtkTreeIter iter;

	if (get_iter_for_connection (self, connection, &iter))
		update_connection_row (self, &iter, connection);
}

static gboolean
get_parent_iter_for_connection (NMConnectionList *list,
                                NMRemoteConnection *connection,
                                GtkTreeIter *iter)
{
	NMSettingConnection *s_con;
	const char *str_type;
	GType type, row_type;

	s_con = nm_connection_get_setting_connection (NM_CONNECTION (connection));
	g_assert (s_con);
	str_type = nm_setting_connection_get_connection_type (s_con);
	if (!str_type) {
		g_warning ("Ignoring incomplete connection");
		return FALSE;
	}

	if (!strcmp (str_type, NM_SETTING_CDMA_SETTING_NAME))
		str_type = NM_SETTING_GSM_SETTING_NAME;
	type = nm_connection_lookup_setting_type (str_type);

	if (gtk_tree_model_get_iter_first (list->model, iter)) {
		do {
			gtk_tree_model_get (list->model, iter,
			                    COL_GTYPE, &row_type,
			                    -1);
			if (row_type == type)
				return TRUE;
		} while (gtk_tree_model_iter_next (list->model, iter));
	}

	g_warning ("Unsupported connection type '%s'", str_type);
	return FALSE;
}

static void
connection_added (NMRemoteSettings *settings,
                  NMRemoteConnection *connection,
                  gpointer user_data)
{
	NMConnectionList *self = NM_CONNECTION_LIST (user_data);
	GtkTreeIter parent_iter, iter;
	NMSettingConnection *s_con;
	char *last_used;
	gboolean expand = TRUE;

	if (!get_parent_iter_for_connection (self, connection, &parent_iter))
		return;

	s_con = nm_connection_get_setting_connection (NM_CONNECTION (connection));

	last_used = format_last_used (nm_setting_connection_get_timestamp (s_con));

	gtk_tree_store_append (GTK_TREE_STORE (self->model), &iter, &parent_iter);
	gtk_tree_store_set (GTK_TREE_STORE (self->model), &iter,
	                    COL_ID, nm_setting_connection_get_id (s_con),
	                    COL_LAST_USED, last_used,
	                    COL_TIMESTAMP, nm_setting_connection_get_timestamp (s_con),
	                    COL_CONNECTION, connection,
	                    -1);

	g_free (last_used);

	if (self->displayed_type) {
		GType added_type;

		gtk_tree_model_get (self->model, &parent_iter,
		                    COL_GTYPE, &added_type,
		                    -1);
		if (added_type != self->displayed_type)
			expand = FALSE;
	}

	if (expand) {
		GtkTreePath *path, *filtered_path;

		path = gtk_tree_model_get_path (self->model, &parent_iter);
		filtered_path = gtk_tree_model_filter_convert_child_path_to_path (self->filter, path);
		gtk_tree_view_expand_row (self->connection_list, filtered_path, FALSE);
		gtk_tree_path_free (filtered_path);
		gtk_tree_path_free (path);
	}

	g_signal_connect (connection, NM_REMOTE_CONNECTION_REMOVED, G_CALLBACK (connection_removed), self);
	g_signal_connect (connection, NM_REMOTE_CONNECTION_UPDATED, G_CALLBACK (connection_updated), self);
	gtk_tree_model_filter_refilter (self->filter);
}

static void
initial_connections_read (NMRemoteSettings *settings, gpointer user_data)
{
	NMConnectionList *list = user_data;
	GtkTreePath *path;
	GtkTreeIter iter;

	g_signal_handlers_disconnect_by_func (settings, G_CALLBACK (initial_connections_read), list);

	if (gtk_tree_model_get_iter_first (GTK_TREE_MODEL (list->sortable), &iter)) {
		path = gtk_tree_model_get_path (GTK_TREE_MODEL (list->sortable), &iter);
		gtk_tree_view_scroll_to_cell (list->connection_list,
		                              path, NULL,
		                              FALSE, 0, 0);
		gtk_tree_path_free (path);
	}
}

NMConnectionList *
nm_connection_list_new (void)
{
	NMConnectionList *list;
	DBusGConnection *bus;
	GError *error = NULL;
	const char *objects[] = { "NMConnectionList", NULL };

	list = g_object_new (NM_TYPE_CONNECTION_LIST, NULL);
	if (!list)
		return NULL;

	/* load GUI */
	list->gui = gtk_builder_new ();

	if (!gtk_builder_add_objects_from_file (list->gui,
	                                        UIDIR "/nm-connection-editor.ui",
	                                        (char **) objects,
	                                        &error)) {
		g_warning ("Couldn't load builder file: %s", error->message);
		g_error_free (error);
		goto error;
	}

	gtk_window_set_default_icon_name ("preferences-system-network");

	list->nm_client = nm_client_new ();
	if (!list->nm_client)
		goto error;

	bus = dbus_g_bus_get (DBUS_BUS_SYSTEM, &error);
	if (error) {
		g_warning ("Could not connect to the system bus: %s", error->message);
		g_error_free (error);
		goto error;
	}

	list->settings = nm_remote_settings_new (bus);
	dbus_g_connection_unref (bus);
	g_signal_connect (list->settings,
	                  NM_REMOTE_SETTINGS_NEW_CONNECTION,
	                  G_CALLBACK (connection_added),
	                  list);
	g_signal_connect (list->settings,
	                  NM_REMOTE_SETTINGS_CONNECTIONS_READ,
	                  G_CALLBACK (initial_connections_read),
	                  list);

	list->editors = g_hash_table_new_full (g_direct_hash, g_direct_equal, NULL, g_object_unref);

	list->connection_list = GTK_TREE_VIEW (gtk_builder_get_object (list->gui, "connection_list"));
	initialize_treeview (list);
	add_connection_buttons (list);

	/* Connect to the main dialog's response handler */
	list->dialog = GTK_WIDGET (gtk_builder_get_object (list->gui, "NMConnectionList"));
	if (!list->dialog)
		goto error;
	g_signal_connect (G_OBJECT (list->dialog), "response", G_CALLBACK (dialog_response_cb), list);

	if (!vpn_get_plugins (&error)) {
		g_warning ("%s: failed to load VPN plugins: %s", __func__, error->message);
		g_error_free (error);
	}

	return list;

error:
	g_object_unref (list);
	return NULL;
}

void
nm_connection_list_set_type (NMConnectionList *self, GType ctype)
{
	g_return_if_fail (NM_IS_CONNECTION_LIST (self));

	self->displayed_type = ctype;
}

void
nm_connection_list_create (NMConnectionList *self, GType ctype, const char *detail)
{
	ConnectionTypeData *types;
	int i;

	g_return_if_fail (NM_IS_CONNECTION_LIST (self));

	types = get_connection_type_list ();
	for (i = 0; types[i].name; i++) {
		if (types[i].setting_type == ctype)
			break;
	}
	if (!types[i].name) {
		error_dialog (NULL,
		              _("Error creating connection"),
		              _("Don't know how to create '%s' connections"), g_type_name (ctype));
	} else {
		new_connection_of_type (GTK_WINDOW (self->dialog),
		                        detail,
		                        self->settings,
		                        types[i].new_connection_func,
		                        really_add_connection,
		                        self);
	}
}

static NMConnection *
get_connection (NMRemoteSettings *settings, const gchar *id)
{
	const gchar *uuid;
	NMConnection *connection = NULL;
	GSList *list, *l;

	list = nm_remote_settings_list_connections (settings);
	for (l = list; l; l = l->next) {
		connection = l->data;
		uuid = nm_connection_get_uuid (connection);
		if (g_strcmp0 (uuid, id) == 0) {
			g_slist_free (list);
			return connection;
		}
	}

	g_slist_free (list);
	return NULL;
}

typedef struct {
	NMConnectionList *self;
	const gchar *uuid;
	gboolean wait;
} EditData;

static void
connections_read (NMRemoteSettings *settings, EditData *data)
{
	NMConnection *connection;
	static gulong signal_id = 0;

	connection = get_connection (settings, data->uuid);
	if (connection) {
		edit_connection (data->self, connection);
		g_object_unref (connection);
	} else if (data->wait) {
		data->wait = FALSE;
		signal_id = g_signal_connect (settings, "connections-read",
		                              G_CALLBACK (connections_read), data);
		return;
	} else {
		error_dialog (NULL,
		              _("Error editing connection"),
		              _("Did not find a connection with UUID '%s'"), data->uuid);
	}

	if (signal_id != 0) {
		g_signal_handler_disconnect (settings, signal_id);
		signal_id = 0;
	}

	g_free (data);
}

void
nm_connection_list_edit (NMConnectionList *self, const gchar *uuid)
{
	EditData *data;

	g_return_if_fail (NM_IS_CONNECTION_LIST (self));

	data =  g_new0 (EditData, 1);
	data->self = self;
	data->uuid = uuid;
	data->wait = TRUE;

	connections_read (self->settings, data);
}

static void
list_response_cb (GtkDialog *dialog, gint response, gpointer user_data)
{
	g_signal_emit (NM_CONNECTION_LIST (user_data), list_signals[LIST_DONE], 0, response);
}

static void
list_close_cb (GtkDialog *dialog, gpointer user_data)
{
	gtk_dialog_response (dialog, GTK_RESPONSE_CLOSE);
}

void
nm_connection_list_present (NMConnectionList *list)
{
	g_return_if_fail (NM_IS_CONNECTION_LIST (list));

	if (!list->signals_connected) {
		g_signal_connect (G_OBJECT (list->dialog), "response",
			              G_CALLBACK (list_response_cb), list);
		g_signal_connect (G_OBJECT (list->dialog), "close",
			              G_CALLBACK (list_close_cb), list);
		list->signals_connected = TRUE;
	}

	gtk_window_present (GTK_WINDOW (list->dialog));
}

