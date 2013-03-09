/*
 * tube-stream.c - Source for GabbleTubeStream
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
#include "tube-stream.h"

#include <string.h>
#include <sys/types.h>
#include <errno.h>

#include <gibber/gibber-sockets.h>

#ifdef HAVE_UNISTD_H
# include <unistd.h>
#endif

#include <glib/gstdio.h>
#include <telepathy-glib/telepathy-glib.h>
#include <telepathy-glib/telepathy-glib-dbus.h>

#include "extensions/extensions.h"

#define DEBUG_FLAG GABBLE_DEBUG_TUBES

#include <gibber/gibber-fd-transport.h>
#include <gibber/gibber-listener.h>
#include <gibber/gibber-tcp-transport.h>
#include <gibber/gibber-transport.h>
#include <gibber/gibber-unix-transport.h>
#include <gibber/gibber-util.h>

#include "bytestream-factory.h"
#include "bytestream-iface.h"
#include "connection.h"
#include "debug.h"
#include "disco.h"
#include "gabble-signals-marshal.h"
#include "muc-channel.h"
#include "muc-tube-stream.h"
#include "namespaces.h"
#include "presence-cache.h"
#include "presence.h"
#include "tube-iface.h"
#include "util.h"

static void tube_iface_init (gpointer g_iface, gpointer iface_data);
static void streamtube_iface_init (gpointer g_iface, gpointer iface_data);

G_DEFINE_TYPE_WITH_CODE (GabbleTubeStream, gabble_tube_stream,
    TP_TYPE_BASE_CHANNEL,
    G_IMPLEMENT_INTERFACE (TP_TYPE_SVC_DBUS_PROPERTIES,
      tp_dbus_properties_mixin_iface_init);
    G_IMPLEMENT_INTERFACE (GABBLE_TYPE_TUBE_IFACE, tube_iface_init);
    G_IMPLEMENT_INTERFACE (TP_TYPE_SVC_CHANNEL_TYPE_STREAM_TUBE,
      streamtube_iface_init);
    G_IMPLEMENT_INTERFACE (TP_TYPE_SVC_CHANNEL_INTERFACE_TUBE,
      NULL);
    G_IMPLEMENT_INTERFACE (TP_TYPE_SVC_CHANNEL_INTERFACE_GROUP,
      tp_external_group_mixin_iface_init);
);

static const gchar * const gabble_tube_stream_channel_allowed_properties[] = {
    TP_IFACE_CHANNEL ".TargetHandle",
    TP_IFACE_CHANNEL ".TargetID",
    TP_IFACE_CHANNEL_TYPE_STREAM_TUBE ".Service",
    NULL
};

/* Linux glibc bits/socket.h suggests that struct sockaddr_storage is
 * not guaranteed to be big enough for AF_UNIX addresses */
typedef union
{
#ifdef GIBBER_TYPE_UNIX_TRANSPORT
  /* we'd call this unix, but gcc predefines that. Thanks, gcc */
  struct sockaddr_un un;
#endif
  struct sockaddr_in ipv4;
  struct sockaddr_in6 ipv6;
} SockAddr;

/* signals */
enum
{
  OPENED,
  NEW_CONNECTION,
  CLOSED,
  OFFERED,
  LAST_SIGNAL
};

static guint signals[LAST_SIGNAL] = {0};

/* properties */
enum
{
  PROP_SELF_HANDLE = 1,
  PROP_ID,
  PROP_TYPE,
  PROP_SERVICE,
  PROP_PARAMETERS,
  PROP_STATE,
  PROP_ADDRESS_TYPE,
  PROP_ADDRESS,
  PROP_ACCESS_CONTROL,
  PROP_ACCESS_CONTROL_PARAM,
  PROP_SUPPORTED_SOCKET_TYPES,
  PROP_MUC,
  LAST_PROPERTY
};

struct _GabbleTubeStreamPrivate
{
  TpHandle self_handle;
  guint64 id;

  /* Bytestreams for tubes. One tube can have several bytestreams. The
   * mapping between the tube bytestream and the transport to the local
   * application is stored in the transport_to_bytestream and
   * bytestream_to_transport fields. This is used both on initiator-side and
   * on recipient-side. */

  /* (GabbleBytestreamIface *) -> (GibberTransport *)
   *
   * The (b->t) is inserted as soon as they are created. On initiator side,
   * we receive an incoming bytestream, create a transport and insert (b->t).
   * On recipient side, we receive an incoming transport, create a bytestream
   * and insert (b->t).
   */
  GHashTable *bytestream_to_transport;

  /* (GibberTransport *) -> (GabbleBytestreamIface *)
   *
   * The (t->b) is also inserted as soon as they are created.
   */
  GHashTable *transport_to_bytestream;

  /* (GibberTransport *) -> guint */
  GHashTable *transport_to_id;
  guint last_connection_id;

  gchar *service;
  GHashTable *parameters;
  TpTubeChannelState state;

  TpSocketAddressType address_type;
  GValue *address;
  TpSocketAccessControl access_control;
  GValue *access_control_param;

  /* listen for connections from local applications */
  GibberListener *local_listener;
  GabbleMucChannel *muc;

  gboolean dispose_has_run;
};

static GPtrArray *
gabble_tube_stream_get_interfaces (TpBaseChannel *base)
{
  GPtrArray *interfaces;

  interfaces = TP_BASE_CHANNEL_CLASS (
      gabble_tube_stream_parent_class)->get_interfaces (base);

  g_ptr_array_add (interfaces, TP_IFACE_CHANNEL_INTERFACE_TUBE);

  return interfaces;
}

typedef struct
{
  GabbleTubeStream *self;
  TpHandle contact;
} transport_connected_data;

static void data_received_cb (GabbleBytestreamIface *ibb, TpHandle sender,
    GString *data, gpointer user_data);
static void transport_connected_cb (GibberTransport *transport,
    transport_connected_data *data);

#ifdef GIBBER_TYPE_UNIX_TRANSPORT
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
#endif

static void
transport_handler (GibberTransport *transport,
                   GibberBuffer *data,
                   gpointer user_data)
{
  GabbleTubeStream *self = GABBLE_TUBE_STREAM (user_data);
  GabbleTubeStreamPrivate *priv = self->priv;
  GabbleBytestreamIface *bytestream;

  bytestream = g_hash_table_lookup (priv->transport_to_bytestream, transport);
  if (bytestream == NULL)
    {
      DEBUG ("no open bytestream associated with this transport");
      return;
    }

  gabble_bytestream_iface_send (bytestream, data->length,
      (const gchar *) data->data);
}

static void
fire_connection_closed (GabbleTubeStream *self,
    GibberTransport *transport,
    const gchar *error,
    const gchar *debug_msg)
{
  GabbleTubeStreamPrivate *priv = self->priv;
  guint connection_id;

  connection_id = GPOINTER_TO_UINT (g_hash_table_lookup (priv->transport_to_id,
        transport));
  if (connection_id == 0)
    {
      DEBUG ("ConnectionClosed has already been fired for this connection");
      return;
    }

  /* remove the ID so we are sure we won't fire ConnectionClosed twice for the
   * same connection. */
  g_hash_table_remove (priv->transport_to_id, transport);

  tp_svc_channel_type_stream_tube_emit_connection_closed (self,
      connection_id, error, debug_msg);
}

static void
transport_disconnected_cb (GibberTransport *transport,
                           GabbleTubeStream *self)
{
  GabbleTubeStreamPrivate *priv = self->priv;
  GabbleBytestreamIface *bytestream;

  fire_connection_closed (self, transport, TP_ERROR_STR_CANCELLED,
      "local socket has been disconnected");

  bytestream = g_hash_table_lookup (priv->transport_to_bytestream, transport);
  if (bytestream == NULL)
    return;

  DEBUG ("transport disconnected. close the extra bytestream");

  gabble_bytestream_iface_close (bytestream, NULL);
}

static void
remove_transport (GabbleTubeStream *self,
                  GabbleBytestreamIface *bytestream,
                  GibberTransport *transport)
{
  GabbleTubeStreamPrivate *priv = self->priv;

  DEBUG ("disconnect and remove transport");
  g_signal_handlers_disconnect_matched (transport, G_SIGNAL_MATCH_DATA,
      0, 0, NULL, NULL, self);
  /* The callback on the "connected" signal doesn't match the above
   * disconnection as it receives a transport_connected_data as user_data
   * and not the self pointer. */
  g_signal_handlers_disconnect_matched (transport, G_SIGNAL_MATCH_FUNC,
      0, 0, NULL, G_CALLBACK (transport_connected_cb), NULL);

  gibber_transport_disconnect (transport);

  fire_connection_closed (self, transport, TP_ERROR_STR_CONNECTION_LOST,
      "bytestream has been broken");

  g_hash_table_remove (priv->transport_to_bytestream, transport);
  g_hash_table_remove (priv->bytestream_to_transport, bytestream);
  g_hash_table_remove (priv->transport_to_id, transport);
}

static void
transport_buffer_empty_cb (GibberTransport *transport,
                           GabbleTubeStream *self)
{
  GabbleTubeStreamPrivate *priv = self->priv;
  GabbleBytestreamIface *bytestream;
  GabbleBytestreamState state;

  bytestream = g_hash_table_lookup (priv->transport_to_bytestream, transport);
  g_assert (bytestream != NULL);
  g_object_get (bytestream, "state", &state, NULL);

  if (state == GABBLE_BYTESTREAM_STATE_CLOSED)
    {
      DEBUG ("buffer is now empty. Transport can be removed");
      remove_transport (self, bytestream, transport);
      return;
    }

  /* Buffer is empty so we can unblock the buffer if it was blocked */
  gabble_bytestream_iface_block_reading (bytestream, FALSE);
}

