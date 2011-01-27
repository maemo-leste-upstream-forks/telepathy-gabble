/*
 * call-stream-endpoint.c - Source for TpyCallStreamEndpoint
 * Copyright (C) 2009 Collabora Ltd.
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

#include "call-stream-endpoint.h"

#include <stdio.h>
#include <stdlib.h>


#include <telepathy-glib/dbus.h>
#include <telepathy-glib/dbus-properties-mixin.h>
#include <telepathy-glib/enums.h>
#include <telepathy-glib/errors.h>
#include <telepathy-glib/svc-properties-interface.h>
#include <telepathy-glib/util.h>

#include <telepathy-yell/enums.h>
#include <telepathy-yell/interfaces.h>
#include <telepathy-yell/gtypes.h>
#include <telepathy-yell/svc-call.h>


#define DEBUG_FLAG TPY_DEBUG_CALL
#include "debug.h"

static void call_stream_endpoint_iface_init (gpointer, gpointer);

G_DEFINE_TYPE_WITH_CODE(TpyCallStreamEndpoint,
  tpy_call_stream_endpoint,
  G_TYPE_OBJECT,
  G_IMPLEMENT_INTERFACE (TPY_TYPE_SVC_CALL_STREAM_ENDPOINT,
      call_stream_endpoint_iface_init);
   G_IMPLEMENT_INTERFACE (TP_TYPE_SVC_DBUS_PROPERTIES,
      tp_dbus_properties_mixin_iface_init);
);

/* properties */
enum
{
  PROP_OBJECT_PATH = 1,
  PROP_DBUS_DAEMON,
  PROP_REMOTE_CANDIDATES,
  PROP_REMOTE_CREDENTIALS,
  PROP_SELECTED_CANDIDATE,
  PROP_STREAM_STATE,
  PROP_TRANSPORT,
};

struct _TpyCallStreamEndpointPrivate
{
  gboolean dispose_has_run;

  TpDBusDaemon *dbus_daemon;
  gchar *object_path;
  GValueArray *remote_credentials;
  GPtrArray *remote_candidates;
  GValueArray *selected_candidate;
  TpMediaStreamState stream_state;
  TpyStreamTransportType transport;
};

static void
tpy_call_stream_endpoint_init (TpyCallStreamEndpoint *self)
{
  TpyCallStreamEndpointPrivate *priv = G_TYPE_INSTANCE_GET_PRIVATE (self,
      TPY_TYPE_CALL_STREAM_ENDPOINT,
      TpyCallStreamEndpointPrivate);

  self->priv = priv;

  priv->selected_candidate = tp_value_array_build (4,
      G_TYPE_UINT, 0,
      G_TYPE_STRING, "",
      G_TYPE_UINT, 0,
      TPY_HASH_TYPE_CANDIDATE_INFO,
          g_hash_table_new (g_str_hash, g_str_equal),
      G_TYPE_INVALID);

  priv->remote_credentials = tp_value_array_build (2,
      G_TYPE_STRING, "",
      G_TYPE_STRING, "",
      G_TYPE_INVALID);

  priv->remote_candidates = g_ptr_array_new ();
}

static void tpy_call_stream_endpoint_dispose (GObject *object);
static void tpy_call_stream_endpoint_finalize (GObject *object);

static void
tpy_call_stream_endpoint_get_property (GObject    *object,
    guint       property_id,
    GValue     *value,
    GParamSpec *pspec)
{
  TpyCallStreamEndpoint *endpoint = TPY_CALL_STREAM_ENDPOINT (object);
  TpyCallStreamEndpointPrivate *priv = endpoint->priv;

  switch (property_id)
    {
      case PROP_OBJECT_PATH:
        g_value_set_string (value, priv->object_path);
        break;
      case PROP_REMOTE_CANDIDATES:
        g_value_set_boxed (value, priv->remote_candidates);
        break;
      case PROP_REMOTE_CREDENTIALS:
        g_value_set_boxed (value, priv->remote_credentials);
        break;
      case PROP_SELECTED_CANDIDATE:
        g_value_set_boxed (value, priv->selected_candidate);
        break;
      case PROP_STREAM_STATE:
        g_value_set_uint (value, priv->stream_state);
        break;
      case PROP_TRANSPORT:
        g_value_set_uint (value, priv->transport);
        break;
      default:
        G_OBJECT_WARN_INVALID_PROPERTY_ID (object, property_id, pspec);
        break;
    }
}

