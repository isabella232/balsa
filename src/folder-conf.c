/* -*-mode:c; c-style:k&r; c-basic-offset:4; -*- */
/* Balsa E-Mail Client
 * Copyright (C) 1997-2016 Stuart Parmenter and others,
 *                         See the file AUTHORS for a list.
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2, or (at your option) 
 * any later version.
 *  
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of 
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the  
 * GNU General Public License for more details.
 *  
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, see <http://www.gnu.org/licenses/>.
 */

#if defined(HAVE_CONFIG_H) && HAVE_CONFIG_H
# include "config.h"
#endif                          /* HAVE_CONFIG_H */
#include "folder-conf.h"

#include <string.h>
#include "balsa-app.h"
#include "balsa-icons.h"
#include "mailbox-conf.h"
#include "mailbox-node.h"
#include "save-restore.h"
#include "pref-manager.h"
#include "imap-server.h"
#include <glib/gi18n.h>

#if HAVE_MACOSX_DESKTOP
#  include "macosx-helpers.h"
#endif

typedef struct _CommonDialogData CommonDialogData;
typedef struct _FolderDialogData FolderDialogData;
typedef struct _SubfolderDialogData SubfolderDialogData;

typedef gboolean (*CommonDialogFunc)(CommonDialogData * cdd);

#define BALSA_FOLDER_CONF_IMAP_KEY "balsa-folder-conf-imap"

struct _CommonDialogData {
    GtkWidget *dialog;
    BalsaMailboxNode *mbnode;
    CommonDialogFunc ok;
};

struct _FolderDialogData {
    CommonDialogData cdd;
    BalsaServerConf bsc;
    GtkWidget *folder_name, *port, *username, *anonymous, *remember,
        *password, *subscribed, *list_inbox, *prefix;
    GtkWidget *connection_limit, *enable_persistent,
        *use_idle, *has_bugs, *use_status;
};

struct _SubfolderDialogData {
    CommonDialogData cdd;
    BalsaMailboxConfView *mcv;
    GtkWidget *parent_folder, *folder_name, *host_label;
    const gchar *old_folder, *old_parent;
    BalsaMailboxNode *parent;   /* (new) parent of the mbnode.  */
    /* Used for renaming and creation */
};

/* Destroy notification */
static void
folder_conf_destroy_cdd(CommonDialogData * cdd)
{
    if (cdd->dialog != NULL) {
        /* The mailbox node was destroyed. Close the dialog, but don't
         * trigger further calls to folder_conf_destroy_cdd. */
        cdd->mbnode = NULL;
        gtk_dialog_response(GTK_DIALOG(cdd->dialog), GTK_RESPONSE_NONE);
    } else
        g_free(cdd);
}

static void
folder_conf_response(GtkDialog * dialog, int response,
                     CommonDialogData * cdd)
{
    GError *err = NULL;

    /* If mbnode's parent gets rescanned, mbnode will be finalized,
     * which triggers folder_conf_destroy_cdd, and recursively calls
     * folder_conf_response, which results in cdd being freed before
     * we're done with it; we ref mbnode to avoid that. */
    if (cdd->mbnode)
	g_object_ref(cdd->mbnode);
    switch (response) {
    case GTK_RESPONSE_HELP:
        gtk_show_uri_on_window(GTK_WINDOW(dialog), "help:balsa/folder-config",
                               gtk_get_current_event_time(), &err);
        if (err) {
            balsa_information(LIBBALSA_INFORMATION_WARNING,
                              _("Error displaying config help: %s\n"),
                              err->message);
            g_error_free(err);
        }
	g_clear_object(&cdd->mbnode);
        return;
    case GTK_RESPONSE_OK:
        if(!cdd->ok(cdd))
            break;
        /* ...or fall over */
    default:
        g_clear_pointer(&cdd->dialog, gtk_widget_destroy);
        if (cdd->mbnode) {
            /* Clearing the data signifies that the dialog has been
             * destroyed. It also triggers a call to
             * folder_conf_destroy_cdd, which will free cdd, so we cache
             * cdd->mbnode. */
	    BalsaMailboxNode *mbnode = cdd->mbnode;
            g_object_set_data(G_OBJECT(mbnode),
                              BALSA_FOLDER_CONF_IMAP_KEY, NULL);
	    g_object_unref(mbnode);
	} else
            /* Cancelling, without creating a mailbox node. Nobody owns
             * the xDialogData, so we'll free it here. */
            g_free(cdd);
        break;
    }
}

/* folder_conf_imap_node:
   show configuration widget for given mailbox node, allow user to 
   modify it and update mailbox node accordingly.
   Creates the node when mn == NULL.
*/
static void 
validate_folder(GtkWidget *w, FolderDialogData * fcw)
{
    CommonDialogData *cdd = (CommonDialogData *) fcw;
    gboolean sensitive = TRUE;

    if (!*gtk_editable_get_text(GTK_EDITABLE(fcw->folder_name))) {
    	sensitive = FALSE;
    } else if (!*gtk_editable_get_text(GTK_EDITABLE(fcw->bsc.server))) {
    	sensitive = FALSE;
    }

    /* encryption w/ client cert requires cert file */
    if (sensitive &&
    	((gtk_combo_box_get_active(GTK_COMBO_BOX(fcw->bsc.security)) + 1) != NET_CLIENT_CRYPT_NONE) &&
		gtk_toggle_button_get_active(GTK_TOGGLE_BUTTON(fcw->bsc.need_client_cert))) {
    	gchar *cert_file;

    	cert_file = gtk_file_chooser_get_filename(GTK_FILE_CHOOSER(fcw->bsc.client_cert_file));
    	if ((cert_file == NULL) || (cert_file[0] == '\0')) {
    		sensitive = FALSE;
    	}
    	g_free(cert_file);
    }

    gtk_dialog_set_response_sensitive(GTK_DIALOG(cdd->dialog), GTK_RESPONSE_OK, sensitive);
}

