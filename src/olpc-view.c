/*
 * olpc-buddy-view.c - Source for GabbleOlpcView
 * Copyright (C) 2008 Collabora Ltd.
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2.1 of the License, or (at your option) any later version.
 *
 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with this library; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin St, Fifth Floor, Boston, MA  02110-1301  USA
 */

#include "olpc-view.h"

#include <stdlib.h>
#include <string.h>

#include <glib.h>

#include <loudmouth/loudmouth.h>
#include <telepathy-glib/dbus.h>

#define DEBUG_FLAG GABBLE_DEBUG_OLPC

#include "debug.h"
#include "extensions/extensions.h"
#include "gabble-signals-marshal.h"
#include "olpc-activity.h"
#include "namespaces.h"
#include "util.h"

/* signals */
enum
{
  CLOSED,
  BUDDY_ACTIVITIES_CHANGED,
  LAST_SIGNAL
};

static guint signals[LAST_SIGNAL] = {0};

/* properties */
enum
{
  PROP_CONNECTION = 1,
  PROP_TYPE,
  PROP_OBJECT_PATH,
  PROP_ID,
  LAST_PROPERTY
};

typedef struct _GabbleOlpcViewPrivate GabbleOlpcViewPrivate;
struct _GabbleOlpcViewPrivate
{
  GabbleConnection *conn;
  /* FIXME: subclass instead of using a type attribute ? */
  GabbleOlpcViewType type;
  char *object_path;
  guint id;

  TpHandleSet *buddies;
  /* TpHandle => GabbleOlpcActivity * */
  GHashTable *activities;

  /* TpHandle (owned in priv->buddies) => GHashTable * */
  GHashTable *buddy_properties;
  /* TpHandle (owned in priv->buddies) => TpHandleSet of activity rooms */
  GHashTable *buddy_rooms;

  gboolean dispose_has_run;
};

static void view_iface_init (gpointer, gpointer);

G_DEFINE_TYPE_WITH_CODE (
    GabbleOlpcView, gabble_olpc_view, G_TYPE_OBJECT,
    G_IMPLEMENT_INTERFACE (GABBLE_TYPE_SVC_OLPC_VIEW,
      view_iface_init));

#define GABBLE_OLPC_VIEW_GET_PRIVATE(obj) \
    ((GabbleOlpcViewPrivate *) obj->priv)


static void
gabble_olpc_view_init (GabbleOlpcView *self)
{
  GabbleOlpcViewPrivate *priv = G_TYPE_INSTANCE_GET_PRIVATE (self,
      GABBLE_TYPE_OLPC_VIEW, GabbleOlpcViewPrivate);

  self->priv = priv;

  priv->dispose_has_run = FALSE;
}

static void
gabble_olpc_view_dispose (GObject *object)
{
  GabbleOlpcView *self = GABBLE_OLPC_VIEW (object);
  GabbleOlpcViewPrivate *priv = GABBLE_OLPC_VIEW_GET_PRIVATE (self);

  if (priv->dispose_has_run)
    return;

  if (priv->buddies != NULL)
    {
      tp_handle_set_destroy (priv->buddies);
      priv->buddies = NULL;
    }

  if (priv->activities != NULL)
    {
      g_hash_table_destroy (priv->activities);
      priv->activities = NULL;
    }

  if (priv->buddy_properties != NULL)
    {
      g_hash_table_destroy (priv->buddy_properties);
      priv->buddy_properties = NULL;
    }

  if (priv->buddy_rooms != NULL)
    {
      g_hash_table_destroy (priv->buddy_rooms);
      priv->buddy_rooms = NULL;
    }

  priv->dispose_has_run = TRUE;

  if (G_OBJECT_CLASS (gabble_olpc_view_parent_class)->dispose)
    G_OBJECT_CLASS (gabble_olpc_view_parent_class)->dispose (object);
}

