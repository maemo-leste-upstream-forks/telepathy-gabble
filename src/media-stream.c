/*
 * gabble-media-stream.c - Source for GabbleMediaStream
 * Copyright © 2006-2009 Collabora Ltd.
 * Copyright © 2006-2009 Nokia Corporation
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
#include "media-stream.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <dbus/dbus-glib.h>

#include <telepathy-glib/telepathy-glib.h>
#include <telepathy-glib/telepathy-glib-dbus.h>

#define DEBUG_FLAG GABBLE_DEBUG_MEDIA

#include "connection.h"
#include "debug.h"
#include "gabble-signals-marshal.h"
#include "jingle-content.h"
#include "jingle-session.h"
#include "jingle-media-rtp.h"
#include "media-channel.h"
#include "namespaces.h"
#include "util.h"

static void stream_handler_iface_init (gpointer, gpointer);

G_DEFINE_TYPE_WITH_CODE(GabbleMediaStream,
    gabble_media_stream,
    G_TYPE_OBJECT,
    G_IMPLEMENT_INTERFACE (TP_TYPE_SVC_MEDIA_STREAM_HANDLER,
      stream_handler_iface_init);
    G_IMPLEMENT_INTERFACE (TP_TYPE_SVC_DBUS_PROPERTIES,
      tp_dbus_properties_mixin_iface_init);
    )

/* signal enum */
enum
{
    ERROR,
    UNHOLD_FAILED,

    LAST_SIGNAL
};

static guint signals[LAST_SIGNAL] = {0};

/* properties */
enum
{
  PROP_DBUS_DAEMON = 1,
  PROP_OBJECT_PATH,
  PROP_NAME,
  PROP_ID,
  PROP_MEDIA_TYPE,
  PROP_CONNECTION_STATE,
  PROP_READY,
  PROP_PLAYING,
  PROP_COMBINED_DIRECTION,
  PROP_LOCAL_HOLD,
  PROP_CONTENT,
  PROP_STUN_SERVERS,
  PROP_RELAY_INFO,
  PROP_NAT_TRAVERSAL,
  PROP_CREATED_LOCALLY,
  LAST_PROPERTY
};

/* private structure */

struct _GabbleMediaStreamPrivate
{
  GabbleJingleContent *content;

  GabbleMediaSessionMode mode;
  TpDBusDaemon *dbus_daemon;
  gchar *object_path;
  guint id;
  guint media_type;

  gboolean local_codecs_set;

  /* Whether we're waiting for a codec intersection from the streaming
   * implementation. If FALSE, SupportedCodecs is a no-op.
   */
  gboolean awaiting_intersection;

  GValue local_rtp_hdrexts;
  GValue local_feedback_messages;

  GValue remote_codecs;
  GValue remote_rtp_hdrexts;
  GValue remote_feedback_messages;
  GValue remote_candidates;

  guint remote_candidate_count;

  /* source ID for initial codecs/candidates getter */
  gulong initial_getter_id;

  gchar *nat_traversal;
  /* GPtrArray(GValueArray(STRING, UINT)) */
  GPtrArray *stun_servers;
  /* GPtrArray(GHashTable(string => GValue)) */
  GPtrArray *relay_info;

  gboolean on_hold;

  /* These are really booleans, but gboolean is signed. Thanks, GLib */
  unsigned closed:1;
  unsigned dispose_has_run:1;
  unsigned local_hold:1;
  unsigned ready:1;
  unsigned sending:1;
  unsigned created_locally:1;
};

static void push_remote_media_description (GabbleMediaStream *stream);
static void push_remote_candidates (GabbleMediaStream *stream);
static void push_playing (GabbleMediaStream *stream);
static void push_sending (GabbleMediaStream *stream);

static void new_remote_candidates_cb (GabbleJingleContent *content,
    GList *clist, GabbleMediaStream *stream);
static void new_remote_media_description_cb (GabbleJingleContent *content,
    JingleMediaDescription *md, GabbleMediaStream *stream);
static void content_state_changed_cb (GabbleJingleContent *c,
     GParamSpec *pspec, GabbleMediaStream *stream);
static void content_senders_changed_cb (GabbleJingleContent *c,
     GParamSpec *pspec, GabbleMediaStream *stream);
static void remote_state_changed_cb (GabbleJingleSession *session,
    GabbleMediaStream *stream);
static void content_removed_cb (GabbleJingleContent *content,
      GabbleMediaStream *stream);
static void update_direction (GabbleMediaStream *stream, GabbleJingleContent *c);
static void update_sending (GabbleMediaStream *stream, gboolean start_sending);

GabbleMediaStream *
gabble_media_stream_new (
    TpDBusDaemon *dbus_daemon,
    const gchar *object_path,
    GabbleJingleContent *content,
    const gchar *name,
    guint id,
    const gchar *nat_traversal,
    const GPtrArray *relay_info,
    gboolean local_hold)
{
  GPtrArray *empty = NULL;
  GabbleMediaStream *result;

  g_return_val_if_fail (GABBLE_IS_JINGLE_MEDIA_RTP (content), NULL);

  if (relay_info == NULL)
    {
      empty = g_ptr_array_sized_new (0);
      relay_info = empty;
    }

  result = g_object_new (GABBLE_TYPE_MEDIA_STREAM,
      "dbus-daemon", dbus_daemon,
      "object-path", object_path,
      "content", content,
      "name", name,
      "id", id,
      "nat-traversal", nat_traversal,
      "relay-info", relay_info,
      "local-hold", local_hold,
      NULL);

  if (empty != NULL)
    g_ptr_array_unref (empty);

  return result;
}

TpMediaStreamType
gabble_media_stream_get_media_type (GabbleMediaStream *self)
{
  return self->priv->media_type;
}

static void
gabble_media_stream_init (GabbleMediaStream *self)
{
  GabbleMediaStreamPrivate *priv = G_TYPE_INSTANCE_GET_PRIVATE (self,
      GABBLE_TYPE_MEDIA_STREAM, GabbleMediaStreamPrivate);
  GType candidate_list_type =
      TP_ARRAY_TYPE_MEDIA_STREAM_HANDLER_CANDIDATE_LIST;
  GType codec_list_type =
      TP_ARRAY_TYPE_MEDIA_STREAM_HANDLER_CODEC_LIST;
  GType rtp_hdrext_list_type = TP_ARRAY_TYPE_RTP_HEADER_EXTENSIONS_LIST;
  GType fb_msg_map_type  = TP_HASH_TYPE_RTCP_FEEDBACK_MESSAGE_MAP;

  self->priv = priv;

  g_value_init (&priv->local_rtp_hdrexts, rtp_hdrext_list_type);
  g_value_init (&priv->local_feedback_messages, fb_msg_map_type);

  g_value_init (&priv->remote_codecs, codec_list_type);
  g_value_take_boxed (&priv->remote_codecs,
      dbus_g_type_specialized_construct (codec_list_type));

  g_value_init (&priv->remote_rtp_hdrexts, rtp_hdrext_list_type);
  g_value_take_boxed (&priv->remote_rtp_hdrexts,
      dbus_g_type_specialized_construct (rtp_hdrext_list_type));

  g_value_init (&priv->remote_feedback_messages, fb_msg_map_type);
  g_value_take_boxed (&priv->remote_feedback_messages,
      dbus_g_type_specialized_construct (fb_msg_map_type));

  g_value_init (&priv->remote_candidates, candidate_list_type);
  g_value_take_boxed (&priv->remote_candidates,
      dbus_g_type_specialized_construct (candidate_list_type));

  priv->stun_servers = g_ptr_array_sized_new (1);
}

static gboolean
_get_initial_codecs_and_candidates (gpointer user_data)
{
  GabbleMediaStream *stream = GABBLE_MEDIA_STREAM (user_data);
  GabbleMediaStreamPrivate *priv = stream->priv;
  JingleMediaDescription *md;

  priv->initial_getter_id = 0;

  /* we can immediately get the codecs if we're responder */
  md = gabble_jingle_media_rtp_get_remote_media_description (
      GABBLE_JINGLE_MEDIA_RTP (priv->content));
  if (md != NULL)
    new_remote_media_description_cb (priv->content, md, stream);

  /* if any candidates arrived before idle loop had the chance to excute
   * us (e.g. specified in session-initiate/content-add), we don't want to
   * miss them */
  new_remote_candidates_cb (priv->content,
      gabble_jingle_content_get_remote_candidates (priv->content), stream);

  return FALSE;
}

