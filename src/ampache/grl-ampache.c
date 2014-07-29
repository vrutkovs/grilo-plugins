#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

#include "grl-ampache.h"
#include <grilo.h>
#include <glib.h>
#include <glib/gi18n-lib.h>
#include <net/grl-net.h>
#include <libxml/parser.h>
#include <libxml/xmlmemory.h>
#include <string.h>
#include <errno.h>

/* --------- Logging  -------- */

#define GRL_LOG_DOMAIN_DEFAULT ampache_log_domain
GRL_LOG_DOMAIN_STATIC(ampache_log_domain);

#define GRL_TRACE(x) GRL_DEBUG(__PRETTY_FUNCTION__)

#define MAX_ELEMENTS 100
#define AMPACHE_VERSION "350001"
/* ------- Categories ------- */
#define AMPACHE_ARTIST "artist"
#define AMPACHE_ALBUM "album"
#define AMPACHE_TRACK "song" 
/* ---- ampache Web API  ---- */

#define API_URL "%s/remote.php/ampache/server/xml.server.php?"
#define AMPACHE_ACTION API_URL "action="
#define FILTER "&filter=%s"
#define OFFSET "&offset=%u"
#define LIMIT  "&limit=%u"
#define AUTH "&auth=%s"

#define AMPACHE_HANDSHAKE AMPACHE_ACTION "handshake" AUTH
#define AMPACHE_GENERATE_AUTH AMPACHE_HANDSHAKE "&timestamp=%s&version=%s&user=%s"

#define AMPACHE_ALBUMS AMPACHE_ACTION "albums" AUTH OFFSET LIMIT
#define AMPACHE_ARTISTS AMPACHE_ACTION "artists" AUTH OFFSET LIMIT
#define AMPACHE_TRACKS AMPACHE_ACTION "songs" AUTH OFFSET LIMIT

#define AMPACHE_ARTIST_ALBUMS AMPACHE_ACTION "artist_albums" AUTH OFFSET LIMIT FILTER
#define AMPACHE_ALBUM_TRACKS AMPACHE_ACTION "album_songs" AUTH OFFSET LIMIT FILTER

#define AMPACHE_SEARCH_ALBUMS AMPACHE_ALBUMS FILTER
#define AMPACHE_SEARCH_ARTISTS AMPACHE_ARTISTS FILTER
#define AMPACHE_SEARCH_TRACKS AMPACHE_TRACKS FILTER
#define AMPACHE_SEARCH_ALL AMPACHE_ACTION "search_songs" AUTH FILTER

#define AMPACHE_ARTIST_URL AMPACHE_ACTION AMPACHE_ARTIST FILTER
#define AMPACHE_ALBUM_URL AMPACHE_ACTION AMPACHE_ALBUM FILTER
#define AMPACHE_TRACK_URL AMPACHE_ACTION AMPACHE_TRACK FILTER

/* --- Plugin information --- */
#define PLUGIN_ID   AMPACHE_PLUGIN_ID

#define SOURCE_ID   "grl-ampache"
#define SOURCE_NAME "Ampache"
#define SOURCE_DESC _("A source for browsing and searching Ampache streaming server")



enum {
  RESOLVE,
  BROWSE,
  QUERY,
  SEARCH
};

typedef enum {
  AMPACHE_ARTIST_CAT = 1,
  AMPACHE_ALBUM_CAT,
  AMPACHE_TRACK_CAT,
} AmpacheCategory;

typedef struct {
  AmpacheCategory category;
  gchar *id;
  gchar *artist_name;
  gchar *artist_id;
  gchar *album_name;
  gchar *album_id;
  gchar *album_image;
  gchar *track_name;
  gchar *track_url;
  gchar *track_duration;
} Entry;

typedef struct {
  gint type;
  union {
    GrlSourceBrowseSpec *bs;
    GrlSourceQuerySpec *qs;
    GrlSourceResolveSpec *rs;
    GrlSourceSearchSpec *ss;
  } spec;
  xmlNodePtr node;
  xmlDocPtr doc;
  guint total_results;
  guint index; 
  gboolean cancelled;
} XmlParseEntries;