static void
tpy_call_stream_endpoint_set_property (GObject *object,
    guint property_id,
    const GValue *value,
    GParamSpec *pspec)
{
  TpyCallStreamEndpoint *endpoint = TPY_CALL_STREAM_ENDPOINT (object);
  TpyCallStreamEndpointPrivate *priv = endpoint->priv;

  switch (property_id)
    {
      case PROP_OBJECT_PATH:
        priv->object_path = g_value_dup_string (value);
        g_assert (priv->object_path != NULL);
        break;
      case PROP_DBUS_DAEMON:
        g_assert (priv->dbus_daemon == NULL);   /* construct-only */
        priv->dbus_daemon = g_value_dup_object (value);
        break;
      case PROP_TRANSPORT:
        priv->transport = g_value_get_uint (value);
        break;
      case PROP_STREAM_STATE:
        priv->stream_state = g_value_get_uint (value);
        break;
      case PROP_SELECTED_CANDIDATE:
        {
          g_boxed_free (TPY_STRUCT_TYPE_CANDIDATE,
              priv->selected_candidate);
          priv->selected_candidate = g_value_get_boxed (value);
          break;
        }
      default:
        G_OBJECT_WARN_INVALID_PROPERTY_ID (object, property_id, pspec);
        break;
    }
}

static void
tpy_call_stream_endpoint_constructed (GObject *obj)
{
  TpyCallStreamEndpointPrivate *priv;

  priv = TPY_CALL_STREAM_ENDPOINT (obj)->priv;

  /* register object on the bus */
  DEBUG ("Registering %s", priv->object_path);
  tp_dbus_daemon_register_object (priv->dbus_daemon, priv->object_path, obj);

  if (G_OBJECT_CLASS (tpy_call_stream_endpoint_parent_class)->constructed
      != NULL)
    G_OBJECT_CLASS (tpy_call_stream_endpoint_parent_class)->constructed (
      obj);
}

static void
tpy_call_stream_endpoint_class_init (
  TpyCallStreamEndpointClass *tpy_call_stream_endpoint_class)
{
  GObjectClass *object_class =
      G_OBJECT_CLASS (tpy_call_stream_endpoint_class);
  GParamSpec *param_spec;
  static TpDBusPropertiesMixinPropImpl endpoint_props[] = {
    { "RemoteCandidates", "remote-candidates", NULL },
    { "RemoteCredentials", "remote-credentials", NULL },
    { "SelectedCandidate", "selected-candidate", NULL },
    { "StreamState", "stream-state", NULL },
    { "Transport", "transport", NULL },
    { NULL }
  };
  static TpDBusPropertiesMixinIfaceImpl prop_interfaces[] = {
      { TPY_IFACE_CALL_STREAM_ENDPOINT,
        tp_dbus_properties_mixin_getter_gobject_properties,
        NULL,
        endpoint_props,
      },
      { NULL }
  };

  g_type_class_add_private (tpy_call_stream_endpoint_class,
      sizeof (TpyCallStreamEndpointPrivate));

  object_class->dispose = tpy_call_stream_endpoint_dispose;
  object_class->finalize = tpy_call_stream_endpoint_finalize;
  object_class->constructed = tpy_call_stream_endpoint_constructed;

  object_class->set_property = tpy_call_stream_endpoint_set_property;
  object_class->get_property = tpy_call_stream_endpoint_get_property;

  param_spec = g_param_spec_string ("object-path", "D-Bus object path",
      "The D-Bus object path used for this "
      "object on the bus.",
      NULL,
      G_PARAM_CONSTRUCT_ONLY | G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS);
  g_object_class_install_property (object_class, PROP_OBJECT_PATH, param_spec);

  param_spec = g_param_spec_boxed ("remote-candidates",
      "RemoteCandidates",
      "The remote candidates of this endpoint",
      TPY_ARRAY_TYPE_CANDIDATE_LIST,
      G_PARAM_READABLE | G_PARAM_STATIC_STRINGS);
  g_object_class_install_property (object_class, PROP_REMOTE_CANDIDATES,
      param_spec);

  param_spec = g_param_spec_boxed ("remote-credentials",
      "RemoteCredentials",
      "The remote credentials of this endpoint",
      TPY_STRUCT_TYPE_STREAM_CREDENTIALS,
      G_PARAM_READABLE | G_PARAM_STATIC_STRINGS);
  g_object_class_install_property (object_class, PROP_REMOTE_CREDENTIALS,
      param_spec);

  param_spec = g_param_spec_boxed ("selected-candidate",
      "SelectedCandidate",
      "The candidate selected for this endpoint",
      TPY_STRUCT_TYPE_CANDIDATE,
      G_PARAM_READABLE | G_PARAM_STATIC_STRINGS);
  g_object_class_install_property (object_class, PROP_SELECTED_CANDIDATE,
      param_spec);

  param_spec = g_param_spec_uint ("stream-state", "StreamState",
      "The stream state of this endpoint.",
      0, G_MAXUINT32,
      TP_MEDIA_STREAM_STATE_DISCONNECTED,
      G_PARAM_READABLE | G_PARAM_STATIC_STRINGS);
  g_object_class_install_property (object_class, PROP_STREAM_STATE,
      param_spec);

  param_spec = g_param_spec_uint ("transport",
      "Transport",
      "The transport type for the content of this endpoint.",
      0, NUM_TPY_STREAM_TRANSPORT_TYPES, 0,
      G_PARAM_READWRITE | G_PARAM_CONSTRUCT_ONLY | G_PARAM_STATIC_STRINGS);
  g_object_class_install_property (object_class, PROP_TRANSPORT, param_spec);

  param_spec = g_param_spec_object ("dbus-daemon",
      "The DBus daemon connection",
      "The connection to the DBus daemon owning the CM",
      TP_TYPE_DBUS_DAEMON,
      G_PARAM_WRITABLE | G_PARAM_CONSTRUCT_ONLY | G_PARAM_STATIC_STRINGS);
  g_object_class_install_property (object_class, PROP_DBUS_DAEMON, param_spec);

  tpy_call_stream_endpoint_class->dbus_props_class.interfaces =
      prop_interfaces;
  tp_dbus_properties_mixin_class_init (object_class,
      G_STRUCT_OFFSET (TpyCallStreamEndpointClass, dbus_props_class));
}