static GObject *
gabble_media_stream_constructor (GType type, guint n_props,
                                 GObjectConstructParam *props)
{
  GObject *obj;
  GabbleMediaStream *stream;
  GabbleMediaStreamPrivate *priv;
  GabbleJingleFactory *jf;
  gchar *stun_server;
  guint stun_port;

  /* call base class constructor */
  obj = G_OBJECT_CLASS (gabble_media_stream_parent_class)->
           constructor (type, n_props, props);
  stream = GABBLE_MEDIA_STREAM (obj);
  priv = stream->priv;

  g_assert (priv->content != NULL);

  /* STUN servers are needed as soon as the stream appears, so there's little
   * point in waiting for them - either they've already been resolved, or
   * we're too late to use them for this stream */
  jf = gabble_jingle_session_get_factory (priv->content->session);

  /* maybe one day we'll support multiple STUN servers */
  if (gabble_jingle_info_get_stun_server (
        gabble_jingle_factory_get_jingle_info (jf),
        &stun_server, &stun_port))
    {
      GValueArray *va = g_value_array_new (2);

      g_value_array_append (va, NULL);
      g_value_array_append (va, NULL);
      g_value_init (va->values + 0, G_TYPE_STRING);
      g_value_init (va->values + 1, G_TYPE_UINT);
      g_value_take_string (va->values + 0, stun_server);
      g_value_set_uint (va->values + 1, stun_port);
      g_ptr_array_add (priv->stun_servers, va);
    }

  /* go for the bus */
  g_assert (priv->dbus_daemon != NULL);
  tp_dbus_daemon_register_object (priv->dbus_daemon, priv->object_path, obj);

  update_direction (stream, priv->content);

  /* MediaStream is created as soon as GabbleJingleContent is
   * created, but we want to let it parse the initiation (if
   * initiated by remote end) before we pick up initial
   * codecs and candidates.
   * FIXME: add API for ordering IQs rather than using g_idle_add.
   */
  priv->initial_getter_id =
      g_idle_add (_get_initial_codecs_and_candidates, stream);

  if (priv->created_locally)
    {
      g_object_set (stream, "combined-direction",
          MAKE_COMBINED_DIRECTION (TP_MEDIA_STREAM_DIRECTION_BIDIRECTIONAL,
            0), NULL);
    }
  else
    {
      priv->awaiting_intersection = TRUE;
    }

  return obj;
}

static void
gabble_media_stream_get_property (GObject    *object,
                                  guint       property_id,
                                  GValue     *value,
                                  GParamSpec *pspec)
{
  GabbleMediaStream *stream = GABBLE_MEDIA_STREAM (object);
  GabbleMediaStreamPrivate *priv = stream->priv;

  switch (property_id) {
    case PROP_DBUS_DAEMON:
      g_value_set_object (value, priv->dbus_daemon);
      break;
    case PROP_OBJECT_PATH:
      g_value_set_string (value, priv->object_path);
      break;
    case PROP_NAME:
      g_value_set_string (value, stream->name);
      break;
    case PROP_ID:
      g_value_set_uint (value, priv->id);
      break;
    case PROP_MEDIA_TYPE:
      g_value_set_uint (value, priv->media_type);
      break;
    case PROP_CONNECTION_STATE:
      g_value_set_uint (value, stream->connection_state);
      break;
    case PROP_READY:
      g_value_set_boolean (value, priv->ready);
      break;
    case PROP_PLAYING:
      g_value_set_boolean (value, stream->playing);
      break;
    case PROP_COMBINED_DIRECTION:
      g_value_set_uint (value, stream->combined_direction);
      break;
    case PROP_LOCAL_HOLD:
      g_value_set_boolean (value, priv->local_hold);
      break;
    case PROP_CONTENT:
      g_value_set_object (value, priv->content);
      break;
    case PROP_STUN_SERVERS:
      g_value_set_boxed (value, priv->stun_servers);
      break;
    case PROP_NAT_TRAVERSAL:
      g_value_set_string (value, priv->nat_traversal);
      break;
    case PROP_CREATED_LOCALLY:
      g_value_set_boolean (value, priv->created_locally);
      break;
    case PROP_RELAY_INFO:
      g_value_set_boxed (value, priv->relay_info);
      break;
    default:
      G_OBJECT_WARN_INVALID_PROPERTY_ID (object, property_id, pspec);
      break;
  }
}

static void
gabble_media_stream_set_property (GObject      *object,
                                  guint         property_id,
                                  const GValue *value,
                                  GParamSpec   *pspec)
{
  GabbleMediaStream *stream = GABBLE_MEDIA_STREAM (object);
  GabbleMediaStreamPrivate *priv = stream->priv;

  switch (property_id) {
    case PROP_OBJECT_PATH:
      g_free (priv->object_path);
      priv->object_path = g_value_dup_string (value);
      break;
    case PROP_DBUS_DAEMON:
      g_assert (priv->dbus_daemon == NULL);
      priv->dbus_daemon = g_value_dup_object (value);
      break;
    case PROP_NAME:
      g_free (stream->name);
      stream->name = g_value_dup_string (value);
      break;
    case PROP_ID:
      priv->id = g_value_get_uint (value);
      break;
    case PROP_CONNECTION_STATE:
      DEBUG ("stream %s connection state %d",
          stream->name, stream->connection_state);
      stream->connection_state = g_value_get_uint (value);
      break;
    case PROP_READY:
      priv->ready = g_value_get_boolean (value);
      break;
    case PROP_PLAYING:
        {
          gboolean old = stream->playing;
          stream->playing = g_value_get_boolean (value);
          if (stream->playing != old)
            push_playing (stream);
        }
      break;
    case PROP_COMBINED_DIRECTION:
      DEBUG ("changing combined direction from %u to %u",
          stream->combined_direction, g_value_get_uint (value));
      stream->combined_direction = g_value_get_uint (value);
      break;
    case PROP_CONTENT:
      g_assert (priv->content == NULL);

      priv->content = g_value_dup_object (value);

        {
          guint jtype;
          gboolean locally_created;

          g_object_get (priv->content,
              "media-type", &jtype,
              "locally-created", &locally_created,
              NULL);

          if (jtype == JINGLE_MEDIA_TYPE_VIDEO)
            priv->media_type = TP_MEDIA_STREAM_TYPE_VIDEO;
          else
            priv->media_type = TP_MEDIA_STREAM_TYPE_AUDIO;

          priv->created_locally = locally_created;
        }

      DEBUG ("%p: connecting to content %p signals", stream, priv->content);

      gabble_signal_connect_weak (priv->content, "new-candidates",
          (GCallback) new_remote_candidates_cb, object);

      /* we need this also, if we're the initiator of the stream
       * (so remote codecs arrive later) */
      gabble_signal_connect_weak (priv->content, "remote-media-description",
          (GCallback) new_remote_media_description_cb, object);

      gabble_signal_connect_weak (priv->content, "notify::state",
          (GCallback) content_state_changed_cb, object);

      gabble_signal_connect_weak (priv->content, "notify::senders",
          (GCallback) content_senders_changed_cb, object);

      gabble_signal_connect_weak (priv->content->session,
          "remote-state-changed", (GCallback) remote_state_changed_cb, object);

      gabble_signal_connect_weak (priv->content, "removed",
          (GCallback) content_removed_cb, object);
      break;
    case PROP_NAT_TRAVERSAL:
      g_assert (priv->nat_traversal == NULL);
      priv->nat_traversal = g_value_dup_string (value);
      break;
    case PROP_RELAY_INFO:
      g_assert (priv->relay_info == NULL);
      priv->relay_info = g_value_dup_boxed (value);
      break;
    case PROP_LOCAL_HOLD:
      priv->local_hold = g_value_get_boolean (value);
      break;
    default:
      G_OBJECT_WARN_INVALID_PROPERTY_ID (object, property_id, pspec);
      break;
  }
}

static void gabble_media_stream_dispose (GObject *object);
static void gabble_media_stream_finalize (GObject *object);