struct _GrlAmpacheSourcePriv {
  gchar *api_key;
  gchar *host_url;
  GrlNetWc *wc;
  GCancellable *cancellable;
};

struct PluginInfo {
  GrlPlugin *plugin;
  gchar *host_url;
};

#define GRL_AMPACHE_SOURCE_GET_PRIVATE(object)		\
  (G_TYPE_INSTANCE_GET_PRIVATE((object),                \
                               GRL_AMPACHE_SOURCE_TYPE,	\
                               GrlAmpacheSourcePriv))

static GrlAmpacheSource *grl_ampache_source_new (const gchar *host_url,
                                                 const gchar *api_key);

gboolean grl_ampache_plugin_init (GrlRegistry *registry,
                                  GrlPlugin *plugin,
                                  GList *configs);

static const GList *grl_ampache_source_supported_keys (GrlSource *source);

static void grl_ampache_source_browse (GrlSource *source,
                                       GrlSourceBrowseSpec *bs);

static void grl_ampache_source_search (GrlSource *source,
                                       GrlSourceSearchSpec *ss);

static void grl_ampache_source_cancel (GrlSource *source,
                                       guint operation_id);
/* ================== Ampache GObject ================ */

static GrlAmpacheSource *
grl_ampache_source_new (const gchar *host_url, const gchar *api_key)
{
  GRL_TRACE();
  GrlAmpacheSource *source = GRL_AMPACHE_SOURCE(g_object_new (GRL_AMPACHE_SOURCE_TYPE,
		       "source-id", SOURCE_ID,
		       "source-name", SOURCE_NAME,
		       "source-desc", SOURCE_DESC,
		       "supported-media", GRL_MEDIA_TYPE_AUDIO,
		       NULL));
  source->priv->host_url = g_strdup(host_url);
  source->priv->api_key = g_strdup(api_key);
  return source;
}

G_DEFINE_TYPE (GrlAmpacheSource, grl_ampache_source, GRL_TYPE_SOURCE);

static void
grl_ampache_source_finalize (GObject *object)
{
  GrlAmpacheSource *self;

  self = GRL_AMPACHE_SOURCE (object);

  g_clear_object (&self->priv->wc);
  g_clear_object (&self->priv->cancellable);

  G_OBJECT_CLASS (grl_ampache_source_parent_class)->finalize (object);
}

static void
grl_ampache_source_class_init (GrlAmpacheSourceClass * klass)
{
  GObjectClass *g_class = G_OBJECT_CLASS (klass);
  GrlSourceClass *source_class = GRL_SOURCE_CLASS (klass);

  g_class->finalize = grl_ampache_source_finalize;

  source_class->cancel = grl_ampache_source_cancel;
  source_class->supported_keys = grl_ampache_source_supported_keys;
  source_class->browse = grl_ampache_source_browse;
  source_class->search = grl_ampache_source_search;

  g_type_class_add_private (klass, sizeof (GrlAmpacheSourcePriv));
}

static void
grl_ampache_source_init (GrlAmpacheSource *source)
{
  source->priv = GRL_AMPACHE_SOURCE_GET_PRIVATE (source);

  /* If we try to get too much elements in a single step, Ampache might return
     nothing. So limit the maximum amount of elements in each query */
  grl_source_set_auto_split_threshold (GRL_SOURCE (source), MAX_ELEMENTS);
}

/* ======================= Utilities ==================== */

static void
free_entry (Entry *entry)
{
  g_free (entry->id);
  g_free (entry->artist_name);
  g_free (entry->artist_id);
  g_free (entry->album_name);
  g_free (entry->album_id);
  g_free (entry->album_image);
  g_free (entry->track_name);
  g_free (entry->track_url);
  g_free (entry->track_duration);
  g_slice_free (Entry, entry);
}

