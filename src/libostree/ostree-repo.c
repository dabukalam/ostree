/* -*- mode: C; c-file-style: "gnu"; indent-tabs-mode: nil; -*-
 *
 * Copyright (C) 2011 Colin Walters <walters@verbum.org>
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2 of the License, or (at your option) any later version.
 *
 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with this library; if not, write to the
 * Free Software Foundation, Inc., 59 Temple Place - Suite 330,
 * Boston, MA 02111-1307, USA.
 *
 * Author: Colin Walters <walters@verbum.org>
 */

#define _GNU_SOURCE

#include "config.h"

#include "ostree.h"
#include "otutil.h"
#include "ostree-repo-file-enumerator.h"

#include <gio/gunixoutputstream.h>
#include <gio/gunixinputstream.h>
#include <glib/gstdio.h>

#include <stdio.h>
#include <stdlib.h>

#ifdef HAVE_LIBARCHIVE
#include <archive.h>
#include <archive_entry.h>
#include "ostree-libarchive-input-stream.h"
#endif

struct OstreeRepo {
  GObject parent;

  GFile *repodir;
  GFile *tmp_dir;
  GFile *pending_dir;
  GFile *local_heads_dir;
  GFile *remote_heads_dir;
  GFile *objects_dir;
  GFile *uncompressed_objects_dir;
  GFile *remote_cache_dir;
  GFile *config_file;

  GMutex cache_lock;
  GPtrArray *cached_meta_indexes;
  GPtrArray *cached_content_indexes;

  gboolean inited;
  gboolean in_transaction;
  GHashTable *loose_object_devino_hash;
  GHashTable *updated_uncompressed_dirs;

  GKeyFile *config;
  OstreeRepoMode mode;
  gboolean enable_uncompressed_cache;

  OstreeRepo *parent_repo;
};

typedef struct {
  GObjectClass parent_class;
} OstreeRepoClass;

static gboolean      
repo_find_object (OstreeRepo           *self,
                  OstreeObjectType      objtype,
                  const char           *checksum,
                  GFile               **out_stored_path,
                  GCancellable         *cancellable,
                  GError             **error);

enum {
  PROP_0,

  PROP_PATH
};

G_DEFINE_TYPE (OstreeRepo, ostree_repo, G_TYPE_OBJECT)

static void
ostree_repo_finalize (GObject *object)
{
  OstreeRepo *self = OSTREE_REPO (object);

  g_clear_object (&self->parent_repo);

  g_clear_object (&self->repodir);
  g_clear_object (&self->tmp_dir);
  g_clear_object (&self->pending_dir);
  g_clear_object (&self->local_heads_dir);
  g_clear_object (&self->remote_heads_dir);
  g_clear_object (&self->objects_dir);
  g_clear_object (&self->uncompressed_objects_dir);
  g_clear_object (&self->remote_cache_dir);
  g_clear_object (&self->config_file);
  if (self->loose_object_devino_hash)
    g_hash_table_destroy (self->loose_object_devino_hash);
  if (self->updated_uncompressed_dirs)
    g_hash_table_destroy (self->updated_uncompressed_dirs);
  if (self->config)
    g_key_file_free (self->config);
  g_clear_pointer (&self->cached_meta_indexes, (GDestroyNotify) g_ptr_array_unref);
  g_clear_pointer (&self->cached_content_indexes, (GDestroyNotify) g_ptr_array_unref);
  g_mutex_clear (&self->cache_lock);

  G_OBJECT_CLASS (ostree_repo_parent_class)->finalize (object);
}

static void
ostree_repo_set_property(GObject         *object,
			   guint            prop_id,
			   const GValue    *value,
			   GParamSpec      *pspec)
{
  OstreeRepo *self = OSTREE_REPO (object);

  switch (prop_id)
    {
    case PROP_PATH:
      /* Canonicalize */
      self->repodir = g_file_new_for_path (gs_file_get_path_cached (g_value_get_object (value)));
      break;
    default:
      G_OBJECT_WARN_INVALID_PROPERTY_ID (object, prop_id, pspec);
      break;
    }
}

static void
ostree_repo_get_property(GObject         *object,
			   guint            prop_id,
			   GValue          *value,
			   GParamSpec      *pspec)
{
  OstreeRepo *self = OSTREE_REPO (object);

  switch (prop_id)
    {
    case PROP_PATH:
      g_value_set_object (value, self->repodir);
      break;
    default:
      G_OBJECT_WARN_INVALID_PROPERTY_ID (object, prop_id, pspec);
      break;
    }
}

static GObject *
ostree_repo_constructor (GType                  gtype,
                           guint                  n_properties,
                           GObjectConstructParam *properties)
{
  OstreeRepo *self;
  GObject *object;
  GObjectClass *parent_class;

  parent_class = G_OBJECT_CLASS (ostree_repo_parent_class);
  object = parent_class->constructor (gtype, n_properties, properties);
  self = (OstreeRepo*)object;

  g_assert (self->repodir != NULL);
  
  self->tmp_dir = g_file_resolve_relative_path (self->repodir, "tmp");
  self->pending_dir = g_file_resolve_relative_path (self->repodir, "tmp/pending");
  self->local_heads_dir = g_file_resolve_relative_path (self->repodir, "refs/heads");
  self->remote_heads_dir = g_file_resolve_relative_path (self->repodir, "refs/remotes");
  
  self->objects_dir = g_file_get_child (self->repodir, "objects");
  self->uncompressed_objects_dir = g_file_get_child (self->repodir, "uncompressed-objects-cache");
  self->remote_cache_dir = g_file_get_child (self->repodir, "remote-cache");
  self->config_file = g_file_get_child (self->repodir, "config");

  return object;
}

static void
ostree_repo_class_init (OstreeRepoClass *klass)
{
  GObjectClass *object_class = G_OBJECT_CLASS (klass);

  object_class->constructor = ostree_repo_constructor;
  object_class->get_property = ostree_repo_get_property;
  object_class->set_property = ostree_repo_set_property;
  object_class->finalize = ostree_repo_finalize;

  g_object_class_install_property (object_class,
                                   PROP_PATH,
                                   g_param_spec_object ("path",
                                                        "",
                                                        "",
                                                        G_TYPE_FILE,
                                                        G_PARAM_READWRITE | G_PARAM_CONSTRUCT_ONLY));
}

static void
ostree_repo_init (OstreeRepo *self)
{
  g_mutex_init (&self->cache_lock);
}

OstreeRepo*
ostree_repo_new (GFile *path)
{
  return g_object_new (OSTREE_TYPE_REPO, "path", path, NULL);
}

static gboolean
parse_rev_file (OstreeRepo     *self,
                GFile          *f,
                char          **sha256,
                GError        **error) G_GNUC_UNUSED;

static gboolean
parse_rev_file (OstreeRepo     *self,
                GFile          *f,
                char          **sha256,
                GError        **error)
{
  gboolean ret = FALSE;
  GError *temp_error = NULL;
  ot_lfree char *rev = NULL;

  if ((rev = gs_file_load_contents_utf8 (f, NULL, &temp_error)) == NULL)
    goto out;

  if (rev == NULL)
    {
      if (g_error_matches (temp_error, G_FILE_ERROR, G_FILE_ERROR_NOENT))
        {
          g_clear_error (&temp_error);
        }
      else
        {
          g_propagate_error (error, temp_error);
          goto out;
        }
    }
  else
    {
      g_strchomp (rev);
    }

  if (g_str_has_prefix (rev, "ref: "))
    {
      ot_lobj GFile *ref = NULL;
      char *ref_sha256;
      gboolean subret;

      ref = g_file_resolve_relative_path (self->local_heads_dir, rev + 5);
      subret = parse_rev_file (self, ref, &ref_sha256, error);
        
      if (!subret)
        {
          g_free (ref_sha256);
          goto out;
        }
      
      g_free (rev);
      rev = ref_sha256;
    }
  else 
    {
      if (!ostree_validate_checksum_string (rev, error))
        goto out;
    }

  ot_transfer_out_value(sha256, &rev);
  ret = TRUE;
 out:
  return ret;
}

static gboolean
find_rev_in_remotes (OstreeRepo         *self,
                     const char         *rev,
                     GFile             **out_file,
                     GError            **error)
{
  gboolean ret = FALSE;
  GError *temp_error = NULL;
  ot_lobj GFileEnumerator *dir_enum = NULL;
  ot_lobj GFileInfo *file_info = NULL;
  ot_lobj GFile *child = NULL;
  ot_lobj GFile *ret_file = NULL;

  dir_enum = g_file_enumerate_children (self->remote_heads_dir, OSTREE_GIO_FAST_QUERYINFO,
                                        G_FILE_QUERY_INFO_NOFOLLOW_SYMLINKS,
                                        NULL, error);
  if (!dir_enum)
    goto out;

  while ((file_info = g_file_enumerator_next_file (dir_enum, NULL, error)) != NULL)
    {
      if (g_file_info_get_file_type (file_info) == G_FILE_TYPE_DIRECTORY)
        {
          g_clear_object (&child);
          child = g_file_get_child (self->remote_heads_dir,
                                    g_file_info_get_name (file_info));
          g_clear_object (&ret_file);
          ret_file = g_file_resolve_relative_path (child, rev);
          if (!g_file_query_exists (ret_file, NULL))
            g_clear_object (&ret_file);
        }

      g_clear_object (&file_info);
      
      if (ret_file)
        break;
    }
  if (temp_error != NULL)
    {
      g_propagate_error (error, temp_error);
      goto out;
    }

  ret = TRUE;
  ot_transfer_out_value (out_file, &ret_file);
 out:
  return ret;
}

gboolean
ostree_repo_resolve_rev (OstreeRepo     *self,
                         const char     *rev,
                         gboolean        allow_noent,
                         char          **sha256,
                         GError        **error)
{
  gboolean ret = FALSE;
  GError *temp_error = NULL;
  ot_lfree char *tmp = NULL;
  ot_lfree char *tmp2 = NULL;
  ot_lfree char *ret_rev = NULL;
  ot_lobj GFile *child = NULL;
  ot_lobj GFile *origindir = NULL;
  ot_lvariant GVariant *commit = NULL;
  ot_lvariant GVariant *parent_csum_v = NULL;
  
  g_return_val_if_fail (rev != NULL, FALSE);

  if (!ostree_validate_rev (rev, error))
    goto out;

  /* We intentionally don't allow a ref that looks like a checksum */
  if (ostree_validate_checksum_string (rev, NULL))
    {
      ret_rev = g_strdup (rev);
    }
  else if (g_str_has_suffix (rev, "^"))
    {
      tmp = g_strdup (rev);
      tmp[strlen(tmp) - 1] = '\0';

      if (!ostree_repo_resolve_rev (self, tmp, allow_noent, &tmp2, error))
        goto out;

      if (!ostree_repo_load_variant (self, OSTREE_OBJECT_TYPE_COMMIT, tmp2, &commit, error))
        goto out;
      
      g_variant_get_child (commit, 1, "@ay", &parent_csum_v);
      if (g_variant_n_children (parent_csum_v) == 0)
        {
          g_set_error (error, G_IO_ERROR, G_IO_ERROR_FAILED,
                       "Commit %s has no parent", tmp2);
          goto out;
        }
      ret_rev = ostree_checksum_from_bytes_v (parent_csum_v);
    }
  else
    {
      child = g_file_resolve_relative_path (self->local_heads_dir, rev);

      if (!g_file_query_exists (child, NULL))
        {
          g_clear_object (&child);

          child = g_file_resolve_relative_path (self->remote_heads_dir, rev);

          if (!g_file_query_exists (child, NULL))
            {
              g_clear_object (&child);
              
              if (!find_rev_in_remotes (self, rev, &child, error))
                goto out;
              
              if (child == NULL)
                {
                  if (self->parent_repo)
                    {
                      if (!ostree_repo_resolve_rev (self->parent_repo, rev,
                                                    allow_noent, &ret_rev,
                                                    error))
                        goto out;
                    }
                  else if (!allow_noent)
                    {
                      g_set_error (error, G_IO_ERROR, G_IO_ERROR_FAILED,
                                   "Rev '%s' not found", rev);
                      goto out;
                    }
                  else
                    g_clear_object (&child);
                }
            }
        }

      if (child)
        {
          if ((ret_rev = gs_file_load_contents_utf8 (child, NULL, &temp_error)) == NULL)
            {
              g_propagate_error (error, temp_error);
              g_prefix_error (error, "Couldn't open ref '%s': ", gs_file_get_path_cached (child));
              goto out;
            }

          g_strchomp (ret_rev);
          if (!ostree_validate_checksum_string (ret_rev, error))
            goto out;
        }
    }

  ot_transfer_out_value(sha256, &ret_rev);
  ret = TRUE;
 out:
  return ret;
}

static gboolean
write_checksum_file (GFile *parentdir,
                     const char *name,
                     const char *sha256,
                     GError **error)
{
  gboolean ret = FALSE;
  gsize bytes_written;
  int i;
  ot_lobj GFile *parent = NULL;
  ot_lobj GFile *child = NULL;
  ot_lobj GOutputStream *out = NULL;
  ot_lptrarray GPtrArray *components = NULL;

  if (!ostree_validate_checksum_string (sha256, error))
    goto out;

  if (ostree_validate_checksum_string (name, NULL))
    {
      g_set_error (error, G_IO_ERROR, G_IO_ERROR_FAILED,
                   "Rev name '%s' looks like a checksum", name);
      goto out;
    }

  if (!ot_util_path_split_validate (name, &components, error))
    goto out;

  if (components->len == 0)
    {
      g_set_error (error, G_IO_ERROR, G_IO_ERROR_FAILED,
                   "Invalid empty ref name");
      goto out;
    }

  parent = g_object_ref (parentdir);
  for (i = 0; i+1 < components->len; i++)
    {
      child = g_file_get_child (parent, (char*)components->pdata[i]);

      if (!gs_file_ensure_directory (child, FALSE, NULL, error))
        goto out;

      g_clear_object (&parent);
      parent = child;
      child = NULL;
    }

  child = g_file_get_child (parent, components->pdata[components->len - 1]);
  if ((out = (GOutputStream*)g_file_replace (child, NULL, FALSE, 0, NULL, error)) == NULL)
    goto out;
  if (!g_output_stream_write_all (out, sha256, strlen (sha256), &bytes_written, NULL, error))
    goto out;
  if (!g_output_stream_write_all (out, "\n", 1, &bytes_written, NULL, error))
    goto out;
  if (!g_output_stream_close (out, NULL, error))
    goto out;

  ret = TRUE;
 out:
  return ret;
}

/**
 * ostree_repo_get_config:
 * @self:
 *
 * Returns: (transfer none): The repository configuration; do not modify
 */
GKeyFile *
ostree_repo_get_config (OstreeRepo *self)
{
  g_return_val_if_fail (self->inited, NULL);

  return self->config;
}

/**
 * ostree_repo_copy_config:
 * @self:
 *
 * Returns: (transfer full): A newly-allocated copy of the repository config
 */
GKeyFile *
ostree_repo_copy_config (OstreeRepo *self)
{
  GKeyFile *copy;
  char *data;
  gsize len;

  g_return_val_if_fail (self->inited, NULL);

  copy = g_key_file_new ();
  data = g_key_file_to_data (self->config, &len, NULL);
  if (!g_key_file_load_from_data (copy, data, len, 0, NULL))
    g_assert_not_reached ();
  g_free (data);
  return copy;
}

/**
 * ostree_repo_write_config:
 * @self:
 * @new_config: Overwrite the config file with this data.  Do not change later!
 * @error: a #GError
 *
 * Save @new_config in place of this repository's config file.  Note
 * that @new_config should not be modified after - this function
 * simply adds a reference.
 */