static void
gabble_media_stream_class_init (GabbleMediaStreamClass *gabble_media_stream_class)
{
  GObjectClass *object_class = G_OBJECT_CLASS (gabble_media_stream_class);
  GParamSpec *param_spec;
  static TpDBusPropertiesMixinPropImpl stream_handler_props[] = {
      { "RelayInfo", "relay-info", NULL },
      { "STUNServers", "stun-servers", NULL },
      { "NATTraversal", "nat-traversal", NULL },
      { "CreatedLocally", "created-locally", NULL },
      { NULL }
  };
  static TpDBusPropertiesMixinIfaceImpl prop_interfaces[] = {
      { TP_IFACE_MEDIA_STREAM_HANDLER,
        tp_dbus_properties_mixin_getter_gobject_properties,
        NULL,
        stream_handler_props,
      },
      { NULL }
  };

  g_type_class_add_private (gabble_media_stream_class,
      sizeof (GabbleMediaStreamPrivate));

  object_class->constructor = gabble_media_stream_constructor;

  object_class->get_property = gabble_media_stream_get_property;
  object_class->set_property = gabble_media_stream_set_property;

  object_class->dispose = gabble_media_stream_dispose;
  object_class->finalize = gabble_media_stream_finalize;

  param_spec = g_param_spec_object ("dbus-daemon", "TpDBusDaemon",
      "Bus on which to export this object",
      TP_TYPE_DBUS_DAEMON,
      G_PARAM_CONSTRUCT_ONLY | G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS);
  g_object_class_install_property (object_class, PROP_DBUS_DAEMON, param_spec);

  param_spec = g_param_spec_string ("object-path", "D-Bus object path",
                                    "The D-Bus object path used for this "
                                    "object on the bus.",
                                    NULL,
                                    G_PARAM_CONSTRUCT_ONLY |
                                    G_PARAM_READWRITE |
                                    G_PARAM_STATIC_NAME |
                                    G_PARAM_STATIC_BLURB);
  g_object_class_install_property (object_class, PROP_OBJECT_PATH, param_spec);

  param_spec = g_param_spec_string ("name", "Stream name",
      "An opaque name for the stream used in the signalling.", NULL,
      G_PARAM_CONSTRUCT_ONLY | G_PARAM_READWRITE | G_PARAM_STATIC_NAME |
      G_PARAM_STATIC_BLURB);
  g_object_class_install_property (object_class, PROP_NAME, param_spec);

  param_spec = g_param_spec_uint ("id", "Stream ID",
                                  "A stream number for the stream used in the "
                                  "D-Bus API.",
                                  0, G_MAXUINT, 0,
                                  G_PARAM_CONSTRUCT_ONLY |
                                  G_PARAM_READWRITE |
                                  G_PARAM_STATIC_NAME |
                                  G_PARAM_STATIC_BLURB);
  g_object_class_install_property (object_class, PROP_ID, param_spec);

  param_spec = g_param_spec_uint ("media-type", "Stream media type",
      "A constant indicating which media type the stream carries.",
      TP_MEDIA_STREAM_TYPE_AUDIO, TP_MEDIA_STREAM_TYPE_VIDEO,
      TP_MEDIA_STREAM_TYPE_AUDIO,
      G_PARAM_READABLE | G_PARAM_STATIC_STRINGS);
  g_object_class_install_property (object_class, PROP_MEDIA_TYPE, param_spec);

  param_spec = g_param_spec_uint ("connection-state", "Stream connection state",
                                  "An integer indicating the state of the"
                                  "stream's connection.",
                                  TP_MEDIA_STREAM_STATE_DISCONNECTED,
                                  TP_MEDIA_STREAM_STATE_CONNECTED,
                                  TP_MEDIA_STREAM_STATE_DISCONNECTED,
                                  G_PARAM_CONSTRUCT |
                                  G_PARAM_READWRITE |
                                  G_PARAM_STATIC_NAME |
                                  G_PARAM_STATIC_BLURB);
  g_object_class_install_property (object_class, PROP_CONNECTION_STATE,
      param_spec);

  param_spec = g_param_spec_boolean ("ready", "Ready?",
                                     "A boolean signifying whether the user "
                                     "is ready to handle signals from this "
                                     "object.",
                                     FALSE,
                                     G_PARAM_CONSTRUCT |
                                     G_PARAM_READWRITE |
                                     G_PARAM_STATIC_NAME |
                                     G_PARAM_STATIC_BLURB);
  g_object_class_install_property (object_class, PROP_READY, param_spec);

  param_spec = g_param_spec_boolean ("playing", "Set playing",
                                     "A boolean signifying whether the stream "
                                     "has been set playing yet.",
                                     FALSE,
                                     G_PARAM_CONSTRUCT |
                                     G_PARAM_READWRITE |
                                     G_PARAM_STATIC_NAME |
                                     G_PARAM_STATIC_BLURB);
  g_object_class_install_property (object_class, PROP_PLAYING, param_spec);

  param_spec = g_param_spec_uint ("combined-direction",
      "Combined direction",
      "An integer indicating the directions the stream currently sends in, "
      "and the peers who have been asked to send.",
      TP_MEDIA_STREAM_DIRECTION_NONE,
      MAKE_COMBINED_DIRECTION (TP_MEDIA_STREAM_DIRECTION_BIDIRECTIONAL,
        TP_MEDIA_STREAM_PENDING_LOCAL_SEND |
        TP_MEDIA_STREAM_PENDING_REMOTE_SEND),
      TP_MEDIA_STREAM_DIRECTION_NONE,
      G_PARAM_CONSTRUCT | G_PARAM_READWRITE | G_PARAM_STATIC_NAME |
      G_PARAM_STATIC_BLURB);
  g_object_class_install_property (object_class, PROP_COMBINED_DIRECTION,
      param_spec);

  param_spec = g_param_spec_boolean ("local-hold", "Local hold?",
      "True if resources used for this stream have been freed.", FALSE,
      G_PARAM_READWRITE | G_PARAM_CONSTRUCT_ONLY |
      G_PARAM_STATIC_NAME | G_PARAM_STATIC_BLURB | G_PARAM_STATIC_NICK);
  g_object_class_install_property (object_class, PROP_LOCAL_HOLD, param_spec);

  param_spec = g_param_spec_object ("content", "GabbleJingleContent object",
                                    "Jingle content signalling this media stream.",
                                    GABBLE_TYPE_JINGLE_CONTENT,
                                    G_PARAM_CONSTRUCT_ONLY |
                                    G_PARAM_READWRITE |
                                    G_PARAM_STATIC_NICK |
                                    G_PARAM_STATIC_BLURB);
  g_object_class_install_property (object_class, PROP_CONTENT, param_spec);

  param_spec = g_param_spec_boxed ("stun-servers", "STUN servers",
      "Array of (STRING: address literal, UINT: port) pairs",
      /* FIXME: use correct macro when available */
      tp_type_dbus_array_su (),
      G_PARAM_READABLE | G_PARAM_STATIC_STRINGS);
  g_object_class_install_property (object_class, PROP_STUN_SERVERS, param_spec);

  param_spec = g_param_spec_boxed ("relay-info", "Relay info",
      "Array of mappings containing relay server information",
      TP_ARRAY_TYPE_STRING_VARIANT_MAP_LIST,
      G_PARAM_CONSTRUCT_ONLY | G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS);
  g_object_class_install_property (object_class, PROP_RELAY_INFO, param_spec);

  param_spec = g_param_spec_string ("nat-traversal", "NAT traversal",
      "NAT traversal mechanism for this stream", NULL,
      G_PARAM_CONSTRUCT_ONLY | G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS);
  g_object_class_install_property (object_class, PROP_NAT_TRAVERSAL,
      param_spec);

  param_spec = g_param_spec_boolean ("created-locally", "Created locally?",
      "True if this stream was created by the local user", FALSE,
      G_PARAM_READABLE | G_PARAM_STATIC_STRINGS);
  g_object_class_install_property (object_class, PROP_CREATED_LOCALLY,
      param_spec);

  /* signals not exported by D-Bus interface */

  signals[ERROR] =
    g_signal_new ("error",
                  G_OBJECT_CLASS_TYPE (gabble_media_stream_class),
                  G_SIGNAL_RUN_LAST | G_SIGNAL_DETAILED,
                  0,
                  NULL, NULL,
                  gabble_marshal_VOID__UINT_STRING,
                  G_TYPE_NONE, 2, G_TYPE_UINT, G_TYPE_STRING);

  signals[UNHOLD_FAILED] = g_signal_new ("unhold-failed",
      G_OBJECT_CLASS_TYPE (gabble_media_stream_class),
      G_SIGNAL_RUN_LAST | G_SIGNAL_DETAILED, 0, NULL, NULL,
      g_cclosure_marshal_VOID__VOID, G_TYPE_NONE, 0);

  gabble_media_stream_class->props_class.interfaces = prop_interfaces;
  tp_dbus_properties_mixin_class_init (object_class,
      G_STRUCT_OFFSET (GabbleMediaStreamClass, props_class));
}

void
gabble_media_stream_dispose (GObject *object)
{
  GabbleMediaStream *self = GABBLE_MEDIA_STREAM (object);
  GabbleMediaStreamPrivate *priv = self->priv;

  DEBUG ("called");

  if (priv->dispose_has_run)
    return;

  if (priv->initial_getter_id != 0)
    {
      g_source_remove (priv->initial_getter_id);
      priv->initial_getter_id = 0;
    }

  gabble_media_stream_close (self);

  priv->dispose_has_run = TRUE;

  tp_clear_object (&priv->content);
  tp_clear_pointer (&self->name, g_free);
  g_clear_object (&priv->dbus_daemon);

  if (G_OBJECT_CLASS (gabble_media_stream_parent_class)->dispose)
    G_OBJECT_CLASS (gabble_media_stream_parent_class)->dispose (object);
}

void
gabble_media_stream_finalize (GObject *object)
{
  GabbleMediaStream *self = GABBLE_MEDIA_STREAM (object);
  GabbleMediaStreamPrivate *priv = self->priv;

  g_free (priv->object_path);
  g_free (priv->nat_traversal);

  /* FIXME: use correct macro when available */
  if (priv->stun_servers != NULL)
    g_boxed_free (tp_type_dbus_array_su (), priv->stun_servers);

  if (priv->relay_info != NULL)
    g_boxed_free (TP_ARRAY_TYPE_STRING_VARIANT_MAP_LIST, priv->relay_info);

  g_value_unset (&priv->local_rtp_hdrexts);
  g_value_unset (&priv->local_feedback_messages);

  g_value_unset (&priv->remote_codecs);
  g_value_unset (&priv->remote_rtp_hdrexts);
  g_value_unset (&priv->remote_feedback_messages);
  g_value_unset (&priv->remote_candidates);

  G_OBJECT_CLASS (gabble_media_stream_parent_class)->finalize (object);
}

