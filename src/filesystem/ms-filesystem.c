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
#include <string.h>
#include <stdlib.h>

#include "ms-filesystem.h"

/* --------- Logging  -------- */ 

#undef G_LOG_DOMAIN
#define G_LOG_DOMAIN "ms-filesystem"

/* -------- File info ------- */

#define FILE_ATTRIBUTES "standard::*"

/* ---- Emission chunks ----- */

#define BROWSE_IDLE_CHUNK_SIZE 5

/* --- Plugin information --- */

#define PLUGIN_ID   "ms-filesystem"
#define PLUGIN_NAME "Filesystem"
#define PLUGIN_DESC "A plugin for browsing the filesystem"

#define SOURCE_ID   "ms-filesystem"
#define SOURCE_NAME "Filesystem"
#define SOURCE_DESC "A source for browsing the filesystem"

#define AUTHOR      "Igalia S.L."
#define LICENSE     "LGPL"
#define SITE        "http://www.igalia.com"

/* --- Data types --- */

typedef struct {
  MsMediaSourceBrowseSpec *spec;
  GList *entries;
  GList *current;
  const gchar *path;
  guint remaining;
}  BrowseIdleData;

static MsFilesystemSource *ms_filesystem_source_new (void);

gboolean ms_filesystem_plugin_init (MsPluginRegistry *registry,
				    const MsPluginInfo *plugin);

static const GList *ms_filesystem_source_supported_keys (MsMetadataSource *source);

static void ms_filesystem_source_metadata (MsMediaSource *source,
					   MsMediaSourceMetadataSpec *ms);

static void ms_filesystem_source_browse (MsMediaSource *source,
					 MsMediaSourceBrowseSpec *bs);


/* =================== Filesystem Plugin  =============== */

gboolean
ms_filesystem_plugin_init (MsPluginRegistry *registry, const MsPluginInfo *plugin)
{
  g_debug ("filesystem_plugin_init\n");

  MsFilesystemSource *source = ms_filesystem_source_new ();
  ms_plugin_registry_register_source (registry, plugin, MS_MEDIA_PLUGIN (source));
  return TRUE;
}

MS_PLUGIN_REGISTER (ms_filesystem_plugin_init, 
                    NULL, 
                    PLUGIN_ID,
                    PLUGIN_NAME, 
                    PLUGIN_DESC, 
                    PACKAGE_VERSION,
                    AUTHOR, 
                    LICENSE, 
                    SITE);

/* ================== Filesystem GObject ================ */

static MsFilesystemSource *
ms_filesystem_source_new (void)
{
  g_debug ("ms_filesystem_source_new");
  return g_object_new (MS_FILESYSTEM_SOURCE_TYPE,
		       "source-id", SOURCE_ID,
		       "source-name", SOURCE_NAME,
		       "source-desc", SOURCE_DESC,
		       NULL);
}

static void
ms_filesystem_source_class_init (MsFilesystemSourceClass * klass)
{
  MsMediaSourceClass *source_class = MS_MEDIA_SOURCE_CLASS (klass);
  MsMetadataSourceClass *metadata_class = MS_METADATA_SOURCE_CLASS (klass);
  source_class->browse = ms_filesystem_source_browse;
  source_class->metadata = ms_filesystem_source_metadata;
  metadata_class->supported_keys = ms_filesystem_source_supported_keys;
}

static void
ms_filesystem_source_init (MsFilesystemSource *source)
{
}

G_DEFINE_TYPE (MsFilesystemSource, ms_filesystem_source, MS_TYPE_MEDIA_SOURCE);

/* ======================= Utilities ==================== */

static void
set_container_childcount (const gchar *path, MsContentMedia *media)
{
  GDir *dir;
  GError *error = NULL;
  gint count;

  /* Open directory */
  g_debug ("Opening directory '%s' for childcount", path);
  dir = g_dir_open (path, 0, &error);
  if (error) {
    g_warning ("Failed to open directory '%s': %s", path, error->message);
    g_error_free (error);
    return;
  }

  /* Count entries */
  count = 0;
  while (g_dir_read_name (dir) != NULL)
    count++;

  ms_content_set_int (MS_CONTENT (media), MS_METADATA_KEY_CHILDCOUNT, count);
}

static MsContent *
create_content (const gchar *path)
{
  MsContentMedia *media;
  gchar *str;
  GError *error = NULL;

  GFile *file = g_file_new_for_path (path);
  GFileInfo *info = g_file_query_info (file,
				       FILE_ATTRIBUTES,
				       0,
				       NULL,
				       &error);

  if (error) {
    g_warning ("Failed to get info for file '%s': %s", path, error->message);
    media = ms_content_media_new ();

    /* Title */
    str = g_strrstr (path, G_DIR_SEPARATOR_S);
    if (!str) {
      str = (gchar *) path;
    }
    ms_content_media_set_title (media, str);
    g_error_free (error);
  } else {
    if (g_file_info_get_file_type (info) == G_FILE_TYPE_DIRECTORY) {
      media = ms_content_media_container_new ();
    } else {
      media = ms_content_media_new ();
    }

    /* Title */
    str = (gchar *) g_file_info_get_display_name (info);
    ms_content_media_set_title (media, str);

    /* Mime */
    str = (gchar *) g_file_info_get_content_type (info);
    ms_content_set_string (MS_CONTENT (media), MS_METADATA_KEY_MIME, str);

    /* Date */
    GTimeVal time;
    gchar *time_str;
    g_file_info_get_modification_time (info, &time);
    time_str = g_time_val_to_iso8601 (&time);
    ms_content_set_string (MS_CONTENT (media), MS_METADATA_KEY_DATE, time_str);
    g_free (time_str);

    g_object_unref (info);
  }

  /* ID */
  ms_content_media_set_id (media, path);

  /* URL */
  str = g_strconcat ("file://", path, NULL);
  ms_content_media_set_url (media, str);
  g_free (str);

  /* Childcount */
  if (ms_content_is_container (MS_CONTENT (media))) {
    set_container_childcount (path, media);
  }

  g_object_unref (file);
  
  return MS_CONTENT (media);
}