gboolean
ostree_repo_write_config (OstreeRepo *self,
                          GKeyFile   *new_config,
                          GError    **error)
{
  gboolean ret = FALSE;
  ot_lfree char *data = NULL;
  gsize len;

  g_return_val_if_fail (self->inited, FALSE);

  data = g_key_file_to_data (new_config, &len, error);
  if (!g_file_replace_contents (self->config_file, data, len, NULL, FALSE, 0, NULL,
                                NULL, error))
    goto out;
  
  g_key_file_free (self->config);
  self->config = g_key_file_new ();
  if (!g_key_file_load_from_data (self->config, data, len, 0, error))
    goto out;

  ret = TRUE;
 out:
  return ret;
}

gboolean
ostree_repo_mode_from_string (const char      *mode,
                              OstreeRepoMode  *out_mode,
                              GError         **error)
{
  gboolean ret = FALSE;
  OstreeRepoMode ret_mode;

  if (strcmp (mode, "bare") == 0)
    ret_mode = OSTREE_REPO_MODE_BARE;
  else if (strcmp (mode, "archive") == 0)
    ret_mode = OSTREE_REPO_MODE_ARCHIVE;
  else if (strcmp (mode, "archive-z2") == 0)
    ret_mode = OSTREE_REPO_MODE_ARCHIVE_Z2;
  else
    {
      g_set_error (error, G_IO_ERROR, G_IO_ERROR_FAILED,
                   "Invalid mode '%s' in repository configuration", mode);
      goto out;
    }

  ret = TRUE;
  *out_mode = ret_mode;
 out:
  return ret;
}

gboolean
ostree_repo_check (OstreeRepo *self, GError **error)
{
  gboolean ret = FALSE;
  gboolean is_archive;
  ot_lfree char *version = NULL;
  ot_lfree char *mode = NULL;
  ot_lfree char *parent_repo_path = NULL;

  g_return_val_if_fail (error == NULL || *error == NULL, FALSE);

  if (self->inited)
    return TRUE;

  if (!g_file_test (gs_file_get_path_cached (self->objects_dir), G_FILE_TEST_IS_DIR))
    {
      g_set_error (error, G_IO_ERROR, G_IO_ERROR_FAILED,
                   "Couldn't find objects directory '%s'",
                   gs_file_get_path_cached (self->objects_dir));
      goto out;
    }

  if (!gs_file_ensure_directory (self->pending_dir, FALSE, NULL, error))
    goto out;
  
  self->config = g_key_file_new ();
  if (!g_key_file_load_from_file (self->config, gs_file_get_path_cached (self->config_file), 0, error))
    {
      g_prefix_error (error, "Couldn't parse config file: ");
      goto out;
    }

  version = g_key_file_get_value (self->config, "core", "repo_version", error);
  if (!version)
    goto out;

  if (strcmp (version, "1") != 0)
    {
      g_set_error (error, G_IO_ERROR, G_IO_ERROR_FAILED,
                   "Invalid repository version '%s'", version);
      goto out;
    }

  if (!ot_keyfile_get_boolean_with_default (self->config, "core", "archive",
                                            FALSE, &is_archive, error))
    goto out;
  
  if (is_archive)
    self->mode = OSTREE_REPO_MODE_ARCHIVE;
  else
    {
      if (!ot_keyfile_get_value_with_default (self->config, "core", "mode",
                                              "bare", &mode, error))
        goto out;

      if (!ostree_repo_mode_from_string (mode, &self->mode, error))
        goto out;
    }

  if (!ot_keyfile_get_value_with_default (self->config, "core", "parent",
                                          NULL, &parent_repo_path, error))
    goto out;

  if (parent_repo_path && parent_repo_path[0])
    {
      ot_lobj GFile *parent_repo_f = g_file_new_for_path (parent_repo_path);

      self->parent_repo = ostree_repo_new (parent_repo_f);

      if (!ostree_repo_check (self->parent_repo, error))
        {
          g_prefix_error (error, "While checking parent repository '%s': ",
                          gs_file_get_path_cached (parent_repo_f));
          goto out;
        }
    }

  if (!ot_keyfile_get_boolean_with_default (self->config, "core", "enable-uncompressed-cache",
                                            TRUE, &self->enable_uncompressed_cache, error))
    goto out;

  self->inited = TRUE;
  
  ret = TRUE;
 out:
  return ret;
}

GFile *
ostree_repo_get_path (OstreeRepo  *self)
{
  return self->repodir;
}

GFile *
ostree_repo_get_tmpdir (OstreeRepo  *self)
{
  return self->tmp_dir;
}

OstreeRepoMode
ostree_repo_get_mode (OstreeRepo  *self)
{
  g_return_val_if_fail (self->inited, FALSE);

  return self->mode;
}

/**
 * ostree_repo_get_parent:
 * @self:
 * 
 * Before this function can be used, ostree_repo_init() must have been
 * called.
 *
 * Returns: (transfer none): Parent repository, or %NULL if none
 */
OstreeRepo *
ostree_repo_get_parent (OstreeRepo  *self)
{
  return self->parent_repo;
}

GFile *
ostree_repo_get_file_object_path (OstreeRepo   *self,
                                  const char   *checksum)
{
  return ostree_repo_get_object_path (self, checksum, OSTREE_OBJECT_TYPE_FILE);
}

GFile *
ostree_repo_get_archive_content_path (OstreeRepo    *self,
                                      const char    *checksum)
{
  ot_lfree char *path = NULL;

  g_assert (ostree_repo_get_mode (self) == OSTREE_REPO_MODE_ARCHIVE);

  path = ostree_get_relative_archive_content_path (checksum);
  return g_file_resolve_relative_path (self->repodir, path);
}

/**
 * ensure_file_data_synced:
 *
 * Ensure that in case of a power cut, these files have the data we
 * want.   See http://lwn.net/Articles/322823/
 */
static gboolean
ensure_file_data_synced (GFile         *file,
                         GCancellable  *cancellable,
                         GError       **error)
{
  gboolean ret = FALSE;
  int fd = -1;

  if (!ot_unix_open_noatime (gs_file_get_path_cached (file), &fd, error))
    goto out;

  if (!ot_unix_fdatasync (fd, error))
    goto out;

  if (!ot_unix_close (fd, error))
    goto out;
  fd = -1;

  ret = TRUE;
 out:
  if (fd != -1)
    (void) close (fd);
  return ret;
}

static gboolean
commit_loose_object_impl (OstreeRepo        *self,
                          GFile             *tempfile_path,
                          GFile             *dest,
                          gboolean           is_regular,
                          GCancellable      *cancellable,
                          GError           **error)
{
  gboolean ret = FALSE;
  ot_lobj GFile *parent = NULL;

  parent = g_file_get_parent (dest);
  if (!gs_file_ensure_directory (parent, FALSE, cancellable, error))
    goto out;

  if (is_regular)
    {
      if (!ensure_file_data_synced (tempfile_path, cancellable, error))
        goto out;
    }
  
  if (rename (gs_file_get_path_cached (tempfile_path), gs_file_get_path_cached (dest)) < 0)
    {
      if (errno != EEXIST)
        {
          ot_util_set_error_from_errno (error, errno);
          g_prefix_error (error, "Storing file '%s': ",
                          gs_file_get_path_cached (dest));
          goto out;
        }
    }

  ret = TRUE;
 out:
  return ret;
}

static gboolean
commit_loose_object_trusted (OstreeRepo        *self,
                             const char        *checksum,
                             OstreeObjectType   objtype,
                             GFile             *tempfile_path,
                             gboolean           is_regular,
                             GCancellable      *cancellable,
                             GError           **error)
{
  gboolean ret = FALSE;
  ot_lobj GFile *dest_file = NULL;

  dest_file = ostree_repo_get_object_path (self, checksum, objtype);

  if (!commit_loose_object_impl (self, tempfile_path, dest_file, is_regular,
                                 cancellable, error))
    goto out;

  ret = TRUE;
 out:
  return ret;
}

static gboolean
stage_object (OstreeRepo         *self,
              OstreeObjectType    objtype,
              const char         *expected_checksum,
              GInputStream       *input,
              guint64             file_object_length,
              guchar            **out_csum,
              GCancellable       *cancellable,
              GError            **error)
{
  gboolean ret = FALSE;
  const char *actual_checksum;
  gboolean do_commit;
  OstreeRepoMode repo_mode;
  ot_lobj GFileInfo *temp_info = NULL;
  ot_lobj GFile *temp_file = NULL;
  ot_lobj GFile *raw_temp_file = NULL;
  ot_lobj GFile *stored_path = NULL;
  ot_lfree guchar *ret_csum = NULL;
  ot_lobj OstreeChecksumInputStream *checksum_input = NULL;
  gboolean have_obj;
  GChecksum *checksum = NULL;
  gboolean staged_raw_file = FALSE;
  gboolean staged_archive_file = FALSE;
  gboolean temp_file_is_regular;

  g_return_val_if_fail (self->in_transaction, FALSE);
  
  if (g_cancellable_set_error_if_cancelled (cancellable, error))
    return FALSE;

  g_assert (expected_checksum || out_csum);

  if (expected_checksum)
    {
      if (!repo_find_object (self, objtype, expected_checksum, &stored_path,
                             cancellable, error))
        goto out;
    }

  repo_mode = ostree_repo_get_mode (self);

  if (out_csum)
    {
      checksum = g_checksum_new (G_CHECKSUM_SHA256);
      if (input)
        checksum_input = ostree_checksum_input_stream_new (input, checksum);
    }

  if (objtype == OSTREE_OBJECT_TYPE_FILE)
    {
      ot_lobj GInputStream *file_input = NULL;
      ot_lobj GFileInfo *file_info = NULL;
      ot_lvariant GVariant *xattrs = NULL;

      if (!ostree_content_stream_parse (FALSE, checksum_input ? (GInputStream*)checksum_input : input,
                                        file_object_length, FALSE,
                                        &file_input, &file_info, &xattrs,
                                        cancellable, error))
        goto out;

      temp_file_is_regular = g_file_info_get_file_type (file_info) == G_FILE_TYPE_REGULAR;

      if (repo_mode == OSTREE_REPO_MODE_BARE)
        {
          if (!ostree_create_temp_file_from_input (self->tmp_dir,
                                                   ostree_object_type_to_string (objtype), NULL,
                                                   file_info, xattrs, file_input,
                                                   &temp_file,
                                                   cancellable, error))
            goto out;
          staged_raw_file = TRUE;
        }
      else if (repo_mode == OSTREE_REPO_MODE_ARCHIVE_Z2)
        {
          ot_lvariant GVariant *file_meta = NULL;
          ot_lobj GOutputStream *temp_out = NULL;
          ot_lobj GConverter *zlib_compressor = NULL;
          ot_lobj GOutputStream *compressed_out_stream = NULL;

          if (!ostree_create_temp_regular_file (self->tmp_dir,
                                                ostree_object_type_to_string (objtype), NULL,
                                                &temp_file, &temp_out,
                                                cancellable, error))
            goto out;
          temp_file_is_regular = TRUE;

          file_meta = ostree_zlib_file_header_new (file_info, xattrs);

          if (!ostree_write_variant_with_size (temp_out, file_meta, 0, NULL, NULL,
                                               cancellable, error))
            goto out;

          if (g_file_info_get_file_type (file_info) == G_FILE_TYPE_REGULAR)
            {
              zlib_compressor = (GConverter*)g_zlib_compressor_new (G_ZLIB_COMPRESSOR_FORMAT_RAW, 9);
              compressed_out_stream = g_converter_output_stream_new (temp_out, zlib_compressor);
              
              if (g_output_stream_splice (compressed_out_stream, file_input,
                                          G_OUTPUT_STREAM_SPLICE_CLOSE_TARGET,
                                          cancellable, error) < 0)
                goto out;
            }

          if (!g_output_stream_close (temp_out, cancellable, error))
            goto out;
        }
      else if (repo_mode == OSTREE_REPO_MODE_ARCHIVE)
        {
          ot_lvariant GVariant *file_meta = NULL;
          ot_lobj GInputStream *file_meta_input = NULL;
          ot_lobj GFileInfo *archive_content_file_info = NULL;
          
          file_meta = ostree_file_header_new (file_info, xattrs);
          file_meta_input = ot_variant_read (file_meta);

          if (!ostree_create_temp_file_from_input (self->tmp_dir,
                                                   ostree_object_type_to_string (objtype), NULL,
                                                   NULL, NULL, file_meta_input,
                                                   &temp_file,
                                                   cancellable, error))
            goto out;

          if (g_file_info_get_file_type (file_info) == G_FILE_TYPE_REGULAR)
            {
              ot_lobj GOutputStream *content_out = NULL;
              guint32 src_mode;
              guint32 target_mode;

              if (!ostree_create_temp_regular_file (self->tmp_dir,
                                                    ostree_object_type_to_string (objtype), NULL,
                                                    &raw_temp_file, &content_out,
                                                    cancellable, error))
                goto out;

              /* Don't make setuid files in the repository; all we want to preserve
               * is file type and permissions.
               */
              src_mode = g_file_info_get_attribute_uint32 (file_info, "unix::mode");
              target_mode = src_mode & (S_IRWXU | S_IRWXG | S_IRWXO | S_IFMT);
              /* However, do ensure that archive mode files are
               * readable by all users.  This is important for serving
               * files via HTTP.
               */
              target_mode |= (S_IRUSR | S_IRGRP | S_IROTH);
              
              if (chmod (gs_file_get_path_cached (raw_temp_file), target_mode) < 0)
                {
                  ot_util_set_error_from_errno (error, errno);
                  goto out;
                }

              if (g_output_stream_splice (content_out, file_input,
                                          G_OUTPUT_STREAM_SPLICE_CLOSE_SOURCE | G_OUTPUT_STREAM_SPLICE_CLOSE_TARGET,
                                          cancellable, error) < 0)
                goto out;

              staged_archive_file = TRUE;
            }
        }
      else
        g_assert_not_reached ();
    }
  else
    {
      if (!ostree_create_temp_file_from_input (self->tmp_dir,
                                               ostree_object_type_to_string (objtype), NULL,
                                               NULL, NULL,
                                               checksum_input ? (GInputStream*)checksum_input : input,
                                               &temp_file,
                                               cancellable, error))
        goto out;
      temp_file_is_regular = TRUE;
    }

  if (!checksum)
    actual_checksum = expected_checksum;
  else
    {
      actual_checksum = g_checksum_get_string (checksum);
      if (expected_checksum && strcmp (actual_checksum, expected_checksum) != 0)
        {
          g_set_error (error, G_IO_ERROR, G_IO_ERROR_FAILED,
                       "Corrupted %s object %s (actual checksum is %s)",
                       ostree_object_type_to_string (objtype),
                       expected_checksum, actual_checksum);
          goto out;
        }
    }
          
  if (!ostree_repo_has_object (self, objtype, actual_checksum, &have_obj,
                               cancellable, error))
    goto out;
          
  do_commit = !have_obj;

  if (do_commit)
    {
      /* Only do this if we *didn't* stage a bare file above */
      if (!staged_raw_file
          && objtype == OSTREE_OBJECT_TYPE_FILE && self->mode == OSTREE_REPO_MODE_BARE)
        {
          ot_lobj GInputStream *file_input = NULL;
          ot_lobj GFileInfo *file_info = NULL;
          ot_lvariant GVariant *xattrs = NULL;
          gboolean is_regular;
              
          if (!ostree_content_file_parse (FALSE, temp_file, FALSE, &file_input,
                                          &file_info, &xattrs,
                                          cancellable, error))
            goto out;
              
          if (!ostree_create_temp_file_from_input (self->tmp_dir,
                                                   ostree_object_type_to_string (objtype), NULL,
                                                   file_info, xattrs, file_input,
                                                   &raw_temp_file,
                                                   cancellable, error))
            goto out;

          is_regular = g_file_info_get_file_type (file_info) == G_FILE_TYPE_REGULAR;

          if (!commit_loose_object_trusted (self, actual_checksum, objtype, 
                                            raw_temp_file, is_regular, cancellable, error))
            goto out;
          g_clear_object (&raw_temp_file);
        }
      else
        {
          /* Commit content first so the process is atomic */
          if (staged_archive_file)
            {
              ot_lobj GFile *archive_content_dest = NULL;

              archive_content_dest = ostree_repo_get_archive_content_path (self, actual_checksum);
                                                                   
              if (!commit_loose_object_impl (self, raw_temp_file, archive_content_dest, TRUE,
                                             cancellable, error))
                goto out;
              g_clear_object (&raw_temp_file);
            }
          if (!commit_loose_object_trusted (self, actual_checksum, objtype, temp_file, temp_file_is_regular,
                                            cancellable, error))
            goto out;
          g_clear_object (&temp_file);
        }
    }
      
  if (checksum)
    ret_csum = ot_csum_from_gchecksum (checksum);

  ret = TRUE;
  ot_transfer_out_value(out_csum, &ret_csum);
 out:
  if (temp_file)
    (void) unlink (gs_file_get_path_cached (temp_file));
  if (raw_temp_file)
    (void) unlink (gs_file_get_path_cached (raw_temp_file));
  g_clear_pointer (&checksum, (GDestroyNotify) g_checksum_free);
  return ret;
}

