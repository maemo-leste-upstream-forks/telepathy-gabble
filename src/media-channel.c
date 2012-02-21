/*
 * gabble-media-channel.c - Source for GabbleMediaChannel
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
#include "media-channel.h"
#include "media-channel-internal.h"

#include <dbus/dbus-glib.h>
#include <dbus/dbus-glib-lowlevel.h>

#include <telepathy-glib/dbus.h>
#include <telepathy-glib/errors.h>
#include <telepathy-glib/exportable-channel.h>
#include <telepathy-glib/interfaces.h>
#include <telepathy-glib/channel-iface.h>
#include <telepathy-glib/svc-channel.h>
#include <telepathy-glib/svc-properties-interface.h>
#include <telepathy-glib/svc-media-interfaces.h>

#define DEBUG_FLAG GABBLE_DEBUG_MEDIA

#include "connection.h"
#include "debug.h"
#include "jingle-content.h"
#include "jingle-factory.h"
#include "jingle-media-rtp.h"
#include "jingle-session.h"
#include "media-factory.h"
#include "media-stream.h"
#include "namespaces.h"
#include "presence-cache.h"
#include "presence.h"
#include "util.h"

#define MAX_STREAMS 99

static void channel_iface_init (gpointer, gpointer);
static void dtmf_iface_init (gpointer, gpointer);
static void media_signalling_iface_init (gpointer, gpointer);
static void streamed_media_iface_init (gpointer, gpointer);
static void session_handler_iface_init (gpointer, gpointer);

G_DEFINE_TYPE_WITH_CODE (GabbleMediaChannel, gabble_media_channel,
    G_TYPE_OBJECT,
    G_IMPLEMENT_INTERFACE (TP_TYPE_SVC_CHANNEL,
      channel_iface_init);
    G_IMPLEMENT_INTERFACE (TP_TYPE_SVC_CHANNEL_INTERFACE_CALL_STATE,
      gabble_media_channel_call_state_iface_init);
    G_IMPLEMENT_INTERFACE (TP_TYPE_SVC_CHANNEL_INTERFACE_DTMF,
      dtmf_iface_init);
    G_IMPLEMENT_INTERFACE (TP_TYPE_SVC_CHANNEL_INTERFACE_GROUP,
      tp_group_mixin_iface_init);
    G_IMPLEMENT_INTERFACE (TP_TYPE_SVC_CHANNEL_INTERFACE_HOLD,
      gabble_media_channel_hold_iface_init);
    G_IMPLEMENT_INTERFACE (TP_TYPE_SVC_CHANNEL_INTERFACE_MEDIA_SIGNALLING,
      media_signalling_iface_init);
    G_IMPLEMENT_INTERFACE (TP_TYPE_SVC_CHANNEL_TYPE_STREAMED_MEDIA,
      streamed_media_iface_init);
    G_IMPLEMENT_INTERFACE (TP_TYPE_SVC_PROPERTIES_INTERFACE,
      tp_properties_mixin_iface_init);
    G_IMPLEMENT_INTERFACE (TP_TYPE_SVC_DBUS_PROPERTIES,
      tp_dbus_properties_mixin_iface_init);
    G_IMPLEMENT_INTERFACE (TP_TYPE_EXPORTABLE_CHANNEL, NULL);
    G_IMPLEMENT_INTERFACE (TP_TYPE_SVC_MEDIA_SESSION_HANDLER,
      session_handler_iface_init);
    G_IMPLEMENT_INTERFACE (TP_TYPE_CHANNEL_IFACE, NULL));

static const gchar *gabble_media_channel_interfaces[] = {
    TP_IFACE_CHANNEL_INTERFACE_CALL_STATE,
    TP_IFACE_CHANNEL_INTERFACE_DTMF,
    TP_IFACE_CHANNEL_INTERFACE_GROUP,
    TP_IFACE_CHANNEL_INTERFACE_HOLD,
    TP_IFACE_CHANNEL_INTERFACE_MEDIA_SIGNALLING,
    TP_IFACE_PROPERTIES_INTERFACE,
    TP_IFACE_MEDIA_SESSION_HANDLER,
    NULL
};

/* properties */
enum
{
  PROP_OBJECT_PATH = 1,
  PROP_CHANNEL_TYPE,
  PROP_HANDLE_TYPE,
  PROP_HANDLE,
  PROP_TARGET_ID,
  PROP_INITIAL_PEER,
  PROP_PEER_IN_RP,
  PROP_PEER,
  PROP_REQUESTED,
  PROP_CONNECTION,
  PROP_CREATOR,
  PROP_CREATOR_ID,
  PROP_INTERFACES,
  PROP_CHANNEL_DESTROYED,
  PROP_CHANNEL_PROPERTIES,
  PROP_INITIAL_AUDIO,
  PROP_INITIAL_VIDEO,
  PROP_IMMUTABLE_STREAMS,
  PROP_CURRENTLY_SENDING_TONES,
  PROP_INITIAL_TONES,
  PROP_DEFERRED_TONES,
  /* TP properties (see also below) */
  PROP_NAT_TRAVERSAL,
  PROP_STUN_SERVER,
  PROP_STUN_PORT,
  PROP_GTALK_P2P_RELAY_TOKEN,
  PROP_SESSION,
  LAST_PROPERTY
};

/* TP properties */
enum
{
  CHAN_PROP_NAT_TRAVERSAL = 0,
  CHAN_PROP_STUN_SERVER,
  CHAN_PROP_STUN_PORT,
  CHAN_PROP_GTALK_P2P_RELAY_TOKEN,
  NUM_CHAN_PROPS,
  INVALID_CHAN_PROP
};

const TpPropertySignature channel_property_signatures[NUM_CHAN_PROPS] = {
      { "nat-traversal",          G_TYPE_STRING },
      { "stun-server",            G_TYPE_STRING },
      { "stun-port",              G_TYPE_UINT   },
      { "gtalk-p2p-relay-token",  G_TYPE_STRING }
};

typedef struct {
    GabbleMediaChannel *self;
    GabbleJingleContent *content;
    gulong removed_id;
    gchar *name;
    const gchar *nat_traversal;
    gboolean initial;
} StreamCreationData;

struct _delayed_request_streams_ctx {
  GabbleMediaChannel *chan;
  gulong caps_disco_id;
  gulong unsure_period_ended_id;
  guint contact_handle;
  GArray *types;
  GFunc succeeded_cb;
  GFunc failed_cb;
  gpointer context;
};

static void destroy_request (struct _delayed_request_streams_ctx *ctx,
    gpointer user_data);

static void
tones_deferred_cb (GabbleMediaChannel *self,
    const gchar *tones,
    TpDTMFPlayer *dtmf_player)
{
  DEBUG ("waiting for user to continue sending '%s'", tones);

  g_free (self->priv->deferred_tones);
  self->priv->deferred_tones = g_strdup (tones);
  tp_svc_channel_interface_dtmf_emit_tones_deferred (self, tones);
}

static void
gabble_media_channel_init (GabbleMediaChannel *self)
{
  GabbleMediaChannelPrivate *priv = G_TYPE_INSTANCE_GET_PRIVATE (self,
      GABBLE_TYPE_MEDIA_CHANNEL, GabbleMediaChannelPrivate);

  self->priv = priv;

  priv->next_stream_id = 1;
  priv->delayed_request_streams = g_ptr_array_sized_new (1);
  priv->streams = g_ptr_array_sized_new (1);

  /* initialize properties mixin */
  tp_properties_mixin_init (G_OBJECT (self), G_STRUCT_OFFSET (
        GabbleMediaChannel, properties));

  priv->dtmf_player = tp_dtmf_player_new ();

  tp_g_signal_connect_object (priv->dtmf_player, "finished",
      G_CALLBACK (tp_svc_channel_interface_dtmf_emit_stopped_tones), self,
      G_CONNECT_SWAPPED);

  tp_g_signal_connect_object (priv->dtmf_player, "tones-deferred",
      G_CALLBACK (tones_deferred_cb), self,
      G_CONNECT_SWAPPED);
}

static void session_state_changed_cb (GabbleJingleSession *session,
    GParamSpec *arg1, GabbleMediaChannel *channel);
static void session_terminated_cb (GabbleJingleSession *session,
    gboolean local_terminator, JingleReason reason, const gchar *text,
    gpointer user_data);
static void session_new_content_cb (GabbleJingleSession *session,
    GabbleJingleContent *c, gpointer user_data);
static void create_stream_from_content (GabbleMediaChannel *chan,
    GabbleJingleContent *c, gboolean initial);
static gboolean contact_is_media_capable (GabbleMediaChannel *chan, TpHandle peer,
    gboolean *wait, GError **error);
static void stream_creation_data_cancel (gpointer p, gpointer unused);
static void session_content_rejected_cb (GabbleJingleSession *session,
    GabbleJingleContent *c, JingleReason reason, const gchar *message,
    gpointer user_data);

static void
create_initial_streams (GabbleMediaChannel *chan)
{
  GabbleMediaChannelPrivate *priv = chan->priv;
  GList *contents, *li;

  contents = gabble_jingle_session_get_contents (priv->session);

  for (li = contents; li; li = li->next)
    {
      GabbleJingleContent *c = li->data;

      /* I'm so sorry. */
      if (G_OBJECT_TYPE (c) == GABBLE_TYPE_JINGLE_MEDIA_RTP)
        {
          guint media_type;

          g_object_get (c, "media-type", &media_type, NULL);

          switch (media_type)
            {
            case JINGLE_MEDIA_TYPE_AUDIO:
              priv->initial_audio = TRUE;
              break;
            case JINGLE_MEDIA_TYPE_VIDEO:
              priv->initial_video = TRUE;
              break;
            default:
              /* smell? */
              DEBUG ("unknown rtp media type %u", media_type);
            }
        }
      else
        {
          g_assert_not_reached ();
        }

      create_stream_from_content (chan, c, TRUE);
    }

  DEBUG ("initial_audio: %s, initial_video: %s",
      priv->initial_audio ? "true" : "false",
      priv->initial_video ? "true" : "false");

  g_list_free (contents);
}

static void
_latch_to_session (GabbleMediaChannel *chan)
{
  GabbleMediaChannelPrivate *priv = chan->priv;

  g_assert (priv->session != NULL);

  DEBUG ("%p: Latching onto session %p", chan, priv->session);

  g_signal_connect (priv->session, "notify::state",
                    (GCallback) session_state_changed_cb, chan);

  g_signal_connect (priv->session, "new-content",
                    (GCallback) session_new_content_cb, chan);

  g_signal_connect (priv->session, "terminated",
                    (GCallback) session_terminated_cb, chan);

  g_signal_connect (priv->session, "content-rejected",
                    (GCallback) session_content_rejected_cb, chan);

  gabble_media_channel_hold_latch_to_session (chan);

  g_assert (priv->streams->len == 0);

  tp_svc_channel_interface_media_signalling_emit_new_session_handler (
      G_OBJECT (chan), priv->object_path, "rtp");
}

static void
create_session (GabbleMediaChannel *chan,
    TpHandle peer,
    const gchar *jid)
{
  GabbleMediaChannelPrivate *priv = chan->priv;
  gboolean local_hold = (priv->hold_state != TP_LOCAL_HOLD_STATE_UNHELD);

  g_assert (priv->session == NULL);

  DEBUG ("%p: Creating new outgoing session", chan);

  priv->session = g_object_ref (
      gabble_jingle_factory_create_session (priv->conn->jingle_factory,
          peer, jid, local_hold));

  _latch_to_session (chan);
}

