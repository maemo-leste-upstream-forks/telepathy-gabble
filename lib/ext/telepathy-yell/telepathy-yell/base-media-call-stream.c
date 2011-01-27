/*
 * gabble-call-stream.c - Source for TpyBaseMediaCallStream
 * Copyright (C) 2009-2011 Collabora Ltd.
 * @author Sjoerd Simons <sjoerd.simons@collabora.co.uk>
 * @author Jonny Lamb <jonny.lamb@collabora.co.uk>
 * @author David Laban <david.laban@collabora.co.uk>
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

#include "base-media-call-stream.h"

#include <stdio.h>
#include <stdlib.h>

#include <telepathy-glib/dbus.h>
#include <telepathy-glib/svc-properties-interface.h>
#include <telepathy-glib/base-connection.h>
#include <telepathy-glib/gtypes.h>
#include <telepathy-glib/util.h>

#include <telepathy-yell/enums.h>
#include <telepathy-yell/gtypes.h>
#include <telepathy-yell/interfaces.h>
#include <telepathy-yell/svc-call.h>
#include <telepathy-yell/call-stream-endpoint.h>

#define DEBUG_FLAG TPY_DEBUG_CALL
#include "debug.h"

static void call_stream_media_iface_init (gpointer, gpointer);

G_DEFINE_TYPE_WITH_CODE(TpyBaseMediaCallStream, tpy_base_media_call_stream,
   TPY_TYPE_BASE_CALL_STREAM,
   G_IMPLEMENT_INTERFACE (TPY_TYPE_SVC_CALL_STREAM_INTERFACE_MEDIA,
    call_stream_media_iface_init);
  );

/* properties */
enum
{
  PROP_LOCAL_CANDIDATES = 1,
  PROP_ENDPOINTS,
  PROP_TRANSPORT,
  PROP_STUN_SERVERS,
  PROP_RELAY_INFO,
  PROP_HAS_SERVER_INFO,
};

/* private structure */
struct _TpyBaseMediaCallStreamPrivate
{
  gboolean dispose_has_run;

  GList *endpoints;
  GPtrArray *local_candidates;
  GPtrArray *relay_info;
  GPtrArray *stun_servers;
  TpyStreamTransportType transport;

  gboolean got_relay_info;
};

static void
tpy_base_media_call_stream_init (TpyBaseMediaCallStream *self)
{
  TpyBaseMediaCallStreamPrivate *priv = G_TYPE_INSTANCE_GET_PRIVATE (self,
      TPY_TYPE_BASE_MEDIA_CALL_STREAM, TpyBaseMediaCallStreamPrivate);

  self->priv = priv;

  priv->local_candidates = g_ptr_array_new();
  priv->relay_info = g_ptr_array_new ();
  priv->stun_servers = g_ptr_array_new ();
}

static void tpy_base_media_call_stream_dispose (GObject *object);
static void tpy_base_media_call_stream_finalize (GObject *object);

static gboolean
has_server_info (TpyBaseMediaCallStream *self)
{
  /* extend this function when HasServerInfo gains more info to
   * retrieve than just relay info */
  return self->priv->got_relay_info;
}

static void
tpy_base_media_call_stream_get_property (GObject    *object,
    guint       property_id,
    GValue     *value,
    GParamSpec *pspec)
{
  TpyBaseMediaCallStream *stream = TPY_BASE_MEDIA_CALL_STREAM (object);
  TpyBaseMediaCallStreamPrivate *priv = stream->priv;

  switch (property_id)
    {
      case PROP_LOCAL_CANDIDATES:
        {
          g_value_set_boxed (value, stream->priv->local_candidates);
          break;
        }
      case PROP_ENDPOINTS:
        {
          GPtrArray *arr = g_ptr_array_sized_new (1);
          GList *l;

          for (l = priv->endpoints; l != NULL; l = g_list_next (l))
            {
              TpyCallStreamEndpoint *e =
                TPY_CALL_STREAM_ENDPOINT (l->data);
              g_ptr_array_add (arr,
                g_strdup (tpy_call_stream_endpoint_get_object_path (e)));
            }

          g_value_take_boxed (value, arr);
          break;
        }
      case PROP_TRANSPORT:
        {
          g_value_set_uint (value, priv->transport);

          break;
        }
      case PROP_STUN_SERVERS:
        {
          g_value_set_boxed (value, priv->stun_servers);
          break;
        }
      case PROP_RELAY_INFO:
        {
          g_value_set_boxed (value, priv->relay_info);
          break;
        }
      case PROP_HAS_SERVER_INFO:
        {
          g_value_set_boolean (value, has_server_info (stream));
          break;
        }
      default:
        G_OBJECT_WARN_INVALID_PROPERTY_ID (object, property_id, pspec);
        break;
    }
}

