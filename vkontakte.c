// disable gdk_thread_enter\leave warnings
#define GDK_VERSION_MIN_REQUIRED GDK_VERSION_3_4

#include <gtk/gtk.h>
#include <deadbeef/deadbeef.h>
#include <deadbeef/gtkui_api.h>
#include <json-glib/json-glib.h>

#include "common-defs.h"
#include "vk-api.h"
#include "ui.h"

DB_functions_t *deadbeef;
ddb_gtkui_t *gtkui_plugin;

static VkAuthData *vk_auth_data = NULL;

static intptr_t http_tid;	// thread for communication

typedef struct {
    gchar *query;
    GtkTreeModel *store;
} SeachQuery;

#define VK_AUTH_APP_ID "3035566"
#define VK_AUTH_REDIR_URL "http://scorpspot.blogspot.com/p/vk-id.html"
#define VK_AUTH_URL "http://oauth.vkontakte.ru/authorize?client_id=" VK_AUTH_APP_ID \
		"&scope=audio,friends&redirect_uri=" VK_AUTH_REDIR_URL \
		"&response_type=token"


#define CONF_VK_AUTH_URL "vk.auth.url"
#define CONF_VK_AUTH_DATA "vk.auth.data"


// util.c
gchar *     http_get_string(const gchar *url, GError **error);


static void
parse_audio_track_callback (VkAudioTrack *track, guint index, gpointer userdata) {
	GtkTreeModel *treestore;
	GtkTreeIter iter;

	treestore = GTK_TREE_MODEL (userdata);

	// write to list store
	gtk_list_store_append(GTK_LIST_STORE (treestore), &iter);
	gtk_list_store_set (GTK_LIST_STORE (treestore), &iter,
	                    ARTIST_COLUMN, track->artist,
	                    TITLE_COLUMN, track->title,
	                    DURATION_COLUMN, track->duration,
	                    URL_COLUMN, track->url,
	                    -1);
}

static void
parse_audio_resp (GtkTreeModel *treestore, const gchar *resp_str) {
	GError *error;

	gdk_threads_enter ();
	gtk_list_store_clear (GTK_LIST_STORE (treestore));

	if (!vk_audio_response_parse (resp_str, parse_audio_track_callback, treestore, &error)) {
		show_message(GTK_MESSAGE_ERROR, error->message);
		g_error_free (error);
	}
	gdk_threads_leave ();
}

void
vk_add_track_from_tree_model_to_playlist (GtkTreeModel *treestore, GtkTreeIter *iter) {
	gchar *artist;
	gchar *title;
	int duration;
	gchar *url;
	ddb_playlist_t *plt;
	
	gtk_tree_model_get (treestore, iter,
	                    ARTIST_COLUMN, &artist,
	                    TITLE_COLUMN, &title,
	                    DURATION_COLUMN, &duration,
	                    URL_COLUMN, &url,
	                    -1);
	plt = deadbeef->plt_get_curr ();
	                    
	DB_playItem_t *pt;
	int pabort = 0;
	
	deadbeef->pl_lock();
	if (!deadbeef->pl_add_files_begin (plt) ) {
		pt = deadbeef->plt_insert_file (plt, NULL, url, &pabort, NULL, NULL);
		deadbeef->pl_add_meta (pt, "artist", artist);
		deadbeef->pl_add_meta (pt, "title", title);
		deadbeef->plt_set_item_duration (plt, pt, duration);
		deadbeef->pl_add_files_end();
		// there is no legal way to refresh the playlist :(
	}
	deadbeef->pl_unlock();
	
	deadbeef->plt_unref (plt);
	g_free (artist);
	g_free (title);
	g_free (url);
}

static void
vk_search_audio_thread_func (void *ctx) {
	SeachQuery *query = (SeachQuery *) ctx;
	GError *error;
	gchar *resp_str;

	char *method_url = g_strdup_printf (VK_API_METHOD_AUDIO_SEARCH "?access_token=%s&q=%s",
										vk_auth_data->access_token,
										query->query);

	resp_str = http_get_string (method_url, &error);
	if (NULL == resp_str) {
	    trace ("VK error: %s\n", error->message);
	    g_error_free (error);
	} else {
	    parse_audio_resp (query->store, resp_str);
	    g_free (resp_str);
	}

	g_free (method_url);
	g_free (query->query);
	g_free (query);
	http_tid = 0;
}

