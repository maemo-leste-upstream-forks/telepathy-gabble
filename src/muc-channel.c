/*
 * gabble-muc-channel.c - Source for GabbleMucChannel
 * Copyright (C) 2006 Collabora Ltd.
 * Copyright (C) 2006 Nokia Corporation
 *   @author Ole Andre Vadla Ravnaas <ole.andre.ravnaas@collabora.co.uk>
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
#include "muc-channel.h"

#include <stdio.h>
#include <string.h>

#include <dbus/dbus-glib.h>
#include <telepathy-glib/dbus.h>
#include <telepathy-glib/debug-ansi.h>
#include <telepathy-glib/errors.h>
#include <telepathy-glib/exportable-channel.h>
#include <telepathy-glib/gtypes.h>
#include <telepathy-glib/interfaces.h>
#include <telepathy-glib/channel-iface.h>

#define DEBUG_FLAG GABBLE_DEBUG_MUC
#include "connection.h"
#include "conn-aliasing.h"
#include "debug.h"
#include "disco.h"
#include "error.h"
#include "message-util.h"
#include "namespaces.h"
#include "presence.h"
#include "util.h"
#include "presence-cache.h"

#define DEFAULT_JOIN_TIMEOUT 180
#define MAX_NICK_RETRIES 3

#define PROPS_POLL_INTERVAL_LOW  60 * 5
#define PROPS_POLL_INTERVAL_HIGH 60

static void channel_iface_init (gpointer, gpointer);
static void password_iface_init (gpointer, gpointer);
static void chat_state_iface_init (gpointer, gpointer);

G_DEFINE_TYPE_WITH_CODE (GabbleMucChannel, gabble_muc_channel,
    G_TYPE_OBJECT,
    G_IMPLEMENT_INTERFACE (TP_TYPE_SVC_DBUS_PROPERTIES,
      tp_dbus_properties_mixin_iface_init);
    G_IMPLEMENT_INTERFACE (TP_TYPE_SVC_CHANNEL,
      channel_iface_init);
    G_IMPLEMENT_INTERFACE (TP_TYPE_SVC_PROPERTIES_INTERFACE,
      tp_properties_mixin_iface_init);
    G_IMPLEMENT_INTERFACE (TP_TYPE_SVC_CHANNEL_INTERFACE_GROUP,
      tp_group_mixin_iface_init);
    G_IMPLEMENT_INTERFACE (TP_TYPE_SVC_CHANNEL_INTERFACE_PASSWORD,
      password_iface_init);
    G_IMPLEMENT_INTERFACE (TP_TYPE_SVC_CHANNEL_TYPE_TEXT,
      tp_message_mixin_text_iface_init);
    G_IMPLEMENT_INTERFACE (TP_TYPE_SVC_CHANNEL_INTERFACE_MESSAGES,
      tp_message_mixin_messages_iface_init);
    G_IMPLEMENT_INTERFACE (TP_TYPE_EXPORTABLE_CHANNEL, NULL);
    G_IMPLEMENT_INTERFACE (TP_TYPE_CHANNEL_IFACE, NULL);
    G_IMPLEMENT_INTERFACE (TP_TYPE_SVC_CHANNEL_INTERFACE_CHAT_STATE,
      chat_state_iface_init)
    )

static void gabble_muc_channel_send (GObject *obj, TpMessage *message,
    TpMessageSendingFlags flags);

static const gchar *gabble_muc_channel_interfaces[] = {
    TP_IFACE_CHANNEL_INTERFACE_GROUP,
    TP_IFACE_CHANNEL_INTERFACE_PASSWORD,
    TP_IFACE_PROPERTIES_INTERFACE,
    TP_IFACE_CHANNEL_INTERFACE_CHAT_STATE,
    TP_IFACE_CHANNEL_INTERFACE_MESSAGES,
    NULL
};

/* signal enum */
enum
{
    READY,
    JOIN_ERROR,
    PRE_INVITE,
    CONTACT_JOIN,
    PRE_PRESENCE,
    LAST_SIGNAL
};

static guint signals[LAST_SIGNAL] = {0};

/* properties */
enum
{
  PROP_OBJECT_PATH = 1,
  PROP_CHANNEL_TYPE,
  PROP_HANDLE_TYPE,
  PROP_HANDLE,
  PROP_TARGET_ID,
  PROP_REQUESTED,
  PROP_CONNECTION,
  PROP_STATE,
  PROP_INVITED,
  PROP_INVITATION_MESSAGE,
  PROP_INTERFACES,
  PROP_INITIATOR_HANDLE,
  PROP_INITIATOR_ID,
  PROP_CHANNEL_DESTROYED,
  PROP_CHANNEL_PROPERTIES,
  PROP_SELF_JID,
  LAST_PROPERTY
};

#ifdef ENABLE_DEBUG
static const gchar *muc_states[] =
{
  "MUC_STATE_CREATED",
  "MUC_STATE_INITIATED",
  "MUC_STATE_AUTH",
  "MUC_STATE_JOINED",
  "MUC_STATE_ENDED",
};
#endif

/* role and affiliation enums */
typedef enum {
    ROLE_NONE = 0,
    ROLE_VISITOR,
    ROLE_PARTICIPANT,
    ROLE_MODERATOR,

    NUM_ROLES,

    INVALID_ROLE,
} GabbleMucRole;

typedef enum {
    AFFILIATION_NONE = 0,
    AFFILIATION_MEMBER,
    AFFILIATION_ADMIN,
    AFFILIATION_OWNER,

    NUM_AFFILIATIONS,

    INVALID_AFFILIATION,
} GabbleMucAffiliation;

static const gchar *muc_roles[NUM_ROLES] =
{
  "none",
  "visitor",
  "participant",
  "moderator",
};

static const gchar *muc_affiliations[NUM_AFFILIATIONS] =
{
  "none",
  "member",
  "admin",
  "owner",
};

/* room properties */
enum
{
  ROOM_PROP_ANONYMOUS = 0,
  ROOM_PROP_INVITE_ONLY,
  ROOM_PROP_INVITE_RESTRICTED,
  ROOM_PROP_MODERATED,
  ROOM_PROP_NAME,
  ROOM_PROP_DESCRIPTION,
  ROOM_PROP_PASSWORD,
  ROOM_PROP_PASSWORD_REQUIRED,
  ROOM_PROP_PERSISTENT,
  ROOM_PROP_PRIVATE,
  ROOM_PROP_SUBJECT,
  ROOM_PROP_SUBJECT_CONTACT,
  ROOM_PROP_SUBJECT_TIMESTAMP,

  NUM_ROOM_PROPS,

  INVALID_ROOM_PROP,
};

const TpPropertySignature room_property_signatures[NUM_ROOM_PROPS] = {
      { "anonymous",         G_TYPE_BOOLEAN },  /* impl: READ, WRITE */
      { "invite-only",       G_TYPE_BOOLEAN },  /* impl: READ, WRITE */
      { "invite-restricted", G_TYPE_BOOLEAN },  /* impl: WRITE */
      { "moderated",         G_TYPE_BOOLEAN },  /* impl: READ, WRITE */
      { "name",              G_TYPE_STRING },   /* impl: READ, WRITE */
      { "description",       G_TYPE_STRING },   /* impl: READ, WRITE */
      { "password",          G_TYPE_STRING },   /* impl: WRITE */
      { "password-required", G_TYPE_BOOLEAN },  /* impl: READ, WRITE */
      { "persistent",        G_TYPE_BOOLEAN },  /* impl: READ, WRITE */
      { "private",           G_TYPE_BOOLEAN },  /* impl: READ, WRITE */
      { "subject",           G_TYPE_STRING },   /* impl: READ, WRITE */
      { "subject-contact",   G_TYPE_UINT },     /* impl: READ */
      { "subject-timestamp", G_TYPE_UINT },     /* impl: READ */
};

/* private structures */

typedef struct {
    TpHandleSet *members;
    TpHandleSet *owners;
    GHashTable *owner_map;
} InitialStateAggregator;

struct _GabbleMucChannelPrivate
{
  GabbleConnection *conn;
  gchar *object_path;

  TpHandle initiator;

  GabbleMucState state;

  guint join_timer_id;
  guint poll_timer_id;

  TpChannelPasswordFlags password_flags;
  DBusGMethodInvocation *password_ctx;
  gchar *password;

  TpHandle handle;
  const gchar *jid;
  gboolean requested;

  guint nick_retry_count;
  GString *self_jid;
  GabbleMucRole self_role;
  GabbleMucAffiliation self_affil;

  guint recv_id;

  TpPropertiesContext *properties_ctx;

  unsigned ready:1;
  unsigned closed:1;
  unsigned dispose_has_run:1;
  unsigned invited:1;

  gchar *invitation_message;

  /* Aggregate all presences when joining the chatroom */
  InitialStateAggregator *initial_state_aggregator;
};

#define GABBLE_MUC_CHANNEL_GET_PRIVATE(o) \
  (G_TYPE_INSTANCE_GET_PRIVATE ((o), GABBLE_TYPE_MUC_CHANNEL, \
                                GabbleMucChannelPrivate))


static void
initial_state_aggregator_free (InitialStateAggregator *isa)
{
  g_hash_table_destroy (isa->owner_map);
  tp_handle_set_destroy (isa->members);
  tp_handle_set_destroy (isa->owners);
  g_slice_free (InitialStateAggregator, isa);
}


static void
gabble_muc_channel_init (GabbleMucChannel *obj)
{
  GabbleMucChannelPrivate *priv = GABBLE_MUC_CHANNEL_GET_PRIVATE (obj);

  priv->initial_state_aggregator = g_slice_new0 (InitialStateAggregator);
  priv->initial_state_aggregator->owner_map = g_hash_table_new (g_direct_hash,
      g_direct_equal);
}


static TpHandle create_room_identity (GabbleMucChannel *)
  G_GNUC_WARN_UNUSED_RESULT;

#define NUM_SUPPORTED_MESSAGE_TYPES 3

static GObject *
gabble_muc_channel_constructor (GType type, guint n_props,
                                GObjectConstructParam *props)
{
  GObject *obj;
  GabbleMucChannel *self;
  GabbleMucChannelPrivate *priv;
  TpBaseConnection *conn;
  DBusGConnection *bus;
  TpHandleRepoIface *room_handles, *contact_handles;
  TpHandle self_handle;
  TpChannelTextMessageType types[NUM_SUPPORTED_MESSAGE_TYPES] = {
      TP_CHANNEL_TEXT_MESSAGE_TYPE_NORMAL,
      TP_CHANNEL_TEXT_MESSAGE_TYPE_ACTION,
      TP_CHANNEL_TEXT_MESSAGE_TYPE_NOTICE,
  };
  const gchar * supported_content_types[] = {
      "text/plain",
      NULL
  };

  obj = G_OBJECT_CLASS (gabble_muc_channel_parent_class)->
           constructor (type, n_props, props);
  self = GABBLE_MUC_CHANNEL (obj);

  priv = GABBLE_MUC_CHANNEL_GET_PRIVATE (self);
  conn = (TpBaseConnection *) priv->conn;

  room_handles = tp_base_connection_get_handles (conn, TP_HANDLE_TYPE_ROOM);
  contact_handles = tp_base_connection_get_handles (conn,
      TP_HANDLE_TYPE_CONTACT);

  /* ref our room handle */
  tp_handle_ref (room_handles, priv->handle);

  /* get the room's jid */
  priv->jid = tp_handle_inspect (room_handles, priv->handle);

  g_assert (priv->initiator != 0);
  tp_handle_ref (contact_handles, priv->initiator);

  /* create our own identity in the room */
  self_handle = create_room_identity (self);
  /* this causes us to have one ref to the self handle which is unreffed
   * at the end of this function */

  priv->initial_state_aggregator->members = tp_handle_set_new (
      contact_handles);
  priv->initial_state_aggregator->owners = tp_handle_set_new (contact_handles);

  /* initialize our own role and affiliation */
  priv->self_role = ROLE_NONE;
  priv->self_affil = AFFILIATION_NONE;

  /* register object on the bus */
  bus = tp_get_bus ();
  dbus_g_connection_register_g_object (bus, priv->object_path, obj);

  /* initialize group mixin */
  tp_group_mixin_init (obj,
      G_STRUCT_OFFSET (GabbleMucChannel, group),
      contact_handles, self_handle);

  /* set initial group flags */
  tp_group_mixin_change_flags (obj,
      TP_CHANNEL_GROUP_FLAG_PROPERTIES |
      TP_CHANNEL_GROUP_FLAG_CHANNEL_SPECIFIC_HANDLES |
      TP_CHANNEL_GROUP_FLAG_HANDLE_OWNERS_NOT_AVAILABLE |
      TP_CHANNEL_GROUP_FLAG_CAN_ADD,
      0);

  /* initialize properties mixin */
  tp_properties_mixin_init (obj, G_STRUCT_OFFSET (
        GabbleMucChannel, properties));

  /* initialize message mixin */
  tp_message_mixin_init (obj, G_STRUCT_OFFSET (GabbleMucChannel, message_mixin),
      conn);
  tp_message_mixin_implement_sending (obj, gabble_muc_channel_send,
      NUM_SUPPORTED_MESSAGE_TYPES, types, 0,
      TP_DELIVERY_REPORTING_SUPPORT_FLAG_RECEIVE_FAILURES |
      TP_DELIVERY_REPORTING_SUPPORT_FLAG_RECEIVE_SUCCESSES,
      supported_content_types);

  tp_group_mixin_add_handle_owner (obj, self_handle, conn->self_handle);

  if (priv->invited)
    {
      /* invited: add ourself to local pending and the inviter to members */
      TpIntSet *set_members, *set_pending;

      set_members = tp_intset_new ();
      set_pending = tp_intset_new ();

      tp_intset_add (set_members, priv->initiator);
      tp_intset_add (set_pending, self->group.self_handle);

      tp_group_mixin_change_members (obj, priv->invitation_message,
          set_members, NULL, set_pending, NULL,
          priv->initiator, TP_CHANNEL_GROUP_CHANGE_REASON_INVITED);

      tp_intset_destroy (set_members);
      tp_intset_destroy (set_pending);

      /* we've dealt with it (and copied it elsewhere), so there's no point
       * in keeping it */
      g_free (priv->invitation_message);
      priv->invitation_message = NULL;

      /* mark channel ready so NewChannel is emitted immediately */
      priv->ready = TRUE;
    }
  else
    {
      /* not invited: add ourselves to members (and hence join immediately) */
      GError *error = NULL;
      GArray *members = g_array_sized_new (FALSE, FALSE, sizeof (TpHandle), 1);

      g_assert (priv->initiator == conn->self_handle);
      g_assert (priv->invitation_message == NULL);

      g_array_append_val (members, self_handle);
      tp_group_mixin_add_members (obj, members,
          "", &error);
      g_assert (error == NULL);
      g_array_free (members, TRUE);
    }

  tp_handle_unref (contact_handles, self_handle);

  return obj;
}

