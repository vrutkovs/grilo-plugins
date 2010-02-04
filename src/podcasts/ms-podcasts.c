/*
 * Copyright (C) 2010 Igalia S.L.
 *
 * Contact: Iago Toral Quiroga <itoral@igalia.com>
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public License
 * as published by the Free Software Foundation; version 2.1 of
 * the License, or (at your option) any later version.
 *
 * This library is distributed in the hope that it will be useful, but
 * WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with this library; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin St, Fifth Floor, Boston, MA
 * 02110-1301 USA
 *
 */

#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

#include <media-store.h>
#include <gio/gio.h>
#include <libxml/parser.h>
#include <libxml/xmlmemory.h>
#include <sqlite3.h>
#include <string.h>
#include <stdlib.h>

#include "ms-podcasts.h"

#define MS_PODCASTS_GET_PRIVATE(object)				\
  (G_TYPE_INSTANCE_GET_PRIVATE((object), MS_PODCASTS_SOURCE_TYPE, MsPodcastsPrivate))

/* --------- Logging  -------- */ 

#undef G_LOG_DOMAIN
#define G_LOG_DOMAIN "ms-podcasts"

/* --- Database --- */

#define MS_SQL_DB        ".ms-podcasts"

#define MS_SQL_CREATE_TABLE_PODCASTS				\
  "CREATE TABLE IF NOT EXISTS podcasts ("			\
  "id    INTEGER  PRIMARY KEY AUTOINCREMENT,"			\
  "title TEXT,"							\
  "url   TEXT,"							\
  "desc  TEXT,"							\
  "last_refreshed DATE)"

#define MS_SQL_CREATE_TABLE_STREAMS					\
  "CREATE TABLE IF NOT EXISTS streams ("				\
  "podcast INTEGER REFERENCES podcasts (id),"				\
  "url     TEXT PRIMARY KEY,"						\
  "title   TEXT,"							\
  "length  INTEGER,"							\
  "mime    TEXT,"							\
  "date    TEXT,"							\
  "desc    TEXT)"

#define MS_SQL_GET_PODCASTS			\
  "SELECT * FROM podcasts LIMIT %u OFFSET %u"

#define MS_SQL_GET_PODCASTS_BY_TEXT			\
  "SELECT * FROM podcasts "				\
  "WHERE title LIKE '%%%s%%' OR desc LIKE '%%%s%%' "	\
  "LIMIT %u OFFSET %u"

#define MS_SQL_GET_PODCASTS_BY_QUERY			\
  "SELECT * FROM podcasts "				\
  "WHERE %s "						\
  "LIMIT %u OFFSET %u"

#define MS_SQL_GET_PODCAST_BY_ID			\
  "SELECT * FROM podcasts "				\
  "WHERE id='%s' "					\
  "LIMIT 1"

#define MS_SQL_STORE_STREAM			     \
  "INSERT INTO streams "			     \
  "(podcast, url, title, length, mime, date, desc) " \
  "VALUES (?, ?, ?, ?, ?, ?, ?)"

#define MS_SQL_DELETE_PODCAST_STREAMS		     \
  "DELETE FROM streams WHERE podcast='%s'"

#define MS_SQL_GET_PODCAST_STREAMS		     \
  "SELECT * FROM streams "			     \
  "WHERE podcast='%s' "				     \
  "LIMIT %u  OFFSET %u"

#define MS_SQL_GET_PODCAST_STREAM		     \
  "SELECT * FROM streams "			     \
  "WHERE url='%s' "				     \
  "LIMIT 1"

#define MS_SQL_TOUCH_PODCAST			\
  "UPDATE podcasts "				\
  "SET last_refreshed='%s' "			\
  "WHERE id='%s'"

/* --- Other --- */

#define CACHE_DURATION (24 * 60 * 60)

/* --- Plugin information --- */

#define PLUGIN_ID   "ms-podcasts"
#define PLUGIN_NAME "Podcasts"
#define PLUGIN_DESC "A plugin for browsing podcasts"

#define SOURCE_ID   "ms-podcasts"
#define SOURCE_NAME "Podcasts"
#define SOURCE_DESC "A source for browsing podcasts"

#define AUTHOR      "Igalia S.L."
#define LICENSE     "LGPL"
#define SITE        "http://www.igalia.com"

enum {
  PODCAST_ID = 0,
  PODCAST_TITLE,
  PODCAST_URL,
  PODCAST_DESC,
  PODCAST_LAST_REFRESHED,
};

enum {
  STREAM_PODCAST = 0,
  STREAM_URL,
  STREAM_TITLE,
  STREAM_LENGTH,
  STREAM_MIME,
  STREAM_DATE,
  STREAM_DESC,
};

typedef void (*AsyncReadCbFunc) (gchar *data, gpointer user_data);

typedef struct {
  AsyncReadCbFunc callback;
  gchar *url;
  gpointer user_data;
} AsyncReadCb;

