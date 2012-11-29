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

static gboolean
get_file_checksum (GFile  *f,
                   GFileInfo *f_info,
                   char  **out_checksum,
                   GCancellable *cancellable,
                   GError   **error)
{
  gboolean ret = FALSE;
  ot_lfree char *ret_checksum = NULL;
  ot_lfree guchar *csum = NULL;

  if (OSTREE_IS_REPO_FILE (f))
    {
      ret_checksum = g_strdup (ostree_repo_file_get_checksum ((OstreeRepoFile*)f));
    }
  else
    {
      if (!ostree_checksum_file (f, OSTREE_OBJECT_TYPE_FILE,
                                 &csum, cancellable, error))
        goto out;
      ret_checksum = ostree_checksum_from_bytes (csum);
    }

  ret = TRUE;
  ot_transfer_out_value(out_checksum, &ret_checksum);
 out:
  return ret;
}

OstreeDiffItem *
ostree_diff_item_ref (OstreeDiffItem *diffitem)
{
  g_atomic_int_inc (&diffitem->refcount);
  return diffitem;
}

void
ostree_diff_item_unref (OstreeDiffItem *diffitem)
{
  if (!g_atomic_int_dec_and_test (&diffitem->refcount))
    return;

  g_clear_object (&diffitem->src);
  g_clear_object (&diffitem->target);
  g_clear_object (&diffitem->src_info);
  g_clear_object (&diffitem->target_info);
  g_free (diffitem->src_checksum);
  g_free (diffitem->target_checksum);
  g_free (diffitem);
}

static OstreeDiffItem *
diff_item_new (GFile          *a,
               GFileInfo      *a_info,
               GFile          *b,
               GFileInfo      *b_info,
               char           *checksum_a,
               char           *checksum_b)
{
  OstreeDiffItem *ret = g_new0 (OstreeDiffItem, 1);
  ret->refcount = 1;
  ret->src = a ? g_object_ref (a) : NULL;
  ret->src_info = a_info ? g_object_ref (a_info) : NULL;
  ret->target = b ? g_object_ref (b) : NULL;
  ret->target_info = b_info ? g_object_ref (b_info) : b_info;
  ret->src_checksum = g_strdup (checksum_a);
  ret->target_checksum = g_strdup (checksum_b);
  return ret;
}

static gboolean
diff_files (GFile           *a,
            GFileInfo       *a_info,
            GFile           *b,
            GFileInfo       *b_info,
            OstreeDiffItem **out_item,
            GCancellable    *cancellable,
            GError         **error)
{
  gboolean ret = FALSE;
  ot_lfree char *checksum_a = NULL;
  ot_lfree char *checksum_b = NULL;
  OstreeDiffItem *ret_item = NULL;

  if (!get_file_checksum (a, a_info, &checksum_a, cancellable, error))
    goto out;
  if (!get_file_checksum (b, b_info, &checksum_b, cancellable, error))
    goto out;

  if (strcmp (checksum_a, checksum_b) != 0)
    {
      ret_item = diff_item_new (a, a_info, b, b_info,
                                checksum_a, checksum_b);
    }

  ret = TRUE;
  ot_transfer_out_value(out_item, &ret_item);
 out:
  if (ret_item)
    ostree_diff_item_unref (ret_item);
  return ret;
}