static gboolean
get_loose_object_dirs (OstreeRepo       *self,
                       GPtrArray       **out_object_dirs,
                       GCancellable     *cancellable,
                       GError          **error)
{
  gboolean ret = FALSE;
  GError *temp_error = NULL;
  ot_lobj GFile *object_dir_to_scan;
  ot_lptrarray GPtrArray *ret_object_dirs = NULL;
  ot_lobj GFileEnumerator *enumerator = NULL;
  ot_lobj GFileInfo *file_info = NULL;

  ret_object_dirs = g_ptr_array_new_with_free_func ((GDestroyNotify)g_object_unref);

  if (ostree_repo_get_mode (self) == OSTREE_REPO_MODE_ARCHIVE_Z2)
    object_dir_to_scan = g_file_get_child (self->uncompressed_objects_dir, "objects");
  else
    object_dir_to_scan = g_object_ref (self->objects_dir);

  enumerator = g_file_enumerate_children (object_dir_to_scan, OSTREE_GIO_FAST_QUERYINFO, 
                                          G_FILE_QUERY_INFO_NOFOLLOW_SYMLINKS,
                                          cancellable, 
                                          &temp_error);
  if (!enumerator)
    {
      if (g_error_matches (temp_error, G_IO_ERROR, G_IO_ERROR_NOT_FOUND))
        {
          g_clear_error (&temp_error);
          ret = TRUE;
          ot_transfer_out_value (out_object_dirs, &ret_object_dirs);
        }
      else
        g_propagate_error (error, temp_error);

      goto out;
    }

  while ((file_info = g_file_enumerator_next_file (enumerator, cancellable, &temp_error)) != NULL)
    {
      const char *name;
      guint32 type;

      name = g_file_info_get_attribute_byte_string (file_info, "standard::name"); 
      type = g_file_info_get_attribute_uint32 (file_info, "standard::type");
      
      if (strlen (name) == 2 && type == G_FILE_TYPE_DIRECTORY)
        {
          GFile *objdir = g_file_get_child (object_dir_to_scan, name);
          g_ptr_array_add (ret_object_dirs, objdir);  /* transfer ownership */
        }
      g_clear_object (&file_info);
    }
  if (file_info == NULL && temp_error != NULL)
    {
      g_propagate_error (error, temp_error);
      goto out;
    }
  if (!g_file_enumerator_close (enumerator, cancellable, error))
    goto out;

  ret = TRUE;
  ot_transfer_out_value (out_object_dirs, &ret_object_dirs);
 out:
  return ret;
}

typedef struct {
  dev_t dev;
  ino_t ino;
} OstreeDevIno;

static guint
devino_hash (gconstpointer a)
{
  OstreeDevIno *a_i = (gpointer)a;
  return (guint) (a_i->dev + a_i->ino);
}

static int
devino_equal (gconstpointer   a,
              gconstpointer   b)
{
  OstreeDevIno *a_i = (gpointer)a;
  OstreeDevIno *b_i = (gpointer)b;
  return a_i->dev == b_i->dev
    && a_i->ino == b_i->ino;
}

static gboolean
scan_loose_devino (OstreeRepo                     *self,
                   GHashTable                     *devino_cache,
                   GCancellable                   *cancellable,
                   GError                        **error)
{
  gboolean ret = FALSE;
  GError *temp_error = NULL;
  guint i;
  OstreeRepoMode repo_mode;
  ot_lptrarray GPtrArray *object_dirs = NULL;
  ot_lobj GFile *objdir = NULL;

  if (self->parent_repo)
    {
      if (!scan_loose_devino (self->parent_repo, devino_cache, cancellable, error))
        goto out;
    }

  repo_mode = ostree_repo_get_mode (self);

  if (!get_loose_object_dirs (self, &object_dirs, cancellable, error))
    goto out;

  for (i = 0; i < object_dirs->len; i++)
    {
      GFile *objdir = object_dirs->pdata[i];
      ot_lobj GFileEnumerator *enumerator = NULL;
      ot_lobj GFileInfo *file_info = NULL;
      const char *dirname;

      enumerator = g_file_enumerate_children (objdir, OSTREE_GIO_FAST_QUERYINFO, 
                                              G_FILE_QUERY_INFO_NOFOLLOW_SYMLINKS,
                                              cancellable, 
                                              error);
      if (!enumerator)
        goto out;

      dirname = gs_file_get_basename_cached (objdir);

      while ((file_info = g_file_enumerator_next_file (enumerator, cancellable, &temp_error)) != NULL)
        {
          const char *name;
          const char *dot;
          guint32 type;
          OstreeDevIno *key;
          GString *checksum;
          gboolean skip;

          name = g_file_info_get_attribute_byte_string (file_info, "standard::name"); 
          type = g_file_info_get_attribute_uint32 (file_info, "standard::type");

          if (type == G_FILE_TYPE_DIRECTORY)
            {
              g_clear_object (&file_info);
              continue;
            }
      
          switch (repo_mode)
            {
            case OSTREE_REPO_MODE_ARCHIVE:
              skip = !g_str_has_suffix (name, ".filecontent");
              break;
            case OSTREE_REPO_MODE_ARCHIVE_Z2:
            case OSTREE_REPO_MODE_BARE:
              skip = !g_str_has_suffix (name, ".file");
              break;
            default:
              g_assert_not_reached ();
            }
          if (skip)
            continue;

          dot = strrchr (name, '.');
          g_assert (dot);

          if ((dot - name) != 62)
            {
              g_clear_object (&file_info);
              continue;
            }
                  
          checksum = g_string_new (dirname);
          g_string_append_len (checksum, name, 62);
          
          key = g_new (OstreeDevIno, 1);
          key->dev = g_file_info_get_attribute_uint32 (file_info, "unix::device");
          key->ino = g_file_info_get_attribute_uint64 (file_info, "unix::inode");
          
          g_hash_table_replace (devino_cache, key, g_string_free (checksum, FALSE));
          g_clear_object (&file_info);
        }

      if (temp_error != NULL)
        {
          g_propagate_error (error, temp_error);
          goto out;
        }
      if (!g_file_enumerator_close (enumerator, NULL, error))
        goto out;
    }

  ret = TRUE;
 out:
  return ret;
}

static const char *
devino_cache_lookup (OstreeRepo           *self,
                     GFileInfo            *finfo)
{
  OstreeDevIno dev_ino;

  if (!self->loose_object_devino_hash)
    return NULL;

  dev_ino.dev = g_file_info_get_attribute_uint32 (finfo, "unix::device");
  dev_ino.ino = g_file_info_get_attribute_uint64 (finfo, "unix::inode");
  return g_hash_table_lookup (self->loose_object_devino_hash, &dev_ino);
}

gboolean
ostree_repo_prepare_transaction (OstreeRepo     *self,
                                 gboolean        enable_commit_hardlink_scan,
                                 GCancellable   *cancellable,
                                 GError        **error)
{
  gboolean ret = FALSE;

  g_return_val_if_fail (self->in_transaction == FALSE, FALSE);

  self->in_transaction = TRUE;

  if (enable_commit_hardlink_scan)
    {
      if (!self->loose_object_devino_hash)
        self->loose_object_devino_hash = g_hash_table_new_full (devino_hash, devino_equal, g_free, g_free);
      g_hash_table_remove_all (self->loose_object_devino_hash);
      if (!scan_loose_devino (self, self->loose_object_devino_hash, cancellable, error))
        goto out;
    }

  ret = TRUE;
 out:
  return ret;
}

gboolean      
ostree_repo_commit_transaction (OstreeRepo     *self,
                                GCancellable   *cancellable,
                                GError        **error)
{
  gboolean ret = FALSE;

  g_return_val_if_fail (self->in_transaction == TRUE, FALSE);

  ret = TRUE;
  /* out: */
  self->in_transaction = FALSE;
  if (self->loose_object_devino_hash)
    g_hash_table_remove_all (self->loose_object_devino_hash);

  return ret;
}

gboolean
ostree_repo_abort_transaction (OstreeRepo     *self,
                               GCancellable   *cancellable,
                               GError        **error)
{
  gboolean ret = FALSE;

  self->in_transaction = FALSE;
  if (self->loose_object_devino_hash)
    g_hash_table_remove_all (self->loose_object_devino_hash);

  ret = TRUE;
  return ret;
}

/**
 * ostree_repo_stage_metadata:
 * 
 * Store the metadata object @variant.  Return the checksum
 * as @out_csum.
 *
 * If @expected_checksum is not %NULL, verify it against the
 * computed checksum.
 */
gboolean
ostree_repo_stage_metadata (OstreeRepo         *self,
                            OstreeObjectType    type,
                            const char         *expected_checksum,
                            GVariant           *variant,
                            guchar            **out_csum,
                            GCancellable       *cancellable,
                            GError            **error)
{
  ot_lobj GInputStream *input = NULL;
  ot_lvariant GVariant *normalized = NULL;

  normalized = g_variant_get_normal_form (variant);
  input = ot_variant_read (normalized);
  
  return stage_object (self, type, expected_checksum, input, 0, out_csum,
                       cancellable, error);
}

/**
 * ostree_repo_stage_metadata_trusted:
 * 
 * Store the metadata object @variant; the provided @checksum
 * is trusted.
 */
gboolean
ostree_repo_stage_metadata_trusted (OstreeRepo         *self,
                                    OstreeObjectType    type,
                                    const char         *checksum,
                                    GVariant           *variant,
                                    GCancellable       *cancellable,
                                    GError            **error)
{
  ot_lobj GInputStream *input = NULL;
  ot_lvariant GVariant *normalized = NULL;

  normalized = g_variant_get_normal_form (variant);
  input = ot_variant_read (normalized);
  
  return stage_object (self, type, checksum, input, 0, NULL,
                       cancellable, error);
}

typedef struct {
  OstreeRepo *repo;
  OstreeObjectType objtype;
  char *expected_checksum;
  GVariant *object;
  GCancellable *cancellable;
  GSimpleAsyncResult *result;
  
  guchar *result_csum;
} StageMetadataAsyncData;

static void
stage_metadata_async_data_free (gpointer user_data)
{
  StageMetadataAsyncData *data = user_data;

  g_clear_object (&data->repo);
  g_clear_object (&data->cancellable);
  g_variant_unref (data->object);
  g_free (data->result_csum);
  g_free (data->expected_checksum);
  g_free (data);
}

static void
stage_metadata_thread (GSimpleAsyncResult  *res,
                       GObject             *object,
                       GCancellable        *cancellable)
{
  GError *error = NULL;
  StageMetadataAsyncData *data;

  data = g_simple_async_result_get_op_res_gpointer (res);
  if (!ostree_repo_stage_metadata (data->repo, data->objtype, data->expected_checksum,
                                   data->object,
                                   &data->result_csum,
                                   cancellable, &error))
    g_simple_async_result_take_error (res, error);
}

/**
 * ostree_repo_stage_metadata_async:
 * 
 * Asynchronously store the metadata object @variant.  If provided,
 * the checksum @expected_checksum will be verified.
 */
void          
ostree_repo_stage_metadata_async (OstreeRepo               *self,
                                  OstreeObjectType          objtype,
                                  const char               *expected_checksum,
                                  GVariant                 *object,
                                  GCancellable             *cancellable,
                                  GAsyncReadyCallback       callback,
                                  gpointer                  user_data)
{
  StageMetadataAsyncData *asyncdata;

  asyncdata = g_new0 (StageMetadataAsyncData, 1);
  asyncdata->repo = g_object_ref (self);
  asyncdata->objtype = objtype;
  asyncdata->expected_checksum = g_strdup (expected_checksum);
  asyncdata->object = g_variant_ref (object);
  asyncdata->cancellable = cancellable ? g_object_ref (cancellable) : NULL;

  asyncdata->result = g_simple_async_result_new ((GObject*) self,
                                                 callback, user_data,
                                                 ostree_repo_stage_metadata_async);

  g_simple_async_result_set_op_res_gpointer (asyncdata->result, asyncdata,
                                             stage_metadata_async_data_free);
  g_simple_async_result_run_in_thread (asyncdata->result, stage_metadata_thread, G_PRIORITY_DEFAULT, cancellable);
  g_object_unref (asyncdata->result);
}

gboolean
ostree_repo_stage_metadata_finish (OstreeRepo        *self,
                                   GAsyncResult      *result,
                                   guchar           **out_csum,
                                   GError           **error)
{
  GSimpleAsyncResult *simple = G_SIMPLE_ASYNC_RESULT (result);
  StageMetadataAsyncData *data;

  g_warn_if_fail (g_simple_async_result_get_source_tag (simple) == ostree_repo_stage_metadata_async);

  if (g_simple_async_result_propagate_error (simple, error))
    return FALSE;

  data = g_simple_async_result_get_op_res_gpointer (simple);
  /* Transfer ownership */
  *out_csum = data->result_csum;
  data->result_csum = NULL;
  return TRUE;
}

static gboolean
stage_directory_meta (OstreeRepo   *self,
                      GFileInfo    *file_info,
                      GVariant     *xattrs,
                      guchar      **out_csum,
                      GCancellable *cancellable,
                      GError      **error)
{
  ot_lvariant GVariant *dirmeta = NULL;

  if (g_cancellable_set_error_if_cancelled (cancellable, error))
    return FALSE;

  dirmeta = ostree_create_directory_metadata (file_info, xattrs);
  
  return ostree_repo_stage_metadata (self, OSTREE_OBJECT_TYPE_DIR_META, NULL,
                                     dirmeta, out_csum, cancellable, error);
}

GFile *
ostree_repo_get_object_path (OstreeRepo       *self,
                             const char       *checksum,
                             OstreeObjectType  type)
{
  char *relpath;
  GFile *ret;
  gboolean compressed;

  compressed = (type == OSTREE_OBJECT_TYPE_FILE
                && ostree_repo_get_mode (self) == OSTREE_REPO_MODE_ARCHIVE_Z2);
  relpath = ostree_get_relative_object_path (checksum, type, compressed);
  ret = g_file_resolve_relative_path (self->repodir, relpath);
  g_free (relpath);
 
  return ret;
}