static GObject *
gabble_media_channel_constructor (GType type, guint n_props,
                                  GObjectConstructParam *props)
{
  GObject *obj;
  GabbleMediaChannelPrivate *priv;
  TpBaseConnection *conn;
  TpDBusDaemon *bus;
  TpIntSet *set;
  TpHandleRepoIface *contact_handles;
  GabbleJingleFactory *jf;
  const gchar *relay_token;
  gchar *stun_server;
  guint stun_port;

  obj = G_OBJECT_CLASS (gabble_media_channel_parent_class)->
           constructor (type, n_props, props);

  priv = GABBLE_MEDIA_CHANNEL (obj)->priv;
  conn = (TpBaseConnection *) priv->conn;
  contact_handles = tp_base_connection_get_handles (conn,
      TP_HANDLE_TYPE_CONTACT);

  /* register object on the bus */
  bus = tp_base_connection_get_dbus_daemon (conn);
  tp_dbus_daemon_register_object (bus, priv->object_path, obj);

  tp_group_mixin_init (obj, G_STRUCT_OFFSET (GabbleMediaChannel, group),
      contact_handles, conn->self_handle);

  if (priv->session != NULL)
      priv->creator = priv->session->peer;
  else
      priv->creator = conn->self_handle;

  /* automatically add creator to channel, but also ref them again (because
   * priv->creator is the InitiatorHandle) */
  g_assert (priv->creator != 0);
  tp_handle_ref (contact_handles, priv->creator);

  set = tp_intset_new_containing (priv->creator);
  tp_group_mixin_change_members (obj, "", set, NULL, NULL, NULL, 0,
      TP_CHANNEL_GROUP_CHANGE_REASON_NONE);
  tp_intset_destroy (set);

  /* We implement the 0.17.6 properties correctly, and can include a message
   * when ending a call.
   */
  tp_group_mixin_change_flags (obj,
      TP_CHANNEL_GROUP_FLAG_PROPERTIES |
      TP_CHANNEL_GROUP_FLAG_MESSAGE_REMOVE |
      TP_CHANNEL_GROUP_FLAG_MESSAGE_REJECT |
      TP_CHANNEL_GROUP_FLAG_MESSAGE_RESCIND,
      0);

  /* Set up Google relay related properties */
  jf = priv->conn->jingle_factory;

  if (gabble_jingle_factory_get_stun_server (jf, &stun_server,
        &stun_port))
    {
      g_object_set (obj,
          "stun-server", stun_server,
          "stun-port", stun_port,
          NULL);
      g_free (stun_server);
    }

  relay_token = gabble_jingle_factory_get_google_relay_token (jf);

  if (relay_token != NULL)
    {
      g_object_set (obj,
          "gtalk-p2p-relay-token", relay_token,
          NULL);
    }

  if (priv->session != NULL)
    {
      /* This is an incoming call; make us local pending and don't set any
       * group flags (all we can do is add or remove ourselves, which is always
       * valid per the spec)
       */
      set = tp_intset_new_containing (conn->self_handle);
      tp_group_mixin_change_members (obj, "", NULL, NULL, set, NULL,
          priv->session->peer, TP_CHANNEL_GROUP_CHANGE_REASON_INVITED);
      tp_intset_destroy (set);

      /* Set up signal callbacks, emit session handler, initialize streams,
       * figure out InitialAudio and InitialVideo
       */
      _latch_to_session (GABBLE_MEDIA_CHANNEL (obj));
      create_initial_streams (GABBLE_MEDIA_CHANNEL (obj));
    }
  else
    {
      /* This is an outgoing call. */

      if (priv->initial_peer != 0)
        {
          if (priv->peer_in_rp)
            {
              /* This channel was created with RequestChannel(SM, Contact, h)
               * so the peer should start out in remote pending.
               */
              set = tp_intset_new_containing (priv->initial_peer);
              tp_group_mixin_change_members (obj, "", NULL, NULL, NULL, set,
                  conn->self_handle, TP_CHANNEL_GROUP_CHANGE_REASON_INVITED);
              tp_intset_destroy (set);
            }

          /* else this channel was created with CreateChannel or EnsureChannel,
           * so don't.
           */
        }
      else
        {
          /* This channel was created with RequestChannel(SM, None, 0). */

          /* The peer can't be in remote pending */
          g_assert (!priv->peer_in_rp);

          /* The UI may call AddMembers([h], "") before calling
           * RequestStreams(h, [...]).
           */
          tp_group_mixin_change_flags (obj, TP_CHANNEL_GROUP_FLAG_CAN_ADD, 0);
        }
    }

  /* If this is a Google session, let's set ImmutableStreams */
  if (priv->session != NULL)
    {
      priv->immutable_streams = !gabble_jingle_session_can_modify_contents (priv->session);
    }
  /* If there's no session yet, but we know who the peer will be, and we have
   * presence for them, we can set ImmutableStreams using the same algorithm as
   * for old-style capabilities.  If we don't know who the peer will be, then
   * the client is using an old calling convention and doesn't need to know
   * this.
   */
  else if (priv->initial_peer != 0)
    {
      GabblePresence *presence = gabble_presence_cache_get (
          priv->conn->presence_cache, priv->initial_peer);
      TpChannelMediaCapabilities flags = 0;

      if (presence != NULL)
        flags = _gabble_media_factory_caps_to_typeflags (
            gabble_presence_peek_caps (presence));

      if (flags & TP_CHANNEL_MEDIA_CAPABILITY_IMMUTABLE_STREAMS)
        priv->immutable_streams = TRUE;
    }

  return obj;
}

static void
gabble_media_channel_get_property (GObject    *object,
                                   guint       property_id,
                                   GValue     *value,
                                   GParamSpec *pspec)
{
  GabbleMediaChannel *chan = GABBLE_MEDIA_CHANNEL (object);
  GabbleMediaChannelPrivate *priv = chan->priv;
  TpBaseConnection *base_conn = (TpBaseConnection *) priv->conn;
  const gchar *param_name;
  guint tp_property_id;

  switch (property_id) {
    case PROP_OBJECT_PATH:
      g_value_set_string (value, priv->object_path);
      break;
    case PROP_CHANNEL_TYPE:
      g_value_set_static_string (value, TP_IFACE_CHANNEL_TYPE_STREAMED_MEDIA);
      break;
    case PROP_HANDLE_TYPE:
      /* This is used to implement TargetHandleType, which is immutable.  If
       * the peer was known at channel-creation time, this will be Contact;
       * otherwise, it must be None even if we subsequently learn who the peer
       * is.
       */
      if (priv->initial_peer != 0)
        g_value_set_uint (value, TP_HANDLE_TYPE_CONTACT);
      else
        g_value_set_uint (value, TP_HANDLE_TYPE_NONE);
      break;
    case PROP_INITIAL_PEER:
    case PROP_HANDLE:
      /* As above: TargetHandle is immutable, so non-0 only if the peer handle
       * was known at creation time.
       */
      g_value_set_uint (value, priv->initial_peer);
      break;
    case PROP_TARGET_ID:
      /* As above. */
      if (priv->initial_peer != 0)
        {
          TpHandleRepoIface *repo = tp_base_connection_get_handles (
              base_conn, TP_HANDLE_TYPE_CONTACT);
          const gchar *target_id = tp_handle_inspect (repo, priv->initial_peer);

          g_value_set_string (value, target_id);
        }
      else
        {
          g_value_set_static_string (value, "");
        }

      break;
    case PROP_PEER:
      {
        TpHandle peer = 0;

        if (priv->initial_peer != 0)
          peer = priv->initial_peer;
        else if (priv->session != NULL)
          g_object_get (priv->session,
              "peer", &peer,
              NULL);

        g_value_set_uint (value, peer);
        break;
      }
    case PROP_CONNECTION:
      g_value_set_object (value, priv->conn);
      break;
    case PROP_CREATOR:
      g_value_set_uint (value, priv->creator);
      break;
    case PROP_CREATOR_ID:
        {
          TpHandleRepoIface *repo = tp_base_connection_get_handles (
              base_conn, TP_HANDLE_TYPE_CONTACT);

          g_value_set_string (value, tp_handle_inspect (repo, priv->creator));
        }
      break;
    case PROP_REQUESTED:
      g_value_set_boolean (value, (priv->creator == base_conn->self_handle));
      break;
    case PROP_INTERFACES:
      g_value_set_boxed (value, gabble_media_channel_interfaces);
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
              TP_IFACE_CHANNEL_TYPE_STREAMED_MEDIA, "InitialAudio",
              TP_IFACE_CHANNEL_TYPE_STREAMED_MEDIA, "InitialVideo",
              TP_IFACE_CHANNEL_TYPE_STREAMED_MEDIA, "ImmutableStreams",
              NULL));
      break;
    case PROP_SESSION:
      g_value_set_object (value, priv->session);
      break;
    case PROP_INITIAL_AUDIO:
      g_value_set_boolean (value, priv->initial_audio);
      break;
    case PROP_INITIAL_VIDEO:
      g_value_set_boolean (value, priv->initial_video);
      break;
    case PROP_IMMUTABLE_STREAMS:
      g_value_set_boolean (value, priv->immutable_streams);
      break;
    case PROP_CURRENTLY_SENDING_TONES:
      g_value_set_boolean (value,
          tp_dtmf_player_is_active (priv->dtmf_player));
      break;
    case PROP_INITIAL_TONES:
      /* FIXME: stub */
      g_value_set_static_string (value, "");
      break;
    case PROP_DEFERRED_TONES:
      if (priv->deferred_tones != NULL)
        g_value_set_string (value, priv->deferred_tones);
      else
        g_value_set_static_string (value, "");
      break;
    default:
      param_name = g_param_spec_get_name (pspec);

      if (tp_properties_mixin_has_property (object, param_name,
            &tp_property_id))
        {
          GValue *tp_property_value =
            chan->properties.properties[tp_property_id].value;

          if (tp_property_value)
            {
              g_value_copy (tp_property_value, value);
              return;
            }
        }

      G_OBJECT_WARN_INVALID_PROPERTY_ID (object, property_id, pspec);
      break;
  }
}

static void
gabble_media_channel_set_property (GObject     *object,
                                   guint        property_id,
                                   const GValue *value,
                                   GParamSpec   *pspec)
{
  GabbleMediaChannel *chan = GABBLE_MEDIA_CHANNEL (object);
  GabbleMediaChannelPrivate *priv = chan->priv;
  const gchar *param_name;
  guint tp_property_id;

  switch (property_id) {
    case PROP_OBJECT_PATH:
      g_free (priv->object_path);
      priv->object_path = g_value_dup_string (value);
      break;
    case PROP_HANDLE_TYPE:
    case PROP_HANDLE:
    case PROP_CHANNEL_TYPE:
      /* these properties are writable in the interface, but not actually
       * meaningfully changable on this channel, so we do nothing */
      break;
    case PROP_CONNECTION:
      priv->conn = g_value_get_object (value);
      break;
    case PROP_CREATOR:
      priv->creator = g_value_get_uint (value);
      break;
    case PROP_INITIAL_PEER:
      priv->initial_peer = g_value_get_uint (value);

      if (priv->initial_peer != 0)
        {
          TpBaseConnection *base_conn = (TpBaseConnection *) priv->conn;
          TpHandleRepoIface *repo = tp_base_connection_get_handles (base_conn,
              TP_HANDLE_TYPE_CONTACT);
          tp_handle_ref (repo, priv->initial_peer);
        }

      break;
    case PROP_PEER_IN_RP:
      priv->peer_in_rp = g_value_get_boolean (value);
      break;
    case PROP_SESSION:
      g_assert (priv->session == NULL);
      priv->session = g_value_dup_object (value);
      if (priv->session != NULL)
        {

        }
      break;
    case PROP_INITIAL_AUDIO:
      priv->initial_audio = g_value_get_boolean (value);
      break;
    case PROP_INITIAL_VIDEO:
      priv->initial_video = g_value_get_boolean (value);
      break;
    default:
      param_name = g_param_spec_get_name (pspec);

      if (tp_properties_mixin_has_property (object, param_name,
            &tp_property_id))
        {
          tp_properties_mixin_change_value (object, tp_property_id, value,
                                                NULL);
          tp_properties_mixin_change_flags (object, tp_property_id,
                                                TP_PROPERTY_FLAG_READ,
                                                0, NULL);

          return;
        }

      G_OBJECT_WARN_INVALID_PROPERTY_ID (object, property_id, pspec);
      break;
  }
}

static void gabble_media_channel_dispose (GObject *object);
static void gabble_media_channel_finalize (GObject *object);
static gboolean gabble_media_channel_add_member (GObject *obj,
    TpHandle handle,
    const gchar *message,
    GError **error);
static gboolean gabble_media_channel_remove_member (GObject *obj,
    TpHandle handle, const gchar *message, guint reason, GError **error);