static gboolean
diff_add_dir_recurse (GFile          *d,
                      GPtrArray      *added,
                      GCancellable   *cancellable,
                      GError        **error)
{
  gboolean ret = FALSE;
  GError *temp_error = NULL;
  ot_lobj GFileEnumerator *dir_enum = NULL;
  ot_lobj GFile *child = NULL;
  ot_lobj GFileInfo *child_info = NULL;

  dir_enum = g_file_enumerate_children (d, OSTREE_GIO_FAST_QUERYINFO, 
                                        G_FILE_QUERY_INFO_NOFOLLOW_SYMLINKS,
                                        cancellable, 
                                        error);
  if (!dir_enum)
    goto out;

  while ((child_info = g_file_enumerator_next_file (dir_enum, cancellable, &temp_error)) != NULL)
    {
      const char *name;

      name = g_file_info_get_name (child_info);

      g_clear_object (&child);
      child = g_file_get_child (d, name);

      g_ptr_array_add (added, g_object_ref (child));

      if (g_file_info_get_file_type (child_info) == G_FILE_TYPE_DIRECTORY)
        {
          if (!diff_add_dir_recurse (child, added, cancellable, error))
            goto out;
        }
      
      g_clear_object (&child_info);
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

/**
 * ostree_diff_dirs:
 *
 * Compute the difference between directory @a and @b as 3 separate
 * sets of #OstreeDiffItem in @modified, @removed, and @added.
 */
gboolean
ostree_diff_dirs (GFile          *a,
                  GFile          *b,
                  GPtrArray      *modified,
                  GPtrArray      *removed,
                  GPtrArray      *added,
                  GCancellable   *cancellable,
                  GError        **error)
{
  gboolean ret = FALSE;
  GError *temp_error = NULL;
  ot_lobj GFileEnumerator *dir_enum = NULL;
  ot_lobj GFile *child_a = NULL;
  ot_lobj GFile *child_b = NULL;
  ot_lobj GFileInfo *child_a_info = NULL;
  ot_lobj GFileInfo *child_b_info = NULL;

  child_a_info = g_file_query_info (a, OSTREE_GIO_FAST_QUERYINFO,
                                    G_FILE_QUERY_INFO_NOFOLLOW_SYMLINKS,
                                    cancellable, error);
  if (!child_a_info)
    goto out;

  child_b_info = g_file_query_info (b, OSTREE_GIO_FAST_QUERYINFO,
                                    G_FILE_QUERY_INFO_NOFOLLOW_SYMLINKS,
                                    cancellable, error);
  if (!child_b_info)
    goto out;

  /* Fast path test for unmodified directories */
  if (g_file_info_get_file_type (child_a_info) == G_FILE_TYPE_DIRECTORY
      && g_file_info_get_file_type (child_b_info) == G_FILE_TYPE_DIRECTORY
      && OSTREE_IS_REPO_FILE (a)
      && OSTREE_IS_REPO_FILE (b))
    {
      OstreeRepoFile *a_repof = (OstreeRepoFile*) a;
      OstreeRepoFile *b_repof = (OstreeRepoFile*) b;
      
      if (strcmp (ostree_repo_file_tree_get_content_checksum (a_repof),
                  ostree_repo_file_tree_get_content_checksum (b_repof)) == 0)
        {
          ret = TRUE;
          goto out;
        }
    }

  g_clear_object (&child_a_info);
  g_clear_object (&child_b_info);

  dir_enum = g_file_enumerate_children (a, OSTREE_GIO_FAST_QUERYINFO, 
                                        G_FILE_QUERY_INFO_NOFOLLOW_SYMLINKS,
                                        cancellable, error);
  if (!dir_enum)
    goto out;

  while ((child_a_info = g_file_enumerator_next_file (dir_enum, cancellable, &temp_error)) != NULL)
    {
      const char *name;
      GFileType child_a_type;
      GFileType child_b_type;

      name = g_file_info_get_name (child_a_info);

      g_clear_object (&child_a);
      child_a = g_file_get_child (a, name);
      child_a_type = g_file_info_get_file_type (child_a_info);

      g_clear_object (&child_b);
      child_b = g_file_get_child (b, name);

      g_clear_object (&child_b_info);
      child_b_info = g_file_query_info (child_b, OSTREE_GIO_FAST_QUERYINFO,
                                        G_FILE_QUERY_INFO_NOFOLLOW_SYMLINKS,
                                        cancellable,
                                        &temp_error);
      if (!child_b_info)
        {
          if (g_error_matches (temp_error, G_IO_ERROR, G_IO_ERROR_NOT_FOUND))
            {
              g_clear_error (&temp_error);
              g_ptr_array_add (removed, g_object_ref (child_a));
            }
          else
            {
              g_propagate_error (error, temp_error);
              goto out;
            }
        }
      else
        {
          child_b_type = g_file_info_get_file_type (child_b_info);
          if (child_a_type != child_b_type)
            {
              OstreeDiffItem *diff_item = diff_item_new (child_a, child_a_info,
                                                   child_b, child_b_info, NULL, NULL);
              
              g_ptr_array_add (modified, diff_item);
            }
          else
            {
              OstreeDiffItem *diff_item = NULL;

              if (!diff_files (child_a, child_a_info, child_b, child_b_info, &diff_item,
                               cancellable, error))
                goto out;
              
              if (diff_item)
                g_ptr_array_add (modified, diff_item); /* Transfer ownership */

              if (child_a_type == G_FILE_TYPE_DIRECTORY)
                {
                  if (!ostree_diff_dirs (child_a, child_b, modified,
                                         removed, added, cancellable, error))
                    goto out;
                }
            }
        }
      
      g_clear_object (&child_a_info);
    }
  if (temp_error != NULL)
    {
      g_propagate_error (error, temp_error);
      goto out;
    }

  g_clear_object (&dir_enum);
  dir_enum = g_file_enumerate_children (b, OSTREE_GIO_FAST_QUERYINFO, 
                                        G_FILE_QUERY_INFO_NOFOLLOW_SYMLINKS,
                                        cancellable, error);
  if (!dir_enum)
    goto out;

  g_clear_object (&child_b_info);
  while ((child_b_info = g_file_enumerator_next_file (dir_enum, cancellable, &temp_error)) != NULL)
    {
      const char *name;

      name = g_file_info_get_name (child_b_info);

      g_clear_object (&child_a);
      child_a = g_file_get_child (a, name);

      g_clear_object (&child_b);
      child_b = g_file_get_child (b, name);

      g_clear_object (&child_a_info);
      child_a_info = g_file_query_info (child_a, OSTREE_GIO_FAST_QUERYINFO,
                                        G_FILE_QUERY_INFO_NOFOLLOW_SYMLINKS,
                                        cancellable,
                                        &temp_error);
      if (!child_a_info)
        {
          if (g_error_matches (temp_error, G_IO_ERROR, G_IO_ERROR_NOT_FOUND))
            {
              g_clear_error (&temp_error);
              g_ptr_array_add (added, g_object_ref (child_b));
              if (g_file_info_get_file_type (child_b_info) == G_FILE_TYPE_DIRECTORY)
                {
                  if (!diff_add_dir_recurse (child_b, added, cancellable, error))
                    goto out;
                }
            }
          else
            {
              g_propagate_error (error, temp_error);
              goto out;
            }
        }
      g_clear_object (&child_b_info);
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

void
ostree_diff_print (GFile          *base,
                   GPtrArray      *modified,
                   GPtrArray      *removed,
                   GPtrArray      *added)
{
  guint i;

  for (i = 0; i < modified->len; i++)
    {
      OstreeDiffItem *diff = modified->pdata[i];
      g_print ("M    %s\n", gs_file_get_path_cached (diff->src));
    }
  for (i = 0; i < removed->len; i++)
    {
      g_print ("D    %s\n", gs_file_get_path_cached (removed->pdata[i]));
    }
  for (i = 0; i < added->len; i++)
    {
      GFile *added_f = added->pdata[i];
      if (g_file_is_native (added_f))
        {
          char *relpath = g_file_get_relative_path (base, added_f);
          g_assert (relpath != NULL);
          g_print ("A    /%s\n", relpath);
          g_free (relpath);
        }
      else
        g_print ("A    %s\n", gs_file_get_path_cached (added_f));
    }
}
