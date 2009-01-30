/*
 * tube-dbus.c - Source for GabbleTubeDBus
 * Copyright (C) 2007-2008 Collabora Ltd.
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
#include "tube-dbus.h"

#include <string.h>
#include <errno.h>

#include <glib/gstdio.h>
#include <dbus/dbus-glib.h>
#include <dbus/dbus-glib-lowlevel.h>
#include <loudmouth/loudmouth.h>

#define DEBUG_FLAG GABBLE_DEBUG_TUBES

#include "base64.h"
#include "bytestream-factory.h"
#include "bytestream-ibb.h"
#include "bytestream-iface.h"
#include "connection.h"
#include "debug.h"
#include "disco.h"
#include "gabble-signals-marshal.h"
#include "namespaces.h"
#include "tube-iface.h"
#include "util.h"

/* When we receive D-Bus messages to be delivered to the application and the
 * application is not yet connected to the D-Bus tube, theses D-Bus messages
 * are queued and delivered when the application connects to the D-Bus tube.
 *
 * If the application never connects, there is a risk that the contact sends
 * too many messages and eat all the memory. To avoid this, there is an
 * arbitrary limit on the queue size set to 4MB. */
#define MAX_QUEUE_SIZE (4096*1024)

static void
tube_iface_init (gpointer g_iface, gpointer iface_data);

G_DEFINE_TYPE_WITH_CODE (GabbleTubeDBus, gabble_tube_dbus, G_TYPE_OBJECT,
    G_IMPLEMENT_INTERFACE (GABBLE_TYPE_TUBE_IFACE, tube_iface_init));

/* signals */
enum
{
  OPENED,
  CLOSED,
  OFFERED,
  LAST_SIGNAL
};

static guint signals[LAST_SIGNAL] = {0};

/* properties */
enum
{
  PROP_CONNECTION = 1,
  PROP_HANDLE,
  PROP_HANDLE_TYPE,
  PROP_SELF_HANDLE,
  PROP_ID,
  PROP_BYTESTREAM,
  PROP_STREAM_ID,
  PROP_TYPE,
  PROP_INITIATOR_HANDLE,
  PROP_SERVICE,
  PROP_PARAMETERS,
  PROP_STATE,
  PROP_DBUS_ADDRESS,
  PROP_DBUS_NAME,
  PROP_DBUS_NAMES,
  LAST_PROPERTY
};

struct _GabbleTubeDBusPrivate
{
  GabbleConnection *conn;
  TpHandle handle;
  TpHandleType handle_type;
  TpHandle self_handle;
  guint id;
  GabbleBytestreamIface *bytestream;
  gchar *stream_id;
  TpHandle initiator;
  gchar *service;
  GHashTable *parameters;

  /* our unique D-Bus name on the virtual tube bus (NULL for 1-1 D-Bus tubes)*/
  gchar *dbus_local_name;
  /* the address that we are listening for D-Bus connections on */
  gchar *dbus_srv_addr;
  /* the path of the UNIX socket used by the D-Bus server */
  gchar *socket_path;
  /* the server that's listening on dbus_srv_addr */
  DBusServer *dbus_srv;
  /* the connection to dbus_srv from a local client, or NULL */
  DBusConnection *dbus_conn;
  /* the queue of D-Bus messages to be delivered to a local client when it
   * will connect */
  GSList *dbus_msg_queue;
  /* current size of the queue in bytes. The maximum is MAX_QUEUE_SIZE */
  unsigned long dbus_msg_queue_size;
  /* mapping of contact handle -> D-Bus name (NULL for 1-1 D-Bus tubes) */
  GHashTable *dbus_names;
  /* mapping of D-Bus name -> contact handle */
  GHashTable *dbus_name_to_handle;

  /* Message reassembly buffer (CONTACT tubes only) */
  GString *reassembly_buffer;
  /* Number of bytes that will be in the next message, 0 if unknown */
  guint32 reassembly_bytes_needed;

  gboolean dispose_has_run;
};

#define GABBLE_TUBE_DBUS_GET_PRIVATE(obj) ((obj)->priv)

static void data_received_cb (GabbleBytestreamIface *stream, TpHandle sender,
    GString *data, gpointer user_data);

static void gabble_tube_dbus_close (GabbleTubeIface *tube, gboolean
    closed_remotely);

/*
 * Characters used are permissible both in filenames and in D-Bus names. (See
 * D-Bus specification for restrictions.)
 */
static void
generate_ascii_string (guint len,
                       gchar *buf)
{
  const gchar *chars =
    "0123456789"
    "abcdefghijklmnopqrstuvwxyz"
    "ABCDEFGHIJKLMNOPQRSTUVWXYZ"
    "_-";
  guint i;

  for (i = 0; i < len; i++)
    buf[i] = chars[g_random_int_range (0, 64)];
}