/**
 * gabble_media_stream_codec_choice
 *
 * Implements D-Bus method CodecChoice
 * on interface org.freedesktop.Telepathy.Media.StreamHandler
 */
static void
gabble_media_stream_codec_choice (TpSvcMediaStreamHandler *iface,
                                  guint codec_id,
                                  DBusGMethodInvocation *context)
{
  tp_svc_media_stream_handler_return_from_codec_choice (context);
}


gboolean
gabble_media_stream_error (GabbleMediaStream *self,
                           guint errnum,
                           const gchar *message,
                           GError **error)
{
  g_assert (GABBLE_IS_MEDIA_STREAM (self));

  DEBUG ( "Media.StreamHandler::Error called, error %u (%s) -- emitting signal",
      errnum, message);
  g_signal_emit (self, signals[ERROR], 0, errnum, message);

  return TRUE;
}


/**
 * gabble_media_stream_error
 *
 * Implements D-Bus method Error
 * on interface org.freedesktop.Telepathy.Media.StreamHandler
 */
static void
gabble_media_stream_error_async (TpSvcMediaStreamHandler *iface,
                                 guint errnum,
                                 const gchar *message,
                                 DBusGMethodInvocation *context)
{
  GabbleMediaStream *self = GABBLE_MEDIA_STREAM (iface);
  GError *error = NULL;

  if (gabble_media_stream_error (self, errnum, message, &error))
    {
      tp_svc_media_stream_handler_return_from_error (context);
    }
  else
    {
      dbus_g_method_return_error (context, error);
      g_error_free (error);
    }
}


/**
 * gabble_media_stream_hold:
 *
 * Tell streaming clients that the stream is going on hold, so they should
 * stop streaming and free up any resources they are currently holding
 * (e.g. close hardware devices); or that the stream is coming off hold,
 * so they should reacquire those resources.
 */
void
gabble_media_stream_hold (GabbleMediaStream *self,
                          gboolean hold)
{
  tp_svc_media_stream_handler_emit_set_stream_held (self, hold);
}


/**
 * gabble_media_stream_hold_state:
 *
 * Called by streaming clients when the stream's hold state has been changed
 * successfully in response to SetStreamHeld.
 */
static void
gabble_media_stream_hold_state (TpSvcMediaStreamHandler *iface,
                                gboolean hold_state,
                                DBusGMethodInvocation *context)
{
  GabbleMediaStream *self = GABBLE_MEDIA_STREAM (iface);
  GabbleMediaStreamPrivate *priv = self->priv;

  DEBUG ("%p: %s", self, hold_state ? "held" : "unheld");
  priv->local_hold = hold_state;

  g_object_notify ((GObject *) self, "local-hold");

  tp_svc_media_stream_handler_return_from_hold_state (context);
}


/**
 * gabble_media_stream_unhold_failure:
 *
 * Called by streaming clients when an attempt to reacquire the necessary
 * hardware or software resources to unhold the stream, in response to
 * SetStreamHeld, has failed.
 */
static void
gabble_media_stream_unhold_failure (TpSvcMediaStreamHandler *iface,
                                    DBusGMethodInvocation *context)
{
  GabbleMediaStream *self = GABBLE_MEDIA_STREAM (iface);
  GabbleMediaStreamPrivate *priv = self->priv;

  DEBUG ("%p", self);

  priv->local_hold = TRUE;

  g_signal_emit (self, signals[UNHOLD_FAILED], 0);
  g_object_notify ((GObject *) self, "local-hold");

  tp_svc_media_stream_handler_return_from_unhold_failure (context);
}


/**
 * gabble_media_stream_native_candidates_prepared
 *
 * Implements D-Bus method NativeCandidatesPrepared
 * on interface org.freedesktop.Telepathy.Media.StreamHandler
 */
static void
gabble_media_stream_native_candidates_prepared (TpSvcMediaStreamHandler *iface,
                                                DBusGMethodInvocation *context)
{
  tp_svc_media_stream_handler_return_from_native_candidates_prepared (context);
}


/**
 * gabble_media_stream_new_active_candidate_pair
 *
 * Implements D-Bus method NewActiveCandidatePair
 * on interface org.freedesktop.Telepathy.Media.StreamHandler
 */
static void
gabble_media_stream_new_active_candidate_pair (TpSvcMediaStreamHandler *iface,
                                               const gchar *native_candidate_id,
                                               const gchar *remote_candidate_id,
                                               DBusGMethodInvocation *context)
{
  DEBUG ("called (%s, %s); this is a no-op on Jingle", native_candidate_id,
      remote_candidate_id);

  tp_svc_media_stream_handler_return_from_new_active_candidate_pair (context);
}


/**
 * gabble_media_stream_new_native_candidate
 *
 * Implements D-Bus method NewNativeCandidate
 * on interface org.freedesktop.Telepathy.Media.StreamHandler
 */
static void
gabble_media_stream_new_native_candidate (TpSvcMediaStreamHandler *iface,
                                          const gchar *candidate_id,
                                          const GPtrArray *transports,
                                          DBusGMethodInvocation *context)
{
  GabbleMediaStream *self = GABBLE_MEDIA_STREAM (iface);
  GabbleMediaStreamPrivate *priv;
  JingleState state;
  GList *li = NULL;
  guint i;

  g_assert (GABBLE_IS_MEDIA_STREAM (self));

  priv = self->priv;

  g_object_get (priv->content->session, "state", &state, NULL);

  /* FIXME: maybe this should be an assertion in case the channel
   * isn't closed early enough right now? */
  if (state > JINGLE_STATE_ACTIVE)
    {
      DEBUG ("state > JINGLE_STATE_ACTIVE, doing nothing");
      tp_svc_media_stream_handler_return_from_new_native_candidate (context);
      return;
    }

  for (i = 0; i < transports->len; i++)
    {
      GValueArray *transport;
      guint component;
      const gchar *addr;
      JingleCandidate *c;

      transport = g_ptr_array_index (transports, i);
      component = g_value_get_uint (g_value_array_get_nth (transport, 0));

      /* Farsight 1 compatibility */
      if (component == 0)
        component = 1;

      /* We understand RTP and RTCP, and silently ignore the rest */
      if ((component != 1) && (component != 2))
        continue;

      addr = g_value_get_string (g_value_array_get_nth (transport, 1));
      if (!strcmp (addr, "127.0.0.1"))
        {
          DEBUG ("ignoring native localhost candidate");
          continue;
        }

      c = jingle_candidate_new (
          /* protocol */
          g_value_get_uint (g_value_array_get_nth (transport, 3)),
          /* candidate type, we're relying on 1:1 candidate type mapping */
          g_value_get_uint (g_value_array_get_nth (transport, 7)),
          /* id */
          candidate_id,
          /* component */
          component,
          /* address */
          g_value_get_string (g_value_array_get_nth (transport, 1)),
          /* port */
          g_value_get_uint (g_value_array_get_nth (transport, 2)),
          /* generation */
          0,
          /* preference */
          (int) (g_value_get_double (g_value_array_get_nth (transport, 6)) * 65536),
          /* username */
          g_value_get_string (g_value_array_get_nth (transport, 8)),
          /* password */
          g_value_get_string (g_value_array_get_nth (transport, 9)),
          /* network */
          0);

      li = g_list_prepend (li, c);
    }

  if (li != NULL)
    gabble_jingle_content_add_candidates (priv->content, li);

  tp_svc_media_stream_handler_return_from_new_native_candidate (context);
}

static void gabble_media_stream_set_local_codecs (TpSvcMediaStreamHandler *,
    const GPtrArray *codecs, DBusGMethodInvocation *);

/**
 * gabble_media_stream_ready
 *
 * Implements D-Bus method Ready
 * on interface org.freedesktop.Telepathy.Media.StreamHandler
 */
static void
gabble_media_stream_ready (TpSvcMediaStreamHandler *iface,
                           const GPtrArray *codecs,
                           DBusGMethodInvocation *context)
{
  GabbleMediaStream *self = GABBLE_MEDIA_STREAM (iface);
  GabbleMediaStreamPrivate *priv;

  g_assert (GABBLE_IS_MEDIA_STREAM (self));

  priv = self->priv;

  DEBUG ("ready called");

  if (priv->ready == FALSE)
    {
      g_object_set (self, "ready", TRUE, NULL);

      push_remote_media_description (self);
      push_remote_candidates (self);
      push_playing (self);
      push_sending (self);

      /* If a new stream is added while the call's on hold, it will have
       * local_hold set at construct time. So once tp-fs has called Ready(), we
       * should let it know this stream's on hold.
       */
      if (priv->local_hold)
        gabble_media_stream_hold (self, priv->local_hold);
    }
  else
    {
      DEBUG ("Ready called twice, running plain SetLocalCodecs instead");
    }

  /* set_local_codecs and ready return the same thing, so we can do... */
  gabble_media_stream_set_local_codecs (iface, codecs, context);
}