static void generate_api_key_cb(GObject *source_object,
                                GAsyncResult *res,
                                gpointer user_data)
{
  GRL_TRACE();
  GError *wc_error = NULL;
  GError *error = NULL;
  gchar *content = NULL;
  gchar *api_key = NULL;
  xmlDocPtr doc;
  xmlNodePtr node;
  struct PluginInfo *plugininfo = (struct PluginInfo *) user_data;
  GrlRegistry *registry = grl_registry_get_default();

  if(!grl_net_wc_request_finish(GRL_NET_WC(source_object),
                                res,
                                &content,
                                NULL,
                                &wc_error)){
    GRL_DEBUG("request unsuccessful");
    error = g_error_new (GRL_CORE_ERROR,
                         GRL_CORE_ERROR_LOAD_PLUGIN_FAILED,
                         _("Failed to connect: %s"),
                         wc_error->message);

  }
  if(content){
    doc = xmlReadMemory(content, strlen(content), NULL, NULL,
                        XML_PARSE_RECOVER | XML_PARSE_NOBLANKS);
  }

  if (!doc) {
    error = g_error_new_literal (GRL_CORE_ERROR,
                                  GRL_CORE_ERROR_BROWSE_FAILED,
                                  _("Failed to parse response"));
    goto free_resources;
  }

  node = xmlDocGetRootElement(doc);
  if (!node) {
    error = g_error_new_literal (GRL_CORE_ERROR,
                                  GRL_CORE_ERROR_BROWSE_FAILED,
                                  _("Empty response"));
    goto free_resources;
  }

  if (xmlStrcmp (node->name, (const xmlChar *) "root")) {
    error = g_error_new_literal (GRL_CORE_ERROR,
                                  GRL_CORE_ERROR_BROWSE_FAILED,
                                  _("Empty response"));
    goto free_resources;
  }

  node = node->xmlChildrenNode;
  while(node)
  {
    if (!xmlStrcmp (node->name, (const xmlChar *) "auth")) {
      api_key =
        (gchar *) xmlNodeListGetString (doc, node->xmlChildrenNode, 1);
      break;
    }
  }

  if (!api_key) {
    GRL_INFO ("Missing API Key, cannot load plugin");
    return;
  }

  GrlAmpacheSource *source = grl_ampache_source_new (plugininfo->host_url,
                                                     api_key);
  grl_registry_register_source (registry,
                                plugininfo->plugin,
                                GRL_SOURCE (source),
                                NULL);
  g_free(api_key);
  g_free(content);


  free_resources:
    xmlFreeDoc (doc);
}

static Entry *
xml_parse_entry (xmlDocPtr doc, xmlNodePtr entry)
{
  xmlNodePtr node;
  Entry *data = g_slice_new0 (Entry);

  GRL_TRACE();

  if (strcmp ((gchar *) entry->name, AMPACHE_ARTIST) == 0) {
    data->category = AMPACHE_ARTIST_CAT;
  } else if (strcmp ((gchar *) entry->name, AMPACHE_ALBUM) == 0) {
    data->category = AMPACHE_ALBUM_CAT;
  } else if (strcmp ((gchar *) entry->name, AMPACHE_TRACK) == 0) {
    data->category = AMPACHE_TRACK_CAT;
  } else {
    g_return_val_if_reached (NULL);
  }

  data->id = (gchar *) xmlGetProp (entry, (const xmlChar *) "id");
  
  node = entry->xmlChildrenNode;

  while (node) {
    if (!xmlStrcmp (node->name, (const xmlChar *) "name")) {
      if(data->category == AMPACHE_ARTIST_CAT){
        data->artist_name =
          (gchar *) xmlNodeListGetString (doc, node->xmlChildrenNode, 1);
      } else {
        data->album_name =
          (gchar *) xmlNodeListGetString (doc, node->xmlChildrenNode, 1);
      }

    } else if (!xmlStrcmp (node->name, (const xmlChar *) "album")) {
      data->album_name =
        (gchar *) xmlNodeListGetString (doc, node->xmlChildrenNode, 1);

    } else if (!xmlStrcmp (node->name, (const xmlChar *) "artist")) {
      data->artist_name =
        (gchar *) xmlNodeListGetString (doc, node->xmlChildrenNode, 1);

    } else if (!xmlStrcmp (node->name, (const xmlChar *) "art")) {
      data->album_image =
        (gchar *) xmlNodeListGetString (doc, node->xmlChildrenNode, 1);

    } else if (!xmlStrcmp (node->name, (const xmlChar *) "title")) {
      data->track_name =
        (gchar *) xmlNodeListGetString (doc, node->xmlChildrenNode, 1);

    } else if (!xmlStrcmp (node->name, (const xmlChar *) "url")) {
      data->track_url =
        (gchar *) xmlNodeListGetString (doc, node->xmlChildrenNode, 1);

    } else if (!xmlStrcmp (node->name, (const xmlChar *) "time")) {
      data->track_duration =
        (gchar *) xmlNodeListGetString (doc, node->xmlChildrenNode, 1);
    }

    node = node->next;
  }

  return data;
} 