static DBusHandlerResult
filter_cb (DBusConnection *conn,
           DBusMessage *msg,
           void *user_data)
{
  GabbleTubeDBus *tube = GABBLE_TUBE_DBUS (user_data);
  GabbleTubeDBusPrivate *priv = GABBLE_TUBE_DBUS_GET_PRIVATE (tube);
  gchar *marshalled = NULL;
  gint len;

  if (dbus_message_get_type (msg) == DBUS_MESSAGE_TYPE_SIGNAL &&
      !tp_strdiff (dbus_message_get_interface (msg),
        "org.freedesktop.DBus.Local") &&
      !tp_strdiff (dbus_message_get_member (msg), "Disconnected"))
    {
      /* connection was disconnected */
      DEBUG ("connection was disconnected");
      dbus_connection_close (priv->dbus_conn);
      dbus_connection_unref (priv->dbus_conn);
      priv->dbus_conn = NULL;
      goto out;
    }

  if (priv->dbus_local_name != NULL)
    {
      dbus_message_set_sender (msg, priv->dbus_local_name);
    }

  if (!dbus_message_marshal (msg, &marshalled, &len))
    goto out;

  if (GABBLE_IS_BYTESTREAM_MUC (priv->bytestream))
    {
      /* This bytestream support direct send */
      const gchar *dest;

      dest = dbus_message_get_destination (msg);

      if (dest != NULL)
        {
          TpHandle handle;

          handle = GPOINTER_TO_UINT (g_hash_table_lookup (
                priv->dbus_name_to_handle, dest));

          if (handle == 0)
            {
              DEBUG ("Unknown D-Bus name: %s", dest);
              goto out;
            }

          gabble_bytestream_muc_send_to (
              GABBLE_BYTESTREAM_MUC (priv->bytestream), handle, len,
              marshalled);

          goto out;
        }
    }

  gabble_bytestream_iface_send (priv->bytestream, len, marshalled);

out:
  if (marshalled != NULL)
    g_free (marshalled);

  return DBUS_HANDLER_RESULT_HANDLED;
}

static void
new_connection_cb (DBusServer *server,
                   DBusConnection *conn,
                   void *data)
{
  GabbleTubeDBus *tube = GABBLE_TUBE_DBUS (data);
  GabbleTubeDBusPrivate *priv = GABBLE_TUBE_DBUS_GET_PRIVATE (tube);
  guint32 serial;
  GSList *i;

  if (priv->dbus_conn != NULL)
    /* we already have a connection; drop this new one */
    /* return without reffing conn means it will be dropped */
    return;

  DEBUG ("got connection");

  dbus_connection_ref (conn);
  dbus_connection_setup_with_g_main (conn, NULL);
  dbus_connection_add_filter (conn, filter_cb, tube, NULL);
  priv->dbus_conn = conn;

  /* We may have received messages to deliver before the local connection is
   * established. Theses messages are kept in the dbus_msg_queue list and are
   * delivered as soon as we get the connection. */
  DEBUG ("%u messages in the queue (%lu bytes)",
         g_slist_length (priv->dbus_msg_queue), priv->dbus_msg_queue_size);
  priv->dbus_msg_queue = g_slist_reverse (priv->dbus_msg_queue);
  for (i = priv->dbus_msg_queue; i != NULL; i = g_slist_delete_link (i, i))
    {
      DBusMessage *msg = i->data;
      DEBUG ("delivering queued message from '%s' to '%s' on the "
             "new connection",
             dbus_message_get_sender (msg),
             dbus_message_get_destination (msg));
      dbus_connection_send (priv->dbus_conn, msg, &serial);
      dbus_message_unref (msg);
    }
  priv->dbus_msg_queue = NULL;
  priv->dbus_msg_queue_size = 0;
}

static void
do_close (GabbleTubeDBus *self)
{
  GabbleTubeDBusPrivate *priv = GABBLE_TUBE_DBUS_GET_PRIVATE (self);

  if (priv->bytestream != NULL)
    {
      gabble_bytestream_iface_close (priv->bytestream, NULL);
    }
  else
    {
      g_signal_emit (G_OBJECT (self), signals[CLOSED], 0);
    }
}

/* There is two step to enable receiving a D-Bus connection from the local
 * application:
 * - listen on the socket
 * - add the socket in the mainloop
 *
 * We need to know the socket path to return from the AcceptDBusTube D-Bus
 * call but the socket in the mainloop must be added only when we are ready
 * to receive connections, that is when the bytestream is fully open with the
 * remote contact.
 *
 * See also Bug 13891:
 * https://bugs.freedesktop.org/show_bug.cgi?id=13891
 * */
void
gabble_tube_dbus_listen (GabbleTubeDBus *self)
{
#define SERVER_LISTEN_MAX_TRIES 5
  GabbleTubeDBusPrivate *priv = GABBLE_TUBE_DBUS_GET_PRIVATE (self);
  guint i;

  if (priv->dbus_srv != NULL)
    return;

  g_signal_connect (priv->bytestream, "data-received",
      G_CALLBACK (data_received_cb), self);

  for (i = 0; i < SERVER_LISTEN_MAX_TRIES; i++)
    {
      gchar suffix[8];
      DBusError error;

      g_free (priv->dbus_srv_addr);
      g_free (priv->socket_path);

      generate_ascii_string (8, suffix);
      priv->socket_path = g_strdup_printf ("%s/dbus-gabble-%.8s",
          g_get_tmp_dir (), suffix);
      priv->dbus_srv_addr = g_strdup_printf ("unix:path=%s",
          priv->socket_path);

      dbus_error_init (&error);
      priv->dbus_srv = dbus_server_listen (priv->dbus_srv_addr, &error);

      if (priv->dbus_srv != NULL)
        break;

      DEBUG ("dbus_server_listen failed (try %u): %s: %s", i, error.name,
          error.message);
      dbus_error_free (&error);
    }

  if (priv->dbus_srv == NULL)
    {
      DEBUG ("all attempts failed. Close the tube");

      g_free (priv->dbus_srv_addr);
      priv->dbus_srv_addr = NULL;

      g_free (priv->socket_path);
      priv->socket_path = NULL;

      do_close (self);
      return;
    }

  DEBUG ("listening on %s", priv->dbus_srv_addr);

  dbus_server_set_new_connection_function (priv->dbus_srv, new_connection_cb,
      self, NULL);
}