static void
anonymous_cb(GtkToggleButton * button, FolderDialogData * fcw)
{
    gtk_widget_set_sensitive(fcw->anonymous,
                             gtk_toggle_button_get_active(button));
}

static void
remember_cb(GtkToggleButton * button, FolderDialogData * fcw)
{
    gtk_widget_set_sensitive(fcw->password,
                             gtk_toggle_button_get_active(button));
}

static void
security_cb(GtkComboBox *combo, FolderDialogData *fcw)
{
	gboolean sensitive;

	sensitive = (gtk_combo_box_get_active(combo) + 1) != NET_CLIENT_CRYPT_NONE;
	gtk_widget_set_sensitive(fcw->bsc.need_client_cert, sensitive);
	sensitive = sensitive & gtk_toggle_button_get_active(GTK_TOGGLE_BUTTON(fcw->bsc.need_client_cert));
	gtk_widget_set_sensitive(fcw->bsc.client_cert_file, sensitive);
	gtk_widget_set_sensitive(fcw->bsc.client_cert_passwd, sensitive);
	validate_folder(GTK_WIDGET(combo), fcw);
}

static gboolean
folder_conf_clicked_ok(FolderDialogData * fcw)
{
    CommonDialogData *cdd = (CommonDialogData *) fcw;
    gboolean insert;
    LibBalsaServer *s;
    const gchar *username;
    const gchar *host;

    host = gtk_editable_get_text(GTK_EDITABLE(fcw->bsc.server));
    username = gtk_editable_get_text(GTK_EDITABLE(fcw->username));

    if (cdd->mbnode) {
        insert = FALSE;
        s = balsa_mailbox_node_get_server(cdd->mbnode);
    } else {
        insert = TRUE;
	s = LIBBALSA_SERVER(libbalsa_imap_server_new(username, host));
        libbalsa_server_connect_get_password(s, G_CALLBACK(ask_password), NULL);
    }

    libbalsa_server_set_security(s, balsa_server_conf_get_security(&fcw->bsc));
    libbalsa_imap_server_set_max_connections
        (LIBBALSA_IMAP_SERVER(s),
         gtk_spin_button_get_value_as_int(GTK_SPIN_BUTTON
                                          (fcw->connection_limit)));
    libbalsa_imap_server_enable_persistent_cache
        (LIBBALSA_IMAP_SERVER(s),
         gtk_toggle_button_get_active(GTK_TOGGLE_BUTTON(fcw->enable_persistent)));
    libbalsa_imap_server_set_use_idle
        (LIBBALSA_IMAP_SERVER(s), 
         gtk_toggle_button_get_active(GTK_TOGGLE_BUTTON(fcw->use_idle)));
    libbalsa_imap_server_set_bug
        (LIBBALSA_IMAP_SERVER(s), ISBUG_FETCH,
         gtk_toggle_button_get_active(GTK_TOGGLE_BUTTON(fcw->has_bugs)));
    libbalsa_imap_server_set_use_status
        (LIBBALSA_IMAP_SERVER(s),
         gtk_toggle_button_get_active(GTK_TOGGLE_BUTTON(fcw->use_status)));
    libbalsa_server_set_username(s, username);
    libbalsa_server_set_try_anonymous(s,
        gtk_toggle_button_get_active(GTK_TOGGLE_BUTTON(fcw->anonymous)));
    libbalsa_server_set_remember_passwd(s,
        gtk_toggle_button_get_active(GTK_TOGGLE_BUTTON(fcw->remember)));
    libbalsa_server_set_password(s,
                                 gtk_editable_get_text(GTK_EDITABLE
                                                    (fcw->password)));
    if (!cdd->mbnode) {
        cdd->mbnode = balsa_mailbox_node_new_imap_folder(s, NULL);
	/* mbnode will be unrefed in folder_conf_response. */
	g_object_ref(cdd->mbnode);
        /* The mailbox node takes over ownership of the
         * FolderDialogData. */
        g_object_set_data_full(G_OBJECT(cdd->mbnode),
                               BALSA_FOLDER_CONF_IMAP_KEY, fcw,
                               (GDestroyNotify) folder_conf_destroy_cdd);
    }

    balsa_mailbox_node_set_dir(cdd->mbnode,
            gtk_editable_get_text(GTK_EDITABLE(fcw->prefix)));
    balsa_mailbox_node_set_name(cdd->mbnode,
            gtk_editable_get_text(GTK_EDITABLE(fcw->folder_name)));
    balsa_mailbox_node_set_subscribed(cdd->mbnode,
            gtk_toggle_button_get_active(GTK_TOGGLE_BUTTON(fcw->subscribed)));
    balsa_mailbox_node_set_list_inbox(cdd->mbnode,
            gtk_toggle_button_get_active(GTK_TOGGLE_BUTTON(fcw->list_inbox)));

    libbalsa_server_set_host(s, host);
    libbalsa_server_config_changed(s); /* trigger config save */

    if (insert) {
	balsa_mblist_mailbox_node_append(NULL, cdd->mbnode);
        balsa_mailbox_node_append_subtree(cdd->mbnode);
        config_folder_add(cdd->mbnode, NULL);
	g_signal_connect_swapped(s, "config-changed",
		                 G_CALLBACK(config_folder_update),
				 cdd->mbnode);
        update_mail_servers();
    } else {
        balsa_mailbox_node_rescan(cdd->mbnode);
	balsa_mblist_mailbox_node_redraw(cdd->mbnode);
    }
    return TRUE;
}

