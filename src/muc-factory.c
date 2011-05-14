/*
 * muc-factory.c - Source for GabbleMucFactory
 * Copyright (C) 2006 Collabora Ltd.
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

#include "config.h"
#include "muc-factory.h"

#define DBUS_API_SUBJECT_TO_CHANGE

#include <string.h>

#include <dbus/dbus-glib.h>
#include <dbus/dbus-glib-lowlevel.h>
#include <loudmouth/loudmouth.h>
#include <telepathy-glib/channel-manager.h>
#include <telepathy-glib/dbus.h>
#include <telepathy-glib/interfaces.h>
#include <telepathy-glib/util.h>

#include <telepathy-yell/interfaces.h>

#define DEBUG_FLAG GABBLE_DEBUG_MUC

#include "caps-channel-manager.h"
#include "connection.h"
#include "conn-olpc.h"
#include "debug.h"
#include "disco.h"
#include "im-channel.h"
#include "media-factory.h"
#include "message-util.h"
#include "muc-channel.h"
#include "namespaces.h"
#include "presence-cache.h"
#include "tubes-channel.h"
#include "tube-dbus.h"
#include "tube-stream.h"
#include "util.h"
#include "call-muc-channel.h"

static void channel_manager_iface_init (gpointer, gpointer);

G_DEFINE_TYPE_WITH_CODE (GabbleMucFactory, gabble_muc_factory, G_TYPE_OBJECT,
    G_IMPLEMENT_INTERFACE (TP_TYPE_CHANNEL_MANAGER,
      channel_manager_iface_init);
    G_IMPLEMENT_INTERFACE (GABBLE_TYPE_CAPS_CHANNEL_MANAGER, NULL));

/* properties */
enum
{
  PROP_CONNECTION = 1,
  LAST_PROPERTY
};

typedef struct {
  GabbleMucFactory *self;
  gpointer token;
} Request;

struct _GabbleMucFactoryPrivate
{
  GabbleConnection *conn;
  gulong status_changed_id;

  LmMessageHandler *message_cb;
  /* GUINT_TO_POINTER(room_handle) => (GabbleMucChannel *) */
  GHashTable *text_channels;
  /* Tubes channels which will be considered ready when the corresponding
   * text channel is created.
   * Borrowed GabbleMucChannel => borrowed GabbleTubesChannel */
  GHashTable *text_needed_for_tubes;
  /* Tube channels which will be considered ready when the corresponding
   * tubes channel is created.
   * Borrowed GabbleTubesChannel => GSlist of borrowed GabbleTubeIface */
  GHashTable *tubes_needed_for_tube;
  /* GabbleDiscoRequest * => NULL (used as a set) */
  GHashTable *disco_requests;

  /* Map from channels to the request-tokens of requests that they will satisfy
   * when they're ready.
   * Borrowed TpExportableChannel => GSList of gpointer */
  GHashTable *queued_requests;

  gboolean dispose_has_run;
};

#define GABBLE_MUC_FACTORY_GET_PRIVATE(o) \
  (G_TYPE_INSTANCE_GET_PRIVATE ((o), GABBLE_TYPE_MUC_FACTORY, \
                                GabbleMucFactoryPrivate))

static GObject *gabble_muc_factory_constructor (GType type, guint n_props,
    GObjectConstructParam *props);

static void
gabble_muc_factory_init (GabbleMucFactory *fac)
{
  GabbleMucFactoryPrivate *priv = GABBLE_MUC_FACTORY_GET_PRIVATE (fac);

  fac->priv = priv;

  priv->text_channels = g_hash_table_new_full (g_direct_hash, g_direct_equal,
      NULL, g_object_unref);
  priv->text_needed_for_tubes = g_hash_table_new_full (g_direct_hash,
      g_direct_equal, NULL, NULL);
  priv->tubes_needed_for_tube = g_hash_table_new_full (g_direct_hash,
      g_direct_equal, NULL, (GDestroyNotify) g_slist_free);

  priv->disco_requests = g_hash_table_new_full (g_direct_hash, g_direct_equal,
                                                NULL, NULL);

  priv->queued_requests = g_hash_table_new_full (g_direct_hash,
      g_direct_equal, NULL, NULL);

  priv->message_cb = NULL;

  priv->conn = NULL;
  priv->dispose_has_run = FALSE;
}


static void
cancel_disco_request (gpointer key, gpointer value, gpointer user_data)
{
  GabbleDisco *disco = GABBLE_DISCO (user_data);
  GabbleDiscoRequest *request = (GabbleDiscoRequest *) key;

  gabble_disco_cancel_request (disco, request);
}


static void gabble_muc_factory_close_all (GabbleMucFactory *fac);


static void
gabble_muc_factory_dispose (GObject *object)
{
  GabbleMucFactory *fac = GABBLE_MUC_FACTORY (object);
  GabbleMucFactoryPrivate *priv = GABBLE_MUC_FACTORY_GET_PRIVATE (fac);

  if (priv->dispose_has_run)
    return;

  DEBUG ("dispose called");
  priv->dispose_has_run = TRUE;

  gabble_muc_factory_close_all (fac);
  g_assert (priv->text_channels == NULL);
  g_assert (priv->text_needed_for_tubes == NULL);
  g_assert (priv->tubes_needed_for_tube == NULL);
  g_assert (priv->queued_requests == NULL);

  g_hash_table_foreach (priv->disco_requests, cancel_disco_request,
      priv->conn->disco);
  g_hash_table_destroy (priv->disco_requests);

  if (G_OBJECT_CLASS (gabble_muc_factory_parent_class)->dispose)
    G_OBJECT_CLASS (gabble_muc_factory_parent_class)->dispose (object);
}

static void
gabble_muc_factory_get_property (GObject    *object,
                                 guint       property_id,
                                 GValue     *value,
                                 GParamSpec *pspec)
{
  GabbleMucFactory *fac = GABBLE_MUC_FACTORY (object);
  GabbleMucFactoryPrivate *priv = GABBLE_MUC_FACTORY_GET_PRIVATE (fac);

  switch (property_id) {
    case PROP_CONNECTION:
      g_value_set_object (value, priv->conn);
      break;
    default:
      G_OBJECT_WARN_INVALID_PROPERTY_ID (object, property_id, pspec);
      break;
  }
}

static void
gabble_muc_factory_set_property (GObject      *object,
                                 guint         property_id,
                                 const GValue *value,
                                 GParamSpec   *pspec)
{
  GabbleMucFactory *fac = GABBLE_MUC_FACTORY (object);
  GabbleMucFactoryPrivate *priv = GABBLE_MUC_FACTORY_GET_PRIVATE (fac);

  switch (property_id) {
    case PROP_CONNECTION:
      priv->conn = g_value_get_object (value);
      break;
    default:
      G_OBJECT_WARN_INVALID_PROPERTY_ID (object, property_id, pspec);
      break;
  }
}

static void
gabble_muc_factory_class_init (GabbleMucFactoryClass *gabble_muc_factory_class)
{
  GObjectClass *object_class = G_OBJECT_CLASS (gabble_muc_factory_class);
  GParamSpec *param_spec;

  g_type_class_add_private (gabble_muc_factory_class,
      sizeof (GabbleMucFactoryPrivate));

  object_class->constructor = gabble_muc_factory_constructor;
  object_class->dispose = gabble_muc_factory_dispose;

  object_class->get_property = gabble_muc_factory_get_property;
  object_class->set_property = gabble_muc_factory_set_property;

  param_spec = g_param_spec_object ("connection", "GabbleConnection object",
      "Gabble connection object that owns this MUC factory object.",
      GABBLE_TYPE_CONNECTION,
      G_PARAM_CONSTRUCT_ONLY | G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS);
  g_object_class_install_property (object_class, PROP_CONNECTION, param_spec);
}

/**
 * muc_channel_closed_cb:
 *
 * Signal callback for when a MUC channel is closed. Removes the references
 * that MucFactory holds to them.
 */