static void
tube_dbus_open (GabbleTubeDBus *self)
{
  GabbleTubeDBusPrivate *priv = GABBLE_TUBE_DBUS_GET_PRIVATE (self);

  gabble_tube_dbus_listen (self);

  if (priv->dbus_srv != NULL)
    {
      dbus_server_setup_with_g_main (priv->dbus_srv, NULL);
    }
}

static void
gabble_tube_dbus_init (GabbleTubeDBus *self)
{
  GabbleTubeDBusPrivate *priv = G_TYPE_INSTANCE_GET_PRIVATE (self,
      GABBLE_TYPE_TUBE_DBUS, GabbleTubeDBusPrivate);

  self->priv = priv;
}

static void
unref_handle_foreach (gpointer key,
                      gpointer value,
                      gpointer user_data)
{
  TpHandle handle = GPOINTER_TO_UINT (key);
  TpHandleRepoIface *contact_repo = (TpHandleRepoIface *) user_data;

  tp_handle_unref (contact_repo, handle);
}

static TpTubeState
get_tube_state (GabbleTubeDBus *self)
{
  GabbleTubeDBusPrivate *priv = GABBLE_TUBE_DBUS_GET_PRIVATE (self);
  GabbleBytestreamState bytestream_state;

  if (priv->bytestream == NULL)
    /* bytestream not yet created as we're waiting for the SI reply */
    return TP_TUBE_STATE_REMOTE_PENDING;

  g_object_get (priv->bytestream, "state", &bytestream_state, NULL);

  switch (bytestream_state)
    {
      case GABBLE_BYTESTREAM_STATE_OPEN:
        return TP_TUBE_STATE_OPEN;
        break;
      case GABBLE_BYTESTREAM_STATE_LOCAL_PENDING:
      case GABBLE_BYTESTREAM_STATE_ACCEPTED:
        return TP_TUBE_STATE_LOCAL_PENDING;
        break;
      case GABBLE_BYTESTREAM_STATE_INITIATING:
        return TP_TUBE_STATE_REMOTE_PENDING;
        break;
      default:
        g_return_val_if_reached (0);
    }
}

static void
bytestream_state_changed_cb (GabbleBytestreamIface *bytestream,
                             GabbleBytestreamState state,
                             gpointer user_data)
{
  GabbleTubeDBus *self = GABBLE_TUBE_DBUS (user_data);
  GabbleTubeDBusPrivate *priv = GABBLE_TUBE_DBUS_GET_PRIVATE (self);

  if (state == GABBLE_BYTESTREAM_STATE_CLOSED)
    {
      if (priv->bytestream != NULL)
        {
          g_object_unref (priv->bytestream);
          priv->bytestream = NULL;
        }

      g_signal_emit (G_OBJECT (self), signals[CLOSED], 0);
    }
  else if (state == GABBLE_BYTESTREAM_STATE_OPEN)
    {
      tube_dbus_open (self);
      g_signal_emit (G_OBJECT (self), signals[OPENED], 0);
    }
}

static void
gabble_tube_dbus_dispose (GObject *object)
{
  GabbleTubeDBus *self = GABBLE_TUBE_DBUS (object);
  GabbleTubeDBusPrivate *priv = GABBLE_TUBE_DBUS_GET_PRIVATE (self);
  TpHandleRepoIface *contact_repo = tp_base_connection_get_handles (
      (TpBaseConnection *) priv->conn, TP_HANDLE_TYPE_CONTACT);

  if (priv->dispose_has_run)
    return;

  priv->dispose_has_run = TRUE;

  if (priv->bytestream != NULL)
    {
      gabble_bytestream_iface_close (priv->bytestream, NULL);
    }

  if (priv->dbus_conn != NULL)
    {
      dbus_connection_close (priv->dbus_conn);
      dbus_connection_unref (priv->dbus_conn);
    }

  if (priv->dbus_srv != NULL)
    {
      dbus_server_disconnect (priv->dbus_srv);
      dbus_server_unref (priv->dbus_srv);
      priv->dbus_srv = NULL;
    }

  if (priv->socket_path != NULL)
    {
      if (g_unlink (priv->socket_path) != 0)
        {
          DEBUG ("unlink of %s failed: %s", priv->socket_path,
              g_strerror (errno));
        }
    }

  if (priv->dbus_msg_queue != NULL)
    {
      GSList *i;
      for (i = priv->dbus_msg_queue; i != NULL; i = g_slist_delete_link (i, i))
        {
          DBusMessage *msg = i->data;
          dbus_message_unref (msg);
        }
      priv->dbus_msg_queue = NULL;
      priv->dbus_msg_queue_size = 0;
    }

  g_free (priv->dbus_srv_addr);
  priv->dbus_srv_addr = NULL;
  g_free (priv->socket_path);
  priv->socket_path = NULL;
  g_free (priv->dbus_local_name);
  priv->dbus_local_name = NULL;

  if (priv->dbus_names)
    {
      g_hash_table_foreach (priv->dbus_names, unref_handle_foreach,
          contact_repo);
      g_hash_table_destroy (priv->dbus_names);
    }

  if (priv->dbus_name_to_handle)
     {
       g_hash_table_destroy (priv->dbus_name_to_handle);
       priv->dbus_name_to_handle = NULL;
     }

  if (priv->reassembly_buffer)
    g_string_free (priv->reassembly_buffer, TRUE);

  tp_handle_unref (contact_repo, priv->initiator);

  if (G_OBJECT_CLASS (gabble_tube_dbus_parent_class)->dispose)
    G_OBJECT_CLASS (gabble_tube_dbus_parent_class)->dispose (object);
}