typedef struct {
  gchar *id;
  gchar *url;
  gchar *title;
  gchar *published;
  gchar *duration;
  gchar *summary;
  gchar *mime;
} Entry; 

struct _MsPodcastsPrivate {
  sqlite3 *db;
};

typedef struct {
  MsMediaSource *source;
  guint operation_id;
  const gchar *media_id;
  guint skip;
  guint count;
  const gchar *text;
  MsMediaSourceResultCb callback;
  guint error_code;
  gboolean is_query;
  gpointer user_data;
} OperationSpec;

static MsPodcastsSource *ms_podcasts_source_new (void);

static void ms_podcasts_source_finalize (GObject *plugin);

static const GList *ms_podcasts_source_supported_keys (MsMetadataSource *source);

static void ms_podcasts_source_browse (MsMediaSource *source,
				      MsMediaSourceBrowseSpec *bs);
static void ms_podcasts_source_search (MsMediaSource *source,
				       MsMediaSourceSearchSpec *ss);
static void ms_podcasts_source_query (MsMediaSource *source,
				      MsMediaSourceQuerySpec *qs);
static void ms_podcasts_source_metadata (MsMediaSource *source,
					 MsMediaSourceMetadataSpec *ms);


/* =================== Podcasts Plugin  =============== */

static gboolean
ms_podcasts_plugin_init (MsPluginRegistry *registry, const MsPluginInfo *plugin)
{
  g_debug ("podcasts_plugin_init\n");

  MsPodcastsSource *source = ms_podcasts_source_new ();
  ms_plugin_registry_register_source (registry, plugin, MS_MEDIA_PLUGIN (source));
  return TRUE;
}

MS_PLUGIN_REGISTER (ms_podcasts_plugin_init, 
                    NULL, 
                    PLUGIN_ID,
                    PLUGIN_NAME, 
                    PLUGIN_DESC, 
                    PACKAGE_VERSION,
                    AUTHOR, 
                    LICENSE, 
                    SITE);

/* ================== Podcasts GObject ================ */

static MsPodcastsSource *
ms_podcasts_source_new (void)
{
  g_debug ("ms_podcasts_source_new");
  return g_object_new (MS_PODCASTS_SOURCE_TYPE,
		       "source-id", SOURCE_ID,
		       "source-name", SOURCE_NAME,
		       "source-desc", SOURCE_DESC,
		       NULL);
}

static void
ms_podcasts_source_class_init (MsPodcastsSourceClass * klass)
{
  GObjectClass *gobject_class = G_OBJECT_CLASS (klass);
  MsMediaSourceClass *source_class = MS_MEDIA_SOURCE_CLASS (klass);
  MsMetadataSourceClass *metadata_class = MS_METADATA_SOURCE_CLASS (klass);

  gobject_class->finalize = ms_podcasts_source_finalize;

  source_class->browse = ms_podcasts_source_browse;
  source_class->search = ms_podcasts_source_search;
  source_class->query = ms_podcasts_source_query;
  source_class->metadata = ms_podcasts_source_metadata;

  metadata_class->supported_keys = ms_podcasts_source_supported_keys;

  g_type_class_add_private (klass, sizeof (MsPodcastsPrivate));
}

static void
ms_podcasts_source_init (MsPodcastsSource *source)
{
  gint r;
  const gchar *home;
  gchar *db_path;
  gchar *sql_error = NULL;

  source->priv = MS_PODCASTS_GET_PRIVATE (source);
  memset (source->priv, 0, sizeof (MsPodcastsPrivate));

  home = g_getenv ("HOME");
  if (!home) {
    g_warning ("$HOME not set, cannot open database");
    return;
  }

  g_debug ("Opening database connection...");
  db_path = g_strconcat (home, G_DIR_SEPARATOR_S, MS_SQL_DB, NULL);
  r = sqlite3_open (db_path, &source->priv->db);
  if (r) {
    g_critical ("Failed to open database '%s': %s",
		db_path, sqlite3_errmsg (source->priv->db));
    sqlite3_close (source->priv->db);
    return;
  }
  g_debug ("  OK");

  g_debug ("Checking database tables...");
  r = sqlite3_exec (source->priv->db, MS_SQL_CREATE_TABLE_PODCASTS,
		    NULL, NULL, &sql_error);

  if (!r) {
    /* TODO: if this fails, sqlite stays in an unreliable state fix that */
    r = sqlite3_exec (source->priv->db, MS_SQL_CREATE_TABLE_STREAMS,
		      NULL, NULL, &sql_error);
  }
  if (r) {
    if (sql_error) {
      g_warning ("Failed to create database tables: %s", sql_error);
      sqlite3_free (sql_error);
      sql_error = NULL;
    } else {
      g_warning ("Failed to create database tables.");      
    }
    sqlite3_close (source->priv->db);
    return;
  }
  g_debug ("  OK");
  
  g_free (db_path);
}