static void
properties_disco_cb (GabbleDisco *disco,
                     GabbleDiscoRequest *request,
                     const gchar *jid,
                     const gchar *node,
                     LmMessageNode *query_result,
                     GError *error,
                     gpointer user_data)
{
  GabbleMucChannel *chan = user_data;
  TpIntSet *changed_props_val, *changed_props_flags;
  LmMessageNode *lm_node;
  const gchar *str;
  GValue val = { 0, };
  NodeIter i;

  g_assert (GABBLE_IS_MUC_CHANNEL (chan));

  if (error)
    {
      DEBUG ("got error %s", error->message);
      return;
    }

  changed_props_val = tp_intset_sized_new (NUM_ROOM_PROPS);
  changed_props_flags = tp_intset_sized_new (NUM_ROOM_PROPS);

  /*
   * Update room definition.
   */

  /* ROOM_PROP_NAME */
  lm_node = lm_message_node_get_child (query_result, "identity");
  if (lm_node)
    {
      const gchar *category, *type, *name;

      category = lm_message_node_get_attribute (lm_node, "category");
      type = lm_message_node_get_attribute (lm_node, "type");
      name = lm_message_node_get_attribute (lm_node, "name");

      if (!tp_strdiff (category, "conference") &&
          !tp_strdiff (type, "text") &&
          name != NULL)
        {
          g_value_init (&val, G_TYPE_STRING);
          g_value_set_string (&val, name);

          tp_properties_mixin_change_value (G_OBJECT (chan), ROOM_PROP_NAME,
                                                &val, changed_props_val);

          tp_properties_mixin_change_flags (G_OBJECT (chan), ROOM_PROP_NAME,
                                                TP_PROPERTY_FLAG_READ,
                                                0, changed_props_flags);

          g_value_unset (&val);
        }
    }

  for (i = node_iter (query_result); i; i = node_iter_next (i))
    {
      guint prop_id = INVALID_ROOM_PROP;
      LmMessageNode *child = node_iter_data (i);

      if (strcmp (child->name, "feature") == 0)
        {
          str = lm_message_node_get_attribute (child, "var");
          if (str == NULL)
            continue;

          /* ROOM_PROP_ANONYMOUS */
          if (strcmp (str, "muc_nonanonymous") == 0)
            {
              prop_id = ROOM_PROP_ANONYMOUS;
              g_value_init (&val, G_TYPE_BOOLEAN);
              g_value_set_boolean (&val, FALSE);
            }
          else if (strcmp (str, "muc_semianonymous") == 0 ||
                   strcmp (str, "muc_anonymous") == 0)
            {
              prop_id = ROOM_PROP_ANONYMOUS;
              g_value_init (&val, G_TYPE_BOOLEAN);
              g_value_set_boolean (&val, TRUE);
            }

          /* ROOM_PROP_INVITE_ONLY */
          else if (strcmp (str, "muc_open") == 0)
            {
              prop_id = ROOM_PROP_INVITE_ONLY;
              g_value_init (&val, G_TYPE_BOOLEAN);
              g_value_set_boolean (&val, FALSE);
            }
          else if (strcmp (str, "muc_membersonly") == 0)
            {
              prop_id = ROOM_PROP_INVITE_ONLY;
              g_value_init (&val, G_TYPE_BOOLEAN);
              g_value_set_boolean (&val, TRUE);
            }

          /* ROOM_PROP_MODERATED */
          else if (strcmp (str, "muc_unmoderated") == 0)
            {
              prop_id = ROOM_PROP_MODERATED;
              g_value_init (&val, G_TYPE_BOOLEAN);
              g_value_set_boolean (&val, FALSE);
            }
          else if (strcmp (str, "muc_moderated") == 0)
            {
              prop_id = ROOM_PROP_MODERATED;
              g_value_init (&val, G_TYPE_BOOLEAN);
              g_value_set_boolean (&val, TRUE);
            }

          /* ROOM_PROP_PASSWORD_REQUIRED */
          else if (strcmp (str, "muc_unsecure") == 0 ||
                   strcmp (str, "muc_unsecured") == 0)
            {
              prop_id = ROOM_PROP_PASSWORD_REQUIRED;
              g_value_init (&val, G_TYPE_BOOLEAN);
              g_value_set_boolean (&val, FALSE);
            }
          else if (strcmp (str, "muc_passwordprotected") == 0)
            {
              prop_id = ROOM_PROP_PASSWORD_REQUIRED;
              g_value_init (&val, G_TYPE_BOOLEAN);
              g_value_set_boolean (&val, TRUE);
            }

          /* ROOM_PROP_PERSISTENT */
          else if (strcmp (str, "muc_temporary") == 0)
            {
              prop_id = ROOM_PROP_PERSISTENT;
              g_value_init (&val, G_TYPE_BOOLEAN);
              g_value_set_boolean (&val, FALSE);
            }
          else if (strcmp (str, "muc_persistent") == 0)
            {
              prop_id = ROOM_PROP_PERSISTENT;
              g_value_init (&val, G_TYPE_BOOLEAN);
              g_value_set_boolean (&val, TRUE);
            }

          /* ROOM_PROP_PRIVATE */
          else if (strcmp (str, "muc_public") == 0)
            {
              prop_id = ROOM_PROP_PRIVATE;
              g_value_init (&val, G_TYPE_BOOLEAN);
              g_value_set_boolean (&val, FALSE);
            }
          else if (strcmp (str, "muc_hidden") == 0)
            {
              prop_id = ROOM_PROP_PRIVATE;
              g_value_init (&val, G_TYPE_BOOLEAN);
              g_value_set_boolean (&val, TRUE);
            }

          /* Ignored */
          else if (strcmp (str, NS_MUC) == 0)
            {
            }

          /* Unhandled */
          else
            {
              DEBUG ("unhandled feature '%s'", str);
            }
        }
      else if (strcmp (child->name, "x") == 0)
        {
          if (lm_message_node_has_namespace (child, NS_X_DATA, NULL))
            {
              NodeIter j;

              for (j = node_iter (child); j; j = node_iter_next (j))
                {
                  LmMessageNode *field = node_iter_data (j);
                  LmMessageNode *value_node;

                  if (strcmp (field->name, "field") != 0)
                    continue;

                  str = lm_message_node_get_attribute (field, "var");
                  if (str == NULL)
                    continue;

                  if (strcmp (str, "muc#roominfo_description") != 0)
                    continue;

                  value_node = lm_message_node_get_child (field, "value");
                  if (value_node == NULL)
                    continue;

                  str = lm_message_node_get_value (value_node);
                  if (str == NULL)
                    {
                      str = "";
                    }

                  prop_id = ROOM_PROP_DESCRIPTION;
                  g_value_init (&val, G_TYPE_STRING);
                  g_value_set_string (&val, str);
                }
            }
        }

      if (prop_id != INVALID_ROOM_PROP)
        {
          tp_properties_mixin_change_value (G_OBJECT (chan), prop_id, &val,
                                                changed_props_val);

          tp_properties_mixin_change_flags (G_OBJECT (chan), prop_id,
                                                TP_PROPERTY_FLAG_READ,
                                                0, changed_props_flags);

          g_value_unset (&val);
        }
    }

  /*
   * Emit signals.
   */
  tp_properties_mixin_emit_changed (G_OBJECT (chan), changed_props_val);
  tp_properties_mixin_emit_flags (G_OBJECT (chan), changed_props_flags);
  tp_intset_destroy (changed_props_val);
  tp_intset_destroy (changed_props_flags);
}

static void
room_properties_update (GabbleMucChannel *chan)
{
  GabbleMucChannelPrivate *priv;
  GError *error = NULL;

  g_assert (GABBLE_IS_MUC_CHANNEL (chan));

  priv = GABBLE_MUC_CHANNEL_GET_PRIVATE (chan);

  if (gabble_disco_request (priv->conn->disco, GABBLE_DISCO_TYPE_INFO,
        priv->jid, NULL, properties_disco_cb, chan, G_OBJECT (chan),
        &error) == NULL)
    {
      DEBUG ("disco query failed: '%s'", error->message);
      g_error_free (error);
    }
}

static TpHandle
create_room_identity (GabbleMucChannel *chan)
{
  GabbleMucChannelPrivate *priv;
  TpBaseConnection *conn;
  TpHandleRepoIface *contact_repo;
  gchar *alias = NULL;
  GabbleConnectionAliasSource source;

  priv = GABBLE_MUC_CHANNEL_GET_PRIVATE (chan);
  conn = (TpBaseConnection *) priv->conn;
  contact_repo = tp_base_connection_get_handles (conn, TP_HANDLE_TYPE_CONTACT);

  g_assert (priv->self_jid == NULL);

  source = _gabble_connection_get_cached_alias (priv->conn, conn->self_handle,
      &alias);
  g_assert (alias != NULL);

  if (source == GABBLE_CONNECTION_ALIAS_FROM_JID)
    {
      /* If our 'alias' is, in fact, our JID, we'll just use the local part as
       * our MUC resource.
       */
      gchar *local_part;

      g_assert (gabble_decode_jid (alias, &local_part, NULL, NULL));
      g_assert (local_part != NULL);
      g_free (alias);

      alias = local_part;
    }

  priv->self_jid = g_string_new (priv->jid);
  g_string_append_c (priv->self_jid, '/');
  g_string_append (priv->self_jid, alias);

  g_free (alias);

  return tp_handle_ensure (contact_repo, priv->self_jid->str,
      GUINT_TO_POINTER (GABBLE_JID_ROOM_MEMBER), NULL);
}

static LmMessage *
create_presence_message (GabbleMucChannel *self,
                         LmMessageSubType sub_type,
                         LmMessageNode **x_node)
{
  GabbleMucChannelPrivate *priv = GABBLE_MUC_CHANNEL_GET_PRIVATE (self);
  LmMessage *msg;
  LmMessageNode *node;

  /* only take the connection-wide presence if the MUC protocol doesn't rely on
   * a particular subtype (eg, leaving the room requires type='unavailable') */
  if (sub_type == LM_MESSAGE_SUB_TYPE_NOT_SET)
    {
      /* note that this message doesn't contain capabilities, but that's OK as
       * most IQ-based protocols won't work in a MUC anyway */
      msg = gabble_presence_as_message (priv->conn->self_presence,
          priv->self_jid->str);
    }
  else
    {
      msg = lm_message_new_with_sub_type (priv->self_jid->str,
          LM_MESSAGE_TYPE_PRESENCE, sub_type);
    }

  node = lm_message_node_add_child (msg->node, "x", NULL);
  lm_message_node_set_attribute (node, "xmlns", NS_MUC);

  if (x_node != NULL)
    *x_node = node;

  return msg;
}

static gboolean
send_join_request (GabbleMucChannel *channel,
                   const gchar *password,
                   GError **error)
{
  GabbleMucChannelPrivate *priv;
  LmMessage *msg;
  LmMessageNode *x_node;
  gboolean ret;

  priv = GABBLE_MUC_CHANNEL_GET_PRIVATE (channel);

  /* build the message */
  msg = create_presence_message (channel, LM_MESSAGE_SUB_TYPE_NOT_SET,
      &x_node);

  g_free (priv->password);

  if (password != NULL)
    {
      priv->password = g_strdup (password);
      lm_message_node_add_child (x_node, "password", password);
    }

  g_signal_emit (channel, signals[PRE_PRESENCE], 0, msg);

  /* send it */
  ret = _gabble_connection_send (priv->conn, msg, error);
  if (!ret)
    {
      DEBUG ("_gabble_connection_send failed");
    }
  else
    {
      DEBUG ("join request sent");
    }

  lm_message_unref (msg);

  return ret;
}

static gboolean
send_leave_message (GabbleMucChannel *channel,
                    const gchar *reason)
{
  GabbleMucChannelPrivate *priv;
  LmMessage *msg;
  GError *error = NULL;
  gboolean ret;

  priv = GABBLE_MUC_CHANNEL_GET_PRIVATE (channel);

  /* build the message */
  msg = create_presence_message (channel, LM_MESSAGE_SUB_TYPE_UNAVAILABLE,
      NULL);

  if (reason != NULL)
    {
      lm_message_node_add_child (msg->node, "status", reason);
    }

  g_signal_emit (channel, signals[PRE_PRESENCE], 0, msg);

  /* send it */
  ret = _gabble_connection_send (priv->conn, msg, &error);
  if (!ret)
    {
      DEBUG ("_gabble_connection_send failed");
      g_error_free (error);
    }
  else
    {
      DEBUG ("leave message sent");
    }

  lm_message_unref (msg);

  return ret;
}