static void
gabble_tube_dbus_finalize (GObject *object)
{
  GabbleTubeDBus *self = GABBLE_TUBE_DBUS (object);
  GabbleTubeDBusPrivate *priv = GABBLE_TUBE_DBUS_GET_PRIVATE (self);

  g_free (priv->stream_id);
  g_free (priv->service);
  g_hash_table_destroy (priv->parameters);

  G_OBJECT_CLASS (gabble_tube_dbus_parent_class)->finalize (object);
}

static void
gabble_tube_dbus_get_property (GObject *object,
                               guint property_id,
                               GValue *value,
                               GParamSpec *pspec)
{
  GabbleTubeDBus *self = GABBLE_TUBE_DBUS (object);
  GabbleTubeDBusPrivate *priv = GABBLE_TUBE_DBUS_GET_PRIVATE (self);

  switch (property_id)
    {
      case PROP_CONNECTION:
        g_value_set_object (value, priv->conn);
        break;
      case PROP_HANDLE:
        g_value_set_uint (value, priv->handle);
        break;
      case PROP_HANDLE_TYPE:
        g_value_set_uint (value, priv->handle_type);
        break;
      case PROP_SELF_HANDLE:
        g_value_set_uint (value, priv->self_handle);
        break;
      case PROP_ID:
        g_value_set_uint (value, priv->id);
        break;
      case PROP_BYTESTREAM:
        g_value_set_object (value, priv->bytestream);
        break;
      case PROP_STREAM_ID:
        g_value_set_string (value, priv->stream_id);
        break;
      case PROP_TYPE:
        g_value_set_uint (value, TP_TUBE_TYPE_DBUS);
        break;
      case PROP_INITIATOR_HANDLE:
        g_value_set_uint (value, priv->initiator);
        break;
      case PROP_SERVICE:
        g_value_set_string (value, priv->service);
        break;
      case PROP_PARAMETERS:
        g_value_set_boxed (value, priv->parameters);
        break;
      case PROP_STATE:
        g_value_set_uint (value, get_tube_state (self));
        break;
      case PROP_DBUS_ADDRESS:
        g_value_set_string (value, priv->dbus_srv_addr);
        break;
      case PROP_DBUS_NAME:
        g_value_set_string (value, priv->dbus_local_name);
        break;
      case PROP_DBUS_NAMES:
        g_value_set_boxed (value, priv->dbus_names);
        break;
      default:
        G_OBJECT_WARN_INVALID_PROPERTY_ID (object, property_id, pspec);
        break;
    }
}

static void
gabble_tube_dbus_set_property (GObject *object,
                               guint property_id,
                               const GValue *value,
                               GParamSpec *pspec)
{
  GabbleTubeDBus *self = GABBLE_TUBE_DBUS (object);
  GabbleTubeDBusPrivate *priv = GABBLE_TUBE_DBUS_GET_PRIVATE (self);

  switch (property_id)
    {
      case PROP_CONNECTION:
        priv->conn = g_value_get_object (value);
        break;
      case PROP_HANDLE:
        priv->handle = g_value_get_uint (value);
        break;
      case PROP_HANDLE_TYPE:
        priv->handle_type = g_value_get_uint (value);
        break;
      case PROP_SELF_HANDLE:
        priv->self_handle = g_value_get_uint (value);
        break;
      case PROP_ID:
        priv->id = g_value_get_uint (value);
        break;
      case PROP_BYTESTREAM:
        if (priv->bytestream == NULL)
          {
            GabbleBytestreamState state;

            priv->bytestream = g_value_get_object (value);
            g_object_ref (priv->bytestream);

            g_object_get (priv->bytestream, "state", &state, NULL);
            if (state == GABBLE_BYTESTREAM_STATE_OPEN)
              {
                tube_dbus_open (self);
              }

            g_signal_connect (priv->bytestream, "state-changed",
                G_CALLBACK (bytestream_state_changed_cb), self);
          }
        break;
      case PROP_STREAM_ID:
        g_free (priv->stream_id);
        priv->stream_id = g_value_dup_string (value);
        break;
      case PROP_INITIATOR_HANDLE:
        priv->initiator = g_value_get_uint (value);
        break;
      case PROP_SERVICE:
        g_free (priv->service);
        priv->service = g_value_dup_string (value);
        break;
      case PROP_PARAMETERS:
        priv->parameters = g_value_dup_boxed (value);
        break;
      default:
        G_OBJECT_WARN_INVALID_PROPERTY_ID (object, property_id, pspec);
        break;
    }
}