static GFile *
get_uncompressed_object_cache_path (OstreeRepo       *self,
                                    const char       *checksum)
{
  char *relpath;
  GFile *ret;

  relpath = ostree_get_relative_object_path (checksum, OSTREE_OBJECT_TYPE_FILE, FALSE);
  ret = g_file_resolve_relative_path (self->uncompressed_objects_dir, relpath);
  g_free (relpath);
 
  return ret;
}

/**
 * ostree_repo_stage_content_trusted:
 *
 * Store the content object streamed as @object_input, with total
 * length @length.  The given @checksum will be treated as trusted.
 *
 * This function should be used when importing file objects from local
 * disk, for example.
 */
gboolean      
ostree_repo_stage_content_trusted (OstreeRepo       *self,
                                   const char       *checksum,
                                   GInputStream     *object_input,
                                   guint64           length,
                                   GCancellable     *cancellable,
                                   GError          **error)
{
  return stage_object (self, OSTREE_OBJECT_TYPE_FILE, checksum,
                       object_input, length, NULL,
                       cancellable, error);
}

/**
 * ostree_repo_stage_content:
 *
 * Store the content object streamed as @object_input,
 * with total length @length.  The actual checksum will
 * be returned as @out_csum.
 */
gboolean
ostree_repo_stage_content (OstreeRepo       *self,
                           const char       *expected_checksum,
                           GInputStream     *object_input,
                           guint64           length,
                           guchar          **out_csum,
                           GCancellable     *cancellable,
                           GError          **error)
{
  return stage_object (self, OSTREE_OBJECT_TYPE_FILE, expected_checksum,
                       object_input, length, out_csum,
                       cancellable, error);
}

typedef struct {
  OstreeRepo *repo;
  char *expected_checksum;
  GInputStream *object;
  guint64 file_object_length;
  GCancellable *cancellable;
  GSimpleAsyncResult *result;
  
  guchar *result_csum;
} StageContentAsyncData;

static void
stage_content_async_data_free (gpointer user_data)
{
  StageContentAsyncData *data = user_data;

  g_clear_object (&data->repo);
  g_clear_object (&data->cancellable);
  g_clear_object (&data->object);
  g_free (data->result_csum);
  g_free (data->expected_checksum);
  g_free (data);
}

static void
stage_content_thread (GSimpleAsyncResult  *res,
                      GObject             *object,
                      GCancellable        *cancellable)
{
  GError *error = NULL;
  StageContentAsyncData *data;

  data = g_simple_async_result_get_op_res_gpointer (res);
  if (!ostree_repo_stage_content (data->repo, data->expected_checksum,
                                  data->object, data->file_object_length,
                                  &data->result_csum,
                                  cancellable, &error))
    g_simple_async_result_take_error (res, error);
}

/**
 * ostree_repo_stage_content_async:
 * 
 * Asynchronously store the content object @object.  If provided,
 * the checksum @expected_checksum will be verified.
 */
void          
ostree_repo_stage_content_async (OstreeRepo               *self,
                                 const char               *expected_checksum,
                                 GInputStream             *object,
                                 guint64                   file_object_length,
                                 GCancellable             *cancellable,
                                 GAsyncReadyCallback       callback,
                                 gpointer                  user_data)
{
  StageContentAsyncData *asyncdata;

  asyncdata = g_new0 (StageContentAsyncData, 1);
  asyncdata->repo = g_object_ref (self);
  asyncdata->expected_checksum = g_strdup (expected_checksum);
  asyncdata->object = g_object_ref (object);
  asyncdata->file_object_length = file_object_length;
  asyncdata->cancellable = cancellable ? g_object_ref (cancellable) : NULL;

  asyncdata->result = g_simple_async_result_new ((GObject*) self,
                                                 callback, user_data,
                                                 ostree_repo_stage_content_async);

  g_simple_async_result_set_op_res_gpointer (asyncdata->result, asyncdata,
                                             stage_content_async_data_free);
  g_simple_async_result_run_in_thread (asyncdata->result, stage_content_thread, G_PRIORITY_DEFAULT, cancellable);
  g_object_unref (asyncdata->result);
}

gboolean
ostree_repo_stage_content_finish (OstreeRepo        *self,
                                  GAsyncResult      *result,
                                  guchar           **out_csum,
                                  GError           **error)
{
  GSimpleAsyncResult *simple = G_SIMPLE_ASYNC_RESULT (result);
  StageContentAsyncData *data;

  g_warn_if_fail (g_simple_async_result_get_source_tag (simple) == ostree_repo_stage_content_async);

  if (g_simple_async_result_propagate_error (simple, error))
    return FALSE;

  data = g_simple_async_result_get_op_res_gpointer (simple);
  /* Transfer ownership */
  *out_csum = data->result_csum;
  data->result_csum = NULL;
  return TRUE;
}

static GVariant *
create_empty_gvariant_dict (void)
{
  GVariantBuilder builder;
  g_variant_builder_init (&builder, G_VARIANT_TYPE("a{sv}"));
  return g_variant_builder_end (&builder);
}

static gboolean
enumerate_refs_recurse (OstreeRepo    *repo,
                        GFile         *base,
                        GFile         *dir,
                        GHashTable    *refs,
                        GCancellable  *cancellable,
                        GError       **error)
{
  gboolean ret = FALSE;
  GError *temp_error = NULL;
  ot_lobj GFileInfo *file_info = NULL;
  ot_lobj GFileEnumerator *enumerator = NULL;
  ot_lobj GFile *child = NULL;

  enumerator = g_file_enumerate_children (dir, OSTREE_GIO_FAST_QUERYINFO,
                                          G_FILE_QUERY_INFO_NOFOLLOW_SYMLINKS,
                                          cancellable, error);
  if (!enumerator)
    goto out;

  while ((file_info = g_file_enumerator_next_file (enumerator, cancellable, &temp_error)) != NULL)
    {
      g_clear_object (&child);
      child = g_file_get_child (dir, g_file_info_get_name (file_info));
      if (g_file_info_get_file_type (file_info) == G_FILE_TYPE_DIRECTORY)
        {
          if (!enumerate_refs_recurse (repo, base, child, refs, cancellable, error))
            goto out;
        }
      else if (g_file_info_get_file_type (file_info) == G_FILE_TYPE_REGULAR)
        {
          char *contents;
          gsize len;

          if (!g_file_load_contents (child, cancellable, &contents, &len, NULL, error))
            goto out;

          g_strchomp (contents);

          g_hash_table_insert (refs, g_file_get_relative_path (base, child), contents);
        }

      g_clear_object (&file_info);
    }
  if (temp_error != NULL)
    {
      g_propagate_error (error, temp_error);
      goto out;
    }

  ret = TRUE;
 out:
  return ret;
}

gboolean
ostree_repo_list_all_refs (OstreeRepo       *repo,
                           GHashTable      **out_all_refs,
                           GCancellable     *cancellable,
                           GError          **error)
{
  gboolean ret = FALSE;
  ot_lhash GHashTable *ret_all_refs = NULL;
  ot_lobj GFile *dir = NULL;

  ret_all_refs = g_hash_table_new_full (g_str_hash, g_str_equal, g_free, g_free);

  dir = g_file_resolve_relative_path (ostree_repo_get_path (repo), "refs/heads");
  if (!enumerate_refs_recurse (repo, dir, dir, ret_all_refs, cancellable, error))
    goto out;

  g_clear_object (&dir);
  dir = g_file_resolve_relative_path (ostree_repo_get_path (repo), "refs/remotes");
  if (!enumerate_refs_recurse (repo, dir, dir, ret_all_refs, cancellable, error))
    goto out;

  ret = TRUE;
  ot_transfer_out_value (out_all_refs, &ret_all_refs);
 out:
  return ret;
}

static gboolean
write_ref_summary (OstreeRepo      *self,
                   GCancellable    *cancellable,
                   GError         **error)
{
  gboolean ret = FALSE;
  GHashTableIter hash_iter;
  gpointer key, value;
  gsize bytes_written;
  ot_lhash GHashTable *all_refs = NULL;
  ot_lobj GFile *summary_path = NULL;
  ot_lobj GOutputStream *out = NULL;
  ot_lfree char *buf = NULL;

  if (!ostree_repo_list_all_refs (self, &all_refs, cancellable, error))
    goto out;

  summary_path = g_file_resolve_relative_path (ostree_repo_get_path (self),
                                               "refs/summary");

  out = (GOutputStream*) g_file_replace (summary_path, NULL, FALSE, 0, cancellable, error);
  if (!out)
    goto out;
  
  g_hash_table_iter_init (&hash_iter, all_refs);
  while (g_hash_table_iter_next (&hash_iter, &key, &value))
    {
      const char *name = key;
      const char *sha256 = value;

      g_free (buf);
      buf = g_strdup_printf ("%s %s\n", sha256, name);
      if (!g_output_stream_write_all (out, buf, strlen (buf), &bytes_written, cancellable, error))
        goto out;
    }

  if (!g_output_stream_close (out, cancellable, error))
    goto out;

  ret = TRUE;
 out:
  return ret;
}

gboolean      
ostree_repo_write_ref (OstreeRepo  *self,
                       const char  *remote,
                       const char  *name,
                       const char  *rev,
                       GError     **error)
{
  gboolean ret = FALSE;
  ot_lobj GFile *dir = NULL;

  if (remote == NULL)
    dir = g_object_ref (self->local_heads_dir);
  else
    {
      dir = g_file_get_child (self->remote_heads_dir, remote);

      if (!gs_file_ensure_directory (dir, FALSE, NULL, error))
        goto out;
    }

  if (!write_checksum_file (dir, name, rev, error))
    goto out;

  if (self->mode == OSTREE_REPO_MODE_ARCHIVE
      || self->mode == OSTREE_REPO_MODE_ARCHIVE_Z2)
    {
      if (!write_ref_summary (self, NULL, error))
        goto out;
    }

  ret = TRUE;
 out:
  return ret;
}

gboolean
ostree_repo_stage_commit (OstreeRepo *self,
                          const char   *branch,
                          const char   *parent,
                          const char   *subject,
                          const char   *body,
                          GVariant     *metadata,
                          GVariant     *related_objects,
                          const char   *root_contents_checksum,
                          const char   *root_metadata_checksum,
                          char        **out_commit,
                          GCancellable *cancellable,
                          GError      **error)
{
  gboolean ret = FALSE;
  ot_lfree char *ret_commit = NULL;
  ot_lvariant GVariant *commit = NULL;
  ot_lfree guchar *commit_csum = NULL;
  GDateTime *now = NULL;

  g_return_val_if_fail (branch != NULL, FALSE);
  g_return_val_if_fail (subject != NULL, FALSE);
  g_return_val_if_fail (root_contents_checksum != NULL, FALSE);
  g_return_val_if_fail (root_metadata_checksum != NULL, FALSE);

  now = g_date_time_new_now_utc ();
  commit = g_variant_new ("(@a{sv}@ay@a(say)sst@ay@ay)",
                          metadata ? metadata : create_empty_gvariant_dict (),
                          parent ? ostree_checksum_to_bytes_v (parent) : ot_gvariant_new_bytearray (NULL, 0),
                          related_objects ? related_objects : g_variant_new_array (G_VARIANT_TYPE ("(say)"), NULL, 0),
                          subject, body ? body : "",
                          GUINT64_TO_BE (g_date_time_to_unix (now)),
                          ostree_checksum_to_bytes_v (root_contents_checksum),
                          ostree_checksum_to_bytes_v (root_metadata_checksum));
  g_variant_ref_sink (commit);
  if (!ostree_repo_stage_metadata (self, OSTREE_OBJECT_TYPE_COMMIT, NULL,
                                   commit, &commit_csum,
                                   cancellable, error))
    goto out;

  ret_commit = ostree_checksum_from_bytes (commit_csum);

  ret = TRUE;
  ot_transfer_out_value(out_commit, &ret_commit);
 out:
  if (now)
    g_date_time_unref (now);
  return ret;
}

static GVariant *
create_tree_variant_from_hashes (GHashTable            *file_checksums,
                                 GHashTable            *dir_contents_checksums,
                                 GHashTable            *dir_metadata_checksums)
{
  GHashTableIter hash_iter;
  gpointer key, value;
  GVariantBuilder files_builder;
  GVariantBuilder dirs_builder;
  GSList *sorted_filenames = NULL;
  GSList *iter;
  GVariant *serialized_tree;

  g_variant_builder_init (&files_builder, G_VARIANT_TYPE ("a(say)"));
  g_variant_builder_init (&dirs_builder, G_VARIANT_TYPE ("a(sayay)"));

  g_hash_table_iter_init (&hash_iter, file_checksums);
  while (g_hash_table_iter_next (&hash_iter, &key, &value))
    {
      const char *name = key;
      sorted_filenames = g_slist_prepend (sorted_filenames, (char*)name);
    }

  sorted_filenames = g_slist_sort (sorted_filenames, (GCompareFunc)strcmp);

  for (iter = sorted_filenames; iter; iter = iter->next)
    {
      const char *name = iter->data;
      const char *value;

      value = g_hash_table_lookup (file_checksums, name);
      g_variant_builder_add (&files_builder, "(s@ay)", name,
                             ostree_checksum_to_bytes_v (value));
    }
  
  g_slist_free (sorted_filenames);
  sorted_filenames = NULL;

  g_hash_table_iter_init (&hash_iter, dir_metadata_checksums);
  while (g_hash_table_iter_next (&hash_iter, &key, &value))
    {
      const char *name = key;
      sorted_filenames = g_slist_prepend (sorted_filenames, (char*)name);
    }

  sorted_filenames = g_slist_sort (sorted_filenames, (GCompareFunc)strcmp);

  for (iter = sorted_filenames; iter; iter = iter->next)
    {
      const char *name = iter->data;
      const char *content_checksum;
      const char *meta_checksum;

      content_checksum = g_hash_table_lookup (dir_contents_checksums, name);
      meta_checksum = g_hash_table_lookup (dir_metadata_checksums, name);

      g_variant_builder_add (&dirs_builder, "(s@ay@ay)",
                             name,
                             ostree_checksum_to_bytes_v (content_checksum),
                             ostree_checksum_to_bytes_v (meta_checksum));
    }

  g_slist_free (sorted_filenames);
  sorted_filenames = NULL;

  serialized_tree = g_variant_new ("(@a(say)@a(sayay))",
                                   g_variant_builder_end (&files_builder),
                                   g_variant_builder_end (&dirs_builder));
  g_variant_ref_sink (serialized_tree);

  return serialized_tree;
}

static OstreeRepoCommitFilterResult
apply_commit_filter (OstreeRepo            *self,
                     OstreeRepoCommitModifier *modifier,
                     GPtrArray                *path,
                     GFileInfo                *file_info,
                     GFileInfo               **out_modified_info)
{
  GString *path_buf;
  guint i;
  OstreeRepoCommitFilterResult result;
  GFileInfo *modified_info;
  
  if (modifier == NULL || modifier->filter == NULL)
    {
      *out_modified_info = g_object_ref (file_info);
      return OSTREE_REPO_COMMIT_FILTER_ALLOW;
    }

  path_buf = g_string_new ("");

  if (path->len == 0)
    g_string_append_c (path_buf, '/');
  else
    {
      for (i = 0; i < path->len; i++)
        {
          const char *elt = path->pdata[i];
          
          g_string_append_c (path_buf, '/');
          g_string_append (path_buf, elt);
        }
    }

  modified_info = g_file_info_dup (file_info);
  result = modifier->filter (self, path_buf->str, modified_info, modifier->user_data);
  *out_modified_info = modified_info;

  g_string_free (path_buf, TRUE);
  return result;
}