static void
add_transport (GabbleTubeStream *self,
               GibberTransport *transport,
               GabbleBytestreamIface *bytestream)
{
  gibber_transport_set_handler (transport, transport_handler, self);

  g_signal_connect (transport, "disconnected",
      G_CALLBACK (transport_disconnected_cb), self);
  g_signal_connect (transport, "buffer-empty",
      G_CALLBACK (transport_buffer_empty_cb), self);

  /* We can transfer transport's data; unblock it. */
  gibber_transport_block_receiving (transport, FALSE);
}

static void
bytestream_write_blocked_cb (GabbleBytestreamIface *bytestream,
                             gboolean blocked,
                             GabbleTubeStream *self)
{
  GabbleTubeStreamPrivate *priv = self->priv;
  GibberTransport *transport;

  transport = g_hash_table_lookup (priv->bytestream_to_transport,
      bytestream);
  g_assert (transport != NULL);

  gibber_transport_block_receiving (transport, blocked);
}

static void
extra_bytestream_state_changed_cb (GabbleBytestreamIface *bytestream,
                                   GabbleBytestreamState state,
                                   gpointer user_data)
{
  GabbleTubeStream *self = GABBLE_TUBE_STREAM (user_data);
  GabbleTubeStreamPrivate *priv = self->priv;

  DEBUG ("Called.");

  if (state == GABBLE_BYTESTREAM_STATE_OPEN)
    {
      GibberTransport *transport;

      DEBUG ("extra bytestream open");

      g_signal_connect (bytestream, "data-received",
          G_CALLBACK (data_received_cb), self);
      g_signal_connect (bytestream, "write-blocked",
          G_CALLBACK (bytestream_write_blocked_cb), self);

      transport = g_hash_table_lookup (priv->bytestream_to_transport,
            bytestream);
      g_assert (transport != NULL);

      add_transport (self, transport, bytestream);
    }
  else if (state == GABBLE_BYTESTREAM_STATE_CLOSED)
    {
      GibberTransport *transport;

      DEBUG ("extra bytestream closed");
      transport = g_hash_table_lookup (priv->bytestream_to_transport,
          bytestream);
      if (transport != NULL)
        {
          if (gibber_transport_buffer_is_empty (transport))
            {
              DEBUG ("Buffer is empty, we can remove the transport");
              remove_transport (self, bytestream, transport);
            }
          else
            {
              DEBUG ("Wait buffer is empty before disconnect the transport");
            }
        }
    }
}

static void
extra_bytestream_negotiate_cb (GabbleBytestreamIface *bytestream,
                               WockyStanza *msg,
                               GObject *object,
                               gpointer user_data)
{
  GabbleTubeStream *self = GABBLE_TUBE_STREAM (object);
  GabbleTubeStreamPrivate *priv = self->priv;
  GibberTransport *transport = GIBBER_TRANSPORT (user_data);

  if (bytestream == NULL)
    {
      DEBUG ("initiator refused new bytestream");

      fire_connection_closed (self, transport,
          TP_ERROR_STR_CONNECTION_REFUSED, "connection has been refused");

      g_object_unref (transport);
      return;
    }

  DEBUG ("extra bytestream accepted");

  /* transport has been refed in start_stream_initiation () */
  g_assert (gibber_transport_get_state (transport) ==
      GIBBER_TRANSPORT_CONNECTED);
  g_hash_table_insert (priv->bytestream_to_transport, g_object_ref (bytestream),
      transport);
  g_hash_table_insert (priv->transport_to_bytestream,
      g_object_ref (transport), g_object_ref (bytestream));


  g_signal_connect (bytestream, "state-changed",
                G_CALLBACK (extra_bytestream_state_changed_cb), self);
}

static gboolean
start_stream_initiation (GabbleTubeStream *self,
                         GibberTransport *transport,
                         GError **error)
{
  GabbleTubeStreamPrivate *priv = self->priv;
  TpBaseChannel *base = TP_BASE_CHANNEL (self);
  TpBaseChannelClass *cls = TP_BASE_CHANNEL_GET_CLASS (base);
  TpBaseConnection *base_conn = tp_base_channel_get_connection (base);
  GabbleConnection *conn = GABBLE_CONNECTION (base_conn);
  TpHandle initiator = tp_base_channel_get_initiator (base);
  WockyNode *node, *si_node;
  WockyStanza *msg;
  TpHandleRepoIface *contact_repo;
  const gchar *jid;
  gchar *full_jid, *stream_id, *id_str;

  contact_repo = tp_base_connection_get_handles (
     base_conn, TP_HANDLE_TYPE_CONTACT);

  jid = tp_handle_inspect (contact_repo, initiator);

  if (cls->target_handle_type == TP_HANDLE_TYPE_CONTACT)
    {
      /* Private tube */
      GabblePresence *presence;
      const gchar *resource;

      presence = gabble_presence_cache_get (conn->presence_cache,
          initiator);
      if (presence == NULL)
        {
          DEBUG ("can't find initiator's presence");
          g_set_error (error, TP_ERROR, TP_ERROR_NOT_AVAILABLE,
              "can't find initiator's presence");
          return FALSE;
        }

      resource = gabble_presence_pick_resource_by_caps (presence, 0,
          gabble_capability_set_predicate_has, NS_TUBES);
      if (resource == NULL)
        {
          DEBUG ("initiator doesn't have tubes capabilities");
          g_set_error (error, TP_ERROR, TP_ERROR_NOT_AVAILABLE,
              "initiator doesn't have tubes capabilities");
          return FALSE;
        }

        full_jid = g_strdup_printf ("%s/%s", jid, resource);
    }
  else
    {
      /* Muc tube */
      full_jid = g_strdup (jid);
    }

  stream_id = gabble_bytestream_factory_generate_stream_id ();

  msg = gabble_bytestream_factory_make_stream_init_iq (full_jid,
      stream_id, NS_TUBES);

  si_node = wocky_node_get_child_ns (
      wocky_stanza_get_top_node (msg), "si", NS_SI);
  g_assert (si_node != NULL);

  id_str = g_strdup_printf ("%" G_GUINT64_FORMAT, priv->id);

  if (cls->target_handle_type == TP_HANDLE_TYPE_CONTACT)
    {
      node = wocky_node_add_child_ns (si_node, "stream", NS_TUBES);
    }
  else
    {
      node = wocky_node_add_child_ns (si_node, "muc-stream", NS_TUBES);
    }

  wocky_node_set_attribute (node, "tube", id_str);

  gabble_bytestream_factory_negotiate_stream (
      conn->bytestream_factory, msg, stream_id,
      extra_bytestream_negotiate_cb, g_object_ref (transport), G_OBJECT (self));

  /* FIXME: data and one ref on data->transport are leaked if the tube is
   * closed before we got the SI reply. */
  g_object_unref (msg);
  g_free (stream_id);
  g_free (full_jid);
  g_free (id_str);

  return TRUE;
}

static guint
generate_connection_id (GabbleTubeStream *self,
                        GibberTransport *transport)
{
  GabbleTubeStreamPrivate *priv = self->priv;

  priv->last_connection_id++;

  g_hash_table_insert (priv->transport_to_id, transport,
      GUINT_TO_POINTER (priv->last_connection_id));

  return priv->last_connection_id;
}

static void
fire_new_local_connection (GabbleTubeStream *self,
    GibberTransport *transport)
{
  guint connection_id;

  connection_id = generate_connection_id (self, transport);

  tp_svc_channel_type_stream_tube_emit_new_local_connection (self,
      connection_id);
}

#ifdef GIBBER_TYPE_UNIX_TRANSPORT
static void
credentials_received_cb (GibberUnixTransport *transport,
                         GibberBuffer *buffer,
                         GibberCredentials *credentials,
                         GError *error,
                         gpointer user_data)
{
  GabbleTubeStream *self = GABBLE_TUBE_STREAM (user_data);

  /* Credentials received; reblock the transport */
  gibber_transport_block_receiving (GIBBER_TRANSPORT (transport), TRUE);

  if (error != NULL)
    {
      DEBUG ("Didn't receive credentials (%s). Closing transport",
          error->message);
      goto credentials_received_cb_out;
    }

  g_assert (credentials != NULL);

  if (buffer->length != 1)
    {
      DEBUG ("Got more than one byte (%" G_GSIZE_FORMAT "). Rejecting",
          buffer->length);
      goto credentials_received_cb_out;
    }

  if (credentials->uid != getuid ())
    {
      DEBUG ("Wrong uid (%u). Rejecting", credentials->uid);
      goto credentials_received_cb_out;
    }

  DEBUG ("Connection properly authentificated");

  if (!start_stream_initiation (self, GIBBER_TRANSPORT (transport), NULL))
    {
      DEBUG ("SI failed. Closing connection");
    }
  else
    {
      fire_new_local_connection (self, GIBBER_TRANSPORT (transport));
    }

credentials_received_cb_out:
  /* start_stream_initiation reffed the transport if everything went fine */
  g_object_unref (transport);
}
#endif