static void
gabble_olpc_view_finalize (GObject *object)
{
  GabbleOlpcView *self = GABBLE_OLPC_VIEW (object);
  GabbleOlpcViewPrivate *priv = GABBLE_OLPC_VIEW_GET_PRIVATE (self);

  g_free (priv->object_path);

  G_OBJECT_CLASS (gabble_olpc_view_parent_class)->finalize (object);
}

static void
gabble_olpc_view_get_property (GObject *object,
                               guint property_id,
                               GValue *value,
                               GParamSpec *pspec)
{
  GabbleOlpcView *self = GABBLE_OLPC_VIEW (object);
  GabbleOlpcViewPrivate *priv = GABBLE_OLPC_VIEW_GET_PRIVATE (self);

  switch (property_id)
    {
      case PROP_CONNECTION:
        g_value_set_object (value, priv->conn);
        break;
      case PROP_TYPE:
        g_value_set_uint (value, priv->type);
        break;
      case PROP_OBJECT_PATH:
        g_value_set_string (value, priv->object_path);
        break;
      case PROP_ID:
        g_value_set_uint (value, priv->id);
        break;
      default:
        G_OBJECT_WARN_INVALID_PROPERTY_ID (object, property_id, pspec);
        break;
    }
}

static void
gabble_olpc_view_set_property (GObject *object,
                               guint property_id,
                               const GValue *value,
                               GParamSpec *pspec)
{
  GabbleOlpcView *self = GABBLE_OLPC_VIEW (object);
  GabbleOlpcViewPrivate *priv = GABBLE_OLPC_VIEW_GET_PRIVATE (self);

  switch (property_id)
    {
      case PROP_CONNECTION:
        priv->conn = g_value_get_object (value);
        break;
      case PROP_TYPE:
        priv->type = g_value_get_uint (value);
        break;
      case PROP_ID:
        priv->id = g_value_get_uint (value);
        break;
      default:
        G_OBJECT_WARN_INVALID_PROPERTY_ID (object, property_id, pspec);
        break;
    }
}

static GObject *
gabble_olpc_view_constructor (GType type,
                              guint n_props,
                              GObjectConstructParam *props)
{
  GObject *obj;
  GabbleOlpcViewPrivate *priv;
  DBusGConnection *bus;
  TpBaseConnection *conn;
  TpHandleRepoIface *contact_handles;

  obj = G_OBJECT_CLASS (gabble_olpc_view_parent_class)->
           constructor (type, n_props, props);

  priv = GABBLE_OLPC_VIEW_GET_PRIVATE (GABBLE_OLPC_VIEW (obj));
  conn = (TpBaseConnection *) priv->conn;

  priv->object_path = g_strdup_printf ("%s/OlpcView%u",
      conn->object_path, priv->id);
  bus = tp_get_bus ();
  dbus_g_connection_register_g_object (bus, priv->object_path, obj);

  contact_handles = tp_base_connection_get_handles (conn,
      TP_HANDLE_TYPE_CONTACT);

  priv->buddies = tp_handle_set_new (contact_handles);

  priv->activities = g_hash_table_new_full (g_direct_hash, g_direct_equal,
      NULL, g_object_unref);
  priv->buddy_properties = g_hash_table_new_full (g_direct_hash,
      g_direct_equal, NULL, (GDestroyNotify) g_hash_table_unref);
  priv->buddy_rooms = g_hash_table_new_full (g_direct_hash, g_direct_equal,
      NULL, (GDestroyNotify) tp_handle_set_destroy);

  return obj;
}