G_DEFINE_TYPE (MsPodcastsSource, ms_podcasts_source, MS_TYPE_MEDIA_SOURCE);

static void
ms_podcasts_source_finalize (GObject *object)
{
  MsPodcastsSource *source;
  
  g_debug ("ms_podcasts_source_finalize");

  source = MS_PODCASTS_SOURCE (object);

  sqlite3_close (source->priv->db);

  G_OBJECT_CLASS (ms_podcasts_source_parent_class)->finalize (object);
}

/* ======================= Utilities ==================== */

static void
print_entry (Entry *entry)
{
  g_print ("Entry Information:\n");
  g_print ("            ID: %s\n", entry->id);
  g_print ("         Title: %s\n", entry->title);
  g_print ("          Date: %s\n", entry->published);
  g_print ("      Duration: %s\n", entry->duration);
  g_print ("       Summary: %s\n", entry->summary);
  g_print ("           URL: %s\n", entry->url);
  g_print ("          Mime: %s\n", entry->mime);
}

static void
free_entry (Entry *entry)
{
  g_free (entry->id);
  g_free (entry->title);
  g_free (entry->published);
  g_free (entry->summary);
  g_free (entry->url);
  g_free (entry->mime);
}

static void
read_done_cb (GObject *source_object,
              GAsyncResult *res,
              gpointer user_data)
{
  AsyncReadCb *arc = (AsyncReadCb *) user_data;
  GError *vfs_error = NULL;
  gchar *content = NULL;

  g_debug ("  Done");

  g_file_load_contents_finish (G_FILE (source_object),
                               res,
                               &content,
                               NULL,
                               NULL,
                               &vfs_error);
  g_object_unref (source_object);
  if (vfs_error) {
    g_warning ("Failed to open '%s': %s", arc->url, vfs_error->message);
  } else {
    arc->callback (content, arc->user_data);
  }
  g_free (arc->url);
  g_free (arc);
}

static void
read_url_async (const gchar *url,
                AsyncReadCbFunc callback,
                gpointer user_data)
{
  GVfs *vfs;
  GFile *uri;
  AsyncReadCb *arc;

  vfs = g_vfs_get_default ();

  g_debug ("Opening async '%s'", url);

  arc = g_new0 (AsyncReadCb, 1);
  arc->url = g_strdup (url);
  arc->callback = callback;
  arc->user_data = user_data;
  uri = g_vfs_get_file_for_uri (vfs, url);
  g_file_load_contents_async (uri, NULL, read_done_cb, arc);
}

static gint
duration_to_seconds (const gchar *str)
{
  gint seconds = 0;
  gchar **parts;
  gint i;
  guint multiplier = 1;

  if (!str || str[0] == '\0') {
    return -1;
  }

  parts = g_strsplit (str, ":", 3);

  /* Get last portion (seconds) */
  i = 0;
  while (parts[i]) i++;
  if (i == 0) {
    g_strfreev (parts);
    return -1;
  } else {
    i--;
  }
  
  do {
    seconds += atoi (parts[i]) * multiplier;
    multiplier *= 60;
    i--;
  } while (i >= 0);
  
  g_strfreev (parts);

  return seconds;
}

static gboolean
mime_is_video (const gchar *mime)
{
  return mime && strstr (mime, "video") != NULL;
}

static gboolean
mime_is_audio (const gchar *mime)
{
  return mime && strstr (mime, "audio") != NULL;
}

static MsContentMedia *
build_media_from_stmt (MsContentMedia *content,
		       sqlite3_stmt *sql_stmt,
		       gboolean is_podcast)
{
  MsContentMedia *media = NULL;
  gchar *id;
  gchar *title;
  gchar *url;
  gchar *desc;
  gchar *mime;
  gchar *date;
  guint duration;
  gchar *podcast;

  if (content) {
    media = content;
  }

  if (is_podcast) {
    if (!media) {
      media = MS_CONTENT_MEDIA (ms_content_box_new ());
    }

    id = (gchar *) sqlite3_column_text (sql_stmt, PODCAST_ID);
    title = (gchar *) sqlite3_column_text (sql_stmt, PODCAST_TITLE);
    url = (gchar *) sqlite3_column_text (sql_stmt, PODCAST_URL);
    desc = (gchar *) sqlite3_column_text (sql_stmt, PODCAST_DESC);

    ms_content_media_set_id (media, id);
    ms_content_media_set_title (media, title);
    ms_content_media_set_url (media, url);
    ms_content_media_set_description (media, desc);
  } else { /* podcast stream */
    mime = (gchar *) sqlite3_column_text (sql_stmt, STREAM_MIME);
    podcast = (gchar *) sqlite3_column_text (sql_stmt, STREAM_PODCAST);
    url = (gchar *) sqlite3_column_text (sql_stmt, STREAM_URL);
    title = (gchar *) sqlite3_column_text (sql_stmt, STREAM_TITLE);
    date = (gchar *) sqlite3_column_text (sql_stmt, STREAM_DATE);
    desc = (gchar *) sqlite3_column_text (sql_stmt, STREAM_DESC);
    duration = sqlite3_column_int (sql_stmt, STREAM_LENGTH);

    if (!media) {
      if (mime_is_audio (mime)) {
	media = ms_content_audio_new ();
      } else if (mime_is_video (mime)) {
	media = ms_content_video_new ();
      } else {
	media = ms_content_media_new ();
      }
    }

    ms_content_media_set_id (media, url);
    ms_content_media_set_title (media, title);
    ms_content_media_set_url (media, url);
    ms_content_media_set_date (media, date);
    ms_content_media_set_description (media, desc);
    ms_content_media_set_mime (media, mime);
    if (duration > 0) {
      ms_content_media_set_duration (media, duration);
    }
  }

  return media;
}

