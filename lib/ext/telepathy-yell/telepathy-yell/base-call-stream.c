/*
 * base-call-stream.c - Source for TpyBaseCallStream
 * Copyright © 2009–2010 Collabora Ltd.
 * @author Sjoerd Simons <sjoerd.simons@collabora.co.uk>
 * @author Will Thompson <will.thompson@collabora.co.uk>
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

#include "base-call-stream.h"

#define DEBUG_FLAG TPY_DEBUG_CALL
#include "debug.h"

#include <telepathy-yell/interfaces.h>
#include <telepathy-yell/gtypes.h>
#include <telepathy-yell/svc-call.h>

static void call_stream_iface_init (gpointer g_iface, gpointer iface_data);

G_DEFINE_TYPE_WITH_CODE(TpyBaseCallStream, tpy_base_call_stream,
    G_TYPE_OBJECT,
    G_IMPLEMENT_INTERFACE (TP_TYPE_SVC_DBUS_PROPERTIES,
        tp_dbus_properties_mixin_iface_init);
    G_IMPLEMENT_INTERFACE (TPY_TYPE_SVC_CALL_STREAM, call_stream_iface_init);
    )

enum
{
  PROP_OBJECT_PATH = 1,
  PROP_CONNECTION,

  /* Call interface properties */
  PROP_INTERFACES,
  PROP_REMOTE_MEMBERS,
  PROP_LOCAL_SENDING_STATE,
  PROP_CAN_REQUEST_RECEIVING,
};

struct _TpyBaseCallStreamPrivate
{
  gboolean dispose_has_run;

  gchar *object_path;
  TpBaseConnection *conn;

  GHashTable *remote_members;

  TpySendingState local_sending_state;
};

static void
tpy_base_call_stream_init (TpyBaseCallStream *self)
{
  TpyBaseCallStreamPrivate *priv = G_TYPE_INSTANCE_GET_PRIVATE (self,
      TPY_TYPE_BASE_CALL_STREAM, TpyBaseCallStreamPrivate);

  self->priv = priv;
  priv->remote_members = g_hash_table_new (g_direct_hash, g_direct_equal);
}

static void
tpy_base_call_stream_constructed (GObject *obj)
{
  TpyBaseCallStream *self = TPY_BASE_CALL_STREAM (obj);
  TpyBaseCallStreamPrivate *priv = self->priv;
  TpDBusDaemon *bus = tp_base_connection_get_dbus_daemon (
      (TpBaseConnection *) priv->conn);

  if (G_OBJECT_CLASS (tpy_base_call_stream_parent_class)->constructed
      != NULL)
    G_OBJECT_CLASS (tpy_base_call_stream_parent_class)->constructed (obj);

  priv->local_sending_state = TPY_SENDING_STATE_NONE;

  /* register object on the bus */
  DEBUG ("Registering %s", priv->object_path);
  tp_dbus_daemon_register_object (bus, priv->object_path, obj);
}

static void
tpy_base_call_stream_dispose (GObject *object)
{
  TpyBaseCallStream *self = TPY_BASE_CALL_STREAM (object);
  TpyBaseCallStreamPrivate *priv = self->priv;

  if (priv->dispose_has_run)
    return;

  priv->dispose_has_run = TRUE;

  tp_clear_object (&priv->conn);

  if (G_OBJECT_CLASS (tpy_base_call_stream_parent_class)->dispose != NULL)
    G_OBJECT_CLASS (tpy_base_call_stream_parent_class)->dispose (object);
}

static void
tpy_base_call_stream_finalize (GObject *object)
{
  TpyBaseCallStream *self = TPY_BASE_CALL_STREAM (object);
  TpyBaseCallStreamPrivate *priv = self->priv;

  /* free any data held directly by the object here */
  g_free (priv->object_path);
  g_hash_table_destroy (priv->remote_members);

  if (G_OBJECT_CLASS (tpy_base_call_stream_parent_class)->finalize != NULL)
    G_OBJECT_CLASS (tpy_base_call_stream_parent_class)->finalize (object);
}

static void
tpy_base_call_stream_get_property (
    GObject *object,
    guint property_id,
    GValue *value,
    GParamSpec *pspec)
{
  TpyBaseCallStream *self = TPY_BASE_CALL_STREAM (object);
  TpyBaseCallStreamPrivate *priv = self->priv;

  switch (property_id)
    {
      case PROP_CONNECTION:
        g_value_set_object (value, priv->conn);
        break;
      case PROP_OBJECT_PATH:
        g_value_set_string (value, priv->object_path);
        break;
      case PROP_REMOTE_MEMBERS:
        g_value_set_boxed (value, priv->remote_members);
        break;
      case PROP_LOCAL_SENDING_STATE:
        g_value_set_uint (value, priv->local_sending_state);
        break;
      case PROP_CAN_REQUEST_RECEIVING:
        {
          TpyBaseCallStreamClass *klass =
              TPY_BASE_CALL_STREAM_GET_CLASS (self);

          g_value_set_boolean (value, klass->request_receiving != NULL);
          break;
        }
      case PROP_INTERFACES:
        {
          TpyBaseCallStreamClass *klass =
              TPY_BASE_CALL_STREAM_GET_CLASS (self);

          if (klass->extra_interfaces != NULL)
            {
              g_value_set_boxed (value, klass->extra_interfaces);
            }
          else
            {
              gchar *empty[] = { NULL };

              g_value_set_boxed (value, empty);
            }
          break;
        }
      default:
        G_OBJECT_WARN_INVALID_PROPERTY_ID (object, property_id, pspec);
        break;
    }
}

