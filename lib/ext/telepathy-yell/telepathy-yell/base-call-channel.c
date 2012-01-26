/*
 * base-call-channel.c - Source for TpyBaseCallChannel
 * Copyright © 2009–2010 Collabora Ltd.
 * @author Sjoerd Simons <sjoerd.simons@collabora.co.uk>
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

#include "base-call-channel.h"

#include <stdio.h>
#include <stdlib.h>

#include <gio/gio.h>

#include <telepathy-glib/dbus.h>
#include <telepathy-glib/dtmf.h>
#include <telepathy-glib/enums.h>
#include <telepathy-glib/exportable-channel.h>
#include <telepathy-glib/interfaces.h>
#include <telepathy-glib/channel-iface.h>
#include <telepathy-glib/svc-channel.h>
#include <telepathy-glib/svc-properties-interface.h>
#include <telepathy-glib/base-connection.h>
#include <telepathy-glib/gtypes.h>

#include <telepathy-yell/enums.h>
#include <telepathy-yell/gtypes.h>
#include <telepathy-yell/interfaces.h>
#include <telepathy-yell/svc-call.h>
#include <telepathy-yell/base-call-content.h>

#define DEBUG_FLAG TPY_DEBUG_CALL
#include "debug.h"

static void call_iface_init (gpointer, gpointer);
static void tpy_base_call_channel_close (TpBaseChannel *base);
static void dtmf_iface_init (gpointer, gpointer);

G_DEFINE_TYPE_WITH_CODE(TpyBaseCallChannel, tpy_base_call_channel,
  TP_TYPE_BASE_CHANNEL,
  G_IMPLEMENT_INTERFACE (TPY_TYPE_SVC_CHANNEL_TYPE_CALL,
        call_iface_init)
  G_IMPLEMENT_INTERFACE (TP_TYPE_SVC_CHANNEL_INTERFACE_DTMF,
      dtmf_iface_init);
);

static const gchar *tpy_base_call_channel_interfaces[] = {
    NULL
};

/* properties */
enum
{
  PROP_INITIAL_AUDIO = 1,
  PROP_INITIAL_VIDEO,
  PROP_INITIAL_AUDIO_NAME,
  PROP_INITIAL_VIDEO_NAME,
  PROP_MUTABLE_CONTENTS,
  PROP_HARDWARE_STREAMING,
  PROP_CONTENTS,

  PROP_CALL_STATE,
  PROP_CALL_FLAGS,
  PROP_CALL_STATE_DETAILS,
  PROP_CALL_STATE_REASON,

  PROP_CALL_MEMBERS,

  PROP_CURRENTLY_SENDING_TONES,
  PROP_INITIAL_TONES,
  PROP_DEFERRED_TONES,

  LAST_PROPERTY
};

enum
{
  ENDED,
  LAST_SIGNAL
};

static guint signals[LAST_SIGNAL] = { 0, };


/* private structure */
struct _TpyBaseCallChannelPrivate
{
  gboolean dispose_has_run;

  GList *contents;

  gchar *initial_audio_name;
  gchar *initial_video_name;

  TpyCallState state;
  TpyCallFlags flags;
  GHashTable *details;
  GValueArray *reason;

  TpDTMFPlayer *dtmf_player;
  gchar *deferred_tones;
  gboolean have_some_audio;

  /* CallMember handle => flag hash table */
  GHashTable *call_members;
};

static void
tpy_base_call_channel_tones_deferred_cb (TpyBaseCallChannel *self,
    const gchar *tones,
    TpDTMFPlayer *dtmf_player)
{
  DEBUG ("waiting for user to continue sending '%s'", tones);

  g_free (self->priv->deferred_tones);
  self->priv->deferred_tones = g_strdup (tones);
  tp_svc_channel_interface_dtmf_emit_tones_deferred (self, tones);
}