static void
produce_podcast_contents_from_db (OperationSpec *os)
{
  sqlite3 *db;
  gchar *sql;
  sqlite3_stmt *sql_stmt = NULL;
  GList *iter, *medias = NULL;
  guint count = 0;
  MsContentMedia *media;
  gint r;
  GError *error = NULL;

  db = MS_PODCASTS_SOURCE (os->source)->priv->db;
  sql = g_strdup_printf (MS_SQL_GET_PODCAST_STREAMS,
			 os->media_id, os->count, os->skip);
  g_debug ("%s", sql);
  r = sqlite3_prepare_v2 (db, sql, strlen (sql), &sql_stmt, NULL); 
  g_free (sql);

  if (r != SQLITE_OK) {
    g_warning ("Failed to retrieve podcast streams: %s", sqlite3_errmsg (db));
    error = g_error_new (MS_ERROR,
			 os->error_code,
			 "Failed to retrieve podcast streams");
    os->callback (os->source, os->operation_id, NULL, 0, os->user_data, error);
    g_error_free (error);
    return;
  }
  
  while ((r = sqlite3_step (sql_stmt)) == SQLITE_BUSY);

  while (r == SQLITE_ROW) {
    media = build_media_from_stmt (NULL, sql_stmt, FALSE);
    medias = g_list_prepend (medias, media);
    count++;
    r = sqlite3_step (sql_stmt);
  }

  if (r != SQLITE_DONE) {
    g_warning ("Failed to retrive podcast streams: %s", sqlite3_errmsg (db));
    error = g_error_new (MS_ERROR,
			 os->error_code,
			 "Failed to retrieve podcast streams");
    os->callback (os->source, os->operation_id, NULL, 0, os->user_data, error);
    g_error_free (error);
    sqlite3_finalize (sql_stmt);
    return;
  }

  sqlite3_finalize (sql_stmt);

  if (count > 0) {
    medias = g_list_reverse (medias);
    iter = medias;
    while (iter) {
      media = MS_CONTENT_MEDIA (iter->data);
      os->callback (os->source,
		    os->operation_id,
		    media,
		    --count,
		    os->user_data,
		    NULL);
      iter = g_list_next (iter);
    }
    g_list_free (medias);
  } else {
    os->callback (os->source, os->operation_id, NULL, 0, os->user_data, NULL);
  }
}

static void
store_stream (sqlite3 *db, const gchar *podcast_id, Entry *entry)
{
  gint r;
  guint seconds;
  sqlite3_stmt *sql_stmt = NULL;

  seconds = duration_to_seconds (entry->duration);
  g_debug ("%s", MS_SQL_STORE_STREAM);  
  r = sqlite3_prepare_v2 (db,
			  MS_SQL_STORE_STREAM,
			  strlen (MS_SQL_STORE_STREAM),
			  &sql_stmt, NULL); 
  if (r != SQLITE_OK) {
    g_warning ("Failed to store podcast stream '%s': %s",
	       entry->url, sqlite3_errmsg (db));
    return;
  }

  sqlite3_bind_text (sql_stmt, 1, podcast_id, -1, SQLITE_STATIC);
  sqlite3_bind_text (sql_stmt, 2, entry->url, -1, SQLITE_STATIC);
  sqlite3_bind_text (sql_stmt, 3, entry->title, -1, SQLITE_STATIC);
  sqlite3_bind_int  (sql_stmt, 4, seconds);
  sqlite3_bind_text (sql_stmt, 5, entry->mime, -1, SQLITE_STATIC);
  sqlite3_bind_text (sql_stmt, 6, entry->published, -1, SQLITE_STATIC);
  sqlite3_bind_text (sql_stmt, 7, entry->summary, -1, SQLITE_STATIC);

  while ((r = sqlite3_step (sql_stmt)) == SQLITE_BUSY);

  if (r != SQLITE_DONE) {
    g_warning ("Failed to store podcast stream '%s': %s",
	       entry->url, sqlite3_errmsg (db));
  }

  sqlite3_finalize (sql_stmt);
}