static void
gabble_media_channel_class_init (GabbleMediaChannelClass *gabble_media_channel_class)
{
  static TpDBusPropertiesMixinPropImpl channel_props[] = {
      { "TargetHandleType", "handle-type", NULL },
      { "TargetHandle", "handle", NULL },
      { "TargetID", "target-id", NULL },
      { "ChannelType", "channel-type", NULL },
      { "Interfaces", "interfaces", NULL },
      { "Requested", "requested", NULL },
      { "InitiatorHandle", "creator", NULL },
      { "InitiatorID", "creator-id", NULL },
      { NULL }
  };
  static TpDBusPropertiesMixinPropImpl streamed_media_props[] = {
      { "ImmutableStreams", "immutable-streams", NULL },
      { "InitialAudio", "initial-audio", NULL },
      { "InitialVideo", "initial-video", NULL },
      { NULL }
  };
  static TpDBusPropertiesMixinPropImpl dtmf_props[] = {
      { "CurrentlySendingTones", "currently-sending-tones", NULL },
      { "InitialTones", "initial-tones", NULL },
      { "DeferredTones", "deferred-tones", NULL },
      { NULL }
  };
  static TpDBusPropertiesMixinIfaceImpl prop_interfaces[] = {
      { TP_IFACE_CHANNEL,
        tp_dbus_properties_mixin_getter_gobject_properties,
        NULL,
        channel_props,
      },
      { TP_IFACE_CHANNEL_TYPE_STREAMED_MEDIA,
        tp_dbus_properties_mixin_getter_gobject_properties,
        NULL,
        streamed_media_props,
      },
      { TP_IFACE_CHANNEL_INTERFACE_DTMF,
        tp_dbus_properties_mixin_getter_gobject_properties,
        NULL,
        dtmf_props,
      },
      { NULL }
  };
  GObjectClass *object_class = G_OBJECT_CLASS (gabble_media_channel_class);
  GParamSpec *param_spec;

  g_type_class_add_private (gabble_media_channel_class,
      sizeof (GabbleMediaChannelPrivate));

  object_class->constructor = gabble_media_channel_constructor;

  object_class->get_property = gabble_media_channel_get_property;
  object_class->set_property = gabble_media_channel_set_property;

  object_class->dispose = gabble_media_channel_dispose;
  object_class->finalize = gabble_media_channel_finalize;

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

  param_spec = g_param_spec_string ("target-id", "Target JID",
      "Currently empty, because this channel always has handle 0.",
      NULL,
      G_PARAM_READABLE | G_PARAM_STATIC_STRINGS);
  g_object_class_install_property (object_class, PROP_TARGET_ID, param_spec);

  param_spec = g_param_spec_uint ("initial-peer", "Other participant",
      "The TpHandle representing the other participant in the channel if known "
      "at construct-time; 0 if the other participant was unknown at the time "
      "of channel creation",
      0, G_MAXUINT32, 0,
      G_PARAM_CONSTRUCT_ONLY | G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS);
  g_object_class_install_property (object_class, PROP_INITIAL_PEER, param_spec);

  param_spec = g_param_spec_boolean ("peer-in-rp",
      "Peer initially in Remote Pending?",
      "True if the channel was created with the most-deprecated "
      "RequestChannels form, and so the peer should be in Remote Pending "
      "before any XML has been sent.",
      FALSE,
      G_PARAM_CONSTRUCT_ONLY | G_PARAM_WRITABLE | G_PARAM_STATIC_STRINGS);
  g_object_class_install_property (object_class, PROP_PEER_IN_RP, param_spec);

  param_spec = g_param_spec_uint ("peer", "Other participant",
      "The TpHandle representing the other participant in the channel if "
      "currently known; 0 if this is an anonymous channel on which "
      "RequestStreams  has not yet been called.",
      0, G_MAXUINT32, 0,
      G_PARAM_READABLE | G_PARAM_STATIC_STRINGS);
  g_object_class_install_property (object_class, PROP_PEER, param_spec);

  param_spec = g_param_spec_object ("connection", "GabbleConnection object",
      "Gabble connection object that owns this media channel object.",
      GABBLE_TYPE_CONNECTION,
      G_PARAM_CONSTRUCT_ONLY | G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS);
  g_object_class_install_property (object_class, PROP_CONNECTION, param_spec);

  param_spec = g_param_spec_uint ("creator", "Channel creator",
      "The TpHandle representing the contact who created the channel.",
      0, G_MAXUINT32, 0,
      G_PARAM_CONSTRUCT_ONLY | G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS);
  g_object_class_install_property (object_class, PROP_CREATOR, param_spec);

  param_spec = g_param_spec_string ("creator-id", "Creator bare JID",
      "The bare JID obtained by inspecting the creator handle.",
      NULL,
      G_PARAM_READABLE | G_PARAM_STATIC_STRINGS);
  g_object_class_install_property (object_class, PROP_CREATOR_ID, param_spec);

  param_spec = g_param_spec_boolean ("requested", "Requested?",
      "True if this channel was requested by the local user",
      FALSE,
      G_PARAM_READABLE | G_PARAM_STATIC_STRINGS);
  g_object_class_install_property (object_class, PROP_REQUESTED, param_spec);

  param_spec = g_param_spec_boxed ("interfaces", "Extra D-Bus interfaces",
      "Additional Channel.Interface.* interfaces",
      G_TYPE_STRV,
      G_PARAM_READABLE | G_PARAM_STATIC_STRINGS);
  g_object_class_install_property (object_class, PROP_INTERFACES, param_spec);

  param_spec = g_param_spec_string ("nat-traversal", "NAT traversal",
      "NAT traversal mechanism.",
      "gtalk-p2p",
      G_PARAM_CONSTRUCT | G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS);
  g_object_class_install_property (object_class, PROP_NAT_TRAVERSAL,
      param_spec);

  param_spec = g_param_spec_string ("stun-server", "STUN server",
      "IP or address of STUN server.",
      NULL,
      G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS);
  g_object_class_install_property (object_class, PROP_STUN_SERVER, param_spec);

  param_spec = g_param_spec_uint ("stun-port", "STUN port",
      "UDP port of STUN server.",
      0, G_MAXUINT16, 0,
      G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS);
  g_object_class_install_property (object_class, PROP_STUN_PORT, param_spec);

  param_spec = g_param_spec_string ("gtalk-p2p-relay-token",
      "GTalk P2P Relay Token",
      "Magic token to authenticate with the Google Talk relay server.",
      NULL,
      G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS);
  g_object_class_install_property (object_class, PROP_GTALK_P2P_RELAY_TOKEN,
      param_spec);

  param_spec = g_param_spec_object ("session", "GabbleJingleSession object",
      "Jingle session associated with this media channel object.",
      GABBLE_TYPE_JINGLE_SESSION,
      G_PARAM_CONSTRUCT_ONLY | G_PARAM_READWRITE |
      G_PARAM_STATIC_NAME | G_PARAM_STATIC_NICK | G_PARAM_STATIC_BLURB);
  g_object_class_install_property (object_class, PROP_SESSION, param_spec);

  param_spec = g_param_spec_boolean ("initial-audio", "InitialAudio",
      "Whether the channel initially contained an audio stream",
      FALSE,
      G_PARAM_CONSTRUCT_ONLY | G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS);
  g_object_class_install_property (object_class, PROP_INITIAL_AUDIO,
      param_spec);

  param_spec = g_param_spec_boolean ("initial-video", "InitialVideo",
      "Whether the channel initially contained an video stream",
      FALSE,
      G_PARAM_CONSTRUCT_ONLY | G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS);
  g_object_class_install_property (object_class, PROP_INITIAL_VIDEO,
      param_spec);

  param_spec = g_param_spec_boolean ("immutable-streams", "ImmutableStreams",
      "Whether the set of streams on this channel are fixed once requested",
      FALSE,
      G_PARAM_READABLE | G_PARAM_STATIC_STRINGS);
  g_object_class_install_property (object_class, PROP_IMMUTABLE_STREAMS,
      param_spec);

  param_spec = g_param_spec_boolean ("currently-sending-tones",
      "CurrentlySendingTones",
      "True if a DTMF tone is being sent",
      FALSE,
      G_PARAM_READABLE | G_PARAM_STATIC_STRINGS);
  g_object_class_install_property (object_class, PROP_CURRENTLY_SENDING_TONES,
      param_spec);

  param_spec = g_param_spec_string ("initial-tones", "InitialTones",
      "Initial DTMF tones to be sent in the first audio stream",
      "", G_PARAM_READABLE | G_PARAM_STATIC_STRINGS);
  g_object_class_install_property (object_class, PROP_INITIAL_TONES,
      param_spec);

  param_spec = g_param_spec_string ("deferred-tones", "DeferredTones",
      "DTMF tones that followed a 'w' or 'W', to be resumed on user request",
      "", G_PARAM_READABLE | G_PARAM_STATIC_STRINGS);
  g_object_class_install_property (object_class, PROP_DEFERRED_TONES,
      param_spec);

  tp_properties_mixin_class_init (object_class,
      G_STRUCT_OFFSET (GabbleMediaChannelClass, properties_class),
      channel_property_signatures, NUM_CHAN_PROPS, NULL);

  gabble_media_channel_class->dbus_props_class.interfaces = prop_interfaces;
  tp_dbus_properties_mixin_class_init (object_class,
      G_STRUCT_OFFSET (GabbleMediaChannelClass, dbus_props_class));

  tp_group_mixin_class_init (object_class,
      G_STRUCT_OFFSET (GabbleMediaChannelClass, group_class),
      gabble_media_channel_add_member, NULL);
  tp_group_mixin_class_set_remove_with_reason_func (object_class,
      gabble_media_channel_remove_member);
  tp_group_mixin_class_allow_self_removal (object_class);

  tp_group_mixin_init_dbus_properties (object_class);
}

void
gabble_media_channel_dispose (GObject *object)
{
  GabbleMediaChannel *self = GABBLE_MEDIA_CHANNEL (object);
  GabbleMediaChannelPrivate *priv = self->priv;
  TpBaseConnection *conn = (TpBaseConnection *) priv->conn;
  TpHandleRepoIface *contact_handles = tp_base_connection_get_handles (
      conn, TP_HANDLE_TYPE_CONTACT);
  GList *l;

  if (priv->dispose_has_run)
    return;

  DEBUG ("called");

  priv->dispose_has_run = TRUE;

  if (!priv->closed)
    gabble_media_channel_close (self);

  g_assert (priv->closed);
  g_assert (priv->session == NULL);

  /* Since the session's dead, all the stream_creation_datas should have been
   * cancelled (which is indicated by their 'content' being NULL).
   */
  for (l = priv->stream_creation_datas; l != NULL; l = l->next)
    {
      StreamCreationData *d = l->data;
      g_assert (d->content == NULL);
    }

  g_list_free (priv->stream_creation_datas);
  priv->stream_creation_datas = NULL;

  if (priv->delayed_request_streams != NULL)
    {
      g_ptr_array_foreach (priv->delayed_request_streams,
          (GFunc) destroy_request, NULL);
      g_ptr_array_unref (priv->delayed_request_streams);
      priv->delayed_request_streams = NULL;
    }

  tp_handle_unref (contact_handles, priv->creator);
  priv->creator = 0;

  if (priv->initial_peer != 0)
    {
      tp_handle_unref (contact_handles, priv->initial_peer);
      priv->initial_peer = 0;
    }

  /* All of the streams should have closed in response to the contents being
   * removed when the call ended.
   */
  g_assert (priv->streams->len == 0);
  g_ptr_array_unref (priv->streams);
  priv->streams = NULL;

  if (G_OBJECT_CLASS (gabble_media_channel_parent_class)->dispose)
    G_OBJECT_CLASS (gabble_media_channel_parent_class)->dispose (object);
}

void
gabble_media_channel_finalize (GObject *object)
{
  GabbleMediaChannel *self = GABBLE_MEDIA_CHANNEL (object);
  GabbleMediaChannelPrivate *priv = self->priv;

  g_free (priv->object_path);
  tp_clear_pointer (&self->priv->deferred_tones, g_free);

  tp_group_mixin_finalize (object);
  tp_properties_mixin_finalize (object);

  G_OBJECT_CLASS (gabble_media_channel_parent_class)->finalize (object);
}


/**
 * gabble_media_channel_close_async:
 *
 * Implements D-Bus method Close
 * on interface org.freedesktop.Telepathy.Channel
 */
static void
gabble_media_channel_close_async (TpSvcChannel *iface,
                                  DBusGMethodInvocation *context)
{
  GabbleMediaChannel *self = GABBLE_MEDIA_CHANNEL (iface);

  if (DEBUGGING)
    {
      gchar *caller = dbus_g_method_get_sender (context);

      DEBUG ("called by %s", caller);
      g_free (caller);
    }

  gabble_media_channel_close (self);
  tp_svc_channel_return_from_close (context);
}

void
gabble_media_channel_close (GabbleMediaChannel *self)
{
  GabbleMediaChannelPrivate *priv = self->priv;

  DEBUG ("called on %p", self);

  if (!priv->closed)
    {
      priv->closed = TRUE;

      if (priv->session != NULL)
        gabble_jingle_session_terminate (priv->session,
            JINGLE_REASON_UNKNOWN, NULL, NULL);

      tp_svc_channel_emit_closed (self);
    }
}


/**
 * gabble_media_channel_get_channel_type
 *
 * Implements D-Bus method GetChannelType
 * on interface org.freedesktop.Telepathy.Channel
 */
static void
gabble_media_channel_get_channel_type (TpSvcChannel *iface,
                                       DBusGMethodInvocation *context)
{
  tp_svc_channel_return_from_get_channel_type (context,
      TP_IFACE_CHANNEL_TYPE_STREAMED_MEDIA);
}


/**
 * gabble_media_channel_get_handle
 *
 * Implements D-Bus method GetHandle
 * on interface org.freedesktop.Telepathy.Channel
 */
static void
gabble_media_channel_get_handle (TpSvcChannel *iface,
                                 DBusGMethodInvocation *context)
{
  GabbleMediaChannel *self = GABBLE_MEDIA_CHANNEL (iface);

  if (self->priv->initial_peer == 0)
    tp_svc_channel_return_from_get_handle (context, TP_HANDLE_TYPE_NONE, 0);
  else
    tp_svc_channel_return_from_get_handle (context, TP_HANDLE_TYPE_CONTACT,
        self->priv->initial_peer);
}


/**
 * gabble_media_channel_get_interfaces
 *
 * Implements D-Bus method GetInterfaces
 * on interface org.freedesktop.Telepathy.Channel
 */
static void
gabble_media_channel_get_interfaces (TpSvcChannel *iface,
                                     DBusGMethodInvocation *context)
{
  tp_svc_channel_return_from_get_interfaces (context,
      gabble_media_channel_interfaces);
}


/**
 * gabble_media_channel_get_session_handlers
 *
 * Implements D-Bus method GetSessionHandlers
 * on interface org.freedesktop.Telepathy.Channel.Interface.MediaSignalling
 */