static void
gabble_muc_channel_get_property (GObject    *object,
                                 guint       property_id,
                                 GValue     *value,
                                 GParamSpec *pspec)
{
  GabbleMucChannel *chan = GABBLE_MUC_CHANNEL (object);
  GabbleMucChannelPrivate *priv = GABBLE_MUC_CHANNEL_GET_PRIVATE (chan);
  TpBaseConnection *base_conn = (TpBaseConnection *) priv->conn;

  switch (property_id) {
    case PROP_OBJECT_PATH:
      g_value_set_string (value, priv->object_path);
      break;
    case PROP_CHANNEL_TYPE:
      g_value_set_static_string (value, TP_IFACE_CHANNEL_TYPE_TEXT);
      break;
    case PROP_HANDLE_TYPE:
      g_value_set_uint (value, TP_HANDLE_TYPE_ROOM);
      break;
    case PROP_HANDLE:
      g_value_set_uint (value, priv->handle);
      break;
    case PROP_TARGET_ID:
      g_assert (priv->jid != NULL && strchr (priv->jid, '/') == NULL);
      g_value_set_string (value, priv->jid);
      break;
    case PROP_CONNECTION:
      g_value_set_object (value, priv->conn);
      break;
    case PROP_STATE:
      g_value_set_uint (value, priv->state);
      break;
    case PROP_INTERFACES:
      g_value_set_boxed (value, gabble_muc_channel_interfaces);
      break;
    case PROP_INITIATOR_HANDLE:
      g_assert (priv->initiator != 0);
      g_value_set_uint (value, priv->initiator);
      break;
    case PROP_INITIATOR_ID:
        {
          TpHandleRepoIface *repo = tp_base_connection_get_handles (
              base_conn, TP_HANDLE_TYPE_CONTACT);

          g_assert (priv->initiator != 0);
          g_value_set_string (value,
              tp_handle_inspect (repo, priv->initiator));
        }
      break;
    case PROP_REQUESTED:
      g_value_set_boolean (value, priv->requested);
      break;
    case PROP_CHANNEL_DESTROYED:
      g_value_set_boolean (value, priv->closed);
      break;
    case PROP_CHANNEL_PROPERTIES:
      g_value_take_boxed (value,
          tp_dbus_properties_mixin_make_properties_hash (object,
              TP_IFACE_CHANNEL, "TargetHandle",
              TP_IFACE_CHANNEL, "TargetHandleType",
              TP_IFACE_CHANNEL, "ChannelType",
              TP_IFACE_CHANNEL, "TargetID",
              TP_IFACE_CHANNEL, "InitiatorHandle",
              TP_IFACE_CHANNEL, "InitiatorID",
              TP_IFACE_CHANNEL, "Requested",
              TP_IFACE_CHANNEL, "Interfaces",
              NULL));
      break;
    case PROP_SELF_JID:
      g_value_set_string (value, priv->self_jid->str);
      break;
    default:
      G_OBJECT_WARN_INVALID_PROPERTY_ID (object, property_id, pspec);
      break;
  }
}

static void channel_state_changed (GabbleMucChannel *chan,
                                   GabbleMucState prev_state,
                                   GabbleMucState new_state);

static void
gabble_muc_channel_set_property (GObject     *object,
                                 guint        property_id,
                                 const GValue *value,
                                 GParamSpec   *pspec)
{
  GabbleMucChannel *chan = GABBLE_MUC_CHANNEL (object);
  GabbleMucChannelPrivate *priv = GABBLE_MUC_CHANNEL_GET_PRIVATE (chan);
  GabbleMucState prev_state;

  switch (property_id) {
    case PROP_OBJECT_PATH:
      g_free (priv->object_path);
      priv->object_path = g_value_dup_string (value);
      break;
    case PROP_HANDLE:
      priv->handle = g_value_get_uint (value);
      break;
    case PROP_HANDLE_TYPE:
    case PROP_CHANNEL_TYPE:
      /* these properties are writable in the interface, but not actually
       * meaningfully changeable on this channel, so we do nothing */
      break;
    case PROP_CONNECTION:
      priv->conn = g_value_get_object (value);
      break;
    case PROP_STATE:
      prev_state = priv->state;
      priv->state = g_value_get_uint (value);

      if (priv->state != prev_state)
        channel_state_changed (chan, prev_state, priv->state);

      break;
    case PROP_INVITED:
      priv->invited = g_value_get_boolean (value);
      break;
    case PROP_INITIATOR_HANDLE:
      priv->initiator = g_value_get_uint (value);
      g_assert (priv->initiator != 0);
      break;
    case PROP_INVITATION_MESSAGE:
      g_assert (priv->invitation_message == NULL);
      priv->invitation_message = g_value_dup_string (value);
      break;
    case PROP_REQUESTED:
      priv->requested = g_value_get_boolean (value);
      break;
    default:
      G_OBJECT_WARN_INVALID_PROPERTY_ID (object, property_id, pspec);
      break;
  }
}

static void gabble_muc_channel_dispose (GObject *object);
static void gabble_muc_channel_finalize (GObject *object);
static gboolean gabble_muc_channel_add_member (GObject *obj, TpHandle handle,
    const gchar *message, GError **error);
static gboolean gabble_muc_channel_remove_member (GObject *obj,
    TpHandle handle, const gchar *message, GError **error);
static gboolean gabble_muc_channel_do_set_properties (GObject *obj,
    TpPropertiesContext *ctx, GError **error);

static void
gabble_muc_channel_class_init (GabbleMucChannelClass *gabble_muc_channel_class)
{
  static TpDBusPropertiesMixinPropImpl channel_props[] = {
      { "TargetHandleType", "handle-type", NULL },
      { "TargetHandle", "handle", NULL },
      { "TargetID", "target-id", NULL },
      { "ChannelType", "channel-type", NULL },
      { "Interfaces", "interfaces", NULL },
      { "Requested", "requested", NULL },
      { "InitiatorHandle", "initiator-handle", NULL },
      { "InitiatorID", "initiator-id", NULL },
      { NULL }
  };
  static TpDBusPropertiesMixinIfaceImpl prop_interfaces[] = {
      { TP_IFACE_CHANNEL,
        tp_dbus_properties_mixin_getter_gobject_properties,
        NULL,
        channel_props,
      },
      { NULL }
  };
  GObjectClass *object_class = G_OBJECT_CLASS (gabble_muc_channel_class);
  GParamSpec *param_spec;

  g_type_class_add_private (gabble_muc_channel_class,
      sizeof (GabbleMucChannelPrivate));

  object_class->constructor = gabble_muc_channel_constructor;

  object_class->get_property = gabble_muc_channel_get_property;
  object_class->set_property = gabble_muc_channel_set_property;

  object_class->dispose = gabble_muc_channel_dispose;
  object_class->finalize = gabble_muc_channel_finalize;

  g_object_class_override_property (object_class, PROP_OBJECT_PATH,
      "object-path");
  g_object_class_override_property (object_class, PROP_CHANNEL_TYPE,
      "channel-type");
  g_object_class_override_property (object_class, PROP_HANDLE_TYPE,
      "handle-type");
  g_object_class_override_property (object_class, PROP_HANDLE, "handle");
  g_object_class_override_property (object_class, PROP_CHANNEL_DESTROYED,
      "channel-destroyed");
  g_object_class_override_property (object_class, PROP_CHANNEL_PROPERTIES,
      "channel-properties");

  param_spec = g_param_spec_object ("connection", "GabbleConnection object",
      "Gabble connection object that owns this MUC channel object.",
      GABBLE_TYPE_CONNECTION,
      G_PARAM_CONSTRUCT_ONLY | G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS);
  g_object_class_install_property (object_class, PROP_CONNECTION, param_spec);

  param_spec = g_param_spec_uint ("state", "Channel state",
      "The current state that the channel is in.",
      0, G_MAXUINT32, 0,
      G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS);
  g_object_class_install_property (object_class, PROP_STATE, param_spec);

  param_spec = g_param_spec_boolean ("invited", "Invited?",
      "Whether the user has been invited to the channel.", FALSE,
      G_PARAM_CONSTRUCT_ONLY | G_PARAM_WRITABLE | G_PARAM_STATIC_STRINGS);
  g_object_class_install_property (object_class, PROP_INVITED, param_spec);

  param_spec = g_param_spec_boxed ("interfaces", "Extra D-Bus interfaces",
      "Additional Channel.Interface.* interfaces",
      G_TYPE_STRV,
      G_PARAM_READABLE | G_PARAM_STATIC_STRINGS);
  g_object_class_install_property (object_class, PROP_INTERFACES, param_spec);

  param_spec = g_param_spec_string ("target-id", "MUC's JID",
      "The string obtained by inspecting the MUC's handle",
      NULL,
      G_PARAM_READABLE | G_PARAM_STATIC_STRINGS);
  g_object_class_install_property (object_class, PROP_TARGET_ID, param_spec);

  param_spec = g_param_spec_uint ("initiator-handle", "Initiator's handle",
      "The contact who initiated the channel",
      0, G_MAXUINT32, 0,
      G_PARAM_CONSTRUCT_ONLY | G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS);
  g_object_class_install_property (object_class, PROP_INITIATOR_HANDLE,
      param_spec);

  param_spec = g_param_spec_string ("initiator-id", "Initiator's bare JID",
      "The string obtained by inspecting the initiator-handle",
      NULL,
      G_PARAM_READABLE | G_PARAM_STATIC_STRINGS);
  g_object_class_install_property (object_class, PROP_INITIATOR_ID,
      param_spec);

  param_spec = g_param_spec_boolean ("requested", "Requested?",
      "True if this channel was requested by the local user",
      FALSE,
      G_PARAM_CONSTRUCT_ONLY | G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS);
  g_object_class_install_property (object_class, PROP_REQUESTED, param_spec);

  param_spec = g_param_spec_string ("invitation-message",
      "Invitation message",
      "The message we were sent when invited; NULL if not invited or if "
      "already processed",
      NULL,
      G_PARAM_WRITABLE | G_PARAM_CONSTRUCT_ONLY | G_PARAM_STATIC_STRINGS);
  g_object_class_install_property (object_class, PROP_INVITATION_MESSAGE,
      param_spec);

  param_spec = g_param_spec_string ("self-jid", "Our self JID",
      "Our self muc jid in this room",
      NULL,
      G_PARAM_READABLE | G_PARAM_STATIC_STRINGS);
  g_object_class_install_property (object_class, PROP_SELF_JID,
      param_spec);

  signals[READY] =
    g_signal_new ("ready",
                  G_OBJECT_CLASS_TYPE (gabble_muc_channel_class),
                  G_SIGNAL_RUN_LAST | G_SIGNAL_DETAILED,
                  0,
                  NULL, NULL,
                  g_cclosure_marshal_VOID__VOID,
                  G_TYPE_NONE, 0);

  signals[JOIN_ERROR] =
    g_signal_new ("join-error",
                  G_OBJECT_CLASS_TYPE (gabble_muc_channel_class),
                  G_SIGNAL_RUN_LAST | G_SIGNAL_DETAILED,
                  0,
                  NULL, NULL,
                  g_cclosure_marshal_VOID__POINTER,
                  G_TYPE_NONE, 1, G_TYPE_POINTER);

  signals[PRE_INVITE] =
    g_signal_new ("pre-invite",
                  G_OBJECT_CLASS_TYPE (gabble_muc_channel_class),
                  G_SIGNAL_RUN_LAST | G_SIGNAL_DETAILED,
                  0,
                  NULL, NULL,
                  g_cclosure_marshal_VOID__STRING,
                  G_TYPE_NONE, 1, G_TYPE_STRING);

  signals[CONTACT_JOIN] =
    g_signal_new ("contact-join",
                  G_OBJECT_CLASS_TYPE (gabble_muc_channel_class),
                  G_SIGNAL_RUN_LAST | G_SIGNAL_DETAILED,
                  0,
                  NULL, NULL,
                  g_cclosure_marshal_VOID__UINT,
                  G_TYPE_NONE, 1, G_TYPE_UINT);

  signals[PRE_PRESENCE] =
    g_signal_new ("pre-presence",
                  G_OBJECT_CLASS_TYPE (gabble_muc_channel_class),
                  G_SIGNAL_RUN_LAST | G_SIGNAL_DETAILED,
                  0,
                  NULL, NULL,
                  g_cclosure_marshal_VOID__POINTER,
                  G_TYPE_NONE, 1, G_TYPE_POINTER);

  tp_properties_mixin_class_init (object_class,
                                      G_STRUCT_OFFSET (GabbleMucChannelClass,
                                        properties_class),
                                      room_property_signatures, NUM_ROOM_PROPS,
                                      gabble_muc_channel_do_set_properties);

  gabble_muc_channel_class->dbus_props_class.interfaces = prop_interfaces;
  tp_dbus_properties_mixin_class_init (object_class,
      G_STRUCT_OFFSET (GabbleMucChannelClass, dbus_props_class));

  tp_message_mixin_init_dbus_properties (object_class);

  tp_group_mixin_class_init (object_class,
      G_STRUCT_OFFSET (GabbleMucChannelClass, group_class),
      gabble_muc_channel_add_member,
      gabble_muc_channel_remove_member);
  tp_group_mixin_init_dbus_properties (object_class);
  tp_group_mixin_class_allow_self_removal (object_class);
}

static void clear_join_timer (GabbleMucChannel *chan);
static void clear_poll_timer (GabbleMucChannel *chan);

void
gabble_muc_channel_dispose (GObject *object)
{
  GabbleMucChannel *self = GABBLE_MUC_CHANNEL (object);
  GabbleMucChannelPrivate *priv = GABBLE_MUC_CHANNEL_GET_PRIVATE (self);

  if (priv->dispose_has_run)
    return;

  DEBUG ("called");

  priv->dispose_has_run = TRUE;

  clear_join_timer (self);
  clear_poll_timer (self);

  if (!priv->closed)
    {
      priv->closed = TRUE;
      tp_svc_channel_emit_closed (self);
    }

  if (G_OBJECT_CLASS (gabble_muc_channel_parent_class)->dispose)
    G_OBJECT_CLASS (gabble_muc_channel_parent_class)->dispose (object);
}

void
gabble_muc_channel_finalize (GObject *object)
{
  GabbleMucChannel *self = GABBLE_MUC_CHANNEL (object);
  GabbleMucChannelPrivate *priv = GABBLE_MUC_CHANNEL_GET_PRIVATE (self);
  TpHandleRepoIface *contact_handles = tp_base_connection_get_handles (
      (TpBaseConnection *) priv->conn, TP_HANDLE_TYPE_CONTACT);
  TpHandleRepoIface *room_handles = tp_base_connection_get_handles (
      (TpBaseConnection *) priv->conn, TP_HANDLE_TYPE_ROOM);

  DEBUG ("called");

  if (priv->initial_state_aggregator != NULL)
    {
      initial_state_aggregator_free (priv->initial_state_aggregator);
      priv->initial_state_aggregator = NULL;
    }

  /* free any data held directly by the object here */
  tp_handle_unref (room_handles, priv->handle);

  if (priv->initiator != 0)
    tp_handle_unref (contact_handles, priv->initiator);

  g_free (priv->object_path);

  if (priv->self_jid)
    {
      g_string_free (priv->self_jid, TRUE);
    }

  g_free (priv->password);

  tp_properties_mixin_finalize (object);

  tp_group_mixin_finalize (object);

  tp_message_mixin_finalize (object);

  G_OBJECT_CLASS (gabble_muc_channel_parent_class)->finalize (object);
}

