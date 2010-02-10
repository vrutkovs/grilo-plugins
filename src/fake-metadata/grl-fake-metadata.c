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

#include <grilo.h>

#include "grl-fake-metadata.h"

#undef G_LOG_DOMAIN
#define G_LOG_DOMAIN "grl-fake-metadata"

#define PLUGIN_ID   "grl-fake-metadata"
#define PLUGIN_NAME "Fake Metadata Provider"
#define PLUGIN_DESC "A plugin for faking metadata resolution"

#define SOURCE_ID   "grl-fake-metadata"
#define SOURCE_NAME "Fake Metadata Provider"
#define SOURCE_DESC "A source for faking metadata resolution"

#define AUTHOR      "Igalia S.L."
#define LICENSE     "LGPL"
#define SITE        "http://www.igalia.com"


static GrlFakeMetadataSource *grl_fake_metadata_source_new (void);

static void grl_fake_metadata_source_resolve (GrlMetadataSource *source,
                                              GrlMetadataSourceResolveSpec *rs);

static const GList *grl_fake_metadata_source_supported_keys (GrlMetadataSource *source);

static const GList *grl_fake_metadata_source_key_depends (GrlMetadataSource *source,
                                                          GrlKeyID key_id);

gboolean grl_fake_metadata_source_plugin_init (GrlPluginRegistry *registry,
                                               const GrlPluginInfo *plugin);


/* =================== GrlFakeMetadata Plugin  =============== */

gboolean
grl_fake_metadata_source_plugin_init (GrlPluginRegistry *registry,
                                      const GrlPluginInfo *plugin)
{
  g_debug ("grl_fake_metadata_source_plugin_init");
  GrlFakeMetadataSource *source = grl_fake_metadata_source_new ();
  grl_plugin_registry_register_source (registry,
                                       plugin,
                                       GRL_MEDIA_PLUGIN (source));
  return TRUE;
}

GRL_PLUGIN_REGISTER (grl_fake_metadata_source_plugin_init,
                     NULL,
                     PLUGIN_ID,
                     PLUGIN_NAME,
                     PLUGIN_DESC,
                     PACKAGE_VERSION,
                     AUTHOR,
                     LICENSE,
                     SITE);

/* ================== GrlFakeMetadata GObject ================ */

static GrlFakeMetadataSource *
grl_fake_metadata_source_new (void)
{
  g_debug ("grl_fake_metadata_source_new");
  return g_object_new (GRL_FAKE_METADATA_SOURCE_TYPE,
		       "source-id", SOURCE_ID,
		       "source-name", SOURCE_NAME,
		       "source-desc", SOURCE_DESC,
		       NULL);
}

static void
grl_fake_metadata_source_class_init (GrlFakeMetadataSourceClass * klass)
{
  GrlMetadataSourceClass *metadata_class = GRL_METADATA_SOURCE_CLASS (klass);
  metadata_class->supported_keys = grl_fake_metadata_source_supported_keys;
  metadata_class->key_depends = grl_fake_metadata_source_key_depends;
  metadata_class->resolve = grl_fake_metadata_source_resolve;
}

static void
grl_fake_metadata_source_init (GrlFakeMetadataSource *source)
{
}

G_DEFINE_TYPE (GrlFakeMetadataSource,
               grl_fake_metadata_source,
               GRL_TYPE_METADATA_SOURCE);

/* ======================= Utilities ==================== */

static void
fill_metadata (GrlContentMedia *media, GrlKeyID key_id)
{
  switch (key_id) {
  case GRL_METADATA_KEY_AUTHOR:
    grl_content_media_set_author (media, "fake author");
    break;
  case GRL_METADATA_KEY_ARTIST:
    grl_content_set_string (GRL_CONTENT (media),
                            GRL_METADATA_KEY_ARTIST, "fake artist");
    break;
  case GRL_METADATA_KEY_ALBUM:
    grl_content_set_string (GRL_CONTENT (media),
                            GRL_METADATA_KEY_ALBUM, "fake album");
    break;
  case GRL_METADATA_KEY_GENRE:
    grl_content_set_string (GRL_CONTENT (media),
                            GRL_METADATA_KEY_GENRE, "fake genre");
    break;
  case GRL_METADATA_KEY_DESCRIPTION:
    grl_content_media_set_description (media, "fake description");
    break;
  case GRL_METADATA_KEY_DURATION:
    grl_content_media_set_duration (media, 99);
    break;
  case GRL_METADATA_KEY_DATE:
    grl_content_set_string (GRL_CONTENT (media),
                            GRL_METADATA_KEY_DATE, "01/01/1970");
    break;
  case GRL_METADATA_KEY_THUMBNAIL:
    grl_content_media_set_thumbnail (media,
                                     "http://fake.thumbnail.com/fake-image.jpg");
    break;
  default:
    break;
  }
}


/* ================== API Implementation ================ */

static const GList *
grl_fake_metadata_source_supported_keys (GrlMetadataSource *source)
{
  static GList *keys = NULL;
  if (!keys) {
    keys = grl_metadata_key_list_new (GRL_METADATA_KEY_AUTHOR,
                                      GRL_METADATA_KEY_ARTIST,
                                      GRL_METADATA_KEY_ALBUM,
                                      GRL_METADATA_KEY_GENRE,
                                      GRL_METADATA_KEY_DESCRIPTION,
                                      GRL_METADATA_KEY_DURATION,
                                      GRL_METADATA_KEY_DATE,
                                      GRL_METADATA_KEY_THUMBNAIL,
                                      NULL);
  }
  return keys;
}

static const GList *
grl_fake_metadata_source_key_depends (GrlMetadataSource *source,
                                      GrlKeyID key_id)
{
  static GList *deps = NULL;
  if (!deps) {
    deps = grl_metadata_key_list_new (GRL_METADATA_KEY_TITLE, NULL);
  }

  switch (key_id) {
  case GRL_METADATA_KEY_AUTHOR:
  case GRL_METADATA_KEY_ARTIST:
  case GRL_METADATA_KEY_ALBUM:
  case GRL_METADATA_KEY_GENRE:
  case GRL_METADATA_KEY_DESCRIPTION:
  case GRL_METADATA_KEY_DURATION:
  case GRL_METADATA_KEY_DATE:
  case GRL_METADATA_KEY_THUMBNAIL:
    return deps;
  default:
    break;
  }

  return  NULL;
}

static void
grl_fake_metadata_source_resolve (GrlMetadataSource *source,
                                  GrlMetadataSourceResolveSpec *rs)
{
  g_debug ("grl_fake_metadata_source_resolve");

  GList *iter;

  iter = rs->keys;
  while (iter) {
    GrlKeyID key_id = POINTER_TO_GRLKEYID (iter->data);
    fill_metadata (GRL_CONTENT_MEDIA (rs->media), key_id);
    iter = g_list_next (iter);
  }

  rs->callback (source, rs->media, rs->user_data, NULL);
}