static gboolean
stage_directory_to_mtree_internal (OstreeRepo           *self,
                                   GFile                *dir,
                                   OstreeMutableTree    *mtree,
                                   OstreeRepoCommitModifier *modifier,
                                   GPtrArray             *path,
                                   GCancellable         *cancellable,
                                   GError              **error)
{
  gboolean ret = FALSE;
  GError *temp_error = NULL;
  gboolean repo_dir_was_empty = FALSE;
  OstreeRepoCommitFilterResult filter_result;
  ot_lobj OstreeRepoFile *repo_dir = NULL;
  ot_lobj GFileEnumerator *dir_enum = NULL;
  ot_lobj GFileInfo *child_info = NULL;

  /* We can only reuse checksums directly if there's no modifier */
  if (OSTREE_IS_REPO_FILE (dir) && modifier == NULL)
    repo_dir = (OstreeRepoFile*)g_object_ref (dir);

  if (repo_dir)
    {
      if (!ostree_repo_file_ensure_resolved (repo_dir, error))
        goto out;

      ostree_mutable_tree_set_metadata_checksum (mtree, ostree_repo_file_get_checksum (repo_dir));
      repo_dir_was_empty = 
        g_hash_table_size (ostree_mutable_tree_get_files (mtree)) == 0
        && g_hash_table_size (ostree_mutable_tree_get_subdirs (mtree)) == 0;

      filter_result = OSTREE_REPO_COMMIT_FILTER_ALLOW;
    }
  else
    {
      ot_lobj GFileInfo *modified_info = NULL;
      ot_lvariant GVariant *xattrs = NULL;
      ot_lfree guchar *child_file_csum = NULL;
      ot_lfree char *tmp_checksum = NULL;

      child_info = g_file_query_info (dir, OSTREE_GIO_FAST_QUERYINFO,
                                      G_FILE_QUERY_INFO_NOFOLLOW_SYMLINKS,
                                      cancellable, error);
      if (!child_info)
        goto out;
      
      filter_result = apply_commit_filter (self, modifier, path, child_info, &modified_info);

      if (filter_result == OSTREE_REPO_COMMIT_FILTER_ALLOW)
        {
          if (!(modifier && modifier->skip_xattrs))
            {
              if (!ostree_get_xattrs_for_file (dir, &xattrs, cancellable, error))
                goto out;
            }
          
          if (!stage_directory_meta (self, modified_info, xattrs, &child_file_csum,
                                     cancellable, error))
            goto out;
          
          g_free (tmp_checksum);
          tmp_checksum = ostree_checksum_from_bytes (child_file_csum);
          ostree_mutable_tree_set_metadata_checksum (mtree, tmp_checksum);
        }

      g_clear_object (&child_info);
    }

  if (filter_result == OSTREE_REPO_COMMIT_FILTER_ALLOW)
    {
      dir_enum = g_file_enumerate_children ((GFile*)dir, OSTREE_GIO_FAST_QUERYINFO, 
                                            G_FILE_QUERY_INFO_NOFOLLOW_SYMLINKS,
                                            cancellable, 
                                            error);
      if (!dir_enum)
        goto out;

      while ((child_info = g_file_enumerator_next_file (dir_enum, cancellable, &temp_error)) != NULL)
        {
          ot_lobj GFileInfo *modified_info = NULL;
          ot_lobj GFile *child = NULL;
          ot_lobj OstreeMutableTree *child_mtree = NULL;
          const char *name = g_file_info_get_name (child_info);

          g_ptr_array_add (path, (char*)name);
          filter_result = apply_commit_filter (self, modifier, path, child_info, &modified_info);

          if (filter_result == OSTREE_REPO_COMMIT_FILTER_ALLOW)
            {
              child = g_file_get_child (dir, name);

              if (g_file_info_get_file_type (child_info) == G_FILE_TYPE_DIRECTORY)
                {
                  if (!ostree_mutable_tree_ensure_dir (mtree, name, &child_mtree, error))
                    goto out;

                  if (!stage_directory_to_mtree_internal (self, child, child_mtree,
                                                          modifier, path, cancellable, error))
                    goto out;
                }
              else if (repo_dir)
                {
                  if (!ostree_mutable_tree_replace_file (mtree, name, 
                                                         ostree_repo_file_get_checksum ((OstreeRepoFile*) child),
                                                         error))
                    goto out;
                }
              else
                {
                  guint64 file_obj_length;
                  const char *loose_checksum;
                  ot_lobj GInputStream *file_input = NULL;
                  ot_lvariant GVariant *xattrs = NULL;
                  ot_lobj GInputStream *file_object_input = NULL;
                  ot_lfree guchar *child_file_csum = NULL;
                  ot_lfree char *tmp_checksum = NULL;

                  loose_checksum = devino_cache_lookup (self, child_info);

                  if (loose_checksum)
                    {
                      if (!ostree_mutable_tree_replace_file (mtree, name, loose_checksum,
                                                             error))
                        goto out;
                    }
                  else
                    {
                     if (g_file_info_get_file_type (modified_info) == G_FILE_TYPE_REGULAR)
                        {
                          file_input = (GInputStream*)g_file_read (child, cancellable, error);
                          if (!file_input)
                            goto out;
                        }

                      if (!(modifier && modifier->skip_xattrs))
                        {
                          g_clear_pointer (&xattrs, (GDestroyNotify) g_variant_unref);
                          if (!ostree_get_xattrs_for_file (child, &xattrs, cancellable, error))
                            goto out;
                        }

                      if (!ostree_raw_file_to_content_stream (file_input,
                                                              modified_info, xattrs,
                                                              &file_object_input, &file_obj_length,
                                                              cancellable, error))
                        goto out;
                      if (!ostree_repo_stage_content (self, NULL, file_object_input, file_obj_length,
                                                      &child_file_csum, cancellable, error))
                        goto out;

                      g_free (tmp_checksum);
                      tmp_checksum = ostree_checksum_from_bytes (child_file_csum);
                      if (!ostree_mutable_tree_replace_file (mtree, name, tmp_checksum,
                                                             error))
                        goto out;
                    }
                }

              g_ptr_array_remove_index (path, path->len - 1);
            }

          g_clear_object (&child_info);
        }
      if (temp_error != NULL)
        {
          g_propagate_error (error, temp_error);
          goto out;
        }
    }

  if (repo_dir && repo_dir_was_empty)
    ostree_mutable_tree_set_contents_checksum (mtree, ostree_repo_file_tree_get_content_checksum (repo_dir));

  ret = TRUE;
 out:
  return ret;
}

gboolean
ostree_repo_stage_directory_to_mtree (OstreeRepo           *self,
                                      GFile                *dir,
                                      OstreeMutableTree    *mtree,
                                      OstreeRepoCommitModifier *modifier,
                                      GCancellable         *cancellable,
                                      GError              **error)
{
  gboolean ret = FALSE;
  GPtrArray *path = NULL;

  path = g_ptr_array_new ();
  if (!stage_directory_to_mtree_internal (self, dir, mtree, modifier, path, cancellable, error))
    goto out;
  
  ret = TRUE;
 out:
  if (path)
    g_ptr_array_free (path, TRUE);
  return ret;
}

gboolean
ostree_repo_stage_mtree (OstreeRepo           *self,
                         OstreeMutableTree    *mtree,
                         char                **out_contents_checksum,
                         GCancellable         *cancellable,
                         GError              **error)
{
  gboolean ret = FALSE;
  GHashTableIter hash_iter;
  gpointer key, value;
  const char *existing_checksum;
  ot_lfree char *ret_contents_checksum = NULL;
  ot_lhash GHashTable *dir_metadata_checksums = NULL;
  ot_lhash GHashTable *dir_contents_checksums = NULL;
  ot_lvariant GVariant *serialized_tree = NULL;
  ot_lfree guchar *contents_csum = NULL;

  existing_checksum = ostree_mutable_tree_get_contents_checksum (mtree);
  if (existing_checksum)
    {
      ret_contents_checksum = g_strdup (existing_checksum);
    }
  else
    {
      dir_contents_checksums = g_hash_table_new_full (g_str_hash, g_str_equal,
                                                      (GDestroyNotify)g_free, (GDestroyNotify)g_free);
      dir_metadata_checksums = g_hash_table_new_full (g_str_hash, g_str_equal,
                                                      (GDestroyNotify)g_free, (GDestroyNotify)g_free);
      
      g_hash_table_iter_init (&hash_iter, ostree_mutable_tree_get_subdirs (mtree));
      while (g_hash_table_iter_next (&hash_iter, &key, &value))
        {
          const char *name = key;
          const char *metadata_checksum;
          OstreeMutableTree *child_dir = value;
          char *child_dir_contents_checksum;

          if (!ostree_repo_stage_mtree (self, child_dir, &child_dir_contents_checksum,
                                        cancellable, error))
            goto out;
      
          g_assert (child_dir_contents_checksum);
          g_hash_table_replace (dir_contents_checksums, g_strdup (name),
                                child_dir_contents_checksum); /* Transfer ownership */
          metadata_checksum = ostree_mutable_tree_get_metadata_checksum (child_dir);
          g_assert (metadata_checksum);
          g_hash_table_replace (dir_metadata_checksums, g_strdup (name),
                                g_strdup (metadata_checksum));
        }
    
      serialized_tree = create_tree_variant_from_hashes (ostree_mutable_tree_get_files (mtree),
                                                         dir_contents_checksums,
                                                         dir_metadata_checksums);
      
      if (!ostree_repo_stage_metadata (self, OSTREE_OBJECT_TYPE_DIR_TREE, NULL,
                                       serialized_tree, &contents_csum,
                                       cancellable, error))
        goto out;
      ret_contents_checksum = ostree_checksum_from_bytes (contents_csum);
    }

  ret = TRUE;
  ot_transfer_out_value(out_contents_checksum, &ret_contents_checksum);
 out:
  return ret;
}

#ifdef HAVE_LIBARCHIVE

static GFileInfo *
create_modified_file_info (GFileInfo               *info,
                           OstreeRepoCommitModifier *modifier)
{
  GFileInfo *ret;

  if (!modifier)
    return (GFileInfo*)g_object_ref (info);

  ret = g_file_info_dup (info);
  
  return ret;
}

static void
propagate_libarchive_error (GError      **error,
                            struct archive *a)
{
  g_set_error (error, G_IO_ERROR, G_IO_ERROR_FAILED,
               "%s", archive_error_string (a));
}

static GFileInfo *
file_info_from_archive_entry_and_modifier (struct archive_entry  *entry,
                                           OstreeRepoCommitModifier *modifier)
{
  GFileInfo *info = g_file_info_new ();
  GFileInfo *modified_info = NULL;
  const struct stat *st;
  guint32 file_type;

  st = archive_entry_stat (entry);

  file_type = ot_gfile_type_for_mode (st->st_mode);
  g_file_info_set_attribute_boolean (info, "standard::is-symlink", S_ISLNK (st->st_mode));
  g_file_info_set_attribute_uint32 (info, "standard::type", file_type);
  g_file_info_set_attribute_uint32 (info, "unix::uid", st->st_uid);
  g_file_info_set_attribute_uint32 (info, "unix::gid", st->st_gid);
  g_file_info_set_attribute_uint32 (info, "unix::mode", st->st_mode);

  if (file_type == G_FILE_TYPE_REGULAR)
    {
      g_file_info_set_attribute_uint64 (info, "standard::size", st->st_size);
    }
  else if (file_type == G_FILE_TYPE_SYMBOLIC_LINK)
    {
      g_file_info_set_attribute_byte_string (info, "standard::symlink-target", archive_entry_symlink (entry));
    }
  else if (file_type == G_FILE_TYPE_SPECIAL)
    {
      g_file_info_set_attribute_uint32 (info, "unix::rdev", st->st_rdev);
    }

  modified_info = create_modified_file_info (info, modifier);

  g_object_unref (info);
  
  return modified_info;
}

static gboolean
import_libarchive_entry_file (OstreeRepo           *self,
                              struct archive       *a,
                              struct archive_entry *entry,
                              GFileInfo            *file_info,
                              guchar              **out_csum,
                              GCancellable         *cancellable,
                              GError              **error)
{
  gboolean ret = FALSE;
  ot_lobj GInputStream *file_object_input = NULL;
  ot_lobj GInputStream *archive_stream = NULL;
  guint64 length;
  
  if (g_cancellable_set_error_if_cancelled (cancellable, error))
    return FALSE;

  if (g_file_info_get_file_type (file_info) == G_FILE_TYPE_REGULAR)
    archive_stream = ostree_libarchive_input_stream_new (a);
  
  if (!ostree_raw_file_to_content_stream (archive_stream, file_info, NULL,
                                          &file_object_input, &length, cancellable, error))
    goto out;
  
  if (!ostree_repo_stage_content (self, NULL, file_object_input, length, out_csum,
                                  cancellable, error))
    goto out;

  ret = TRUE;
 out:
  return ret;
}

static gboolean
stage_libarchive_entry_to_mtree (OstreeRepo           *self,
                                 OstreeMutableTree    *root,
                                 struct archive       *a,
                                 struct archive_entry *entry,
                                 OstreeRepoCommitModifier *modifier,
                                 const guchar         *tmp_dir_csum,
                                 GCancellable         *cancellable,
                                 GError              **error)
{
  gboolean ret = FALSE;
  const char *pathname;
  const char *hardlink;
  const char *basename;
  ot_lobj GFileInfo *file_info = NULL;
  ot_lptrarray GPtrArray *split_path = NULL;
  ot_lptrarray GPtrArray *hardlink_split_path = NULL;
  ot_lobj OstreeMutableTree *subdir = NULL;
  ot_lobj OstreeMutableTree *parent = NULL;
  ot_lobj OstreeMutableTree *hardlink_source_parent = NULL;
  ot_lfree char *hardlink_source_checksum = NULL;
  ot_lobj OstreeMutableTree *hardlink_source_subdir = NULL;
  ot_lfree guchar *tmp_csum = NULL;
  ot_lfree char *tmp_checksum = NULL;

  pathname = archive_entry_pathname (entry); 
      
  if (!ot_util_path_split_validate (pathname, &split_path, error))
    goto out;

  if (split_path->len == 0)
    {
      parent = NULL;
      basename = NULL;
    }
  else
    {
      if (tmp_dir_csum)
        {
          g_free (tmp_checksum);
          tmp_checksum = ostree_checksum_from_bytes (tmp_dir_csum);
          if (!ostree_mutable_tree_ensure_parent_dirs (root, split_path,
                                                       tmp_checksum,
                                                       &parent,
                                                       error))
            goto out;
        }
      else
        {
          if (!ostree_mutable_tree_walk (root, split_path, 0, &parent, error))
            goto out;
        }
      basename = (char*)split_path->pdata[split_path->len-1];
    }

  hardlink = archive_entry_hardlink (entry);
  if (hardlink)
    {
      const char *hardlink_basename;
      
      g_assert (parent != NULL);

      if (!ot_util_path_split_validate (hardlink, &hardlink_split_path, error))
        goto out;
      if (hardlink_split_path->len == 0)
        {
          g_set_error (error, G_IO_ERROR, G_IO_ERROR_FAILED,
                       "Invalid hardlink path %s", hardlink);
          goto out;
        }
      
      hardlink_basename = hardlink_split_path->pdata[hardlink_split_path->len - 1];
      
      if (!ostree_mutable_tree_walk (root, hardlink_split_path, 0, &hardlink_source_parent, error))
        goto out;
      
      if (!ostree_mutable_tree_lookup (hardlink_source_parent, hardlink_basename,
                                       &hardlink_source_checksum,
                                       &hardlink_source_subdir,
                                       error))
        {
              g_prefix_error (error, "While resolving hardlink target: ");
              goto out;
        }
      
      if (hardlink_source_subdir)
        {
          g_set_error (error, G_IO_ERROR, G_IO_ERROR_FAILED,
                       "Hardlink %s refers to directory %s",
                       pathname, hardlink);
          goto out;
        }
      g_assert (hardlink_source_checksum);
      
      if (!ostree_mutable_tree_replace_file (parent,
                                             basename,
                                             hardlink_source_checksum,
                                             error))
        goto out;
    }
  else
    {
      file_info = file_info_from_archive_entry_and_modifier (entry, modifier);

      if (g_file_info_get_file_type (file_info) == G_FILE_TYPE_UNKNOWN)
        {
          g_set_error (error, G_IO_ERROR, G_IO_ERROR_FAILED,
                       "Unsupported file for import: %s", pathname);
          goto out;
        }

      if (g_file_info_get_file_type (file_info) == G_FILE_TYPE_DIRECTORY)
        {

          if (!stage_directory_meta (self, file_info, NULL, &tmp_csum, cancellable, error))
            goto out;

          if (parent == NULL)
            {
              subdir = g_object_ref (root);
            }
          else
            {
              if (!ostree_mutable_tree_ensure_dir (parent, basename, &subdir, error))
                goto out;
            }

          g_free (tmp_checksum);
          tmp_checksum = ostree_checksum_from_bytes (tmp_csum);
          ostree_mutable_tree_set_metadata_checksum (subdir, tmp_checksum);
        }
      else 
        {
          if (parent == NULL)
            {
              g_set_error (error, G_IO_ERROR, G_IO_ERROR_FAILED,
                           "Can't import file as root");
              goto out;
            }

          if (!import_libarchive_entry_file (self, a, entry, file_info, &tmp_csum, cancellable, error))
            goto out;
          
          g_free (tmp_checksum);
          tmp_checksum = ostree_checksum_from_bytes (tmp_csum);
          if (!ostree_mutable_tree_replace_file (parent, basename,
                                                 tmp_checksum,
                                                 error))
            goto out;
        }
    }

  ret = TRUE;
 out:
  return ret;
}
#endif
                          