static void
update_media_from_entry (GrlMedia *media, const Entry *entry)
{
  gchar *id;
  if (entry->id) {
    id = g_strdup_printf ("%d/%s", entry->category, entry->id);
  } else {
    id = g_strdup_printf ("%d", entry->category);
  }

  /* Common fields */
  grl_media_set_id (media, id);
  g_free (id);

  if (entry->artist_name) {
    grl_data_set_string (GRL_DATA (media),
                         GRL_METADATA_KEY_ARTIST,
                         entry->artist_name);
  }

  if (entry->album_name) {
    grl_data_set_string (GRL_DATA (media),
                         GRL_METADATA_KEY_ALBUM,
                         entry->album_name);
  }


  /* Fields for artist */
  if (entry->category == AMPACHE_ARTIST_CAT) {

  if (entry->artist_name) {
      grl_media_set_title (media, entry->artist_name);
    }

  } else if (entry->category == AMPACHE_ALBUM_CAT) {

    if (entry->album_name) {
      grl_media_set_title (media, entry->artist_name);
    }


    if (entry->album_image) {
      grl_media_set_thumbnail (media, entry->album_image);
    }

    /* Fields for track */
  } else if (entry->category == AMPACHE_TRACK_CAT) {
    if (entry->track_name) {
      grl_media_set_title (media, entry->track_name);
    }


    if (entry->album_image) {
      grl_media_set_thumbnail (media, entry->album_image);
    }

    if (entry->track_url) {
      grl_media_set_url (media, entry->track_url);
    }

    if (entry->track_duration) {
      grl_media_set_duration (media, atoi (entry->track_duration));
    }
  }
}

static void
xml_parse_result (const gchar *str, GError **error, XmlParseEntries *xpe)
{
  xmlDocPtr doc;
  xmlNodePtr node;
  gint child_nodes = 0;
  GRL_TRACE();
  doc = xmlReadMemory (str, strlen (str), NULL, NULL,
                       XML_PARSE_RECOVER | XML_PARSE_NOBLANKS);
  if (!doc) {
    *error = g_error_new_literal (GRL_CORE_ERROR,
                                  GRL_CORE_ERROR_BROWSE_FAILED,
                                  _("Failed to parse response"));
    goto free_resources;
  }

  node = xmlDocGetRootElement (doc);
  if (!node) {
    *error = g_error_new_literal (GRL_CORE_ERROR,
                                  GRL_CORE_ERROR_BROWSE_FAILED,
                                  _("Empty response"));
    goto free_resources;
  }
  if (xmlStrcmp (node->name, (const xmlChar *) "root")) {
    *error = g_error_new_literal (GRL_CORE_ERROR,
                                  GRL_CORE_ERROR_BROWSE_FAILED,
                                  _("Empty response"));
    goto free_resources;
  }

  child_nodes = xmlChildElementCount (node);
  node = node->xmlChildrenNode;


  xpe->node = node;
  xpe->doc = doc;
  xpe->total_results = child_nodes;

  return;

 free_resources:
  xmlFreeDoc (doc);
}