static void
gabble_media_channel_get_session_handlers (TpSvcChannelInterfaceMediaSignalling *iface,
                                           DBusGMethodInvocation *context)
{
  GabbleMediaChannel *self = GABBLE_MEDIA_CHANNEL (iface);
  GabbleMediaChannelPrivate *priv;
  GPtrArray *ret;
  GType info_type = TP_STRUCT_TYPE_MEDIA_SESSION_HANDLER_INFO;

  g_assert (GABBLE_IS_MEDIA_CHANNEL (self));

  priv = self->priv;

  if (priv->session)
    {
      GValue handler = { 0, };
      TpHandle member;

      g_value_init (&handler, info_type);
      g_value_take_boxed (&handler,
          dbus_g_type_specialized_construct (info_type));

      g_object_get (priv->session,
                    "peer", &member,
                    NULL);

      dbus_g_type_struct_set (&handler,
          0, priv->object_path,
          1, "rtp",
          G_MAXUINT);

      ret = g_ptr_array_sized_new (1);
      g_ptr_array_add (ret, g_value_get_boxed (&handler));
    }
  else
    {
      ret = g_ptr_array_sized_new (0);
    }

  tp_svc_channel_interface_media_signalling_return_from_get_session_handlers (
      context, ret);
  g_ptr_array_foreach (ret, (GFunc) g_value_array_free, NULL);
  g_ptr_array_unref (ret);
}

/**
 * make_stream_list:
 *
 * Creates an array of MediaStreamInfo structs.
 *
 * Precondition: priv->session is non-NULL.
 */
static GPtrArray *
make_stream_list (GabbleMediaChannel *self,
                  guint len,
                  GabbleMediaStream **streams)
{
  GabbleMediaChannelPrivate *priv = self->priv;
  GPtrArray *ret;
  guint i;
  GType info_type = TP_STRUCT_TYPE_MEDIA_STREAM_INFO;

  g_assert (priv->session != NULL);

  ret = g_ptr_array_sized_new (len);

  for (i = 0; i < len; i++)
    {
      GValue entry = { 0, };
      guint id;
      TpHandle peer;
      TpMediaStreamType type;
      TpMediaStreamState connection_state;
      CombinedStreamDirection combined_direction;

      g_object_get (streams[i],
          "id", &id,
          "media-type", &type,
          "connection-state", &connection_state,
          "combined-direction", &combined_direction,
          NULL);

      g_object_get (priv->session, "peer", &peer, NULL);

      g_value_init (&entry, info_type);
      g_value_take_boxed (&entry,
          dbus_g_type_specialized_construct (info_type));

      dbus_g_type_struct_set (&entry,
          0, id,
          1, peer,
          2, type,
          3, connection_state,
          4, COMBINED_DIRECTION_GET_DIRECTION (combined_direction),
          5, COMBINED_DIRECTION_GET_PENDING_SEND (combined_direction),
          G_MAXUINT);

      g_ptr_array_add (ret, g_value_get_boxed (&entry));
    }

  return ret;
}

/**
 * gabble_media_channel_list_streams
 *
 * Implements D-Bus method ListStreams
 * on interface org.freedesktop.Telepathy.Channel.Type.StreamedMedia
 */
static void
gabble_media_channel_list_streams (TpSvcChannelTypeStreamedMedia *iface,
                                   DBusGMethodInvocation *context)
{
  GabbleMediaChannel *self = GABBLE_MEDIA_CHANNEL (iface);
  GabbleMediaChannelPrivate *priv;
  GPtrArray *ret;

  g_assert (GABBLE_IS_MEDIA_CHANNEL (self));

  priv = self->priv;

  /* If the session has not yet started, or has ended, return an empty array.
   */
  if (priv->session == NULL)
    {
      ret = g_ptr_array_new ();
    }
  else
    {
      ret = make_stream_list (self, priv->streams->len,
          (GabbleMediaStream **) priv->streams->pdata);
    }

  tp_svc_channel_type_streamed_media_return_from_list_streams (context, ret);
  g_ptr_array_foreach (ret, (GFunc) g_value_array_free, NULL);
  g_ptr_array_unref (ret);
}


static GabbleMediaStream *
_find_stream_by_id (GabbleMediaChannel *chan,
    guint stream_id,
    GError **error)
{
  GabbleMediaChannelPrivate *priv;
  guint i;

  g_assert (GABBLE_IS_MEDIA_CHANNEL (chan));

  priv = chan->priv;

  for (i = 0; i < priv->streams->len; i++)
    {
      GabbleMediaStream *stream = g_ptr_array_index (priv->streams, i);
      guint id;

      g_object_get (stream, "id", &id, NULL);
      if (id == stream_id)
        return stream;
    }

  g_set_error (error, TP_ERRORS, TP_ERROR_INVALID_ARGUMENT,
      "given stream id %u does not exist", stream_id);
  return NULL;
}

static GabbleMediaStream *
_find_stream_by_content (GabbleMediaChannel *chan,
    GabbleJingleContent *content)
{
  GabbleMediaChannelPrivate *priv;
  guint i;

  g_assert (GABBLE_IS_MEDIA_CHANNEL (chan));

  priv = chan->priv;

  for (i = 0; i < priv->streams->len; i++)
    {
      GabbleMediaStream *stream = g_ptr_array_index (priv->streams, i);
      GabbleJingleContent *c = GABBLE_JINGLE_CONTENT (
          gabble_media_stream_get_content (stream));

      if (content == c)
        return stream;
    }

  return NULL;
}

/**
 * gabble_media_channel_remove_streams
 *
 * Implements DBus method RemoveStreams
 * on interface org.freedesktop.Telepathy.Channel.Type.StreamedMedia
 */
static void
gabble_media_channel_remove_streams (TpSvcChannelTypeStreamedMedia *iface,
                                     const GArray * streams,
                                     DBusGMethodInvocation *context)
{
  GabbleMediaChannel *obj = GABBLE_MEDIA_CHANNEL (iface);
  GabbleMediaChannelPrivate *priv;
  GPtrArray *stream_objs;
  GError *error = NULL;
  guint i;

  g_assert (GABBLE_IS_MEDIA_CHANNEL (obj));

  priv = obj->priv;

  if (!gabble_jingle_session_can_modify_contents (priv->session))
    {
      GError e = { TP_ERRORS, TP_ERROR_NOT_IMPLEMENTED,
          "Streams can't be removed from Google Talk calls" };
      dbus_g_method_return_error (context, &e);
      return;
    }

  stream_objs = g_ptr_array_sized_new (streams->len);

  /* check that all stream ids are valid and at the same time build an array
   * of stream objects so we don't have to look them up again after verifying
   * all stream identifiers. */
  for (i = 0; i < streams->len; i++)
    {
      guint id = g_array_index (streams, guint, i);
      GabbleMediaStream *stream;
      guint j;

      stream = _find_stream_by_id (obj, id, &error);

      if (stream == NULL)
        goto OUT;

      /* make sure we don't allow the client to repeatedly remove the same
      stream */
      for (j = 0; j < stream_objs->len; j++)
        {
          GabbleMediaStream *tmp = g_ptr_array_index (stream_objs, j);

          if (tmp == stream)
            {
              stream = NULL;
              break;
            }
        }

      if (stream != NULL)
        g_ptr_array_add (stream_objs, stream);
    }

  /* groovy, it's all good dude, let's remove them */
  if (stream_objs->len > 0)
    {
      GabbleMediaStream *stream;
      GabbleJingleMediaRtp *c;

      for (i = 0; i < stream_objs->len; i++)
        {
          stream = g_ptr_array_index (stream_objs, i);
          c = gabble_media_stream_get_content (stream);

          /* FIXME: make sure session emits content-removed, on which we can
           * delete it from the list */
          gabble_jingle_session_remove_content (priv->session,
              (GabbleJingleContent *) c);
        }
    }

OUT:
  g_ptr_array_unref (stream_objs);

  if (error)
    {
      dbus_g_method_return_error (context, error);
      g_error_free (error);
    }
  else
    {
      tp_svc_channel_type_streamed_media_return_from_remove_streams (context);
    }
}


/**
 * gabble_media_channel_request_stream_direction
 *
 * Implements D-Bus method RequestStreamDirection
 * on interface org.freedesktop.Telepathy.Channel.Type.StreamedMedia
 */
static void
gabble_media_channel_request_stream_direction (TpSvcChannelTypeStreamedMedia *iface,
                                               guint stream_id,
                                               guint stream_direction,
                                               DBusGMethodInvocation *context)
{
  GabbleMediaChannel *self = GABBLE_MEDIA_CHANNEL (iface);
  GabbleMediaChannelPrivate *priv;
  GabbleMediaStream *stream;
  GError *error = NULL;

  g_assert (GABBLE_IS_MEDIA_CHANNEL (self));

  priv = self->priv;

  if (stream_direction > TP_MEDIA_STREAM_DIRECTION_BIDIRECTIONAL)
    {
      g_set_error (&error, TP_ERRORS, TP_ERROR_INVALID_ARGUMENT,
          "given stream direction %u is not valid", stream_direction);
      dbus_g_method_return_error (context, error);
      g_error_free (error);
      return;
    }

  stream = _find_stream_by_id (self, stream_id, &error);

  if (stream == NULL)
    {
      dbus_g_method_return_error (context, error);
      g_error_free (error);
      return;
    }

  DEBUG ("called (stream %s, direction %u)", stream->name, stream_direction);

  /* streams with no session? I think not... */
  g_assert (priv->session != NULL);

  if (stream_direction == TP_MEDIA_STREAM_DIRECTION_NONE)
    {
      if (gabble_jingle_session_can_modify_contents (priv->session))
        {
          GabbleJingleMediaRtp *c;

          DEBUG ("request for NONE direction; removing stream");

          c = gabble_media_stream_get_content (stream);
          gabble_jingle_session_remove_content (priv->session,
              (GabbleJingleContent *) c);

          tp_svc_channel_type_streamed_media_return_from_request_stream_direction (
              context);
        }
      else
        {
          GError e = { TP_ERRORS, TP_ERROR_NOT_IMPLEMENTED,
              "Stream direction can't be set to None in Google Talk calls" };
          DEBUG ("%s", e.message);
          dbus_g_method_return_error (context, &e);
        }

      return;
    }

  if (gabble_media_stream_change_direction (stream, stream_direction, &error))
    {
      tp_svc_channel_type_streamed_media_return_from_request_stream_direction (
          context);
    }
  else
    {
      dbus_g_method_return_error (context, error);
      g_error_free (error);
    }
}

typedef struct {
    /* number of streams requested == number of content objects */
    guint len;
    /* array of @len borrowed pointers */
    GabbleJingleContent **contents;
    /* accumulates borrowed pointers to streams. Initially @len NULL pointers;
     * when the stream for contents[i] is created, it is stored at streams[i].
     */
    GabbleMediaStream **streams;
    /* number of non-NULL elements in streams (0 <= satisfied <= contents) */
    guint satisfied;
    /* succeeded_cb(context, GPtrArray<TP_STRUCT_TYPE_MEDIA_STREAM_INFO>)
     * will be called if the stream request succeeds.
     */
    GFunc succeeded_cb;
    /* failed_cb(context, GError *) will be called if the stream request fails.
     */
    GFunc failed_cb;
    gpointer context;
} PendingStreamRequest;

static PendingStreamRequest *
pending_stream_request_new (GPtrArray *contents,
    GFunc succeeded_cb,
    GFunc failed_cb,
    gpointer context)
{
  PendingStreamRequest *p = g_slice_new0 (PendingStreamRequest);

  g_assert (succeeded_cb);
  g_assert (failed_cb);

  p->len = contents->len;
  p->contents = g_memdup (contents->pdata, contents->len * sizeof (gpointer));
  p->streams = g_new0 (GabbleMediaStream *, contents->len);
  p->satisfied = 0;
  p->succeeded_cb = succeeded_cb;
  p->failed_cb = failed_cb;
  p->context = context;

  return p;
}

static gboolean
pending_stream_request_maybe_satisfy (PendingStreamRequest *p,
                                      GabbleMediaChannel *channel,
                                      GabbleJingleContent *content,
                                      GabbleMediaStream *stream)
{
  guint i;

  for (i = 0; i < p->len; i++)
    {
      if (p->contents[i] == content)
        {
          g_assert (p->streams[i] == NULL);
          p->streams[i] = stream;

          if (++p->satisfied == p->len && p->context != NULL)
            {
              GPtrArray *ret = make_stream_list (channel, p->len, p->streams);

              p->succeeded_cb (p->context, ret);
              g_ptr_array_foreach (ret, (GFunc) g_value_array_free, NULL);
              g_ptr_array_unref (ret);
              p->context = NULL;
              return TRUE;
            }
        }
    }

  return FALSE;
}

static gboolean
pending_stream_request_maybe_fail (PendingStreamRequest *p,
                                   GabbleMediaChannel *channel,
                                   GabbleJingleContent *content)
{
  guint i;

  for (i = 0; i < p->len; i++)
    {
      if (content == p->contents[i])
        {
          GError e = { TP_ERRORS, TP_ERROR_NOT_AVAILABLE,
              "A stream was removed before it could be fully set up" };

          /* return early */
          p->failed_cb (p->context, &e);
          p->context = NULL;
          return TRUE;
        }
    }

  return FALSE;
}