static void
muc_channel_closed_cb (GabbleMucChannel *chan, gpointer user_data)
{
  GabbleMucFactory *fac = GABBLE_MUC_FACTORY (user_data);
  GabbleMucFactoryPrivate *priv = GABBLE_MUC_FACTORY_GET_PRIVATE (fac);
  TpHandle room_handle;

  tp_channel_manager_emit_channel_closed_for_object (fac,
      TP_EXPORTABLE_CHANNEL (chan));

  if (priv->text_channels != NULL)
    {
      g_object_get (chan, "handle", &room_handle, NULL);

      DEBUG ("removing MUC channel with handle %d", room_handle);

      gabble_muc_channel_close_tube (chan);

      g_hash_table_remove (priv->text_channels, GUINT_TO_POINTER (room_handle));
    }
}

static void
muc_ready_cb (GabbleMucChannel *text_chan,
              gpointer data)
{
  GabbleMucFactory *fac = GABBLE_MUC_FACTORY (data);
  GabbleMucFactoryPrivate *priv = GABBLE_MUC_FACTORY_GET_PRIVATE (fac);
  GabbleTubesChannel *tubes_chan;
  GSList *requests_satisfied_text, *requests_satisfied_tubes = NULL;
  gboolean text_requested;
  GSList *tube_channels, *l;

  DEBUG ("text chan=%p", text_chan);

  g_object_get (text_chan, "requested", &text_requested, NULL);

  requests_satisfied_text = g_hash_table_lookup (
      priv->queued_requests, text_chan);
  g_hash_table_steal (priv->queued_requests, text_chan);
  requests_satisfied_text = g_slist_reverse (requests_satisfied_text);

  tubes_chan = g_hash_table_lookup (priv->text_needed_for_tubes, text_chan);
  g_hash_table_remove (priv->text_needed_for_tubes, text_chan);

  if (tubes_chan != NULL)
    {
      requests_satisfied_tubes = g_hash_table_lookup (
          priv->queued_requests, tubes_chan);
      g_hash_table_steal (priv->queued_requests, tubes_chan);
    }

  /* Announce tube channels now */
  /* FIXME: we should probably aggregate tube announcement with tubes and text
   * ones in some cases. */
  tube_channels = g_hash_table_lookup (priv->tubes_needed_for_tube,
      tubes_chan);

  tube_channels = g_slist_reverse (tube_channels);
  for (l = tube_channels; l != NULL; l = g_slist_next (l))
    {
      GabbleTubeIface *tube_chan = GABBLE_TUBE_IFACE (l->data);
      GSList *requests_satisfied_tube;

      requests_satisfied_tube = g_hash_table_lookup (priv->queued_requests,
          tube_chan);
      g_hash_table_steal (priv->queued_requests, tube_chan);
      requests_satisfied_tube = g_slist_reverse (requests_satisfied_tube);

      tp_channel_manager_emit_new_channel (fac,
          TP_EXPORTABLE_CHANNEL (tube_chan), requests_satisfied_tube);

      g_slist_free (requests_satisfied_tube);
    }

  if (tubes_chan == NULL || text_requested)
    {
      /* There is no tubes channel or the text channel has been explicitely
       * requested. In both cases, the text channel has to be announced
       * separately. */

      /* announce text channel */
      tp_channel_manager_emit_new_channel (fac,
          TP_EXPORTABLE_CHANNEL (text_chan), requests_satisfied_text);

      if (tubes_chan != NULL)
        {
          tp_channel_manager_emit_new_channel (fac,
              TP_EXPORTABLE_CHANNEL (tubes_chan), requests_satisfied_tubes);
        }
    }
  else
    {
      /* Announce text and tubes text_chan together */
      GHashTable *channels;

      channels = g_hash_table_new (g_direct_hash, g_direct_equal);
      g_hash_table_insert (channels, text_chan, requests_satisfied_text);
      g_hash_table_insert (channels, tubes_chan, requests_satisfied_tubes);

      tp_channel_manager_emit_new_channels (fac, channels);

      g_hash_table_destroy (channels);
    }

  g_hash_table_remove (priv->tubes_needed_for_tube, tubes_chan);
  g_slist_free (requests_satisfied_text);
  g_slist_free (requests_satisfied_tubes);
}

static void
muc_join_error_cb (GabbleMucChannel *chan,
                   GError *error,
                   gpointer data)
{
  GabbleMucFactory *fac = GABBLE_MUC_FACTORY (data);
  GabbleMucFactoryPrivate *priv = GABBLE_MUC_FACTORY_GET_PRIVATE (fac);
  GabbleTubesChannel *tubes_chan;
  GSList *requests_satisfied;
  GSList *iter;

  DEBUG ("error->code=%u, error->message=\"%s\"", error->code, error->message);

  requests_satisfied = g_slist_reverse (g_hash_table_lookup (
        priv->queued_requests, chan));
  g_hash_table_steal (priv->queued_requests, chan);

  for (iter = requests_satisfied; iter != NULL; iter = iter->next)
    {
      tp_channel_manager_emit_request_failed (fac, iter->data,
          error->domain, error->code, error->message);
    }

  g_slist_free (requests_satisfied);

  tubes_chan = g_hash_table_lookup (priv->text_needed_for_tubes, chan);

  if (tubes_chan != NULL)
    {
      g_hash_table_remove (priv->text_needed_for_tubes, chan);

      requests_satisfied = g_slist_reverse (g_hash_table_lookup (
            priv->queued_requests, tubes_chan));
      g_hash_table_steal (priv->queued_requests, tubes_chan);

      for (iter = requests_satisfied; iter != NULL; iter = iter->next)
        {
          tp_channel_manager_emit_request_failed (fac, iter->data,
              error->domain, error->code, error->message);
        }

      g_slist_free (requests_satisfied);
    }
}

static void
muc_sub_channel_closed_cb (TpSvcChannel *chan,
    gpointer user_data)
{
  GabbleMucFactory *fac = GABBLE_MUC_FACTORY (user_data);

  tp_channel_manager_emit_channel_closed_for_object (fac,
      TP_EXPORTABLE_CHANNEL (chan));
}

static void
muc_channel_new_call (GabbleMucChannel *muc,
    GabbleCallMucChannel *call,
    GSList *requests,
    gpointer user_data)
{
  GabbleMucFactory *fac = GABBLE_MUC_FACTORY (user_data);

  DEBUG ("Emitting new Call channel");

  tp_channel_manager_emit_new_channel (fac,
      TP_EXPORTABLE_CHANNEL (call), requests);

  g_signal_connect (call, "closed",
    G_CALLBACK (muc_sub_channel_closed_cb), fac);
}

static void
muc_channel_new_tube (GabbleMucChannel *channel,
    GabbleTubesChannel *tube,
    gpointer user_data)
{
  GabbleMucFactory *fac = GABBLE_MUC_FACTORY (user_data);
  GabbleMucFactoryPrivate *priv = fac->priv;

  /* If the muc channel is ready announce the tubes channel right away
   * otherwise wait for the text channel to be ready */
  if (_gabble_muc_channel_is_ready (channel))
    tp_channel_manager_emit_new_channel (fac,
      TP_EXPORTABLE_CHANNEL (tube), NULL);
  else
    g_hash_table_insert (priv->text_needed_for_tubes, channel, tube);

  g_signal_connect (tube, "closed",
    G_CALLBACK (muc_sub_channel_closed_cb), fac);
}

/**
 * new_muc_channel
 */