static gboolean
xml_parse_entries_idle (gpointer user_data)
{
  XmlParseEntries *xpe = (XmlParseEntries *) user_data;
  gboolean parse_more;
  GrlMedia *media = NULL;
  Entry *entry;
  gint remaining = 0;

  GRL_TRACE ();

  parse_more = (xpe->cancelled == FALSE && xpe->node);

  if (parse_more) {
    entry = xml_parse_entry (xpe->doc, xpe->node);
    if (entry->category == AMPACHE_TRACK_CAT) {
      media = grl_media_audio_new ();
    } else {
      media = grl_media_box_new ();
    }

    update_media_from_entry (media, entry);
    free_entry (entry);

    xpe->index++;
    xpe->node = xpe->node->next;
    remaining = xpe->total_results - xpe->index;
  }

  if (parse_more || xpe->cancelled) {
    switch (xpe->type) {
    case BROWSE:
      xpe->spec.bs->callback (xpe->spec.bs->source,
                              xpe->spec.bs->operation_id,
                              media,
                              remaining,
                              xpe->spec.bs->user_data,
                              NULL);
      break;
    case QUERY:
      xpe->spec.qs->callback (xpe->spec.qs->source,
                              xpe->spec.qs->operation_id,
                              media,
                              remaining,
                              xpe->spec.qs->user_data,
                              NULL);
      break;
    case SEARCH:
      xpe->spec.ss->callback (xpe->spec.ss->source,
                              xpe->spec.ss->operation_id,
                              media,
                              remaining,
                              xpe->spec.ss->user_data,
                              NULL);
      break;
    }
  }

  if (!parse_more) {
    xmlFreeDoc (xpe->doc);
    g_slice_free (XmlParseEntries, xpe);
  }

  return parse_more;
}

static void
generate_api_key(GrlConfig *config, GrlRegistry *registry, GrlPlugin *plugin)
{
  GRL_TRACE();
  struct PluginInfo *plugininfo = g_slice_new0 (struct PluginInfo);
  gchar *current_time =  g_strdup_printf("%" G_GUINT64_FORMAT, g_get_real_time()/1000000);
  GRL_DEBUG ("current_time %s", current_time);
  gchar *pass_hash =  g_compute_checksum_for_string(G_CHECKSUM_SHA256, grl_config_get_password(config), -1);
  GRL_DEBUG ("pass_hash %s", pass_hash);
  gchar *passphrase =  g_compute_checksum_for_string(G_CHECKSUM_SHA256, g_strconcat(current_time, pass_hash, NULL), -1);
  GRL_DEBUG ("passphrase %s", passphrase);
  gchar *username = grl_config_get_username(config);
  GRL_DEBUG ("username %s", username);
  gchar *host_url = grl_config_get_string(config, "host_url");
  GRL_DEBUG ("host_url %s", host_url);
  gchar *url = g_strdup_printf(AMPACHE_GENERATE_AUTH, host_url, passphrase, current_time, AMPACHE_VERSION, username);
  GRL_DEBUG ("url %s", url);

  GrlNetWc * wc = grl_net_wc_new();
  plugininfo->host_url = grl_config_get_string(config, "host_url");
  plugininfo->plugin = plugin;
  GRL_INFO ("Starting request");
  grl_net_wc_request_async(wc, url, NULL, generate_api_key_cb, plugininfo);
  g_free(current_time);
  g_free(pass_hash);
  g_free(passphrase);
  g_free(username);
  g_free(host_url);
}

static void
update_media_from_artists (GrlMedia *media)
{
  Entry entry = {
    .category = AMPACHE_ARTIST_CAT,
    .artist_name = _("Artists"),
  };

  update_media_from_entry (media, &entry);
}

static void
update_media_from_albums (GrlMedia *media)
{
  Entry entry = {
    .category = AMPACHE_ALBUM_CAT,
    .album_name = _("Albums"),
  };

  update_media_from_entry (media, &entry);
}