static gboolean
check_incoming_connection (GabbleTubeStream *self,
                           GibberTransport *transport)
{
  GabbleTubeStreamPrivate *priv = self->priv;

  if (priv->access_control == TP_SOCKET_ACCESS_CONTROL_LOCALHOST)
    {
      return TRUE;
    }
#ifdef GIBBER_TYPE_UNIX_TRANSPORT
  else if (priv->access_control == TP_SOCKET_ACCESS_CONTROL_CREDENTIALS)
    {
      if (!gibber_unix_transport_recv_credentials (
            GIBBER_UNIX_TRANSPORT (transport), credentials_received_cb, self))
        {
          DEBUG ("Can't receive credentials. Closing transport");
          return FALSE;
        }

      /* Temporarly unblock the transport to be able to receive credentials */
      gibber_transport_block_receiving (transport, FALSE);

      /* We ref the transport so it won't be destroyed by GibberListener */
      g_object_ref (transport);

      /* Returns FALSE as we are waiting for credentials so SI can't be
       * started yet. */
      return FALSE;
    }
#endif
  else if (priv->access_control == TP_SOCKET_ACCESS_CONTROL_PORT)
    {
      struct sockaddr_storage addr;
      socklen_t len = sizeof (addr);
      int ret;
      char peer_host[NI_MAXHOST];
      char peer_port[NI_MAXSERV];
      guint port;
      gchar *host;
      gchar *tmp;

      if (!gibber_transport_get_peeraddr (transport, &addr, &len))
        {
          DEBUG ("gibber_transport_get_peeraddr failed");
          return FALSE;
        }

      gibber_normalize_address (&addr);

      g_assert (addr.ss_family == AF_INET || addr.ss_family == AF_INET6);

      ret = getnameinfo ((struct sockaddr *) &addr, len,
          peer_host, NI_MAXHOST, peer_port, NI_MAXSERV,
          NI_NUMERICHOST | NI_NUMERICSERV);

      if (ret != 0)
        {
          DEBUG ("getnameinfo failed: %s", gai_strerror(ret));
          return FALSE;
        }

      dbus_g_type_struct_get (priv->access_control_param,
          0, &host,
          1, &port,
          G_MAXUINT);

      if (tp_strdiff (host, peer_host))
        {
          DEBUG ("Wrong ip: %s (%s was expected)", peer_host, host);
          g_free (host);
          return FALSE;
        }
      g_free (host);

      tmp = g_strdup_printf ("%u", port);
      if (tp_strdiff (tmp, peer_port))
        {
          DEBUG ("Wrong port: %s (%u was expected)", peer_port, port);
          g_free (tmp);
          return FALSE;
        }
      g_free (tmp);

      return TRUE;
    }
  else
    {
      /* access_control has already been checked when accepting the tube */
      g_assert_not_reached ();
    }

  return FALSE;
}

/* callback for listening connections from the local application */
static void
local_new_connection_cb (GibberListener *listener,
                         GibberTransport *transport,
                         struct sockaddr_storage *addr,
                         guint size,
                         gpointer user_data)
{
  GabbleTubeStream *self = GABBLE_TUBE_STREAM (user_data);

  /* Block the transport while there is no open bytestream to transfer
   * its data. */
  gibber_transport_block_receiving (transport, TRUE);

  if (!check_incoming_connection (self, transport))
    {
      /* We didn't ref the connection so it will be destroyed by the
       * GibberListener if needed. */
      return;
    }

  /* Streams in stream tubes are established with stream initiation (XEP-0095).
   * We use SalutSiBytestreamManager.
   */
  if (!start_stream_initiation (self, transport, NULL))
    {
      DEBUG ("closing new client connection");
    }
  else
    {
      fire_new_local_connection (self, transport);
    }
}

#ifdef GIBBER_TYPE_UNIX_TRANSPORT
static gboolean
set_credentials_access_control_param (GValue *access_control_param,
    GibberTransport *transport)
{
  guint8 credentials;

  credentials = g_random_int_range (0, G_MAXUINT8);

  /* The Credentials access control would have be rejected earlier if the
   * socket type wasn't UNIX. */
  if (!gibber_unix_transport_send_credentials (
        GIBBER_UNIX_TRANSPORT (transport), &credentials, sizeof (guint8)))
    {
      DEBUG ("send_credentials failed");
      return FALSE;
    }

  g_value_init (access_control_param,
      G_TYPE_UCHAR);
  g_value_set_uchar (access_control_param, credentials);

  return TRUE;
}
#endif

static gboolean
set_port_access_control_param (GValue *access_control_param,
    GibberTransport *transport)
{
  struct sockaddr_storage addr;
  socklen_t addrlen = sizeof (struct sockaddr_storage);
  char host[NI_MAXHOST];
  char port_str[NI_MAXSERV];
  int ret;
  guint16 port;
  unsigned long tmp;
  gchar *endptr;

  if (!gibber_transport_get_sockaddr (transport, &addr, &addrlen))
    {
      DEBUG ("Failed to get connection address");
      return FALSE;
    }

  ret = getnameinfo ((struct sockaddr *) &addr, addrlen,
      host, NI_MAXHOST, port_str, NI_MAXSERV,
      NI_NUMERICHOST | NI_NUMERICSERV);
  if (ret != 0)
    {
      DEBUG ("getnameinfo failed: %s", g_strerror (ret));
      return FALSE;
    }

  tmp = strtoul (port_str, &endptr, 10);
  if (!endptr || *endptr || tmp > G_MAXUINT16)
    {
      DEBUG ("invalid port: %s", port_str);
      return FALSE;
    }
  port = (guint16) tmp;

  g_value_init (access_control_param,
      TP_STRUCT_TYPE_SOCKET_ADDRESS_IPV4);
  g_value_take_boxed (access_control_param,
      dbus_g_type_specialized_construct (
                TP_STRUCT_TYPE_SOCKET_ADDRESS_IPV4));

  dbus_g_type_struct_set (access_control_param,
      0, host,
      1, port,
      G_MAXUINT);

  return TRUE;
}

static transport_connected_data *
transport_connected_data_new (GabbleTubeStream *self,
    TpHandle contact)
{
  transport_connected_data *data = g_slice_new (transport_connected_data);
  data->self = self;
  data->contact = contact;
  return data;
}

static void
transport_connected_data_free (transport_connected_data *data)
{
  g_slice_free (transport_connected_data, data);
}

static void
fire_new_remote_connection (GabbleTubeStream *self,
    GibberTransport *transport,
    TpHandle contact)
{
  GabbleTubeStreamPrivate *priv = self->priv;
  GValue access_control_param = {0,};
  guint connection_id;

#ifdef GIBBER_TYPE_UNIX_TRANSPORT
  if (priv->access_control == TP_SOCKET_ACCESS_CONTROL_CREDENTIALS)
    {
      if (!set_credentials_access_control_param (&access_control_param,
            transport))
        {
          gibber_transport_disconnect (transport);
          return;
        }
    }
  else
#endif
  if (priv->access_control == TP_SOCKET_ACCESS_CONTROL_PORT)
    {
      if (!set_port_access_control_param (&access_control_param, transport))
        {
          gibber_transport_disconnect (transport);
          return;
        }
    }
  else
    {
      /* set a dummy value */
      g_value_init (&access_control_param, G_TYPE_INT);
      g_value_set_int (&access_control_param, 0);
    }

  /* fire NewConnection D-Bus signal */

  connection_id = GPOINTER_TO_UINT (g_hash_table_lookup (priv->transport_to_id,
        transport));
  g_assert (connection_id != 0);

  tp_svc_channel_type_stream_tube_emit_new_remote_connection (self,
      contact, &access_control_param, connection_id);
  g_value_unset (&access_control_param);
}

static void
transport_connected_cb (GibberTransport *transport,
    transport_connected_data *data)
{
  GabbleTubeStreamPrivate *priv = data->self->priv;
  GabbleBytestreamIface *bytestream;

  fire_new_remote_connection (data->self, transport, data->contact);

  bytestream = g_hash_table_lookup (priv->transport_to_bytestream, transport);
  if (bytestream == NULL)
    return;

  gabble_bytestream_iface_block_reading (bytestream, FALSE);
}

static GibberTransport *
new_connection_to_socket (GabbleTubeStream *self,
                          GabbleBytestreamIface *bytestream,
                          TpHandle contact)
{
  GabbleTubeStreamPrivate *priv = self->priv;
  TpBaseChannel *base = TP_BASE_CHANNEL (self);
  GibberTransport *transport;

  DEBUG ("Called.");

  g_assert (tp_base_channel_is_requested (base));

#ifdef GIBBER_TYPE_UNIX_TRANSPORT
  if (priv->address_type == TP_SOCKET_ADDRESS_TYPE_UNIX)
    {
      GArray *array;
      array = g_value_get_boxed (priv->address);
      DEBUG ("Will try to connect to socket: %s", (const gchar *) array->data);

      transport = GIBBER_TRANSPORT (gibber_unix_transport_new ());
      gibber_unix_transport_connect (GIBBER_UNIX_TRANSPORT (transport),
          array->data, NULL);
    }
  else
#endif
  if (priv->address_type == TP_SOCKET_ADDRESS_TYPE_IPV4 ||
      priv->address_type == TP_SOCKET_ADDRESS_TYPE_IPV6)
    {
      gchar *ip;
      guint port;

      dbus_g_type_struct_get (priv->address,
          0, &ip,
          1, &port,
          G_MAXUINT);

      transport = GIBBER_TRANSPORT (gibber_tcp_transport_new ());
      gibber_tcp_transport_connect (GIBBER_TCP_TRANSPORT (transport), ip,
          port);

      g_free (ip);
    }
  else
    {
      g_assert_not_reached ();
    }

  /* Block the transport while there is no open bytestream to transfer
   * its data. */
  gibber_transport_block_receiving (transport, TRUE);

  generate_connection_id (self, transport);

  gabble_bytestream_iface_block_reading (bytestream, TRUE);
  g_hash_table_insert (priv->bytestream_to_transport, g_object_ref (bytestream),
      g_object_ref (transport));
  g_hash_table_insert (priv->transport_to_bytestream,
      g_object_ref (transport), g_object_ref (bytestream));


  g_signal_connect (bytestream, "state-changed",
      G_CALLBACK (extra_bytestream_state_changed_cb), self);

  g_object_unref (transport);

  return transport;
}