static GObject *
gabble_tube_dbus_constructor (GType type,
                              guint n_props,
                              GObjectConstructParam *props)
{
  GObject *obj;
  GabbleTubeDBus *self;
  GabbleTubeDBusPrivate *priv;
  TpHandleRepoIface *contact_repo;
  TpBaseConnection *base;

  obj = G_OBJECT_CLASS (gabble_tube_dbus_parent_class)->
    constructor (type, n_props, props);
  self = GABBLE_TUBE_DBUS (obj);

  priv = GABBLE_TUBE_DBUS_GET_PRIVATE (self);

  /* Ref the initiator handle */
  g_assert (priv->conn != NULL);
  g_assert (priv->initiator != 0);
  contact_repo = tp_base_connection_get_handles
      ((TpBaseConnection *) priv->conn, TP_HANDLE_TYPE_CONTACT);
  tp_handle_ref (contact_repo, priv->initiator);

  base = (TpBaseConnection *) priv->conn;

  g_assert (priv->self_handle != 0);
  if (priv->handle_type == TP_HANDLE_TYPE_ROOM)
    {
      /*
       * We have to create a pseudo-IBB bytestream that will be
       * used by this MUC tube to communicate.
       *
       * We don't create the bytestream of private D-Bus tube yet.
       * It will be when we'll receive the answer of the SI request
       */
      GabbleBytestreamMuc *bytestream;
      GabbleBytestreamState state;
      gchar *nick;

      g_assert (priv->stream_id != NULL);

      priv->dbus_names = g_hash_table_new_full (g_direct_hash, g_direct_equal,
          NULL, g_free);
      priv->dbus_name_to_handle = g_hash_table_new_full (g_str_hash,
         g_str_equal, NULL, NULL);

      gabble_decode_jid (tp_handle_inspect (contact_repo, priv->self_handle),
          NULL, NULL, &nick);

      priv->dbus_local_name = _gabble_generate_dbus_unique_name (nick);

      DEBUG ("local name: %s", priv->dbus_local_name);

      if (priv->initiator == priv->self_handle)
        {
          /* We create this tube, bytestream is open */
          state = GABBLE_BYTESTREAM_STATE_OPEN;
        }
      else
        {
          /* We don't create this tube, bytestream is local pending */
          state = GABBLE_BYTESTREAM_STATE_LOCAL_PENDING;
        }

      bytestream = gabble_bytestream_factory_create_muc (
          priv->conn->bytestream_factory,
          priv->handle,
          priv->stream_id,
          state);

      g_object_set (self, "bytestream", bytestream, NULL);

      g_free (nick);
    }
  else
    {
      /* The D-Bus names mapping is used in muc tubes only */
      priv->dbus_local_name = NULL;
      priv->dbus_names = NULL;
      priv->dbus_name_to_handle = NULL;

      /* For contact (IBB) tubes we need to be able to reassemble messages. */
      priv->reassembly_buffer = g_string_new ("");
      priv->reassembly_bytes_needed = 0;
    }

  return obj;
}

static void
gabble_tube_dbus_class_init (GabbleTubeDBusClass *gabble_tube_dbus_class)
{
  GObjectClass *object_class = G_OBJECT_CLASS (gabble_tube_dbus_class);
  GParamSpec *param_spec;

  object_class->get_property = gabble_tube_dbus_get_property;
  object_class->set_property = gabble_tube_dbus_set_property;
  object_class->constructor = gabble_tube_dbus_constructor;

  g_type_class_add_private (gabble_tube_dbus_class,
      sizeof (GabbleTubeDBusPrivate));

  object_class->dispose = gabble_tube_dbus_dispose;
  object_class->finalize = gabble_tube_dbus_finalize;

  g_object_class_override_property (object_class, PROP_CONNECTION,
    "connection");
  g_object_class_override_property (object_class, PROP_HANDLE,
    "handle");
  g_object_class_override_property (object_class, PROP_HANDLE_TYPE,
    "handle-type");
  g_object_class_override_property (object_class, PROP_SELF_HANDLE,
    "self-handle");
  g_object_class_override_property (object_class, PROP_ID,
    "id");
  g_object_class_override_property (object_class, PROP_TYPE,
    "type");
  g_object_class_override_property (object_class, PROP_SERVICE,
    "service");
  g_object_class_override_property (object_class, PROP_PARAMETERS,
    "parameters");
  g_object_class_override_property (object_class, PROP_STATE,
    "state");

  /* TODO: When D-Bus tubes will be channels, this will be replaced by
   * g_object_class_override_property*/
  param_spec = g_param_spec_uint (
      "initiator-handle",
      "Initiator handle",
      "The TpHandle of the initiator of this tube object.",
      0, G_MAXUINT32, 0,
      G_PARAM_CONSTRUCT_ONLY | G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS);
  g_object_class_install_property (object_class, PROP_INITIATOR_HANDLE,
      param_spec);

  param_spec = g_param_spec_object (
      "bytestream",
      "Object implementing the GabbleBytestreamIface interface",
      "Bytestream object used for streaming data for this"
      "tube object.",
      G_TYPE_OBJECT,
      G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS);
  g_object_class_install_property (object_class, PROP_BYTESTREAM,
      param_spec);

  param_spec = g_param_spec_string (
      "stream-id",
      "stream id",
      "The identifier of this tube's bytestream",
      "",
      G_PARAM_CONSTRUCT_ONLY | G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS);
  g_object_class_install_property (object_class, PROP_STREAM_ID,
      param_spec);

  param_spec = g_param_spec_string (
      "dbus-address",
      "D-Bus address",
      "The D-Bus address on which this tube will listen for connections",
      "",
      G_PARAM_READABLE | G_PARAM_STATIC_STRINGS);
  g_object_class_install_property (object_class, PROP_DBUS_ADDRESS,
      param_spec);

  param_spec = g_param_spec_string (
      "dbus-name",
      "D-Bus name",
      "The local D-Bus name on the virtual bus (used for muc tubes only).",
      "",
      G_PARAM_READABLE | G_PARAM_STATIC_STRINGS);
  g_object_class_install_property (object_class, PROP_DBUS_NAME, param_spec);

  param_spec = g_param_spec_boxed (
      "dbus-names",
      "D-Bus names",
      "Mapping of contact handles to D-Bus names (used for muc tubes only).",
      G_TYPE_HASH_TABLE,
      G_PARAM_READABLE | G_PARAM_STATIC_STRINGS);
  g_object_class_install_property (object_class, PROP_DBUS_NAMES, param_spec);

  signals[OPENED] =
    g_signal_new ("tube-opened",
                  G_OBJECT_CLASS_TYPE (gabble_tube_dbus_class),
                  G_SIGNAL_RUN_LAST | G_SIGNAL_DETAILED,
                  0,
                  NULL, NULL,
                  gabble_marshal_VOID__VOID,
                  G_TYPE_NONE, 0);

  signals[CLOSED] =
    g_signal_new ("tube-closed",
                  G_OBJECT_CLASS_TYPE (gabble_tube_dbus_class),
                  G_SIGNAL_RUN_LAST | G_SIGNAL_DETAILED,
                  0,
                  NULL, NULL,
                  gabble_marshal_VOID__VOID,
                  G_TYPE_NONE, 0);

  signals[OFFERED] =
    g_signal_new ("tube-offered",
                  G_OBJECT_CLASS_TYPE (gabble_tube_dbus_class),
                  G_SIGNAL_RUN_LAST | G_SIGNAL_DETAILED,
                  0,
                  NULL, NULL,
                  gabble_marshal_VOID__VOID,
                  G_TYPE_NONE, 0);
}