static GabbleMucChannel *
new_muc_channel (GabbleMucFactory *fac,
                 TpHandle handle,
                 gboolean invited,
                 TpHandle inviter,
                 const gchar *message,
                 gboolean requested,
                 GHashTable *initial_channels,
                 GArray *initial_handles,
                 char **initial_ids)
{
  GabbleMucFactoryPrivate *priv = GABBLE_MUC_FACTORY_GET_PRIVATE (fac);
  TpBaseConnection *conn = (TpBaseConnection *) priv->conn;
  GabbleMucChannel *chan;
  char *object_path;
  GPtrArray *initial_channels_array = NULL;

  g_assert (g_hash_table_lookup (priv->text_channels,
        GUINT_TO_POINTER (handle)) == NULL);

  object_path = g_strdup_printf ("%s/MucChannel%u",
      conn->object_path, handle);

  initial_channels_array = g_ptr_array_new ();
  if (initial_channels != NULL)
    {
      GHashTableIter iter;
      gpointer key;

      g_hash_table_iter_init (&iter, initial_channels);
      while (g_hash_table_iter_next (&iter, &key, NULL))
        {
          g_ptr_array_add (initial_channels_array, key);
        }
    }

  if (initial_handles != NULL)
    g_array_ref (initial_handles);
  else
    initial_handles = g_array_new (FALSE, TRUE, sizeof (TpHandle));

  DEBUG ("creating new chan, object path %s", object_path);

  chan = g_object_new (GABBLE_TYPE_MUC_CHANNEL,
       "connection", priv->conn,
       "object-path", object_path,
       "handle", handle,
       "invited", invited,
       "initiator-handle", invited ? inviter : conn->self_handle,
       "invitation-message", message,
       "requested", requested,
       "initial-channels", initial_channels_array,
       "initial-invitee-handles", initial_handles,
       "initial-invitee-ids", initial_ids,
       NULL);

  g_signal_connect (chan, "closed", (GCallback) muc_channel_closed_cb, fac);
  g_signal_connect (chan, "new-tube", (GCallback) muc_channel_new_tube, fac);
  g_signal_connect (chan, "new-call",
      (GCallback) muc_channel_new_call, fac);

  g_hash_table_insert (priv->text_channels, GUINT_TO_POINTER (handle), chan);

  g_free (object_path);
  g_ptr_array_free (initial_channels_array, TRUE);
  g_array_unref (initial_handles);

  if (_gabble_muc_channel_is_ready (chan))
    muc_ready_cb (chan, fac);
  else
    g_signal_connect (chan, "ready", G_CALLBACK (muc_ready_cb), fac);

  g_signal_connect (chan, "join-error", G_CALLBACK (muc_join_error_cb),
                    fac);

  return chan;
}

// tubes_channel_closed_cb

static void
do_invite (GabbleMucFactory *fac,
           const gchar *room,
           TpHandle inviter_handle,
           const gchar *reason)
{
  GabbleMucFactoryPrivate *priv = GABBLE_MUC_FACTORY_GET_PRIVATE (fac);
  TpHandleRepoIface *room_repo = tp_base_connection_get_handles (
      (TpBaseConnection *) priv->conn, TP_HANDLE_TYPE_ROOM);
  TpHandle room_handle;

  room_handle = tp_handle_ensure (room_repo, room, NULL, NULL);

  if (room_handle == 0)
    {
      DEBUG ("got a MUC invitation message with invalid room JID \"%s\"; "
          "ignoring", room);
      return;
    }

  if (g_hash_table_lookup (priv->text_channels,
        GUINT_TO_POINTER (room_handle)) == NULL)
    {
      new_muc_channel (fac, room_handle, TRUE, inviter_handle, reason, FALSE,
          NULL, NULL, NULL);
    }
  else
    {
      DEBUG ("ignoring invite to room \"%s\"; we're already there", room);
    }

  tp_handle_unref (room_repo, room_handle);
}

struct DiscoInviteData {
    GabbleMucFactory *factory;
    gchar *reason;
    TpHandle inviter;
};

/**
 * obsolete_invite_disco_cb:
 *
 * Callback for disco request we fired upon encountering obsolete disco.
 * If the object is in fact MUC room, create a channel for it.
 */
static void
obsolete_invite_disco_cb (GabbleDisco *self,
                          GabbleDiscoRequest *request,
                          const gchar *jid,
                          const gchar *node,
                          LmMessageNode *query_result,
                          GError* error,
                          gpointer user_data)
{
  struct DiscoInviteData *data = (struct DiscoInviteData *) user_data;

  GabbleMucFactory *fac = GABBLE_MUC_FACTORY (data->factory);
  GabbleMucFactoryPrivate *priv = GABBLE_MUC_FACTORY_GET_PRIVATE (fac);
  TpHandleRepoIface *contact_repo = tp_base_connection_get_handles (
      (TpBaseConnection *) priv->conn, TP_HANDLE_TYPE_CONTACT);
  LmMessageNode *identity;
  const char *category = NULL, *type = NULL;

  g_hash_table_remove (priv->disco_requests, request);

  if (error != NULL)
    {
      DEBUG ("ignoring obsolete invite to room '%s'; got disco error: %s",
          jid, error->message);
      goto out;
    }

  identity = lm_message_node_get_child (query_result, "identity");
  if (identity != NULL)
    {
      category = lm_message_node_get_attribute (identity, "category");
      type = lm_message_node_get_attribute (identity, "type");
    }

  if (tp_strdiff (category, "conference") ||
      tp_strdiff (type, "text"))
    {
      DEBUG ("obsolete invite request specified inappropriate jid '%s' "
          "(not a text conference); ignoring request", jid);
      goto out;
    }

  /* OK, it's MUC after all, create a new channel */
  do_invite (fac, jid, data->inviter, data->reason);

out:
  tp_handle_unref (contact_repo, data->inviter);
  g_free (data->reason);
  g_slice_free (struct DiscoInviteData, data);
}

static gboolean
process_muc_invite (GabbleMucFactory *fac,
                    LmMessage *message,
                    const gchar *from,
                    TpChannelTextSendError send_error)
{
  GabbleMucFactoryPrivate *priv = GABBLE_MUC_FACTORY_GET_PRIVATE (fac);
  TpBaseConnection *conn = (TpBaseConnection *) priv->conn;
  TpHandleRepoIface *contact_repo = tp_base_connection_get_handles (conn,
      TP_HANDLE_TYPE_CONTACT);

  LmMessageNode *x_node, *invite_node, *reason_node;
  const gchar *invite_from, *reason = NULL;
  TpHandle inviter_handle;
  gchar *room;

  /* does it have a muc subnode? */
  x_node = lm_message_node_get_child_with_namespace (
      wocky_stanza_get_top_node (message), "x",
      NS_MUC_USER);

  if (x_node == NULL)
    return FALSE;

  /* and an invitation? */
  invite_node = lm_message_node_get_child (x_node, "invite");

  if (invite_node == NULL)
    return FALSE;

  /* FIXME: do something with these? */
  if (send_error != GABBLE_TEXT_CHANNEL_SEND_NO_ERROR)
    {
      STANZA_DEBUG (message, "got a MUC invitation message with a send "
          "error; ignoring");

      return TRUE;
    }

  invite_from = lm_message_node_get_attribute (invite_node, "from");
  if (invite_from == NULL)
    {
      STANZA_DEBUG (message, "got a MUC invitation message with no JID; "
          "ignoring");

      return TRUE;
    }

  inviter_handle = tp_handle_ensure (contact_repo, invite_from,
      NULL, NULL);
  if (inviter_handle == 0)
    {
      STANZA_DEBUG (message, "got a MUC invitation message with invalid "
          "inviter JID; ignoring");

      return TRUE;
    }

  reason_node = lm_message_node_get_child (invite_node, "reason");

  if (reason_node != NULL)
    reason = lm_message_node_get_value (reason_node);

  /* create the channel */
  room = gabble_remove_resource (from);
  do_invite (fac, room, inviter_handle, reason);
  g_free (room);

  tp_handle_unref (contact_repo, inviter_handle);

  return TRUE;
}