static void
parse_entry (xmlDocPtr doc, xmlNodePtr entry, Entry *data)
{
  xmlNodePtr node;
  node = entry->xmlChildrenNode;
  while (node) {
    if (!xmlStrcmp (node->name, (const xmlChar *) "title")) {
      data->title = 
	(gchar *) xmlNodeListGetString (doc, node->xmlChildrenNode, 1);
    } else if (!xmlStrcmp (node->name, (const xmlChar *) "enclosure")) {
      data->id = (gchar *) xmlGetProp (node, (xmlChar *) "url");
      data->url = g_strdup (data->id);
      data->mime = (gchar *) xmlGetProp (node, (xmlChar *) "type");
    } else if (!xmlStrcmp (node->name, (const xmlChar *) "summary")) {
      data->summary = 
	(gchar *) xmlNodeListGetString (doc, node->xmlChildrenNode, 1);
    } else if (!xmlStrcmp (node->name, (const xmlChar *) "pubDate")) {
      data->published = 
	(gchar *) xmlNodeListGetString (doc, node->xmlChildrenNode, 1);
    } else if (!xmlStrcmp (node->name, (const xmlChar *) "duration")) {
      data->duration = 
	(gchar *) xmlNodeListGetString (doc, node->xmlChildrenNode, 1);
    }
    node = node->next;
  }
}

static void
touch_podcast (sqlite3 *db, const gchar *podcast_id)
{
  gint r;
  gchar *sql, *sql_error;
  GTimeVal now;
  gchar *now_str;

  g_get_current_time (&now);
  now_str = g_time_val_to_iso8601 (&now);

  sql = g_strdup_printf (MS_SQL_TOUCH_PODCAST, now_str, podcast_id);
  g_debug ("%s", sql);
  r = sqlite3_exec (db, sql, NULL, NULL, &sql_error);
  g_free (sql);

  if (r != SQLITE_OK) {
    g_warning ("Failed to touch podcast, '%s': %s", podcast_id, sql_error);
    sqlite3_free (sql_error);
    return;
  }
}

static void
parse_feed (OperationSpec *os, const gchar *str, GError **error)
{
  xmlDocPtr doc;
  xmlNodePtr node;
  gint r;
  gchar *sql;
  gchar *sql_error;

  g_debug ("parse_feed");

  doc = xmlRecoverDoc ((xmlChar *) str);
  if (!doc) {
    *error = g_error_new (MS_ERROR, 
			  os->error_code,
			  "Failed to parse podcast contents");
    goto free_resources;
  }

  node = xmlDocGetRootElement (doc);
  if (!node) {
    *error = g_error_new (MS_ERROR, 
			  os->error_code,
			  "Podcast contains no data");
    goto free_resources;
  }

  if (xmlStrcmp (node->name, (const xmlChar *) "rss")) {
    *error = g_error_new (MS_ERROR, 
			  os->error_code,
			  "Podcast is not in RSS format");
    goto free_resources;
  }

  /* TODO: handle various channels, maybe as categories within the podcast.
     Right now we only parse the first channel */

  /* Search first channel node */
  node = node->xmlChildrenNode;
  while (node) {
    if (!xmlStrcmp (node->name, (const xmlChar *) "channel")) {
      break;
    }
    node = node->next;
  }
  if (!node) {
    *error = g_error_new (MS_ERROR, 
			  os->error_code,
			  "Podcast contains no channels");
    goto free_resources;
  }
  
  node = node->xmlChildrenNode;
  if (!node) {
    *error = g_error_new (MS_ERROR, 
			  os->error_code,
			  "Podcast contains no channel data");
    goto free_resources;
  }
  
  /* The feed is ok, let's parse it and store the streams in the database */

  /* First we remove old entries */
  sql = g_strdup_printf (MS_SQL_DELETE_PODCAST_STREAMS, os->media_id);
  g_debug ("%s", sql);
  r = sqlite3_exec (MS_PODCASTS_SOURCE (os->source)->priv->db, sql,
		    NULL, NULL, &sql_error);
  g_free (sql);
  if (r) {
    g_warning ("Failed to remove podcast streams cache: %s", sql_error);
    sqlite3_free (error);
  }

  /* Then we parse the feed and store the streams */
  do {
    while (node && xmlStrcmp (node->name, (const xmlChar *) "item")) {
      node = node->next;
    }
    if (node) {
      Entry *entry = g_new0 (Entry, 1);
      parse_entry (doc, node, entry);
      if (0) print_entry (entry);
      store_stream (MS_PODCASTS_SOURCE (os->source)->priv->db,
		    os->media_id, entry);
      free_entry (entry);
      node = node->next;
    }
  } while (node);

  /* We also update the last_refreshed date of the podcast */
  touch_podcast (MS_PODCASTS_SOURCE (os->source)->priv->db, os->media_id);

  /* Now that we have parsed the feed and stored the contents, let's
     resolve the user's query */
  produce_podcast_contents_from_db (os);

 free_resources:
  xmlFreeDoc (doc);
  return;
}