static void clear_join_timer (GabbleMucChannel *chan)
{
  GabbleMucChannelPrivate *priv = GABBLE_MUC_CHANNEL_GET_PRIVATE (chan);

  if (priv->join_timer_id != 0)
    {
      g_source_remove (priv->join_timer_id);
      priv->join_timer_id = 0;
    }
}

static void clear_poll_timer (GabbleMucChannel *chan)
{
  GabbleMucChannelPrivate *priv = GABBLE_MUC_CHANNEL_GET_PRIVATE (chan);

  if (priv->poll_timer_id != 0)
    {
      g_source_remove (priv->poll_timer_id);
      priv->poll_timer_id = 0;
    }
}

static void
change_password_flags (GabbleMucChannel *chan,
                       TpChannelPasswordFlags add,
                       TpChannelPasswordFlags del)
{
  GabbleMucChannelPrivate *priv;
  TpChannelPasswordFlags added, removed;

  g_assert (GABBLE_IS_MUC_CHANNEL (chan));

  priv = GABBLE_MUC_CHANNEL_GET_PRIVATE (chan);

  added = add & ~priv->password_flags;
  priv->password_flags |= added;

  removed = del & priv->password_flags;
  priv->password_flags &= ~removed;

  if (add != 0 || del != 0)
    {
      DEBUG ("emitting password flags changed, added 0x%X, removed 0x%X",
              added, removed);

      tp_svc_channel_interface_password_emit_password_flags_changed (
          chan, added, removed);
    }
}

static void
provide_password_return_if_pending (GabbleMucChannel *chan, gboolean success)
{
  GabbleMucChannelPrivate *priv = GABBLE_MUC_CHANNEL_GET_PRIVATE (chan);

  if (priv->password_ctx)
    {
      dbus_g_method_return (priv->password_ctx, success);
      priv->password_ctx = NULL;
    }

  if (success)
    {
      change_password_flags (chan, 0, TP_CHANNEL_PASSWORD_FLAG_PROVIDE);
    }
}

static void close_channel (GabbleMucChannel *chan, const gchar *reason,
    gboolean inform_muc, TpHandle actor, guint reason_code);

static gboolean
timeout_join (gpointer data)
{
  GabbleMucChannel *chan = data;

  DEBUG ("join timed out, closing channel");

  provide_password_return_if_pending (chan, FALSE);

  close_channel (chan, NULL, FALSE, 0, 0);

  return FALSE;
}

static gboolean
timeout_poll (gpointer data)
{
  GabbleMucChannel *chan = data;

  DEBUG ("polling for room properties");

  room_properties_update (chan);

  return TRUE;
}

static void
channel_state_changed (GabbleMucChannel *chan,
                       GabbleMucState prev_state,
                       GabbleMucState new_state)
{
  GabbleMucChannelPrivate *priv = GABBLE_MUC_CHANNEL_GET_PRIVATE (chan);

  DEBUG ("state changed from %s to %s", muc_states[prev_state],
      muc_states[new_state]);

  if (new_state == MUC_STATE_INITIATED)
    {
      priv->join_timer_id =
        g_timeout_add_seconds (DEFAULT_JOIN_TIMEOUT, timeout_join, chan);
    }
  else if (new_state == MUC_STATE_JOINED)
    {
      gboolean low_bandwidth;
      gint interval;

      provide_password_return_if_pending (chan, TRUE);

      clear_join_timer (chan);

      g_object_get (priv->conn, "low-bandwidth", &low_bandwidth, NULL);

      if (low_bandwidth)
        interval = PROPS_POLL_INTERVAL_LOW;
      else
        interval = PROPS_POLL_INTERVAL_HIGH;

      priv->poll_timer_id =
          g_timeout_add_seconds (interval, timeout_poll, chan);

      /* no need to keep this around any longer, if it's set */
      g_free (priv->password);
      priv->password = NULL;
    }
  else if (new_state == MUC_STATE_ENDED)
    {
      clear_poll_timer (chan);
    }

  if (new_state == MUC_STATE_JOINED || new_state == MUC_STATE_AUTH)
    {
      if (!priv->ready)
        {
          g_signal_emit (chan, signals[READY], 0);
          priv->ready = TRUE;
        }
    }
}


static void
close_channel (GabbleMucChannel *chan, const gchar *reason,
               gboolean inform_muc, TpHandle actor, guint reason_code)
{
  GabbleMucChannelPrivate *priv;
  TpIntSet *set;
  GArray *handles;

  g_assert (GABBLE_IS_MUC_CHANNEL (chan));

  priv = GABBLE_MUC_CHANNEL_GET_PRIVATE (chan);

  if (priv->closed)
    return;

  priv->closed = TRUE;

  /* Remove us from member list */
  set = tp_intset_new ();
  tp_intset_add (set, TP_GROUP_MIXIN (chan)->self_handle);

  tp_group_mixin_change_members ((GObject *) chan,
                                     (reason != NULL) ? reason : "",
                                     NULL, set, NULL, NULL, actor,
                                     reason_code);

  tp_intset_destroy (set);

  /* Inform the MUC if requested */
  if (inform_muc && priv->state >= MUC_STATE_INITIATED)
    {
      send_leave_message (chan, reason);
    }

  handles = tp_handle_set_to_array (chan->group.members);

  gabble_presence_cache_update_many (priv->conn->presence_cache, handles,
    NULL, GABBLE_PRESENCE_UNKNOWN, NULL, 0);

  g_array_free (handles, TRUE);

  /* Update state and emit Closed signal */
  g_object_set (chan, "state", MUC_STATE_ENDED, NULL);

  tp_svc_channel_emit_closed (chan);
}

gboolean
_gabble_muc_channel_is_ready (GabbleMucChannel *chan)
{
  GabbleMucChannelPrivate *priv;

  g_assert (GABBLE_IS_MUC_CHANNEL (chan));

  priv = GABBLE_MUC_CHANNEL_GET_PRIVATE (chan);

  return priv->ready;
}

static gboolean
handle_nick_conflict (GabbleMucChannel *chan,
                      GError **tp_error)
{
  GabbleMucChannelPrivate *priv = GABBLE_MUC_CHANNEL_GET_PRIVATE (chan);
  TpGroupMixin *mixin = TP_GROUP_MIXIN (chan);
  TpHandleRepoIface *contact_repo = tp_base_connection_get_handles (
      (TpBaseConnection *) priv->conn, TP_HANDLE_TYPE_CONTACT);
  TpHandle self_handle;
  TpIntSet *add_rp, *remove_rp;

  if (priv->nick_retry_count >= MAX_NICK_RETRIES)
    {
      g_set_error (tp_error, TP_ERRORS, TP_ERROR_NOT_AVAILABLE,
          "nickname already in use and retry count exceeded");
      return FALSE;
    }

  /* Add a _ to our jid, and update the group mixin's self handle
   * and remote pending members appropriately.
   */
  g_string_append_c (priv->self_jid, '_');
  self_handle = tp_handle_ensure (contact_repo, priv->self_jid->str,
      GUINT_TO_POINTER (GABBLE_JID_ROOM_MEMBER), NULL);

  add_rp = tp_intset_new ();
  remove_rp = tp_intset_new ();
  tp_intset_add (add_rp, self_handle);
  tp_intset_add (remove_rp, mixin->self_handle);

  tp_group_mixin_change_self_handle ((GObject *) chan, self_handle);
  tp_group_mixin_change_members ((GObject *) chan, NULL, NULL, remove_rp, NULL,
      add_rp, 0, TP_CHANNEL_GROUP_CHANGE_REASON_RENAMED);

  tp_intset_destroy (add_rp);
  tp_intset_destroy (remove_rp);
  tp_handle_unref (contact_repo, self_handle);

  priv->nick_retry_count++;
  return send_join_request (chan, priv->password, tp_error);
}

/**
 * _gabble_muc_channel_presence_error
 */
void
_gabble_muc_channel_presence_error (GabbleMucChannel *chan,
                                    const gchar *jid,
                                    LmMessageNode *pres_node)
{
  GabbleMucChannelPrivate *priv;
  LmMessageNode *error_node;
  GabbleXmppError error;
  TpHandle actor = 0;
  guint reason_code = TP_CHANNEL_GROUP_CHANGE_REASON_NONE;

  g_assert (GABBLE_IS_MUC_CHANNEL (chan));

  priv = GABBLE_MUC_CHANNEL_GET_PRIVATE (chan);

  if (strcmp (jid, priv->self_jid->str) != 0)
    {
      DEBUG ("presence error from other jids than self not handled");
      return;
    }

  error_node = lm_message_node_get_child (pres_node, "error");
  if (error_node == NULL)
    {
      DEBUG ("missing required node 'error'");
      return;
    }

  error = gabble_xmpp_error_from_node (error_node, NULL);

  if (priv->state >= MUC_STATE_JOINED)
    {
      DEBUG ("presence error while already member of the channel "
          "-- NYI");
      return;
    }

  /* We're not a member, find out why the join request failed
   * and act accordingly. */
  if (error == XMPP_ERROR_NOT_AUTHORIZED)
    {
      /* channel can sit requiring a password indefinitely */
      clear_join_timer (chan);

      /* Password already provided and incorrect? */
      if (priv->state == MUC_STATE_AUTH)
        {
          provide_password_return_if_pending (chan, FALSE);

          return;
        }

      DEBUG ("password required to join, changing password flags");

      change_password_flags (chan, TP_CHANNEL_PASSWORD_FLAG_PROVIDE, 0);

      g_object_set (chan, "state", MUC_STATE_AUTH, NULL);
    }
  else
    {
      GError *tp_error /* doesn't need initializing */;

      switch (error) {
        case XMPP_ERROR_FORBIDDEN:
          tp_error = g_error_new (TP_ERRORS, TP_ERROR_CHANNEL_BANNED,
                                  "banned from room");
          reason_code = TP_CHANNEL_GROUP_CHANGE_REASON_BANNED;
          break;
        case XMPP_ERROR_SERVICE_UNAVAILABLE:
          tp_error = g_error_new (TP_ERRORS, TP_ERROR_CHANNEL_FULL,
                                  "room is full");
          break;
        case XMPP_ERROR_REGISTRATION_REQUIRED:
          tp_error = g_error_new (TP_ERRORS, TP_ERROR_CHANNEL_INVITE_ONLY,
                                  "room is invite only");
          break;
        case XMPP_ERROR_CONFLICT:
          if (handle_nick_conflict (chan, &tp_error))
            return;

          break;
        default:
          tp_error = g_error_new (TP_ERRORS, TP_ERROR_NOT_AVAILABLE,
              "%s", gabble_xmpp_error_description (error));
          break;
      }

      g_signal_emit (chan, signals[JOIN_ERROR], 0, tp_error);

      close_channel (chan, tp_error->message, FALSE, actor, reason_code);

      g_error_free (tp_error);
    }
}

static GabbleMucRole
get_role_from_string (const gchar *role)
{
  guint i;

  if (role == NULL)
    {
      return ROLE_VISITOR;
    }

  for (i = 0; i < NUM_ROLES; i++)
    {
      if (strcmp (role, muc_roles[i]) == 0)
        {
          return i;
        }
    }

  DEBUG ("unknown role '%s' -- defaulting to ROLE_VISITOR", role);

  return ROLE_VISITOR;
}

static GabbleMucAffiliation
get_affiliation_from_string (const gchar *affil)
{
  guint i;

  if (affil == NULL)
    {
      return AFFILIATION_NONE;
    }

  for (i = 0; i < NUM_AFFILIATIONS; i++)
    {
      if (strcmp (affil, muc_affiliations[i]) == 0)
        {
          return i;
        }
    }

  DEBUG ("unknown affiliation '%s' -- defaulting to "
             "AFFILIATION_NONE", affil);

  return AFFILIATION_NONE;
}

static LmHandlerResult
room_created_submit_reply_cb (GabbleConnection *conn, LmMessage *sent_msg,
                              LmMessage *reply_msg, GObject *object,
                              gpointer user_data)
{
  if (lm_message_get_sub_type (reply_msg) != LM_MESSAGE_SUB_TYPE_RESULT)
    {
      DEBUG ("failed to submit room config");
    }

  return LM_HANDLER_RESULT_REMOVE_MESSAGE;
}

static LmMessageNode *
config_form_get_form_node (LmMessage *msg)
{
  LmMessageNode *node;
  NodeIter i;

  /* find the query node */
  node = lm_message_node_get_child (msg->node, "query");
  if (node == NULL)
    return NULL;

  /* then the form node */
  for (i = node_iter (node); i; i = node_iter_next (i))
    {
      LmMessageNode *child = node_iter_data (i);

      if (tp_strdiff (child->name, "x"))
        {
          continue;
        }

      if (!lm_message_node_has_namespace (child, NS_X_DATA, NULL))
        {
          continue;
        }

      if (tp_strdiff (lm_message_node_get_attribute (child, "type"), "form"))
        {
          continue;
        }

      return child;
    }

  return NULL;
}