gboolean
ostree_repo_stage_archive_to_mtree (OstreeRepo                *self,
                                    GFile                     *archive_f,
                                    OstreeMutableTree         *root,
                                    OstreeRepoCommitModifier  *modifier,
                                    gboolean                   autocreate_parents,
                                    GCancellable             *cancellable,
                                    GError                  **error)
{
#ifdef HAVE_LIBARCHIVE
  gboolean ret = FALSE;
  struct archive *a = NULL;
  struct archive_entry *entry;
  int r;
  ot_lobj GFileInfo *tmp_dir_info = NULL;
  ot_lfree guchar *tmp_csum = NULL;

  a = archive_read_new ();
  archive_read_support_compression_all (a);
  archive_read_support_format_all (a);
  if (archive_read_open_filename (a, gs_file_get_path_cached (archive_f), 8192) != ARCHIVE_OK)
    {
      propagate_libarchive_error (error, a);
      goto out;
    }

  while (TRUE)
    {
      r = archive_read_next_header (a, &entry);
      if (r == ARCHIVE_EOF)
        break;
      else if (r != ARCHIVE_OK)
        {
          propagate_libarchive_error (error, a);
          goto out;
        }

      if (autocreate_parents && !tmp_csum)
        {
          tmp_dir_info = g_file_info_new ();
          
          g_file_info_set_attribute_uint32 (tmp_dir_info, "unix::uid", archive_entry_uid (entry));
          g_file_info_set_attribute_uint32 (tmp_dir_info, "unix::gid", archive_entry_gid (entry));
          g_file_info_set_attribute_uint32 (tmp_dir_info, "unix::mode", 0755 | S_IFDIR);
          
          if (!stage_directory_meta (self, tmp_dir_info, NULL, &tmp_csum, cancellable, error))
            goto out;
        }

      if (!stage_libarchive_entry_to_mtree (self, root, a,
                                            entry, modifier,
                                            autocreate_parents ? tmp_csum : NULL,
                                            cancellable, error))
        goto out;
    }
  if (archive_read_close (a) != ARCHIVE_OK)
    {
      propagate_libarchive_error (error, a);
      goto out;
    }

  ret = TRUE;
 out:
  if (a)
    (void)archive_read_close (a);
  return ret;
#else
  g_set_error (error, G_IO_ERROR, G_IO_ERROR_NOT_SUPPORTED,
               "This version of ostree is not compiled with libarchive support");
  return FALSE;
#endif
}

OstreeRepoCommitModifier *
ostree_repo_commit_modifier_new (void)
{
  OstreeRepoCommitModifier *modifier = g_new0 (OstreeRepoCommitModifier, 1);

  modifier->refcount = 1;

  return modifier;
}

void
ostree_repo_commit_modifier_unref (OstreeRepoCommitModifier *modifier)
{
  if (!modifier)
    return;
  if (!g_atomic_int_dec_and_test (&modifier->refcount))
    return;

  g_free (modifier);
  return;
}

static gboolean
list_loose_object_dir (OstreeRepo             *self,
                       GFile                  *dir,
                       GHashTable             *inout_objects,
                       GCancellable           *cancellable,
                       GError                **error)
{
  gboolean ret = FALSE;
  GError *temp_error = NULL;
  const char *dirname = NULL;
  const char *dot = NULL;
  ot_lobj GFileEnumerator *enumerator = NULL;
  ot_lobj GFileInfo *file_info = NULL;
  GString *checksum = NULL;

  dirname = gs_file_get_basename_cached (dir);

  /* We're only querying name */
  enumerator = g_file_enumerate_children (dir, "standard::name,standard::type", 
                                          G_FILE_QUERY_INFO_NOFOLLOW_SYMLINKS,
                                          cancellable, 
                                          error);
  if (!enumerator)
    goto out;
  
  while ((file_info = g_file_enumerator_next_file (enumerator, cancellable, &temp_error)) != NULL)
    {
      const char *name;
      guint32 type;
      OstreeObjectType objtype;

      name = g_file_info_get_attribute_byte_string (file_info, "standard::name"); 
      type = g_file_info_get_attribute_uint32 (file_info, "standard::type");

      if (type == G_FILE_TYPE_DIRECTORY)
        goto loop_next;
      
      if (g_str_has_suffix (name, ".file"))
        objtype = OSTREE_OBJECT_TYPE_FILE;
      else if (g_str_has_suffix (name, ".dirtree"))
        objtype = OSTREE_OBJECT_TYPE_DIR_TREE;
      else if (g_str_has_suffix (name, ".dirmeta"))
        objtype = OSTREE_OBJECT_TYPE_DIR_META;
      else if (g_str_has_suffix (name, ".commit"))
        objtype = OSTREE_OBJECT_TYPE_COMMIT;
      else
        goto loop_next;
          
      dot = strrchr (name, '.');
      g_assert (dot);

      if ((dot - name) == 62)
        {
          GVariant *key, *value;

          if (checksum)
            g_string_free (checksum, TRUE);
          checksum = g_string_new (dirname);
          g_string_append_len (checksum, name, 62);
          
          key = ostree_object_name_serialize (checksum->str, objtype);
          value = g_variant_new ("(b@as)",
                                 TRUE, g_variant_new_strv (NULL, 0));
          /* transfer ownership */
          g_hash_table_replace (inout_objects, key,
                                g_variant_ref_sink (value));
        }
    loop_next:
      g_clear_object (&file_info);
    }
  if (temp_error != NULL)
    {
      g_propagate_error (error, temp_error);
      goto out;
    }
  if (!g_file_enumerator_close (enumerator, NULL, error))
    goto out;

  ret = TRUE;
 out:
  if (checksum)
    g_string_free (checksum, TRUE);
  return ret;
}

static gboolean
list_loose_objects (OstreeRepo                     *self,
                    GHashTable                     *inout_objects,
                    GCancellable                   *cancellable,
                    GError                        **error)
{
  gboolean ret = FALSE;
  guint i;
  ot_lptrarray GPtrArray *object_dirs = NULL;
  ot_lobj GFile *objdir = NULL;

  if (!get_loose_object_dirs (self, &object_dirs, cancellable, error))
    goto out;

  for (i = 0; i < object_dirs->len; i++)
    {
      GFile *objdir = object_dirs->pdata[i];
      if (!list_loose_object_dir (self, objdir, inout_objects, cancellable, error))
        goto out;
    }

  ret = TRUE;
 out:
  return ret;
}

gboolean
ostree_repo_load_file (OstreeRepo         *self,
                       const char         *checksum,
                       GInputStream      **out_input,
                       GFileInfo         **out_file_info,
                       GVariant          **out_xattrs,
                       GCancellable       *cancellable,
                       GError            **error)
{
  gboolean ret = FALSE;
  OstreeRepoMode repo_mode;
  ot_lvariant GVariant *file_data = NULL;
  ot_lobj GFile *loose_path = NULL;
  ot_lobj GFileInfo *content_loose_info = NULL;
  ot_lobj GInputStream *ret_input = NULL;
  ot_lobj GFileInfo *ret_file_info = NULL;
  ot_lvariant GVariant *ret_xattrs = NULL;

  if (!repo_find_object (self, OSTREE_OBJECT_TYPE_FILE, checksum, &loose_path,
                         cancellable, error))
    goto out;

  repo_mode = ostree_repo_get_mode (self);

  if (loose_path)
    {
      switch (repo_mode)
        {
        case OSTREE_REPO_MODE_ARCHIVE:
          {
            ot_lvariant GVariant *archive_meta = NULL;

            if (!ot_util_variant_map (loose_path, OSTREE_FILE_HEADER_GVARIANT_FORMAT,
                                      TRUE, &archive_meta, error))
              goto out;

            if (!ostree_file_header_parse (archive_meta, &ret_file_info, &ret_xattrs,
                                           error))
              goto out;

            if (g_file_info_get_file_type (ret_file_info) == G_FILE_TYPE_REGULAR)
              {
                ot_lobj GFile *archive_content_path = NULL;
                ot_lobj GFileInfo *content_info = NULL;

                archive_content_path = ostree_repo_get_archive_content_path (self, checksum);
                content_info = g_file_query_info (archive_content_path, OSTREE_GIO_FAST_QUERYINFO,
                                                  G_FILE_QUERY_INFO_NOFOLLOW_SYMLINKS,
                                                  cancellable, error);
                if (!content_info)
                  goto out;

                if (out_input)
                  {
                    ret_input = (GInputStream*)gs_file_read_noatime (archive_content_path, cancellable, error);
                    if (!ret_input)
                      goto out;
                  }
                g_file_info_set_size (ret_file_info, g_file_info_get_size (content_info));
              }
          }
          break;
        case OSTREE_REPO_MODE_ARCHIVE_Z2:
          {
            if (!ostree_content_file_parse (TRUE, loose_path, TRUE,
                                            out_input ? &ret_input : NULL,
                                            &ret_file_info, &ret_xattrs,
                                            cancellable, error))
              goto out;
          }
          break;
        case OSTREE_REPO_MODE_BARE:
          {
            ret_file_info = g_file_query_info (loose_path, OSTREE_GIO_FAST_QUERYINFO,
                                               G_FILE_QUERY_INFO_NOFOLLOW_SYMLINKS,
                                               cancellable, error);
            if (!ret_file_info)
              goto out;

            if (out_xattrs)
              {
                if (!ostree_get_xattrs_for_file (loose_path, &ret_xattrs,
                                                 cancellable, error))
                  goto out;
              }

            if (out_input && g_file_info_get_file_type (ret_file_info) == G_FILE_TYPE_REGULAR)
              {
                ret_input = (GInputStream*) gs_file_read_noatime (loose_path, cancellable, error);
                if (!ret_input)
                  {
                    g_prefix_error (error, "Error opening loose file object %s: ", gs_file_get_path_cached (loose_path));
                    goto out;
                  }
              }
          }
          break;
        }
    }
  else if (self->parent_repo)
    {
      if (!ostree_repo_load_file (self->parent_repo, checksum, 
                                  out_input ? &ret_input : NULL,
                                  out_file_info ? &ret_file_info : NULL,
                                  out_xattrs ? &ret_xattrs : NULL,
                                  cancellable, error))
        goto out;
    }
  else
    {
      g_set_error (error, G_IO_ERROR, G_IO_ERROR_NOT_FOUND,
                   "Couldn't find file object '%s'", checksum);
      goto out;
    }

  ret = TRUE;
  ot_transfer_out_value (out_input, &ret_input);
  ot_transfer_out_value (out_file_info, &ret_file_info);
  ot_transfer_out_value (out_xattrs, &ret_xattrs);
 out:
  return ret;
}

static gboolean      
repo_find_object (OstreeRepo           *self,
                  OstreeObjectType      objtype,
                  const char           *checksum,
                  GFile               **out_stored_path,
                  GCancellable         *cancellable,
                  GError             **error)
{
  gboolean ret = FALSE;
  struct stat stbuf;
  ot_lobj GFile *object_path = NULL;
  ot_lobj GFile *ret_stored_path = NULL;

  object_path = ostree_repo_get_object_path (self, checksum, objtype);
  
  if (lstat (gs_file_get_path_cached (object_path), &stbuf) == 0)
    {
      ret_stored_path = object_path;
      object_path = NULL; /* Transfer ownership */
    }
  else if (errno != ENOENT)
    {
      ot_util_set_error_from_errno (error, errno);
      goto out;
    }
      
  ret = TRUE;
  ot_transfer_out_value (out_stored_path, &ret_stored_path);
out:
  return ret;
}

gboolean
ostree_repo_has_object (OstreeRepo           *self,
                        OstreeObjectType      objtype,
                        const char           *checksum,
                        gboolean             *out_have_object,
                        GCancellable         *cancellable,
                        GError              **error)
{
  gboolean ret = FALSE;
  gboolean ret_have_object;
  ot_lobj GFile *loose_path = NULL;

  if (!repo_find_object (self, objtype, checksum, &loose_path,
                         cancellable, error))
    goto out;

  ret_have_object = (loose_path != NULL);

  if (!ret_have_object && self->parent_repo)
    {
      if (!ostree_repo_has_object (self->parent_repo, objtype, checksum,
                                   &ret_have_object, cancellable, error))
        goto out;
    }
                                
  ret = TRUE;
  if (out_have_object)
    *out_have_object = ret_have_object;
 out:
  return ret;
}

gboolean
ostree_repo_load_variant_c (OstreeRepo          *self,
                            OstreeObjectType     objtype,
                            const guchar        *csum, 
                            GVariant           **out_variant,
                            GError             **error)
{
  gboolean ret = FALSE;
  ot_lfree char *checksum = NULL;

  checksum = ostree_checksum_from_bytes (csum);

  if (!ostree_repo_load_variant (self, objtype, checksum, out_variant, error))
    goto out;

  ret = TRUE;
 out:
  return ret;
}