void
vk_search_music (const gchar *query_text, GtkTreeModel *liststore) {
    SeachQuery *query;

    if (http_tid) {
        trace("Killing http thread\n");
        deadbeef->thread_detach (http_tid);

        http_tid = 0;
    }
    trace("== Searching for %s\n", query_text);

    query = g_malloc (sizeof(SeachQuery));
    query->query = g_strdup (query_text);
    query->store = liststore;

    http_tid = deadbeef->thread_start (vk_search_audio_thread_func, query);
}

static gboolean
vk_action_gtk (void *data) {
    GtkWidget *add_tracks_dlg;

	if (vk_auth_data == NULL) {
		// not authenticated, show warning and that's it
		gdk_threads_enter();
		show_message (GTK_MESSAGE_WARNING,
		              "To be able to use VKontakte plugin you need to provide "
		              "your authentication details. Please visit plugin configuration. "
		              "Then you will be able to add tracks from VK.com");
		gdk_threads_leave();
		return FALSE;
	}
	
    add_tracks_dlg = vk_create_add_tracks_dlg ();
    gtk_widget_set_size_request (add_tracks_dlg, 400, 400);
    gtk_window_set_transient_for (GTK_WINDOW (add_tracks_dlg),
                                  GTK_WINDOW (gtkui_plugin->get_mainwin() ) );
    gtk_widget_show (add_tracks_dlg);
    return FALSE;
}

static int
vk_action_callback (DB_plugin_action_t *action,
                    void *user_data) {
	g_idle_add (vk_action_gtk, NULL);
	return 0;
}

static void
vk_config_changed() {
    // restore auth url if it was occasionally changed
    deadbeef->conf_set_str (CONF_VK_AUTH_URL, VK_AUTH_URL);

    // read VK auth data
    const gchar *auth_data_str = deadbeef->conf_get_str_fast (CONF_VK_AUTH_DATA, NULL);
    vk_auth_data_free (vk_auth_data);
    vk_auth_data = vk_auth_data_parse (auth_data_str);
}

static int
vk_ddb_connect() {
#if GTK_CHECK_VERSION(3,0,0)
	gtkui_plugin = (ddb_gtkui_t *) deadbeef->plug_get_for_id ("gtkui3");
#else
	gtkui_plugin = (ddb_gtkui_t *) deadbeef->plug_get_for_id ("gtkui");
#endif
	
	if (!gtkui_plugin) {
		return -1;
	}
	
	return 0;
}

static DB_plugin_action_t vk_action = {
    .title = "File/Add tracks from VK",
    .name = "vk_add_tracks",
    .flags = DB_ACTION_COMMON,
    .callback = vk_action_callback,
    .next = NULL,
};

static DB_plugin_action_t *
vk_ddb_getactions (DB_playItem_t *it) {
    return &vk_action;
}

/**
 * DeadBeef messages handler.
 *
 * @param id message id.
 * @param ctx ?
 * @param p1 ?
 * @param p2 ?
 * @return ?
 */
static int
vk_ddb_message (uint32_t id, uintptr_t ctx, uint32_t p1, uint32_t p2) {
	switch (id) {
	case DB_EV_CONFIGCHANGED:
		vk_config_changed();
		break;
	}
	return 0;
}

static int
vk_ddb_stop() {
    vk_auth_data_free(vk_auth_data);
    return 0;
}

static const char vk_config_dlg[] =
    "property \"Navigate to the URL in text box\n(don't change the URL here)\" entry " CONF_VK_AUTH_URL " " VK_AUTH_URL ";\n"
    "property \"Paste data from the page here\" entry " CONF_VK_AUTH_DATA " \"\";\n";
	
DB_misc_t plugin = {
	.plugin.api_vmajor = 1,
	.plugin.api_vminor = 0,
	.plugin.type = DB_PLUGIN_MISC,
	.plugin.version_major = 1,
	.plugin.version_minor = 0,
	.plugin.id = "vkontakte",
	.plugin.name = "VKontakte",
	.plugin.descr = "Play music from VKontakte social network site.\n",
	.plugin.copyright =
	        "TODO (C) scorpp\n",
	.plugin.website = "https://github.com/scorpp/db-vk",
	.plugin.configdialog = vk_config_dlg,
	.plugin.stop = vk_ddb_stop,
	.plugin.connect = vk_ddb_connect,
	.plugin.message = vk_ddb_message,
	.plugin.get_actions = vk_ddb_getactions,
};

DB_plugin_t *
vkontakte_load (DB_functions_t *api) {
	deadbeef = api;
	return &plugin.plugin;
}