/* folder_conf_imap_node:
   show the IMAP Folder configuration dialog for given mailbox node.
   If mn is NULL, setup it with default values for folder creation.
*/
void
folder_conf_imap_node(BalsaMailboxNode *mn)
{
    GtkWidget *notebook, *grid, *label, *advanced;
    FolderDialogData *fcw;
    CommonDialogData *cdd;
    static FolderDialogData *fcw_new;
    LibBalsaServer *s;
    gchar *default_server;
    int r = 0;

#if defined(HAVE_LIBSECRET)
    static const gchar *remember_password_message =
        N_("_Remember password in Secret Service");
#else
    static const gchar *remember_password_message =
        N_("_Remember password");
#endif                          /* defined(HAVE_LIBSECRET) */

    /* Allow only one dialog per mailbox node, and one with mn == NULL
     * for creating a new folder. */
    fcw = mn ? g_object_get_data(G_OBJECT(mn), BALSA_FOLDER_CONF_IMAP_KEY)
             : fcw_new;
    if (fcw) {
        gtk_window_present_with_time(GTK_WINDOW(fcw->cdd.dialog),
                                     gtk_get_current_event_time());
        return;
    }

    s = mn ? balsa_mailbox_node_get_server(mn) : NULL;

    fcw = g_new(FolderDialogData, 1);
    cdd = (CommonDialogData *) fcw;
    cdd->ok = (CommonDialogFunc) folder_conf_clicked_ok;
    cdd->mbnode = mn;
    cdd->dialog = gtk_dialog_new_with_buttons
                   (_("Remote IMAP folder"),
                    GTK_WINDOW(balsa_app.main_window),
                    GTK_DIALOG_DESTROY_WITH_PARENT |
                    libbalsa_dialog_flags(),
                    mn ? _("_Update") : _("C_reate"), GTK_RESPONSE_OK,
                    _("_Cancel"), GTK_RESPONSE_CANCEL,
                    _("_Help"), GTK_RESPONSE_HELP,
                    NULL);
#if HAVE_MACOSX_DESKTOP
    libbalsa_macosx_menu_for_parent(cdd->dialog, GTK_WINDOW(balsa_app.main_window));
#endif
    g_object_add_weak_pointer(G_OBJECT(cdd->dialog),
                              (gpointer) &cdd->dialog);
    if (mn) {
        g_object_set_data_full(G_OBJECT(mn),
                               BALSA_FOLDER_CONF_IMAP_KEY, fcw, 
                               (GDestroyNotify) folder_conf_destroy_cdd);
    } else {
        fcw_new = fcw;
        g_object_add_weak_pointer(G_OBJECT(cdd->dialog),
                                  (gpointer) &fcw_new);
    }

    notebook = gtk_notebook_new();
    gtk_widget_set_vexpand(notebook, TRUE);
    gtk_container_add(GTK_CONTAINER(gtk_dialog_get_content_area(GTK_DIALOG(cdd->dialog))), notebook);
    grid = libbalsa_create_grid();
    g_object_set(G_OBJECT(grid), "margin", 12, NULL);
    gtk_notebook_append_page(GTK_NOTEBOOK(notebook), grid,
                             gtk_label_new_with_mnemonic(_("_Basic")));
    advanced = balsa_server_conf_get_advanced_widget(&fcw->bsc);
    /* Limit number of connections */
    fcw->connection_limit = 
        balsa_server_conf_add_spinner
        (&fcw->bsc, _("_Max number of connections:"), 1, 40, 1,
         s 
         ? libbalsa_imap_server_get_max_connections(LIBBALSA_IMAP_SERVER(s))
         : 20);
    fcw->enable_persistent = 
        balsa_server_conf_add_checkbox(&fcw->bsc,
                                       _("Enable _persistent cache"));
    if(!s || 
       libbalsa_imap_server_has_persistent_cache(LIBBALSA_IMAP_SERVER(s)))
	gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(fcw->enable_persistent),
                                     TRUE);
    fcw->use_idle = 
        balsa_server_conf_add_checkbox(&fcw->bsc,
                                       _("Use IDLE command"));
    if(s &&
       libbalsa_imap_server_get_use_idle(LIBBALSA_IMAP_SERVER(s)))
	gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(fcw->use_idle),
                                     TRUE);
    fcw->has_bugs = 
        balsa_server_conf_add_checkbox(&fcw->bsc,
                                       _("Enable _bug workarounds"));
    if(s &&
       libbalsa_imap_server_has_bug(LIBBALSA_IMAP_SERVER(s), ISBUG_FETCH))
	gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(fcw->has_bugs),
                                     TRUE);
    fcw->use_status = 
        balsa_server_conf_add_checkbox(&fcw->bsc,
                                       _("Use STATUS for mailbox checking"));
    if(s &&
       libbalsa_imap_server_get_use_status(LIBBALSA_IMAP_SERVER(s)))
	gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(fcw->use_status),
                                     TRUE);
    gtk_notebook_append_page(GTK_NOTEBOOK(notebook), advanced,
                             gtk_label_new_with_mnemonic(_("_Advanced")));

    /* INPUT FIELD CREATION */
    label = libbalsa_create_grid_label(_("Descriptive _name:"), grid, 0);
    fcw->folder_name =
        libbalsa_create_grid_entry(grid, G_CALLBACK(validate_folder),
                                   fcw, r++, mn ? balsa_mailbox_node_get_name(mn) : NULL,
				   label);

    default_server = libbalsa_guess_imap_server();
    label = libbalsa_create_grid_label(_("_Server:"), grid, 1);
    fcw->bsc.server =
        libbalsa_create_grid_entry(grid, G_CALLBACK(validate_folder),
                                   fcw, r++, s ? libbalsa_server_get_host(s) : default_server,
                                   label);
    g_free(default_server);

    label = libbalsa_create_grid_label(_("Se_curity:"), grid, r);
    fcw->bsc.security = gtk_combo_box_text_new();
    gtk_widget_set_hexpand(fcw->bsc.security, TRUE);
    gtk_combo_box_text_append_text(GTK_COMBO_BOX_TEXT(fcw->bsc.security), _("IMAP over SSL (IMAPS)"));
    gtk_combo_box_text_append_text(GTK_COMBO_BOX_TEXT(fcw->bsc.security), _("TLS required"));
    gtk_combo_box_text_append_text(GTK_COMBO_BOX_TEXT(fcw->bsc.security), _("TLS if possible (not recommended)"));
    gtk_combo_box_text_append_text(GTK_COMBO_BOX_TEXT(fcw->bsc.security), _("None (not recommended)"));
    gtk_grid_attach(GTK_GRID(grid), fcw->bsc.security, 1, r++, 1, 1);
    gtk_combo_box_set_active(GTK_COMBO_BOX(fcw->bsc.security),
                             s != NULL ? libbalsa_server_get_security(s) - 1 : NET_CLIENT_CRYPT_STARTTLS - 1);
    g_signal_connect(fcw->bsc.security, "changed", G_CALLBACK(security_cb), fcw);
    gtk_label_set_mnemonic_widget(GTK_LABEL(label), fcw->bsc.security);

    label= libbalsa_create_grid_label(_("Use_r name:"), grid, r);
    fcw->username =
        libbalsa_create_grid_entry(grid, G_CALLBACK(validate_folder),
                                   fcw, r++, s ? libbalsa_server_get_username(s) : g_get_user_name(),
                                   label);

    label = libbalsa_create_grid_label(_("_Password:"), grid, r);
    fcw->password =
        libbalsa_create_grid_entry(grid, NULL, NULL, r++,
                                   s ? libbalsa_server_get_password(s) : NULL, label);
    gtk_entry_set_visibility(GTK_ENTRY(fcw->password), FALSE);

    fcw->anonymous =
        libbalsa_create_grid_check(_("_Anonymous access"), grid, r++,
                                   s ? libbalsa_server_get_try_anonymous(s) : FALSE);
    g_signal_connect(fcw->anonymous, "toggled",
                     G_CALLBACK(anonymous_cb), fcw);
    fcw->remember =
        libbalsa_create_grid_check(_(remember_password_message), grid, r++,
                                   s ? libbalsa_server_get_remember_passwd(s) : TRUE);
    g_signal_connect(fcw->remember, "toggled",
                     G_CALLBACK(remember_cb), fcw);

    fcw->subscribed =
        libbalsa_create_grid_check(_("Subscribed _folders only"), grid, r++,
                                   mn ? balsa_mailbox_node_get_subscribed(mn) : FALSE);
    fcw->list_inbox =
        libbalsa_create_grid_check(_("Always show _Inbox"), grid, r++,
                                   mn ? balsa_mailbox_node_get_list_inbox(mn) : TRUE);

    label = libbalsa_create_grid_label(_("Pr_efix:"), grid, r);
    fcw->prefix =
        libbalsa_create_grid_entry(grid, NULL, NULL, r++,
                                   mn ? balsa_mailbox_node_get_dir(mn) : NULL, label);

    gtk_widget_show(cdd->dialog);

    validate_folder(NULL, fcw);
    gtk_widget_grab_focus(fcw->folder_name);

    gtk_dialog_set_default_response(GTK_DIALOG(cdd->dialog),
                                    mn ? GTK_RESPONSE_OK
                                    : GTK_RESPONSE_CANCEL);
    gtk_notebook_set_current_page(GTK_NOTEBOOK(notebook), 0);

    g_signal_connect(cdd->dialog, "response",
                     G_CALLBACK(folder_conf_response), fcw);
    gtk_widget_show(cdd->dialog);
}