static void
tpy_base_media_call_stream_set_property (GObject *object,
    guint property_id,
    const GValue *value,
    GParamSpec *pspec)
{
  TpyBaseMediaCallStream *stream = TPY_BASE_MEDIA_CALL_STREAM (object);
  TpyBaseMediaCallStreamPrivate *priv = stream->priv;

  switch (property_id)
    {
      case PROP_TRANSPORT:
        priv->transport = g_value_get_uint (value);
        break;
      default:
        G_OBJECT_WARN_INVALID_PROPERTY_ID (object, property_id, pspec);
        break;
    }
}

static void
maybe_emit_server_info_retrieved (TpyBaseMediaCallStream *self)
{
  if (has_server_info (self))
    tpy_svc_call_stream_interface_media_emit_server_info_retrieved (self);
}

void
tpy_base_media_call_stream_set_relay_info (
    TpyBaseMediaCallStream *self,
    const GPtrArray *relays)
{
  TpyBaseMediaCallStreamPrivate *priv = self->priv;

  g_return_if_fail (relays != NULL);

  g_boxed_free (TP_ARRAY_TYPE_STRING_VARIANT_MAP_LIST, priv->relay_info);
  priv->relay_info =
      g_boxed_copy (TP_ARRAY_TYPE_STRING_VARIANT_MAP_LIST, relays);

  if (!priv->got_relay_info)
    {
      priv->got_relay_info = TRUE;
      maybe_emit_server_info_retrieved (self);
    }

  tpy_svc_call_stream_interface_media_emit_relay_info_changed (
      self, priv->relay_info);
}

void tpy_base_media_call_stream_set_transport (
    TpyBaseMediaCallStream *self,
    TpyStreamTransportType transport)
{
  self->priv->transport = transport;
}

void
tpy_base_media_call_stream_take_endpoint (
    TpyBaseMediaCallStream *self,
    TpyCallStreamEndpoint *endpoint)
{
  self->priv->endpoints = g_list_append (self->priv->endpoints, endpoint);
}

void
tpy_base_media_call_stream_set_stun_servers (TpyBaseMediaCallStream *self,
    const GPtrArray *stun_servers)
{
  TpyBaseMediaCallStreamPrivate *priv = self->priv;

  g_return_if_fail (stun_servers != NULL);

  g_boxed_free (TP_ARRAY_TYPE_SOCKET_ADDRESS_IP_LIST, priv->stun_servers);
  priv->stun_servers =
      g_boxed_copy (TP_ARRAY_TYPE_SOCKET_ADDRESS_IP_LIST, stun_servers);

  tpy_svc_call_stream_interface_media_emit_stun_servers_changed (
      self, stun_servers);
}

static void
tpy_base_media_call_stream_constructed (GObject *obj)
{
  TpyBaseMediaCallStreamClass *klass =
      TPY_BASE_MEDIA_CALL_STREAM_GET_CLASS (obj);
  GObjectClass *g_klass = G_OBJECT_CLASS (
    tpy_base_media_call_stream_parent_class);

  if (g_klass->constructed != NULL)
      g_klass->constructed (obj);

  g_return_if_fail (klass->add_local_candidates != NULL);
}