static void
gabble_olpc_view_class_init (GabbleOlpcViewClass *gabble_olpc_view_class)
{
  GObjectClass *object_class = G_OBJECT_CLASS (gabble_olpc_view_class);
  GParamSpec *param_spec;

  object_class->get_property = gabble_olpc_view_get_property;
  object_class->set_property = gabble_olpc_view_set_property;
  object_class->constructor = gabble_olpc_view_constructor;

  g_type_class_add_private (gabble_olpc_view_class,
      sizeof (GabbleOlpcViewPrivate));

  object_class->dispose = gabble_olpc_view_dispose;
  object_class->finalize = gabble_olpc_view_finalize;

   param_spec = g_param_spec_object (
      "connection",
      "GabbleConnection object",
      "Gabble connection object that owns this view object.",
      GABBLE_TYPE_CONNECTION,
      G_PARAM_CONSTRUCT_ONLY |
      G_PARAM_READWRITE |
      G_PARAM_STATIC_NAME |
      G_PARAM_STATIC_NICK |
      G_PARAM_STATIC_BLURB);
  g_object_class_install_property (object_class, PROP_CONNECTION, param_spec);

  param_spec = g_param_spec_uint (
      "type",
      "view type",
      "the type of query who creates this view object. A GabbleOlpcViewType",
      GABBLE_OLPC_VIEW_TYPE_BUDDY, NUM_GABBLE_OLPC_VIEW_TYPE -1,
      GABBLE_OLPC_VIEW_TYPE_BUDDY,
      G_PARAM_CONSTRUCT_ONLY |
      G_PARAM_READWRITE |
      G_PARAM_STATIC_NAME |
      G_PARAM_STATIC_NICK |
      G_PARAM_STATIC_BLURB);
  g_object_class_install_property (object_class, PROP_TYPE, param_spec);

  param_spec = g_param_spec_string (
      "object-path",
      "D-Bus object path",
      "The D-Bus object path of this view object",
      NULL,
      G_PARAM_READABLE |
      G_PARAM_STATIC_NAME |
      G_PARAM_STATIC_NICK |
      G_PARAM_STATIC_BLURB);
  g_object_class_install_property (object_class, PROP_OBJECT_PATH, param_spec);

  param_spec = g_param_spec_uint (
      "id",
      "query ID",
      "The ID of the query associated with this view",
      0, G_MAXUINT, 0,
      G_PARAM_CONSTRUCT_ONLY |
      G_PARAM_READWRITE |
      G_PARAM_STATIC_NAME |
      G_PARAM_STATIC_NICK |
      G_PARAM_STATIC_BLURB);
  g_object_class_install_property (object_class, PROP_ID, param_spec);

  /* We can't use "closed" as it's already use for the D-Bus signal... */
  signals[CLOSED] =
    g_signal_new ("closed_",
        G_OBJECT_CLASS_TYPE (gabble_olpc_view_class),
        G_SIGNAL_RUN_LAST | G_SIGNAL_DETAILED,
        0,
        NULL, NULL,
        gabble_marshal_VOID__VOID,
        G_TYPE_NONE, 0);

  signals[BUDDY_ACTIVITIES_CHANGED] =
    g_signal_new ("buddy-activities-changed",
        G_OBJECT_CLASS_TYPE (gabble_olpc_view_class),
        G_SIGNAL_RUN_LAST | G_SIGNAL_DETAILED,
        0,
        NULL, NULL,
        gabble_marshal_VOID__UINT,
        G_TYPE_NONE, 1, G_TYPE_UINT);
}

GabbleOlpcView *
gabble_olpc_view_new (GabbleConnection *conn,
                      GabbleOlpcViewType type,
                      guint id)
{
  return g_object_new (GABBLE_TYPE_OLPC_VIEW,
      "connection", conn,
      "type", type,
      "id", id,
      NULL);
}

static void
olpc_view_get_buddies (GabbleSvcOLPCView *iface,
                       DBusGMethodInvocation *context)
{
  GabbleOlpcView *self = GABBLE_OLPC_VIEW (iface);
  GabbleOlpcViewPrivate *priv = GABBLE_OLPC_VIEW_GET_PRIVATE (self);
  GArray *buddies;

  buddies = tp_handle_set_to_array (priv->buddies);

  gabble_svc_olpc_view_return_from_get_buddies (context, buddies);

  g_array_free (buddies, TRUE);
}