static void
message_received (GabbleTubeDBus *tube,
                  TpHandle sender,
                  const char *data,
                  size_t len)
{
  GabbleTubeDBusPrivate *priv = GABBLE_TUBE_DBUS_GET_PRIVATE (tube);
  DBusMessage *msg;
  DBusError error = {0,};
  const gchar *sender_name;
  const gchar *destination;
  guint32 serial;

  msg = dbus_message_demarshal (data, len, &error);

  if (msg == NULL)
    {
      /* message was corrupted */
      DEBUG ("received corrupted message from %d: %s: %s", sender,
          error.name, error.message);
      dbus_error_free (&error);
      return;
    }

  if (priv->handle_type == TP_HANDLE_TYPE_ROOM)
    {
      destination = dbus_message_get_destination (msg);
      /* If destination is NULL this msg is broadcasted (signals) so we don't
       * have to check it */
      if (destination != NULL && tp_strdiff (priv->dbus_local_name,
            destination))
        {
          /* This message is not intended for this participant.
           * Discard it. */
          DEBUG ("message not intended for this participant (destination = "
              "%s)", destination);
          goto unref;
        }

      sender_name = g_hash_table_lookup (priv->dbus_names,
          GUINT_TO_POINTER (sender));

      if (tp_strdiff (sender_name, dbus_message_get_sender (msg)))
        {
          DEBUG ("invalid sender %s (expected %s for sender handle %d)",
                 dbus_message_get_sender (msg), sender_name, sender);
          goto unref;
        }
    }

  if (!priv->dbus_conn)
    {
      DEBUG ("no D-Bus connection: queuing the message");

      /* If the application never connects to the private dbus connection, we
       * don't want to eat all the memory. Only queue MAX_QUEUE_SIZE bytes. If
       * there are more messages, drop them. */
      if (priv->dbus_msg_queue_size + len > MAX_QUEUE_SIZE)
        {
          DEBUG ("D-Bus message queue size limit reached (%u bytes). "
                 "Ignore this message.",
                 MAX_QUEUE_SIZE);
          goto unref;
        }

      priv->dbus_msg_queue = g_slist_prepend (priv->dbus_msg_queue, msg);
      priv->dbus_msg_queue_size += len;

      /* returns without unref the message */
      return;
    }

  DEBUG ("delivering message from '%s' to '%s'",
         dbus_message_get_sender (msg),
         dbus_message_get_destination (msg));

  /* XXX: what do do if this returns FALSE? */
  dbus_connection_send (priv->dbus_conn, msg, &serial);

unref:
  dbus_message_unref (msg);
}

static guint32
collect_le32 (char *str)
{
  unsigned char *bytes = (unsigned char *) str;

  return bytes[0] | (bytes[1] << 8) | (bytes[2] << 16) | (bytes[3] << 24);
}

static guint32
collect_be32 (char *str)
{
  unsigned char *bytes = (unsigned char *) str;

  return (bytes[0] << 24) | (bytes[1] << 16) | (bytes[2] << 24) | bytes[3];
}