static LmHandlerResult
perms_config_form_reply_cb (GabbleConnection *conn, LmMessage *sent_msg,
                            LmMessage *reply_msg, GObject *object,
                            gpointer user_data)
{
  GabbleMucChannel *chan = GABBLE_MUC_CHANNEL (object);
  GabbleMucChannelPrivate *priv = GABBLE_MUC_CHANNEL_GET_PRIVATE (chan);
  LmMessageNode *form_node;
  NodeIter i;

  if (lm_message_get_sub_type (reply_msg) != LM_MESSAGE_SUB_TYPE_RESULT)
    {
      DEBUG ("request for config form denied, property permissions "
                 "will be inaccurate");
      goto OUT;
    }

  /* just in case our affiliation has changed in the meantime */
  if (priv->self_affil != AFFILIATION_OWNER)
    goto OUT;

  form_node = config_form_get_form_node (reply_msg);
  if (form_node == NULL)
    {
      DEBUG ("form node node found, property permissions will be inaccurate");
      goto OUT;
    }

  for (i = node_iter (form_node); i; i = node_iter_next (i))
    {
      const gchar *var;
      LmMessageNode *node = node_iter_data (i);

      if (strcmp (node->name, "field") != 0)
        continue;

      var = lm_message_node_get_attribute (node, "var");
      if (var == NULL)
        continue;

      if (strcmp (var, "muc#roomconfig_roomdesc") == 0 ||
          strcmp (var, "muc#owner_roomdesc") == 0)
        {
          if (tp_properties_mixin_is_readable (G_OBJECT (chan),
                                                   ROOM_PROP_DESCRIPTION))
            {
              tp_properties_mixin_change_flags (G_OBJECT (chan),
                  ROOM_PROP_DESCRIPTION, TP_PROPERTY_FLAG_WRITE, 0,
                  NULL);

              goto OUT;
            }
        }
    }

OUT:
  return LM_HANDLER_RESULT_REMOVE_MESSAGE;
}

static void
update_permissions (GabbleMucChannel *chan)
{
  GabbleMucChannelPrivate *priv = GABBLE_MUC_CHANNEL_GET_PRIVATE (chan);
  TpChannelGroupFlags grp_flags_add, grp_flags_rem;
  TpPropertyFlags prop_flags_add, prop_flags_rem;
  TpIntSet *changed_props_val, *changed_props_flags;

  /*
   * Update group flags.
   */
  grp_flags_add = TP_CHANNEL_GROUP_FLAG_CAN_ADD |
                  TP_CHANNEL_GROUP_FLAG_MESSAGE_ADD;
  grp_flags_rem = 0;

  if (priv->self_role == ROLE_MODERATOR)
    {
      grp_flags_add |= TP_CHANNEL_GROUP_FLAG_CAN_REMOVE |
                       TP_CHANNEL_GROUP_FLAG_MESSAGE_REMOVE;
    }
  else
    {
      grp_flags_rem |= TP_CHANNEL_GROUP_FLAG_CAN_REMOVE |
                       TP_CHANNEL_GROUP_FLAG_MESSAGE_REMOVE;
    }

  tp_group_mixin_change_flags ((GObject *) chan, grp_flags_add, grp_flags_rem);


  /*
   * Update write capabilities based on room configuration
   * and own role and affiliation.
   */

  changed_props_val = tp_intset_sized_new (NUM_ROOM_PROPS);
  changed_props_flags = tp_intset_sized_new (NUM_ROOM_PROPS);

  /*
   * Subject
   *
   * FIXME: this might be allowed for participants/moderators only,
   *        so for now just rely on the server making that call.
   */

  if (priv->self_role >= ROLE_VISITOR)
    {
      prop_flags_add = TP_PROPERTY_FLAG_WRITE;
      prop_flags_rem = 0;
    }
  else
    {
      prop_flags_add = 0;
      prop_flags_rem = TP_PROPERTY_FLAG_WRITE;
    }

  tp_properties_mixin_change_flags (G_OBJECT (chan),
      ROOM_PROP_SUBJECT, prop_flags_add, prop_flags_rem,
      changed_props_flags);

  /* Room definition */
  if (priv->self_affil == AFFILIATION_OWNER)
    {
      prop_flags_add = TP_PROPERTY_FLAG_WRITE;
      prop_flags_rem = 0;
    }
  else
    {
      prop_flags_add = 0;
      prop_flags_rem = TP_PROPERTY_FLAG_WRITE;
    }

  tp_properties_mixin_change_flags (G_OBJECT (chan),
      ROOM_PROP_ANONYMOUS, prop_flags_add, prop_flags_rem,
      changed_props_flags);

  tp_properties_mixin_change_flags (G_OBJECT (chan),
      ROOM_PROP_INVITE_ONLY, prop_flags_add, prop_flags_rem,
      changed_props_flags);

  tp_properties_mixin_change_flags (G_OBJECT (chan),
      ROOM_PROP_INVITE_RESTRICTED, prop_flags_add, prop_flags_rem,
      changed_props_flags);

  tp_properties_mixin_change_flags (G_OBJECT (chan),
      ROOM_PROP_MODERATED, prop_flags_add, prop_flags_rem,
      changed_props_flags);

  tp_properties_mixin_change_flags (G_OBJECT (chan),
      ROOM_PROP_NAME, prop_flags_add, prop_flags_rem,
      changed_props_flags);

  tp_properties_mixin_change_flags (G_OBJECT (chan),
      ROOM_PROP_PASSWORD, prop_flags_add, prop_flags_rem,
      changed_props_flags);

  tp_properties_mixin_change_flags (G_OBJECT (chan),
      ROOM_PROP_PASSWORD_REQUIRED, prop_flags_add, prop_flags_rem,
      changed_props_flags);

  tp_properties_mixin_change_flags (G_OBJECT (chan),
      ROOM_PROP_PERSISTENT, prop_flags_add, prop_flags_rem,
      changed_props_flags);

  tp_properties_mixin_change_flags (G_OBJECT (chan),
      ROOM_PROP_PRIVATE, prop_flags_add, prop_flags_rem,
      changed_props_flags);

  tp_properties_mixin_change_flags (G_OBJECT (chan),
      ROOM_PROP_SUBJECT, prop_flags_add, prop_flags_rem,
      changed_props_flags);

  if (priv->self_affil == AFFILIATION_OWNER)
    {
      /* request the configuration form purely to see if the description
       * is writable by us in this room. sigh. GO MUC!!! */
      LmMessage *msg;
      LmMessageNode *node;
      GError *error = NULL;
      gboolean success;

      msg = lm_message_new_with_sub_type (priv->jid,
          LM_MESSAGE_TYPE_IQ, LM_MESSAGE_SUB_TYPE_GET);
      node = lm_message_node_add_child (msg->node, "query", NULL);
      lm_message_node_set_attribute (node, "xmlns", NS_MUC_OWNER);

      success = _gabble_connection_send_with_reply (priv->conn, msg,
          perms_config_form_reply_cb, G_OBJECT (chan), NULL,
          &error);

      lm_message_unref (msg);

      if (!success)
        {
          DEBUG ("failed to request config form: %s", error->message);
          g_error_free (error);
        }
    }
  else
    {
      /* mark description unwritable if we're no longer an owner */
      tp_properties_mixin_change_flags (G_OBJECT (chan),
          ROOM_PROP_DESCRIPTION, 0, TP_PROPERTY_FLAG_WRITE,
          changed_props_flags);
    }

  /*
   * Emit signals.
   */
  tp_properties_mixin_emit_changed (G_OBJECT (chan), changed_props_val);
  tp_properties_mixin_emit_flags (G_OBJECT (chan), changed_props_flags);
  tp_intset_destroy (changed_props_val);
  tp_intset_destroy (changed_props_flags);
}

static void
handle_unavailable_presence_update (GabbleMucChannel *chan,
                                    TpHandleRepoIface *contact_handles,
                                    TpHandle handle,
                                    TpIntSet *handle_singleton,
                                    LmMessageNode *item_node,
                                    const gchar *status_code)
{
  TpGroupMixin *mixin = TP_GROUP_MIXIN (chan);
  LmMessageNode *reason_node, *actor_node;
  const gchar *reason = "", *actor_jid = "";
  TpHandle actor = 0;
  TpChannelGroupChangeReason reason_code = TP_CHANNEL_GROUP_CHANGE_REASON_NONE;

  actor_node = lm_message_node_get_child (item_node, "actor");
  if (actor_node != NULL)
    {
      actor_jid = lm_message_node_get_attribute (actor_node, "jid");
      if (actor_jid != NULL)
        {
          actor = tp_handle_ensure (contact_handles, actor_jid, NULL,
              NULL);
          if (actor == 0)
            {
              DEBUG ("ignoring invalid actor JID %s", actor_jid);
            }
        }
    }

  /* Possible reasons we could have been removed from the room:
   * 301 banned
   * 307 kicked
   * 321 "because of an affiliation change" - no reason_code
   * 322 room has become members-only and we're not a member - no
   *    reason_code
   * 332 system (server) is being shut down - no reason code
   */
  if (status_code)
    {
      if (strcmp (status_code, "301") == 0)
        {
          reason_code = TP_CHANNEL_GROUP_CHANGE_REASON_BANNED;
        }
      else if (strcmp (status_code, "307") == 0)
        {
          reason_code = TP_CHANNEL_GROUP_CHANGE_REASON_KICKED;
        }
    }

  reason_node = lm_message_node_get_child (item_node, "reason");
  if (reason_node != NULL)
    {
      reason = lm_message_node_get_value (reason_node);
    }

  if (handle != mixin->self_handle)
    {
      tp_group_mixin_change_members ((GObject *) chan, reason,
                                         NULL, handle_singleton, NULL, NULL,
                                         actor, reason_code);
    }
  else
    {
      close_channel (chan, reason, FALSE, actor, reason_code);
    }

  if (actor)
    tp_handle_unref (contact_handles, actor);
}

static gboolean
renamed_by_server (LmMessageNode *x_node)
{
  gboolean is_self = FALSE;
  gboolean renamed = FALSE;
  NodeIter i;

  for (i = node_iter (x_node); i; i = node_iter_next (i))
    {
      LmMessageNode *child = node_iter_data (i);
      const gchar *code;

      if (strcmp (child->name, "status") != 0)
        continue;

      code = lm_message_node_get_attribute (child, "code");

      if (!tp_strdiff (code, "110"))
        is_self = TRUE;
      else if (!tp_strdiff (code, "210"))
        renamed = TRUE;
    }

  return (is_self && renamed);
}

static void
handle_member_added (GabbleMucChannel *chan,
                     GabbleMucChannelPrivate *priv,
                     TpGroupMixin *mixin,
                     TpHandleRepoIface *contact_handles,
                     TpHandle handle,
                     TpIntSet *handle_singleton,
                     LmMessageNode *x_node,
                     LmMessageNode *item_node)
{
  TpBaseConnection *conn = (TpBaseConnection *) priv->conn;
  const gchar *owner_jid = lm_message_node_get_attribute (item_node, "jid");
  TpHandle owner_handle = 0;
  TpIntSet *old_self_handle_singleton = NULL;

  if (owner_jid != NULL)
    {
      owner_handle = tp_handle_ensure (contact_handles, owner_jid,
          GUINT_TO_POINTER (GABBLE_JID_GLOBAL), NULL);

      if (owner_handle == 0)
        DEBUG ("Invalid owner handle '%s', treating as no owner", owner_jid);
    }

  if (renamed_by_server (x_node))
    {
      old_self_handle_singleton = tp_intset_new ();
      tp_intset_add (old_self_handle_singleton, mixin->self_handle);

      tp_group_mixin_change_self_handle ((GObject *) chan, handle);
    }

  if (handle == mixin->self_handle &&
      owner_handle != conn->self_handle)
    {
      /* In XEP-0045-compliant MUCs, if we get presence for the jid we asked
       * for (or for another jid, with status 110 and 210) then we know it's
       * us. There can't be another user in the MUC with the nick we asked for:
       * the service MUST reject us with code 409/"conflict" in this case. So,
       * if someone in the room has the nick we want, it's us.
       *
       * If the MUC service fails to comply with this requirement, we get
       * hopelessly confused. Given that the service isn't required to label
       * our own presence as 110 ("this is you") and there's no way to label a
       * presence for the jid we asked for as "not you" it's not possible for a
       * client not to get hopelessly confused if the service is broken. So
       * this is the best we can do.
       */
      DEBUG ("Overriding ownership of channel-specific handle %u "
          "from %u to %u because I know it's mine",
          mixin->self_handle, owner_handle, conn->self_handle);

      if (owner_handle != 0)
        tp_handle_unref (contact_handles, owner_handle);

      tp_handle_ref (contact_handles, conn->self_handle);
      owner_handle = conn->self_handle;
    }

  if (priv->initial_state_aggregator == NULL)
    {
      /* we've already had the initial batch of presence stanzas */
      tp_group_mixin_add_handle_owner ((GObject *) chan, handle,
          owner_handle);
      tp_group_mixin_change_members ((GObject *) chan, "",
          handle_singleton, NULL, NULL, NULL, 0, 0);
    }
  else
    {
      /* aggregate this presence */
      tp_handle_set_add (priv->initial_state_aggregator->members,
          handle);

      g_hash_table_insert (priv->initial_state_aggregator->owner_map,
          GUINT_TO_POINTER (handle), GUINT_TO_POINTER (owner_handle));

      if (owner_handle != 0)
        tp_handle_set_add (priv->initial_state_aggregator->owners,
            owner_handle);

      /* Do not emit one signal per presence. Instead, get all
       * presences, and add them in priv->initial_state_aggregator.
       * When we get the last presence, emit the signal. The last
       * presence is ourselves. */
      if (handle == mixin->self_handle)
        {
          /* Add all handle owners in a single operation */
          tp_group_mixin_add_handle_owners ((GObject *) chan,
              priv->initial_state_aggregator->owner_map);

          /* Change all presences in a single operation */
          tp_group_mixin_change_members ((GObject *) chan, "",
              tp_handle_set_peek (
                  priv->initial_state_aggregator->members),
              old_self_handle_singleton, NULL, NULL, 0, 0);

          initial_state_aggregator_free (
              priv->initial_state_aggregator);
          priv->initial_state_aggregator = NULL;
          g_object_set (chan, "state", MUC_STATE_JOINED, NULL);
        }
    }

  if (owner_handle != 0)
    {
      if (handle != mixin->self_handle)
        {
          /* If at least one other handle in the channel has an owner,
           * the HANDLE_OWNERS_NOT_AVAILABLE flag should be removed.
           */
          tp_group_mixin_change_flags ((GObject *) chan, 0,
              TP_CHANNEL_GROUP_FLAG_HANDLE_OWNERS_NOT_AVAILABLE);
        }

      g_signal_emit (chan, signals[CONTACT_JOIN], 0, owner_handle);

      tp_handle_unref (contact_handles, owner_handle);
    }

  if (old_self_handle_singleton != NULL)
    tp_intset_destroy (old_self_handle_singleton);
}