static void
tpy_base_media_call_stream_class_init (
    TpyBaseMediaCallStreamClass *tpy_base_media_call_stream_class)
{
  GObjectClass *object_class =
    G_OBJECT_CLASS (tpy_base_media_call_stream_class);
  GParamSpec *param_spec;
  TpyBaseCallStreamClass *bcs_class =
      TPY_BASE_CALL_STREAM_CLASS (tpy_base_media_call_stream_class);

  static TpDBusPropertiesMixinPropImpl stream_media_props[] = {
    { "Transport", "transport", NULL },
    { "LocalCandidates", "local-candidates", NULL },
    { "STUNServers", "stun-servers", NULL },
    { "RelayInfo", "relay-info", NULL },
    { "HasServerInfo", "has-server-info", NULL },
    { "Endpoints", "endpoints", NULL },
    { NULL }
  };

  static const gchar *interfaces[] = {
      TPY_IFACE_CALL_STREAM_INTERFACE_MEDIA,
      NULL
  };

  g_type_class_add_private (tpy_base_media_call_stream_class,
    sizeof (TpyBaseMediaCallStreamPrivate));

  object_class->set_property = tpy_base_media_call_stream_set_property;
  object_class->get_property = tpy_base_media_call_stream_get_property;

  object_class->dispose = tpy_base_media_call_stream_dispose;
  object_class->finalize = tpy_base_media_call_stream_finalize;
  object_class->constructed = tpy_base_media_call_stream_constructed;

  param_spec = g_param_spec_boxed ("local-candidates", "LocalCandidates",
      "List of local candidates",
      TPY_ARRAY_TYPE_CANDIDATE_LIST,
      G_PARAM_READABLE | G_PARAM_STATIC_STRINGS);
  g_object_class_install_property (object_class, PROP_LOCAL_CANDIDATES,
      param_spec);

  param_spec = g_param_spec_boxed ("endpoints", "Endpoints",
      "The endpoints of this content",
      TP_ARRAY_TYPE_OBJECT_PATH_LIST,
      G_PARAM_READABLE | G_PARAM_STATIC_STRINGS);
  g_object_class_install_property (object_class, PROP_ENDPOINTS,
      param_spec);

  param_spec = g_param_spec_uint ("transport", "Transport",
      "The transport type of this stream",
      0, G_MAXUINT, 0,
      G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS);
  g_object_class_install_property (object_class, PROP_TRANSPORT,
      param_spec);

  param_spec = g_param_spec_boxed ("stun-servers", "STUNServers",
      "List of STUN servers",
      TP_ARRAY_TYPE_SOCKET_ADDRESS_IP_LIST,
      G_PARAM_READABLE | G_PARAM_STATIC_STRINGS);
  g_object_class_install_property (object_class, PROP_STUN_SERVERS,
      param_spec);

  param_spec = g_param_spec_boxed ("relay-info", "RelayInfo",
      "List of relay information",
      TP_ARRAY_TYPE_STRING_VARIANT_MAP_LIST,
      G_PARAM_READABLE | G_PARAM_STATIC_STRINGS);
  g_object_class_install_property (object_class, PROP_RELAY_INFO,
      param_spec);

  param_spec = g_param_spec_boolean ("has-server-info",
      "HasServerInfo",
      "True if the server information about STUN and "
      "relay servers has been retrieved",
      FALSE,
      G_PARAM_READABLE | G_PARAM_STATIC_STRINGS);
  g_object_class_install_property (object_class, PROP_HAS_SERVER_INFO,
      param_spec);

  tp_dbus_properties_mixin_implement_interface (object_class,
      TPY_IFACE_QUARK_CALL_STREAM_INTERFACE_MEDIA,
      tp_dbus_properties_mixin_getter_gobject_properties,
      NULL,
      stream_media_props);

  bcs_class->extra_interfaces = interfaces;
}