void
tpy_call_stream_endpoint_dispose (GObject *object)
{
  TpyCallStreamEndpoint *self = TPY_CALL_STREAM_ENDPOINT (object);
  TpyCallStreamEndpointPrivate *priv = self->priv;

  if (priv->dispose_has_run)
    return;

  priv->dispose_has_run = TRUE;

  tp_clear_object (&priv->dbus_daemon);

  if (G_OBJECT_CLASS (tpy_call_stream_endpoint_parent_class)->dispose)
    G_OBJECT_CLASS (tpy_call_stream_endpoint_parent_class)->dispose (
        object);
}

void
tpy_call_stream_endpoint_finalize (GObject *object)
{
  TpyCallStreamEndpoint *self = TPY_CALL_STREAM_ENDPOINT (object);
  TpyCallStreamEndpointPrivate *priv = self->priv;

  /* free any data held directly by the object here */
  g_free (priv->object_path);

  g_boxed_free (TPY_STRUCT_TYPE_CANDIDATE, priv->selected_candidate);
  g_boxed_free (TPY_STRUCT_TYPE_STREAM_CREDENTIALS,
      priv->remote_credentials);
  g_boxed_free (TPY_ARRAY_TYPE_CANDIDATE_LIST, priv->remote_candidates);

  G_OBJECT_CLASS (tpy_call_stream_endpoint_parent_class)->finalize (object);
}

static void
call_stream_endpoint_set_stream_state (TpySvcCallStreamEndpoint *iface,
    TpMediaStreamState state,
    DBusGMethodInvocation *context)
{
  TpyCallStreamEndpoint *self = TPY_CALL_STREAM_ENDPOINT (iface);

  if (state >= NUM_TP_MEDIA_STREAM_STATES)
    {
      GError *error = g_error_new (TP_ERRORS, TP_ERROR_INVALID_ARGUMENT,
          "Stream state %d is out of the valid range.", state);
      dbus_g_method_return_error (context, error);
      g_error_free (error);
      return;
    }

  self->priv->stream_state = state;
  g_object_notify (G_OBJECT (self), "stream-state");

  tpy_svc_call_stream_endpoint_emit_stream_state_changed (self, state);
  tpy_svc_call_stream_endpoint_return_from_set_stream_state (context);
}