static void
send_toplevel_categories (GrlSourceBrowseSpec *bs)
{
  GrlMedia *media;
  gint remaining;
  guint skip = grl_operation_options_get_skip (bs->options);
  gint count = grl_operation_options_get_count (bs->options);

  /* Check if all elements must be skipped */
  if (skip > 2 || count == 0) {
    bs->callback (bs->source, bs->operation_id, NULL, 0, bs->user_data, NULL);
    return;
  }

  count = MIN (count, 3);
  remaining = MIN (count, 3 - skip);

  while (remaining > 0) {
    media = grl_media_box_new ();
    switch (skip) {
    case 0:
      update_media_from_artists (media);
      break;
    case 1:
      update_media_from_albums (media);
      break;
    }
    remaining--;
    skip++;
    bs->callback (bs->source, bs->operation_id, media, remaining, bs->user_data, NULL);
  }
}

static void
read_done_cb (GObject *source_object,
              GAsyncResult *res,
              gpointer user_data)
{
  GRL_TRACE();
  XmlParseEntries *xpe = (XmlParseEntries *) user_data;
  gint error_code = -1;
  GError *wc_error = NULL;
  GError *error = NULL;
  gchar *content = NULL;
  Entry *entry = NULL;

  /* Check if operation was cancelled */
  if (xpe->cancelled) {
    goto invoke_cb;
  }

  if (!grl_net_wc_request_finish (GRL_NET_WC (source_object),
                              res,
                              &content,
                              NULL,
                              &wc_error)) {
    GRL_DEBUG("unable to connect");
    switch (xpe->type) {
    case RESOLVE:
      error_code = GRL_CORE_ERROR_RESOLVE_FAILED;
      break;
    case BROWSE:
      error_code = GRL_CORE_ERROR_BROWSE_FAILED;
      break;
    case QUERY:
      error_code = GRL_CORE_ERROR_QUERY_FAILED;
      break;
    case SEARCH:
      error_code = GRL_CORE_ERROR_SEARCH_FAILED;
      break;
    }

    error = g_error_new (GRL_CORE_ERROR,
                         error_code,
                         _("Failed to connect: %s"),
                         wc_error->message);
    g_error_free (wc_error);
    goto invoke_cb;
  }

  if (content) {
    xml_parse_result (content, &error, xpe);
  } else {
    goto invoke_cb;
  }

  if (error) {
    goto invoke_cb;
  }

  if (xpe->node) {
    if (xpe->type == RESOLVE) {
      entry = xml_parse_entry (xpe->doc, xpe->node);
      xmlFreeDoc (xpe->doc);
      update_media_from_entry (xpe->spec.rs->media, entry);
      free_entry (entry);
      goto invoke_cb;
    } else {
      guint id = g_idle_add (xml_parse_entries_idle, xpe);
      g_source_set_name_by_id (id, "[ampache] xml_parse_entries_idle");
    }
  } else {
    if (xpe->type == RESOLVE) {
      error = g_error_new_literal (GRL_CORE_ERROR,
                                   GRL_CORE_ERROR_RESOLVE_FAILED,
                                   _("Failed to parse response"));
    }
    goto invoke_cb;
  }

  return;

 invoke_cb:
  switch (xpe->type) {
  case RESOLVE:
    xpe->spec.rs->callback (xpe->spec.rs->source,
                            xpe->spec.rs->operation_id,
                            xpe->spec.rs->media,
                            xpe->spec.rs->user_data,
                            error);
    break;
  case BROWSE:
    xpe->spec.bs->callback (xpe->spec.bs->source,
                            xpe->spec.bs->operation_id,
                            NULL,
                            0,
                            xpe->spec.bs->user_data,
                            error);
    break;
  case QUERY:
    xpe->spec.qs->callback (xpe->spec.qs->source,
                            xpe->spec.qs->operation_id,
                            NULL,
                            0,
                            xpe->spec.qs->user_data,
                            error);
    break;
  case SEARCH:
    xpe->spec.ss->callback (xpe->spec.ss->source,
                            xpe->spec.ss->operation_id,
                            NULL,
                            0,
                            xpe->spec.ss->user_data,
                            error);
    break;
  }

  g_slice_free (XmlParseEntries, xpe);
  g_clear_error (&error);
}