static gboolean
process_obsolete_invite (GabbleMucFactory *fac,
                         LmMessage *message,
                         const gchar *from,
                         const gchar *body,
                         TpChannelTextSendError send_error)
{
  GabbleMucFactoryPrivate *priv = GABBLE_MUC_FACTORY_GET_PRIVATE (fac);
  TpBaseConnection *conn = (TpBaseConnection *) priv->conn;
  TpHandleRepoIface *contact_repo = tp_base_connection_get_handles (conn,
      TP_HANDLE_TYPE_CONTACT);

  LmMessageNode *x_node;
  const gchar *room;
  TpHandle inviter_handle;
  GabbleDiscoRequest *request;
  struct DiscoInviteData *disco_udata;

  /* check for obsolete invite method */
  x_node = lm_message_node_get_child_with_namespace (
      wocky_stanza_get_top_node (message), "x", NS_X_CONFERENCE);
  if (x_node == NULL)
    return FALSE;

  /* this can only happen if the user sent an obsolete invite with another
   * client or something */
  if (send_error != GABBLE_TEXT_CHANNEL_SEND_NO_ERROR)
    {
      STANZA_DEBUG (message,
          "got an obsolete MUC invitation message with "
          "a send error; ignoring");

      return TRUE;
    }

  /* the room JID is in x */
  room = lm_message_node_get_attribute (x_node, "jid");
  if (room == NULL)
    {
      STANZA_DEBUG (message,
          "got a obsolete MUC invitation with no room JID; ignoring");

      return TRUE;
    }

  /* the inviter JID is in "from" */
  inviter_handle = tp_handle_ensure (contact_repo, from, NULL, NULL);
  if (inviter_handle == 0)
    {
      STANZA_DEBUG (message, "got an obsolete MUC invitation message from "
          "an invalid JID; ignoring");

      return TRUE;
    }

  disco_udata = g_slice_new0 (struct DiscoInviteData);
  disco_udata->factory = fac;
  disco_udata->reason = g_strdup (body);
  disco_udata->inviter = inviter_handle;

  DEBUG ("received obsolete MUC invite from handle %u (%s), discoing room %s",
      inviter_handle, from, room);

  request = gabble_disco_request (priv->conn->disco, GABBLE_DISCO_TYPE_INFO,
      room, NULL, obsolete_invite_disco_cb, disco_udata, G_OBJECT (fac), NULL);

  if (request != NULL)
    {
      g_hash_table_insert (priv->disco_requests, request, NULL);
    }
  else
    {
      DEBUG ("obsolete MUC invite disco failed, freeing info");

      tp_handle_unref (contact_repo, inviter_handle);
      g_free (disco_udata->reason);
      g_slice_free (struct DiscoInviteData, disco_udata);
    }

  return TRUE;
}

/**
 * muc_factory_message_cb:
 *
 * Called by loudmouth when we get an incoming <message>.
 * We filter only groupchat and MUC messages, ignoring the rest.
 */
static LmHandlerResult
muc_factory_message_cb (LmMessageHandler *handler,
                        LmConnection *connection,
                        LmMessage *message,
                        gpointer user_data)
{
  GabbleMucFactory *fac = GABBLE_MUC_FACTORY (user_data);
  GabbleMucFactoryPrivate *priv = GABBLE_MUC_FACTORY_GET_PRIVATE (fac);

  const gchar *from, *body, *id;
  time_t stamp;
  TpChannelTextMessageType msgtype;
  gint state;
  TpChannelTextSendError send_error;
  TpDeliveryStatus delivery_status;

  if (!gabble_message_util_parse_incoming_message (message, &from, &stamp,
        &msgtype, &id, &body, &state, &send_error, &delivery_status))
    return LM_HANDLER_RESULT_REMOVE_MESSAGE;

  if (conn_olpc_process_activity_properties_message (priv->conn, message,
        from))
    return LM_HANDLER_RESULT_REMOVE_MESSAGE;

  if (conn_olpc_process_activity_uninvite_message (priv->conn, message,
        from))
    return LM_HANDLER_RESULT_REMOVE_MESSAGE;

  if (process_muc_invite (fac, message, from, send_error))
    return LM_HANDLER_RESULT_REMOVE_MESSAGE;

  if (process_obsolete_invite (fac, message, from, body, send_error))
    return LM_HANDLER_RESULT_REMOVE_MESSAGE;

  /* we used to check if a room with the jid exists, instead at this  *
   * point we stop caring: actual MUC messages are handled internally *
   * by the wocky muc implementation                                  */
  return LM_HANDLER_RESULT_ALLOW_MORE_HANDLERS;
}

void
gabble_muc_factory_broadcast_presence (GabbleMucFactory *self)
{
  GabbleMucFactoryPrivate *priv = GABBLE_MUC_FACTORY_GET_PRIVATE (self);
  GHashTableIter iter;
  gpointer channel = NULL;

  g_hash_table_iter_init (&iter, priv->text_channels);

  while (g_hash_table_iter_next (&iter, NULL, &channel))
    {
      g_assert (GABBLE_IS_MUC_CHANNEL (channel));
      gabble_muc_channel_send_presence (GABBLE_MUC_CHANNEL (channel));
    }
}

static void
gabble_muc_factory_associate_request (GabbleMucFactory *self,
                                      gpointer channel,
                                      gpointer request)
{
  GabbleMucFactoryPrivate *priv = GABBLE_MUC_FACTORY_GET_PRIVATE (self);
  GSList *list = g_hash_table_lookup (priv->queued_requests, channel);

  g_assert (TP_IS_EXPORTABLE_CHANNEL (channel));

  g_hash_table_steal (priv->queued_requests, channel);
  list = g_slist_prepend (list, request);
  g_hash_table_insert (priv->queued_requests, channel, list);
}


static gboolean
cancel_queued_requests (gpointer k,
                        gpointer v,
                        gpointer d)
{
  GabbleMucFactory *self = GABBLE_MUC_FACTORY (d);
  GSList *requests_satisfied = v;
  GSList *iter;

  requests_satisfied = g_slist_reverse (requests_satisfied);

  for (iter = requests_satisfied; iter != NULL; iter = iter->next)
    {
      tp_channel_manager_emit_request_failed (self,
          iter->data, TP_ERRORS, TP_ERROR_DISCONNECTED,
          "Unable to complete this channel request, we're disconnecting!");
    }

  g_slist_free (requests_satisfied);

  return TRUE;
}


static void
gabble_muc_factory_close_all (GabbleMucFactory *self)
{
  GabbleMucFactoryPrivate *priv = GABBLE_MUC_FACTORY_GET_PRIVATE (self);

  DEBUG ("closing channels");

  if (priv->status_changed_id != 0)
    {
      g_signal_handler_disconnect (priv->conn,
          priv->status_changed_id);
      priv->status_changed_id = 0;
    }

  if (priv->queued_requests != NULL)
    g_hash_table_foreach_steal (priv->queued_requests,
        cancel_queued_requests, self);

  tp_clear_pointer (&priv->queued_requests, g_hash_table_destroy);
  tp_clear_pointer (&priv->text_needed_for_tubes, g_hash_table_destroy);
  tp_clear_pointer (&priv->tubes_needed_for_tube, g_hash_table_destroy);

  /* Use a temporary variable because we don't want
   * muc_channel_closed_cb or tubes_channel_closed_cb to remove the channel
   * from the hash table a second time */
  if (priv->text_channels != NULL)
    {
      GHashTable *tmp = priv->text_channels;
      GHashTableIter iter;
      gpointer chan;

      priv->text_channels = NULL;

      g_hash_table_iter_init (&iter, tmp);
      while (g_hash_table_iter_next (&iter, NULL, &chan))
        gabble_muc_channel_teardown (GABBLE_MUC_CHANNEL (chan));

      g_hash_table_destroy (tmp);
    }

  if (priv->message_cb != NULL)
    {
      DEBUG ("removing callbacks");

      lm_connection_unregister_message_handler (priv->conn->lmconn,
          priv->message_cb, LM_MESSAGE_TYPE_MESSAGE);
    }

  tp_clear_pointer (&priv->message_cb, lm_message_handler_unref);
}


static void
connection_status_changed_cb (GabbleConnection *conn,
                              guint status,
                              guint reason,
                              GabbleMucFactory *self)
{
  GabbleMucFactoryPrivate *priv = GABBLE_MUC_FACTORY_GET_PRIVATE (self);

  switch (status)
    {
    case TP_CONNECTION_STATUS_CONNECTING:
      DEBUG ("adding callbacks");
      g_assert (priv->message_cb == NULL);

      priv->message_cb = lm_message_handler_new (muc_factory_message_cb,
          self, NULL);
      lm_connection_register_message_handler (priv->conn->lmconn,
          priv->message_cb, LM_MESSAGE_TYPE_MESSAGE,
          LM_HANDLER_PRIORITY_NORMAL);
      break;

    case TP_CONNECTION_STATUS_DISCONNECTED:
      gabble_muc_factory_close_all (self);
      break;
    }
}