static void
tpy_base_call_stream_set_property (
    GObject *object,
    guint property_id,
    const GValue *value,
    GParamSpec *pspec)
{
  TpyBaseCallStream *self = TPY_BASE_CALL_STREAM (object);
  TpyBaseCallStreamPrivate *priv = self->priv;

  switch (property_id)
    {
      case PROP_CONNECTION:
        priv->conn = g_value_dup_object (value);
        g_assert (priv->conn != NULL);
        break;
      case PROP_OBJECT_PATH:
        g_free (priv->object_path);
        priv->object_path = g_value_dup_string (value);
        break;
      default:
        G_OBJECT_WARN_INVALID_PROPERTY_ID (object, property_id, pspec);
        break;
    }
}

static void
tpy_base_call_stream_class_init (TpyBaseCallStreamClass *bsc_class)
{
  GObjectClass *object_class = G_OBJECT_CLASS (bsc_class);
  GParamSpec *param_spec;
  static TpDBusPropertiesMixinPropImpl stream_props[] = {
    { "Interfaces", "interfaces", NULL },
    { "RemoteMembers", "remote-members", NULL },
    { "LocalSendingState", "local-sending-state", NULL },
    { "CanRequestReceiving", "can-request-receiving", NULL },
    { NULL }
  };
  static TpDBusPropertiesMixinIfaceImpl prop_interfaces[] = {
      { TPY_IFACE_CALL_STREAM,
        tp_dbus_properties_mixin_getter_gobject_properties,
        NULL,
        stream_props,
      },
      { NULL }
  };

  g_type_class_add_private (bsc_class, sizeof (TpyBaseCallStreamPrivate));

  object_class->constructed = tpy_base_call_stream_constructed;
  object_class->dispose = tpy_base_call_stream_dispose;
  object_class->finalize = tpy_base_call_stream_finalize;
  object_class->set_property = tpy_base_call_stream_set_property;
  object_class->get_property = tpy_base_call_stream_get_property;

  param_spec = g_param_spec_object ("connection", "TpBaseConnection object",
      "Tpy connection object that owns this call stream",
      TP_TYPE_BASE_CONNECTION,
      G_PARAM_CONSTRUCT_ONLY | G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS);
  g_object_class_install_property (object_class, PROP_CONNECTION, param_spec);

  param_spec = g_param_spec_string ("object-path", "D-Bus object path",
      "The D-Bus object path used for this "
      "object on the bus.",
      NULL,
      G_PARAM_CONSTRUCT_ONLY | G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS);
  g_object_class_install_property (object_class, PROP_OBJECT_PATH, param_spec);

  param_spec = g_param_spec_boxed ("interfaces", "Interfaces",
      "Stream interfaces",
      G_TYPE_STRV,
      G_PARAM_READABLE | G_PARAM_STATIC_STRINGS);
  g_object_class_install_property (object_class, PROP_INTERFACES,
      param_spec);

  param_spec = g_param_spec_boxed ("remote-members", "Remote members",
      "Remote member map",
      TPY_HASH_TYPE_CONTACT_SENDING_STATE_MAP,
      G_PARAM_READABLE | G_PARAM_STATIC_STRINGS);
  g_object_class_install_property (object_class, PROP_REMOTE_MEMBERS,
      param_spec);

  param_spec = g_param_spec_uint ("local-sending-state", "LocalSendingState",
      "Local sending state",
      0, G_MAXUINT, 0,
      G_PARAM_READABLE | G_PARAM_STATIC_STRINGS);
  g_object_class_install_property (object_class, PROP_LOCAL_SENDING_STATE,
      param_spec);

  param_spec = g_param_spec_boolean ("can-request-receiving",
      "CanRequestReceiving",
      "If true, the user can request that a remote contact starts sending on"
      "this stream.",
      FALSE,
      G_PARAM_READABLE | G_PARAM_STATIC_STRINGS);
  g_object_class_install_property (object_class, PROP_CAN_REQUEST_RECEIVING,
      param_spec);

  bsc_class->dbus_props_class.interfaces = prop_interfaces;
  tp_dbus_properties_mixin_class_init (object_class,
      G_STRUCT_OFFSET (TpyBaseCallStreamClass, dbus_props_class));
}