static void
read_feed_cb (gchar *xmldata, gpointer user_data)
{
  GError *error = NULL;
  OperationSpec *os = (OperationSpec *) user_data;
  
  if (!xmldata) {
    error = g_error_new (MS_ERROR,
			 MS_ERROR_BROWSE_FAILED,
			 "Failed to read data from podcast");
  } else {
    parse_feed (os, xmldata, &error);
    g_free (xmldata);
  }
  
  if (error) {
    os->callback (os->source, 
		  os->operation_id,
		  NULL,
		  0,
		  os->user_data,
		  error);
    g_error_free (error);
  }
  g_free (os);
}

static void
produce_podcast_contents (OperationSpec *os)
{
  gint r;
  sqlite3_stmt *sql_stmt = NULL;
  sqlite3 *db;
  GError *error = NULL;
  gchar *sql;
  gchar *url;

  /* First we get some information about the podcast */
  db = MS_PODCASTS_SOURCE (os->source)->priv->db;
  sql = g_strdup_printf (MS_SQL_GET_PODCAST_BY_ID, os->media_id);
  g_debug ("%s", sql);
  r = sqlite3_prepare_v2 (db, sql, strlen (sql), &sql_stmt, NULL); 
  g_free (sql);

  if (r != SQLITE_OK) {
    g_warning ("Failed to retrieve podcast '%s': %s",
	       os->media_id, sqlite3_errmsg (db));
    error = g_error_new (MS_ERROR,
			 os->error_code,
			 "Failed to retrieve podcast information");
    os->callback (os->source, os->operation_id, NULL, 0, os->user_data, error);
    g_error_free (error);
    g_free (os);
    return;
  }
  
  while ((r = sqlite3_step (sql_stmt)) == SQLITE_BUSY);

  if (r == SQLITE_ROW) {    
    gchar *lr_str;
    GTimeVal lr;
    GTimeVal now;

    /* Check if we have to refresh the podcast */
    lr_str = (gchar *) sqlite3_column_text (sql_stmt, PODCAST_LAST_REFRESHED);
    g_debug ("Podcast last-refreshed: '%s'", lr_str);
    g_time_val_from_iso8601 (lr_str, &lr);
    g_get_current_time (&now);
    now.tv_sec -= CACHE_DURATION;
    if (now.tv_sec >= lr.tv_sec) {
      /* We have to read the podcast feed again */
      g_debug ("Refreshing podcast '%s'...", os->media_id);
      url = g_strdup ((gchar *) sqlite3_column_text (sql_stmt, PODCAST_URL));
      read_url_async (url, read_feed_cb, os);
      g_free (url);
    } else {
      /* We can read the podcast entries from the database cache */
      produce_podcast_contents_from_db (os);
      g_free (os);
    }
  } else {
    g_warning ("Failed to retrieve podcast information: %s",
	       sqlite3_errmsg (db));
    error = g_error_new (MS_ERROR,
			 os->error_code,
			 "Failed to retrieve podcast information");
    os->callback (os->source, os->operation_id, NULL, 0, os->user_data, error);
    g_error_free (error);
    g_free (os);
  }

  sqlite3_finalize (sql_stmt);
}

static void
produce_podcasts (OperationSpec *os)
{
  gint r;
  sqlite3_stmt *sql_stmt = NULL;
  sqlite3 *db;
  MsContentMedia *media;
  GError *error = NULL;
  GList *medias = NULL;
  guint count = 0;
  GList *iter;
  gchar *sql;

  db = MS_PODCASTS_SOURCE (os->source)->priv->db;

  if (!os->text) {
    /* Browse */
    sql = g_strdup_printf (MS_SQL_GET_PODCASTS, os->count, os->skip);
  } else if (os->is_query) {
    /* Query */
    sql = g_strdup_printf (MS_SQL_GET_PODCASTS_BY_QUERY,
			   os->text, os->count, os->skip);
  } else {
    /* Search */
    sql = g_strdup_printf (MS_SQL_GET_PODCASTS_BY_TEXT,
			   os->text, os->text, os->count, os->skip);
  }
  g_debug ("%s", sql);
  r = sqlite3_prepare_v2 (db, sql, strlen (sql), &sql_stmt, NULL); 
  g_free (sql);

  if (r != SQLITE_OK) {
    g_warning ("Failed to retrieve podcasts: %s", sqlite3_errmsg (db));
    error = g_error_new (MS_ERROR,
			 os->error_code,
			 "Failed to retrieve podcasts list");
    os->callback (os->source, os->operation_id, NULL, 0, os->user_data, error);
    g_error_free (error);
    goto free_resources;
  }
  
  while ((r = sqlite3_step (sql_stmt)) == SQLITE_BUSY);

  while (r == SQLITE_ROW) {
    media = build_media_from_stmt (NULL, sql_stmt, TRUE);
    medias = g_list_prepend (medias, media);
    count++;
    r = sqlite3_step (sql_stmt);
  }

  if (r != SQLITE_DONE) {
    g_warning ("Failed to retrieve podcasts: %s", sqlite3_errmsg (db));
    error = g_error_new (MS_ERROR,
			 os->error_code,
			 "Failed to retrieve podcasts list");
    os->callback (os->source, os->operation_id, NULL, 0, os->user_data, error);
    g_error_free (error);
    goto free_resources;
  }

  if (count > 0) {
    medias = g_list_reverse (medias);
    iter = medias;
    while (iter) {
      media = MS_CONTENT_MEDIA (iter->data);
      os->callback (os->source,
		    os->operation_id,
		    media,
		    --count,
		    os->user_data,
		    NULL);
      iter = g_list_next (iter);
    }
    g_list_free (medias);
  } else {
    os->callback (os->source, os->operation_id, NULL, 0, os->user_data, NULL);
  }

 free_resources:
  if (sql_stmt)
    sqlite3_finalize (sql_stmt);
}