static GObject *
gabble_muc_factory_constructor (GType type, guint n_props,
                                GObjectConstructParam *props)
{
  GObject *obj = G_OBJECT_CLASS (gabble_muc_factory_parent_class)->
           constructor (type, n_props, props);
  GabbleMucFactory *self = GABBLE_MUC_FACTORY (obj);
  GabbleMucFactoryPrivate *priv = GABBLE_MUC_FACTORY_GET_PRIVATE (self);

  priv->status_changed_id = g_signal_connect (priv->conn,
      "status-changed", (GCallback) connection_status_changed_cb, obj);

  return obj;
}


struct _ForeachData
{
  TpExportableChannelFunc foreach;
  gpointer user_data;
};

static void
_foreach_slave (gpointer key, gpointer value, gpointer user_data)
{
  struct _ForeachData *data = (struct _ForeachData *) user_data;
  TpExportableChannel *channel = TP_EXPORTABLE_CHANNEL (value);
  GabbleMucChannel *gmuc = GABBLE_MUC_CHANNEL (value);
  GabbleTubesChannel *tube = NULL;

  data->foreach (channel, data->user_data);

  g_object_get (gmuc, "tube", &tube, NULL);

  if (tube != NULL)
    {
      channel = TP_EXPORTABLE_CHANNEL (tube);
      data->foreach (channel, data->user_data);
      gabble_tubes_channel_foreach (tube, data->foreach, data->user_data);
      g_object_unref (tube);
    }

  g_list_foreach (gabble_muc_channel_get_call_channels (gmuc),
      (GFunc) data->foreach, data->user_data);
}

static void
gabble_muc_factory_foreach_channel (TpChannelManager *manager,
                                    TpExportableChannelFunc foreach,
                                    gpointer user_data)
{
  GabbleMucFactory *fac = GABBLE_MUC_FACTORY (manager);
  GabbleMucFactoryPrivate *priv = GABBLE_MUC_FACTORY_GET_PRIVATE (fac);
  struct _ForeachData data;

  data.user_data = user_data;
  data.foreach = foreach;

  g_hash_table_foreach (priv->text_channels, _foreach_slave, &data);
}


/**
 * ensure_muc_channel:
 *
 * Create a MUC channel in response to RequestChannel.
 *
 * Return TRUE if it already existed, or return FALSE
 * if it needed to be created (so isn't ready yet).
 */
static gboolean
ensure_muc_channel (GabbleMucFactory *fac,
                    GabbleMucFactoryPrivate *priv,
                    TpHandle handle,
                    GabbleMucChannel **ret,
                    gboolean requested,
                    GHashTable *initial_channels,
                    GArray *initial_handles,
                    char **initial_ids)
{
  TpBaseConnection *base_conn = (TpBaseConnection *) priv->conn;

  *ret = g_hash_table_lookup (priv->text_channels, GUINT_TO_POINTER (handle));

  if (*ret == NULL)
    {
      *ret = new_muc_channel (fac, handle, FALSE, base_conn->self_handle, NULL,
          requested, initial_channels, initial_handles, initial_ids);
      return FALSE;
    }

  if (_gabble_muc_channel_is_ready (*ret))
    return TRUE;
  else
    return FALSE;
}


void
gabble_muc_factory_handle_si_stream_request (GabbleMucFactory *self,
                                             GabbleBytestreamIface *bytestream,
                                             TpHandle room_handle,
                                             const gchar *stream_id,
                                             LmMessage *msg)
{
  GabbleMucFactoryPrivate *priv = GABBLE_MUC_FACTORY_GET_PRIVATE (self);
  TpHandleRepoIface *room_repo = tp_base_connection_get_handles (
     (TpBaseConnection *) priv->conn, TP_HANDLE_TYPE_ROOM);
  GabbleMucChannel *gmuc = NULL;
  GabbleTubesChannel *tube = NULL;

  g_return_if_fail (tp_handle_is_valid (room_repo, room_handle, NULL));

  gmuc = g_hash_table_lookup (priv->text_channels,
      GUINT_TO_POINTER (room_handle));
  g_object_get (gmuc, "tube", &tube, NULL);

  if (tube == NULL)
    {
      GError e = { GABBLE_XMPP_ERROR, XMPP_ERROR_BAD_REQUEST,
          "No tubes channel available for this MUC" };

      DEBUG ("tubes channel doesn't exist for muc %d", room_handle);
      gabble_bytestream_iface_close (bytestream, &e);
      return;
    }

  gabble_tubes_channel_bytestream_offered (tube, bytestream, msg);
  g_object_unref (tube);
}

GabbleMucChannel *
gabble_muc_factory_find_text_channel (GabbleMucFactory *self,
                                      TpHandle handle)
{
  GabbleMucFactoryPrivate *priv = GABBLE_MUC_FACTORY_GET_PRIVATE (self);

  return g_hash_table_lookup (priv->text_channels, GUINT_TO_POINTER (handle));
}


static const gchar * const muc_channel_fixed_properties[] = {
    TP_IFACE_CHANNEL ".ChannelType",
    TP_IFACE_CHANNEL ".TargetHandleType",
    NULL
};

static const gchar * const * muc_tubes_channel_fixed_properties =
    muc_channel_fixed_properties;

static const gchar * const muc_channel_allowed_properties[] = {
    TP_IFACE_CHANNEL ".TargetHandle",
    TP_IFACE_CHANNEL ".TargetID",
    TP_IFACE_CHANNEL_INTERFACE_CONFERENCE ".InitialChannels",
    TP_IFACE_CHANNEL_INTERFACE_CONFERENCE ".InitialInviteeHandles",
    TP_IFACE_CHANNEL_INTERFACE_CONFERENCE ".InitialInviteeIDs",
    TP_IFACE_CHANNEL_INTERFACE_CONFERENCE ".InvitationMessage",
    NULL
};

static const gchar * const muc_tubes_channel_allowed_properties[] = {
    TP_IFACE_CHANNEL ".TargetHandle",
    TP_IFACE_CHANNEL ".TargetID",
    NULL
};

static void
gabble_muc_factory_type_foreach_channel_class (GType type,
    TpChannelManagerTypeChannelClassFunc func,
    gpointer user_data)
{
  GHashTable *table = g_hash_table_new_full (g_str_hash, g_str_equal,
      NULL, (GDestroyNotify) tp_g_value_slice_free);
  GValue *channel_type_value, *handle_type_value;

  channel_type_value = tp_g_value_slice_new (G_TYPE_STRING);
  /* no string value yet - we'll change it for each channel class */
  g_hash_table_insert (table, TP_IFACE_CHANNEL ".ChannelType",
      channel_type_value);

  handle_type_value = tp_g_value_slice_new (G_TYPE_UINT);
  g_value_set_uint (handle_type_value, TP_HANDLE_TYPE_ROOM);
  g_hash_table_insert (table, TP_IFACE_CHANNEL ".TargetHandleType",
      handle_type_value);

  /* Channel.Type.Text */
  g_value_set_static_string (channel_type_value, TP_IFACE_CHANNEL_TYPE_TEXT);
  func (type, table, muc_channel_allowed_properties,
      user_data);

  /* Channel.Type.Tubes */
  g_value_set_static_string (channel_type_value, TP_IFACE_CHANNEL_TYPE_TUBES);
  func (type, table, muc_tubes_channel_allowed_properties,
      user_data);

  /* Muc Channel.Type.StreamTube */
  g_value_set_static_string (channel_type_value,
      TP_IFACE_CHANNEL_TYPE_STREAM_TUBE);
  func (type, table, gabble_tube_stream_channel_get_allowed_properties (),
      user_data);

  /* Muc Channel.Type.DBusTube */
  g_value_set_static_string (channel_type_value,
      TP_IFACE_CHANNEL_TYPE_DBUS_TUBE);
  func (type, table, gabble_tube_dbus_channel_get_allowed_properties (),
      user_data);

  /* Muc Channel.Type.Call */
  g_value_set_static_string (channel_type_value,
      TPY_IFACE_CHANNEL_TYPE_CALL);
  func (type, table,
      gabble_media_factory_call_channel_allowed_properties (),
      user_data);

  g_hash_table_destroy (table);
}