static void
handle_presence_update (GabbleMucChannel *chan,
                        TpHandleRepoIface *contact_handles,
                        TpHandle handle,
                        TpIntSet *handle_singleton,
                        LmMessageNode *x_node,
                        LmMessageNode *item_node,
                        const gchar *status_code)
{
  GabbleMucChannelPrivate *priv = GABBLE_MUC_CHANNEL_GET_PRIVATE (chan);
  TpGroupMixin *mixin = TP_GROUP_MIXIN (chan);

  if (!tp_handle_set_is_member (mixin->members, handle))
    handle_member_added (chan, priv, mixin, contact_handles, handle,
        handle_singleton, x_node, item_node);

  if (handle == mixin->self_handle)
    {
      const gchar *role, *affil;
      GabbleMucRole new_role;
      GabbleMucAffiliation new_affil;

      /* accept newly-created room settings before we send anything
       * below which queryies them. */
      if (status_code && strcmp (status_code, "201") == 0)
        {
          LmMessage *msg;
          LmMessageNode *node;
          GError *error = NULL;

          msg = lm_message_new_with_sub_type (priv->jid,
              LM_MESSAGE_TYPE_IQ, LM_MESSAGE_SUB_TYPE_SET);

          node = lm_message_node_add_child (msg->node, "query", NULL);
          lm_message_node_set_attribute (node, "xmlns", NS_MUC_OWNER);

          node = lm_message_node_add_child (node, "x", NULL);
          lm_message_node_set_attributes (node,
                                          "xmlns", NS_X_DATA,
                                          "type", "submit",
                                          NULL);

          if (!_gabble_connection_send_with_reply (priv->conn, msg,
                room_created_submit_reply_cb, G_OBJECT (chan), NULL,
                &error))
            {
              DEBUG ("failed to send submit message: %s",
                  error->message);
              g_error_free (error);

              lm_message_unref (msg);
              close_channel (chan, NULL, TRUE, 0,
                  TP_CHANNEL_GROUP_CHANGE_REASON_NONE);

              return;
            }

          lm_message_unref (msg);
        }

      /* Update room properties */
      room_properties_update (chan);

      /* update permissions after requesting new properties so that if we
       * become an owner, we get our configuration form reply after the
       * discovery reply, so we know whether there is a description
       * property before we try and decide whether we can write to it. */
      role = lm_message_node_get_attribute (item_node, "role");
      affil = lm_message_node_get_attribute (item_node, "affiliation");
      new_role = get_role_from_string (role);
      new_affil = get_affiliation_from_string (affil);

      if (new_role != priv->self_role || new_affil != priv->self_affil)
        {
          priv->self_role = new_role;
          priv->self_affil = new_affil;

          update_permissions (chan);
        }
    }
}

/**
 * _gabble_muc_channel_member_presence_updated:
 *
 * Handles <presence> stanzas with type='unavailable' or other non-'error'
 * types, updating channel members and/or closing the channel as appropriate.
 * (<presence type='error'> is handled by _gabble_muc_channel_presence_error.)
 */
void
_gabble_muc_channel_member_presence_updated (GabbleMucChannel *chan,
                                             TpHandle handle,
                                             LmMessage *message,
                                             LmMessageNode *x_node,
                                             LmMessageNode *item_node)
{
  GabbleMucChannelPrivate *priv;
  TpBaseConnection *conn;
  TpIntSet *handle_singleton;
  LmMessageNode *status_node;
  const gchar *status_code = NULL;
  TpHandleRepoIface *contact_handles;

  DEBUG ("called");

  g_assert (GABBLE_IS_MUC_CHANNEL (chan));
  g_assert (handle != 0);

  priv = GABBLE_MUC_CHANNEL_GET_PRIVATE (chan);
  conn = (TpBaseConnection *) priv->conn;
  contact_handles = tp_base_connection_get_handles (conn,
      TP_HANDLE_TYPE_CONTACT);

  status_node = lm_message_node_get_child (x_node, "status");

  if (status_node != NULL)
    status_code = lm_message_node_get_attribute (status_node, "code");

  /* update channel members according to presence */
  handle_singleton = tp_intset_new ();
  tp_intset_add (handle_singleton, handle);

  if (lm_message_get_sub_type (message) == LM_MESSAGE_SUB_TYPE_UNAVAILABLE)
    handle_unavailable_presence_update (chan, contact_handles, handle,
        handle_singleton, item_node, status_code);
  else
    handle_presence_update (chan, contact_handles, handle, handle_singleton,
        x_node, item_node, status_code);

  tp_intset_destroy (handle_singleton);
}


/**
 * _gabble_muc_channel_handle_subject: handle room subject updates
 */
void
_gabble_muc_channel_handle_subject (GabbleMucChannel *chan,
                                    TpChannelTextMessageType msg_type,
                                    TpHandleType handle_type,
                                    TpHandle sender,
                                    time_t timestamp,
                                    const gchar *subject,
                                    LmMessage *msg)
{
  gboolean is_error;
  GabbleMucChannelPrivate *priv;
  TpIntSet *changed_values, *changed_flags;
  GValue val = { 0, };

  g_assert (GABBLE_IS_MUC_CHANNEL (chan));

  priv = GABBLE_MUC_CHANNEL_GET_PRIVATE (chan);

  is_error = lm_message_get_sub_type (msg) == LM_MESSAGE_SUB_TYPE_ERROR;

  if (priv->properties_ctx)
    {
      tp_properties_context_remove (priv->properties_ctx,
          ROOM_PROP_SUBJECT);
    }

  if (is_error)
    {
      LmMessageNode *node;
      const gchar *err_desc = NULL;

      node = lm_message_node_get_child (msg->node, "error");
      if (node)
        {
          GabbleXmppError xmpp_error = gabble_xmpp_error_from_node (node, NULL);
          err_desc = gabble_xmpp_error_description (xmpp_error);
        }

      if (priv->properties_ctx)
        {
          GError *error = NULL;

          error = g_error_new (TP_ERRORS, TP_ERROR_PERMISSION_DENIED,
              "%s", (err_desc) ? err_desc : "failed to change subject");

          tp_properties_context_return (priv->properties_ctx, error);
          priv->properties_ctx = NULL;

          /* Get the properties into a consistent state. */
          room_properties_update (chan);
        }

      return;
    }

  DEBUG ("updating new property value for subject");

  changed_values = tp_intset_sized_new (NUM_ROOM_PROPS);
  changed_flags = tp_intset_sized_new (NUM_ROOM_PROPS);

  /* ROOM_PROP_SUBJECT */
  g_value_init (&val, G_TYPE_STRING);
  g_value_set_string (&val, subject);

  tp_properties_mixin_change_value (G_OBJECT (chan),
      ROOM_PROP_SUBJECT, &val, changed_values);

  tp_properties_mixin_change_flags (G_OBJECT (chan),
      ROOM_PROP_SUBJECT, TP_PROPERTY_FLAG_READ, 0,
      changed_flags);

  g_value_unset (&val);

  /* ROOM_PROP_SUBJECT_CONTACT */
  g_value_init (&val, G_TYPE_UINT);

  if (handle_type == TP_HANDLE_TYPE_CONTACT)
    {
      g_value_set_uint (&val, sender);
    }
  else
    {
      g_value_set_uint (&val, 0);
    }

  tp_properties_mixin_change_value (G_OBJECT (chan),
      ROOM_PROP_SUBJECT_CONTACT, &val, changed_values);

  tp_properties_mixin_change_flags (G_OBJECT (chan),
      ROOM_PROP_SUBJECT_CONTACT, TP_PROPERTY_FLAG_READ, 0,
      changed_flags);

  g_value_unset (&val);

  /* ROOM_PROP_SUBJECT_TIMESTAMP */
  g_value_init (&val, G_TYPE_UINT);
  g_value_set_uint (&val, timestamp);

  tp_properties_mixin_change_value (G_OBJECT (chan),
      ROOM_PROP_SUBJECT_TIMESTAMP, &val, changed_values);

  tp_properties_mixin_change_flags (G_OBJECT (chan),
      ROOM_PROP_SUBJECT_TIMESTAMP, TP_PROPERTY_FLAG_READ, 0,
      changed_flags);

  g_value_unset (&val);

  /* Emit signals */
  tp_properties_mixin_emit_changed (G_OBJECT (chan), changed_values);
  tp_properties_mixin_emit_flags (G_OBJECT (chan), changed_flags);
  tp_intset_destroy (changed_values);
  tp_intset_destroy (changed_flags);

  if (priv->properties_ctx)
    {
      if (tp_properties_context_return_if_done (priv->properties_ctx))
        {
          priv->properties_ctx = NULL;
        }
    }
}

/**
 * _gabble_muc_channel_receive: receive MUC messages
 */
void
_gabble_muc_channel_receive (GabbleMucChannel *chan,
                             TpChannelTextMessageType msg_type,
                             TpHandleType sender_handle_type,
                             TpHandle sender,
                             time_t timestamp,
                             const gchar *id,
                             const gchar *text,
                             LmMessage *msg,
                             TpChannelTextSendError send_error,
                             TpDeliveryStatus error_status)
{
  GabbleMucChannelPrivate *priv;
  TpBaseConnection *base_conn;
  TpMessage *message;
  TpHandle muc_self_handle;
  gboolean is_echo;
  gboolean is_error;
  gchar *tmp;

  g_assert (GABBLE_IS_MUC_CHANNEL (chan));

  priv = GABBLE_MUC_CHANNEL_GET_PRIVATE (chan);
  base_conn = (TpBaseConnection *) priv->conn;
  muc_self_handle = chan->group.self_handle;

  /* Is this an error report? */
  is_error = (send_error != GABBLE_TEXT_CHANNEL_SEND_NO_ERROR);

  if (is_error && sender == muc_self_handle)
    {
      /* So this is a <message from="ourself" type="error">.  I can only think
       * that this would happen if we send an error stanza and the MUC reflects
       * it back at us, so let's just ignore it.
       */
      NODE_DEBUG (msg->node, "ignoring error stanza from ourself");

      return;
    }

  /* Is this an echo from the MUC of a message we just sent? */
  is_echo = ((sender == muc_self_handle) && (timestamp == 0));

  /* Having excluded the "error from ourself" case, is_error and is_echo are
   * mutually exclusive.
   */

  /* Ignore messages from the channel.  The only such messages I have seen in
   * practice have been on devel@conference.pidgin.im, which sends useful
   * messages like "foo has set the subject to: ..." and "This room is not
   * anonymous".
   */
  if (!is_echo && !is_error && sender_handle_type == TP_HANDLE_TYPE_ROOM)
    {
      NODE_DEBUG (msg->node, "ignoring message from muc");

      return;
    }

  message = tp_message_new (base_conn, 2, 2);

  /* Header common to normal message and delivery-echo */
  if (msg_type != TP_CHANNEL_TEXT_MESSAGE_TYPE_NORMAL)
    tp_message_set_uint32 (message, 0, "message-type", msg_type);

  if (timestamp != 0)
    tp_message_set_uint64 (message, 0, "message-sent", timestamp);

  /* Body */
  tp_message_set_string (message, 1, "content-type", "text/plain");
  tp_message_set_string (message, 1, "content", text);

  if (is_error || is_echo)
    {
      /* Error reports and echos of our own messages are represented as
       * delivery reports.
       */

      TpMessage *delivery_report = tp_message_new (base_conn, 1, 1);
      TpDeliveryStatus status =
          is_error ? error_status : TP_DELIVERY_STATUS_DELIVERED;

      tmp = gabble_generate_id ();
      tp_message_set_string (delivery_report, 0, "message-token", tmp);
      g_free (tmp);

      tp_message_set_uint32 (delivery_report, 0, "message-type",
          TP_CHANNEL_TEXT_MESSAGE_TYPE_DELIVERY_REPORT);
      tp_message_set_uint32 (delivery_report, 0, "delivery-status", status);

      if (id != NULL)
        tp_message_set_string (delivery_report, 0, "delivery-token", id);

      if (is_error)
        tp_message_set_uint32 (delivery_report, 0, "delivery-error",
            send_error);

      /* We do not set a message-sender on the report: the intended recipient
       * of the original message was the MUC, so the spec says we should omit
       * it.
       *
       * The sender of the echo, however, is ourself.  (Unless we get errors
       * for messages that we didn't send, which would be odd.)
       */
      tp_message_set_handle (message, 0, "message-sender",
          TP_HANDLE_TYPE_CONTACT, muc_self_handle);

      /* If we sent the message whose delivery has succeeded or failed, we
       * trust the id='' attribute. */
      if (id != NULL)
        tp_message_set_string (message, 0, "message-token", id);

      tp_message_take_message (delivery_report, 0, "delivery-echo",
          message);

      tp_message_mixin_take_received (G_OBJECT (chan), delivery_report);
    }
  else
    {
      /* Messages from the MUC itself should have no sender. */
      if (sender_handle_type == TP_HANDLE_TYPE_CONTACT)
        tp_message_set_handle (message, 0, "message-sender",
            TP_HANDLE_TYPE_CONTACT, sender);

      if (timestamp != 0)
        tp_message_set_boolean (message, 0, "scrollback", TRUE);

      /* We can't trust the id='' attribute set by the contact to be unique
       * enough to be a message-token, so let's generate one locally.
       */
      tmp = gabble_generate_id ();
      tp_message_set_string (message, 0, "message-token", tmp);
      g_free (tmp);

      tp_message_mixin_take_received (G_OBJECT (chan), message);
    }
}

/**
 * _gabble_muc_channel_state_receive
 *
 * Send the D-BUS signal ChatStateChanged
 * on org.freedesktop.Telepathy.Channel.Interface.ChatState
 */
void
_gabble_muc_channel_state_receive (GabbleMucChannel *chan,
                                   guint state,
                                   guint from_handle)
{
  g_assert (state < NUM_TP_CHANNEL_CHAT_STATES);
  g_assert (GABBLE_IS_MUC_CHANNEL (chan));

  tp_svc_channel_interface_chat_state_emit_chat_state_changed (chan,
      from_handle, state);
}

/**
 * gabble_muc_channel_close
 *
 * Implements D-Bus method Close
 * on interface org.freedesktop.Telepathy.Channel
 */