static void
stream_metadata (MsMediaSourceMetadataSpec *ms)
{
  gint r;
  sqlite3_stmt *sql_stmt = NULL;
  sqlite3 *db;
  GError *error = NULL;
  gchar *sql;
  const gchar *id;

  db = MS_PODCASTS_SOURCE (ms->source)->priv->db;

  id = ms_content_media_get_id (ms->media);
  sql = g_strdup_printf (MS_SQL_GET_PODCAST_STREAM, id);
  g_debug ("%s", sql);
  r = sqlite3_prepare_v2 (db, sql, strlen (sql), &sql_stmt, NULL);
  g_free (sql);

  if (r != SQLITE_OK) {
    g_warning ("Failed to get podcast stream: %s", sqlite3_errmsg (db));
    error = g_error_new (MS_ERROR,
			 MS_ERROR_METADATA_FAILED,
			 "Failed to get podcast stream metadata");
    ms->callback (ms->source, ms->media, ms->user_data, error);
    g_error_free (error);
    return;
  }
  
  while ((r = sqlite3_step (sql_stmt)) == SQLITE_BUSY);

  if (r == SQLITE_ROW) {
    build_media_from_stmt (ms->media, sql_stmt, FALSE);
    ms->callback (ms->source, ms->media, ms->user_data, NULL);
  } else {
    g_warning ("Failed to get podcast stream: %s", sqlite3_errmsg (db));
    error = g_error_new (MS_ERROR,
			 MS_ERROR_METADATA_FAILED,
			 "Failed to get podcast stream metadata");
    ms->callback (ms->source, ms->media, ms->user_data, error);
    g_error_free (error);
  }

  sqlite3_finalize (sql_stmt);
}

static void
podcast_metadata (MsMediaSourceMetadataSpec *ms)
{
  gint r;
  sqlite3_stmt *sql_stmt = NULL;
  sqlite3 *db;
  GError *error = NULL;
  gchar *sql;
  const gchar *id;

  db = MS_PODCASTS_SOURCE (ms->source)->priv->db;

  id = ms_content_media_get_id (ms->media);
  if (!id) {
    /* Root category: special case */
    ms_content_media_set_title (ms->media, "");
    ms->callback (ms->source, ms->media, ms->user_data, NULL);
    return;
  }

  sql = g_strdup_printf (MS_SQL_GET_PODCAST_BY_ID, id);
  g_debug ("%s", sql);
  r = sqlite3_prepare_v2 (db, sql, strlen (sql), &sql_stmt, NULL); 
  g_free (sql);

  if (r != SQLITE_OK) {
    g_warning ("Failed to get podcast: %s", sqlite3_errmsg (db));
    error = g_error_new (MS_ERROR,
			 MS_ERROR_METADATA_FAILED,
			 "Failed to get podcast metadata");
    ms->callback (ms->source, ms->media, ms->user_data, error);
    g_error_free (error);
    return;
  }
  
  while ((r = sqlite3_step (sql_stmt)) == SQLITE_BUSY);

  if (r == SQLITE_ROW) {
    build_media_from_stmt (ms->media, sql_stmt, TRUE);
    ms->callback (ms->source, ms->media, ms->user_data, NULL);
  } else {
    g_warning ("Failed to get podcast: %s", sqlite3_errmsg (db));
    error = g_error_new (MS_ERROR,
			 MS_ERROR_METADATA_FAILED,
			 "Failed to get podcast metadata");
    ms->callback (ms->source, ms->media, ms->user_data, error);
    g_error_free (error);
  }

  sqlite3_finalize (sql_stmt);
}

/* ================== API Implementation ================ */