static void
pending_stream_request_free (gpointer data)
{
  PendingStreamRequest *p = data;

  if (p->context != NULL)
    {
      GError e = { TP_ERRORS, TP_ERROR_CANCELLED,
          "The session terminated before the requested streams could be added"
      };

      p->failed_cb (p->context, &e);
    }

  g_free (p->contents);
  g_free (p->streams);

  g_slice_free (PendingStreamRequest, p);
}

static gboolean
_gabble_media_channel_request_contents (GabbleMediaChannel *chan,
                                        TpHandle peer,
                                        const GArray *media_types,
                                        GPtrArray **ret,
                                        GError **error)
{
  GabbleMediaChannelPrivate *priv = chan->priv;
  gboolean want_audio, want_video;
  JingleDialect dialect;
  guint idx;
  const gchar *peer_resource;
  const gchar *transport_ns = NULL;

  DEBUG ("called");

  want_audio = want_video = FALSE;

  for (idx = 0; idx < media_types->len; idx++)
    {
      guint media_type = g_array_index (media_types, guint, idx);

      if (media_type == TP_MEDIA_STREAM_TYPE_AUDIO)
        {
          want_audio = TRUE;
        }
      else if (media_type == TP_MEDIA_STREAM_TYPE_VIDEO)
        {
          want_video = TRUE;
        }
      else
        {
          g_set_error (error, TP_ERRORS, TP_ERROR_INVALID_ARGUMENT,
              "given media type %u is invalid", media_type);
          return FALSE;
        }
    }

  /* existing call; the recipient and the mode has already been decided */
  if (priv->session != NULL)
    {
      peer_resource = gabble_jingle_session_get_peer_resource (priv->session);

      if (peer_resource[0] != '\0')
        DEBUG ("existing call, using peer resource %s", peer_resource);
      else
        DEBUG ("existing call, using bare JID");

      /* is a google call... we have no other option */
      if (!gabble_jingle_session_can_modify_contents (priv->session))
        {
          g_set_error (error, TP_ERRORS, TP_ERROR_NOT_AVAILABLE,
              "Streams can't be added to ongoing Google Talk calls");
          return FALSE;
        }

      /* check if the resource supports it; FIXME - we assume only
       * one channel type (video or audio) will be added later */
      if (NULL == jingle_pick_best_content_type (priv->conn, peer,
          peer_resource,
          want_audio ? JINGLE_MEDIA_TYPE_AUDIO : JINGLE_MEDIA_TYPE_VIDEO))
        {
          g_set_error (error, TP_ERRORS, TP_ERROR_NOT_AVAILABLE,
              "member does not have the desired audio/video capabilities");

          return FALSE;
        }

      /* We assume we already picked the best possible transport ns for the
       * previous streams, so we just reuse that one */
        {
          GList *contents = gabble_jingle_session_get_contents (priv->session);
          GabbleJingleContent *c;

          /* If we have a session, we must have at least one content. */
          g_assert (contents != NULL);

          c = contents->data;
          g_list_free (contents);

          transport_ns = gabble_jingle_content_get_transport_ns (c);
        }
    }
  /* no existing call; we should choose a recipient and a mode */
  else
    {
      gchar *jid;

      DEBUG ("picking the best resource (want audio: %u, want video: %u",
            want_audio, want_video);

      g_assert (priv->streams->len == 0);

      if (!jingle_pick_best_resource (priv->conn, peer,
          want_audio, want_video, &transport_ns, &dialect, &peer_resource))
        {
          g_set_error (error, TP_ERRORS, TP_ERROR_NOT_CAPABLE,
              "member does not have the desired audio/video capabilities");
          return FALSE;
        }

      DEBUG ("Picking resource '%s' (transport: %s, dialect: %u)",
          peer_resource == NULL ? "(null)" : peer_resource,
          transport_ns, dialect);

      jid = gabble_peer_to_jid (priv->conn, peer, peer_resource);
      create_session (chan, peer, jid);
      g_free (jid);

      g_object_set (priv->session, "dialect", dialect, NULL);

      /* Change nat-traversal if we need to */
      if (!tp_strdiff (transport_ns, NS_JINGLE_TRANSPORT_ICEUDP))
        {
          DEBUG ("changing nat-traversal property to ice-udp");
          g_object_set (chan, "nat-traversal", "ice-udp", NULL);
        }
      else if (!tp_strdiff (transport_ns, NS_JINGLE_TRANSPORT_RAWUDP))
        {
          DEBUG ("changing nat-traversal property to raw-udp");
          g_object_set (chan, "nat-traversal", "none", NULL);
        }
    }

  /* check it's not a ridiculous number of streams */
  if ((priv->streams->len + media_types->len) > MAX_STREAMS)
    {
      g_set_error (error, TP_ERRORS, TP_ERROR_NOT_AVAILABLE,
          "I think that's quite enough streams already");
      return FALSE;
    }

  /* if we've got here, we're good to make the Jingle contents */

  *ret = g_ptr_array_sized_new (media_types->len);

  for (idx = 0; idx < media_types->len; idx++)
    {
      guint media_type = g_array_index (media_types, guint, idx);
      GabbleJingleContent *c;
      const gchar *content_ns;

      content_ns = jingle_pick_best_content_type (priv->conn, peer,
          peer_resource,
          media_type == TP_MEDIA_STREAM_TYPE_AUDIO ?
            JINGLE_MEDIA_TYPE_AUDIO : JINGLE_MEDIA_TYPE_VIDEO);

      /* if we got this far, resource should be capable enough, so we
       * should not fail in choosing ns */
      g_assert (content_ns != NULL);
      g_assert (transport_ns != NULL);

      DEBUG ("Creating new jingle content with ns %s : %s", content_ns, transport_ns);

      c = gabble_jingle_session_add_content (priv->session,
          media_type == TP_MEDIA_STREAM_TYPE_AUDIO ?
            JINGLE_MEDIA_TYPE_AUDIO : JINGLE_MEDIA_TYPE_VIDEO,
            JINGLE_CONTENT_SENDERS_BOTH, NULL, content_ns, transport_ns);

      /* The stream is created in "new-content" callback, and appended to
       * priv->streams. This is now guaranteed to happen asynchronously (adding
       * streams can take time due to the relay info lookup, and if it doesn't,
       * we use an idle so it does). */
      g_assert (c != NULL);
      g_ptr_array_add (*ret, c);
    }

  return TRUE;
}

/* user_data param is here so we match the GFunc prototype */
static void
destroy_request (struct _delayed_request_streams_ctx *ctx,
    gpointer user_data G_GNUC_UNUSED)
{
  GabbleMediaChannelPrivate *priv = ctx->chan->priv;

  if (ctx->unsure_period_ended_id)
    g_signal_handler_disconnect (priv->conn->presence_cache,
        ctx->unsure_period_ended_id);

  if (ctx->caps_disco_id)
    g_signal_handler_disconnect (priv->conn->presence_cache,
        ctx->caps_disco_id);

  if (ctx->context != NULL)
    {
      GError *error = NULL;
      g_set_error (&error, TP_ERRORS, TP_ERROR_NOT_AVAILABLE,
          "cannot add streams: peer has insufficient caps");
      ctx->failed_cb (ctx->context, error);
      g_error_free (error);
    }

  g_array_unref (ctx->types);
  g_slice_free (struct _delayed_request_streams_ctx, ctx);
}

static void
destroy_and_remove_request (struct _delayed_request_streams_ctx *ctx)
{
  GabbleMediaChannelPrivate *priv = ctx->chan->priv;

  destroy_request (ctx, NULL);
  g_ptr_array_remove_fast (priv->delayed_request_streams, ctx);
}

static void media_channel_request_streams (GabbleMediaChannel *self,
    TpHandle contact_handle,
    const GArray *types,
    GFunc succeeded_cb,
    GFunc failed_cb,
    gpointer context);

static gboolean
repeat_request (struct _delayed_request_streams_ctx *ctx)
{
  media_channel_request_streams (ctx->chan, ctx->contact_handle, ctx->types,
      ctx->succeeded_cb, ctx->failed_cb, ctx->context);

  ctx->context = NULL;
  destroy_and_remove_request (ctx);
  return FALSE;
}

static void
capabilities_discovered_cb (GabblePresenceCache *cache,
                            TpHandle handle,
                            struct _delayed_request_streams_ctx *ctx)
{
  /* If this isn't the contact we're waiting for, ignore the signal. */
  if (ctx->contact_handle != handle)
    return;

  /* If we're still unsure about this contact (most likely because there are
   * more cache caps pending), wait for them. */
  if (gabble_presence_cache_is_unsure (cache, handle))
    return;

  repeat_request (ctx);
}

static void
delay_stream_request (GabbleMediaChannel *chan,
                      guint contact_handle,
                      const GArray *types,
                      GFunc succeeded_cb,
                      GFunc failed_cb,
                      gpointer context)
{
  GabbleMediaChannelPrivate *priv = chan->priv;
  struct _delayed_request_streams_ctx *ctx =
    g_slice_new0 (struct _delayed_request_streams_ctx);

  ctx->chan = chan;
  ctx->contact_handle = contact_handle;
  ctx->succeeded_cb = succeeded_cb;
  ctx->failed_cb = failed_cb;
  ctx->context = context;
  ctx->types = g_array_sized_new (FALSE, FALSE, sizeof (guint), types->len);
  g_array_append_vals (ctx->types, types->data, types->len);

  ctx->caps_disco_id = g_signal_connect (priv->conn->presence_cache,
      "capabilities-discovered", G_CALLBACK (capabilities_discovered_cb),
      ctx);
  ctx->unsure_period_ended_id = g_signal_connect_swapped (
      priv->conn->presence_cache, "unsure-period-ended",
      G_CALLBACK (repeat_request), ctx);

  g_ptr_array_add (priv->delayed_request_streams, ctx);
}

static void
media_channel_request_streams (GabbleMediaChannel *self,
    TpHandle contact_handle,
    const GArray *types,
    GFunc succeeded_cb,
    GFunc failed_cb,
    gpointer context)
{
  GabbleMediaChannelPrivate *priv = self->priv;
  GPtrArray *contents;
  gboolean wait;
  PendingStreamRequest *psr;
  GError *error = NULL;

  if (types->len == 0)
    {
      GPtrArray *empty = g_ptr_array_sized_new (0);

      DEBUG ("no streams to request");
      succeeded_cb (context, empty);
      g_ptr_array_unref (empty);

      return;
    }

  /* If we know the caps haven't arrived yet, delay stream creation
   * and check again later. Else, give up. */
  if (!contact_is_media_capable (self, contact_handle, &wait, &error))
    {
      if (wait)
        {
          DEBUG ("Delaying RequestStreams until we get all caps from contact");
          delay_stream_request (self, contact_handle, types,
              succeeded_cb, failed_cb, context);
          g_error_free (error);
          return;
        }

      goto error;
    }

  if (priv->session != NULL)
    {
      TpHandle peer;

      g_object_get (priv->session,
          "peer", &peer,
          NULL);

      if (peer != contact_handle)
        {
          g_set_error (&error, TP_ERRORS, TP_ERROR_NOT_AVAILABLE,
              "cannot add streams for %u: this channel's peer is %u",
              contact_handle, peer);
          goto error;
        }
    }

  if (!_gabble_media_channel_request_contents (self, contact_handle, types,
        &contents, &error))
    goto error;

  psr = pending_stream_request_new (contents, succeeded_cb, failed_cb,
      context);
  priv->pending_stream_requests = g_list_prepend (priv->pending_stream_requests,
      psr);
  g_ptr_array_unref (contents);

  /* signal acceptance */
  gabble_jingle_session_accept (priv->session);

  return;

error:
  DEBUG ("returning error %u: %s", error->code, error->message);
  failed_cb (context, error);
  g_error_free (error);
}

/**
 * gabble_media_channel_request_streams
 *
 * Implements D-Bus method RequestStreams
 * on interface org.freedesktop.Telepathy.Channel.Type.StreamedMedia
 */
static void
gabble_media_channel_request_streams (TpSvcChannelTypeStreamedMedia *iface,
                                      guint contact_handle,
                                      const GArray *types,
                                      DBusGMethodInvocation *context)
{
  GabbleMediaChannel *self = GABBLE_MEDIA_CHANNEL (iface);
  TpBaseConnection *base_conn = (TpBaseConnection *) self->priv->conn;
  TpHandleRepoIface *contact_handles = tp_base_connection_get_handles (
      base_conn, TP_HANDLE_TYPE_CONTACT);
  GError *error = NULL;

  if (!tp_handle_is_valid (contact_handles, contact_handle, &error))
    {
      DEBUG ("that's not a handle, sonny! (%u)", contact_handle);
      dbus_g_method_return_error (context, error);
      g_error_free (error);
      return;
    }
  else
    {
      /* FIXME: disallow this if we've put the peer on hold? */

      media_channel_request_streams (self, contact_handle, types,
          (GFunc) tp_svc_channel_type_streamed_media_return_from_request_streams,
          (GFunc) dbus_g_method_return_error,
          context);
    }
}