/* folder_conf_imap_sub_node:
   Show name and path for an existing subfolder,
   or create a new one.
*/

static void
validate_sub_folder(GtkWidget * w, SubfolderDialogData * sdd)
{
    CommonDialogData *cdd = (CommonDialogData *) sdd;
    BalsaMailboxNode *mn = sdd->parent;
    /*
     * Allow typing in the parent_folder entry box only if we already
     * have the server information in mn:
     */
    gboolean have_server = (mn && LIBBALSA_IS_IMAP_SERVER(balsa_mailbox_node_get_server(mn)));
    gtk_editable_set_editable(GTK_EDITABLE(sdd->parent_folder),
			      have_server);
    /*
     * We'll allow a null parent name, although some IMAP servers
     * will deny permission:
     */
    gtk_dialog_set_response_sensitive(GTK_DIALOG(cdd->dialog), GTK_RESPONSE_OK, 
                                      have_server &&
                                      *gtk_editable_get_text(GTK_EDITABLE
                                                          (sdd->folder_name)));
}

/* callbacks for a `Browse...' button: */

typedef struct _BrowseButtonData BrowseButtonData;
struct _BrowseButtonData {
    SubfolderDialogData *sdd;
    GtkWidget *dialog;
    GtkWidget *button;
    BalsaMailboxNode *mbnode;
};

static void
browse_button_select_row_cb(GtkTreeSelection * selection,
                            BrowseButtonData * bbd)
{
    GtkTreeModel *model;
    GtkTreeIter iter;
    gboolean selected =
        gtk_tree_selection_get_selected(selection, &model, &iter);

    gtk_dialog_set_response_sensitive(GTK_DIALOG(bbd->dialog),
                                      GTK_RESPONSE_OK, selected);
    if (selected)
        gtk_tree_model_get(model, &iter, 0, &bbd->mbnode, -1);
    /* bbd->mbnode is unreffed when bbd is freed. */
}

static void
browse_button_row_activated(GtkTreeView * tree_view, GtkTreePath * path,
                            GtkTreeViewColumn * column,
                            BrowseButtonData * bbd)
{
    gtk_dialog_response(GTK_DIALOG(bbd->dialog), GTK_RESPONSE_OK);
}

static void
browse_button_response(GtkDialog * dialog, gint response,
                       BrowseButtonData * bbd)
{
    if (response == GTK_RESPONSE_OK) {
        BalsaMailboxNode *mbnode = bbd->mbnode;
        const gchar *dir;
        LibBalsaServer *server;

        if (mbnode == NULL)
            return;

        bbd->sdd->parent = mbnode;
        dir = balsa_mailbox_node_get_dir(mbnode);
        if (dir != NULL)
            gtk_editable_set_text(GTK_EDITABLE(bbd->sdd->parent_folder), dir);
        server = balsa_mailbox_node_get_server(mbnode);
        if (server != NULL)
            gtk_label_set_label(GTK_LABEL(bbd->sdd->host_label),
                                libbalsa_server_get_host(server));
    }
    validate_sub_folder(NULL, bbd->sdd);
    gtk_widget_set_sensitive(bbd->button, TRUE);
    gtk_widget_destroy(GTK_WIDGET(dialog));
}