void
tpy_base_media_call_stream_dispose (GObject *object)
{
  TpyBaseMediaCallStream *self = TPY_BASE_MEDIA_CALL_STREAM (object);
  TpyBaseMediaCallStreamPrivate *priv = self->priv;
  GList *l;

  if (priv->dispose_has_run)
    return;

  priv->dispose_has_run = TRUE;

  for (l = priv->endpoints; l != NULL; l = g_list_next (l))
    {
      g_object_unref (l->data);
    }

  tp_clear_pointer (&priv->endpoints, g_list_free);

  if (G_OBJECT_CLASS (tpy_base_media_call_stream_parent_class)->dispose)
    G_OBJECT_CLASS (tpy_base_media_call_stream_parent_class)->dispose (object);
}

void
tpy_base_media_call_stream_finalize (GObject *object)
{
  TpyBaseMediaCallStream *self = TPY_BASE_MEDIA_CALL_STREAM (object);
  TpyBaseMediaCallStreamPrivate *priv = self->priv;

  g_boxed_free (TPY_ARRAY_TYPE_CANDIDATE_LIST, priv->local_candidates);
  g_boxed_free (TPY_ARRAY_TYPE_CANDIDATE_LIST, priv->relay_info);
  g_boxed_free (TP_ARRAY_TYPE_SOCKET_ADDRESS_IP_LIST, priv->stun_servers);

  G_OBJECT_CLASS (tpy_base_media_call_stream_parent_class)->finalize (object);
}

static void
tpy_base_media_call_stream_add_candidates (
    TpySvcCallStreamInterfaceMedia *iface,
    const GPtrArray *candidates,
    DBusGMethodInvocation *context)
{
  TpyBaseMediaCallStream *self = TPY_BASE_MEDIA_CALL_STREAM (iface);
  TpyBaseMediaCallStreamClass *klass =
      TPY_BASE_MEDIA_CALL_STREAM_GET_CLASS (self);
  GPtrArray *accepted_candidates = NULL;
  guint i;
  GError *error = NULL;

  if (klass->add_local_candidates != NULL)
    accepted_candidates = klass->add_local_candidates (self, candidates,
        &error);
  else
    g_set_error_literal (&error, TP_ERRORS, TP_ERROR_CONFUSED,
        "CM failed to implement the compulsory function add_local_candidates");

  if (error != NULL)
    goto except;

  for (i = 0; i < accepted_candidates->len; i++)
      g_ptr_array_add (self->priv->local_candidates,
          g_ptr_array_index (accepted_candidates, i));

  tpy_svc_call_stream_interface_media_emit_local_candidates_added (self,
      accepted_candidates);

  tpy_svc_call_stream_interface_media_return_from_add_candidates (context);

  goto finally;

except:
  dbus_g_method_return_error (context, error);
  g_clear_error (&error);
finally:
  /* Note that we only do a shallow free because we've copied the contents into
   * local_candidates. */
  if (accepted_candidates != NULL)
    g_ptr_array_free (accepted_candidates, TRUE);
}

static void
tpy_base_media_call_stream_candidates_prepared (
    TpySvcCallStreamInterfaceMedia *iface,
    DBusGMethodInvocation *context)
{
  TpyBaseMediaCallStream *self = TPY_BASE_MEDIA_CALL_STREAM (iface);
  TpyBaseMediaCallStreamClass *klass =
      TPY_BASE_MEDIA_CALL_STREAM_GET_CLASS (self);

  if (klass->local_candidates_prepared != NULL)
    klass->local_candidates_prepared (self);

  tpy_svc_call_stream_interface_media_return_from_candidates_prepared (
    context);
}

static void
call_stream_media_iface_init (gpointer g_iface, gpointer iface_data)
{
  TpySvcCallStreamInterfaceMediaClass *klass =
    (TpySvcCallStreamInterfaceMediaClass *) g_iface;

#define IMPLEMENT(x) tpy_svc_call_stream_interface_media_implement_##x (\
    klass, tpy_base_media_call_stream_##x)
  IMPLEMENT(add_candidates);
  IMPLEMENT(candidates_prepared);
#undef IMPLEMENT
}