static void
tpy_base_call_channel_constructed (GObject *obj)
{
  TpyBaseCallChannel *self = TPY_BASE_CALL_CHANNEL (obj);
  TpBaseChannel *base = TP_BASE_CHANNEL (self);

  if (G_OBJECT_CLASS (tpy_base_call_channel_parent_class)->constructed
      != NULL)
    G_OBJECT_CLASS (tpy_base_call_channel_parent_class)->constructed (obj);

  if (tp_base_channel_is_requested (base))
    tpy_base_call_channel_set_state (self,
      TPY_CALL_STATE_PENDING_INITIATOR);
  else
    tpy_base_call_channel_set_state (self,
      TPY_CALL_STATE_PENDING_RECEIVER);
}

static void
tpy_base_call_channel_init (TpyBaseCallChannel *self)
{
  TpyBaseCallChannelPrivate *priv = G_TYPE_INSTANCE_GET_PRIVATE (self,
      TPY_TYPE_BASE_CALL_CHANNEL, TpyBaseCallChannelPrivate);

  self->priv = priv;

  priv->reason = tp_value_array_build (3,
    G_TYPE_UINT, 0,
    G_TYPE_UINT, 0,
    G_TYPE_STRING, "",
    G_TYPE_INVALID);

  priv->details = tp_asv_new (NULL, NULL);

  priv->call_members = g_hash_table_new (g_direct_hash, g_direct_equal);

  priv->dtmf_player = tp_dtmf_player_new ();
  priv->have_some_audio = FALSE;

  tp_g_signal_connect_object (priv->dtmf_player, "finished",
      G_CALLBACK (tp_svc_channel_interface_dtmf_emit_stopped_tones), self,
      G_CONNECT_SWAPPED);

  tp_g_signal_connect_object (priv->dtmf_player, "tones-deferred",
      G_CALLBACK (tpy_base_call_channel_tones_deferred_cb), self,
      G_CONNECT_SWAPPED);
}

static void tpy_base_call_channel_dispose (GObject *object);
static void tpy_base_call_channel_finalize (GObject *object);

static void
tpy_base_call_channel_get_property (GObject    *object,
    guint       property_id,
    GValue     *value,
    GParamSpec *pspec)
{
  TpyBaseCallChannel *self = TPY_BASE_CALL_CHANNEL (object);
  TpyBaseCallChannelPrivate *priv = self->priv;

  switch (property_id)
    {
      case PROP_INITIAL_AUDIO:
        g_value_set_boolean (value, self->initial_audio);
        break;
      case PROP_INITIAL_VIDEO:
        g_value_set_boolean (value, self->initial_video);
        break;
      case PROP_INITIAL_AUDIO_NAME:
        g_value_set_string (value, priv->initial_audio_name);
        break;
      case PROP_INITIAL_VIDEO_NAME:
        g_value_set_string (value, priv->initial_video_name);
        break;
      case PROP_MUTABLE_CONTENTS:
      /* FIXME: this should probably move to the implementation class */
          g_value_set_boolean (value, TRUE);
        break;
      case PROP_CONTENTS:
        {
          GPtrArray *arr = g_ptr_array_sized_new (2);
          GList *l;

          for (l = priv->contents; l != NULL; l = g_list_next (l))
            {
              TpyBaseCallContent *c = TPY_BASE_CALL_CONTENT (l->data);
              g_ptr_array_add (arr,
                (gpointer) tpy_base_call_content_get_object_path (c));
            }

          g_value_set_boxed (value, arr);
          g_ptr_array_free (arr, TRUE);
          break;
        }
      case PROP_HARDWARE_STREAMING:
        g_value_set_boolean (value, FALSE);
        break;
      case PROP_CALL_STATE:
        g_value_set_uint (value, priv->state);
        break;
      case PROP_CALL_FLAGS:
        g_value_set_uint (value, priv->flags);
        break;
      case PROP_CALL_STATE_DETAILS:
        g_value_set_boxed (value, priv->details);
        break;
      case PROP_CALL_STATE_REASON:
        g_value_set_boxed (value, priv->reason);
        break;
      case PROP_CALL_MEMBERS:
        g_value_set_boxed (value, priv->call_members);
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
        G_OBJECT_WARN_INVALID_PROPERTY_ID (object, property_id, pspec);
        break;
    }
}