static gboolean
pass_local_codecs (GabbleMediaStream *stream,
                   const GPtrArray *codecs,
                   gboolean ready,
                   GError **error)
{
  GabbleMediaStreamPrivate *priv = stream->priv;
  guint i;
  JingleMediaDescription *md;
  const GPtrArray *hdrexts;
  GHashTable *fbs;
  GError *wocky_error = NULL;

  DEBUG ("putting list of %d supported codecs from stream-engine into cache",
      codecs->len);

  md = jingle_media_description_new ();

  fbs = g_value_get_boxed (&priv->local_feedback_messages);

  for (i = 0; i < codecs->len; i++)
    {
      GType codec_struct_type = TP_STRUCT_TYPE_MEDIA_STREAM_HANDLER_CODEC;

      GValue codec = { 0, };
      guint id, clock_rate, channels;
      gchar *name;
      GHashTable *params;
      JingleCodec *c;
      GValueArray *fb_codec;

      g_value_init (&codec, codec_struct_type);
      g_value_set_static_boxed (&codec, g_ptr_array_index (codecs, i));

      dbus_g_type_struct_get (&codec,
          0, &id,
          1, &name,
          3, &clock_rate,
          4, &channels,
          5, &params,
          G_MAXUINT);

      c = jingle_media_rtp_codec_new (id, name,
          clock_rate, channels, params);

      if (fbs != NULL)
        {
          fb_codec = g_hash_table_lookup (fbs, GUINT_TO_POINTER (id));
          if (fb_codec != NULL)
            {
              if (G_VALUE_HOLDS_UINT (
                      g_value_array_get_nth (fb_codec, 0)) &&
                  G_VALUE_TYPE (g_value_array_get_nth (fb_codec, 1)) ==
                  TP_ARRAY_TYPE_RTCP_FEEDBACK_MESSAGE_LIST)
                {
                  GValue *val;
                  const GPtrArray *fb_array;
                  guint j;

                  val = g_value_array_get_nth (fb_codec, 0);
                  c->trr_int = g_value_get_uint (val);

                  val = g_value_array_get_nth (fb_codec, 1);
                  fb_array = g_value_get_boxed (val);

                  for (j = 0; j < fb_array->len; j++)
                    {
                      GValueArray *message = g_ptr_array_index (fb_array, j);
                      const gchar *type;
                      const gchar *subtype;

                      val = g_value_array_get_nth (message, 0);
                      type = g_value_get_string (val);

                      val = g_value_array_get_nth (message, 1);
                      subtype = g_value_get_string (val);

                      c->feedback_msgs = g_list_append (c->feedback_msgs,
                          jingle_feedback_message_new (type, subtype));
                    }
                }
            }
        }
      DEBUG ("adding codec %s (%u %u %u)", c->name, c->id, c->clockrate, c->channels);
      md->codecs = g_list_append (md->codecs, c);
      g_free (name);
      g_hash_table_unref (params);
    }

  if (fbs != NULL)
    g_value_reset (&priv->local_feedback_messages);

  hdrexts = g_value_get_boxed (&priv->local_rtp_hdrexts);

  if (hdrexts != NULL)
    {
      gboolean have_initiator = FALSE;
      gboolean initiated_by_us;

      for (i = 0; i < hdrexts->len; i++)
        {
          GValueArray *hdrext;
          guint id;
          guint direction;
          JingleContentSenders senders;
          gchar *uri;
          gchar *params;

          hdrext = g_ptr_array_index (hdrexts, i);

          g_assert (hdrext);
          g_assert (hdrext->n_values == 4);
          g_assert (G_VALUE_HOLDS_UINT   (g_value_array_get_nth (hdrext, 0)));
          g_assert (G_VALUE_HOLDS_UINT   (g_value_array_get_nth (hdrext, 1)));
          g_assert (G_VALUE_HOLDS_STRING (g_value_array_get_nth (hdrext, 2)));
          g_assert (G_VALUE_HOLDS_STRING (g_value_array_get_nth (hdrext, 3)));

          tp_value_array_unpack (hdrext, 4,
              &id,
              &direction,
              &uri,
              &params);

          if (!have_initiator)
            {
              g_object_get (priv->content->session, "local-initiator",
                  &initiated_by_us, NULL);
              have_initiator = TRUE;
            }

          switch (direction)
            {
            case TP_MEDIA_STREAM_DIRECTION_BIDIRECTIONAL:
              senders = JINGLE_CONTENT_SENDERS_BOTH;
              break;
            case TP_MEDIA_STREAM_DIRECTION_NONE:
              senders = JINGLE_CONTENT_SENDERS_NONE;
              break;
            case TP_MEDIA_STREAM_DIRECTION_SEND:
              senders = initiated_by_us ? JINGLE_CONTENT_SENDERS_INITIATOR :
              JINGLE_CONTENT_SENDERS_RESPONDER;
              break;
            case TP_MEDIA_STREAM_DIRECTION_RECEIVE:
              senders = initiated_by_us ? JINGLE_CONTENT_SENDERS_RESPONDER :
              JINGLE_CONTENT_SENDERS_INITIATOR;
              break;
            default:
              g_assert_not_reached ();
            }

          md->hdrexts = g_list_append (md->hdrexts,
              jingle_rtp_header_extension_new (id, senders, uri));
        }
      /* Can only be used once */
      g_value_reset (&priv->local_rtp_hdrexts);
    }

  jingle_media_description_simplify (md);

  if (jingle_media_rtp_set_local_media_description (
          GABBLE_JINGLE_MEDIA_RTP (priv->content), md, ready, &wocky_error))
    return TRUE;

  g_set_error_literal (error, TP_ERROR, TP_ERROR_INVALID_ARGUMENT,
      wocky_error->message);
  g_clear_error (&wocky_error);
  return FALSE;
}

/**
 * gabble_media_stream_set_local_codecs
 *
 * Implements D-Bus method SetLocalCodecs
 * on interface org.freedesktop.Telepathy.Media.StreamHandler
 */
static void
gabble_media_stream_set_local_codecs (TpSvcMediaStreamHandler *iface,
                                      const GPtrArray *codecs,
                                      DBusGMethodInvocation *context)
{
  GabbleMediaStream *self = GABBLE_MEDIA_STREAM (iface);
  GabbleMediaStreamPrivate *priv = self->priv;
  GError *error = NULL;

  DEBUG ("called");

  if (codecs->len == 0)
    goto done;

  priv->local_codecs_set = TRUE;

  if (gabble_jingle_content_is_created_by_us (self->priv->content))
    {
      if (!pass_local_codecs (self, codecs, self->priv->created_locally,
          &error))
        {
          DEBUG ("failed: %s", error->message);

          dbus_g_method_return_error (context, error);
          g_error_free (error);
          return;
        }
    }
  else
    {
      DEBUG ("ignoring local codecs, waiting for codec intersection");
    }
 done:

  tp_svc_media_stream_handler_return_from_set_local_codecs (context);
}

/**
 * gabble_media_stream_stream_state
 *
 * Implements D-Bus method StreamState
 * on interface org.freedesktop.Telepathy.Media.StreamHandler
 */
static void
gabble_media_stream_stream_state (TpSvcMediaStreamHandler *iface,
                                  guint connection_state,
                                  DBusGMethodInvocation *context)
{
  GabbleMediaStream *self = GABBLE_MEDIA_STREAM (iface);
  GabbleMediaStreamPrivate *priv = self->priv;
  JingleTransportState ts = JINGLE_TRANSPORT_STATE_DISCONNECTED;

  switch (connection_state) {
    case TP_MEDIA_STREAM_STATE_DISCONNECTED:
      ts = JINGLE_TRANSPORT_STATE_DISCONNECTED;
      break;
    case TP_MEDIA_STREAM_STATE_CONNECTING:
      ts = JINGLE_TRANSPORT_STATE_CONNECTING;
      break;
    case TP_MEDIA_STREAM_STATE_CONNECTED:
      ts = JINGLE_TRANSPORT_STATE_CONNECTED;
      break;
    default:
      DEBUG ("ignoring unknown connection state %u", connection_state);
      goto OUT;
  }

  g_object_set (self, "connection-state", connection_state, NULL);
  gabble_jingle_content_set_transport_state (priv->content, ts);

OUT:
  tp_svc_media_stream_handler_return_from_stream_state (context);
}


/**
 * gabble_media_stream_supported_codecs
 *
 * Implements D-Bus method SupportedCodecs
 * on interface org.freedesktop.Telepathy.Media.StreamHandler
 */