/**
 * gabble_media_channel_request_initial_streams:
 * @chan: an outgoing call, which must have just been constructed.
 * @succeeded_cb: called with arguments @user_data and a GPtrArray of
 *                TP_STRUCT_TYPE_MEDIA_STREAM_INFO if the request succeeds.
 * @failed_cb: called with arguments @user_data and a GError * if the request
 *             fails.
 * @user_data: context for the callbacks.
 *
 * Request streams corresponding to the values of InitialAudio and InitialVideo
 * in the channel request.
 */
void
gabble_media_channel_request_initial_streams (GabbleMediaChannel *chan,
    GFunc succeeded_cb,
    GFunc failed_cb,
    gpointer user_data)
{
  GabbleMediaChannelPrivate *priv = chan->priv;
  GArray *types = g_array_sized_new (FALSE, FALSE, sizeof (guint), 2);
  guint media_type;

  /* This has to be an outgoing call... */
  g_assert (priv->creator == priv->conn->parent.self_handle);
  /* ...which has just been constructed. */
  g_assert (priv->session == NULL);

  if (priv->initial_peer == 0)
    {
      /* This is a ye olde anonymous channel, so InitialAudio/Video should be
       * impossible.
       */
      g_assert (!priv->initial_audio);
      g_assert (!priv->initial_video);
    }

  if (priv->initial_audio)
    {
      media_type = TP_MEDIA_STREAM_TYPE_AUDIO;
      g_array_append_val (types, media_type);
    }

  if (priv->initial_video)
    {
      media_type = TP_MEDIA_STREAM_TYPE_VIDEO;
      g_array_append_val (types, media_type);
    }

  media_channel_request_streams (chan, priv->initial_peer, types,
      succeeded_cb, failed_cb, user_data);

  g_array_unref (types);
}

static gboolean
contact_is_media_capable (GabbleMediaChannel *chan,
    TpHandle peer,
    gboolean *wait_ret,
    GError **error)
{
  GabbleMediaChannelPrivate *priv = chan->priv;
  GabblePresence *presence;
  TpBaseConnection *conn = (TpBaseConnection *) priv->conn;
  TpHandleRepoIface *contact_handles = tp_base_connection_get_handles (
      conn, TP_HANDLE_TYPE_CONTACT);
  gboolean wait = FALSE;

  presence = gabble_presence_cache_get (priv->conn->presence_cache, peer);

  if (presence != NULL)
    {
      const GabbleCapabilitySet *caps = gabble_presence_peek_caps (presence);

      if (gabble_capability_set_has_one (caps,
            gabble_capabilities_get_any_audio_video ()))
        return TRUE;
    }

  /* Okay, they're not capable (yet). Let's figure out whether we should wait,
   * and return an appropriate error.
   */
  if (gabble_presence_cache_is_unsure (priv->conn->presence_cache, peer))
    {
      DEBUG ("presence cache is still unsure about handle %u", peer);
      wait = TRUE;
    }
  else if (!priv->tried_decloaking &&
      gabble_presence_cache_request_decloaking (priv->conn->presence_cache,
        peer, "media"))
    {
      /* only ask to decloak at most once per call */
      priv->tried_decloaking = TRUE;
      DEBUG ("asked handle %u to decloak, let's see what they do", peer);
      wait = TRUE;
    }

  if (wait_ret != NULL)
    *wait_ret = wait;

  if (presence == NULL)
    g_set_error (error, TP_ERRORS, TP_ERROR_OFFLINE,
        "contact %d (%s) has no presence available", peer,
        tp_handle_inspect (contact_handles, peer));
  else
    g_set_error (error, TP_ERRORS, TP_ERROR_NOT_CAPABLE,
        "contact %d (%s) doesn't have sufficient media caps", peer,
        tp_handle_inspect (contact_handles, peer));

  return FALSE;
}

static gboolean
gabble_media_channel_add_member (GObject *obj,
    TpHandle handle,
    const gchar *message,
    GError **error)
{
  GabbleMediaChannel *chan = GABBLE_MEDIA_CHANNEL (obj);
  GabbleMediaChannelPrivate *priv = chan->priv;
  TpGroupMixin *mixin = TP_GROUP_MIXIN (obj);
  TpIntSet *set;

  /* did we create this channel? */
  if (priv->creator == mixin->self_handle)
    {
      GError *error_ = NULL;
      gboolean wait;

      /* yes: check we don't have a peer already, and if not add this one to
       * remote pending (but don't send an invitation yet).
       */
      if (priv->session != NULL)
        {
          TpHandle peer;

          g_object_get (priv->session, "peer", &peer, NULL);

          if (peer != handle)
            {
              g_set_error (error, TP_ERRORS, TP_ERROR_NOT_AVAILABLE,
                  "handle %u cannot be added: this channel's peer is %u",
                  handle, peer);
              return FALSE;
            }
        }

      /* We can't delay the request at this time, but if there's a chance
       * the caps might be available later, we'll add the contact and
       * hope for the best. */
      if (!contact_is_media_capable (chan, handle, &wait, &error_))
        {
          if (wait)
            {
              DEBUG ("contact %u caps still pending, adding anyways", handle);
              g_error_free (error_);
            }
          else
            {
              DEBUG ("%u: %s", error_->code, error_->message);
              g_propagate_error (error, error_);
              return FALSE;
            }
        }

      /* make the peer remote pending */
      set = tp_intset_new_containing (handle);
      tp_group_mixin_change_members (obj, "", NULL, NULL, NULL, set,
          mixin->self_handle, TP_CHANNEL_GROUP_CHANGE_REASON_INVITED);
      tp_intset_destroy (set);

      /* and remove CanAdd, since it was only here to allow this deprecated
       * API. */
      tp_group_mixin_change_flags (obj, 0, TP_CHANNEL_GROUP_FLAG_CAN_ADD);

      return TRUE;
    }
  else
    {
      /* no: has a session been created, is the handle being added ours,
       *     and are we in local pending? (call answer) */
      if (priv->session &&
          handle == mixin->self_handle &&
          tp_handle_set_is_member (mixin->local_pending, handle))
        {
          /* is the call on hold? */
          if (priv->hold_state != TP_LOCAL_HOLD_STATE_UNHELD)
            {
              g_set_error (error, TP_ERRORS, TP_ERROR_NOT_AVAILABLE,
                  "Can't answer a call while it's on hold");
              return FALSE;
            }

          /* make us a member */
          set = tp_intset_new_containing (handle);
          tp_group_mixin_change_members (obj, "", set, NULL, NULL, NULL,
              handle, TP_CHANNEL_GROUP_CHANGE_REASON_NONE);
          tp_intset_destroy (set);

          /* accept any local pending sends */
          g_ptr_array_foreach (priv->streams,
              (GFunc) gabble_media_stream_accept_pending_local_send, NULL);

          /* signal acceptance */
          gabble_jingle_session_accept (priv->session);

          return TRUE;
        }
    }

  g_set_error (error, TP_ERRORS, TP_ERROR_NOT_AVAILABLE,
      "handle %u cannot be added in the current state", handle);
  return FALSE;
}

static gboolean
gabble_media_channel_remove_member (GObject *obj,
                                    TpHandle handle,
                                    const gchar *message,
                                    TpChannelGroupChangeReason reason,
                                    GError **error)
{
  GabbleMediaChannel *chan = GABBLE_MEDIA_CHANNEL (obj);
  GabbleMediaChannelPrivate *priv = chan->priv;
  TpGroupMixin *mixin = TP_GROUP_MIXIN (obj);

  /* We don't set CanRemove, and did allow self removal. So tp-glib should
   * ensure this.
   */
  g_assert (handle == mixin->self_handle);

  /* Closing up might make GabbleMediaFactory release its ref. */
  g_object_ref (chan);

  if (priv->session == NULL)
    {
      /* The call didn't even start yet; close up. */
      gabble_media_channel_close (chan);
    }
  else
    {
      JingleReason jingle_reason = JINGLE_REASON_UNKNOWN;

      switch (reason)
        {
        case TP_CHANNEL_GROUP_CHANGE_REASON_NONE:
          jingle_reason = JINGLE_REASON_UNKNOWN;
          break;
        case TP_CHANNEL_GROUP_CHANGE_REASON_OFFLINE:
          jingle_reason = JINGLE_REASON_GONE;
          break;
        case TP_CHANNEL_GROUP_CHANGE_REASON_BUSY:
          jingle_reason = JINGLE_REASON_BUSY;
          break;
        case TP_CHANNEL_GROUP_CHANGE_REASON_ERROR:
          jingle_reason = JINGLE_REASON_GENERAL_ERROR;
          break;
        case TP_CHANNEL_GROUP_CHANGE_REASON_NO_ANSWER:
          jingle_reason = JINGLE_REASON_TIMEOUT;
          break;
        default:
          g_set_error (error, TP_ERRORS, TP_ERROR_INVALID_ARGUMENT,
              "%u doesn't make sense as a reason to end a call", reason);
          g_object_unref (chan);
          return FALSE;
        }

      gabble_jingle_session_terminate (priv->session, jingle_reason, message,
          error);
    }

  /* Remove CanAdd if it was there for the deprecated anonymous channel
   * semantics, since the channel will go away RSN. */
  tp_group_mixin_change_flags (obj, 0, TP_CHANNEL_GROUP_FLAG_CAN_ADD);

  g_object_unref (chan);

  return TRUE;
}

/**
 * copy_stream_list:
 *
 * Returns a copy of priv->streams. This is used when applying a function to
 * all streams that could result in them being closed, to avoid stream_close_cb
 * modifying the list being iterated.
 */
static GPtrArray *
copy_stream_list (GabbleMediaChannel *channel)
{
  return gabble_g_ptr_array_copy (channel->priv->streams);
}


/* return TRUE when the jingle reason is reason enough to raise a
 * StreamError */
static gboolean
extract_media_stream_error_from_jingle_reason (JingleReason jingle_reason,
    TpMediaStreamError *stream_error)
{
  TpMediaStreamError _stream_error;

  /* TODO: Make a better mapping with more distinction of possible errors */
  switch (jingle_reason)
    {
    case JINGLE_REASON_CONNECTIVITY_ERROR:
      _stream_error = TP_MEDIA_STREAM_ERROR_NETWORK_ERROR;
      break;
    case JINGLE_REASON_MEDIA_ERROR:
      _stream_error = TP_MEDIA_STREAM_ERROR_MEDIA_ERROR;
      break;
    case JINGLE_REASON_FAILED_APPLICATION:
      _stream_error = TP_MEDIA_STREAM_ERROR_CODEC_NEGOTIATION_FAILED;
      break;
    case JINGLE_REASON_GENERAL_ERROR:
      _stream_error = TP_MEDIA_STREAM_ERROR_UNKNOWN;
      break;
    default:
      {
        if (stream_error != NULL)
          *stream_error =  TP_MEDIA_STREAM_ERROR_UNKNOWN;

        return FALSE;
      }
    }

  if (stream_error != NULL)
    *stream_error = _stream_error;

  return TRUE;
}

static JingleReason
media_stream_error_to_jingle_reason (TpMediaStreamError stream_error)
{
  switch (stream_error)
    {
    case TP_MEDIA_STREAM_ERROR_NETWORK_ERROR:
      return JINGLE_REASON_CONNECTIVITY_ERROR;
    case TP_MEDIA_STREAM_ERROR_MEDIA_ERROR:
      return  JINGLE_REASON_MEDIA_ERROR;
    case TP_MEDIA_STREAM_ERROR_CODEC_NEGOTIATION_FAILED:
      return JINGLE_REASON_FAILED_APPLICATION;
    default:
      return JINGLE_REASON_GENERAL_ERROR;
    }
}

static TpChannelGroupChangeReason
jingle_reason_to_group_change_reason (JingleReason jingle_reason)
{
  switch (jingle_reason)
    {
    case JINGLE_REASON_BUSY:
      return TP_CHANNEL_GROUP_CHANGE_REASON_BUSY;
    case JINGLE_REASON_GONE:
      return TP_CHANNEL_GROUP_CHANGE_REASON_OFFLINE;
    case JINGLE_REASON_TIMEOUT:
      return TP_CHANNEL_GROUP_CHANGE_REASON_NO_ANSWER;
    case JINGLE_REASON_CONNECTIVITY_ERROR:
    case JINGLE_REASON_FAILED_APPLICATION:
    case JINGLE_REASON_FAILED_TRANSPORT:
    case JINGLE_REASON_GENERAL_ERROR:
    case JINGLE_REASON_MEDIA_ERROR:
    case JINGLE_REASON_SECURITY_ERROR:
    case JINGLE_REASON_INCOMPATIBLE_PARAMETERS:
    case JINGLE_REASON_UNSUPPORTED_APPLICATIONS:
    case JINGLE_REASON_UNSUPPORTED_TRANSPORTS:
      return TP_CHANNEL_GROUP_CHANGE_REASON_ERROR;
    default:
      return TP_CHANNEL_GROUP_CHANGE_REASON_NONE;
    }
}