static void
tpy_base_call_channel_set_property (GObject *object,
    guint property_id,
    const GValue *value,
    GParamSpec *pspec)
{
  TpyBaseCallChannel *self = TPY_BASE_CALL_CHANNEL (object);
  TpyBaseCallChannelPrivate *priv = self->priv;

  switch (property_id)
    {
      case PROP_INITIAL_AUDIO:
        self->initial_audio = g_value_get_boolean (value);
        break;
      case PROP_INITIAL_VIDEO:
        self->initial_video = g_value_get_boolean (value);
        break;
      case PROP_INITIAL_AUDIO_NAME:
        priv->initial_audio_name = g_value_dup_string (value);
        break;
      case PROP_INITIAL_VIDEO_NAME:
        priv->initial_video_name = g_value_dup_string (value);
        break;
      default:
        G_OBJECT_WARN_INVALID_PROPERTY_ID (object, property_id, pspec);
        break;
  }
}

static void
tpy_base_call_channel_fill_immutable_properties (
    TpBaseChannel *chan,
    GHashTable *properties)
{
  TP_BASE_CHANNEL_CLASS (tpy_base_call_channel_parent_class)
      ->fill_immutable_properties (chan, properties);

  tp_dbus_properties_mixin_fill_properties_hash (
      G_OBJECT (chan), properties,
      TPY_IFACE_CHANNEL_TYPE_CALL, "InitialAudio",
      TPY_IFACE_CHANNEL_TYPE_CALL, "InitialAudioName",
      TPY_IFACE_CHANNEL_TYPE_CALL, "InitialVideo",
      TPY_IFACE_CHANNEL_TYPE_CALL, "InitialVideoName",
      TPY_IFACE_CHANNEL_TYPE_CALL, "MutableContents",
      TPY_IFACE_CHANNEL_TYPE_CALL, "HardwareStreaming",
      NULL);
}