static void
gabble_media_stream_supported_codecs (TpSvcMediaStreamHandler *iface,
                                      const GPtrArray *codecs,
                                      DBusGMethodInvocation *context)
{
  GabbleMediaStream *self = GABBLE_MEDIA_STREAM (iface);
  GabbleMediaStreamPrivate *priv = self->priv;
  GError *error = NULL;

  DEBUG ("called");

  if (codecs->len == 0)
    {
      GError e = { TP_ERROR, TP_ERROR_INVALID_ARGUMENT,
                   "SupportedCodecs must have a non-empty list of codecs" };

      dbus_g_method_return_error (context, &e);
      return;
    }

  priv->local_codecs_set = TRUE;

  if (priv->awaiting_intersection)
    {
      if (!pass_local_codecs (self, codecs, TRUE, &error))
        {
          DEBUG ("failed: %s", error->message);

          dbus_g_method_return_error (context, error);
          g_error_free (error);
          return;
        }

      priv->awaiting_intersection = FALSE;
    }
  else
    {
      /* If we created the stream, we don't need to send the intersection. If
       * we didn't create it, but have already sent the intersection once, we
       * don't need to send it again. In either case, extra calls to
       * SupportedCodecs are in response to an incoming description-info, which
       * can only change parameters and which XEP-0167 §10 says is purely
       * advisory.
       */
      DEBUG ("we already sent, or don't need to send, our codecs");
    }

  tp_svc_media_stream_handler_return_from_supported_codecs (context);
}

/**
 * gabble_media_stream_codecs_updated
 *
 * Implements D-Bus method CodecsUpdated
 * on interface org.freedesktop.Telepathy.Media.StreamHandler
 */
static void
gabble_media_stream_codecs_updated (TpSvcMediaStreamHandler *iface,
                                    const GPtrArray *codecs,
                                    DBusGMethodInvocation *context)
{
  GabbleMediaStream *self = GABBLE_MEDIA_STREAM (iface);
  GError *error = NULL;

  if (!self->priv->local_codecs_set)
    {
      GError e = { TP_ERROR, TP_ERROR_NOT_AVAILABLE,
          "CodecsUpdated may only be called once an initial set of codecs "
          "has been set" };

      dbus_g_method_return_error (context, &e);
      return;
    }

  if (self->priv->awaiting_intersection)
    {
      /* When awaiting an intersection the initial set of codecs should be set
       * by calling SupportedCodecs as that is the canonical set of codecs,
       * updates are only meaningful afterwards */
      tp_svc_media_stream_handler_return_from_codecs_updated (context);
      return;
    }

  if (pass_local_codecs (self, codecs, self->priv->created_locally, &error))
    {
      tp_svc_media_stream_handler_return_from_codecs_updated (context);
    }
  else
    {
      DEBUG ("failed: %s", error->message);

      dbus_g_method_return_error (context, error);
      g_error_free (error);
    }
}

/**
 * gabble_media_stream_supported_header_extensions
 *
 * Implements D-Bus method SupportedHeaderExtensions
 * on interface org.freedesktop.Telepathy.Media.StreamHandler
 */
static void
gabble_media_stream_supported_header_extensions (TpSvcMediaStreamHandler *iface,
                                                 const GPtrArray *hdrexts,
                                                 DBusGMethodInvocation *context)
{
  GabbleMediaStream *self = GABBLE_MEDIA_STREAM (iface);

  g_value_set_boxed (&self->priv->local_rtp_hdrexts, hdrexts);

  tp_svc_media_stream_handler_return_from_supported_header_extensions (context);
}

/**
 * gabble_media_stream_supported_feedback_messages
 *
 * Implements D-Bus method SupportedFeedbackMessages
 * on interface org.freedesktop.Telepathy.Media.StreamHandler
 */
static void
gabble_media_stream_supported_feedback_messages (TpSvcMediaStreamHandler *iface,
                                                 GHashTable *messages,
                                                 DBusGMethodInvocation *context)
{
  GabbleMediaStream *self = GABBLE_MEDIA_STREAM (iface);

  g_value_set_boxed (&self->priv->local_feedback_messages, messages);

  tp_svc_media_stream_handler_return_from_supported_feedback_messages (context);
}

void
gabble_media_stream_close (GabbleMediaStream *stream)
{
  GabbleMediaStreamPrivate *priv;

  g_assert (GABBLE_IS_MEDIA_STREAM (stream));

  priv = stream->priv;

  if (!priv->closed)
    {
      priv->closed = TRUE;
      tp_svc_media_stream_handler_emit_close (stream);
    }
}

static void
insert_feedback_message (JingleFeedbackMessage *fb, GPtrArray *fb_msgs)
{
  GValueArray *msg;

  msg = tp_value_array_build (3,
      G_TYPE_STRING, fb->type,
      G_TYPE_STRING, fb->subtype,
      G_TYPE_STRING, "",
      G_TYPE_INVALID);

  g_ptr_array_add (fb_msgs, msg);
}

static void
new_remote_media_description_cb (GabbleJingleContent *content,
    JingleMediaDescription *md, GabbleMediaStream *stream)
{
  GabbleMediaStreamPrivate *priv;
  GList *li;
  GPtrArray *codecs;
  GPtrArray *hdrexts;
  GHashTable *fbs;
  GType codec_struct_type = TP_STRUCT_TYPE_MEDIA_STREAM_HANDLER_CODEC;
  gboolean have_initiator = FALSE;
  gboolean initiated_by_us;

  DEBUG ("called");

  g_assert (GABBLE_IS_MEDIA_STREAM (stream));

  priv = stream->priv;

  codecs = g_value_get_boxed (&priv->remote_codecs);

  if (codecs->len != 0)
    {
      /* We already had some codecs; let's free the old list and make a new,
       * empty one to fill in.
       */
      g_value_reset (&priv->remote_codecs);
      codecs = dbus_g_type_specialized_construct (
          TP_ARRAY_TYPE_MEDIA_STREAM_HANDLER_CODEC_LIST);
      g_value_take_boxed (&priv->remote_codecs, codecs);
    }

  hdrexts = g_value_get_boxed (&priv->remote_rtp_hdrexts);

  if (hdrexts->len != 0)
    {
      /* We already had some rtp hdrext; let's free the old list and make a new,
       * empty one to fill in.
       */
      g_value_reset (&priv->remote_rtp_hdrexts);
      hdrexts = dbus_g_type_specialized_construct (
          TP_ARRAY_TYPE_RTP_HEADER_EXTENSIONS_LIST);
      g_value_take_boxed (&priv->remote_rtp_hdrexts, hdrexts);
    }

  fbs = g_value_get_boxed (&priv->remote_feedback_messages);

  if (g_hash_table_size (fbs) != 0)
    {
      /* We already had some rtp hdrext; let's free the old list and make a new,
       * empty one to fill in.
       */
      g_value_reset (&priv->remote_feedback_messages);
      fbs = dbus_g_type_specialized_construct (
          TP_HASH_TYPE_RTCP_FEEDBACK_MESSAGE_MAP);
      g_value_take_boxed (&priv->remote_feedback_messages, fbs);
    }

  for (li = md->codecs; li; li = li->next)
    {
      GValue codec = { 0, };
      JingleCodec *c = li->data;

      g_value_init (&codec, codec_struct_type);
      g_value_take_boxed (&codec,
          dbus_g_type_specialized_construct (codec_struct_type));

      DEBUG ("new remote %s codec: %u '%s' %u %u %u",
          priv->media_type == TP_MEDIA_STREAM_TYPE_AUDIO ? "audio" : "video",
          c->id, c->name, priv->media_type, c->clockrate, c->channels);

      dbus_g_type_struct_set (&codec,
          0, c->id,
          1, c->name,
          2, priv->media_type,
          3, c->clockrate,
          4, c->channels,
          5, c->params,
          G_MAXUINT);

      if (md->trr_int != G_MAXUINT || c->trr_int != G_MAXUINT ||
          md->feedback_msgs != NULL || c->feedback_msgs != NULL)
        {
          GValueArray *fb_msg_props;
          guint trr_int;
          GPtrArray *fb_msgs = g_ptr_array_new ();

          if (c->trr_int != G_MAXUINT)
            trr_int = c->trr_int;
          else
            trr_int = md->trr_int;

          g_list_foreach (md->feedback_msgs, (GFunc) insert_feedback_message,
              fb_msgs);
          g_list_foreach (c->feedback_msgs, (GFunc) insert_feedback_message,
              fb_msgs);

          fb_msg_props = tp_value_array_build (2,
              G_TYPE_UINT, trr_int,
              TP_ARRAY_TYPE_RTCP_FEEDBACK_MESSAGE_LIST, fb_msgs,
              G_TYPE_INVALID);

          g_boxed_free (TP_ARRAY_TYPE_RTCP_FEEDBACK_MESSAGE_LIST, fb_msgs);

          g_hash_table_insert (fbs, GUINT_TO_POINTER (c->id), fb_msg_props);
        }

      g_ptr_array_add (codecs, g_value_get_boxed (&codec));
    }

  for (li = md->hdrexts; li; li = li->next)
    {
      JingleRtpHeaderExtension *h = li->data;
      TpMediaStreamDirection direction;

      if (!have_initiator)
        {
          g_object_get (priv->content->session, "local-initiator",
              &initiated_by_us, NULL);
          have_initiator = TRUE;
        }

      switch (h->senders)
        {
        case JINGLE_CONTENT_SENDERS_BOTH:
          direction = TP_MEDIA_STREAM_DIRECTION_BIDIRECTIONAL;
          break;
        case JINGLE_CONTENT_SENDERS_NONE:
          direction = TP_MEDIA_STREAM_DIRECTION_NONE;
          break;
        case JINGLE_CONTENT_SENDERS_INITIATOR:
          direction = initiated_by_us ? TP_MEDIA_STREAM_DIRECTION_SEND :
          TP_MEDIA_STREAM_DIRECTION_RECEIVE;
          break;
        case JINGLE_CONTENT_SENDERS_RESPONDER:
          direction = initiated_by_us ? TP_MEDIA_STREAM_DIRECTION_RECEIVE :
          TP_MEDIA_STREAM_DIRECTION_SEND;
          break;
        default:
          g_assert_not_reached ();
        }

      DEBUG ("new RTP header ext : %u %s", h->id, h->uri);

      g_ptr_array_add (hdrexts,
          tp_value_array_build (4,
              G_TYPE_UINT,  h->id,
              G_TYPE_UINT, direction,
              G_TYPE_STRING, h->uri,
              G_TYPE_STRING, "", /* No protocol defines parameters */
              G_TYPE_INVALID));
    }

  DEBUG ("pushing remote codecs");

  push_remote_media_description (stream);
}