static gboolean
folder_selection_func(GtkTreeSelection * selection, GtkTreeModel * model,
		      GtkTreePath * path, gboolean path_currently_selected,
		      SubfolderDialogData * sdd)
{
    CommonDialogData *cdd = (CommonDialogData *) sdd;
    GtkTreeIter iter;
    BalsaMailboxNode *mbnode;
    LibBalsaServer *server;
    gboolean retval;

    gtk_tree_model_get_iter(model, &iter, path);
    gtk_tree_model_get(model, &iter, 0, &mbnode, -1);
    server = balsa_mailbox_node_get_server(mbnode);
    retval = (LIBBALSA_IS_IMAP_SERVER(server)
	      && (cdd->mbnode == NULL
		  || balsa_mailbox_node_get_server(cdd->mbnode) == server));
    g_object_unref(mbnode);

    return retval;
}

static void
browse_button_data_free(BrowseButtonData *bbd)
{
    g_clear_object(&bbd->mbnode);
    g_free(bbd);
}

static void
browse_button_cb(GtkWidget * widget, SubfolderDialogData * sdd)
{
    CommonDialogData *cdd = (CommonDialogData *) sdd;
    GtkWidget *scroll, *dialog;
    GtkRequisition req;
    BalsaMBList *mblist = balsa_mblist_new();
    GtkWidget *tree_view = GTK_WIDGET(balsa_mblist_get_tree_view(mblist));
    GtkTreeSelection *selection =
        gtk_tree_view_get_selection(GTK_TREE_VIEW(tree_view));
    BrowseButtonData *bbd;
    /*
     * Make only IMAP nodes selectable:
     */
    gtk_tree_selection_set_select_function(selection,
                                           (GtkTreeSelectionFunc) 
                                           folder_selection_func, sdd,
                                           NULL);

    dialog =
        gtk_dialog_new_with_buttons(_("Select parent folder"),
                                    GTK_WINDOW(cdd->dialog),
                                    GTK_DIALOG_DESTROY_WITH_PARENT |
                                    libbalsa_dialog_flags(),
                                    _("_Cancel"), GTK_RESPONSE_CANCEL,
                                    _("_Help"), GTK_RESPONSE_HELP,
                                    NULL);
#if HAVE_MACOSX_DESKTOP
    libbalsa_macosx_menu_for_parent(dialog, GTK_WINDOW(cdd->dialog));
#endif
    
    scroll = gtk_scrolled_window_new(NULL, NULL);
    gtk_widget_set_vexpand(scroll, TRUE);
    gtk_container_add(GTK_CONTAINER(gtk_dialog_get_content_area(GTK_DIALOG(dialog))), scroll);
    gtk_scrolled_window_set_policy(GTK_SCROLLED_WINDOW(scroll),
                                   GTK_POLICY_AUTOMATIC,
                                   GTK_POLICY_ALWAYS);
    gtk_container_add(GTK_CONTAINER(scroll), tree_view);
    gtk_widget_grab_focus(tree_view);

    bbd = g_new(BrowseButtonData, 1);
    bbd->sdd = sdd;
    bbd->dialog = dialog;
    bbd->button = widget;
    bbd->mbnode = NULL;
    g_object_weak_ref(G_OBJECT(dialog),
		      (GWeakNotify) browse_button_data_free, bbd);
    g_signal_connect(selection, "changed",
                     G_CALLBACK(browse_button_select_row_cb), bbd);
    g_signal_connect(tree_view, "row-activated",
                     G_CALLBACK(browse_button_row_activated), bbd);

    /* Force the mailbox list to be a reasonable size. */
    gtk_widget_get_preferred_size(tree_view, NULL, &req);
    /* don't mess with the width, it gets saved! */
    if (req.height > balsa_app.mw_height)
        req.height = balsa_app.mw_height;
    else if (req.height < balsa_app.mw_height / 2)
        req.height = balsa_app.mw_height / 2;
    gtk_window_set_default_size(GTK_WINDOW(dialog), req.width, req.height);

    /* To prevent multiple dialogs, desensitize the browse button. */
    gtk_widget_set_sensitive(widget, FALSE);
    /* OK button is insensitive until some row is selected. */
    gtk_dialog_set_response_sensitive(GTK_DIALOG(dialog),
                                      GTK_RESPONSE_OK, FALSE);

    g_signal_connect(dialog, "response",
                     G_CALLBACK(browse_button_response), bbd);
    gtk_widget_show(GTK_WIDGET(dialog));
}