static void
data_received_cb (GabbleBytestreamIface *stream,
                  TpHandle sender,
                  GString *data,
                  gpointer user_data)
{
  GabbleTubeDBus *tube = GABBLE_TUBE_DBUS (user_data);
  GabbleTubeDBusPrivate *priv = GABBLE_TUBE_DBUS_GET_PRIVATE (tube);

  if (priv->handle_type == TP_HANDLE_TYPE_CONTACT)
    {
      GString *buf = priv->reassembly_buffer;

      g_assert (buf != NULL);

      g_string_append_len (buf, data->str, data->len);
      DEBUG ("Received %" G_GSIZE_FORMAT " bytes, so we now have %"
          G_GSIZE_FORMAT " bytes in reassembly buffer", data->len, buf->len);

      /* Each D-Bus message has a 16-byte fixed header, in which
       *
       * * byte 0 is 'l' (ell) or 'B' for endianness
       * * bytes 4-7 are body length "n" in bytes in that endianness
       * * bytes 12-15 are length "m" of param array in bytes in that
       *   endianness
       *
       * followed by m + n + ((8 - (m % 8)) % 8) bytes of other content.
       */

      while (buf->len >= 16)
        {
          guint32 body_length, params_length, m;

          /* see if we have a whole message and have already calculated
           * how many bytes it needs */

          if (priv->reassembly_bytes_needed != 0)
            {
              if (buf->len >= priv->reassembly_bytes_needed)
                {
                  DEBUG ("Received complete D-Bus message of size %"
                      G_GINT32_FORMAT, priv->reassembly_bytes_needed);
                  message_received (tube, sender, buf->str,
                      priv->reassembly_bytes_needed);
                  g_string_erase (buf, 0, priv->reassembly_bytes_needed);
                  priv->reassembly_bytes_needed = 0;
                }
              else
                {
                  /* we'll have to wait for more data */
                  break;
                }
            }

          if (buf->len < 16)
            break;

          /* work out how big the next message is going to be */

          if (buf->str[0] == DBUS_BIG_ENDIAN)
            {
              body_length = collect_be32 (buf->str + 4);
              m = collect_be32 (buf->str + 12);
            }
          else if (buf->str[0] == DBUS_LITTLE_ENDIAN)
            {
              body_length = collect_le32 (buf->str + 4);
              m = collect_le32 (buf->str + 12);
            }
          else
            {
              DEBUG ("D-Bus message has unknown endianness byte 0x%x, "
                  "closing tube", (unsigned int) buf->str[0]);
              gabble_tube_dbus_close ((GabbleTubeIface *) tube, TRUE);
              return;
            }

          /* pad to 8-byte boundary */
          params_length = m + ((8 - (m % 8)) % 8);
          g_assert (params_length % 8 == 0);
          g_assert (params_length >= m);
          g_assert (params_length < m + 8);

          priv->reassembly_bytes_needed = params_length + body_length + 16;

          /* n.b.: this looks as if it could be simplified to just the third
           * test, but that would be wrong if the addition had overflowed, so
           * don't do that. The first and second tests are sufficient to
           * ensure no overflow on 32-bit platforms */
          if (body_length > DBUS_MAXIMUM_MESSAGE_LENGTH ||
              params_length > DBUS_MAXIMUM_ARRAY_LENGTH ||
              priv->reassembly_bytes_needed > DBUS_MAXIMUM_MESSAGE_LENGTH)
            {
              DEBUG ("D-Bus message is too large to be valid, closing tube");
              gabble_tube_dbus_close ((GabbleTubeIface *) tube, TRUE);
              return;
            }

          g_assert (priv->reassembly_bytes_needed != 0);
          DEBUG ("We need %" G_GINT32_FORMAT " bytes for the next full "
              "message", priv->reassembly_bytes_needed);
        }
    }
  else
    {
      /* MUC bytestreams are message-boundary preserving, which is necessary,
       * because we can't assume we started at the beginning */
      g_assert (GABBLE_IS_BYTESTREAM_MUC (priv->bytestream));
      message_received (tube, sender, data->str, data->len);
    }
}

GabbleTubeDBus *
gabble_tube_dbus_new (GabbleConnection *conn,
                      TpHandle handle,
                      TpHandleType handle_type,
                      TpHandle self_handle,
                      TpHandle initiator,
                      const gchar *service,
                      GHashTable *parameters,
                      const gchar *stream_id,
                      guint id,
                      GabbleBytestreamIface *bytestream)
{
  GabbleTubeDBus *tube = g_object_new (GABBLE_TYPE_TUBE_DBUS,
      "connection", conn,
      "handle", handle,
      "handle-type", handle_type,
      "self-handle", self_handle,
      "initiator-handle", initiator,
      "service", service,
      "parameters", parameters,
      "stream-id", stream_id,
      "id", id,
      NULL);

  if (bytestream != NULL)
    g_object_set (tube, "bytestream", bytestream, NULL);

  return tube;
}

static void
augment_si_accept_iq (LmMessageNode *si,
                      gpointer user_data)
{
  LmMessageNode *tube_node;

  tube_node = lm_message_node_add_child (si, "tube", "");
  lm_message_node_set_attribute (tube_node, "xmlns", NS_TUBES);
}

/*
 * gabble_tube_dbus_accept
 *
 * Implements gabble_tube_iface_accept on GabbleTubeIface
 */
static gboolean
gabble_tube_dbus_accept (GabbleTubeIface *tube,
                         GError **error)
{
  GabbleTubeDBus *self = GABBLE_TUBE_DBUS (tube);
  GabbleTubeDBusPrivate *priv = GABBLE_TUBE_DBUS_GET_PRIVATE (self);
  GabbleBytestreamState state;

  g_assert (priv->bytestream != NULL);

  g_object_get (priv->bytestream,
      "state", &state,
      NULL);

  if (state != GABBLE_BYTESTREAM_STATE_LOCAL_PENDING)
    return TRUE;

  if (priv->handle_type == TP_HANDLE_TYPE_CONTACT)
    {
      /* Bytestream was created using a SI request so
       * we have to accept it */

      DEBUG ("accept the SI request");

      gabble_bytestream_iface_accept (priv->bytestream, augment_si_accept_iq,
          self);
    }
  else
    {
      /* No SI so the bytestream is open */
      DEBUG ("no SI, bytestream open");
      g_object_set (priv->bytestream,
          "state", GABBLE_BYTESTREAM_STATE_OPEN,
          NULL);
    }

  return TRUE;
}

/*
 * gabble_tube_dbus_close
 *
 * Implements gabble_tube_iface_close on GabbleTubeIface
 */
static void
gabble_tube_dbus_close (GabbleTubeIface *tube, gboolean closed_remotely)
{
  GabbleTubeDBus *self = GABBLE_TUBE_DBUS (tube);

  do_close (self);
}