static void
push_remote_media_description (GabbleMediaStream *stream)
{
  GabbleMediaStreamPrivate *priv;
  GPtrArray *codecs;
  GPtrArray *hdrexts;
  GHashTable *fbs;

  g_assert (GABBLE_IS_MEDIA_STREAM (stream));

  priv = stream->priv;

  if (!priv->ready)
    return;

  codecs = g_value_get_boxed (&priv->remote_codecs);
  if (codecs->len == 0)
    return;

  hdrexts = g_value_get_boxed (&priv->remote_rtp_hdrexts);

  fbs = g_value_get_boxed (&priv->remote_feedback_messages);

  DEBUG ("passing %d remote codecs to stream-engine",
                   codecs->len);

  tp_svc_media_stream_handler_emit_set_remote_header_extensions (stream,
      hdrexts);
  tp_svc_media_stream_handler_emit_set_remote_feedback_messages (stream, fbs);
  tp_svc_media_stream_handler_emit_set_remote_codecs (stream, codecs);
}

static void
new_remote_candidates_cb (GabbleJingleContent *content,
    GList *clist, GabbleMediaStream *stream)
{
  GabbleMediaStreamPrivate *priv = stream->priv;
  GPtrArray *candidates;
  GList *li;

  candidates = g_value_get_boxed (&priv->remote_candidates);

  DEBUG ("got new remote candidates");

  for (li = clist; li; li = li->next)
    {
      gchar *candidate_id;
      GValue candidate = { 0, };
      GPtrArray *transports;
      GValue transport = { 0, };
      JingleCandidate *c = li->data;
      GType transport_struct_type = TP_STRUCT_TYPE_MEDIA_STREAM_HANDLER_TRANSPORT;
      GType candidate_struct_type = TP_STRUCT_TYPE_MEDIA_STREAM_HANDLER_CANDIDATE;

      g_value_init (&transport, transport_struct_type);
      g_value_take_boxed (&transport,
          dbus_g_type_specialized_construct (transport_struct_type));

      dbus_g_type_struct_set (&transport,
          0, c->component,
          1, c->address,
          2, c->port,
          3, c->protocol == JINGLE_TRANSPORT_PROTOCOL_UDP ? 0 : 1,
          4, "RTP",
          5, "AVP",
          6, (gdouble) (c->preference / 65536.0),
          7, c->type, /* FIXME: we're relying on 1:1 tp/jingle candidate type enums */
          8, c->username,
          9, c->password,
          G_MAXUINT);

      transports = g_ptr_array_sized_new (1);
      g_ptr_array_add (transports, g_value_get_boxed (&transport));

      g_value_init (&candidate, candidate_struct_type);
      g_value_take_boxed (&candidate,
          dbus_g_type_specialized_construct (candidate_struct_type));

      if (c->id == NULL)
        /* FIXME: is this naming scheme sensible? */
        candidate_id = g_strdup_printf ("R%d", ++priv->remote_candidate_count);
      else
        candidate_id = c->id;

      dbus_g_type_struct_set (&candidate,
          0, candidate_id,
          1, transports,
          G_MAXUINT);

      g_free (candidate_id);
      g_value_unset (&transport);
      g_ptr_array_unref (transports);

      g_ptr_array_add (candidates, g_value_get_boxed (&candidate));
    }

  push_remote_candidates (stream);
}

static void
content_state_changed_cb (GabbleJingleContent *c,
                          GParamSpec *pspec,
                          GabbleMediaStream *stream)
{
  GabbleMediaStreamPrivate *priv = stream->priv;
  JingleContentState state;

  g_object_get (c, "state", &state, NULL);

  DEBUG ("called");

  switch (state) {
    case JINGLE_CONTENT_STATE_ACKNOWLEDGED:
      /* connected stream means we can play, but sending is determined
       * by content senders (in update_senders) */
      stream->playing = TRUE;
      update_sending (stream, TRUE);
      push_playing (stream);
      push_sending (stream);
      break;
    case JINGLE_CONTENT_STATE_REMOVING:
      stream->playing = FALSE;
      priv->sending = FALSE;
      push_playing (stream);
      break;
    default:
      /* so gcc doesn't cry */
      break;
  }
}

static void
push_remote_candidates (GabbleMediaStream *stream)
{
  GabbleMediaStreamPrivate *priv;
  GPtrArray *candidates;
  guint i;
  GType candidate_list_type =
      TP_ARRAY_TYPE_MEDIA_STREAM_HANDLER_CANDIDATE_LIST;

  g_assert (GABBLE_IS_MEDIA_STREAM (stream));

  priv = stream->priv;

  candidates = g_value_get_boxed (&priv->remote_candidates);

  if (candidates->len == 0)
    return;

  if (!priv->ready)
    return;

  for (i = 0; i < candidates->len; i++)
    {
      GValueArray *candidate = g_ptr_array_index (candidates, i);
      const gchar *candidate_id;
      const GPtrArray *transports;

      candidate_id = g_value_get_string (g_value_array_get_nth (candidate, 0));
      transports = g_value_get_boxed (g_value_array_get_nth (candidate, 1));

      DEBUG ("passing 1 remote candidate to stream engine: %s", candidate_id);
      tp_svc_media_stream_handler_emit_add_remote_candidate (
          stream, candidate_id, transports);
    }

  g_value_take_boxed (&priv->remote_candidates,
      dbus_g_type_specialized_construct (candidate_list_type));
}

static void
push_playing (GabbleMediaStream *stream)
{
  GabbleMediaStreamPrivate *priv;

  g_assert (GABBLE_IS_MEDIA_STREAM (stream));

  priv = stream->priv;

  if (!priv->ready)
    return;

  DEBUG ("stream %s emitting SetStreamPlaying(%s)",
      stream->name, stream->playing ? "true" : "false");

  tp_svc_media_stream_handler_emit_set_stream_playing (
      stream, stream->playing);
}

static void
push_sending (GabbleMediaStream *stream)
{
  GabbleMediaStreamPrivate *priv;
  gboolean emit;

  g_assert (GABBLE_IS_MEDIA_STREAM (stream));

  priv = stream->priv;

  if (!priv->ready)
    return;

  emit = (priv->sending && !(priv->on_hold));
  DEBUG ("stream %s emitting SetStreamSending(%s); sending=%s, on_hold=%s",
      stream->name, emit ? "true" : "false", priv->sending ? "true" : "false",
      priv->on_hold ? "true" : "false");

  tp_svc_media_stream_handler_emit_set_stream_sending (
      stream, emit);
}