static gboolean
tube_stream_open (GabbleTubeStream *self,
                  GError **error)
{
  GabbleTubeStreamPrivate *priv = self->priv;
  TpBaseChannel *base = TP_BASE_CHANNEL (self);

  DEBUG ("called");

  if (tp_base_channel_is_requested (base))
    /* Nothing to do if we are the initiator of this tube.
     * We'll connect to the socket each time request a new bytestream. */
    return TRUE;

  /* We didn't create this tube so it doesn't have
   * a socket associated with it. Let's create one */
  g_assert (priv->address == NULL);
  g_assert (priv->local_listener == NULL);
  priv->local_listener = gibber_listener_new ();

  g_signal_connect (priv->local_listener, "new-connection",
      G_CALLBACK (local_new_connection_cb), self);

#ifdef GIBBER_TYPE_UNIX_TRANSPORT
  if (priv->address_type == TP_SOCKET_ADDRESS_TYPE_UNIX)
    {
      GArray *array;
      gchar suffix[8];
      gchar *path;
      int ret;

      generate_ascii_string (8, suffix);
      path = g_strdup_printf ("/tmp/stream-gabble-%.8s", suffix);

      DEBUG ("create socket: %s", path);

      array = g_array_sized_new (TRUE, FALSE, sizeof (gchar), strlen (path));
      g_array_insert_vals (array, 0, path, strlen (path));

      priv->address = tp_g_value_slice_new (DBUS_TYPE_G_UCHAR_ARRAY);
      g_value_set_boxed (priv->address, array);

      g_array_unref (array);

      ret = gibber_listener_listen_socket (priv->local_listener, path, FALSE,
          error);
      if (ret != TRUE)
        {
          g_assert (error != NULL && *error != NULL);
          DEBUG ("Error listening on socket %s: %s", path, (*error)->message);
          g_free (path);
          return FALSE;
        }

      if (priv->access_control == TP_SOCKET_ACCESS_CONTROL_LOCALHOST)
        {
          /* Everyone can use the socket */
          chmod (path, 0777);
        }

      g_free (path);
    }
  else
#endif
  if (priv->address_type == TP_SOCKET_ADDRESS_TYPE_IPV4)
    {
      int ret;

      ret = gibber_listener_listen_tcp_loopback_af (priv->local_listener, 0,
          GIBBER_AF_IPV4, error);
      if (!ret)
        {
          g_assert (error != NULL && *error != NULL);
          DEBUG ("Error listening on socket: %s", (*error)->message);
          return FALSE;
        }

      priv->address = tp_g_value_slice_new (
          TP_STRUCT_TYPE_SOCKET_ADDRESS_IPV4);
      g_value_take_boxed (priv->address,
          dbus_g_type_specialized_construct (
              TP_STRUCT_TYPE_SOCKET_ADDRESS_IPV4));

      dbus_g_type_struct_set (priv->address,
          0, "127.0.0.1",
          1, gibber_listener_get_port (priv->local_listener),
          G_MAXUINT);
    }
  else if (priv->address_type == TP_SOCKET_ADDRESS_TYPE_IPV6)
    {
      int ret;

      ret = gibber_listener_listen_tcp_loopback_af (priv->local_listener, 0,
          GIBBER_AF_IPV6, error);
      if (!ret)
        {
          g_assert (error != NULL && *error != NULL);
          DEBUG ("Error listening on socket: %s", (*error)->message);
          return FALSE;
        }

      priv->address = tp_g_value_slice_new (
          TP_STRUCT_TYPE_SOCKET_ADDRESS_IPV6);
      g_value_take_boxed (priv->address,
          dbus_g_type_specialized_construct (
            TP_STRUCT_TYPE_SOCKET_ADDRESS_IPV6));

      dbus_g_type_struct_set (priv->address,
          0, "::1",
          1, gibber_listener_get_port (priv->local_listener),
          G_MAXUINT);
    }
  else
    {
      g_assert_not_reached ();
    }

  return TRUE;
}

static void
gabble_tube_stream_init (GabbleTubeStream *self)
{
  GabbleTubeStreamPrivate *priv = G_TYPE_INSTANCE_GET_PRIVATE (self,
      GABBLE_TYPE_TUBE_STREAM, GabbleTubeStreamPrivate);

  self->priv = priv;

  priv->transport_to_bytestream = g_hash_table_new_full (g_direct_hash,
      g_direct_equal, (GDestroyNotify) g_object_unref,
      (GDestroyNotify) g_object_unref);

  priv->bytestream_to_transport = g_hash_table_new_full (g_direct_hash,
      g_direct_equal, (GDestroyNotify) g_object_unref,
      (GDestroyNotify) g_object_unref);

  priv->transport_to_id = g_hash_table_new_full (g_direct_hash,
      g_direct_equal, NULL, NULL);
  priv->last_connection_id = 0;

  priv->address_type = TP_SOCKET_ADDRESS_TYPE_UNIX;
  priv->address = NULL;
  priv->access_control = TP_SOCKET_ACCESS_CONTROL_LOCALHOST;
  priv->access_control_param = NULL;

  priv->dispose_has_run = FALSE;
}

static gboolean
close_each_extra_bytestream (gpointer key,
                             gpointer value,
                             gpointer user_data)
{
  GabbleTubeStream *self = GABBLE_TUBE_STREAM (user_data);
  GabbleTubeStreamPrivate *priv = self->priv;
  GibberTransport *transport = (GibberTransport *) value;
  GabbleBytestreamIface *bytestream = (GabbleBytestreamIface *) key;

  /* We are iterating over priv->fd_to_bytestreams so we can't modify it.
   * Disconnect signals so extra_bytestream_state_changed_cb won't be
   * called */
  g_signal_handlers_disconnect_matched (bytestream, G_SIGNAL_MATCH_DATA,
      0, 0, NULL, NULL, self);
  g_signal_handlers_disconnect_matched (transport, G_SIGNAL_MATCH_DATA,
      0, 0, NULL, NULL, self);
  /* The callback on the "connected" signal doesn't match the above
   * disconnection as it receives a transport_connected_data as user_data
   * and not the self pointer. */
  g_signal_handlers_disconnect_matched (transport, G_SIGNAL_MATCH_FUNC,
      0, 0, NULL, G_CALLBACK (transport_connected_cb), NULL);

  gabble_bytestream_iface_close (bytestream, NULL);
  gibber_transport_disconnect (transport);
  fire_connection_closed (self, transport, TP_ERROR_STR_CANCELLED,
      "tube is closing");

  g_hash_table_remove (priv->transport_to_bytestream, transport);

  return TRUE;
}

static void
gabble_tube_stream_dispose (GObject *object)
{
  GabbleTubeStream *self = GABBLE_TUBE_STREAM (object);
  GabbleTubeStreamPrivate *priv = self->priv;
  TpBaseChannel *base = (TpBaseChannel *) self;

  if (priv->dispose_has_run)
    return;

  gabble_tube_iface_close (GABBLE_TUBE_IFACE (self), TRUE);

  if (tp_base_channel_is_requested (base) &&
      priv->address_type == TP_SOCKET_ADDRESS_TYPE_UNIX &&
      priv->address != NULL)
    {
      /* We created a new UNIX socket. Let's delete it */
      GArray *array;
      GString *path;

      array = g_value_get_boxed (priv->address);
      path = g_string_new_len (array->data, array->len);

      if (g_unlink (path->str) != 0)
        {
          DEBUG ("unlink of %s failed: %s", path->str, g_strerror (errno));
        }

      g_string_free (path, TRUE);
    }

  tp_clear_pointer (&priv->transport_to_bytestream, g_hash_table_unref);
  tp_clear_pointer (&priv->bytestream_to_transport, g_hash_table_unref);
  tp_clear_pointer (&priv->transport_to_id, g_hash_table_unref);

  tp_clear_object (&priv->local_listener);

  if (priv->muc != NULL)
    {
      tp_external_group_mixin_finalize (object);
    }

  priv->dispose_has_run = TRUE;

  if (G_OBJECT_CLASS (gabble_tube_stream_parent_class)->dispose)
    G_OBJECT_CLASS (gabble_tube_stream_parent_class)->dispose (object);
}

static void
gabble_tube_stream_finalize (GObject *object)
{
  GabbleTubeStream *self = GABBLE_TUBE_STREAM (object);
  GabbleTubeStreamPrivate *priv = self->priv;

  g_free (priv->service);
  g_hash_table_unref (priv->parameters);

  if (priv->address != NULL)
    {
      tp_g_value_slice_free (priv->address);
      priv->address = NULL;
    }

  if (priv->access_control_param != NULL)
    {
      tp_g_value_slice_free (priv->access_control_param);
      priv->access_control_param = NULL;
    }

  G_OBJECT_CLASS (gabble_tube_stream_parent_class)->finalize (object);
}

static void
gabble_tube_stream_get_property (GObject *object,
                               guint property_id,
                               GValue *value,
                               GParamSpec *pspec)
{
  GabbleTubeStream *self = GABBLE_TUBE_STREAM (object);
  GabbleTubeStreamPrivate *priv = self->priv;

  switch (property_id)
    {
      case PROP_SELF_HANDLE:
        g_value_set_uint (value, priv->self_handle);
        break;
      case PROP_ID:
        g_value_set_uint64 (value, priv->id);
        break;
      case PROP_TYPE:
        g_value_set_uint (value, TP_TUBE_TYPE_STREAM);
        break;
      case PROP_SERVICE:
        g_value_set_string (value, priv->service);
        break;
      case PROP_PARAMETERS:
        g_value_set_boxed (value, priv->parameters);
        break;
      case PROP_STATE:
        g_value_set_uint (value, priv->state);
        break;
      case PROP_ADDRESS_TYPE:
        g_value_set_uint (value, priv->address_type);
        break;
      case PROP_ADDRESS:
        g_value_set_pointer (value, priv->address);
        break;
      case PROP_ACCESS_CONTROL:
        g_value_set_uint (value, priv->access_control);
        break;
      case PROP_ACCESS_CONTROL_PARAM:
        g_value_set_pointer (value, priv->access_control_param);
        break;
      case PROP_SUPPORTED_SOCKET_TYPES:
        g_value_take_boxed (value,
            gabble_tube_stream_get_supported_socket_types ());
        break;
      case PROP_MUC:
        g_value_set_object (value, priv->muc);
        break;
      default:
        G_OBJECT_WARN_INVALID_PROPERTY_ID (object, property_id, pspec);
        break;
    }
}