TpBaseConnection *
tpy_base_call_stream_get_connection (TpyBaseCallStream *self)
{
  g_return_val_if_fail (TPY_IS_BASE_CALL_STREAM (self), NULL);

  return self->priv->conn;
}

const gchar *
tpy_base_call_stream_get_object_path (TpyBaseCallStream *self)
{
  g_return_val_if_fail (TPY_IS_BASE_CALL_STREAM (self), NULL);

  return self->priv->object_path;
}

gboolean
tpy_base_call_stream_remote_member_update_state (TpyBaseCallStream *self,
    TpHandle contact,
    TpySendingState state)
{
  TpyBaseCallStreamPrivate *priv = self->priv;
  gpointer state_p = 0;
  gboolean exists;

  exists = g_hash_table_lookup_extended (priv->remote_members,
    GUINT_TO_POINTER (contact),
    NULL,
    &state_p);

  if (exists && GPOINTER_TO_UINT (state_p) == state)
    return FALSE;

  DEBUG ("Updating remote member %d state: %d => %d", contact,
    GPOINTER_TO_UINT (state_p), state);

  g_hash_table_insert (priv->remote_members,
    GUINT_TO_POINTER (contact),
    GUINT_TO_POINTER (state));

  return TRUE;
}

TpySendingState
tpy_base_call_stream_get_local_sending_state (
  TpyBaseCallStream *self)
{
  return self->priv->local_sending_state;
}

gboolean
tpy_base_call_stream_update_local_sending_state (TpyBaseCallStream *self,
    TpySendingState state)
{
  TpyBaseCallStreamPrivate *priv = self->priv;

  if (priv->local_sending_state == state)
    return FALSE;

  priv->local_sending_state = state;

  tpy_svc_call_stream_emit_local_sending_state_changed (
    TPY_SVC_CALL_STREAM (self), state);

  return TRUE;
}

gboolean
tpy_base_call_stream_set_sending (TpyBaseCallStream *self,
  gboolean send,
  GError **error)
{
  TpyBaseCallStreamPrivate *priv = self->priv;
  TpyBaseCallStreamClass *klass = TPY_BASE_CALL_STREAM_GET_CLASS (self);

  /* Determine if there is a state change for our sending side */
  switch (priv->local_sending_state)
    {
      case TPY_SENDING_STATE_NONE:
      case TPY_SENDING_STATE_PENDING_SEND:
        if (!send)
          goto out;
        break;
      case TPY_SENDING_STATE_SENDING:
      case TPY_SENDING_STATE_PENDING_STOP_SENDING:
        if (send)
          goto out;
        break;
      default:
        g_assert_not_reached ();
    }

  if (klass->set_sending != NULL)
    {
      if (!klass->set_sending (self, send, error))
        goto failed;
    }
  else
    {
      g_set_error_literal (error, TP_ERRORS, TP_ERROR_NOT_IMPLEMENTED,
        "This CM does not implement SetSending");
      goto failed;
    }

out:
  tpy_base_call_stream_update_local_sending_state (self,
    send ? TPY_SENDING_STATE_SENDING : TPY_SENDING_STATE_NONE);
  return TRUE;

failed:
  return FALSE;
}

static void
tpy_base_call_stream_set_sending_dbus (TpySvcCallStream *iface,
    gboolean sending,
    DBusGMethodInvocation *context)
{
  GError *error = NULL;

  if (tpy_base_call_stream_set_sending (TPY_BASE_CALL_STREAM (iface),
      sending, &error))
    tpy_svc_call_stream_return_from_set_sending (context);
  else
    dbus_g_method_return_error (context, error);

  g_clear_error (&error);
}

static void
tpy_base_call_stream_request_receiving (TpySvcCallStream *iface,
    TpHandle handle,
    gboolean receiving,
    DBusGMethodInvocation *context)
{
  GError *error = NULL;
  TpyBaseCallStream *self = TPY_BASE_CALL_STREAM (iface);
  TpyBaseCallStreamClass *klass = TPY_BASE_CALL_STREAM_GET_CLASS (self);

  if (klass->request_receiving != NULL)
    klass->request_receiving (self, handle, receiving, &error);
  else
    g_set_error_literal (&error, TP_ERRORS, TP_ERROR_NOT_IMPLEMENTED,
        "This CM does not implement request_receiving");

  if (error != NULL)
    dbus_g_method_return_error (context, error);
  else
    tpy_svc_call_stream_return_from_request_receiving (context);

  g_clear_error (&error);
}

static void
call_stream_iface_init (gpointer g_iface, gpointer iface_data)
{
  TpySvcCallStreamClass *klass =
    (TpySvcCallStreamClass *) g_iface;

#define IMPLEMENT(x, suffix) tpy_svc_call_stream_implement_##x (\
    klass, tpy_base_call_stream_##x##suffix)
  IMPLEMENT(set_sending, _dbus);
  IMPLEMENT(request_receiving,);
#undef IMPLEMENT
}