static void
session_terminated_cb (GabbleJingleSession *session,
                       gboolean local_terminator,
                       JingleReason jingle_reason,
                       const gchar *text,
                       gpointer user_data)
{
  GabbleMediaChannel *channel = (GabbleMediaChannel *) user_data;
  GabbleMediaChannelPrivate *priv = channel->priv;
  TpGroupMixin *mixin = TP_GROUP_MIXIN (channel);
  guint terminator;
  JingleState state;
  TpHandle peer;
  TpIntSet *set;

  DEBUG ("called");

  g_object_get (session,
                "state", &state,
                "peer", &peer,
                NULL);

  if (local_terminator)
      terminator = mixin->self_handle;
  else
      terminator = peer;

  set = tp_intset_new ();

  /* remove us and the peer from the member list */
  tp_intset_add (set, mixin->self_handle);
  tp_intset_add (set, peer);

  tp_group_mixin_change_members ((GObject *) channel,
      text, NULL, set, NULL, NULL, terminator,
      jingle_reason_to_group_change_reason (jingle_reason));

  tp_intset_destroy (set);

  /* Ignore any Google relay session responses we're waiting for. */
  g_list_foreach (priv->stream_creation_datas, stream_creation_data_cancel,
      NULL);

  /* any contents that we were waiting for have now lost */
  g_list_foreach (priv->pending_stream_requests,
      (GFunc) pending_stream_request_free, NULL);
  g_list_free (priv->pending_stream_requests);
  priv->pending_stream_requests = NULL;

  {
    GPtrArray *tmp = copy_stream_list (channel);
    guint i;
    TpMediaStreamError stream_error = TP_MEDIA_STREAM_ERROR_UNKNOWN;
    gboolean is_error = extract_media_stream_error_from_jingle_reason (
        jingle_reason, &stream_error);

    for (i = 0; i < tmp->len; i++)
      {
        GabbleMediaStream *stream = tmp->pdata[i];

        if (is_error)
          {
            guint id;

            DEBUG ("emitting stream error");

            g_object_get (stream, "id", &id, NULL);
            tp_svc_channel_type_streamed_media_emit_stream_error (channel, id,
                stream_error, text);
          }

        gabble_media_stream_close (stream);
      }

    /* All the streams should have closed. */
    g_assert (priv->streams->len == 0);

    g_ptr_array_unref (tmp);
  }

  /* remove the session */
  tp_clear_object (&priv->session);

  /* close us if we aren't already closed */
  if (!priv->closed)
    {
      DEBUG ("calling media channel close from session terminated cb");
      gabble_media_channel_close (channel);
    }
}


static void
session_state_changed_cb (GabbleJingleSession *session,
                          GParamSpec *arg1,
                          GabbleMediaChannel *channel)
{
  GObject *as_object = (GObject *) channel;
  GabbleMediaChannelPrivate *priv = channel->priv;
  TpGroupMixin *mixin = TP_GROUP_MIXIN (channel);
  JingleState state;
  TpHandle peer;
  TpIntSet *set;

  DEBUG ("called");

  g_object_get (session,
                "state", &state,
                "peer", &peer,
                NULL);

  set = tp_intset_new_containing (peer);

  if (state >= JINGLE_STATE_PENDING_INITIATE_SENT &&
      state < JINGLE_STATE_ACTIVE &&
      !tp_handle_set_is_member (mixin->members, peer))
    {
      /* The first time we send anything to the other user, they materialise
       * in remote-pending if necessary */

      tp_group_mixin_change_members (as_object, "", NULL, NULL, NULL, set,
          mixin->self_handle, TP_CHANNEL_GROUP_CHANGE_REASON_INVITED);

      /* Remove CanAdd if it happened to be there to support deprecated
       * RequestChannel(..., 0) followed by AddMembers([h], ...) semantics.
       */
      tp_group_mixin_change_flags (as_object, 0, TP_CHANNEL_GROUP_FLAG_CAN_ADD);
    }

  if (state == JINGLE_STATE_ACTIVE &&
      priv->creator == mixin->self_handle)
    {

      DEBUG ("adding peer to the member list and updating flags");

      /* add the peer to the member list */
      tp_group_mixin_change_members (as_object, "", set, NULL, NULL, NULL,
          peer, TP_CHANNEL_GROUP_CHANGE_REASON_NONE);
    }

  tp_intset_destroy (set);
}

static void
stream_close_cb (GabbleMediaStream *stream,
                 GabbleMediaChannel *chan)
{
  GabbleMediaChannelPrivate *priv = chan->priv;
  guint id, i;
  gboolean still_have_audio = FALSE;

  g_assert (GABBLE_IS_MEDIA_CHANNEL (chan));

  g_object_get (stream,
      "id", &id,
      NULL);

  tp_svc_channel_type_streamed_media_emit_stream_removed (chan, id);

  if (g_ptr_array_remove (priv->streams, stream))
    g_object_unref (stream);
  else
    g_warning ("stream %p (%s) removed, but it wasn't in priv->streams!",
        stream, stream->name);

  gabble_media_channel_hold_stream_closed (chan, stream);

  for (i = 0; i < priv->streams->len; i++)
    {
      GabbleMediaStream *other = g_ptr_array_index (priv->streams, i);

      if (gabble_media_stream_get_media_type (other) ==
          TP_MEDIA_STREAM_TYPE_AUDIO)
        {
          still_have_audio = TRUE;
        }
    }

  if (priv->have_some_audio && !still_have_audio)
    {
      /* the last audio stream just closed */
      tp_dtmf_player_cancel (priv->dtmf_player);
    }

  priv->have_some_audio = still_have_audio;
}

static void
stream_error_cb (GabbleMediaStream *stream,
                 TpMediaStreamError errno,
                 const gchar *message,
                 GabbleMediaChannel *chan)
{
  GabbleMediaChannelPrivate *priv = chan->priv;
  GabbleJingleMediaRtp *c;
  GList *contents;
  guint id;

  /* emit signal */
  g_object_get (stream, "id", &id, NULL);
  tp_svc_channel_type_streamed_media_emit_stream_error (chan, id, errno,
      message);

  contents = gabble_jingle_session_get_contents (priv->session);

  if (gabble_jingle_session_can_modify_contents (priv->session) &&
      g_list_length (contents) > 1)
    {
      /* remove stream from session (removal will be signalled
       * so we can dispose of the stream)
       */
      c = gabble_media_stream_get_content (stream);

      if (errno == TP_MEDIA_STREAM_ERROR_CODEC_NEGOTIATION_FAILED)
        gabble_jingle_content_reject ((GabbleJingleContent *) c,
            JINGLE_REASON_FAILED_APPLICATION);
      else
        gabble_jingle_session_remove_content (priv->session,
            (GabbleJingleContent *) c);
    }
  else
    {
      /* We can't remove the content, or it's the only one left; let's
       * terminate the call. (The alternative is to carry on the call with
       * only audio/video, which will look or sound bad to the Google
       * Talk-using peer.)
       */
      DEBUG ("Terminating call in response to stream error");
      gabble_jingle_session_terminate (priv->session,
          media_stream_error_to_jingle_reason (errno), message, NULL);
    }

  g_list_free (contents);
}

static void
stream_state_changed_cb (GabbleMediaStream *stream,
                         GParamSpec *pspec,
                         GabbleMediaChannel *chan)
{
  guint id;
  TpMediaStreamState connection_state;

  g_object_get (stream,
      "id", &id,
      "connection-state", &connection_state,
      NULL);

  tp_svc_channel_type_streamed_media_emit_stream_state_changed (chan,
      id, connection_state);
}

static void
stream_direction_changed_cb (GabbleMediaStream *stream,
                             GParamSpec *pspec,
                             GabbleMediaChannel *chan)
{
  guint id;
  CombinedStreamDirection combined;
  TpMediaStreamDirection direction;
  TpMediaStreamPendingSend pending_send;

  g_object_get (stream,
      "id", &id,
      "combined-direction", &combined,
      NULL);

  direction = COMBINED_DIRECTION_GET_DIRECTION (combined);
  pending_send = COMBINED_DIRECTION_GET_PENDING_SEND (combined);

  DEBUG ("direction: %u, pending_send: %u", direction, pending_send);

  tp_svc_channel_type_streamed_media_emit_stream_direction_changed (
      chan, id, direction, pending_send);
}

static void
construct_stream (GabbleMediaChannel *chan,
                  GabbleJingleContent *c,
                  const gchar *name,
                  const gchar *nat_traversal,
                  const GPtrArray *relays,
                  gboolean initial)
{
  GObject *chan_o = (GObject *) chan;
  GabbleMediaChannelPrivate *priv = chan->priv;
  GabbleMediaStream *stream;
  TpMediaStreamType mtype;
  guint id;
  gchar *object_path;
  gboolean local_hold = (priv->hold_state == TP_LOCAL_HOLD_STATE_HELD ||
      priv->hold_state == TP_LOCAL_HOLD_STATE_PENDING_HOLD);

  id = priv->next_stream_id++;

  object_path = g_strdup_printf ("%s/MediaStream%u",
      priv->object_path, id);

  stream = gabble_media_stream_new (object_path, c, name, id,
      nat_traversal, relays, local_hold);
  mtype = gabble_media_stream_get_media_type (stream);

  if (mtype == TP_MEDIA_STREAM_TYPE_AUDIO)
    {
      gabble_media_stream_add_dtmf_player (stream, priv->dtmf_player);
      priv->have_some_audio = TRUE;
    }

  DEBUG ("%p: created new MediaStream %p for content '%s'", chan, stream, name);

  g_ptr_array_add (priv->streams, stream);

  /* if any RequestStreams call was waiting for a stream to be created for
   * that content, return from it successfully */
    {
      GList *l = priv->pending_stream_requests;

      while (l != NULL)
        {
          if (pending_stream_request_maybe_satisfy (l->data,
                chan, c, stream))
            {
              GList *dead = l;

              pending_stream_request_free (dead->data);

              l = dead->next;
              priv->pending_stream_requests = g_list_delete_link (
                  priv->pending_stream_requests, dead);
            }
          else
            {
              l = l->next;
            }
        }
    }

  gabble_signal_connect_weak (stream, "close", (GCallback) stream_close_cb,
      chan_o);
  gabble_signal_connect_weak (stream, "error", (GCallback) stream_error_cb,
      chan_o);
  gabble_signal_connect_weak (stream, "notify::connection-state",
      (GCallback) stream_state_changed_cb, chan_o);
  gabble_signal_connect_weak (stream, "notify::combined-direction",
      (GCallback) stream_direction_changed_cb, chan_o);

  if (initial)
    {
      /* If we accepted the call, then automagically accept the initial streams
       * when they pop up */
      if (tp_handle_set_is_member (chan->group.members,
          chan->group.self_handle))
        {
          gabble_media_stream_accept_pending_local_send (stream);
        }
    }

  DEBUG ("emitting StreamAdded with type '%s'",
    mtype == TP_MEDIA_STREAM_TYPE_AUDIO ? "audio" : "video");

  tp_svc_channel_type_streamed_media_emit_stream_added (
      chan, id, priv->session->peer, mtype);

  /* StreamAdded does not include the stream's direction and pending send
   * information, so we call the notify::combined-direction handler in order to
   * emit StreamDirectionChanged for the initial state.
   */
  stream_direction_changed_cb (stream, NULL, chan);

  gabble_media_channel_hold_new_stream (chan, stream,
      GABBLE_JINGLE_MEDIA_RTP (c));

  if (priv->ready)
    {
      /* all of the streams are bidirectional from farsight's point of view, it's
       * just in the signalling they change */
      DEBUG ("emitting MediaSessionHandler:NewStreamHandler signal for stream %d", id);
      tp_svc_media_session_handler_emit_new_stream_handler (chan,
        object_path, id, mtype, TP_MEDIA_STREAM_DIRECTION_BIDIRECTIONAL);
    }

  g_free (object_path);
}

static void
stream_creation_data_cancel (gpointer p,
                             gpointer unused)
{
  StreamCreationData *d = p;

  tp_clear_object (&d->content);
}

static void
stream_creation_data_free (gpointer p)
{
  StreamCreationData *d = p;

  g_free (d->name);

  if (d->content != NULL)
    {
      g_signal_handler_disconnect (d->content, d->removed_id);
      g_object_unref (d->content);
    }

  if (d->self != NULL)
    {
      GabbleMediaChannelPrivate *priv = d->self->priv;

      g_object_remove_weak_pointer (G_OBJECT (d->self), (gpointer *) &d->self);
      priv->stream_creation_datas = g_list_remove (
          priv->stream_creation_datas, d);
    }

  g_slice_free (StreamCreationData, d);
}

static gboolean
construct_stream_later_cb (gpointer user_data)
{
  StreamCreationData *d = user_data;

  if (d->content != NULL && d->self != NULL)
    construct_stream (d->self, d->content, d->name, d->nat_traversal, NULL,
      d->initial);

  return FALSE;
}

static void
google_relay_session_cb (GPtrArray *relays,
                         gpointer user_data)
{
  StreamCreationData *d = user_data;

  if (d->content != NULL && d->self != NULL)
    construct_stream (d->self, d->content, d->name, d->nat_traversal, relays,
      d->initial);

  stream_creation_data_free (d);
}