static void
add_activity_to_array (TpHandle handle,
                       GabbleOlpcActivity *activity,
                       GPtrArray *array)
{
  GValue gvalue = { 0 };

  g_value_init (&gvalue, GABBLE_STRUCT_TYPE_ACTIVITY);
  g_value_take_boxed (&gvalue, dbus_g_type_specialized_construct
      (GABBLE_STRUCT_TYPE_ACTIVITY));
  dbus_g_type_struct_set (&gvalue,
      0, activity->id,
      1, activity->room,
      G_MAXUINT);

  g_ptr_array_add (array, g_value_get_boxed (&gvalue));
}

static void
free_activities_array (GPtrArray *activities)
{
  guint i;

  for (i = 0; i < activities->len; i++)
    g_boxed_free (GABBLE_STRUCT_TYPE_ACTIVITY, activities->pdata[i]);

  g_ptr_array_free (activities, TRUE);
}

static void
olpc_view_get_activities (GabbleSvcOLPCView *iface,
                          DBusGMethodInvocation *context)
{
  GabbleOlpcView *self = GABBLE_OLPC_VIEW (iface);
  GabbleOlpcViewPrivate *priv = GABBLE_OLPC_VIEW_GET_PRIVATE (self);
  GPtrArray *activities;

  activities = g_ptr_array_new ();

  g_hash_table_foreach (priv->activities, (GHFunc) add_activity_to_array,
      activities);

  gabble_svc_olpc_view_return_from_get_activities (context, activities);

  free_activities_array (activities);
}

static void
buddy_left_activities_foreach (TpHandleSet *set,
                               TpHandle buddy,
                               GabbleOlpcView *self)
{
  GabbleOlpcViewPrivate *priv = GABBLE_OLPC_VIEW_GET_PRIVATE (self);

  /* Remove all the activity of this buddy */
  if (!g_hash_table_remove (priv->buddy_rooms, GUINT_TO_POINTER (buddy)))
    return;

  g_signal_emit (G_OBJECT (self), signals[BUDDY_ACTIVITIES_CHANGED],
      0, buddy);
}

static void
olpc_view_close (GabbleSvcOLPCView *iface,
                 DBusGMethodInvocation *context)
{
  GabbleOlpcView *self = GABBLE_OLPC_VIEW (iface);
  GError *error = NULL;

  if (!gabble_olpc_view_close (self, &error))
    {
      dbus_g_method_return_error (context, error);
      g_error_free (error);
    }

  gabble_svc_olpc_view_return_from_close (context);
}

gboolean
gabble_olpc_view_close (GabbleOlpcView *self,
                        GError **error)
{
  GabbleOlpcViewPrivate *priv = GABBLE_OLPC_VIEW_GET_PRIVATE (self);
  LmMessage *msg;
  gchar *id_str;

  id_str = g_strdup_printf ("%u", priv->id);

  if (priv->type == GABBLE_OLPC_VIEW_TYPE_BUDDY)
    {
      msg = lm_message_build (priv->conn->olpc_gadget_buddy,
          LM_MESSAGE_TYPE_MESSAGE,
          '(', "close", "",
            '@', "xmlns", NS_OLPC_BUDDY,
            '@', "id", id_str,
          ')', NULL);
    }
  else if (priv->type == GABBLE_OLPC_VIEW_TYPE_ACTIVITY)
    {
      msg = lm_message_build (priv->conn->olpc_gadget_activity,
          LM_MESSAGE_TYPE_MESSAGE,
          '(', "close", "",
            '@', "xmlns", NS_OLPC_ACTIVITY,
            '@', "id", id_str,
          ')', NULL);
    }
  else
    {
      g_assert_not_reached ();
    }

  g_free (id_str);

  if (!_gabble_connection_send (priv->conn, msg, error))
    {
      lm_message_unref (msg);
      return FALSE;
    }

  lm_message_unref (msg);

  /* Claim that all the buddies left their activities */
  tp_handle_set_foreach (priv->buddies,
      (TpHandleSetMemberFunc) buddy_left_activities_foreach, self);

  g_signal_emit (G_OBJECT (self), signals[CLOSED], 0);

  gabble_svc_olpc_view_emit_closed (self);

  return TRUE;
}

