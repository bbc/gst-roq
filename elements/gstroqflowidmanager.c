/*
 * Copyright 2024 British Broadcasting Corporation - Research and Development
 *
 * Author: Sam Hurst <sam.hurst@bbc.co.uk>
 * 
 * Permission is hereby granted, free of charge, to any person obtaining a
 * copy of this software and associated documentation files (the "Software"),
 * to deal in the Software without restriction, including without limitation
 * the rights to use, copy, modify, merge, publish, distribute, sublicense,
 * and/or sell copies of the Software, and to permit persons to whom the
 * Software is furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be included in
 * all copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
 * AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING
 * FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER
 * DEALINGS IN THE SOFTWARE.
 *
 * Alternatively, the contents of this file may be used under the
 * GNU Lesser General Public License Version 2.1 (the "LGPL"), in
 * which case the following provisions apply instead of the ones
 * mentioned above:
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Library General Public
 * License as published by the Free Software Foundation; either
 * version 2 of the License, or (at your option) any later version.
 *
 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Library General Public License for more details.
 *
 * You should have received a copy of the GNU Library General Public
 * License along with this library; if not, write to the
 * Free Software Foundation, Inc., 59 Temple Place - Suite 330,
 * Boston, MA 02111-1307, USA.
 */

#include "gstroqflowidmanager.h"

struct _GstROQFlowIDManager {
  GstObject object;

  GMutex mutex;
  GList *flow_ids;
};

GST_DEBUG_CATEGORY_STATIC (roqflowidmanager);
#define GST_CAT_DEFAULT roqflowidmanager

#define gst_roq_flow_id_manager_parent_class parent_class
G_DEFINE_TYPE (GstROQFlowIDManager, gst_roq_flow_id_manager, GST_TYPE_OBJECT);

static GObject *gst_roq_flow_id_manager_constructor (GType type,
    guint n_construct_params, GObjectConstructParam *construct_params);
void gst_roq_flow_id_manager_dispose (GObject *obj);

static void
gst_roq_flow_id_manager_class_init (GstROQFlowIDManagerClass *klass)
{
  GObjectClass *object_class = G_OBJECT_CLASS (klass);

  object_class->constructor = gst_roq_flow_id_manager_constructor;
  object_class->dispose = gst_roq_flow_id_manager_dispose;

  GST_DEBUG_CATEGORY_INIT (roqflowidmanager, "roqflowidmanager", 0,
      "Singleton class for managing RoQ Flow Identifiers");
}

static void
gst_roq_flow_id_manager_init (GstROQFlowIDManager *manager)
{
  g_mutex_init (&manager->mutex);
}

static GObject *
gst_roq_flow_id_manager_constructor (GType type, guint n_construct_params,
    GObjectConstructParam *construct_params)
{
  /*
   * Adapted from here:
   * https://blogs.gnome.org/xclaesse/2010/02/11/how-to-make-a-gobject-singleton/
   */
  static GObject *self = NULL;

  /* TODO: Make thread safe */
  if (self == NULL) {
    self = G_OBJECT_CLASS (gst_roq_flow_id_manager_parent_class)->constructor (
        type, n_construct_params, construct_params);
    g_object_add_weak_pointer (self, (gpointer) &self);
    return self;
  }

  return g_object_ref (self);
}

void
gst_roq_flow_id_manager_dispose (GObject *obj)
{
  GstROQFlowIDManager *manager = GST_ROQ_FLOW_ID_MANAGER (obj);

  g_mutex_lock (&manager->mutex);
  if (manager->flow_ids) {
    g_list_free_full (manager->flow_ids, g_free);
  }
  g_mutex_unlock (&manager->mutex);

  g_mutex_clear (&manager->mutex);
}

GstROQFlowIDManager *
_roq_flow_id_manager_get_instance ()
{
  return g_object_new (GST_TYPE_ROQ_FLOW_ID_MANAGER, NULL);
}

gpointer
_copy_flow_id (gconstpointer src, gpointer data)
{
  gpointer rv = g_malloc (sizeof (guint64));
  *((guint64 *) rv) = *((guint64* ) src);
  return rv;
}

GList *
_roq_flow_id_manager_get_all_instance (GstROQFlowIDManager *manager)
{
  GList *rv;
  
  g_mutex_lock (&manager->mutex);
  rv = g_list_copy_deep (manager->flow_ids, (GCopyFunc) _copy_flow_id, NULL);
  g_mutex_unlock (&manager->mutex);

  return rv;
}

GList *
gst_roq_flow_id_manager_get_all ()
{
  GstROQFlowIDManager *manager = _roq_flow_id_manager_get_instance ();
  g_return_val_if_fail (manager, NULL);
  return _roq_flow_id_manager_get_all_instance (manager);
}

gboolean
_check_flow_id_in_use (GList *list, guint64 flow_id)
{
  GList *p;

  for (p = list; p != NULL; p = p->next) {
    if (*(guint64 *) p->data == flow_id) {
      return TRUE;
    }
  }
  return FALSE;
}

gboolean
_roq_flow_id_manager_new_flow_id_instance (GstROQFlowIDManager *manager,
    guint64 flow_id)
{
  gboolean rv = FALSE;

  g_mutex_lock (&manager->mutex);
  if (!_check_flow_id_in_use (manager->flow_ids, flow_id)) {
    gpointer *id = g_malloc (sizeof (guint64));
    *((guint64 *) id) = flow_id;
    manager->flow_ids = g_list_append (manager->flow_ids, id);
    rv = manager->flow_ids != NULL;
  }
  g_mutex_unlock (&manager->mutex);

  return rv;
}

gboolean
gst_roq_flow_id_manager_new_flow_id (guint64 flow_id)
{
  GstROQFlowIDManager *manager = _roq_flow_id_manager_get_instance ();
  g_return_val_if_fail (manager, FALSE);
  return _roq_flow_id_manager_new_flow_id_instance (manager, flow_id);
}

gboolean
_roq_flow_id_manager_flow_id_in_use_instance (GstROQFlowIDManager *manager,
    guint64 flow_id)
{
  gboolean rv = FALSE;

  g_mutex_lock (&manager->mutex);
  rv = _check_flow_id_in_use (manager->flow_ids, flow_id);
  g_mutex_unlock (&manager->mutex);

  return rv;
}

gboolean
gst_roq_flow_id_manager_flow_id_in_use (guint64 flow_id)
{
  GstROQFlowIDManager *manager = _roq_flow_id_manager_get_instance ();
  g_return_val_if_fail (manager, FALSE);
  return _roq_flow_id_manager_flow_id_in_use_instance (manager, flow_id);
}

void
_roq_flow_id_manager_retire_flow_id (GstROQFlowIDManager *manager,
    guint64 flow_id)
{
  GList *p;

  g_mutex_lock (&manager->mutex);
  for (p = manager->flow_ids; p != NULL; p = p->next) {
    if ((*(guint64 *) p->data) == flow_id) {
      g_free (p->data);
      manager->flow_ids = g_list_delete_link (manager->flow_ids, p);
      break;
    }
  }
  g_mutex_unlock (&manager->mutex);
}

void
gst_roq_flow_id_manager_retire_flow_id (guint64 flow_id)
{
  GstROQFlowIDManager *manager = _roq_flow_id_manager_get_instance ();
  g_return_if_fail (manager);
  return _roq_flow_id_manager_retire_flow_id (manager, flow_id);
}