static gboolean
load_variant_internal (OstreeRepo       *self,
                       OstreeObjectType  objtype,
                       const char       *sha256, 
                       gboolean          error_if_not_found,
                       GVariant        **out_variant,
                       GError          **error)
{
  gboolean ret = FALSE;
  GCancellable *cancellable = NULL;
  ot_lobj GFile *object_path = NULL;
  ot_lvariant GVariant *ret_variant = NULL;

  g_return_val_if_fail (OSTREE_OBJECT_TYPE_IS_META (objtype), FALSE);

  if (!repo_find_object (self, objtype, sha256, &object_path,
                         cancellable, error))
    goto out;

  if (object_path != NULL)
    {
      if (!ot_util_variant_map (object_path, ostree_metadata_variant_type (objtype),
                                TRUE, &ret_variant, error))
        goto out;
    }
  else if (self->parent_repo)
    {
      if (!ostree_repo_load_variant (self->parent_repo, objtype, sha256, &ret_variant, error))
        goto out;
    }
  else if (error_if_not_found)
    {
      g_set_error (error, G_IO_ERROR, G_IO_ERROR_FAILED,
                   "No such metadata object %s.%s",
                   sha256, ostree_object_type_to_string (objtype));
      goto out;
    }

  ret = TRUE;
  ot_transfer_out_value (out_variant, &ret_variant);
 out:
  return ret;
}

/**
 * ostree_repo_load_variant_if_exists:
 * 
 * Attempt to load the metadata object @sha256 of type @objtype if it
 * exists, storing the result in @out_variant.  If it doesn't exist,
 * %NULL is returned.
 */
gboolean
ostree_repo_load_variant_if_exists (OstreeRepo       *self,
                                    OstreeObjectType  objtype,
                                    const char       *sha256, 
                                    GVariant        **out_variant,
                                    GError          **error)
{
  return load_variant_internal (self, objtype, sha256, FALSE,
                                out_variant, error);
}

/**
 * ostree_repo_load_variant:
 * 
 * Load the metadata object @sha256 of type @objtype, storing the
 * result in @out_variant.
 */
gboolean
ostree_repo_load_variant (OstreeRepo       *self,
                          OstreeObjectType  objtype,
                          const char       *sha256, 
                          GVariant        **out_variant,
                          GError          **error)
{
  return load_variant_internal (self, objtype, sha256, TRUE,
                                out_variant, error);
}


/**
 * ostree_repo_list_objects:
 * @self:
 * @flags:
 * @out_objects: (out): Map of serialized object name to variant data
 * @cancellable:
 * @error:
 *
 * This function synchronously enumerates all objects in the
 * repository, returning data in @out_objects.  @out_objects
 * maps from keys returned by ostree_object_name_serialize()
 * to #GVariant values of type %OSTREE_REPO_LIST_OBJECTS_VARIANT_TYPE.
 *
 * Returns: %TRUE on success, %FALSE on error, and @error will be set
 */ 
gboolean
ostree_repo_list_objects (OstreeRepo                  *self,
                          OstreeRepoListObjectsFlags   flags,
                          GHashTable                 **out_objects,
                          GCancellable                *cancellable,
                          GError                     **error)
{
  gboolean ret = FALSE;
  ot_lhash GHashTable *ret_objects = NULL;

  g_return_val_if_fail (error == NULL || *error == NULL, FALSE);
  g_return_val_if_fail (self->inited, FALSE);
  
  ret_objects = g_hash_table_new_full (ostree_hash_object_name, g_variant_equal,
                                       (GDestroyNotify) g_variant_unref,
                                       (GDestroyNotify) g_variant_unref);

  if (flags & OSTREE_REPO_LIST_OBJECTS_ALL)
    flags |= (OSTREE_REPO_LIST_OBJECTS_LOOSE | OSTREE_REPO_LIST_OBJECTS_PACKED);

  if (flags & OSTREE_REPO_LIST_OBJECTS_LOOSE)
    {
      if (!list_loose_objects (self, ret_objects, cancellable, error))
        goto out;
      if (self->parent_repo)
        {
          if (!list_loose_objects (self->parent_repo, ret_objects, cancellable, error))
            goto out;
        }
    }

  if (flags & OSTREE_REPO_LIST_OBJECTS_PACKED)
    {
      /* Nothing for now... */
    }

  ret = TRUE;
  ot_transfer_out_value (out_objects, &ret_objects);
 out:
  return ret;
}

static gboolean
checkout_file_from_input (GFile          *file,
                          OstreeRepoCheckoutMode mode,
                          OstreeRepoCheckoutOverwriteMode    overwrite_mode,
                          GFileInfo      *finfo,
                          GVariant       *xattrs,
                          GInputStream   *input,
                          GCancellable   *cancellable,
                          GError        **error)
{
  gboolean ret = FALSE;
  GError *temp_error = NULL;
  ot_lobj GFile *dir = NULL;
  ot_lobj GFile *temp_file = NULL;
  ot_lobj GFileInfo *temp_info = NULL;

  if (mode == OSTREE_REPO_CHECKOUT_MODE_USER)
    {
      temp_info = g_file_info_dup (finfo);
      
      g_file_info_set_attribute_uint32 (temp_info, "unix::uid", geteuid ());
      g_file_info_set_attribute_uint32 (temp_info, "unix::gid", getegid ());

      xattrs = NULL;
    }
  else
    temp_info = g_object_ref (finfo);

  if (overwrite_mode == OSTREE_REPO_CHECKOUT_OVERWRITE_UNION_FILES)
    {
      if (g_file_info_get_file_type (temp_info) == G_FILE_TYPE_DIRECTORY)
        {
          if (!ostree_create_file_from_input (file, temp_info,
                                              xattrs, input,
                                              cancellable, &temp_error))
            {
              if (g_error_matches (temp_error, G_IO_ERROR, G_IO_ERROR_EXISTS))
                {
                  g_clear_error (&temp_error);
                }
              else
                {
                  g_propagate_error (error, temp_error);
                  goto out;
                }
            }
        }
      else
        {
          dir = g_file_get_parent (file);
          if (!ostree_create_temp_file_from_input (dir, NULL, "checkout",
                                                   temp_info, xattrs, input, &temp_file, 
                                                   cancellable, error))
            goto out;

          if (g_file_info_get_file_type (temp_info) == G_FILE_TYPE_REGULAR)
            {
              if (!ensure_file_data_synced (temp_file, cancellable, error))
                goto out;
            }

          if (rename (gs_file_get_path_cached (temp_file), gs_file_get_path_cached (file)) < 0)
            {
              ot_util_set_error_from_errno (error, errno);
              goto out;
            }
        }
    }
  else
    {
      if (!ostree_create_file_from_input (file, temp_info,
                                          xattrs, input, cancellable, error))
        goto out;

      if (g_file_info_get_file_type (temp_info) == G_FILE_TYPE_REGULAR)
        {
          if (!ensure_file_data_synced (file, cancellable, error))
            goto out;
        }
    }

  ret = TRUE;
 out:
  return ret;
}

static gboolean
checkout_file_hardlink (OstreeRepo                  *self,
                        OstreeRepoCheckoutMode    mode,
                        OstreeRepoCheckoutOverwriteMode    overwrite_mode,
                        GFile                    *source,
                        GFile                    *destination,
                        int                       dirfd,
                        gboolean                 *out_was_supported,
                        GCancellable             *cancellable,
                        GError                  **error)
{
  gboolean ret = FALSE;
  gboolean ret_was_supported = FALSE;
  ot_lobj GFile *dir = NULL;

  if (dirfd != -1 &&
      linkat (-1, gs_file_get_path_cached (source),
              dirfd, gs_file_get_basename_cached (destination), 0) != -1)
    ret_was_supported = TRUE;
  else if (link (gs_file_get_path_cached (source), gs_file_get_path_cached (destination)) != -1)
    ret_was_supported = TRUE;
  else if (errno == EMLINK || errno == EXDEV || errno == EPERM)
    {
      /* EMLINK, EXDEV and EPERM shouldn't be fatal; we just can't do the
       * optimization of hardlinking instead of copying.
       */
      ret_was_supported = FALSE;
    }
  else if (errno == EEXIST && overwrite_mode == OSTREE_REPO_CHECKOUT_OVERWRITE_UNION_FILES)
    { 
      /* Idiocy, from man rename(2)
       *
       * "If oldpath and newpath are existing hard links referring to
       * the same file, then rename() does nothing, and returns a
       * success status."
       *
       * So we can't make this atomic.  
       */
      (void) unlink (gs_file_get_path_cached (destination));
      if (link (gs_file_get_path_cached (source), gs_file_get_path_cached (destination)) < 0)
        {
          ot_util_set_error_from_errno (error, errno);
          goto out;
        }
      ret_was_supported = TRUE;
    }
  else
    {
      ot_util_set_error_from_errno (error, errno);
      goto out;
    }

  ret = TRUE;
  if (out_was_supported)
    *out_was_supported = ret_was_supported;
 out:
  return ret;
}

static gboolean
find_loose_for_checkout (OstreeRepo             *self,
                         const char             *checksum,
                         GFile                 **out_loose_path,
                         GCancellable           *cancellable,
                         GError                **error)
{
  gboolean ret = FALSE;
  ot_lobj GFile *path = NULL;
  struct stat stbuf;

  do
    {
      switch (self->mode)
        {
        case OSTREE_REPO_MODE_BARE:
          path = ostree_repo_get_object_path (self, checksum, OSTREE_OBJECT_TYPE_FILE);
          break;
        case OSTREE_REPO_MODE_ARCHIVE:
          path = ostree_repo_get_archive_content_path (self, checksum);
          break;
        case OSTREE_REPO_MODE_ARCHIVE_Z2:
          {
            if (self->enable_uncompressed_cache)
              path = get_uncompressed_object_cache_path (self, checksum);
            else
              path = NULL;
          }
          break;
        }

      if (!path)
        {
          self = self->parent_repo;
          continue;
        }

      if (lstat (gs_file_get_path_cached (path), &stbuf) < 0)
        {
          if (errno != ENOENT)
            {
              ot_util_set_error_from_errno (error, errno);
              goto out;
            }
          self = self->parent_repo;
        }
      else if (S_ISLNK (stbuf.st_mode))
        {
          /* Don't check out symbolic links via hardlink; it's very easy
           * to hit the maximum number of hardlinks to an inode this way,
           * especially since right now we have a lot of symbolic links to
           * busybox.
           *
           * fs/ext4/ext4.h:#define EXT4_LINK_MAX		65000
           */
          self = self->parent_repo;
        }
      else
        break;

      g_clear_object (&path);
    } while (self != NULL);

  ret = TRUE;
  ot_transfer_out_value (out_loose_path, &path);
 out:
  return ret;
}

typedef struct {
  OstreeRepo               *repo;
  OstreeRepoCheckoutMode    mode;
  OstreeRepoCheckoutOverwriteMode    overwrite_mode;
  GFile                    *destination;
  int                       dirfd;
  OstreeRepoFile           *source;
  GFileInfo                *source_info;
  GCancellable             *cancellable;

  gboolean                  caught_error;
  GError                   *error;

  GSimpleAsyncResult       *result;
} CheckoutOneFileAsyncData;

static void
checkout_file_async_data_free (gpointer      data)
{
  CheckoutOneFileAsyncData *checkout_data = data;

  g_clear_object (&checkout_data->repo);
  g_clear_object (&checkout_data->destination);
  g_clear_object (&checkout_data->source);
  g_clear_object (&checkout_data->source_info);
  g_clear_object (&checkout_data->cancellable);
  g_free (checkout_data);
}

static void
checkout_file_thread (GSimpleAsyncResult     *result,
                      GObject                *src,
                      GCancellable           *cancellable)
{
  const char *checksum;
  OstreeRepo *repo;
  gboolean is_symlink;
  gboolean hardlink_supported;
  GError *local_error = NULL;
  GError **error = &local_error;
  ot_lobj GFile *loose_path = NULL;
  ot_lobj GInputStream *input = NULL;
  ot_lvariant GVariant *xattrs = NULL;
  CheckoutOneFileAsyncData *checkout_data;

  checkout_data = g_simple_async_result_get_op_res_gpointer (result);
  repo = checkout_data->repo;

  /* Hack to avoid trying to create device files as a user */
  if (checkout_data->mode == OSTREE_REPO_CHECKOUT_MODE_USER
      && g_file_info_get_file_type (checkout_data->source_info) == G_FILE_TYPE_SPECIAL)
    goto out;

  is_symlink = g_file_info_get_file_type (checkout_data->source_info) == G_FILE_TYPE_SYMBOLIC_LINK;

  checksum = ostree_repo_file_get_checksum ((OstreeRepoFile*)checkout_data->source);

  /* We can only do hardlinks in these scenarios */
  if (!is_symlink &&
      ((checkout_data->repo->mode == OSTREE_REPO_MODE_BARE && checkout_data->mode == OSTREE_REPO_CHECKOUT_MODE_NONE)
       || (checkout_data->repo->mode == OSTREE_REPO_MODE_ARCHIVE && checkout_data->mode == OSTREE_REPO_CHECKOUT_MODE_USER)
       || (checkout_data->repo->mode == OSTREE_REPO_MODE_ARCHIVE_Z2 && checkout_data->mode == OSTREE_REPO_CHECKOUT_MODE_USER)))
    {
      if (!find_loose_for_checkout (checkout_data->repo, checksum, &loose_path,
                                    cancellable, error))
        goto out;
    }
  /* Also, if we're archive-z and we didn't find an object, uncompress it now,
   * stick it in the cache, and then hardlink to that.
   */
  if (!is_symlink
      && loose_path == NULL
      && repo->mode == OSTREE_REPO_MODE_ARCHIVE_Z2
      && checkout_data->mode == OSTREE_REPO_CHECKOUT_MODE_USER
      && repo->enable_uncompressed_cache)
    {
      ot_lobj GFile *objdir = NULL;

      loose_path = get_uncompressed_object_cache_path (repo, checksum);
      if (!ostree_repo_load_file (repo, checksum, &input, NULL, &xattrs,
                                  cancellable, error))
        goto out;

      objdir = g_file_get_parent (loose_path);
      if (!gs_file_ensure_directory (objdir, TRUE, cancellable, error))
        {
          g_prefix_error (error, "Creating cache directory %s: ",
                          gs_file_get_path_cached (objdir));
          goto out;
        }

      /* Use UNION_FILES to make this last-one-wins thread behavior
       * for now; we lose deduplication potentially, but oh well
       */ 
      if (!checkout_file_from_input (loose_path,
                                     OSTREE_REPO_CHECKOUT_MODE_USER,
                                     OSTREE_REPO_CHECKOUT_OVERWRITE_UNION_FILES,
                                     checkout_data->source_info, xattrs, 
                                     input, cancellable, error))
        {
          g_prefix_error (error, "Unpacking loose object %s: ", checksum);
          goto out;
        }

      /* Store the 2-byte objdir prefix (e.g. e3) in a set.  The basic
       * idea here is that if we had to unpack an object, it's very
       * likely we're replacing some other object, so we may need a GC.
       *
       * This model ensures that we do work roughly proportional to
       * the size of the changes.  For example, we don't scan any
       * directories if we didn't modify anything, meaning you can
       * checkout the same tree multiple times very quickly.
       *
       * This is also scale independent; we don't hardcode e.g. looking
       * at 1000 objects.
       *
       * The downside is that if we're unlucky, we may not free
       * an object for quite some time.
       */
      g_mutex_lock (&repo->cache_lock);
      {
        gpointer key = GUINT_TO_POINTER ((g_ascii_xdigit_value (checksum[0]) << 4) + 
                                         g_ascii_xdigit_value (checksum[1]));
        if (repo->updated_uncompressed_dirs == NULL)
          repo->updated_uncompressed_dirs = g_hash_table_new (NULL, NULL);
        g_hash_table_insert (repo->updated_uncompressed_dirs, key, key);
      }
      g_mutex_unlock (&repo->cache_lock);
    }

  if (loose_path)
    {
      /* If we found one, try hardlinking */
      if (!checkout_file_hardlink (checkout_data->repo, checkout_data->mode,
                                   checkout_data->overwrite_mode, loose_path,
                                   checkout_data->destination, checkout_data->dirfd,
                                   &hardlink_supported, cancellable, error))
        {
          g_prefix_error (error, "Hardlinking loose object %s to %s: ", checksum,
                          gs_file_get_path_cached (checkout_data->destination));
          goto out;
        }
    }

  /* Fall back to copy if there's no loose object, or we couldn't hardlink */
  if (loose_path == NULL || !hardlink_supported)
    {
      if (!ostree_repo_load_file (checkout_data->repo, checksum, &input, NULL, &xattrs,
                                  cancellable, error))
        goto out;

      if (!checkout_file_from_input (checkout_data->destination,
                                     checkout_data->mode,
                                     checkout_data->overwrite_mode,
                                     checkout_data->source_info, xattrs, 
                                     input, cancellable, error))
        {
          g_prefix_error (error, "Copying object %s to %s: ", checksum,
                          gs_file_get_path_cached (checkout_data->destination));
          goto out;
        }
    }

 out:
  if (local_error)
    g_simple_async_result_take_error (result, local_error);
}