static void
content_removed_cb (GabbleJingleContent *content,
                    StreamCreationData *d)
{

  if (d->content == NULL)
    return;

  if (d->self != NULL)
    {
      GList *l = d->self->priv->pending_stream_requests;

      /* if any RequestStreams call was waiting for a stream to be created for
       * that content, return from it unsuccessfully */
      while (l != NULL)
        {
          if (pending_stream_request_maybe_fail (l->data,
                d->self, d->content))
            {
              GList *dead = l;

              pending_stream_request_free (dead->data);

              l = dead->next;
              d->self->priv->pending_stream_requests = g_list_delete_link (
                  d->self->priv->pending_stream_requests, dead);
            }
          else
            {
              l = l->next;
            }
        }
    }

  g_signal_handler_disconnect (d->content, d->removed_id);
  g_object_unref (d->content);
  d->content = NULL;
}

static void
create_stream_from_content (GabbleMediaChannel *self,
                            GabbleJingleContent *c,
                            gboolean initial)
{
  gchar *name;
  StreamCreationData *d;

  g_object_get (c,
      "name", &name,
      NULL);

  if (G_OBJECT_TYPE (c) != GABBLE_TYPE_JINGLE_MEDIA_RTP)
    {
      DEBUG ("ignoring non MediaRtp content '%s'", name);
      g_free (name);
      return;
    }

  d = g_slice_new0 (StreamCreationData);

  d->self = self;
  d->name = name;
  d->content = g_object_ref (c);
  d->initial = initial;

  g_object_add_weak_pointer (G_OBJECT (d->self), (gpointer *) &d->self);

  /* If the content gets removed before we've finished looking up its
   * relay, we need to cancel the creation of the stream,
   * and make any PendingStreamRequests fail */
  d->removed_id = g_signal_connect (c, "removed",
      G_CALLBACK (content_removed_cb), d);

  self->priv->stream_creation_datas = g_list_prepend (
      self->priv->stream_creation_datas, d);

  switch (gabble_jingle_content_get_transport_type (c))
    {
      case JINGLE_TRANSPORT_GOOGLE_P2P:
        /* See if our server is Google, and if it is, ask them for a relay.
         * We ask for enough relays for 2 components (RTP and RTCP) since we
         * don't yet know whether there will be RTCP. */
        d->nat_traversal = "gtalk-p2p";
        DEBUG ("Attempting to create Google relay session");
        gabble_jingle_factory_create_google_relay_session (
            self->priv->conn->jingle_factory, 2, google_relay_session_cb, d);
        return;

      case JINGLE_TRANSPORT_ICE_UDP:
        d->nat_traversal = "ice-udp";
        break;

      default:
        d->nat_traversal = "none";
    }

  /* If we got here, just create the stream (do it asynchronously so that the
   * behaviour is the same in each case) */
  g_idle_add_full (G_PRIORITY_DEFAULT, construct_stream_later_cb,
      d, stream_creation_data_free);
}

static void
session_content_rejected_cb (GabbleJingleSession *session,
    GabbleJingleContent *c, JingleReason reason, const gchar *message,
    gpointer user_data)
{
  GabbleMediaChannel *chan = GABBLE_MEDIA_CHANNEL (user_data);
  GabbleMediaStream *stream = _find_stream_by_content (chan, c);
  TpMediaStreamError stream_error = TP_MEDIA_STREAM_ERROR_UNKNOWN;
  guint id = 0;

  DEBUG (" ");

  g_return_if_fail (stream != NULL);

  g_object_get (stream,
      "id", &id,
      NULL);

  extract_media_stream_error_from_jingle_reason (reason, &stream_error);

  tp_svc_channel_type_streamed_media_emit_stream_error (chan, id, stream_error,
      message);
}

static void
session_new_content_cb (GabbleJingleSession *session,
    GabbleJingleContent *c, gpointer user_data)
{
  GabbleMediaChannel *chan = GABBLE_MEDIA_CHANNEL (user_data);

  DEBUG ("called");

  create_stream_from_content (chan, c, FALSE);
}

static void
_emit_new_stream (GabbleMediaChannel *chan,
                  GabbleMediaStream *stream)
{
  gchar *object_path;
  guint id, media_type;

  g_object_get (stream,
                "object-path", &object_path,
                "id", &id,
                "media-type", &media_type,
                NULL);

  /* all of the streams are bidirectional from farsight's point of view, it's
   * just in the signalling they change */
  DEBUG ("emitting MediaSessionHandler:NewStreamHandler signal for %s stream %d ",
      media_type == TP_MEDIA_STREAM_TYPE_AUDIO ? "audio" : "video", id);
  tp_svc_media_session_handler_emit_new_stream_handler (chan,
      object_path, id, media_type, TP_MEDIA_STREAM_DIRECTION_BIDIRECTIONAL);

  g_free (object_path);
}


static void
gabble_media_channel_ready (TpSvcMediaSessionHandler *iface,
                            DBusGMethodInvocation *context)
{
  GabbleMediaChannel *self = GABBLE_MEDIA_CHANNEL (iface);
  GabbleMediaChannelPrivate *priv = self->priv;

  if (priv->session == NULL)
    {
      /* This could also be because someone called Ready() before the
       * SessionHandler was announced. But the fact that the SessionHandler is
       * actually also the Channel, and thus this method is available before
       * NewSessionHandler is emitted, is an implementation detail. So the
       * error message describes the only legitimate situation in which this
       * could arise.
       */
      GError e = { TP_ERRORS, TP_ERROR_NOT_AVAILABLE, "call has already ended" };

      DEBUG ("no session, returning an error.");
      dbus_g_method_return_error (context, &e);
      return;
    }

  if (!priv->ready)
    {
      guint i;

      DEBUG ("emitting NewStreamHandler for each stream");

      priv->ready = TRUE;

      for (i = 0; i < priv->streams->len; i++)
        _emit_new_stream (self, g_ptr_array_index (priv->streams, i));
    }

  tp_svc_media_session_handler_return_from_ready (context);
}

static void
gabble_media_channel_error (TpSvcMediaSessionHandler *iface,
                            guint errno,
                            const gchar *message,
                            DBusGMethodInvocation *context)
{
  GabbleMediaChannel *self = GABBLE_MEDIA_CHANNEL (iface);
  GabbleMediaChannelPrivate *priv;
  GPtrArray *tmp;
  guint i;
  JingleState state;

  g_assert (GABBLE_IS_MEDIA_CHANNEL (self));

  priv = self->priv;

  if (priv->session == NULL)
    {
      /* This could also be because someone called Error() before the
       * SessionHandler was announced. But the fact that the SessionHandler is
       * actually also the Channel, and thus this method is available before
       * NewSessionHandler is emitted, is an implementation detail. So the
       * error message describes the only legitimate situation in which this
       * could arise.
       */
      GError e = { TP_ERRORS, TP_ERROR_NOT_AVAILABLE, "call has already ended" };

      DEBUG ("no session, returning an error.");
      dbus_g_method_return_error (context, &e);
      return;
    }

  DEBUG ("Media.SessionHandler::Error called, error %u (%s) -- "
      "emitting error on each stream", errno, message);

  g_object_get (priv->session, "state", &state, NULL);

  if (state == JINGLE_STATE_ENDED)
    {
      tp_svc_media_session_handler_return_from_error (context);
      return;
    }
  else if (state == JINGLE_STATE_PENDING_CREATED)
    {
      /* shortcut to prevent sending remove actions if we haven't sent an
       * initiate yet */
      g_object_set (self, "state", JINGLE_STATE_ENDED, NULL);
      tp_svc_media_session_handler_return_from_error (context);
      return;
    }

  /* Calling gabble_media_stream_error () on all the streams will ultimately
   * cause them all to emit 'closed'. In response to 'closed', stream_close_cb
   * unrefs them, and removes them from priv->streams. So, we copy the stream
   * list to avoid it being modified from underneath us.
   */
  tmp = copy_stream_list (self);

  for (i = 0; i < tmp->len; i++)
    {
      GabbleMediaStream *stream = g_ptr_array_index (tmp, i);

      gabble_media_stream_error (stream, errno, message, NULL);
    }

  g_ptr_array_unref (tmp);

  tp_svc_media_session_handler_return_from_error (context);
}

#define TONE_MS 200
#define GAP_MS 100
#define PAUSE_MS 3000
/* arbitrary limit on the length of a tone started with StartTone */
#define MAX_TONE_SECONDS 10

static void
gabble_media_channel_start_tone (TpSvcChannelInterfaceDTMF *iface,
                                 guint stream_id G_GNUC_UNUSED,
                                 guchar event,
                                 DBusGMethodInvocation *context)
{
  GabbleMediaChannel *self = GABBLE_MEDIA_CHANNEL (iface);
  gchar tones[2] = { '\0', '\0' };
  GError *error = NULL;

  if (!self->priv->have_some_audio)
    {
      GError e = { TP_ERROR, TP_ERROR_NOT_AVAILABLE,
          "There are no audio streams" };

      dbus_g_method_return_error (context, &e);
      return;
    }

  tones[0] = tp_dtmf_event_to_char (event);

  if (tp_dtmf_player_play (self->priv->dtmf_player,
      tones, MAX_TONE_SECONDS * 1000, GAP_MS, PAUSE_MS, &error))
    {
      tp_clear_pointer (&self->priv->deferred_tones, g_free);
      tp_svc_channel_interface_dtmf_emit_sending_tones (self, tones);
      tp_svc_channel_interface_dtmf_return_from_start_tone (context);
    }
  else
    {
      dbus_g_method_return_error (context, error);
      g_clear_error (&error);
    }
}

static void
gabble_media_channel_stop_tone (TpSvcChannelInterfaceDTMF *iface,
                                guint stream_id,
                                DBusGMethodInvocation *context)
{
  GabbleMediaChannel *self = GABBLE_MEDIA_CHANNEL (iface);

  tp_dtmf_player_cancel (self->priv->dtmf_player);
  tp_svc_channel_interface_dtmf_return_from_stop_tone (context);
}

static void
gabble_media_channel_multiple_tones (
    TpSvcChannelInterfaceDTMF *iface,
    const gchar *dialstring,
    DBusGMethodInvocation *context)
{
  GabbleMediaChannel *self = GABBLE_MEDIA_CHANNEL (iface);
  GError *error = NULL;

  if (!self->priv->have_some_audio)
    {
      GError e = { TP_ERROR, TP_ERROR_NOT_AVAILABLE,
          "There are no audio streams" };

      dbus_g_method_return_error (context, &e);
      return;
    }

  if (tp_dtmf_player_play (self->priv->dtmf_player,
      dialstring, TONE_MS, GAP_MS, PAUSE_MS, &error))
    {
      tp_clear_pointer (&self->priv->deferred_tones, g_free);
      tp_svc_channel_interface_dtmf_emit_sending_tones (self, dialstring);
      tp_svc_channel_interface_dtmf_return_from_start_tone (context);
    }
  else
    {
      dbus_g_method_return_error (context, error);
      g_clear_error (&error);
    }
}

static void
channel_iface_init (gpointer g_iface, gpointer iface_data)
{
  TpSvcChannelClass *klass = (TpSvcChannelClass *) g_iface;

#define IMPLEMENT(x, suffix) tp_svc_channel_implement_##x (\
    klass, gabble_media_channel_##x##suffix)
  IMPLEMENT(close,_async);
  IMPLEMENT(get_channel_type,);
  IMPLEMENT(get_handle,);
  IMPLEMENT(get_interfaces,);
#undef IMPLEMENT
}

static void
dtmf_iface_init (gpointer g_iface, gpointer iface_data)
{
  TpSvcChannelInterfaceDTMFClass *klass = g_iface;

#define IMPLEMENT(x) tp_svc_channel_interface_dtmf_implement_##x (\
    klass, gabble_media_channel_##x)
  IMPLEMENT(start_tone);
  IMPLEMENT(stop_tone);
  IMPLEMENT(multiple_tones);
#undef IMPLEMENT
}

static void
streamed_media_iface_init (gpointer g_iface, gpointer iface_data)
{
  TpSvcChannelTypeStreamedMediaClass *klass =
    (TpSvcChannelTypeStreamedMediaClass *) g_iface;

#define IMPLEMENT(x) tp_svc_channel_type_streamed_media_implement_##x (\
    klass, gabble_media_channel_##x)
  IMPLEMENT(list_streams);
  IMPLEMENT(remove_streams);
  IMPLEMENT(request_stream_direction);
  IMPLEMENT(request_streams);
#undef IMPLEMENT
}

static void
media_signalling_iface_init (gpointer g_iface, gpointer iface_data)
{
  TpSvcChannelInterfaceMediaSignallingClass *klass =
    (TpSvcChannelInterfaceMediaSignallingClass *) g_iface;

#define IMPLEMENT(x) tp_svc_channel_interface_media_signalling_implement_##x (\
    klass, gabble_media_channel_##x)
  IMPLEMENT(get_session_handlers);
#undef IMPLEMENT
}

static void
session_handler_iface_init (gpointer g_iface, gpointer iface_data)
{
  TpSvcMediaSessionHandlerClass *klass =
    (TpSvcMediaSessionHandlerClass *) g_iface;

#define IMPLEMENT(x) tp_svc_media_session_handler_implement_##x (\
    klass, gabble_media_channel_##x)
  IMPLEMENT(error);
  IMPLEMENT(ready);
#undef IMPLEMENT
}