/* return TRUE if the text_channel associated is ready */
static gboolean
ensure_tubes_channel (GabbleMucFactory *self,
                      TpHandle handle,
                      GabbleTubesChannel **tubes_chan,
                      gboolean requested)
{
  GabbleMucFactoryPrivate *priv = GABBLE_MUC_FACTORY_GET_PRIVATE (self);
  TpBaseConnection *base_conn = (TpBaseConnection *) priv->conn;
  GabbleMucChannel *text_chan;
  TpHandle initiator = base_conn->self_handle;
  gboolean result;

  result = ensure_muc_channel (self, priv, handle, &text_chan, FALSE,
      NULL, NULL, NULL);

  /* this refs the tube channel object */
  *tubes_chan = gabble_muc_channel_open_tube (text_chan, initiator, requested);

  if (!result)
    g_hash_table_insert (priv->text_needed_for_tubes, text_chan, *tubes_chan);

  return result;
}

static gboolean
handle_text_channel_request (GabbleMucFactory *self,
                            gpointer request_token,
                            GHashTable *request_properties,
                            gboolean require_new,
                            TpHandle room,
                            GError **error)
{
  GabbleMucFactoryPrivate *priv = GABBLE_MUC_FACTORY_GET_PRIVATE (self);
  TpBaseConnection *conn = TP_BASE_CONNECTION (priv->conn);
  GabbleMucChannel *text_chan;
  TpHandleSet *handles;
  TpIntSet *continue_handles;
  guint i;
  gboolean ret = TRUE;

  TpHandleRepoIface *contact_handles = tp_base_connection_get_handles (conn,
      TP_HANDLE_TYPE_CONTACT);
  TpHandleRepoIface *room_handles = tp_base_connection_get_handles (conn,
      TP_HANDLE_TYPE_ROOM);

  GPtrArray *initial_channels;
  GHashTable *final_channels; /* used as a set: (char *) -> NULL */
  GArray *initial_handles, *final_handles;
  char **initial_ids, **final_ids;
  const char *invite_msg;

  if (tp_channel_manager_asv_has_unknown_properties (request_properties,
          muc_channel_fixed_properties, muc_channel_allowed_properties,
          error))
    return FALSE;

  initial_channels = tp_asv_get_boxed (request_properties,
      TP_IFACE_CHANNEL_INTERFACE_CONFERENCE ".InitialChannels",
      TP_ARRAY_TYPE_OBJECT_PATH_LIST);
  initial_handles = tp_asv_get_boxed (request_properties,
      TP_IFACE_CHANNEL_INTERFACE_CONFERENCE ".InitialInviteeHandles",
      DBUS_TYPE_G_UINT_ARRAY);
  initial_ids = tp_asv_get_boxed (request_properties,
      TP_IFACE_CHANNEL_INTERFACE_CONFERENCE ".InitialInviteeIDs",
      G_TYPE_STRV);
  invite_msg = tp_asv_get_string (request_properties,
      TP_IFACE_CHANNEL_INTERFACE_CONFERENCE ".InvitationMessage");

  handles = tp_handle_set_new (contact_handles);
  continue_handles = tp_intset_new ();
  final_channels = g_hash_table_new (g_str_hash, g_str_equal);

  /* look at the list of initial channels, build a set of handles to invite */
  if (initial_channels != NULL)
    {
      TpDBusDaemon *dbus_daemon = tp_base_connection_get_dbus_daemon (conn);
      DBusGConnection *bus = tp_proxy_get_dbus_connection (dbus_daemon);

      for (i = 0; i < initial_channels->len; i++)
        {
          const char *object_path = g_ptr_array_index (initial_channels, i);
          GObject *object;
          TpHandle handle;
          GabbleConnection *connection;

          object = dbus_g_connection_lookup_g_object (bus, object_path);

          if (!GABBLE_IS_IM_CHANNEL (object))
            {
              DEBUG ("Channel %s is not an ImChannel, ignoring",
                  object_path);
              continue;
            }

          g_object_get (object,
              "connection", &connection,
              "handle", &handle,
              NULL);
          g_object_unref (connection); /* drop the ref immediately */

          if (connection != priv->conn)
            {
              DEBUG ("Channel %s is from a different Connection, ignoring",
                  object_path);
              continue;
            }

          tp_handle_set_add (handles, handle);
          tp_intset_add (continue_handles, handle);
          g_hash_table_insert (final_channels, (char *) object_path, NULL);
        }
    }

  /* look at the list of initial handles, add these to the handles set */
  if (initial_handles != NULL)
    {
      for (i = 0; i < initial_handles->len; i++)
        {
          TpHandle handle = g_array_index (initial_handles, TpHandle, i);

          if (tp_handle_inspect (contact_handles, handle) == NULL)
            {
              DEBUG ("Bad Handle %u, ignoring", handle);
              continue;
            }

          tp_handle_set_add (handles, handle);
        }
    }

  /* look at the list of initial ids, add these to the handles set */
  if (initial_ids != NULL)
    {
      char **ptr;

      for (ptr = initial_ids; *ptr != NULL; ptr++)
        {
          char *id = *ptr;
          TpHandle handle = tp_handle_ensure (contact_handles, id, NULL, NULL);

          if (handle == 0)
            {
              DEBUG ("Bad ID '%s', ignoring", id);
              continue;
            }

          tp_handle_set_add (handles, handle);
          tp_handle_unref (contact_handles, handle);
        }
    }

  /* build new InitialInviteeHandles and InitialInviteeIDs */
  /* FIXME: include Self Handle to comply with spec ? */
  final_handles = tp_handle_set_to_array (handles);
  final_ids = g_new0 (char *, final_handles->len + 1);

  for (i = 0; i < final_handles->len; i++)
    {
      TpHandle handle = g_array_index (final_handles, TpHandle, i);
      const char *id = tp_handle_inspect (contact_handles, handle);

      final_ids[i] = (char *) id;
    }

  if (room == 0)
    {
      char *uuid, *id, *server = "";

      /* There's no super obvious way to tell.. you can't invite GMail users to
       * a non-Google MUC (it just doesn't work), and if your own account is on
       * a Google server, you may as well use a Google PMUC. If one of your
       * initial contacts is using GMail, you should also use a Google PMUC */
      for (i = 0; i < final_handles->len; i++)
        {
          TpHandle handle = g_array_index (final_handles, TpHandle, i);
          GabblePresence *presence;

          presence = gabble_presence_cache_get (priv->conn->presence_cache,
              handle);

          if (presence != NULL &&
              gabble_presence_has_cap (presence, QUIRK_GOOGLE_WEBMAIL_CLIENT))
            {
              DEBUG ("Initial invitee includes Google Webmail client");

              server = "@groupchat.google.com";
              break;
            }
        }

      uuid = gabble_generate_id ();
      id = g_strdup_printf ("private-chat-%s%s", uuid, server);

      DEBUG ("Creating PMUC '%s'", id);

      room = tp_handle_ensure (room_handles, id, NULL, error);

      g_free (uuid);
      g_free (id);

      if (room == 0)
        {
          ret = FALSE;
          goto out;
        }
    }
  else
    {
      /* ref room here so we can unref it again, below */
      tp_handle_ref (room_handles, room);
    }

  if (ensure_muc_channel (self, priv, room, &text_chan, TRUE,
        final_channels, final_handles, final_ids))
    {
      /* channel exists */

      if (require_new)
        {
          g_set_error (error, TP_ERRORS, TP_ERROR_NOT_AVAILABLE,
              "That channel has already been created (or requested)");
          ret = FALSE;
        }
      else
        {
          if (initial_channels != NULL ||
              initial_handles != NULL ||
              initial_ids != NULL)
            {
              g_set_error (error, TP_ERRORS, TP_ERROR_INVALID_ARGUMENT,
                  "Cannot set InitialChannels, InitialInviteeHandles or "
                  "InitialInviteIDs for existing channel");
              ret = FALSE;
            }
          else
            {
              tp_channel_manager_emit_request_already_satisfied (self,
                  request_token, TP_EXPORTABLE_CHANNEL (text_chan));
              ret = TRUE;
            }
        }

      goto out;
    }
  else
    {
      gabble_muc_factory_associate_request (self, text_chan,
          request_token);
    }

  /* invite all of the invitees to this new MUC */
  /* members included in an InitialChannel will want the <continue/> node set */
  for (i = 0; i < final_handles->len; i++)
    {
      TpHandle handle = g_array_index (final_handles, TpHandle, i);
      char *id = final_ids[i];

      GError *error2 = NULL;
      gboolean continue_;

      continue_ = tp_intset_is_member (continue_handles, handle);

      /* N.B. contrary to what Google's own spec implies, an invite message
       * will not be handled correctly by the GMail client. We're going to
       * have to strip it out of invites to GMail clients */
      gabble_muc_channel_send_invite (text_chan, id, invite_msg,
          continue_, &error2);
      if (error2 != NULL)
        {
          DEBUG ("%s", error2->message);
          g_error_free (error2);
          continue;
        }
    }

out:
  if (room != 0)
    tp_handle_unref (room_handles, room);

  g_hash_table_destroy (final_channels);
  g_array_free (final_handles, TRUE);
  g_free (final_ids);

  tp_handle_set_destroy (handles);
  tp_intset_destroy (continue_handles);

  return ret;
}