static void
gabble_tube_stream_set_property (GObject *object,
                                 guint property_id,
                                 const GValue *value,
                                 GParamSpec *pspec)
{
  GabbleTubeStream *self = GABBLE_TUBE_STREAM (object);
  GabbleTubeStreamPrivate *priv = self->priv;

  switch (property_id)
    {
      case PROP_SELF_HANDLE:
        priv->self_handle = g_value_get_uint (value);
        break;
      case PROP_ID:
        priv->id = g_value_get_uint64 (value);
        break;
      case PROP_SERVICE:
        g_free (priv->service);
        priv->service = g_value_dup_string (value);
        break;
      case PROP_PARAMETERS:
        if (priv->parameters != NULL)
          g_hash_table_unref (priv->parameters);
        priv->parameters = g_value_dup_boxed (value);
        break;
      case PROP_ADDRESS_TYPE:
        g_assert (g_value_get_uint (value) == TP_SOCKET_ADDRESS_TYPE_UNIX ||
            g_value_get_uint (value) == TP_SOCKET_ADDRESS_TYPE_IPV4 ||
            g_value_get_uint (value) == TP_SOCKET_ADDRESS_TYPE_IPV6);
        priv->address_type = g_value_get_uint (value);
        break;
      case PROP_ADDRESS:
        if (priv->address == NULL)
          {
            priv->address = tp_g_value_slice_dup (g_value_get_pointer (value));
          }
        break;
      case PROP_ACCESS_CONTROL:
        priv->access_control = g_value_get_uint (value);
        break;
      case PROP_ACCESS_CONTROL_PARAM:
        if (priv->access_control_param == NULL)
          {
            priv->access_control_param = tp_g_value_slice_dup (
                g_value_get_pointer (value));
          }
        break;
      case PROP_MUC:
        priv->muc = g_value_get_object (value);
        break;
      default:
        G_OBJECT_WARN_INVALID_PROPERTY_ID (object, property_id, pspec);
        break;
    }
}

static void
gabble_tube_stream_constructed (GObject *obj)
{
  GabbleTubeStream *self = GABBLE_TUBE_STREAM (obj);
  GabbleTubeStreamPrivate *priv = self->priv;
  TpBaseChannel *base = TP_BASE_CHANNEL (self);
  TpBaseChannelClass *cls = TP_BASE_CHANNEL_GET_CLASS (base);
  TpBaseConnection *base_conn = tp_base_channel_get_connection (base);
  GabbleConnection *conn = GABBLE_CONNECTION (base_conn);

  void (*chain_up) (GObject *) =
    ((GObjectClass *) gabble_tube_stream_parent_class)->constructed;

  if (chain_up != NULL)
    chain_up (obj);

  if (tp_base_channel_is_requested (base))
    {
      /* We initiated this tube */
      priv->state = TP_TUBE_CHANNEL_STATE_NOT_OFFERED;
    }
  else
    {
      priv->state = TP_TUBE_CHANNEL_STATE_LOCAL_PENDING;

      /* We'll need SOCKS5 proxies if the tube is accepted */
      gabble_bytestream_factory_query_socks5_proxies (
          conn->bytestream_factory);
    }

  if (cls->target_handle_type == TP_HANDLE_TYPE_CONTACT)
    {
      g_assert (priv->muc == NULL);
    }
  else
    {
      g_assert (priv->muc != NULL);
      tp_external_group_mixin_init (obj, (GObject *) priv->muc);
    }
}

static void
gabble_tube_stream_fill_immutable_properties (TpBaseChannel *chan,
    GHashTable *properties)
{
  TpBaseChannelClass *cls = TP_BASE_CHANNEL_CLASS (
      gabble_tube_stream_parent_class);

  cls->fill_immutable_properties (chan, properties);

  tp_dbus_properties_mixin_fill_properties_hash (
      G_OBJECT (chan), properties,
      TP_IFACE_CHANNEL_TYPE_STREAM_TUBE, "Service",
      TP_IFACE_CHANNEL_TYPE_STREAM_TUBE, "SupportedSocketTypes",
      NULL);

  if (!tp_base_channel_is_requested (chan))
    {
      tp_dbus_properties_mixin_fill_properties_hash (
          G_OBJECT (chan), properties,
          TP_IFACE_CHANNEL_INTERFACE_TUBE, "Parameters",
          NULL);
    }
}

static gchar *
gabble_tube_stream_get_object_path_suffix (TpBaseChannel *base)
{
  GabbleTubeStream *self = GABBLE_TUBE_STREAM (base);

  return g_strdup_printf ("StreamTubeChannel/%u/%" G_GUINT64_FORMAT,
      tp_base_channel_get_target_handle (base),
      self->priv->id);
}

static void
gabble_tube_stream_close (TpBaseChannel *base)
{
  gabble_tube_iface_close (GABBLE_TUBE_IFACE (base), FALSE);
}

static void
gabble_tube_stream_class_init (GabbleTubeStreamClass *gabble_tube_stream_class)
{
  static TpDBusPropertiesMixinPropImpl stream_tube_props[] = {
      { "Service", "service", NULL },
      { "SupportedSocketTypes", "supported-socket-types", NULL },
      { NULL }
  };
  static TpDBusPropertiesMixinPropImpl tube_iface_props[] = {
      { "Parameters", "parameters", NULL },
      { "State", "state", NULL },
      { NULL }
  };
  static TpDBusPropertiesMixinIfaceImpl prop_interfaces[] = {
      { TP_IFACE_CHANNEL_TYPE_STREAM_TUBE,
        tp_dbus_properties_mixin_getter_gobject_properties,
        NULL,
        stream_tube_props,
      },
      { TP_IFACE_CHANNEL_INTERFACE_TUBE,
        tp_dbus_properties_mixin_getter_gobject_properties,
        NULL,
        tube_iface_props,
      },
      { NULL }
  };
  GObjectClass *object_class = G_OBJECT_CLASS (gabble_tube_stream_class);
  TpBaseChannelClass *base_class = TP_BASE_CHANNEL_CLASS (gabble_tube_stream_class);
  GParamSpec *param_spec;

  object_class->get_property = gabble_tube_stream_get_property;
  object_class->set_property = gabble_tube_stream_set_property;
  object_class->constructed = gabble_tube_stream_constructed;
  object_class->dispose = gabble_tube_stream_dispose;
  object_class->finalize = gabble_tube_stream_finalize;

  base_class->channel_type = TP_IFACE_CHANNEL_TYPE_STREAM_TUBE;
  base_class->get_interfaces = gabble_tube_stream_get_interfaces;
  base_class->target_handle_type = TP_HANDLE_TYPE_CONTACT;
  base_class->close = gabble_tube_stream_close;
  base_class->fill_immutable_properties =
    gabble_tube_stream_fill_immutable_properties;
  base_class->get_object_path_suffix =
    gabble_tube_stream_get_object_path_suffix;

  g_type_class_add_private (gabble_tube_stream_class,
      sizeof (GabbleTubeStreamPrivate));

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

  param_spec = g_param_spec_boxed (
      "supported-socket-types",
      "Supported socket types",
      "GHashTable containing supported socket types.",
      TP_HASH_TYPE_SUPPORTED_SOCKET_MAP,
      G_PARAM_READABLE | G_PARAM_STATIC_STRINGS);
  g_object_class_install_property (object_class, PROP_SUPPORTED_SOCKET_TYPES,
      param_spec);

  param_spec = g_param_spec_uint (
      "address-type",
      "address type",
      "a TpSocketAddressType representing the type of the listening"
      "address of the local service",
      0, NUM_TP_SOCKET_ADDRESS_TYPES - 1,
      TP_SOCKET_ADDRESS_TYPE_UNIX,
      G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS);
  g_object_class_install_property (object_class, PROP_ADDRESS_TYPE,
      param_spec);

  param_spec = g_param_spec_pointer (
      "address",
      "address",
      "The listening address of the local service, as indicated by the "
      "address-type",
      G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS);
  g_object_class_install_property (object_class, PROP_ADDRESS, param_spec);

  param_spec = g_param_spec_uint (
      "access-control",
      "access control",
      "a TpSocketAccessControl representing the access control "
      "the local service applies to the local socket",
      0, NUM_TP_SOCKET_ACCESS_CONTROLS - 1,
      TP_SOCKET_ACCESS_CONTROL_LOCALHOST,
      G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS);
  g_object_class_install_property (object_class, PROP_ACCESS_CONTROL,
      param_spec);

  param_spec = g_param_spec_pointer (
      "access-control-param",
      "access control param",
      "A parameter for the access control type, to be interpreted as specified"
      "in the documentation for the Socket_Access_Control enum.",
      G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS);
  g_object_class_install_property (object_class, PROP_ACCESS_CONTROL_PARAM,
      param_spec);

  param_spec = g_param_spec_object (
      "muc",
      "GabbleMucChannel object",
      "Gabble text MUC channel corresponding to this Tube channel object, "
      "if the handle type is ROOM.",
      GABBLE_TYPE_MUC_CHANNEL,
      G_PARAM_CONSTRUCT_ONLY | G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS);
  g_object_class_install_property (object_class, PROP_MUC, param_spec);

  signals[OPENED] =
    g_signal_new ("tube-opened",
                  G_OBJECT_CLASS_TYPE (gabble_tube_stream_class),
                  G_SIGNAL_RUN_LAST | G_SIGNAL_DETAILED,
                  0,
                  NULL, NULL,
                  g_cclosure_marshal_VOID__VOID,
                  G_TYPE_NONE, 0);

  signals[NEW_CONNECTION] =
    g_signal_new ("tube-new-connection",
                  G_OBJECT_CLASS_TYPE (gabble_tube_stream_class),
                  G_SIGNAL_RUN_LAST | G_SIGNAL_DETAILED,
                  0,
                  NULL, NULL,
                  g_cclosure_marshal_VOID__UINT,
                  G_TYPE_NONE, 1, G_TYPE_UINT);

  signals[CLOSED] =
    g_signal_new ("tube-closed",
                  G_OBJECT_CLASS_TYPE (gabble_tube_stream_class),
                  G_SIGNAL_RUN_LAST | G_SIGNAL_DETAILED,
                  0,
                  NULL, NULL,
                  g_cclosure_marshal_VOID__VOID,
                  G_TYPE_NONE, 0);

  signals[OFFERED] =
    g_signal_new ("tube-offered",
                  G_OBJECT_CLASS_TYPE (gabble_tube_stream_class),
                  G_SIGNAL_RUN_LAST | G_SIGNAL_DETAILED,
                  0,
                  NULL, NULL,
                  g_cclosure_marshal_VOID__VOID,
                  G_TYPE_NONE, 0);

  gabble_tube_stream_class->dbus_props_class.interfaces = prop_interfaces;
  tp_dbus_properties_mixin_class_init (object_class,
      G_STRUCT_OFFSET (GabbleTubeStreamClass, dbus_props_class));

  tp_external_group_mixin_init_dbus_properties (object_class);
}