static gboolean
subfolder_conf_clicked_ok(SubfolderDialogData * sdd)
{
    CommonDialogData *cdd = (CommonDialogData *) sdd;
    gchar *parent, *folder;
    gboolean ret = TRUE;

    parent =
        gtk_editable_get_chars(GTK_EDITABLE(sdd->parent_folder), 0, -1);
    folder = gtk_editable_get_chars(GTK_EDITABLE(sdd->folder_name), 0, -1);
    if(balsa_app.debug)
	g_print("sdd->old_parent=%s\nsdd->old_folder=%s\n",
		sdd->old_parent, sdd->old_folder);

    if (cdd->mbnode) {
        /* Views stuff. */
        LibBalsaMailbox *mailbox;

        mailbox = balsa_mailbox_node_get_mailbox(cdd->mbnode);
        if (mailbox != NULL)
            mailbox_conf_view_check(sdd->mcv, mailbox);
        
        /* rename */
        if (g_strcmp0(parent, sdd->old_parent) != 0 ||
            g_strcmp0(folder, sdd->old_folder) != 0) {
            gint button = GTK_RESPONSE_OK;
            if (g_strcmp0(sdd->old_folder, "INBOX") == 0 &&
                (sdd->old_parent == NULL || sdd->old_parent[0] == '\0')) {
                gchar *msg =
                    g_strdup_printf(_
                                    ("Renaming Inbox is special!\n"
                                     "You will create a subfolder %s in %s\n"
                                     "containing the messages from Inbox.\n"
                                     "Inbox and its subfolders will remain.\n"
                                     "What would you like to do?"),
folder, parent);
                GtkWidget *ask =
                    gtk_dialog_new_with_buttons(_("Question"),
                                                GTK_WINDOW(cdd->dialog),
                                                GTK_DIALOG_MODAL |
                                                libbalsa_dialog_flags(),
                                                _("Rename Inbox"),
                                                GTK_RESPONSE_OK,
                                                _("Cancel"),
                                                GTK_RESPONSE_CANCEL,
                                                NULL);
#if HAVE_MACOSX_DESKTOP
		libbalsa_macosx_menu_for_parent(ask, GTK_WINDOW(cdd->dialog));
#endif
                gtk_container_add(GTK_CONTAINER
                                  (gtk_dialog_get_content_area
                                   (GTK_DIALOG(ask))), gtk_label_new(msg));
                g_free(msg);
                button = gtk_dialog_run(GTK_DIALOG(ask));
                gtk_widget_destroy(ask);
            }
            if (button == GTK_RESPONSE_OK) {
                GError* err = NULL;
                /* Close the mailbox before renaming,
                 * otherwise the rescan will try to close it
                 * under its old name.
                 */
                balsa_window_close_mbnode(balsa_app.main_window,
                                          cdd->mbnode);
                if(!libbalsa_imap_rename_subfolder
                   (LIBBALSA_MAILBOX_IMAP(balsa_mailbox_node_get_mailbox(cdd->mbnode)),
                    parent, folder, balsa_mailbox_node_get_subscribed(cdd->mbnode), &err)) {
                    balsa_information(LIBBALSA_INFORMATION_ERROR,
                                      _("Folder rename failed. Reason: %s"),
                                      err ? err->message : "unknown");
                    g_clear_error(&err);
                    ret = FALSE;
                    goto error;
                }
                balsa_mailbox_node_set_dir(cdd->mbnode, parent);

                /*  Rescan as little of the tree as possible. */
                if (sdd->old_parent != NULL
                    && g_str_has_prefix(sdd->old_parent, parent)) {
                    /* moved it up the tree */
		    BalsaMailboxNode *mbnode =
                        balsa_mailbox_node_find_from_dir(balsa_mailbox_node_get_server(sdd->parent), parent);
                    if (mbnode) {
                        balsa_mailbox_node_rescan(mbnode);
			g_object_unref(mbnode);
		    } else
                        printf("Parent not found!?\n");
                } else if (sdd->old_parent != NULL
                           && g_str_has_prefix(parent, sdd->old_parent)) {
                    /* moved it down the tree */
		    BalsaMailboxNode *mbnode =
			balsa_mailbox_node_find_from_dir(balsa_mailbox_node_get_server(sdd->parent), sdd->old_parent);
                    if (mbnode) {
                        balsa_mailbox_node_rescan(mbnode);
			g_object_unref(mbnode);
		    }
                } else {
                    /* moved it sideways: a chain of folders might
                     * go away, so we'd better rescan from higher up
                     */
                    BalsaMailboxNode *mb = balsa_mailbox_node_get_parent(cdd->mbnode);

                    while (balsa_mailbox_node_get_mailbox(mb) == NULL &&
                           balsa_mailbox_node_get_parent(mb) != NULL)
                        mb = balsa_mailbox_node_get_parent(mb);
                    balsa_mailbox_node_rescan(mb);
                    balsa_mailbox_node_rescan(cdd->mbnode);
                }
            }
        }
    } else {
        GError *err = NULL;
        /* create and subscribe, if parent was. */
        if(libbalsa_imap_new_subfolder(parent, folder,
                                       balsa_mailbox_node_get_subscribed(sdd->parent),
                                       balsa_mailbox_node_get_server(sdd->parent),
                                       &err)) {
            /* see it as server sees it: */
            balsa_mailbox_node_rescan(sdd->parent);
        } else {
            balsa_information(LIBBALSA_INFORMATION_ERROR,
                              _("Folder creation failed. Reason: %s"),
                              err ? err->message : "unknown");
            g_clear_error(&err);
            ret = FALSE;
        }
    }
 error:
    g_free(parent);
    g_free(folder);
    return ret;
}

/* folder_conf_imap_sub_node:
   show the IMAP Folder configuration dialog for given mailbox node
   representing a sub-folder.
   If mn is NULL, setup it with default values for folder creation.
*/
static void
set_ok_sensitive(GtkDialog * dialog)
{
    gtk_dialog_set_response_sensitive(dialog, GTK_RESPONSE_OK, TRUE);
}