static gboolean
handle_tubes_channel_request (GabbleMucFactory *self,
                              gpointer request_token,
                              GHashTable *request_properties,
                              gboolean require_new,
                              TpHandle handle,
                              GError **error)
{
  GabbleMucFactoryPrivate *priv = GABBLE_MUC_FACTORY_GET_PRIVATE (self);
  GabbleTubesChannel *tube = NULL;
  GabbleMucChannel *gmuc = NULL;

  if (tp_channel_manager_asv_has_unknown_properties (request_properties,
          muc_tubes_channel_fixed_properties,
          muc_tubes_channel_allowed_properties,
          error))
    return FALSE;

  gmuc = g_hash_table_lookup (priv->text_channels, GUINT_TO_POINTER (handle));

  if (gmuc != NULL)
    g_object_get (gmuc, "tube", &tube, NULL);

  if (tube != NULL)
    {
      if (require_new)
        {
          g_set_error (error, TP_ERRORS, TP_ERROR_NOT_AVAILABLE,
              "That channel has already been created (or requested)");
          return FALSE;
        }
      else
        {
          tp_channel_manager_emit_request_already_satisfied (self,
              request_token, TP_EXPORTABLE_CHANNEL (tube));
        }
    }
  else if (ensure_tubes_channel (self, handle, &tube, TRUE))
    {
      GSList *list = NULL;

      list = g_slist_prepend (list, request_token);
      tp_channel_manager_emit_new_channel (self,
          TP_EXPORTABLE_CHANNEL (tube), list);
      g_slist_free (list);
    }
  else
    {
      gabble_muc_factory_associate_request (self, tube, request_token);
    }

  g_object_unref (tube);

  return TRUE;
}

static gboolean
handle_tube_channel_request (GabbleMucFactory *self,
                             gpointer request_token,
                             GHashTable *request_properties,
                             gboolean require_new,
                             TpHandle handle,
                             GError **error)

{
  GabbleMucFactoryPrivate *priv = GABBLE_MUC_FACTORY_GET_PRIVATE (self);
  gboolean can_announce_now = TRUE;
  gboolean tubes_channel_created = FALSE;
  GabbleTubesChannel *tube = NULL;
  GabbleMucChannel * gmuc;
  GabbleTubeIface *new_channel;

  gmuc = g_hash_table_lookup (priv->text_channels, GUINT_TO_POINTER (handle));

  if (gmuc != NULL)
    g_object_get (gmuc, "tube", &tube, NULL);

  if (tube == NULL)
    {
      /* Need to create a tubes channel */
      if (!ensure_tubes_channel (self, handle, &tube, FALSE))
      {
        /* We have to wait the tubes channel before announcing */
        can_announce_now = FALSE;
      }

      tubes_channel_created = TRUE;
    }

  g_assert (tube != NULL);

  new_channel = gabble_tubes_channel_tube_request (tube,
      request_token, request_properties, TRUE);
  g_assert (new_channel != NULL);

  if (can_announce_now)
    {
      GHashTable *channels;
      GSList *request_tokens;

      channels = g_hash_table_new_full (g_direct_hash, g_direct_equal,
        NULL, NULL);

      if (tubes_channel_created)
        g_hash_table_insert (channels, tube, NULL);

      request_tokens = g_slist_prepend (NULL, request_token);

      g_hash_table_insert (channels, new_channel, request_tokens);
      tp_channel_manager_emit_new_channels (self, channels);

      g_hash_table_destroy (channels);
      g_slist_free (request_tokens);
    }
  else
    {
      GSList *l;

      l = g_hash_table_lookup (priv->tubes_needed_for_tube, tube);
      g_hash_table_steal (priv->tubes_needed_for_tube, tube);

      l = g_slist_prepend (l, new_channel);
      g_hash_table_insert (priv->tubes_needed_for_tube, tube, l);

      /* And now finally associate the new stream or dbus tube channel with
       * the request token so that when the muc channel is ready, the request
       * will be satisfied. */
      gabble_muc_factory_associate_request (self, new_channel, request_token);
    }

  g_object_unref (tube);

  return TRUE;
}

static gboolean
handle_stream_tube_channel_request (GabbleMucFactory *self,
                                    gpointer request_token,
                                    GHashTable *request_properties,
                                    gboolean require_new,
                                    TpHandle handle,
                                    GError **error)
{
  const gchar *service;

  if (tp_channel_manager_asv_has_unknown_properties (request_properties,
          muc_tubes_channel_fixed_properties,
          gabble_tube_stream_channel_get_allowed_properties (),
          error))
    return FALSE;

  /* "Service" is a mandatory, not-fixed property */
  service = tp_asv_get_string (request_properties,
            TP_IFACE_CHANNEL_TYPE_STREAM_TUBE ".Service");
  if (service == NULL)
    {
      g_set_error (error, TP_ERRORS, TP_ERROR_NOT_IMPLEMENTED,
          "Request does not contain the mandatory property '%s'",
          TP_IFACE_CHANNEL_TYPE_STREAM_TUBE ".Service");
      return FALSE;
    }

  return handle_tube_channel_request (self, request_token, request_properties,
      require_new, handle, error);
}

static gboolean
handle_dbus_tube_channel_request (GabbleMucFactory *self,
                                  gpointer request_token,
                                  GHashTable *request_properties,
                                  gboolean require_new,
                                  TpHandle handle,
                                  GError **error)
{
  const gchar *service;

  if (tp_channel_manager_asv_has_unknown_properties (request_properties,
          muc_tubes_channel_fixed_properties,
          gabble_tube_dbus_channel_get_allowed_properties (),
          error))
    return FALSE;

  /* "ServiceName" is a mandatory, not-fixed property */
  service = tp_asv_get_string (request_properties,
      TP_IFACE_CHANNEL_TYPE_DBUS_TUBE ".ServiceName");
  if (service == NULL)
    {
      g_set_error (error, TP_ERRORS, TP_ERROR_NOT_IMPLEMENTED,
          "Request does not contain the mandatory property '%s'",
          TP_IFACE_CHANNEL_TYPE_DBUS_TUBE ".ServiceName");
      return FALSE;
    }

  return handle_tube_channel_request (self, request_token, request_properties,
      require_new, handle, error);
}

static void
call_muc_channel_request_cb (GObject *source,
  GAsyncResult *result,
  gpointer user_data)
{
  Request *r = user_data;
  GabbleMucFactory *self = GABBLE_MUC_FACTORY (r->self);
  GabbleMucChannel *channel = GABBLE_MUC_CHANNEL (source);
  gpointer request_token = r->token;
  GError *error = NULL;

  if (!gabble_muc_channel_request_call_finish (channel,
      result, &error))
    {
      tp_channel_manager_emit_request_failed (self,
        request_token, error->domain, error->code, error->message);
      g_error_free (error);
    }

  /* No need to handle a successful request, this is handled when the muc
   * signals a new call channel automagically */

  g_object_unref (r->self);
  g_slice_free (Request, r);
}