/* If room is not zero, these buddies are associated with the activity
 * of this room. They'll leave the view if the activity is removed.
 */
void
gabble_olpc_view_add_buddies (GabbleOlpcView *self,
                              GArray *buddies,
                              GPtrArray *buddies_properties,
                              TpHandle room)
{
  GabbleOlpcViewPrivate *priv = GABBLE_OLPC_VIEW_GET_PRIVATE (self);
  guint i;
  GArray *empty;
  TpHandleRepoIface *room_repo;
  GArray *buddies_changed;

  g_assert (buddies->len == buddies_properties->len);
  if (buddies->len == 0)
    return;

  room_repo = tp_base_connection_get_handles ((TpBaseConnection *) priv->conn,
      TP_HANDLE_TYPE_ROOM);

  empty = g_array_new (FALSE, FALSE, sizeof (TpHandle));
  buddies_changed = g_array_new (FALSE, FALSE, sizeof (TpHandle));

  /* store properties */
  for (i = 0; i < buddies->len; i++)
    {
      TpHandle handle;
      GHashTable *properties;

      handle = g_array_index (buddies, TpHandle, i);
      properties = g_ptr_array_index (buddies_properties, i);

      tp_handle_set_add (priv->buddies, handle);
      g_hash_table_insert (priv->buddy_properties, GUINT_TO_POINTER (handle),
          properties);
      g_hash_table_ref (properties);

      if (room != 0)
        {
          /* buddies are in an activity */
          TpHandleSet *set;

          set = g_hash_table_lookup (priv->buddy_rooms, GUINT_TO_POINTER (
                handle));

          if (set == NULL)
            {
              set = tp_handle_set_new (room_repo);

              g_hash_table_insert (priv->buddy_rooms, GUINT_TO_POINTER (
                    handle), set);
            }

          if (!tp_handle_set_is_member (set, room))
            {
              tp_handle_set_add (set, room);

              /* We fire BuddyInfo.ActivitiesChanged signal after
               * View.BuddiesChanged so client knows where these buddies
               * come from */
              g_array_append_val (buddies_changed, handle);
            }
        }
    }

  gabble_svc_olpc_view_emit_buddies_changed (self, buddies, empty);

  for (i = 0; i < buddies_changed->len; i++)
    {
      TpHandle handle;

      handle = g_array_index (buddies_changed, TpHandle, i);
      g_signal_emit (G_OBJECT (self),
          signals[BUDDY_ACTIVITIES_CHANGED], 0, handle);
    }

  g_array_free (buddies_changed, TRUE);
  g_array_free (empty, TRUE);
}

static void
remove_buddy_foreach (TpHandleSet *buddies,
                      TpHandle handle,
                      GabbleOlpcView *self)
{
  GabbleOlpcViewPrivate *priv = GABBLE_OLPC_VIEW_GET_PRIVATE (self);

  tp_handle_set_remove (priv->buddies, handle);
  g_hash_table_remove (priv->buddy_properties, GUINT_TO_POINTER (handle));
  g_hash_table_remove (priv->buddy_rooms, GUINT_TO_POINTER (handle));
}

void
gabble_olpc_view_remove_buddies (GabbleOlpcView *self,
                                 TpHandleSet *buddies)
{
  GArray *removed, *empty;

  if (tp_handle_set_size (buddies) == 0)
    return;

  tp_handle_set_foreach (buddies,
      (TpHandleSetMemberFunc) remove_buddy_foreach, self);

  empty = g_array_new (FALSE, FALSE, sizeof (TpHandle));
  removed = tp_handle_set_to_array (buddies);

  gabble_svc_olpc_view_emit_buddies_changed (self, empty, removed);

  g_array_free (empty, TRUE);
  g_array_free (removed, TRUE);
}