void
folder_conf_imap_sub_node(BalsaMailboxNode * mn)
{
    GtkWidget *content, *grid, *button, *label, *hbox;
    SubfolderDialogData *sdd;
    CommonDialogData *cdd;
    static SubfolderDialogData *sdd_new = NULL;
    guint row;
    LibBalsaServer *server;

    /* Allow only one dialog per mailbox node, and one with mn == NULL
     * for creating a new subfolder. */
    sdd = mn ? g_object_get_data(G_OBJECT(mn), BALSA_FOLDER_CONF_IMAP_KEY)
             : sdd_new;
    if (sdd) {
        gtk_window_present_with_time(GTK_WINDOW(sdd->cdd.dialog),
                                     gtk_get_current_event_time());
        return;
    }

    sdd = g_new(SubfolderDialogData, 1);
    cdd = (CommonDialogData *) sdd;
    cdd->ok = (CommonDialogFunc) subfolder_conf_clicked_ok;

    if ((cdd->mbnode = mn) != NULL) {
	/* update */
        LibBalsaMailbox *mailbox;

	mailbox = balsa_mailbox_node_get_mailbox(mn);
	if (mailbox == NULL) {
            balsa_information(LIBBALSA_INFORMATION_ERROR,
                              _("An IMAP folder that is not a mailbox\n"
                                "has no properties that can be changed."));
            g_free(sdd);
	    return;
	}
	sdd->parent = balsa_mailbox_node_get_parent(mn);
	sdd->old_folder = libbalsa_mailbox_get_name(mailbox);
    } else {
	/* create */
        sdd->old_folder = NULL;
        sdd->parent = NULL;
    }
    sdd->old_parent = cdd->mbnode ? balsa_mailbox_node_get_dir(balsa_mailbox_node_get_parent(cdd->mbnode)) : NULL;

    cdd->dialog = 
        gtk_dialog_new_with_buttons
                   (_("Remote IMAP subfolder"), 
                    GTK_WINDOW(balsa_app.main_window),
                    GTK_DIALOG_DESTROY_WITH_PARENT | /* must NOT be modal */
                    libbalsa_dialog_flags(),
                    mn ? _("_Update") : _("_Create"), GTK_RESPONSE_OK,
                    _("_Cancel"), GTK_RESPONSE_CANCEL,
                    _("_Help"), GTK_RESPONSE_HELP,
                    NULL);
#if HAVE_MACOSX_DESKTOP
    libbalsa_macosx_menu_for_parent(cdd->dialog, GTK_WINDOW(balsa_app.main_window));
#endif
    g_object_add_weak_pointer(G_OBJECT(cdd->dialog),
                              (gpointer) &cdd->dialog);
    /* `Enter' key => Create: */
    gtk_dialog_set_default_response(GTK_DIALOG(cdd->dialog), GTK_RESPONSE_OK);

    if (cdd->mbnode) {
        g_object_set_data_full(G_OBJECT(cdd->mbnode),
                               BALSA_FOLDER_CONF_IMAP_KEY, sdd, 
                               (GDestroyNotify) folder_conf_destroy_cdd);
    } else {
        sdd_new = sdd;
        g_object_add_weak_pointer(G_OBJECT(cdd->dialog),
                                  (gpointer) &sdd_new);
    }

    grid = libbalsa_create_grid();
    gtk_grid_set_row_spacing(GTK_GRID(grid), 6);
    gtk_grid_set_column_spacing(GTK_GRID(grid), 12);
    g_object_set(G_OBJECT(grid), "margin", 12, NULL);
    if (mn)
        content = grid;
    else {
        content = gtk_frame_new(_("Create subfolder"));
        gtk_container_add(GTK_CONTAINER(content), grid);
    }
    gtk_widget_set_vexpand(content, TRUE);
    gtk_container_add(GTK_CONTAINER(gtk_dialog_get_content_area(GTK_DIALOG(cdd->dialog))), content);
 
    row = 0;
    /* INPUT FIELD CREATION */
    label= libbalsa_create_grid_label(_("_Folder name:"), grid, row);
    sdd->folder_name =
        libbalsa_create_grid_entry(grid, G_CALLBACK(validate_sub_folder),
                                   sdd, row, sdd->old_folder, label);

    ++row;
    (void) libbalsa_create_grid_label(_("Host:"), grid, row);

    server = cdd->mbnode != NULL ? balsa_mailbox_node_get_server(cdd->mbnode) : NULL;
    sdd->host_label =
        gtk_label_new(server != NULL ? libbalsa_server_get_host(server) : "");
    gtk_widget_set_halign(sdd->host_label, GTK_ALIGN_START);
    gtk_widget_set_hexpand(sdd->host_label, TRUE);
    gtk_grid_attach(GTK_GRID(grid), sdd->host_label, 1, row, 1, 1);

    ++row;
    (void) libbalsa_create_grid_label(_("Subfolder of:"), grid, row);
    sdd->parent_folder = gtk_entry_new();
    gtk_editable_set_editable(GTK_EDITABLE(sdd->parent_folder), FALSE);
    gtk_editable_set_text(GTK_EDITABLE(sdd->parent_folder), sdd->old_parent);

    button = gtk_button_new_with_mnemonic(_("_Browse…"));
    g_signal_connect(button, "clicked",
		     G_CALLBACK(browse_button_cb), sdd);

    hbox = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 12);
    gtk_widget_set_hexpand(sdd->parent_folder, TRUE);
    gtk_container_add(GTK_CONTAINER(hbox), sdd->parent_folder);
    gtk_container_add(GTK_CONTAINER(hbox), button);
    gtk_widget_set_hexpand(hbox, TRUE);
    gtk_grid_attach(GTK_GRID(grid), hbox, 1, row, 1, 1);

    if (!mn)
        validate_sub_folder(NULL, sdd);
    else {
        static const char *std_acls[] = {
            "lrs", N_("read-only"),
            "lrswipkxte", N_("read-write"),
            "lrswipkxtea", N_("admin"),
            "lrsp", N_("post"),
            "lrsip", N_("append"),
            "lrxte", N_("delete"),
            NULL, N_("special") };
        GString *rights_str;
        gchar * rights;
        gchar * quotas;
        gboolean readonly;
        LibBalsaMailbox *mailbox;

        ++row;
        (void) libbalsa_create_grid_label(_("Permissions:"), grid, row);

        /* mailbox closed: no detailed permissions available */
        mailbox = balsa_mailbox_node_get_mailbox(mn);
        readonly = libbalsa_mailbox_get_readonly(mailbox);
        if (!libbalsa_mailbox_imap_is_connected(LIBBALSA_MAILBOX_IMAP(mailbox))) {
            rights_str = g_string_new(std_acls[readonly ? 1 : 3]);
            rights_str =
                g_string_append(rights_str,
                                _("\ndetailed permissions are available only for open folders"));
        } else {
            rights = libbalsa_imap_get_rights(LIBBALSA_MAILBOX_IMAP(mailbox));
            if (!rights) {
                rights_str = g_string_new(std_acls[readonly ? 1 : 3]);
                rights_str =
                    g_string_append(rights_str,
                                    _("\nthe server does not support ACLs"));
            } else {
                gint n;
                gchar **acls;

                /* my rights */
                for (n = 0;
                     g_strcmp0(std_acls[n], rights) != 0;
                     n += 2);
                rights_str = g_string_new(_("mine: "));
                if (std_acls[n])
                    rights_str = g_string_append(rights_str, std_acls[n + 1]);
                else
                    g_string_append_printf(rights_str, "%s (%s)",
                                           std_acls[n + 1], rights);

                /* acl's - only available if I have admin privileges */
                if ((acls =
                     libbalsa_imap_get_acls(LIBBALSA_MAILBOX_IMAP(mailbox))) != NULL) {
                    int uid;

                    for (uid = 0; acls[uid]; uid += 2) {
                        for (n = 0;
                             g_strcmp0(std_acls[n], acls[uid + 1]) != 0;
                             n += 2);
                        if (std_acls[n])
                            g_string_append_printf(rights_str,
                                                   "\nuid '%s': %s",
                                                   acls[uid], std_acls[n + 1]);
                        else
                            g_string_append_printf(rights_str,
                                                   "\nuid '%s': %s (%s)",
                                                   acls[uid], std_acls[n + 1],
                                                   acls[uid + 1]);
                    }
                    g_strfreev(acls);
                }
                g_free(rights);
            }
        }
        rights = g_string_free(rights_str, FALSE);
        label = gtk_label_new(rights);
        gtk_widget_set_halign(label, GTK_ALIGN_START);
        gtk_grid_attach(GTK_GRID(grid), label, 1, row, 1, 1);
        g_free(rights);

        ++row;
        (void) libbalsa_create_grid_label(_("Quota:"), grid, row);

        /* mailbox closed: no quota available */
        if (!libbalsa_mailbox_imap_is_connected(LIBBALSA_MAILBOX_IMAP(mailbox)))
            quotas = g_strdup(_("quota information available only for open folders"));
        else {
            gulong max, used;

            if (!libbalsa_imap_get_quota(LIBBALSA_MAILBOX_IMAP(mailbox), &max, &used))
                quotas = g_strdup(_("the server does not support quotas"));
            else if (max == 0 && used == 0)
                quotas = g_strdup(_("no limits"));
            else {
                gchar *use_str = libbalsa_size_to_gchar(used * G_GUINT64_CONSTANT(1024));
                gchar *max_str = libbalsa_size_to_gchar(max * G_GUINT64_CONSTANT(1024));

                quotas = g_strdup_printf(_("%s of %s (%.1f%%) used"), use_str, max_str,
                                         100.0 * (float) used / (float) max);
                g_free(use_str);
                g_free(max_str);
            }
        }
        label = gtk_label_new(quotas);
        gtk_widget_set_halign(label, GTK_ALIGN_START);
        gtk_widget_set_hexpand(label, TRUE);
        gtk_grid_attach(GTK_GRID(grid), label, 1, row, 1, 1);
        g_free(quotas);

        sdd->mcv = mailbox_conf_view_new(mailbox,
                                         GTK_WINDOW(cdd->dialog),
                                         grid, 5,
                                         G_CALLBACK(set_ok_sensitive));
    }

    gtk_widget_grab_focus(sdd->folder_name);

    g_signal_connect(cdd->dialog, "response",
                     G_CALLBACK(folder_conf_response), sdd);
    gtk_dialog_set_response_sensitive(GTK_DIALOG(cdd->dialog),
                                      GTK_RESPONSE_OK, FALSE);
    gtk_widget_show(cdd->dialog);
}