static void
read_url_async(GrlAmpacheSource *source,
               const gchar *url,
               gpointer user_data)
{
  if (!source->priv->wc)
    source->priv->wc = g_object_new (GRL_TYPE_NET_WC, "throttling", 1, NULL);

  source->priv->cancellable = g_cancellable_new ();

  GRL_DEBUG ("Opening '%s'", url);
  grl_net_wc_request_async (source->priv->wc,
                        url,
                        source->priv->cancellable,
                        read_done_cb,
                        user_data);
}

/* ================== API Implementation ================ */

static const GList *
grl_ampache_source_supported_keys (GrlSource *source)
{
  static GList *keys = NULL;
  if (!keys) {
    keys = grl_metadata_key_list_new (GRL_METADATA_KEY_ID,
                                      GRL_METADATA_KEY_TITLE,
                                      GRL_METADATA_KEY_ARTIST,
                                      GRL_METADATA_KEY_ALBUM,
                                      GRL_METADATA_KEY_URL,
                                      GRL_METADATA_KEY_DURATION,
                                      GRL_METADATA_KEY_THUMBNAIL,
                                      GRL_METADATA_KEY_INVALID);
  }
  return keys;
}

static void grl_ampache_source_search (GrlSource *source,
                                       GrlSourceSearchSpec *ss)
{
  GRL_TRACE();
  GrlAmpacheSource *ampache_source = GRL_AMPACHE_SOURCE(source);
  XmlParseEntries *xpe;
  gchar *url;
  guint count = grl_operation_options_get_count (ss->options);
  guint skip = grl_operation_options_get_skip (ss->options);

  if(ss->text){
    url = g_strdup_printf(AMPACHE_SEARCH_ALL, ampache_source->priv->host_url,
                          ampache_source->priv->api_key,
                          ss->text);
  } else{
    url = g_strdup_printf(AMPACHE_TRACKS, ampache_source->priv->host_url,
                          ampache_source->priv->api_key, count, skip);
  }


  xpe = g_slice_new0 (XmlParseEntries);
  xpe->type = SEARCH;
  xpe->spec.ss = ss;
  read_url_async(ampache_source, url, xpe);
  g_free(url);
}
static void
grl_ampache_source_browse (GrlSource *source,
                           GrlSourceBrowseSpec *bs)
{
  gchar *url = NULL;
  gchar **container_split = NULL;
  AmpacheCategory category;
  XmlParseEntries *xpe = NULL;
  const gchar *container_id;
  GError *error = NULL;
  GrlAmpacheSource *ampache_source = GRL_AMPACHE_SOURCE(source);
  gint count = grl_operation_options_get_count (bs->options);
  guint skip = grl_operation_options_get_skip (bs->options);

  GRL_TRACE ();

  container_id = grl_media_get_id (bs->container);

  if (!container_id) {
    /* Root category: return top-level predefined categories */
    send_toplevel_categories (bs);
    return;
  }

  container_split = g_strsplit (container_id, "/", 0);

  if (g_strv_length (container_split) == 0) {
    error = g_error_new (GRL_CORE_ERROR,
                         GRL_CORE_ERROR_BROWSE_FAILED,
                         _("Invalid container identifier %s"),
                         container_id);
  } else {
    category = atoi (container_split[0]);

    if (category == AMPACHE_ARTIST_CAT) {
      if (container_split[1]) {
        url =
          g_strdup_printf (AMPACHE_ARTIST_ALBUMS,
                           ampache_source->priv->host_url,
                           ampache_source->priv->api_key,
                           skip,
                           count,
                           container_split[1]);
      } else {
        /* Browsing through artists */
        url = g_strdup_printf (AMPACHE_ARTISTS,
                               ampache_source->priv->host_url,
                               ampache_source->priv->api_key,
                               skip,
                               count);
      }

    } else if (category == AMPACHE_ALBUM_CAT) {
      if (container_split[1]) {
        /* Requesting information from a specific album */
        url = g_strdup_printf (AMPACHE_ALBUM_TRACKS,
                               ampache_source->priv->host_url,
                               ampache_source->priv->api_key,
                               skip,
                               count,
                               container_split[1]);
      } else {
        /* Browsing through albums */
        url = g_strdup_printf (AMPACHE_ALBUMS,
                               ampache_source->priv->host_url,
                               ampache_source->priv->api_key,
                               skip,
                               count);
      }

    } else if (category == AMPACHE_TRACK_CAT) {
      error = g_error_new (GRL_CORE_ERROR,
                           GRL_CORE_ERROR_BROWSE_FAILED,
                           _("Failed to browse: %s is a track"),
                           container_id);
    } else {
      error = g_error_new (GRL_CORE_ERROR,
                           GRL_CORE_ERROR_BROWSE_FAILED,
                           _("Invalid container identifier %s"),
                           container_id);
    }
  }

  if (error) {
    bs->callback (source, bs->operation_id, NULL, 0, bs->user_data, error);
    g_error_free (error);
    return;
  }

  xpe = g_slice_new0 (XmlParseEntries);
  xpe->type = BROWSE;
  xpe->spec.bs = bs;

  grl_operation_set_data (bs->operation_id, xpe);

  read_url_async (GRL_AMPACHE_SOURCE (source), url, xpe);
  g_free (url);
  g_clear_pointer (&container_split, g_strfreev);
}