static void
data_received_cb (GabbleBytestreamIface *bytestream,
                  TpHandle sender,
                  GString *data,
                  gpointer user_data)
{
  GabbleTubeStream *tube = GABBLE_TUBE_STREAM (user_data);
  GabbleTubeStreamPrivate *priv = tube->priv;
  GibberTransport *transport;
  GError *error = NULL;

  transport = g_hash_table_lookup (priv->bytestream_to_transport, bytestream);
  g_assert (transport != NULL);

  /* If something goes wrong when trying to write the data on the transport,
   * it could be disconnected, causing its removal from the hash tables.
   * When removed, the transport would be destroyed as the hash tables keep a
   * ref on it and so we'll call _buffer_is_empty on a destroyed transport.
   * We avoid that by reffing the transport between the 2 calls so we keep it
   * artificially alive if needed. */
  g_object_ref (transport);
  if (!gibber_transport_send (transport, (const guint8 *) data->str, data->len,
      &error))
  {
    DEBUG ("sending failed: %s", error->message);
    g_error_free (error);
    g_object_unref (transport);
    return;
  }

  if (!gibber_transport_buffer_is_empty (transport))
    {
      /* We don't want to send more data while the buffer isn't empty */
      DEBUG ("tube buffer isn't empty. Block the bytestream");
      gabble_bytestream_iface_block_reading (bytestream, TRUE);
    }
  g_object_unref (transport);
}

GabbleTubeStream *
gabble_tube_stream_new (GabbleConnection *conn,
                        TpHandle handle,
                        TpHandleType handle_type,
                        TpHandle self_handle,
                        TpHandle initiator,
                        const gchar *service,
                        GHashTable *parameters,
                        guint64 id,
                        GabbleMucChannel *muc,
                        gboolean requested)
{
  GabbleTubeStream *obj;
  GType gtype = GABBLE_TYPE_TUBE_STREAM;

  if (handle_type == TP_HANDLE_TYPE_ROOM)
    gtype = GABBLE_TYPE_MUC_TUBE_STREAM;

  obj = g_object_new (gtype,
      "connection", conn,
      "handle", handle,
      "self-handle", self_handle,
      "initiator-handle", initiator,
      "service", service,
      "parameters", parameters,
      "id", id,
      "muc", muc,
      "requested", requested,
      NULL);

  return obj;
}

/**
 * gabble_tube_stream_accept
 *
 * Implements gabble_tube_iface_accept on GabbleTubeIface
 */
static gboolean
gabble_tube_stream_accept (GabbleTubeIface *tube,
                           GError **error)
{
  GabbleTubeStream *self = GABBLE_TUBE_STREAM (tube);
  GabbleTubeStreamPrivate *priv = self->priv;

  if (!gabble_tube_stream_check_params (priv->address_type, NULL,
        priv->access_control, priv->access_control_param, error))
    {
      goto fail;
    }

  if (priv->state != TP_TUBE_CHANNEL_STATE_LOCAL_PENDING)
    {
      g_set_error (error, TP_ERROR, TP_ERROR_INVALID_ARGUMENT,
          "Tube is not in the local pending state");
      goto fail;
    }

  if (!tube_stream_open (self, error))
    {
      gabble_tube_iface_close (GABBLE_TUBE_IFACE (self), TRUE);
      goto fail;
    }

  priv->state = TP_TUBE_CHANNEL_STATE_OPEN;

  tp_svc_channel_interface_tube_emit_tube_channel_state_changed (self,
      TP_TUBE_CHANNEL_STATE_OPEN);

  g_signal_emit (G_OBJECT (self), signals[OPENED], 0);

  return TRUE;

fail:
  priv->address_type = 0;
  priv->access_control = 0;
  tp_g_value_slice_free (priv->access_control_param);
  priv->access_control_param = NULL;
  return FALSE;
}

/**
 * gabble_tube_iface_stream_close
 *
 * Implements gabble_tube_iface_close on GabbleTubeIface
 */
static void
gabble_tube_iface_stream_close (GabbleTubeIface *tube,
    gboolean closed_remotely)
{
  GabbleTubeStream *self = GABBLE_TUBE_STREAM (tube);
  GabbleTubeStreamPrivate *priv = self->priv;
  TpBaseChannel *base = TP_BASE_CHANNEL (self);
  TpBaseChannelClass *cls = TP_BASE_CHANNEL_GET_CLASS (base);
  TpBaseConnection *base_conn = tp_base_channel_get_connection (base);
  GabbleConnection *conn = GABBLE_CONNECTION (base_conn);

  if (tp_base_channel_is_destroyed (base))
    return;

  g_hash_table_foreach_remove (priv->bytestream_to_transport,
      close_each_extra_bytestream, self);

  if (!closed_remotely && cls->target_handle_type == TP_HANDLE_TYPE_CONTACT)
    {
      WockyStanza *msg;
      const gchar *jid;
      TpHandleRepoIface *contact_repo = tp_base_connection_get_handles (
          base_conn, TP_HANDLE_TYPE_CONTACT);
      gchar *id_str;

      jid = tp_handle_inspect (contact_repo,
          tp_base_channel_get_target_handle (base));
      id_str = g_strdup_printf ("%" G_GUINT64_FORMAT, priv->id);

      /* Send the close message */
      msg = wocky_stanza_build (WOCKY_STANZA_TYPE_MESSAGE,
          WOCKY_STANZA_SUB_TYPE_NONE,
          NULL, jid,
          '(', "close",
            ':', NS_TUBES,
            '@', "tube", id_str,
          ')',
          GABBLE_AMP_DO_NOT_STORE_SPEC,
          NULL);
      g_free (id_str);

      _gabble_connection_send (conn, msg, NULL);

      g_object_unref (msg);
    }

  /* Take a ref to ourselves as when we emit tube-closed
   * GabbleTubesChannel will drop our last ref but we still need to
   * declare ourselves as destroyed. This is rubbish, but will
   * disappear when we finally remove the Tubes channel type.. */
  g_object_ref (self);

  if (cls->target_handle_type == TP_HANDLE_TYPE_ROOM)
    gabble_muc_channel_send_presence (priv->muc);

  g_signal_emit (G_OBJECT (self), signals[CLOSED], 0);

  tp_base_channel_destroyed (base);

  g_object_unref (self);
}

static void
augment_si_accept_iq (WockyNode *si,
                      gpointer user_data)
{
  wocky_node_add_child_ns (si, "tube", NS_TUBES);
}

/**
 * gabble_tube_stream_add_bytestream
 *
 * Implements gabble_tube_iface_add_bytestream on GabbleTubeIface
 */

static void
gabble_tube_stream_add_bytestream (GabbleTubeIface *tube,
                                   GabbleBytestreamIface *bytestream)
{
  GabbleTubeStream *self = GABBLE_TUBE_STREAM (tube);
  GabbleTubeStreamPrivate *priv = self->priv;
  TpBaseChannel *base = TP_BASE_CHANNEL (self);
  TpHandle contact;
  GibberTransport *transport;

  if (!tp_base_channel_is_requested (base))
    {
      DEBUG ("I'm not the initiator of this tube, can't accept "
          "an extra bytestream");

      gabble_bytestream_iface_close (bytestream, NULL);
      return;
    }

  g_object_get (bytestream, "peer-handle", &contact, NULL);

  /* New bytestream, let's connect to the socket */
  transport = new_connection_to_socket (self, bytestream, contact);
  if (transport != NULL)
    {
      if (priv->state == TP_TUBE_CHANNEL_STATE_REMOTE_PENDING)
        {
          DEBUG ("Received first connection. Tube is now open");
          priv->state = TP_TUBE_CHANNEL_STATE_OPEN;

          tp_svc_channel_interface_tube_emit_tube_channel_state_changed (
              self, TP_TUBE_CHANNEL_STATE_OPEN);

          g_signal_emit (G_OBJECT (self), signals[OPENED], 0);
        }

      DEBUG ("accept the extra bytestream");

      gabble_bytestream_iface_accept (bytestream, augment_si_accept_iq, self);

      g_signal_emit (G_OBJECT (self), signals[NEW_CONNECTION], 0, contact);

      if (gibber_transport_get_state (transport) == GIBBER_TRANSPORT_CONNECTED)
        {
          gabble_bytestream_iface_block_reading (bytestream, FALSE);
          fire_new_remote_connection (self, transport, contact);
        }
      else
        {
          /* NewConnection will be fired once the transport is connected.
           * We can't get access_control_param (as the source port for example)
           * until it's connected. */
          transport_connected_data *data;

          data = transport_connected_data_new (self, contact);

          g_signal_connect_data (transport, "connected",
              G_CALLBACK (transport_connected_cb), data,
              (GClosureNotify) transport_connected_data_free, 0);
        }
    }
  else
    {
      gabble_bytestream_iface_close (bytestream, NULL);
    }
}