static void
gabble_muc_channel_close (TpSvcChannel *iface,
                          DBusGMethodInvocation *context)
{
  GabbleMucChannel *self = GABBLE_MUC_CHANNEL (iface);
  GabbleMucChannelPrivate *priv;

  g_assert (GABBLE_IS_MUC_CHANNEL (self));

  priv = GABBLE_MUC_CHANNEL_GET_PRIVATE (self);

  DEBUG ("called on %p", self);

  if (priv->closed)
    {
      GError already = { TP_ERRORS, TP_ERROR_NOT_AVAILABLE,
          "Channel already closed"};
      DEBUG ("channel already closed");

      dbus_g_method_return_error (context, &already);
      return;
    }

  close_channel (self, NULL, TRUE, 0, 0);

  tp_svc_channel_return_from_close (context);
}


/**
 * gabble_muc_channel_get_channel_type
 *
 * Implements D-Bus method GetChannelType
 * on interface org.freedesktop.Telepathy.Channel
 */
static void
gabble_muc_channel_get_channel_type (TpSvcChannel *iface,
                                     DBusGMethodInvocation *context)
{
  tp_svc_channel_return_from_get_channel_type (context,
      TP_IFACE_CHANNEL_TYPE_TEXT);
}


/**
 * gabble_muc_channel_get_handle
 *
 * Implements D-Bus method GetHandle
 * on interface org.freedesktop.Telepathy.Channel
 */
static void
gabble_muc_channel_get_handle (TpSvcChannel *iface,
                               DBusGMethodInvocation *context)
{
  GabbleMucChannel *self = GABBLE_MUC_CHANNEL (iface);
  GabbleMucChannelPrivate *priv;

  g_assert (GABBLE_IS_MUC_CHANNEL (self));

  priv = GABBLE_MUC_CHANNEL_GET_PRIVATE (self);

  tp_svc_channel_return_from_get_handle (context, TP_HANDLE_TYPE_ROOM,
      priv->handle);
}


/**
 * gabble_muc_channel_get_interfaces
 *
 * Implements D-Bus method GetInterfaces
 * on interface org.freedesktop.Telepathy.Channel
 */
static void
gabble_muc_channel_get_interfaces (TpSvcChannel *iface,
                                   DBusGMethodInvocation *context)
{
  tp_svc_channel_return_from_get_interfaces (context,
      gabble_muc_channel_interfaces);
}


/**
 * gabble_muc_channel_get_password_flags
 *
 * Implements D-Bus method GetPasswordFlags
 * on interface org.freedesktop.Telepathy.Channel.Interface.Password
 */
static void
gabble_muc_channel_get_password_flags (TpSvcChannelInterfacePassword *iface,
                                       DBusGMethodInvocation *context)
{
  GabbleMucChannel *self = GABBLE_MUC_CHANNEL (iface);
  GabbleMucChannelPrivate *priv;

  g_assert (GABBLE_IS_MUC_CHANNEL (self));

  priv = GABBLE_MUC_CHANNEL_GET_PRIVATE (self);

  tp_svc_channel_interface_password_return_from_get_password_flags (context,
      priv->password_flags);
}


/**
 * gabble_muc_channel_provide_password
 *
 * Implements D-Bus method ProvidePassword
 * on interface org.freedesktop.Telepathy.Channel.Interface.Password
 *
 * @context: The D-Bus invocation context to use to return values
 *           or throw an error.
 */
static void
gabble_muc_channel_provide_password (TpSvcChannelInterfacePassword *iface,
                                     const gchar *password,
                                     DBusGMethodInvocation *context)
{
  GabbleMucChannel *self = GABBLE_MUC_CHANNEL (iface);
  GError *error = NULL;
  GabbleMucChannelPrivate *priv;

  g_assert (GABBLE_IS_MUC_CHANNEL (self));

  priv = GABBLE_MUC_CHANNEL_GET_PRIVATE (self);

  if ((priv->password_flags & TP_CHANNEL_PASSWORD_FLAG_PROVIDE) == 0 ||
      priv->password_ctx != NULL)
    {
      error = g_error_new (TP_ERRORS, TP_ERROR_NOT_AVAILABLE,
                           "password cannot be provided in the current state");
      dbus_g_method_return_error (context, error);
      g_error_free (error);

      return;
    }

  if (!send_join_request (self, password, &error))
    {
      dbus_g_method_return_error (context, error);
      g_error_free (error);

      return;
    }

  priv->password_ctx = context;
}


/**
 * gabble_muc_channel_send
 *
 * Indirectly implements (via TpMessageMixin) D-Bus method Send on interface
 * org.freedesktop.Telepathy.Channel.Type.Text and D-Bus method SendMessage on
 * Channel.Interface.Messages
 */
static void
gabble_muc_channel_send (GObject *obj,
                         TpMessage *message,
                         TpMessageSendingFlags flags)
{
  GabbleMucChannel *self = GABBLE_MUC_CHANNEL (obj);
  GabbleMucChannelPrivate *priv = GABBLE_MUC_CHANNEL_GET_PRIVATE (self);

  flags &= TP_MESSAGE_SENDING_FLAG_REPORT_DELIVERY;

  gabble_message_util_send_message (obj, priv->conn, message, flags,
      LM_MESSAGE_SUB_TYPE_GROUPCHAT, TP_CHANNEL_CHAT_STATE_ACTIVE,
      priv->jid, FALSE /* send nick */);
}

gboolean
gabble_muc_channel_send_invite (GabbleMucChannel *self,
                                const gchar *jid,
                                const gchar *message,
                                GError **error)
{
  GabbleMucChannelPrivate *priv = GABBLE_MUC_CHANNEL_GET_PRIVATE (self);
  LmMessage *msg;
  LmMessageNode *x_node, *invite_node;
  gboolean result;

  g_signal_emit (self, signals[PRE_INVITE], 0, jid);

  msg = lm_message_new (priv->jid, LM_MESSAGE_TYPE_MESSAGE);

  x_node = lm_message_node_add_child (msg->node, "x", NULL);
  lm_message_node_set_attribute (x_node, "xmlns", NS_MUC_USER);

  invite_node = lm_message_node_add_child (x_node, "invite", NULL);

  lm_message_node_set_attribute (invite_node, "to", jid);

  if (*message != '\0')
    {
      lm_message_node_add_child (invite_node, "reason", message);
    }

  DEBUG ("sending MUC invitation for room %s to contact %s with reason "
      "\"%s\"", priv->jid, jid, message);

  result = _gabble_connection_send (priv->conn, msg, error);
  lm_message_unref (msg);

  return result;
}

static gboolean
gabble_muc_channel_add_member (GObject *obj,
                               TpHandle handle,
                               const gchar *message,
                               GError **error)
{
  GabbleMucChannel *self = GABBLE_MUC_CHANNEL (obj);
  GabbleMucChannelPrivate *priv = GABBLE_MUC_CHANNEL_GET_PRIVATE (self);
  TpGroupMixin *mixin;
  const gchar *jid;

  mixin = TP_GROUP_MIXIN (obj);

  if (handle == mixin->self_handle)
    {
      TpBaseConnection *conn = (TpBaseConnection *) priv->conn;
      TpIntSet *set_remove_members, *set_remote_pending;
      GArray *arr_members;
      gboolean result;

      /* are we already a member or in remote pending? */
      if (tp_handle_set_is_member (mixin->members, handle) ||
          tp_handle_set_is_member (mixin->remote_pending, handle))
        {
          g_set_error (error, TP_ERRORS, TP_ERROR_NOT_AVAILABLE,
              "already a member or in remote pending");

          return FALSE;
        }

      /* add ourself to remote pending and remove the inviter's
       * main jid from the member list */
      set_remove_members = tp_intset_new ();
      set_remote_pending = tp_intset_new ();

      arr_members = tp_handle_set_to_array (mixin->members);
      if (arr_members->len > 0)
        {
          tp_intset_add (set_remove_members,
              g_array_index (arr_members, guint, 0));
        }
      g_array_free (arr_members, TRUE);

      tp_intset_add (set_remote_pending, handle);

      tp_group_mixin_add_handle_owner (obj, mixin->self_handle,
          conn->self_handle);
      tp_group_mixin_change_members (obj, "", NULL, set_remove_members,
          NULL, set_remote_pending, 0,
          priv->invited
            ? TP_CHANNEL_GROUP_CHANGE_REASON_INVITED
            : TP_CHANNEL_GROUP_CHANGE_REASON_NONE);

      tp_intset_destroy (set_remove_members);
      tp_intset_destroy (set_remote_pending);

      /* seek to enter the room */
      result = send_join_request (self, NULL, error);

      g_object_set (obj, "state",
                    (result) ? MUC_STATE_INITIATED : MUC_STATE_ENDED,
                    NULL);

      /* deny adding */
      tp_group_mixin_change_flags (obj, 0, TP_CHANNEL_GROUP_FLAG_CAN_ADD);

      return result;
    }

  /* check that we're indeed a member when attempting to invite others */
  if (priv->state < MUC_STATE_JOINED)
    {
      g_set_error (error, TP_ERRORS, TP_ERROR_NOT_AVAILABLE,
          "channel membership is required for inviting others");

      return FALSE;
    }

  jid = tp_handle_inspect (TP_GROUP_MIXIN (self)->handle_repo, handle);

  return gabble_muc_channel_send_invite (self, jid, message, error);
}

static LmHandlerResult
kick_request_reply_cb (GabbleConnection *conn, LmMessage *sent_msg,
                       LmMessage *reply_msg, GObject *object,
                       gpointer user_data)
{
  if (lm_message_get_sub_type (reply_msg) != LM_MESSAGE_SUB_TYPE_RESULT)
    {
      DEBUG ("Failed to kick user %s from room", (const char *) user_data);
    }

  return LM_HANDLER_RESULT_REMOVE_MESSAGE;
}

static gboolean
gabble_muc_channel_remove_member (GObject *obj,
                                  TpHandle handle,
                                  const gchar *message,
                                  GError **error)
{
  GabbleMucChannel *chan = GABBLE_MUC_CHANNEL (obj);
  GabbleMucChannelPrivate *priv = GABBLE_MUC_CHANNEL_GET_PRIVATE (chan);
  TpGroupMixin *group = TP_GROUP_MIXIN (chan);
  LmMessage *msg;
  LmMessageNode *query_node, *item_node;
  const gchar *jid, *nick;
  gboolean result;

  if (handle == group->self_handle)
    {
      /* User wants to leave the MUC */

      close_channel (chan, message, TRUE, group->self_handle,
          TP_CHANNEL_GROUP_CHANGE_REASON_NONE);
      return TRUE;
    }

  /* Otherwise, the user wants to kick someone. */
  msg = lm_message_new_with_sub_type (priv->jid, LM_MESSAGE_TYPE_IQ,
                                      LM_MESSAGE_SUB_TYPE_SET);

  query_node = lm_message_node_add_child (msg->node, "query", NULL);
  lm_message_node_set_attribute (query_node, "xmlns", NS_MUC_ADMIN);

  item_node = lm_message_node_add_child (query_node, "item", NULL);

  jid = tp_handle_inspect (TP_GROUP_MIXIN (obj)->handle_repo, handle);

  nick = strchr (jid, '/');
  if (nick != NULL)
    nick++;

  lm_message_node_set_attributes (item_node,
                                  "nick", nick,
                                  "role", "none",
                                  NULL);

  if (*message != '\0')
    {
      lm_message_node_add_child (item_node, "reason", message);
    }

  DEBUG ("sending MUC kick request for contact %u (%s) to room %s with reason "
      "\"%s\"", handle, jid, priv->jid, message);

  result = _gabble_connection_send_with_reply (priv->conn, msg,
                                               kick_request_reply_cb,
                                               obj, (gpointer) jid,
                                               error);

  lm_message_unref (msg);

  return result;
}


static LmHandlerResult request_config_form_reply_cb (GabbleConnection *conn,
    LmMessage *sent_msg, LmMessage *reply_msg, GObject *object,
    gpointer user_data);

static gboolean
gabble_muc_channel_do_set_properties (GObject *obj,
                                      TpPropertiesContext *ctx,
                                      GError **error)
{
  GabbleMucChannelPrivate *priv;
  LmMessage *msg;
  LmMessageNode *node;
  gboolean success;

  priv = GABBLE_MUC_CHANNEL_GET_PRIVATE (GABBLE_MUC_CHANNEL (obj));

  g_assert (priv->properties_ctx == NULL);

  /* Changing subject? */
  if (tp_properties_context_has (ctx, ROOM_PROP_SUBJECT))
    {
      const gchar *str;

      str = g_value_get_string (tp_properties_context_get (ctx,
            ROOM_PROP_SUBJECT));

      msg = lm_message_new_with_sub_type (priv->jid,
          LM_MESSAGE_TYPE_MESSAGE, LM_MESSAGE_SUB_TYPE_GROUPCHAT);
      lm_message_node_add_child (msg->node, "subject", str);

      success = _gabble_connection_send (priv->conn, msg, error);

      lm_message_unref (msg);

      if (!success)
        return FALSE;
    }

  /* Changing any other properties? */
  if (tp_properties_context_has_other_than (ctx, ROOM_PROP_SUBJECT))
    {
      msg = lm_message_new_with_sub_type (priv->jid,
          LM_MESSAGE_TYPE_IQ, LM_MESSAGE_SUB_TYPE_GET);
      node = lm_message_node_add_child (msg->node, "query", NULL);
      lm_message_node_set_attribute (node, "xmlns", NS_MUC_OWNER);

      success = _gabble_connection_send_with_reply (priv->conn, msg,
          request_config_form_reply_cb, G_OBJECT (obj), NULL,
          error);

      lm_message_unref (msg);

      if (!success)
        return FALSE;
    }

  priv->properties_ctx = ctx;
  return TRUE;
}

static LmHandlerResult request_config_form_submit_reply_cb (
    GabbleConnection *conn, LmMessage *sent_msg, LmMessage *reply_msg,
    GObject *object, gpointer user_data);