static void
grl_ampache_source_cancel (GrlSource *source, guint operation_id)
{
  GRL_TRACE();
  XmlParseEntries *xpe;
  GrlAmpacheSourcePriv *priv;

  g_return_if_fail (GRL_IS_AMPACHE_SOURCE (source));

  priv = GRL_AMPACHE_SOURCE_GET_PRIVATE (source);

  if (priv->cancellable && G_IS_CANCELLABLE (priv->cancellable))
    g_cancellable_cancel (priv->cancellable);
  priv->cancellable = NULL;

  if (priv->wc)
    grl_net_wc_flush_delayed_requests (priv->wc);

  GRL_DEBUG ("grl_ampache_source_cancel");

  xpe = (XmlParseEntries *) grl_operation_get_data (operation_id);

  if (xpe) {
    GRL_DEBUG("cancelling");
    xpe->cancelled = TRUE;
  }
}
/* =================== Ampache Plugin  =============== */
gboolean
grl_ampache_plugin_init (GrlRegistry *registry,
                         GrlPlugin *plugin,
                         GList *configs)
{


  gchar *host_url;
  GrlConfig *config;
  gint config_count;

  GRL_LOG_DOMAIN_INIT (ampache_log_domain, "ampache");

  GRL_TRACE ();

  /* Initialize i18n */
  bindtextdomain (GETTEXT_PACKAGE, LOCALEDIR);
  bind_textdomain_codeset (GETTEXT_PACKAGE, "UTF-8");

  if (!configs) {
    GRL_INFO ("Configuration not provided! Plugin not loaded");
    return FALSE;
  }

  config_count = g_list_length (configs);
  if (config_count > 1) {
    GRL_INFO ("Provided %d configs, but will only use one", config_count);
  }

  config = GRL_CONFIG (configs->data);

  if (!config) {
    GRL_INFO ("Error parsing config, plugin not loaded");
    return FALSE;
  }

  if (!grl_config_get_username(config)) {
    GRL_INFO ("Username not specifed, plugin not loaded");
    return FALSE;
  }

  if (!grl_config_get_password(config)) {
    GRL_INFO ("Password not specifed, plugin not loaded");
    return FALSE;
  }

  if (!grl_config_get_string(config, "host_url")) {
    GRL_INFO ("Host URL not specifed, plugin not loaded");
    return FALSE;
  }

  generate_api_key(config, registry ,plugin);
  return TRUE;
}

GRL_PLUGIN_REGISTER (grl_ampache_plugin_init,
                     NULL,
                     PLUGIN_ID);