#ifdef GIBBER_TYPE_UNIX_TRANSPORT
static gboolean
check_unix_params (TpSocketAddressType address_type,
                   const GValue *address,
                   TpSocketAccessControl access_control,
                   const GValue *access_control_param,
                   GError **error)
{
  GArray *array;
  GString *socket_address;
  struct stat stat_buff;
  guint i;
  struct sockaddr_un dummy;

  g_assert (address_type == TP_SOCKET_ADDRESS_TYPE_UNIX);

  /* Check address type */
  if (address != NULL)
    {
      if (G_VALUE_TYPE (address) != DBUS_TYPE_G_UCHAR_ARRAY)
        {
          g_set_error (error, TP_ERROR, TP_ERROR_INVALID_ARGUMENT,
              "Unix socket address is supposed to be ay");
          return FALSE;
        }

      array = g_value_get_boxed (address);

      if (array->len > sizeof (dummy.sun_path) - 1)
        {
          g_set_error (error, TP_ERROR, TP_ERROR_INVALID_ARGUMENT,
              "Unix socket path is too long (max length allowed: %"
              G_GSIZE_FORMAT ")",
              sizeof (dummy.sun_path) - 1);
          return FALSE;
        }

      for (i = 0; i < array->len; i++)
        {
          if (g_array_index (array, gchar , i) == '\0')
            {
              g_set_error (error, TP_ERROR, TP_ERROR_INVALID_ARGUMENT,
                  "Unix socket path can't contain zero bytes");
              return FALSE;
            }
        }

      socket_address = g_string_new_len (array->data, array->len);

      if (g_stat (socket_address->str, &stat_buff) == -1)
      {
        DEBUG ("Error calling stat on socket: %s", g_strerror (errno));

        g_set_error (error, TP_ERROR, TP_ERROR_INVALID_ARGUMENT, "%s: %s",
            socket_address->str, g_strerror (errno));
        g_string_free (socket_address, TRUE);
        return FALSE;
      }

      if (!S_ISSOCK (stat_buff.st_mode))
      {
        DEBUG ("%s is not a socket", socket_address->str);

        g_set_error (error, TP_ERROR, TP_ERROR_INVALID_ARGUMENT,
            "%s is not a socket", socket_address->str);
        g_string_free (socket_address, TRUE);
        return FALSE;
      }

      g_string_free (socket_address, TRUE);
    }

  if (access_control == TP_SOCKET_ACCESS_CONTROL_LOCALHOST ||
      access_control == TP_SOCKET_ACCESS_CONTROL_CREDENTIALS)
  {
    /* no variant associated */
    return TRUE;
  }

  g_set_error (error, TP_ERROR, TP_ERROR_INVALID_ARGUMENT,
      "%u socket access control is not supported", access_control);
  return FALSE;
}
#endif

static gboolean
check_ip_params (TpSocketAddressType address_type,
                 const GValue *address,
                 TpSocketAccessControl access_control,
                 const GValue *access_control_param,
                 GError **error)
{
  /* Check address type */
  if (address != NULL)
    {
      gchar *ip;
      guint port;
      struct addrinfo req, *result = NULL;
      int ret;

      if (address_type == TP_SOCKET_ADDRESS_TYPE_IPV4)
        {
          if (G_VALUE_TYPE (address) != TP_STRUCT_TYPE_SOCKET_ADDRESS_IPV4)
            {
              g_set_error (error, TP_ERROR, TP_ERROR_INVALID_ARGUMENT,
                  "IPv4 socket address is supposed to be sq");
              return FALSE;
            }
        }
      else if (address_type == TP_SOCKET_ADDRESS_TYPE_IPV6)
        {
          if (G_VALUE_TYPE (address) != TP_STRUCT_TYPE_SOCKET_ADDRESS_IPV6)
            {
              g_set_error (error, TP_ERROR, TP_ERROR_INVALID_ARGUMENT,
                  "IPv6 socket address is supposed to be sq");
              return FALSE;
            }
        }
      else
        {
          g_return_val_if_reached (FALSE);
        }

      dbus_g_type_struct_get (address,
          0, &ip,
          1, &port,
          G_MAXUINT);

      memset (&req, 0, sizeof (req));
      req.ai_flags = AI_NUMERICHOST;
      req.ai_socktype = SOCK_STREAM;
      req.ai_protocol = IPPROTO_TCP;

      if (address_type == TP_SOCKET_ADDRESS_TYPE_IPV4)
        req.ai_family = AF_INET;
      else
        req.ai_family = AF_INET6;

      ret = getaddrinfo (ip, NULL, &req, &result);
      if (ret != 0)
        {
          g_set_error (error, TP_ERROR, TP_ERROR_INVALID_ARGUMENT,
              "Invalid address: %s", gai_strerror (ret));
          g_free (ip);
          return FALSE;
        }

      g_free (ip);
      freeaddrinfo (result);
    }

  if (access_control == TP_SOCKET_ACCESS_CONTROL_LOCALHOST)
    {
      return TRUE;
    }
  else if (access_control == TP_SOCKET_ACCESS_CONTROL_PORT)
    {
      if (access_control_param != NULL)
        {
          if (G_VALUE_TYPE (access_control_param) !=
              TP_STRUCT_TYPE_SOCKET_ADDRESS_IPV4)
            {
              g_set_error (error, TP_ERROR, TP_ERROR_INVALID_ARGUMENT,
                  "Port access param is supposed to be sq");
              return FALSE;
            }
        }
      return TRUE;
    }

  g_set_error (error, TP_ERROR, TP_ERROR_INVALID_ARGUMENT,
      "%u socket access control is not supported", access_control);
  return FALSE;
}

/* used to check access control parameters both for OfferStreamTube and
 * AcceptStreamTube. In case of AcceptStreamTube, address is NULL because we
 * listen on the socket after the parameters have been accepted
 */
gboolean
gabble_tube_stream_check_params (TpSocketAddressType address_type,
                                 const GValue *address,
                                 TpSocketAccessControl access_control,
                                 const GValue *access_control_param,
                                 GError **error)
{
  switch (address_type)
    {
#ifdef GIBBER_TYPE_UNIX_TRANSPORT
      case TP_SOCKET_ADDRESS_TYPE_UNIX:
        return check_unix_params (address_type, address, access_control,
            access_control_param, error);
#endif

      case TP_SOCKET_ADDRESS_TYPE_IPV4:
      case TP_SOCKET_ADDRESS_TYPE_IPV6:
        return check_ip_params (address_type, address, access_control,
            access_control_param, error);

      default:
        g_set_error (error, TP_ERROR, TP_ERROR_NOT_IMPLEMENTED,
            "Address type %d not implemented", address_type);
        return FALSE;
    }
}

static gboolean
send_tube_offer (GabbleTubeStream *self,
                 GError **error)
{
  GabbleTubeStreamPrivate *priv = self->priv;
  TpBaseChannel *base = TP_BASE_CHANNEL (self);
  TpBaseConnection *base_conn = tp_base_channel_get_connection (base);
  GabbleConnection *conn = GABBLE_CONNECTION (base_conn);
  TpBaseChannelClass *cls = TP_BASE_CHANNEL_GET_CLASS (base);
  WockyNode *tube_node = NULL;
  WockyStanza *msg;
  TpHandleRepoIface *contact_repo;
  const gchar *jid;
  gboolean result;
  GabblePresence *presence;
  const gchar *resource;
  gchar *full_jid;

  g_assert (cls->target_handle_type == TP_HANDLE_TYPE_CONTACT);

  contact_repo = tp_base_connection_get_handles (base_conn,
     TP_HANDLE_TYPE_CONTACT);

  jid = tp_handle_inspect (contact_repo,
      tp_base_channel_get_target_handle (base));

  presence = gabble_presence_cache_get (conn->presence_cache,
      tp_base_channel_get_target_handle (base));
  if (presence == NULL)
    {
      DEBUG ("can't find tube recipient's presence");
      g_set_error (error, TP_ERROR, TP_ERROR_NOT_AVAILABLE,
          "can't find tube recipient's presence");
      return FALSE;
    }

  resource = gabble_presence_pick_resource_by_caps (presence, 0,
      gabble_capability_set_predicate_has, NS_TUBES);
  if (resource == NULL)
    {
      DEBUG ("tube recipient doesn't have tubes capabilities");
      g_set_error (error, TP_ERROR, TP_ERROR_NOT_AVAILABLE,
          "tube recipient doesn't have tubes capabilities");
      return FALSE;
    }

  full_jid = g_strdup_printf ("%s/%s", jid, resource);

  msg = wocky_stanza_build (WOCKY_STANZA_TYPE_MESSAGE, WOCKY_STANZA_SUB_TYPE_NONE,
      NULL, full_jid,
      '(', "tube",
        '*', &tube_node,
        ':', NS_TUBES,
      ')',
      GABBLE_AMP_DO_NOT_STORE_SPEC,
      NULL);
  g_free (full_jid);

  g_assert (tube_node != NULL);

  gabble_tube_iface_publish_in_node (GABBLE_TUBE_IFACE (self),
      base_conn, tube_node);

  result = _gabble_connection_send (conn, msg, error);
  if (result)
    {
      priv->state = TP_TUBE_CHANNEL_STATE_REMOTE_PENDING;
    }

  g_object_unref (msg);
  return TRUE;
}