static LmHandlerResult
request_config_form_reply_cb (GabbleConnection *conn, LmMessage *sent_msg,
                              LmMessage *reply_msg, GObject *object,
                              gpointer user_data)
{
  GabbleMucChannel *chan = GABBLE_MUC_CHANNEL (object);
  GabbleMucChannelPrivate *priv = GABBLE_MUC_CHANNEL_GET_PRIVATE (chan);
  TpPropertiesContext *ctx = priv->properties_ctx;
  GError *error = NULL;
  LmMessage *msg = NULL;
  LmMessageNode *submit_node, *form_node, *node;
  guint i, props_left;
  NodeIter j;

  if (lm_message_get_sub_type (reply_msg) != LM_MESSAGE_SUB_TYPE_RESULT)
    {
      error = g_error_new (TP_ERRORS, TP_ERROR_PERMISSION_DENIED,
                           "request for configuration form denied");

      goto OUT;
    }

  form_node = config_form_get_form_node (reply_msg);
  if (form_node == NULL)
    goto PARSE_ERROR;

  /* initialize */
  msg = lm_message_new_with_sub_type (priv->jid, LM_MESSAGE_TYPE_IQ,
                                      LM_MESSAGE_SUB_TYPE_SET);

  node = lm_message_node_add_child (msg->node, "query", NULL);
  lm_message_node_set_attribute (node, "xmlns", NS_MUC_OWNER);

  submit_node = lm_message_node_add_child (node, "x", NULL);
  lm_message_node_set_attributes (submit_node,
                                  "xmlns", NS_X_DATA,
                                  "type", "submit",
                                  NULL);

  /* we assume that the number of props will fit in a guint on all supported
   * platforms, so fail at compile time if this is no longer the case
   */
#if NUM_ROOM_PROPS > 32
#error GabbleMUCChannel request_config_form_reply_cb needs porting to TpIntSet
#endif

  props_left = 0;
  for (i = 0; i < NUM_ROOM_PROPS; i++)
    {
      if (i == ROOM_PROP_SUBJECT)
        continue;

      if (tp_properties_context_has (ctx, i))
        props_left |= 1 << i;
    }

  for (j = node_iter (form_node); j; j = node_iter_next (j))
    {
      const gchar *var;
      LmMessageNode *field_node;
      LmMessageNode *child = node_iter_data (j);
      guint id;
      GType type;
      gboolean invert;
      const gchar *val_str = NULL, *type_str;
      gboolean val_bool;

      if (strcmp (child->name, "field") != 0)
        {
          DEBUG ("skipping node '%s'", child->name);
          continue;
        }

      var = lm_message_node_get_attribute (child, "var");
      if (var == NULL) {
        DEBUG ("skipping node '%s' because of lacking var attribute",
               child->name);
        continue;
      }

      id = INVALID_ROOM_PROP;
      type = G_TYPE_BOOLEAN;
      invert = FALSE;

      if (strcmp (var, "anonymous") == 0)
        {
          id = ROOM_PROP_ANONYMOUS;
        }
      else if (strcmp (var, "muc#roomconfig_whois") == 0)
        {
          id = ROOM_PROP_ANONYMOUS;

          if (tp_properties_context_has (ctx, id))
            {
              val_bool = g_value_get_boolean (
                  tp_properties_context_get (ctx, id));
              val_str = (val_bool) ? "moderators" : "anyone";
            }
        }
      else if (strcmp (var, "muc#owner_whois") == 0)
        {
          id = ROOM_PROP_ANONYMOUS;

          if (tp_properties_context_has (ctx, id))
            {
              val_bool = g_value_get_boolean (
                  tp_properties_context_get (ctx, id));
              val_str = (val_bool) ? "admins" : "anyone";
            }
        }
      else if (strcmp (var, "members_only") == 0 ||
               strcmp (var, "muc#roomconfig_membersonly") == 0 ||
               strcmp (var, "muc#owner_inviteonly") == 0)
        {
          id = ROOM_PROP_INVITE_ONLY;
        }
      else if (strcmp (var, "muc#roomconfig_allowinvites") == 0)
        {
          id = ROOM_PROP_INVITE_RESTRICTED;
          invert = TRUE;
        }
      else if (strcmp (var, "moderated") == 0 ||
               strcmp (var, "muc#roomconfig_moderatedroom") == 0 ||
               strcmp (var, "muc#owner_moderatedroom") == 0)
        {
          id = ROOM_PROP_MODERATED;
        }
      else if (strcmp (var, "title") == 0 ||
               strcmp (var, "muc#roomconfig_roomname") == 0 ||
               strcmp (var, "muc#owner_roomname") == 0)
        {
          id = ROOM_PROP_NAME;
          type = G_TYPE_STRING;
        }
      else if (strcmp (var, "muc#roomconfig_roomdesc") == 0 ||
               strcmp (var, "muc#owner_roomdesc") == 0)
        {
          id = ROOM_PROP_DESCRIPTION;
          type = G_TYPE_STRING;
        }
      else if (strcmp (var, "password") == 0 ||
               strcmp (var, "muc#roomconfig_roomsecret") == 0 ||
               strcmp (var, "muc#owner_roomsecret") == 0)
        {
          id = ROOM_PROP_PASSWORD;
          type = G_TYPE_STRING;
        }
      else if (strcmp (var, "password_protected") == 0 ||
               strcmp (var, "muc#roomconfig_passwordprotectedroom") == 0 ||
               strcmp (var, "muc#owner_passwordprotectedroom") == 0)
        {
          id = ROOM_PROP_PASSWORD_REQUIRED;
        }
      else if (strcmp (var, "persistent") == 0 ||
               strcmp (var, "muc#roomconfig_persistentroom") == 0 ||
               strcmp (var, "muc#owner_persistentroom") == 0)
        {
          id = ROOM_PROP_PERSISTENT;
        }
      else if (strcmp (var, "public") == 0 ||
               strcmp (var, "muc#roomconfig_publicroom") == 0 ||
               strcmp (var, "muc#owner_publicroom") == 0)
        {
          id = ROOM_PROP_PRIVATE;
          invert = TRUE;
        }
      else
        {
          DEBUG ("ignoring field '%s'", var);
        }

      /* add the corresponding field node to the reply message */
      field_node = lm_message_node_add_child (submit_node, "field", NULL);
      lm_message_node_set_attribute (field_node, "var", var);

      type_str = lm_message_node_get_attribute (child, "type");
      if (type_str)
        {
          lm_message_node_set_attribute (field_node, "type", type_str);
        }

      if (id != INVALID_ROOM_PROP && tp_properties_context_has (ctx, id))
        {
          /* Known property and we have a value to set */
          DEBUG ("looking up %s... has=%d", room_property_signatures[id].name,
              tp_properties_context_has (ctx, id));

          if (!val_str)
            {
              const GValue *provided_value;

              provided_value = tp_properties_context_get (ctx, id);

              switch (type) {
                case G_TYPE_BOOLEAN:
                  val_bool = g_value_get_boolean (provided_value);
                  if (invert)
                    val_bool = !val_bool;
                  val_str = val_bool ? "1" : "0";
                  break;
                case G_TYPE_STRING:
                  val_str = g_value_get_string (provided_value);
                  break;
                default:
                  g_assert_not_reached ();
              }
            }

          DEBUG ("Setting value %s for %s", val_str, var);

          props_left &= ~(1 << id);

          /* add the corresponding value node(s) to the reply message */
          lm_message_node_add_child (field_node, "value", val_str);
        }
      else
        {
          /* Copy all the <value> nodes */
          NodeIter k;

          for (k = node_iter (child); k; k = node_iter_next (k))
            {
              LmMessageNode *value_node = node_iter_data (k);

              if (tp_strdiff (value_node->name, "value"))
                /* Not a value, skip it */
                continue;

              lm_message_node_add_child (field_node, "value",
                  lm_message_node_get_value (value_node));
            }
        }
    }

  if (props_left != 0)
    {
      printf (TP_ANSI_BOLD_ON TP_ANSI_FG_WHITE TP_ANSI_BG_RED
              "\n%s: the following properties were not substituted:\n",
              G_STRFUNC);

      for (i = 0; i < NUM_ROOM_PROPS; i++)
        {
          if ((props_left & (1 << i)) != 0)
            {
              printf ("  %s\n", room_property_signatures[i].name);
            }
        }

      printf ("\nthis is a MUC server compatibility bug in gabble, please "
              "report it with a full debug log attached (running gabble "
              "with LM_DEBUG=net)" TP_ANSI_RESET "\n\n");
      fflush (stdout);

      error = g_error_new (TP_ERRORS, TP_ERROR_INVALID_ARGUMENT,
                           "not all properties were substituted");
      goto OUT;
    }

  _gabble_connection_send_with_reply (priv->conn, msg,
      request_config_form_submit_reply_cb, G_OBJECT (object),
      NULL, &error);

  goto OUT;

PARSE_ERROR:
  error = g_error_new (TP_ERRORS, TP_ERROR_NOT_AVAILABLE,
                       "error parsing reply from server");

OUT:
  if (error)
    {
      tp_properties_context_return (ctx, error);
      priv->properties_ctx = NULL;
    }

  if (msg)
    lm_message_unref (msg);

  return LM_HANDLER_RESULT_REMOVE_MESSAGE;
}

static LmHandlerResult
request_config_form_submit_reply_cb (GabbleConnection *conn,
                                     LmMessage *sent_msg,
                                     LmMessage *reply_msg,
                                     GObject *object,
                                     gpointer user_data)
{
  GabbleMucChannel *chan = GABBLE_MUC_CHANNEL (object);
  GabbleMucChannelPrivate *priv = GABBLE_MUC_CHANNEL_GET_PRIVATE (chan);
  TpPropertiesContext *ctx = priv->properties_ctx;
  GError *error = NULL;
  gboolean returned;

  if (lm_message_get_sub_type (reply_msg) != LM_MESSAGE_SUB_TYPE_RESULT)
    {
      error = g_error_new (TP_ERRORS, TP_ERROR_PERMISSION_DENIED,
                           "submitted configuration form was rejected");
    }

  if (!error)
    {
      guint i;

      for (i = 0; i < NUM_ROOM_PROPS; i++)
        {
          if (i != ROOM_PROP_SUBJECT)
            tp_properties_context_remove (ctx, i);
        }

      returned = tp_properties_context_return_if_done (ctx);
    }
  else
    {
      tp_properties_context_return (ctx, error);
      returned = TRUE;

      /* Get the properties into a consistent state. */
      room_properties_update (chan);
    }

  if (returned)
    priv->properties_ctx = NULL;

  return LM_HANDLER_RESULT_REMOVE_MESSAGE;
}

/**
 * gabble_muc_channel_set_chat_state
 *
 * Implements D-Bus method SetChatState
 * on interface org.freedesktop.Telepathy.Channel.Interface.ChatState
 */
static void
gabble_muc_channel_set_chat_state (TpSvcChannelInterfaceChatState *iface,
                                   guint state,
                                   DBusGMethodInvocation *context)
{
  GabbleMucChannel *self = GABBLE_MUC_CHANNEL (iface);
  GabbleMucChannelPrivate *priv;
  GError *error = NULL;

  g_assert (GABBLE_IS_MUC_CHANNEL (self));

  priv = GABBLE_MUC_CHANNEL_GET_PRIVATE (self);

  if (state >= NUM_TP_CHANNEL_CHAT_STATES)
    {
      DEBUG ("invalid state %u", state);

      g_set_error (&error, TP_ERRORS, TP_ERROR_INVALID_ARGUMENT,
          "invalid state: %u", state);
    }

  if (state == TP_CHANNEL_CHAT_STATE_GONE)
    {
      /* We cannot explicitly set the Gone state */
      DEBUG ("you may not explicitly set the Gone state");

      g_set_error (&error, TP_ERRORS, TP_ERROR_INVALID_ARGUMENT,
          "you may not explicitly set the Gone state");
    }

  if (error != NULL ||
      !gabble_message_util_send_chat_state (G_OBJECT (self), priv->conn,
          LM_MESSAGE_SUB_TYPE_GROUPCHAT, state, priv->jid, &error))
    {
      dbus_g_method_return_error (context, error);
      g_error_free (error);

      return;
    }

  tp_svc_channel_interface_chat_state_return_from_set_chat_state (context);
}

gboolean
gabble_muc_channel_send_presence (GabbleMucChannel *self,
                                  GError **error)
{
  GabbleMucChannelPrivate *priv = GABBLE_MUC_CHANNEL_GET_PRIVATE (self);
  LmMessage *msg;
  gboolean result;

  /* do nothing if we havn't actually joined yet */
  if (priv->state < MUC_STATE_INITIATED)
    return TRUE;

  msg = create_presence_message (self, LM_MESSAGE_SUB_TYPE_NOT_SET, NULL);
  g_signal_emit (self, signals[PRE_PRESENCE], 0, msg);
  result = _gabble_connection_send (priv->conn, msg, error);

  lm_message_unref (msg);
  return result;
}

static void
channel_iface_init (gpointer g_iface, gpointer iface_data)
{
  TpSvcChannelClass *klass = (TpSvcChannelClass *) g_iface;

#define IMPLEMENT(x) tp_svc_channel_implement_##x (\
    klass, gabble_muc_channel_##x)
  IMPLEMENT(close);
  IMPLEMENT(get_channel_type);
  IMPLEMENT(get_handle);
  IMPLEMENT(get_interfaces);
#undef IMPLEMENT
}

static void
password_iface_init (gpointer g_iface, gpointer iface_data)
{
  TpSvcChannelInterfacePasswordClass *klass =
    (TpSvcChannelInterfacePasswordClass *) g_iface;

#define IMPLEMENT(x) tp_svc_channel_interface_password_implement_##x (\
    klass, gabble_muc_channel_##x)
  IMPLEMENT(get_password_flags);
  IMPLEMENT(provide_password);
#undef IMPLEMENT
}

static void
chat_state_iface_init (gpointer g_iface, gpointer iface_data)
{
  TpSvcChannelInterfaceChatStateClass *klass =
    (TpSvcChannelInterfaceChatStateClass *) g_iface;

#define IMPLEMENT(x) tp_svc_channel_interface_chat_state_implement_##x (\
    klass, gabble_muc_channel_##x)
  IMPLEMENT(set_chat_state);
#undef IMPLEMENT
}