static void
tpy_base_call_channel_class_init (
    TpyBaseCallChannelClass *tpy_base_call_channel_class)
{
  GObjectClass *object_class = G_OBJECT_CLASS (tpy_base_call_channel_class);
  TpBaseChannelClass *base_channel_class =
      TP_BASE_CHANNEL_CLASS (tpy_base_call_channel_class);
  GParamSpec *param_spec;
  static TpDBusPropertiesMixinPropImpl call_props[] = {
      { "CallMembers", "call-members", NULL },
      { "MutableContents", "mutable-contents", NULL },
      { "InitialAudio", "initial-audio", NULL },
      { "InitialVideo", "initial-video", NULL },
      { "InitialAudioName", "initial-audio-name", NULL },
      { "InitialVideoName", "initial-video-name", NULL },
      { "Contents", "contents", NULL },
      { "HardwareStreaming", "hardware-streaming", NULL },
      { "CallState", "call-state", NULL },
      { "CallFlags", "call-flags", NULL },
      { "CallStateReason",  "call-state-reason", NULL },
      { "CallStateDetails", "call-state-details", NULL },
      { NULL }
  };
 static TpDBusPropertiesMixinPropImpl dtmf_props[] = {
      { "CurrentlySendingTones", "currently-sending-tones", NULL },
      { "InitialTones", "initial-tones", NULL },
      { "DeferredTones", "deferred-tones", NULL },
      { NULL }
  };

  g_type_class_add_private (tpy_base_call_channel_class,
      sizeof (TpyBaseCallChannelPrivate));

  object_class->constructed = tpy_base_call_channel_constructed;

  object_class->get_property = tpy_base_call_channel_get_property;
  object_class->set_property = tpy_base_call_channel_set_property;

  object_class->dispose = tpy_base_call_channel_dispose;
  object_class->finalize = tpy_base_call_channel_finalize;

  base_channel_class->channel_type = TPY_IFACE_CHANNEL_TYPE_CALL;
  base_channel_class->interfaces = tpy_base_call_channel_interfaces;
  base_channel_class->fill_immutable_properties =
      tpy_base_call_channel_fill_immutable_properties;
  base_channel_class->close = tpy_base_call_channel_close;

  signals[ENDED] = g_signal_new ("ended",
      G_OBJECT_CLASS_TYPE (object_class),
      G_SIGNAL_RUN_LAST,
      0,
      NULL, NULL,
      g_cclosure_marshal_VOID__VOID,
      G_TYPE_NONE, 0);

  param_spec = g_param_spec_boolean ("initial-audio", "InitialAudio",
      "Whether the channel initially contained an audio stream",
      FALSE,
      G_PARAM_CONSTRUCT | G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS);
  g_object_class_install_property (object_class, PROP_INITIAL_AUDIO,
      param_spec);

  param_spec = g_param_spec_boolean ("initial-video", "InitialVideo",
      "Whether the channel initially contained an video stream",
      FALSE,
      G_PARAM_CONSTRUCT | G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS);
  g_object_class_install_property (object_class, PROP_INITIAL_VIDEO,
      param_spec);

  param_spec = g_param_spec_string ("initial-audio-name", "InitialAudioName",
      "Name for the initial audio content",
      "audio",
      G_PARAM_CONSTRUCT_ONLY | G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS);
  g_object_class_install_property (object_class, PROP_INITIAL_AUDIO_NAME,
      param_spec);

  param_spec = g_param_spec_string ("initial-video-name", "InitialVideoName",
      "Name for the initial video content",
      "video",
      G_PARAM_CONSTRUCT_ONLY | G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS);
  g_object_class_install_property (object_class, PROP_INITIAL_VIDEO_NAME,
      param_spec);

  param_spec = g_param_spec_boolean ("mutable-contents", "MutableContents",
      "Whether the set of streams on this channel are mutable once requested",
      FALSE,
      G_PARAM_READABLE | G_PARAM_STATIC_STRINGS);
  g_object_class_install_property (object_class, PROP_MUTABLE_CONTENTS,
      param_spec);

  param_spec = g_param_spec_boxed ("contents", "Contents",
      "The contents of the channel",
      TP_ARRAY_TYPE_OBJECT_PATH_LIST,
      G_PARAM_READABLE | G_PARAM_STATIC_STRINGS);
  g_object_class_install_property (object_class, PROP_CONTENTS,
      param_spec);

  param_spec = g_param_spec_boolean ("hardware-streaming", "HardwareStreaming",
      "True if all the streaming is done by hardware",
      FALSE,
      G_PARAM_READABLE | G_PARAM_STATIC_STRINGS);
  g_object_class_install_property (object_class, PROP_HARDWARE_STREAMING,
      param_spec);

  param_spec = g_param_spec_uint ("call-state", "CallState",
      "The status of the call",
      TPY_CALL_STATE_UNKNOWN,
      NUM_TPY_CALL_STATES,
      TPY_CALL_STATE_UNKNOWN,
      G_PARAM_READABLE | G_PARAM_STATIC_STRINGS);
  g_object_class_install_property (object_class, PROP_CALL_STATE, param_spec);

  param_spec = g_param_spec_uint ("call-flags", "CallFlags",
      "Flags representing the status of the call",
      0, G_MAXUINT, 0,
      G_PARAM_READABLE | G_PARAM_STATIC_STRINGS);
  g_object_class_install_property (object_class, PROP_CALL_FLAGS,
      param_spec);

  param_spec = g_param_spec_boxed ("call-state-reason", "CallStateReason",
      "The reason why the call is in the current state",
      TPY_STRUCT_TYPE_CALL_STATE_REASON,
      G_PARAM_READABLE | G_PARAM_STATIC_STRINGS);
  g_object_class_install_property (object_class, PROP_CALL_STATE_REASON,
      param_spec);

  param_spec = g_param_spec_boxed ("call-state-details", "CallStateDetails",
      "The reason why the call is in the current state",
      TP_HASH_TYPE_QUALIFIED_PROPERTY_VALUE_MAP,
      G_PARAM_READABLE | G_PARAM_STATIC_STRINGS);
  g_object_class_install_property (object_class, PROP_CALL_STATE_DETAILS,
      param_spec);

  param_spec = g_param_spec_boxed ("call-members", "CallMembers",
      "The members",
      TPY_HASH_TYPE_CALL_MEMBER_MAP,
      G_PARAM_READABLE | G_PARAM_STATIC_STRINGS);
  g_object_class_install_property (object_class, PROP_CALL_MEMBERS,
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

  tp_dbus_properties_mixin_implement_interface (object_class,
      TPY_IFACE_QUARK_CHANNEL_TYPE_CALL,
      tp_dbus_properties_mixin_getter_gobject_properties,
      NULL,
      call_props);

  tp_dbus_properties_mixin_implement_interface (object_class,
      TP_IFACE_QUARK_CHANNEL_INTERFACE_DTMF,
      tp_dbus_properties_mixin_getter_gobject_properties,
      NULL,
      dtmf_props);
}

void
tpy_base_call_channel_dispose (GObject *object)
{
  TpyBaseCallChannel *self = TPY_BASE_CALL_CHANNEL (object);
  TpyBaseCallChannelPrivate *priv = self->priv;

  DEBUG ("hello thar");

  if (priv->dispose_has_run)
    return;

  self->priv->dispose_has_run = TRUE;

  g_list_foreach (priv->contents, (GFunc) tpy_base_call_content_deinit, NULL);
  g_list_foreach (priv->contents, (GFunc) g_object_unref, NULL);
  tp_clear_pointer (&priv->contents, g_list_free);

  tp_clear_pointer (&priv->call_members, g_hash_table_unref);

  if (G_OBJECT_CLASS (tpy_base_call_channel_parent_class)->dispose)
    G_OBJECT_CLASS (tpy_base_call_channel_parent_class)->dispose (object);
}

void
tpy_base_call_channel_finalize (GObject *object)
{
  TpyBaseCallChannel *self = TPY_BASE_CALL_CHANNEL (object);
  TpyBaseCallChannelPrivate *priv = self->priv;

  g_hash_table_unref (priv->details);
  g_value_array_free (priv->reason);
  g_free (self->priv->initial_audio_name);
  g_free (self->priv->initial_video_name);
  tp_clear_pointer (&self->priv->deferred_tones, g_free);

  G_OBJECT_CLASS (tpy_base_call_channel_parent_class)->finalize (object);
}

void
tpy_base_call_channel_set_state (TpyBaseCallChannel *self,
  TpyCallState state)
{
  TpyBaseCallChannelPrivate *priv = self->priv;

  /* signal when going to the ended state */
  if (state != priv->state && state == TPY_CALL_STATE_ENDED)
    g_signal_emit (self, signals[ENDED], 0);

  priv->state = state;

  if (priv->state != TPY_CALL_STATE_PENDING_RECEIVER)
    priv->flags &= ~TPY_CALL_FLAG_LOCALLY_RINGING;

  if (tp_base_channel_is_registered (TP_BASE_CHANNEL (self)))
    tpy_svc_channel_type_call_emit_call_state_changed (self, priv->state,
      priv->flags, priv->reason, priv->details);
}

TpyCallState
tpy_base_call_channel_get_state (TpyBaseCallChannel *self)
{
  return self->priv->state;
}

void
tpy_base_call_channel_remove_content (TpyBaseCallChannel *self,
    TpyBaseCallContent *content)
{
  TpyBaseCallChannelPrivate *priv = self->priv;
  const gchar *path;
  GList *l;
  gboolean still_have_audio = FALSE;

  priv->contents = g_list_remove (priv->contents, content);

  path = tpy_base_call_content_get_object_path (
      TPY_BASE_CALL_CONTENT (content));
  tpy_svc_channel_type_call_emit_content_removed (self, path);

  tpy_base_call_content_deinit (TPY_BASE_CALL_CONTENT (content));
  g_object_unref (content);

  /* let's see if we still have any audio contents */
  for (l = priv->contents; l != NULL; l = l->next)
    {
      if (tpy_base_call_content_get_media_type (
              TPY_BASE_CALL_CONTENT (l->data)) ==
          TP_MEDIA_STREAM_TYPE_AUDIO)
        {
          still_have_audio = TRUE;
          break;
        }
    }

  if (priv->have_some_audio && !still_have_audio)
    {
      /* the last audio stream just closed */
      tp_dtmf_player_cancel (priv->dtmf_player);
    }

  priv->have_some_audio = still_have_audio;
}

void
tpy_base_call_channel_add_content (TpyBaseCallChannel *self,
    TpyBaseCallContent *content)
{
  TpyBaseCallChannelPrivate *priv = self->priv;

  g_signal_connect_swapped (content, "removed",
      G_CALLBACK (tpy_base_call_channel_remove_content), self);

  priv->contents = g_list_prepend (priv->contents, content);

  if (tpy_base_call_content_get_media_type (content)
      == TP_MEDIA_STREAM_TYPE_AUDIO)
    priv->have_some_audio = TRUE;

  tpy_svc_channel_type_call_emit_content_added (self,
     tpy_base_call_content_get_object_path (content));
}

static void
tpy_base_call_channel_close (TpBaseChannel *base)
{
  TpyBaseCallChannel *self = TPY_BASE_CALL_CHANNEL (base);
  TpyBaseCallChannelPrivate *priv = self->priv;

  /* FIXME de-init members */
  DEBUG ("Closing media channel %s", tp_base_channel_get_object_path (base));

  /* shutdown all our contents */
  g_list_foreach (priv->contents, (GFunc) tpy_base_call_content_deinit,
      NULL);
  g_list_foreach (priv->contents, (GFunc) g_object_unref, NULL);
  tp_clear_pointer (&priv->contents, g_list_free);

  tp_base_channel_destroyed (base);
}

static void
tpy_base_call_channel_set_ringing (TpySvcChannelTypeCall *iface,
    DBusGMethodInvocation *context)
{
  TpyBaseCallChannel *self = TPY_BASE_CALL_CHANNEL (iface);
  TpyBaseCallChannelPrivate *priv = self->priv;
  TpBaseChannel *tp_base = TP_BASE_CHANNEL (self);

  if (tp_base_channel_is_requested (tp_base))
    {
      GError e = { TP_ERRORS, TP_ERROR_INVALID_ARGUMENT,
          "Call was requested. Ringing doesn't make sense." };
      dbus_g_method_return_error (context, &e);
    }
  else if (priv->state != TPY_CALL_STATE_PENDING_RECEIVER)
    {
      GError e = { TP_ERRORS, TP_ERROR_NOT_AVAILABLE,
          "Call is not in the right state for Ringing." };
      dbus_g_method_return_error (context, &e);
    }
  else
    {
      if ((priv->flags & TPY_CALL_FLAG_LOCALLY_RINGING) == 0)
        {
          DEBUG ("Client is ringing");
          priv->flags |= TPY_CALL_FLAG_LOCALLY_RINGING;
          tpy_base_call_channel_set_state (self, priv->state);
        }

      tpy_svc_channel_type_call_return_from_set_ringing (context);
    }
}

static void
tpy_base_call_channel_accept (TpySvcChannelTypeCall *iface,
        DBusGMethodInvocation *context)
{
  TpyBaseCallChannel *self = TPY_BASE_CALL_CHANNEL (iface);
  TpyBaseCallChannelPrivate *priv = self->priv;
  TpyBaseCallChannelClass *base_class =
      TPY_BASE_CALL_CHANNEL_GET_CLASS (self);
  TpBaseChannel *tp_base = TP_BASE_CHANNEL (self);

  DEBUG ("Client accepted the call");

  if (tp_base_channel_is_requested (tp_base))
    {
      if (priv->state == TPY_CALL_STATE_PENDING_INITIATOR)
        {
          tpy_base_call_channel_set_state (self,
              TPY_CALL_STATE_PENDING_RECEIVER);
        }
      else
        {
          DEBUG ("Invalid state for Accept: Channel requested and "
              "state == %d", priv->state);
          goto err;
        }
    }
  else if (priv->state < TPY_CALL_STATE_ACCEPTED)
    {
      tpy_base_call_channel_set_state (self,
        TPY_CALL_STATE_ACCEPTED);
    }
  else
    {
      DEBUG ("Invalid state for Accept: state == %d", priv->state);
      goto err;
    }

  if (base_class->accept != NULL)
    base_class->accept (self);

  g_list_foreach (self->priv->contents,
      (GFunc)tpy_base_call_content_accepted, NULL);

  tpy_svc_channel_type_call_return_from_accept (context);
  return;

err:
  {
    GError e = { TP_ERRORS, TP_ERROR_NOT_AVAILABLE,
        "Invalid state for Accept" };
    dbus_g_method_return_error (context, &e);
  }
}

static void
tpy_base_call_channel_hangup (TpySvcChannelTypeCall *iface,
  guint reason,
  const gchar *detailed_reason,
  const gchar *message,
  DBusGMethodInvocation *context)
{
  TpyBaseCallChannel *self = TPY_BASE_CALL_CHANNEL (iface);
  TpyBaseCallChannelClass *base_class =
      TPY_BASE_CALL_CHANNEL_GET_CLASS (self);

  if (base_class->hangup)
    base_class->hangup (self, reason, detailed_reason, message);

  tpy_base_call_channel_set_state ( TPY_BASE_CALL_CHANNEL (self),
          TPY_CALL_STATE_ENDED);

  tpy_svc_channel_type_call_return_from_hangup (context);
}

static void
tpy_base_call_channel_add_content_dbus (TpySvcChannelTypeCall *iface,
  const gchar *name,
  TpMediaStreamType mtype,
  DBusGMethodInvocation *context)
{
  TpyBaseCallChannel *self = TPY_BASE_CALL_CHANNEL (iface);
  TpyBaseCallChannelPrivate *priv = self->priv;
  TpyBaseCallChannelClass *base_class =
      TPY_BASE_CALL_CHANNEL_GET_CLASS (self);
  GError *error = NULL;
  TpyBaseCallContent *content;

  if (priv->state == TPY_CALL_STATE_ENDED)
    {
      g_set_error (&error, TP_ERRORS, TP_ERROR_NOT_AVAILABLE,
        "No contents can be added. The call has already ended.");
      goto error;
    }

  if (mtype >= NUM_TP_MEDIA_STREAM_TYPES)
    goto unicorns;

  content = base_class->add_content (self, name, mtype, &error);

  if (content == NULL)
    goto error;

  tpy_svc_channel_type_call_return_from_add_content (context,
      tpy_base_call_content_get_object_path (
          TPY_BASE_CALL_CONTENT (content)));
  return;

unicorns:
  {
    GError e = { TP_ERRORS, TP_ERROR_NOT_IMPLEMENTED, "Unknown content type" };
    dbus_g_method_return_error (context, &e);
    return;
  }

error:
  dbus_g_method_return_error (context, error);
  g_error_free (error);
}


static void
call_iface_init (gpointer g_iface, gpointer iface_data)
{
  TpySvcChannelTypeCallClass *klass =
    (TpySvcChannelTypeCallClass *) g_iface;

#define IMPLEMENT(x, suffix) tpy_svc_channel_type_call_implement_##x (\
    klass, tpy_base_call_channel_##x##suffix)
  IMPLEMENT(set_ringing,);
  IMPLEMENT(accept,);
  IMPLEMENT(hangup,);
  IMPLEMENT(add_content, _dbus);
#undef IMPLEMENT
}

#define TONE_MS 200
#define GAP_MS 100
#define PAUSE_MS 3000
/* arbitrary limit on the length of a tone started with StartTone */
#define MAX_TONE_SECONDS 10

static void
tpy_base_call_channel_start_tone (TpSvcChannelInterfaceDTMF *iface,
    guint stream_id G_GNUC_UNUSED,
    guchar event,
    DBusGMethodInvocation *context)
{
  TpyBaseCallChannel *self = TPY_BASE_CALL_CHANNEL (iface);
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
tpy_base_call_channel_stop_tone (TpSvcChannelInterfaceDTMF *iface,
    guint stream_id,
    DBusGMethodInvocation *context)
{
  TpyBaseCallChannel *self = TPY_BASE_CALL_CHANNEL (iface);

  tp_dtmf_player_cancel (self->priv->dtmf_player);
  tp_svc_channel_interface_dtmf_return_from_stop_tone (context);
}

static void
tpy_base_call_channel_multiple_tones (
    TpSvcChannelInterfaceDTMF *iface,
    const gchar *dialstring,
    DBusGMethodInvocation *context)
{
  TpyBaseCallChannel *self = TPY_BASE_CALL_CHANNEL (iface);
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
dtmf_iface_init (gpointer g_iface, gpointer iface_data)
{
  TpSvcChannelInterfaceDTMFClass *klass = g_iface;

#define IMPLEMENT(x) tp_svc_channel_interface_dtmf_implement_##x (\
    klass, tpy_base_call_channel_##x)
  IMPLEMENT(start_tone);
  IMPLEMENT(stop_tone);
  IMPLEMENT(multiple_tones);
#undef IMPLEMENT
}

static void
base_call_channel_signal_call_members (TpyBaseCallChannel *self,
  TpHandle removed_handle)
{
  GArray *removals = g_array_new (TRUE, TRUE, sizeof (TpHandle));

  if (removed_handle != 0)
    g_array_append_val (removals, removed_handle);

  tpy_svc_channel_type_call_emit_call_members_changed (self,
      self->priv->call_members, removals);

  g_array_unref (removals);
}

void
tpy_base_call_channel_add_member (TpyBaseCallChannel *self,
    TpHandle handle,
    TpyCallMemberFlags initial_flags)
{
  TpyBaseCallChannelPrivate *priv = self->priv;

  DEBUG ("Member %d (flags: %d) added", handle, initial_flags);

  g_assert (!g_hash_table_lookup_extended (priv->call_members,
    GUINT_TO_POINTER (handle), NULL, NULL));

  g_hash_table_insert (priv->call_members,
    GUINT_TO_POINTER (handle), GUINT_TO_POINTER (initial_flags));
  base_call_channel_signal_call_members (self, 0);
}

void tpy_base_call_channel_update_member_flags (TpyBaseCallChannel *self,
    TpHandle handle,
    TpyCallMemberFlags flags)
{
  TpyBaseCallChannelPrivate *priv = self->priv;

  DEBUG ("Member %d (flags: %d) updated", handle, flags);

  g_assert (g_hash_table_lookup_extended (priv->call_members,
    GUINT_TO_POINTER (handle), NULL, NULL));

  g_hash_table_replace (priv->call_members,
    GUINT_TO_POINTER (handle), GUINT_TO_POINTER (flags));
  base_call_channel_signal_call_members (self, 0);
}

void
tpy_base_call_channel_remove_member (TpyBaseCallChannel *self,
    TpHandle handle)
{
  DEBUG ("Member %d removed", handle);

  g_hash_table_remove (self->priv->call_members, GUINT_TO_POINTER (handle));
  base_call_channel_signal_call_members (self, handle);
}

GList *
tpy_base_call_channel_get_contents (TpyBaseCallChannel *self)
{
  return self->priv->contents;
}