static gboolean
handle_call_channel_request (GabbleMucFactory *self,
    gpointer request_token,
    GHashTable *request_properties,
    gboolean require_new,
    TpHandle handle,
    GError **error)
{
  GabbleMucFactoryPrivate *priv = GABBLE_MUC_FACTORY_GET_PRIVATE (self);
  gboolean initial_audio, initial_video;
  GabbleMucChannel *muc;
  GabbleCallMucChannel *call;
  Request *r;

  if (tp_channel_manager_asv_has_unknown_properties (request_properties,
          muc_channel_fixed_properties,
          gabble_media_factory_call_channel_allowed_properties (),
          error))
    return FALSE;

  initial_audio = tp_asv_get_boolean (request_properties,
      TPY_PROP_CHANNEL_TYPE_CALL_INITIAL_AUDIO, NULL);
  initial_video = tp_asv_get_boolean (request_properties,
      TPY_PROP_CHANNEL_TYPE_CALL_INITIAL_VIDEO, NULL);

  if (!initial_audio && !initial_video)
    {
      g_set_error (error, TP_ERRORS, TP_ERROR_NOT_IMPLEMENTED,
          "Request didn't set either InitialAudio or InitialVideo");
      return FALSE;
    }

  ensure_muc_channel (self, priv, handle, &muc, FALSE, NULL, NULL, NULL);

  call = gabble_muc_channel_get_call (muc);

  if (call != NULL)
    {
      if (require_new)
        {
          g_set_error (error, TP_ERRORS, TP_ERROR_NOT_AVAILABLE,
            "There is already a call in this muc");
          goto error;
        }
      else
        {
          tp_channel_manager_emit_request_already_satisfied (self,
            request_token,
            TP_EXPORTABLE_CHANNEL (call));
          goto out;
        }
    }

  /* FIXME not coping properly with deinitialisation */
  r = g_slice_new (Request);
  r->self = g_object_ref (self);
  r->token = request_token;

  gabble_muc_channel_request_call (muc,
      request_properties,
      require_new,
      request_token,
      call_muc_channel_request_cb,
      r);
out:
  return TRUE;

error:
  return FALSE;
}


static gboolean
gabble_muc_factory_request (GabbleMucFactory *self,
                            gpointer request_token,
                            GHashTable *request_properties,
                            gboolean require_new)
{
  GError *error = NULL;
  TpHandleType handle_type;
  TpHandle handle;
  gboolean conference;
  const gchar *channel_type;

  handle_type = tp_asv_get_uint32 (request_properties,
      TP_IFACE_CHANNEL ".TargetHandleType", NULL);
  channel_type = tp_asv_get_string (request_properties,
      TP_IFACE_CHANNEL ".ChannelType");

  /* Conference channels can be anonymous (HandleTypeNone) */
  conference = (handle_type == TP_HANDLE_TYPE_NONE &&
      !tp_strdiff (channel_type, TP_IFACE_CHANNEL_TYPE_TEXT) &&
      (g_hash_table_lookup (request_properties,
         TP_IFACE_CHANNEL_INTERFACE_CONFERENCE ".InitialChannels") ||
       g_hash_table_lookup (request_properties,
         TP_IFACE_CHANNEL_INTERFACE_CONFERENCE ".InitialInviteeHandles") ||
       g_hash_table_lookup (request_properties,
         TP_IFACE_CHANNEL_INTERFACE_CONFERENCE ".InitialInviteeIDs")));

  /* the channel must either be a room, or a new conference */
  if (handle_type != TP_HANDLE_TYPE_ROOM && !conference)
    return FALSE;

  if (tp_strdiff (channel_type, TP_IFACE_CHANNEL_TYPE_TEXT) &&
      tp_strdiff (channel_type, TP_IFACE_CHANNEL_TYPE_TUBES) &&
      tp_strdiff (channel_type, TP_IFACE_CHANNEL_TYPE_STREAM_TUBE) &&
      tp_strdiff (channel_type, TP_IFACE_CHANNEL_TYPE_DBUS_TUBE) &&
      tp_strdiff (channel_type, TPY_IFACE_CHANNEL_TYPE_CALL))
    return FALSE;

  /* validity already checked by TpBaseConnection */
  handle = tp_asv_get_uint32 (request_properties,
      TP_IFACE_CHANNEL ".TargetHandle", NULL);
  g_assert (conference || handle != 0);

  if (!tp_strdiff (channel_type, TP_IFACE_CHANNEL_TYPE_TEXT))
    {
      if (handle_text_channel_request (self, request_token,
          request_properties, require_new, handle, &error))
        return TRUE;
    }
  else if (!tp_strdiff (channel_type, TP_IFACE_CHANNEL_TYPE_TUBES))
    {
      if (handle_tubes_channel_request (self, request_token,
          request_properties, require_new, handle, &error))
        return TRUE;
    }
  else if (!tp_strdiff (channel_type, TP_IFACE_CHANNEL_TYPE_STREAM_TUBE))
    {
      if (handle_stream_tube_channel_request (self, request_token,
          request_properties, require_new, handle, &error))
        return TRUE;
    }
  else if (!tp_strdiff (channel_type, TP_IFACE_CHANNEL_TYPE_DBUS_TUBE))
    {
      if (handle_dbus_tube_channel_request (self, request_token,
          request_properties, require_new, handle, &error))
        return TRUE;
    }
  else if (!tp_strdiff (channel_type, TPY_IFACE_CHANNEL_TYPE_CALL))
    {
      if (handle_call_channel_request (self, request_token,
          request_properties, require_new, handle, &error))
        return TRUE;
    }
  else
    {
      g_assert_not_reached ();
    }

  /* Something failed */
  tp_channel_manager_emit_request_failed (self, request_token,
      error->domain, error->code, error->message);
  g_error_free (error);
  return TRUE;
}


static gboolean
gabble_muc_factory_create_channel (TpChannelManager *manager,
                                   gpointer request_token,
                                   GHashTable *request_properties)
{
  GabbleMucFactory *self = GABBLE_MUC_FACTORY (manager);

  return gabble_muc_factory_request (self, request_token, request_properties,
      TRUE);
}


static gboolean
gabble_muc_factory_request_channel (TpChannelManager *manager,
                                    gpointer request_token,
                                    GHashTable *request_properties)
{
  GabbleMucFactory *self = GABBLE_MUC_FACTORY (manager);

  return gabble_muc_factory_request (self, request_token, request_properties,
      FALSE);
}


static gboolean
gabble_muc_factory_ensure_channel (TpChannelManager *manager,
                                    gpointer request_token,
                                    GHashTable *request_properties)
{
  GabbleMucFactory *self = GABBLE_MUC_FACTORY (manager);

  return gabble_muc_factory_request (self, request_token, request_properties,
      FALSE);
}

gboolean
gabble_muc_factory_handle_jingle_session (GabbleMucFactory *self,
  GabbleJingleSession *session)
{
  GabbleMucFactoryPrivate *priv = GABBLE_MUC_FACTORY_GET_PRIVATE (self);
  TpHandleRepoIface *room_repo = tp_base_connection_get_handles (
     (TpBaseConnection *) priv->conn, TP_HANDLE_TYPE_ROOM);
  TpHandle room;

  room = gabble_get_room_handle_from_jid (room_repo,
    gabble_jingle_session_get_peer_jid (session));

  if (room != 0)
    {
      GabbleMucChannel *channel;

      channel = g_hash_table_lookup (priv->text_channels,
        GUINT_TO_POINTER (room));
      g_assert (GABBLE_IS_MUC_CHANNEL (channel));

      if (channel != NULL)
        return gabble_muc_channel_handle_jingle_session (channel, session);
    }

  return FALSE;
}


static void
channel_manager_iface_init (gpointer g_iface,
                            gpointer iface_data)
{
  TpChannelManagerIface *iface = g_iface;

  iface->foreach_channel = gabble_muc_factory_foreach_channel;
  iface->type_foreach_channel_class =
      gabble_muc_factory_type_foreach_channel_class;
  iface->request_channel = gabble_muc_factory_request_channel;
  iface->create_channel = gabble_muc_factory_create_channel;
  iface->ensure_channel = gabble_muc_factory_ensure_channel;
}