static gboolean
browse_emit_idle (gpointer user_data)
{
  BrowseIdleData *idle_data;
  guint count;
  const gchar *entry;

  g_debug ("browse_emit_idle");

  idle_data = (BrowseIdleData *) user_data;

  count = 0;
  do {    
    gchar *entry_path;
    MsContent *content;
    
    entry = (const gchar *) idle_data->current->data;
    if (strcmp (idle_data->path, G_DIR_SEPARATOR_S)) {
      entry_path = g_strconcat (idle_data->path, G_DIR_SEPARATOR_S,
				entry, NULL);
    } else {
      entry_path = g_strconcat (idle_data->path, entry, NULL);
    }
    content = create_content (entry_path); 
    g_free (entry_path);
    g_free (idle_data->current->data);
   
    idle_data->spec->callback (idle_data->spec->source,
			       idle_data->spec->browse_id,
			       content,
			       idle_data->remaining--,
			       idle_data->spec->user_data,
			       NULL);        
    
    idle_data->current = g_list_next (idle_data->current);
    count++;
  } while (count < BROWSE_IDLE_CHUNK_SIZE && idle_data->current);

  if (!idle_data->current) {
    g_list_free (idle_data->entries);
    g_free (idle_data);
    return FALSE;
  } else {
    return TRUE;
  }
}

static void
produce_from_path (MsMediaSourceBrowseSpec *bs, const gchar *path)
{
  GDir *dir;
  GError *error = NULL;
  const gchar *entry;
  guint skip, count;
  GList *entries = NULL;

  /* Open directory */
  g_debug ("Opening directory '%s'", path);
  dir = g_dir_open (path, 0, &error);
  if (error) {
    g_warning ("Failed to open directory '%s': %s", path, error->message);
    bs->callback (bs->source, bs->browse_id, NULL, 0, bs->user_data, error);
    g_error_free (error);
  }

  /* Get entries to emit */
  skip = bs->skip;
  count = bs->count;
  while (count && (entry = g_dir_read_name (dir)) != NULL) {
    if (skip > 0)  {
      skip--;
      continue;
    }
    if (count > 0) {
      entries = g_list_prepend (entries, g_strdup (entry));
      count--;
    }
  }

  /* Emit results */
  if (entries) {
    /* Use the idle loop to avoid blocking for too long */
    BrowseIdleData *idle_data = g_new (BrowseIdleData, 1);
    idle_data->spec = bs;
    idle_data->remaining = bs->count - count - 1;
    idle_data->path = path;
    idle_data->entries = entries;
    idle_data->current = entries;
    g_idle_add (browse_emit_idle, idle_data);
  } else {
    /* No results */
    bs->callback (bs->source,
		  bs->browse_id,
		  NULL,
		  0,
		  bs->user_data,
		  NULL);        
  }

  g_dir_close (dir);
}

/* ================== API Implementation ================ */

static const GList *
ms_filesystem_source_supported_keys (MsMetadataSource *source)
{
  static GList *keys = NULL;
  if (!keys) {
    keys = ms_metadata_key_list_new (MS_METADATA_KEY_ID,
				     MS_METADATA_KEY_TITLE, 
				     MS_METADATA_KEY_URL,
				     MS_METADATA_KEY_MIME,
				     MS_METADATA_KEY_DATE,
				     MS_METADATA_KEY_CHILDCOUNT,
				     NULL);
  }
  return keys;
}

static void
ms_filesystem_source_browse (MsMediaSource *source, MsMediaSourceBrowseSpec *bs)
{
  const gchar *path;

  g_debug ("ms_filesystem_source_browse (%s)", bs->container_id);

  path = bs->container_id ? bs->container_id : G_DIR_SEPARATOR_S;
  produce_from_path (bs, path);
}

static void
ms_filesystem_source_metadata (MsMediaSource *source,
			       MsMediaSourceMetadataSpec *ms)
{
  MsContent *content;
  gchar *path;

  g_debug ("ms_filesystem_source_metadata");

  path = ms->object_id ? ms->object_id : G_DIR_SEPARATOR_S;

  if (g_file_test (path, G_FILE_TEST_EXISTS)) {
    content = create_content (path);
    ms->callback (ms->source, content, ms->user_data, NULL);
  } else {
    GError *error = g_error_new (MS_ERROR,
				 MS_ERROR_METADATA_FAILED,
				 "File '%s' does not exist",
				 path);
    ms->callback (ms->source, NULL, ms->user_data, error);
    g_error_free (error);
  }
}