static void
checkout_one_file_async (OstreeRepo                  *self,
                         OstreeRepoCheckoutMode    mode,
                         OstreeRepoCheckoutOverwriteMode    overwrite_mode,
                         OstreeRepoFile           *source,
                         GFileInfo                *source_info,
                         GFile                    *destination,
                         int                       dirfd,
                         GCancellable             *cancellable,
                         GAsyncReadyCallback       callback,
                         gpointer                  user_data)
{
  CheckoutOneFileAsyncData *checkout_data;

  checkout_data = g_new0 (CheckoutOneFileAsyncData, 1);
  checkout_data->repo = g_object_ref (self);
  checkout_data->mode = mode;
  checkout_data->overwrite_mode = overwrite_mode;
  checkout_data->destination = g_object_ref (destination);
  checkout_data->dirfd = dirfd;
  checkout_data->source = g_object_ref (source);
  checkout_data->source_info = g_object_ref (source_info);
  checkout_data->cancellable = cancellable ? g_object_ref (cancellable) : NULL;

  checkout_data->result = g_simple_async_result_new ((GObject*) self,
                                                     callback, user_data,
                                                     checkout_one_file_async);

  g_simple_async_result_set_op_res_gpointer (checkout_data->result, checkout_data,
                                             checkout_file_async_data_free);

  g_simple_async_result_run_in_thread (checkout_data->result,
                                       checkout_file_thread, G_PRIORITY_DEFAULT,
                                       cancellable);
  g_object_unref (checkout_data->result);
}

static gboolean
checkout_one_file_finish (OstreeRepo               *self,
                          GAsyncResult             *result,
                          GError                  **error)
{
  GSimpleAsyncResult *simple;

  g_return_val_if_fail (g_simple_async_result_is_valid (result, (GObject*)self, checkout_one_file_async), FALSE);

  simple = G_SIMPLE_ASYNC_RESULT (result);
  if (g_simple_async_result_propagate_error (simple, error))
    return FALSE;
  return TRUE;
}

typedef struct {
  OstreeRepo               *repo;
  OstreeRepoCheckoutMode    mode;
  OstreeRepoCheckoutOverwriteMode    overwrite_mode;
  GFile                    *destination;
  OstreeRepoFile           *source;
  GFileInfo                *source_info;
  GCancellable             *cancellable;

  gboolean                  caught_error;
  GError                   *error;

  DIR                      *dir_handle;

  gboolean                  dir_enumeration_complete;
  guint                     pending_ops;
  guint                     pending_file_ops;
  GPtrArray                *pending_dirs;
  GMainLoop                *loop;
  GSimpleAsyncResult       *result;
} CheckoutTreeAsyncData;

static void
checkout_tree_async_data_free (gpointer      data)
{
  CheckoutTreeAsyncData *checkout_data = data;

  g_clear_object (&checkout_data->repo);
  g_clear_object (&checkout_data->destination);
  g_clear_object (&checkout_data->source);
  g_clear_object (&checkout_data->source_info);
  g_clear_object (&checkout_data->cancellable);
  if (checkout_data->pending_dirs)
    g_ptr_array_unref (checkout_data->pending_dirs);
  if (checkout_data->dir_handle)
    (void) closedir (checkout_data->dir_handle);
  g_free (checkout_data);
}

static void
on_tree_async_child_op_complete (CheckoutTreeAsyncData   *data,
                                 GError                  *local_error)
{
  data->pending_ops--;

  if (local_error)
    {
      if (!data->caught_error)
        {
          data->caught_error = TRUE;
          g_propagate_error (&data->error, local_error);
        }
      else
        g_clear_error (&local_error);
    }

  if (data->pending_ops != 0)
    return;

  if (data->caught_error)
    g_simple_async_result_take_error (data->result, data->error);
  g_simple_async_result_complete_in_idle (data->result);
  g_object_unref (data->result);
}

static void
on_one_subdir_checked_out (GObject          *src,
                           GAsyncResult     *result,
                           gpointer          user_data)
{
  CheckoutTreeAsyncData *data = user_data;
  GError *local_error = NULL;

  if (!ostree_repo_checkout_tree_finish ((OstreeRepo*) src, result, &local_error))
    goto out;

 out:
  on_tree_async_child_op_complete (data, local_error);
}

static void
process_pending_dirs (CheckoutTreeAsyncData *data)
{
  guint i;

  g_assert (data->dir_enumeration_complete);
  g_assert (data->pending_file_ops == 0);

  /* Don't hold a FD open while we're processing
   * recursive calls, otherwise we can pretty easily
   * hit the max of 1024 fds =(
   */
  if (data->dir_handle)
    {
      (void) closedir (data->dir_handle);
      data->dir_handle = NULL;
    }

  if (data->pending_dirs != NULL)
    {
      for (i = 0; i < data->pending_dirs->len; i++)
        {
          GFileInfo *file_info = data->pending_dirs->pdata[i];
          const char *name;
          ot_lobj GFile *dest_path = NULL;
          ot_lobj GFile *src_child = NULL;

          name = g_file_info_get_attribute_byte_string (file_info, "standard::name"); 

          dest_path = g_file_get_child (data->destination, name);
          src_child = g_file_get_child ((GFile*)data->source, name);

          ostree_repo_checkout_tree_async (data->repo,
                                           data->mode,
                                           data->overwrite_mode,
                                           dest_path, (OstreeRepoFile*)src_child, file_info,
                                           data->cancellable,
                                           on_one_subdir_checked_out,
                                           data);
          data->pending_ops++;
        }
      g_ptr_array_set_size (data->pending_dirs, 0);
      on_tree_async_child_op_complete (data, NULL);
    }
}

static void
on_one_file_checked_out (GObject          *src,
                         GAsyncResult     *result,
                         gpointer          user_data)
{
  CheckoutTreeAsyncData *data = user_data;
  GError *local_error = NULL;

  if (!checkout_one_file_finish ((OstreeRepo*) src, result, &local_error))
    goto out;

 out:
  data->pending_file_ops--;
  if (data->dir_enumeration_complete && data->pending_file_ops == 0)
    process_pending_dirs (data);
  on_tree_async_child_op_complete (data, local_error);
}

static void
on_got_next_files (GObject          *src,
                   GAsyncResult     *result,
                   gpointer          user_data)
{
  CheckoutTreeAsyncData *data = user_data;
  GError *local_error = NULL;
  GList *files = NULL;
  GList *iter = NULL;

  files = g_file_enumerator_next_files_finish ((GFileEnumerator*) src, result, &local_error);
  if (local_error)
    goto out;

  if (!files)
    data->dir_enumeration_complete = TRUE;
  else
    {
      g_file_enumerator_next_files_async ((GFileEnumerator*)src, 50, G_PRIORITY_DEFAULT,
                                          data->cancellable,
                                          on_got_next_files, data);
      data->pending_ops++;
    }

  if (data->dir_enumeration_complete && data->pending_file_ops == 0)
    process_pending_dirs (data);

  for (iter = files; iter; iter = iter->next)
    {
      GFileInfo *file_info = iter->data;
      const char *name;
      guint32 type;

      name = g_file_info_get_attribute_byte_string (file_info, "standard::name"); 
      type = g_file_info_get_attribute_uint32 (file_info, "standard::type");

      if (type != G_FILE_TYPE_DIRECTORY)
        {
          ot_lobj GFile *dest_path = NULL;
          ot_lobj GFile *src_child = NULL;

          dest_path = g_file_get_child (data->destination, name);
          src_child = g_file_get_child ((GFile*)data->source, name);

          checkout_one_file_async (data->repo, data->mode,
                                   data->overwrite_mode,
                                   (OstreeRepoFile*)src_child, file_info, 
                                   dest_path, dirfd(data->dir_handle),
                                   data->cancellable, on_one_file_checked_out,
                                   data);
          data->pending_file_ops++;
          data->pending_ops++;
        }
      else
        {
          if (data->pending_dirs == NULL)
            {
              data->pending_dirs = g_ptr_array_new_with_free_func ((GDestroyNotify) g_object_unref);
              data->pending_ops++;
            }
          g_ptr_array_add (data->pending_dirs, g_object_ref (file_info));
        }
      g_object_unref (file_info);
    }

  g_list_free (files);

 out:
  on_tree_async_child_op_complete (data, local_error);
}

void
ostree_repo_checkout_tree_async (OstreeRepo               *self,
                                 OstreeRepoCheckoutMode    mode,
                                 OstreeRepoCheckoutOverwriteMode    overwrite_mode,
                                 GFile                    *destination,
                                 OstreeRepoFile           *source,
                                 GFileInfo                *source_info,
                                 GCancellable             *cancellable,
                                 GAsyncReadyCallback       callback,
                                 gpointer                  user_data)
{
  CheckoutTreeAsyncData *checkout_data;
  ot_lobj GFileInfo *file_info = NULL;
  ot_lvariant GVariant *xattrs = NULL;
  ot_lobj GFileEnumerator *dir_enum = NULL;
  GError *local_error = NULL;
  GError **error = &local_error;

  checkout_data = g_new0 (CheckoutTreeAsyncData, 1);
  checkout_data->repo = g_object_ref (self);
  checkout_data->mode = mode;
  checkout_data->overwrite_mode = overwrite_mode;
  checkout_data->destination = g_object_ref (destination);
  checkout_data->source = g_object_ref (source);
  checkout_data->source_info = g_object_ref (source_info);
  checkout_data->cancellable = cancellable ? g_object_ref (cancellable) : NULL;
  checkout_data->pending_ops++; /* Count this function */

  checkout_data->result = g_simple_async_result_new ((GObject*) self,
                                                     callback, user_data,
                                                     ostree_repo_checkout_tree_async);

  g_simple_async_result_set_op_res_gpointer (checkout_data->result, checkout_data,
                                             checkout_tree_async_data_free);

  if (!ostree_repo_file_get_xattrs (checkout_data->source, &xattrs, NULL, error))
    goto out;

  if (!checkout_file_from_input (checkout_data->destination,
                                 checkout_data->mode,
                                 checkout_data->overwrite_mode,
                                 checkout_data->source_info,
                                 xattrs, NULL,
                                 cancellable, error))
    goto out;

  checkout_data->dir_handle = opendir (gs_file_get_path_cached (checkout_data->destination));
  if (!checkout_data->dir_handle)
    {
      ot_util_set_error_from_errno (error, errno);
      goto out;
    }

  g_clear_pointer (&xattrs, (GDestroyNotify) g_variant_unref);

  dir_enum = g_file_enumerate_children ((GFile*)checkout_data->source,
                                        OSTREE_GIO_FAST_QUERYINFO, 
                                        G_FILE_QUERY_INFO_NOFOLLOW_SYMLINKS,
                                        cancellable, 
                                        error);
  if (!dir_enum)
    goto out;

  g_file_enumerator_next_files_async (dir_enum, 50, G_PRIORITY_DEFAULT, cancellable,
                                      on_got_next_files, checkout_data);
  checkout_data->pending_ops++;

 out:
  on_tree_async_child_op_complete (checkout_data, local_error);
}

gboolean
ostree_repo_checkout_tree_finish (OstreeRepo               *self,
                                  GAsyncResult             *result,
                                  GError                  **error)
{
  GSimpleAsyncResult *simple;

  g_return_val_if_fail (g_simple_async_result_is_valid (result, (GObject*)self, ostree_repo_checkout_tree_async), FALSE);

  simple = G_SIMPLE_ASYNC_RESULT (result);
  if (g_simple_async_result_propagate_error (simple, error))
    return FALSE;
  return TRUE;
}

/**
 * ostree_repo_checkout_gc:
 *
 * Call this after finishing a succession of checkout operations; it
 * will delete any currently-unused uncompressed objects from the
 * cache.
 */
gboolean
ostree_repo_checkout_gc (OstreeRepo        *self,
                         GCancellable      *cancellable,
                         GError           **error)
{
  gboolean ret = FALSE;
  ot_lhash GHashTable *to_clean_dirs = NULL;
  GHashTableIter iter;
  gpointer key, value;

  g_mutex_lock (&self->cache_lock);
  to_clean_dirs = self->updated_uncompressed_dirs;
  self->updated_uncompressed_dirs = g_hash_table_new (NULL, NULL);
  g_mutex_unlock (&self->cache_lock);

  if (to_clean_dirs)
    g_hash_table_iter_init (&iter, to_clean_dirs);
  while (to_clean_dirs && g_hash_table_iter_next (&iter, &key, &value))
    {
      GError *temp_error = NULL;
      ot_lobj GFile *objdir = NULL;
      ot_lobj GFileInfo *file_info = NULL;
      ot_lobj GFileEnumerator *enumerator = NULL;
      ot_lfree char *objdir_name = NULL;

      objdir_name = g_strdup_printf ("%02x", GPOINTER_TO_UINT (key));
      objdir = ot_gfile_get_child_build_path (self->uncompressed_objects_dir, "objects",
                                              objdir_name, NULL);

      enumerator = g_file_enumerate_children (objdir, "standard::name,standard::type,unix::inode,unix::nlink", 
                                              G_FILE_QUERY_INFO_NOFOLLOW_SYMLINKS,
                                              cancellable, 
                                              error);
      if (!enumerator)
        goto out;
  
      while ((file_info = g_file_enumerator_next_file (enumerator, cancellable, &temp_error)) != NULL)
        {
          guint32 nlinks = g_file_info_get_attribute_uint32 (file_info, "unix::nlink");
          if (nlinks == 1)
            {
              ot_lobj GFile *objpath = NULL;
              objpath = ot_gfile_get_child_build_path (objdir, g_file_info_get_name (file_info), NULL);
              if (!gs_file_unlink (objpath, cancellable, error))
                goto out;
            }
          g_object_unref (file_info);
        }
      if (temp_error != NULL)
        {
          g_propagate_error (error, temp_error);
          goto out;
        }
    }

  ret = TRUE;
 out:
  return ret;
}

gboolean
ostree_repo_read_commit (OstreeRepo *self,
                         const char *rev, 
                         GFile       **out_root,
                         GCancellable *cancellable,
                         GError **error)
{
  gboolean ret = FALSE;
  ot_lobj GFile *ret_root = NULL;
  ot_lfree char *resolved_rev = NULL;

  if (!ostree_repo_resolve_rev (self, rev, FALSE, &resolved_rev, error))
    goto out;

  ret_root = ostree_repo_file_new_root (self, resolved_rev);
  if (!ostree_repo_file_ensure_resolved ((OstreeRepoFile*)ret_root, error))
    goto out;

  ret = TRUE;
  ot_transfer_out_value(out_root, &ret_root);
 out:
  return ret;
}