static void
call_stream_endpoint_set_selected_candidate (
    TpySvcCallStreamEndpoint *iface,
    const GValueArray *candidate,
    DBusGMethodInvocation *context)
{
  TpyCallStreamEndpoint *self = TPY_CALL_STREAM_ENDPOINT (iface);
  GValueArray *va = (GValueArray *) candidate;
  GValue *value;
  GError *error = NULL;

  if (candidate->n_values != 4)
    {
      error = g_error_new (TP_ERRORS, TP_ERROR_INVALID_ARGUMENT,
          "A candidate should have 4 values, got %d", candidate->n_values);
      goto error;
    }

  value = g_value_array_get_nth (va, 0);
  if (g_value_get_uint (value) > 2)
    {
      error = g_error_new (TP_ERRORS, TP_ERROR_INVALID_ARGUMENT,
          "Invalid component id: %d", g_value_get_uint (value));
      goto error;
    }

  value = g_value_array_get_nth (va, 1);
  if (g_value_get_string (value) == NULL ||
      g_value_get_string (value)[0] == 0)
    {
      error = g_error_new (TP_ERRORS, TP_ERROR_INVALID_ARGUMENT,
          "Invalid address: %s", g_value_get_string (value));
      goto error;
    }

  value = g_value_array_get_nth (va, 2);
  if (g_value_get_uint (value) > 65535)
    {
      error = g_error_new (TP_ERRORS, TP_ERROR_INVALID_ARGUMENT,
          "Invalid port: %d", g_value_get_uint (value));
      goto error;
    }

  g_boxed_free (TPY_STRUCT_TYPE_CANDIDATE,
      self->priv->selected_candidate);
  self->priv->selected_candidate =
      g_boxed_copy (TPY_STRUCT_TYPE_CANDIDATE, candidate);
  g_object_notify (G_OBJECT (self), "selected-candidate");

  tpy_svc_call_stream_endpoint_emit_candidate_selected (self, candidate);
  tpy_svc_call_stream_endpoint_return_from_set_selected_candidate (context);
  return;

error:
  dbus_g_method_return_error (context, error);
  g_error_free (error);
}

static void
call_stream_endpoint_iface_init (gpointer iface, gpointer data)
{
  TpySvcCallStreamEndpointClass *klass =
    (TpySvcCallStreamEndpointClass *) iface;

#define IMPLEMENT(x) tpy_svc_call_stream_endpoint_implement_##x (\
    klass, call_stream_endpoint_##x)
  IMPLEMENT(set_stream_state);
  IMPLEMENT(set_selected_candidate);
#undef IMPLEMENT
}

TpyCallStreamEndpoint *
tpy_call_stream_endpoint_new (TpDBusDaemon *dbus_daemon,
    const gchar *object_path,
    TpyStreamTransportType type)
{
  return g_object_new (TPY_TYPE_CALL_STREAM_ENDPOINT,
      "dbus-daemon", dbus_daemon,
      "object-path", object_path,
      "transport", type,
    NULL);
}

const gchar *
tpy_call_stream_endpoint_get_object_path (
    TpyCallStreamEndpoint *endpoint)
{
  return endpoint->priv->object_path;
}

void
tpy_call_stream_endpoint_add_new_candidates (
    TpyCallStreamEndpoint *self,
    GPtrArray *candidates)
{
  guint i;
  GValueArray *c;

  if (candidates == NULL)
    return;

  for (i = 0; i < candidates->len; i++)
    {
      c = g_boxed_copy (TPY_STRUCT_TYPE_CANDIDATE,
          g_ptr_array_index (candidates, i));
      g_ptr_array_add (self->priv->remote_candidates, c);
    }

  tpy_svc_call_stream_endpoint_emit_remote_candidates_added (self,
      candidates);
}

void tpy_call_stream_endpoint_add_new_candidate (
    TpyCallStreamEndpoint *self,
    guint component,
    gchar *address,
    guint port,
    GHashTable *info_hash)
{
  GPtrArray *candidates = g_ptr_array_sized_new (1);
  GValueArray *a;

  a = tp_value_array_build (4,
      G_TYPE_UINT, component,
      G_TYPE_STRING, address,
      G_TYPE_UINT, port,
      TPY_HASH_TYPE_CANDIDATE_INFO, info_hash,
      G_TYPE_INVALID);

  g_ptr_array_add (candidates, a);

  tpy_call_stream_endpoint_add_new_candidates (self, candidates);

  g_boxed_free (TPY_ARRAY_TYPE_CANDIDATE_LIST, candidates);
}