gboolean
gabble_olpc_view_set_buddy_properties (GabbleOlpcView *self,
                                       TpHandle buddy,
                                       GHashTable *properties)
{
  GabbleOlpcViewPrivate *priv = GABBLE_OLPC_VIEW_GET_PRIVATE (self);

  if (!tp_handle_set_is_member (priv->buddies, buddy))
    {
      DEBUG ("buddy %d is not member of this view", buddy);
      return FALSE;
    }

  tp_handle_set_add (priv->buddies, buddy);
  g_hash_table_insert (priv->buddy_properties, GUINT_TO_POINTER (buddy),
      properties);
  g_hash_table_ref (properties);

  return TRUE;
}

GHashTable *
gabble_olpc_view_get_buddy_properties (GabbleOlpcView *self,
                                       TpHandle buddy)
{
  GabbleOlpcViewPrivate *priv = GABBLE_OLPC_VIEW_GET_PRIVATE (self);

  return g_hash_table_lookup (priv->buddy_properties,
      GUINT_TO_POINTER (buddy));
}

void
gabble_olpc_view_add_activities (GabbleOlpcView *self,
                                 GHashTable *activities)
{
  GabbleOlpcViewPrivate *priv = GABBLE_OLPC_VIEW_GET_PRIVATE (self);
  GPtrArray *added, *empty;

  if (g_hash_table_size (activities) == 0)
    return;

  tp_g_hash_table_update (priv->activities, activities, NULL, g_object_ref);

  added = g_ptr_array_new ();
  g_hash_table_foreach (activities, (GHFunc) add_activity_to_array, added);

  empty = g_ptr_array_new ();

  gabble_svc_olpc_view_emit_activities_changed (self, added, empty);

  free_activities_array (added);
  g_ptr_array_free (empty, TRUE);
}

struct remove_activity_foreach_buddy_ctx
{
  GabbleOlpcView *view;
  TpHandle room;
  /* buddies who have to be removed */
  TpHandleSet *removed;
};

static void
remove_activity_foreach_buddy (TpHandle buddy,
                               TpHandleSet *set,
                               struct remove_activity_foreach_buddy_ctx *ctx)
{
  if (set == NULL)
    return;

  if (tp_handle_set_remove (set, ctx->room))
    {
      if (tp_handle_set_size (set) == 0)
        {
          /* No more activity for this buddy. Remove it */
          tp_handle_set_add (ctx->removed, buddy);
        }

      g_signal_emit (G_OBJECT (ctx->view), signals[BUDDY_ACTIVITIES_CHANGED],
          0, buddy);
    }
}

void
gabble_olpc_view_remove_activities (GabbleOlpcView *self,
                                    TpHandleSet *rooms)
{
  GabbleOlpcViewPrivate *priv = GABBLE_OLPC_VIEW_GET_PRIVATE (self);
  GPtrArray *removed, *empty;
  GArray *array;
  guint i;
  TpHandleRepoIface *contact_repo;

  if (tp_handle_set_size (rooms) == 0)
    return;

  contact_repo = tp_base_connection_get_handles (
      (TpBaseConnection *) priv->conn, TP_HANDLE_TYPE_CONTACT);

  /* for easier iteration */
  array = tp_handle_set_to_array (rooms);

  removed = g_ptr_array_new ();
  empty = g_ptr_array_new ();

  for (i = 0; i < array->len; i++)
    {
      TpHandle room;
      GabbleOlpcActivity *activity;
      struct remove_activity_foreach_buddy_ctx ctx;

      room = g_array_index (array, TpHandle, i);

      activity = g_hash_table_lookup (priv->activities,
          GUINT_TO_POINTER (room));
      if (activity == NULL)
        continue;

      add_activity_to_array (room, activity, removed);
      g_hash_table_remove (priv->activities, GUINT_TO_POINTER (room));

      /* remove the activity from all rooms set */
      ctx.view = self;
      ctx.room = room;
      ctx.removed = tp_handle_set_new (contact_repo);

      g_hash_table_foreach (priv->buddy_rooms,
          (GHFunc) remove_activity_foreach_buddy, &ctx);

      gabble_olpc_view_remove_buddies (self, ctx.removed);

      tp_handle_set_destroy (ctx.removed);
    }

  gabble_svc_olpc_view_emit_activities_changed (self, empty, removed);

  free_activities_array (removed);
  g_ptr_array_free (empty, TRUE);
  g_array_free (array, TRUE);
}