void
folder_conf_delete(BalsaMailboxNode* mbnode)
{
    GtkWidget* ask;
    gint response;

    if(!balsa_mailbox_node_get_config_prefix(mbnode)) {
        balsa_information(LIBBALSA_INFORMATION_ERROR,
	                  _("This folder is not stored in configuration. "
	                    "I do not yet know how to remove it "
                            "from remote server."));
	return;
    }
	
    ask = gtk_message_dialog_new(GTK_WINDOW(balsa_app.main_window), 0,
                                 GTK_MESSAGE_QUESTION,
                                 GTK_BUTTONS_OK_CANCEL,
                                 _("This will remove the folder "
                                   "“%s” from the list.\n"
                                   "You may use “New IMAP Folder” "
                                   "later to add this folder again.\n"),
                                 balsa_mailbox_node_get_name(mbnode));
#if HAVE_MACOSX_DESKTOP
    libbalsa_macosx_menu_for_parent(ask, GTK_WINDOW(balsa_app.main_window));
#endif
    gtk_window_set_title(GTK_WINDOW(ask), _("Confirm"));

    response = gtk_dialog_run(GTK_DIALOG(ask));
    gtk_widget_destroy(ask);
    if(response != GTK_RESPONSE_OK)
	return;

    /* Delete it from the config file and internal nodes */
    config_folder_delete(mbnode);

    /* Remove the node from balsa's mailbox list */
    balsa_mblist_mailbox_node_remove(mbnode);
    update_mail_servers();
}

void
folder_conf_add_imap_cb(GtkWidget * widget, gpointer data)
{
    folder_conf_imap_node(NULL);
}

void
folder_conf_add_imap_sub_cb(GtkWidget * widget, gpointer data)
{
    folder_conf_imap_sub_node(NULL);
}

void
folder_conf_edit_imap_cb(GtkWidget * widget, gpointer data)
{
    folder_conf_imap_node(BALSA_MAILBOX_NODE(data));
}

void
folder_conf_delete_cb(GtkWidget * widget, gpointer data)
{
    folder_conf_delete(BALSA_MAILBOX_NODE(data));
}