static const GList *
ms_podcasts_source_supported_keys (MsMetadataSource *source)
{
  static GList *keys = NULL;
  if (!keys) {
    keys = ms_metadata_key_list_new (MS_METADATA_KEY_ID,
				     MS_METADATA_KEY_TITLE, 
				     MS_METADATA_KEY_URL,
				     MS_METADATA_KEY_CHILDCOUNT,
				     MS_METADATA_KEY_SITE,
				     NULL);
  }
  return keys;
}

static void
ms_podcasts_source_browse (MsMediaSource *source, MsMediaSourceBrowseSpec *bs)
{
  g_debug ("ms_podcasts_source_browse");

  OperationSpec *os;
  MsPodcastsSource *podcasts_source;
  GError *error = NULL;

  podcasts_source = MS_PODCASTS_SOURCE (source);
  if (!podcasts_source->priv->db) {
    g_warning ("Can't execute operation: no database connection.");
    error = g_error_new (MS_ERROR,
			 MS_ERROR_BROWSE_FAILED,
			 "No database connection");
    bs->callback (bs->source, bs->browse_id, NULL, 0, bs->user_data, error);
    g_error_free (error);
  }

  /* Configure browse operation */
  os = g_new0 (OperationSpec, 1);
  os->source = bs->source;
  os->operation_id = bs->browse_id;
  os->media_id = ms_content_media_get_id (bs->container);
  os->count = bs->count;
  os->skip = bs->skip;
  os->callback = bs->callback;
  os->user_data = bs->user_data;
  os->error_code = MS_ERROR_BROWSE_FAILED;

  if (!os->media_id) {
    /* Browsing podcasts list */
    produce_podcasts (os);
    g_free (os);
  } else {
    /* Browsing a particular podcast. We may need to parse
       the feed (async) and in that case we will need to keep
       os, so we do not free os here */
    produce_podcast_contents (os);
  }
}

static void
ms_podcasts_source_search (MsMediaSource *source, MsMediaSourceSearchSpec *ss)
{
  g_debug ("ms_podcasts_source_search");

  MsPodcastsSource *podcasts_source;
  OperationSpec *os;
  GError *error = NULL;

  podcasts_source = MS_PODCASTS_SOURCE (source);
  if (!podcasts_source->priv->db) {
    g_warning ("Can't execute operation: no database connection.");
    error = g_error_new (MS_ERROR,
			 MS_ERROR_QUERY_FAILED,
			 "No database connection");
    ss->callback (ss->source, ss->search_id, NULL, 0, ss->user_data, error);
    g_error_free (error);
  }

  os = g_new0 (OperationSpec, 1);
  os->source = ss->source;
  os->operation_id = ss->search_id;
  os->text = ss->text;
  os->count = ss->count;
  os->skip = ss->skip;
  os->callback = ss->callback;
  os->user_data = ss->user_data;
  os->error_code = MS_ERROR_SEARCH_FAILED;
  produce_podcasts (os);
  g_free (os);
}

static void
ms_podcasts_source_query (MsMediaSource *source, MsMediaSourceQuerySpec *qs)
{
  g_debug ("ms_podcasts_source_query");

  MsPodcastsSource *podcasts_source;
  OperationSpec *os;
  GError *error = NULL;

  podcasts_source = MS_PODCASTS_SOURCE (source);
  if (!podcasts_source->priv->db) {
    g_warning ("Can't execute operation: no database connection.");
    error = g_error_new (MS_ERROR,
			 MS_ERROR_QUERY_FAILED,
			 "No database connection");
    qs->callback (qs->source, qs->query_id, NULL, 0, qs->user_data, error);
    g_error_free (error);
  }

  os = g_new0 (OperationSpec, 1);
  os->source = qs->source;
  os->operation_id = qs->query_id;
  os->text = qs->query;
  os->count = qs->count;
  os->skip = qs->skip;
  os->callback = qs->callback;
  os->user_data = qs->user_data;
  os->is_query = TRUE;
  os->error_code = MS_ERROR_QUERY_FAILED;
  produce_podcasts (os);
  g_free (os);
}

static void
ms_podcasts_source_metadata (MsMediaSource *source, MsMediaSourceMetadataSpec *ms)
{
  g_debug ("ms_podcasts_source_metadata");

  MsPodcastsSource *podcasts_source;
  GError *error = NULL;
  const gchar *media_id;
  
  podcasts_source = MS_PODCASTS_SOURCE (source);
  if (!podcasts_source->priv->db) {
    g_warning ("Can't execute operation: no database connection.");
    error = g_error_new (MS_ERROR,
			 MS_ERROR_METADATA_FAILED,
			 "No database connection");
    ms->callback (ms->source, ms->media, ms->user_data, error);
    g_error_free (error);
  }

  media_id = ms_content_media_get_id (ms->media);
  if (!media_id || g_ascii_strtoll (media_id, NULL, 10) != 0) {
    podcast_metadata (ms);
  } else {
    stream_metadata (ms);
  }
}