/**
 * gabble_tube_dbus_add_bytestream
 *
 * Implements gabble_tube_iface_add_bytestream on GabbleTubeIface
 */
static void
gabble_tube_dbus_add_bytestream (GabbleTubeIface *tube,
                                 GabbleBytestreamIface *bytestream)
{
  /* FIXME: should we support this, if we don't have a bytestream yet? */
  DEBUG ("D-Bus doesn't support extra bytestream");
  gabble_bytestream_iface_close (bytestream, NULL);
}

gboolean
gabble_tube_dbus_add_name (GabbleTubeDBus *self,
                           TpHandle handle,
                           const gchar *name)
{
  GabbleTubeDBusPrivate *priv = GABBLE_TUBE_DBUS_GET_PRIVATE (self);
  TpHandleRepoIface *contact_repo = tp_base_connection_get_handles (
      (TpBaseConnection *) priv->conn, TP_HANDLE_TYPE_CONTACT);
  gchar *name_copy;

  g_assert (priv->handle_type == TP_HANDLE_TYPE_ROOM);
  g_assert (g_hash_table_size (priv->dbus_names) ==
      g_hash_table_size (priv->dbus_name_to_handle));

  if (g_hash_table_lookup (priv->dbus_names, GUINT_TO_POINTER (handle))
      != NULL)
    {
      DEBUG ("contact %d has already announced his D-Bus name", handle);
      return FALSE;
    }

  if (g_hash_table_lookup (priv->dbus_name_to_handle, name) != NULL)
    {
      DEBUG ("D-Bus name %s already used", name);
      return FALSE;
    }

  if (g_str_has_prefix (name, ":2."))
    {
      gchar *nick, *supposed_name;
      const gchar *jid;

      jid = tp_handle_inspect (contact_repo, handle);
      gabble_decode_jid (jid, NULL, NULL, &nick);
      supposed_name = _gabble_generate_dbus_unique_name (nick);
      g_free (nick);

      if (tp_strdiff (name, supposed_name))
        {
          DEBUG ("contact %s announces %s as D-Bus name but it should be %s",
              jid, name, supposed_name);
          g_free (supposed_name);
          return FALSE;
        }
      g_free (supposed_name);
    }

  name_copy = g_strdup (name);
  g_hash_table_insert (priv->dbus_names, GUINT_TO_POINTER (handle),
      name_copy);
  tp_handle_ref (contact_repo, handle);

  g_hash_table_insert (priv->dbus_name_to_handle, name_copy,
      GUINT_TO_POINTER (handle));

  return TRUE;
}

gboolean
gabble_tube_dbus_remove_name (GabbleTubeDBus *self,
                              TpHandle handle)
{
  GabbleTubeDBusPrivate *priv = GABBLE_TUBE_DBUS_GET_PRIVATE (self);
  TpHandleRepoIface *contact_repo = tp_base_connection_get_handles (
      (TpBaseConnection *) priv->conn, TP_HANDLE_TYPE_CONTACT);
  const gchar *name;

  g_assert (priv->handle_type == TP_HANDLE_TYPE_ROOM);

  name = g_hash_table_lookup (priv->dbus_names, GUINT_TO_POINTER (handle));
  if (name == NULL)
    return FALSE;

  g_hash_table_remove (priv->dbus_name_to_handle, name);
  g_hash_table_remove (priv->dbus_names, GUINT_TO_POINTER (handle));

  g_assert (g_hash_table_size (priv->dbus_names) ==
      g_hash_table_size (priv->dbus_name_to_handle));

  tp_handle_unref (contact_repo, handle);
  return TRUE;
}

gboolean
gabble_tube_dbus_handle_in_names (GabbleTubeDBus *self,
                                  TpHandle handle)
{
  GabbleTubeDBusPrivate *priv = GABBLE_TUBE_DBUS_GET_PRIVATE (self);

  g_assert (priv->handle_type == TP_HANDLE_TYPE_ROOM);

  return (g_hash_table_lookup (priv->dbus_names, GUINT_TO_POINTER (handle))
      != NULL);
}

gchar *
_gabble_generate_dbus_unique_name (const gchar *nick)
{
  gchar *encoded, *result;
  size_t len;
  guint i;

  len = strlen (nick);

  if (len <= 186)
    {
      encoded = base64_encode (len, nick, FALSE);
    }
  else
    {
      guchar sha1[20];
      GString *tmp;

      sha1_bin (nick, len, sha1);
      tmp = g_string_sized_new (169 + 20);

      g_string_append_len (tmp, nick, 169);
      g_string_append_len (tmp, (const gchar *) sha1, 20);

      encoded = base64_encode (tmp->len, tmp->str, FALSE);

      g_string_free (tmp, TRUE);
    }

  for (i = 0; encoded[i] != '\0'; i++)
    {
      switch (encoded[i])
        {
          case '+':
            encoded[i] = '_';
            break;
          case '/':
            encoded[i] = '-';
            break;
          case '=':
            encoded[i] = 'A';
            break;
        }
    }

  result = g_strdup_printf (":2.%s", encoded);

  g_free (encoded);
  return result;
}

static void
tube_iface_init (gpointer g_iface,
                 gpointer iface_data)
{
  GabbleTubeIfaceClass *klass = (GabbleTubeIfaceClass *) g_iface;

  klass->accept = gabble_tube_dbus_accept;
  klass->close = gabble_tube_dbus_close;
  klass->add_bytestream = gabble_tube_dbus_add_bytestream;
}