GPtrArray *
gabble_olpc_view_get_buddy_activities (GabbleOlpcView *self,
                                       TpHandle buddy)
{
  GabbleOlpcViewPrivate *priv = GABBLE_OLPC_VIEW_GET_PRIVATE (self);
  GPtrArray *activities;
  TpHandleSet *rooms_set;
  GArray *rooms;
  guint i;

  activities = g_ptr_array_new ();

  rooms_set = g_hash_table_lookup (priv->buddy_rooms,
      GUINT_TO_POINTER (buddy));
  if (rooms_set == NULL || tp_handle_set_size (rooms_set) == 0)
    return activities;

  /* Convert to an array for easier iteration */
  rooms = tp_handle_set_to_array (rooms_set);
  for (i = 0; i < rooms->len; i++)
    {
      TpHandle room;
      GabbleOlpcActivity *activity;

      room = g_array_index (rooms, TpHandle, i);

      activity = g_hash_table_lookup (priv->activities,
          GUINT_TO_POINTER (room));
      if (activity == NULL)
        {
          /* This shouldn't happen as long as:
           *
           * - Gadget doesn't send us <joined> stanzas about an activity
           *   which was not previously announced as being part of the view.
           *
           * - We don't call gabble_olpc_view_add_buddies with an activity
           *   which was not previoulsy added to the view.
           */
          DEBUG ("Buddy %d is supposed to be in activity %d but view doesn't"
              " contain its info", buddy, room);
          continue;
        }

      g_ptr_array_add (activities, activity);
    }

  g_array_free (rooms, TRUE);

  return activities;
}

void
gabble_olpc_view_buddies_left_activity (GabbleOlpcView *self,
                                        GArray *buddies,
                                        TpHandle room)
{
  GabbleOlpcViewPrivate *priv = GABBLE_OLPC_VIEW_GET_PRIVATE (self);
  guint i;
  TpHandleRepoIface *contact_repo;
  TpHandleSet *removed;

  contact_repo = tp_base_connection_get_handles (
      (TpBaseConnection *) priv->conn, TP_HANDLE_TYPE_CONTACT);

  removed = tp_handle_set_new (contact_repo);

  for (i = 0; i < buddies->len; i++)
    {
      TpHandleSet *set;
      TpHandle buddy;

      buddy = g_array_index (buddies, TpHandle, i);
      set = g_hash_table_lookup (priv->buddy_rooms, GUINT_TO_POINTER (buddy));
      if (set == NULL)
        continue;

      if (tp_handle_set_remove (set, room))
        {
          if (tp_handle_set_size (set) == 0)
            {
              /* Remove from the view */
              tp_handle_set_add (removed, buddy);
            }

          g_signal_emit (G_OBJECT (self), signals[BUDDY_ACTIVITIES_CHANGED],
              0, buddy);
        }
    }

  gabble_olpc_view_remove_buddies (self, removed);

  tp_handle_set_destroy (removed);
}

static void
view_iface_init (gpointer g_iface,
                 gpointer iface_data)
{
  GabbleSvcOLPCViewClass *klass = g_iface;

#define IMPLEMENT(x) gabble_svc_olpc_view_implement_##x (\
    klass, olpc_view_##x)
  IMPLEMENT(get_buddies);
  IMPLEMENT(get_activities);
  IMPLEMENT(close);
#undef IMPLEMENT
}