static void
update_direction (GabbleMediaStream *stream, GabbleJingleContent *c)
{
  CombinedStreamDirection new_combined_dir;
  TpMediaStreamDirection requested_dir, current_dir;
  TpMediaStreamPendingSend pending_send;
  JingleContentSenders senders;
  gboolean local_initiator;

  DEBUG ("called");

  g_object_get (c, "senders", &senders, NULL);
  g_object_get (c->session, "local-initiator", &local_initiator, NULL);

  switch (senders) {
      case JINGLE_CONTENT_SENDERS_INITIATOR:
        requested_dir = local_initiator ?
          TP_MEDIA_STREAM_DIRECTION_SEND : TP_MEDIA_STREAM_DIRECTION_RECEIVE;
        break;
      case JINGLE_CONTENT_SENDERS_RESPONDER:
        requested_dir = local_initiator ?
          TP_MEDIA_STREAM_DIRECTION_RECEIVE : TP_MEDIA_STREAM_DIRECTION_SEND;
        break;
      case JINGLE_CONTENT_SENDERS_BOTH:
        requested_dir = TP_MEDIA_STREAM_DIRECTION_BIDIRECTIONAL;
        break;
      default:
        requested_dir = TP_MEDIA_STREAM_DIRECTION_NONE;
  }

  current_dir = COMBINED_DIRECTION_GET_DIRECTION (stream->combined_direction);
  pending_send = COMBINED_DIRECTION_GET_PENDING_SEND
    (stream->combined_direction);

  /* if local sending has been added, remove it,
   * and set the pending local send flag */
  if (((current_dir & TP_MEDIA_STREAM_DIRECTION_SEND) == 0) &&
    ((requested_dir & TP_MEDIA_STREAM_DIRECTION_SEND) != 0))
    {
      DEBUG ("setting pending local send flag");
      requested_dir &= ~TP_MEDIA_STREAM_DIRECTION_SEND;
      pending_send |= TP_MEDIA_STREAM_PENDING_LOCAL_SEND;
    }

  /* make any necessary changes */
  new_combined_dir = MAKE_COMBINED_DIRECTION (requested_dir, pending_send);
  if (new_combined_dir != stream->combined_direction)
    {
      g_object_set (stream, "combined-direction", new_combined_dir, NULL);
      update_sending (stream, FALSE);
    }

}

static void
content_senders_changed_cb (GabbleJingleContent *c,
                            GParamSpec *pspec,
                            GabbleMediaStream *stream)
{
  update_direction (stream, c);
}

static void
remote_state_changed_cb (GabbleJingleSession *session,
    GabbleMediaStream *stream)
{
  GabbleMediaStreamPrivate *priv = stream->priv;
  gboolean old_hold = priv->on_hold;

  priv->on_hold = gabble_jingle_session_get_remote_hold (session);

  if (old_hold != priv->on_hold)
    push_sending (stream);
}

static void
content_removed_cb (GabbleJingleContent *content, GabbleMediaStream *stream)
{
  gabble_media_stream_close (stream);
}


gboolean
gabble_media_stream_change_direction (GabbleMediaStream *stream,
    guint requested_dir, GError **error)
{
  GabbleMediaStreamPrivate *priv = stream->priv;
  CombinedStreamDirection new_combined_dir;
  TpMediaStreamDirection current_dir;
  TpMediaStreamPendingSend pending_send;
  JingleContentSenders senders;
  gboolean local_initiator;

  current_dir = COMBINED_DIRECTION_GET_DIRECTION (stream->combined_direction);
  pending_send = COMBINED_DIRECTION_GET_PENDING_SEND
    (stream->combined_direction);

  /* if we're awaiting a local decision on sending... */
  if ((pending_send & TP_MEDIA_STREAM_PENDING_LOCAL_SEND) != 0)
    {
      /* clear the flag */
      pending_send &= ~TP_MEDIA_STREAM_PENDING_LOCAL_SEND;

      /* make our current_dir match what other end thinks (he thinks we're
       * bidirectional) so that we send the correct transitions */
      current_dir ^= TP_MEDIA_STREAM_DIRECTION_SEND;
    }

  /* make any necessary changes */
  new_combined_dir = MAKE_COMBINED_DIRECTION (requested_dir, pending_send);
  if (new_combined_dir != stream->combined_direction)
    {
      JingleContentState state;
      gboolean start_sending;

      g_object_set (stream, "combined-direction", new_combined_dir, NULL);

      /* We would like to emit SetStreamSending(True) (if appropriate) only if:
       *  - the content was locally created, or
       *  - the user explicitly okayed the content.
       * This appears to be the meaning of Acknowledged. :-)
       */
      g_object_get (stream->priv->content, "state", &state, NULL);
      start_sending = (state == JINGLE_CONTENT_STATE_ACKNOWLEDGED);

      update_sending (stream, start_sending);
    }

  DEBUG ("current_dir: %u, requested_dir: %u", current_dir, requested_dir);

  /* short-circuit sending a request if we're not asking for anything new */
  if (current_dir == requested_dir)
    return TRUE;

  g_object_get (priv->content->session, "local-initiator", &local_initiator, NULL);

  switch (requested_dir)
    {
      case TP_MEDIA_STREAM_DIRECTION_SEND:
        senders = local_initiator ?
          JINGLE_CONTENT_SENDERS_INITIATOR : JINGLE_CONTENT_SENDERS_RESPONDER;
        break;

      case TP_MEDIA_STREAM_DIRECTION_RECEIVE:
        senders = local_initiator ?
          JINGLE_CONTENT_SENDERS_RESPONDER : JINGLE_CONTENT_SENDERS_INITIATOR;
        break;

      case TP_MEDIA_STREAM_DIRECTION_BIDIRECTIONAL:
        senders = JINGLE_CONTENT_SENDERS_BOTH;
        break;

      default:
        g_assert_not_reached ();
    }

  if (!gabble_jingle_content_change_direction (priv->content, senders))
    {
      g_set_error (error, TP_ERROR, TP_ERROR_NOT_AVAILABLE,
          "stream direction invalid for the Jingle dialect in use");
      return FALSE;
    }

  return TRUE;
}

void
gabble_media_stream_accept_pending_local_send (GabbleMediaStream *stream)
{
  CombinedStreamDirection combined_dir = stream->combined_direction;
  TpMediaStreamDirection current_dir;
  TpMediaStreamPendingSend pending_send;

  current_dir = COMBINED_DIRECTION_GET_DIRECTION (combined_dir);
  pending_send = COMBINED_DIRECTION_GET_PENDING_SEND (combined_dir);

  if ((pending_send & TP_MEDIA_STREAM_PENDING_LOCAL_SEND) != 0)
    {
      DEBUG ("accepting pending local send on stream %s", stream->name);

      gabble_media_stream_change_direction (stream,
          current_dir | TP_MEDIA_STREAM_DIRECTION_SEND, NULL);
    }
  else
    {
      DEBUG ("stream %s not pending local send", stream->name);
    }
}

static void
update_sending (GabbleMediaStream *stream, gboolean start_sending)
{
  GabbleMediaStreamPrivate *priv = stream->priv;
  gboolean new_sending;

  new_sending =
    ((stream->combined_direction & TP_MEDIA_STREAM_DIRECTION_SEND) != 0);

  if (priv->sending == new_sending)
    return;

  if (new_sending && !start_sending)
    return;

  priv->sending = new_sending;
  push_sending (stream);
}

static void
stream_handler_iface_init (gpointer g_iface, gpointer iface_data)
{
  TpSvcMediaStreamHandlerClass *klass =
    (TpSvcMediaStreamHandlerClass *) g_iface;

#define IMPLEMENT(x,suffix) tp_svc_media_stream_handler_implement_##x (\
    klass, gabble_media_stream_##x##suffix)
  IMPLEMENT(codec_choice,);
  IMPLEMENT(error,_async);
  IMPLEMENT(hold_state,);
  IMPLEMENT(native_candidates_prepared,);
  IMPLEMENT(new_active_candidate_pair,);
  IMPLEMENT(new_native_candidate,);
  IMPLEMENT(ready,);
  IMPLEMENT(set_local_codecs,);
  IMPLEMENT(stream_state,);
  IMPLEMENT(supported_codecs,);
  IMPLEMENT(unhold_failure,);
  IMPLEMENT(codecs_updated,);
  IMPLEMENT(supported_header_extensions,);
  IMPLEMENT(supported_feedback_messages,);
#undef IMPLEMENT
}

GabbleJingleMediaRtp *
gabble_media_stream_get_content (GabbleMediaStream *self)
{
  /* FIXME: we should fix this whole class up. It relies throughout on
   *        self->priv->content actually secretly being a GabbleJingleMediaRtp.
   */
  return GABBLE_JINGLE_MEDIA_RTP (self->priv->content);
}

void
gabble_media_stream_start_telephony_event (GabbleMediaStream *self,
    guchar event)
{
  DEBUG ("stream %s: %c", self->name, tp_dtmf_event_to_char (event));

  tp_svc_media_stream_handler_emit_start_telephony_event (
      (TpSvcMediaStreamHandler *) self, event);
}

void
gabble_media_stream_stop_telephony_event (GabbleMediaStream *self)
{
  DEBUG ("stream %s", self->name);

  tp_svc_media_stream_handler_emit_stop_telephony_event (
      (TpSvcMediaStreamHandler *) self);
}

void
gabble_media_stream_add_dtmf_player (GabbleMediaStream *self,
    TpDTMFPlayer *dtmf_player)
{
  tp_g_signal_connect_object (dtmf_player, "started-tone",
      G_CALLBACK (gabble_media_stream_start_telephony_event), self,
      G_CONNECT_SWAPPED);
  tp_g_signal_connect_object (dtmf_player, "stopped-tone",
      G_CALLBACK (gabble_media_stream_stop_telephony_event), self,
      G_CONNECT_SWAPPED);
}