/* can be called both from the old tube API and the new tube API */
gboolean
gabble_tube_stream_offer (GabbleTubeStream *self,
                          GError **error)
{
  GabbleTubeStreamPrivate *priv = self->priv;
  TpBaseChannel *base = TP_BASE_CHANNEL (self);
  TpBaseChannelClass *cls = TP_BASE_CHANNEL_GET_CLASS (base);

  g_assert (priv->state == TP_TUBE_CHANNEL_STATE_NOT_OFFERED);

  if (cls->target_handle_type == TP_HANDLE_TYPE_CONTACT)
    {
      /* 1-1 tube. Send tube offer message */
      if (!send_tube_offer (self, error))
        return FALSE;
    }
  else
    {
      /* muc tube is open as soon it's offered */
      priv->state = TP_TUBE_CHANNEL_STATE_OPEN;
      g_signal_emit (G_OBJECT (self), signals[OPENED], 0);

      gabble_muc_channel_send_presence (priv->muc);
    }

  g_signal_emit (G_OBJECT (self), signals[OFFERED], 0);
  return TRUE;
}

static void
destroy_socket_control_list (gpointer data)
{
  GArray *tab = data;
  g_array_unref (tab);
}

/**
 * gabble_tube_stream_get_supported_socket_types
 *
 * Used to implement D-Bus property
 * org.freedesktop.Telepathy.Channel.Type.StreamTube.SupportedSocketTypes
 * and D-Bus method GetAvailableStreamTubeTypes
 * on org.freedesktop.Telepathy.Channel.Type.Tubes
 */
GHashTable *
gabble_tube_stream_get_supported_socket_types (void)
{
  GHashTable *ret;
  GArray *ipv4_tab, *ipv6_tab;
  TpSocketAccessControl access_control;
#ifdef GIBBER_TYPE_UNIX_TRANSPORT
  GArray *unix_tab;
#endif

  ret = g_hash_table_new_full (g_direct_hash, g_direct_equal, NULL,
      destroy_socket_control_list);

#ifdef GIBBER_TYPE_UNIX_TRANSPORT
  /* Socket_Address_Type_Unix */
  unix_tab = g_array_sized_new (FALSE, FALSE, sizeof (TpSocketAccessControl),
      1);
  access_control = TP_SOCKET_ACCESS_CONTROL_LOCALHOST;
  g_array_append_val (unix_tab, access_control);

  /* Credentials-passing is non-portable, so only advertise it on platforms
   * where we have an implementation (like Linux) */
  if (gibber_unix_transport_supports_credentials ())
    {
      access_control = TP_SOCKET_ACCESS_CONTROL_CREDENTIALS;
      g_array_append_val (unix_tab, access_control);
    }

  g_hash_table_insert (ret, GUINT_TO_POINTER (TP_SOCKET_ADDRESS_TYPE_UNIX),
      unix_tab);
#endif

  /* Socket_Address_Type_IPv4 */
  ipv4_tab = g_array_sized_new (FALSE, FALSE, sizeof (TpSocketAccessControl),
      1);
  access_control = TP_SOCKET_ACCESS_CONTROL_LOCALHOST;
  g_array_append_val (ipv4_tab, access_control);
  access_control = TP_SOCKET_ACCESS_CONTROL_PORT;
  g_array_append_val (ipv4_tab, access_control);
  g_hash_table_insert (ret, GUINT_TO_POINTER (TP_SOCKET_ADDRESS_TYPE_IPV4),
      ipv4_tab);

  /* Socket_Address_Type_IPv6 */
  ipv6_tab = g_array_sized_new (FALSE, FALSE, sizeof (TpSocketAccessControl),
      1);
  access_control = TP_SOCKET_ACCESS_CONTROL_LOCALHOST;
  g_array_append_val (ipv6_tab, access_control);
  access_control = TP_SOCKET_ACCESS_CONTROL_PORT;
  g_array_append_val (ipv6_tab, access_control);
  g_hash_table_insert (ret, GUINT_TO_POINTER (TP_SOCKET_ADDRESS_TYPE_IPV6),
      ipv6_tab);

  return ret;
}

/**
 * gabble_tube_stream_offer_async
 *
 * Implements D-Bus method Offer
 * on org.freedesktop.Telepathy.Channel.Type.StreamTube
 */
static void
gabble_tube_stream_offer_async (TpSvcChannelTypeStreamTube *iface,
    guint address_type,
    const GValue *address,
    guint access_control,
    GHashTable *parameters,
    DBusGMethodInvocation *context)
{
  GabbleTubeStream *self = GABBLE_TUBE_STREAM (iface);
  GabbleTubeStreamPrivate *priv = self->priv;
  GError *error = NULL;
  TpBaseChannel *base = TP_BASE_CHANNEL (self);
  TpBaseChannelClass *cls = TP_BASE_CHANNEL_GET_CLASS (base);

  if (priv->state != TP_TUBE_CHANNEL_STATE_NOT_OFFERED)
    {
      g_set_error (&error, TP_ERROR, TP_ERROR_INVALID_ARGUMENT,
          "Tube is not in the not offered state");
      dbus_g_method_return_error (context, error);
      g_error_free (error);
      return;
    }

  if (!gabble_tube_stream_check_params (address_type, address,
        access_control, NULL, &error))
    {
      dbus_g_method_return_error (context, error);
      g_error_free (error);
      return;
    }

  g_assert (address_type == TP_SOCKET_ADDRESS_TYPE_UNIX ||
      address_type == TP_SOCKET_ADDRESS_TYPE_IPV4 ||
      address_type == TP_SOCKET_ADDRESS_TYPE_IPV6);
  g_assert (priv->address == NULL);
  priv->address_type = address_type;
  priv->address = tp_g_value_slice_dup (address);
  g_assert (priv->access_control == TP_SOCKET_ACCESS_CONTROL_LOCALHOST);
  priv->access_control = access_control;

  g_object_set (self, "parameters", parameters, NULL);

  if (!gabble_tube_stream_offer (self, &error))
    {
      gabble_tube_iface_stream_close (GABBLE_TUBE_IFACE (self), TRUE);

      dbus_g_method_return_error (context, error);

      g_error_free (error);
      return;
    }

  if (cls->target_handle_type == TP_HANDLE_TYPE_CONTACT)
    {
      tp_svc_channel_interface_tube_emit_tube_channel_state_changed (
          self, TP_TUBE_CHANNEL_STATE_REMOTE_PENDING);
    }
  else
    {
      tp_svc_channel_interface_tube_emit_tube_channel_state_changed (
          self, TP_TUBE_CHANNEL_STATE_OPEN);
    }

  tp_svc_channel_type_stream_tube_return_from_offer (context);
}

/**
 * gabble_tube_stream_accept_async
 *
 * Implements D-Bus method Accept
 * on org.freedesktop.Telepathy.Channel.Type.StreamTube
 */
static void
gabble_tube_stream_accept_async (TpSvcChannelTypeStreamTube *iface,
    guint address_type,
    guint access_control,
    const GValue *access_control_param,
    DBusGMethodInvocation *context)
{
  GabbleTubeStream *self = GABBLE_TUBE_STREAM (iface);
  GabbleTubeStreamPrivate *priv = self->priv;
  GError *error = NULL;

  /* parameters sanity checks are done in gabble_tube_stream_accept */
  priv->address_type = address_type;
  priv->access_control = access_control;
  if (priv->access_control_param != NULL)
    tp_g_value_slice_free (priv->access_control_param);
  priv->access_control_param = tp_g_value_slice_dup (access_control_param);

  if (!gabble_tube_stream_accept (GABBLE_TUBE_IFACE (self), &error))
    {
      dbus_g_method_return_error (context, error);
      g_error_free (error);
      return;
    }

#if 0
  /* TODO: add a property "muc" and set it at initialization */
  if (priv->handle_type == TP_HANDLE_TYPE_ROOM)
    gabble_muc_channel_send_presence (self->muc, NULL);
#endif

  tp_svc_channel_type_stream_tube_return_from_accept (context,
      priv->address);
}

const gchar * const *
gabble_tube_stream_channel_get_allowed_properties (void)
{
  return gabble_tube_stream_channel_allowed_properties;
}

static void
tube_iface_init (gpointer g_iface,
                 gpointer iface_data)
{
  GabbleTubeIfaceClass *klass = (GabbleTubeIfaceClass *) g_iface;

  klass->accept = gabble_tube_stream_accept;
  klass->close = gabble_tube_iface_stream_close;
  klass->add_bytestream = gabble_tube_stream_add_bytestream;
}

static void
streamtube_iface_init (gpointer g_iface,
                       gpointer iface_data)
{
  TpSvcChannelTypeStreamTubeClass *klass =
      (TpSvcChannelTypeStreamTubeClass *) g_iface;

#define IMPLEMENT(x, suffix) tp_svc_channel_type_stream_tube_implement_##x (\
    klass, gabble_tube_stream_##x##suffix)
  IMPLEMENT(offer,_async);
  IMPLEMENT(accept,_async);
#undef IMPLEMENT
}
