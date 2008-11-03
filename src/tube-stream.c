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
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>
#include <netdb.h>

#include <glib/gstdio.h>
#include <loudmouth/loudmouth.h>
#include <telepathy-glib/channel-iface.h>
#include <telepathy-glib/dbus.h>
#include <telepathy-glib/group-mixin.h>
#include <telepathy-glib/gtypes.h>
#include <telepathy-glib/interfaces.h>
#include <telepathy-glib/svc-channel.h>
#include <telepathy-glib/svc-generic.h>

#include "extensions/extensions.h"

#define DEBUG_FLAG GABBLE_DEBUG_TUBES

#include "bytestream-factory.h"
#include "bytestream-iface.h"
#include "connection.h"
#include "debug.h"
#include "disco.h"
#include "gabble-signals-marshal.h"
#include "muc-channel.h"
#include "namespaces.h"
#include "presence-cache.h"
#include "presence.h"
#include "tube-iface.h"
#include "util.h"

static void channel_iface_init (gpointer, gpointer);
static void tube_iface_init (gpointer g_iface, gpointer iface_data);
static void streamtube_iface_init (gpointer g_iface, gpointer iface_data);

G_DEFINE_TYPE_WITH_CODE (GabbleTubeStream, gabble_tube_stream, G_TYPE_OBJECT,
    G_IMPLEMENT_INTERFACE (TP_TYPE_SVC_DBUS_PROPERTIES,
      tp_dbus_properties_mixin_iface_init);
    G_IMPLEMENT_INTERFACE (TP_TYPE_SVC_CHANNEL, channel_iface_init);
    G_IMPLEMENT_INTERFACE (GABBLE_TYPE_TUBE_IFACE, tube_iface_init);
    G_IMPLEMENT_INTERFACE (GABBLE_TYPE_SVC_CHANNEL_TYPE_STREAM_TUBE,
      streamtube_iface_init);
    G_IMPLEMENT_INTERFACE (GABBLE_TYPE_SVC_CHANNEL_INTERFACE_TUBE,
      NULL);
    G_IMPLEMENT_INTERFACE (TP_TYPE_SVC_CHANNEL_INTERFACE_GROUP,
      tp_external_group_mixin_iface_init);
    G_IMPLEMENT_INTERFACE (TP_TYPE_EXPORTABLE_CHANNEL, NULL);
    G_IMPLEMENT_INTERFACE (TP_TYPE_CHANNEL_IFACE, NULL));

static const gchar *gabble_tube_stream_interfaces[] = {
    TP_IFACE_CHANNEL_INTERFACE_GROUP,
    /* If more interfaces are added, either keep Group as the first, or change
     * the implementations of gabble_tube_stream_get_interfaces () and
     * gabble_tube_stream_get_property () too */
    GABBLE_IFACE_CHANNEL_INTERFACE_TUBE,
    NULL
};

/* Linux glibc bits/socket.h suggests that struct sockaddr_storage is
 * not guaranteed to be big enough for AF_UNIX addresses */
typedef union
{
  /* we'd call this unix, but gcc predefines that. Thanks, gcc */
  struct sockaddr_un un;
  struct sockaddr_in ipv4;
  struct sockaddr_in6 ipv6;
} SockAddr;

/* signals */
enum
{
  OPENED,
  NEW_CONNECTION,
  CLOSED,
  LAST_SIGNAL
};

static guint signals[LAST_SIGNAL] = {0};

/* properties */
enum
{
  PROP_OBJECT_PATH = 1,
  PROP_CHANNEL_TYPE,
  PROP_CONNECTION,
  PROP_INTERFACES,
  PROP_HANDLE,
  PROP_HANDLE_TYPE,
  PROP_SELF_HANDLE,
  PROP_ID,
  PROP_TYPE,
  PROP_SERVICE,
  PROP_PARAMETERS,
  PROP_STATE,
  PROP_ADDRESS_TYPE,
  PROP_ADDRESS,
  PROP_ACCESS_CONTROL,
  PROP_ACCESS_CONTROL_PARAM,
  PROP_CHANNEL_DESTROYED,
  PROP_CHANNEL_PROPERTIES,
  PROP_REQUESTED,
  PROP_TARGET_ID,
  PROP_INITIATOR_HANDLE,
  PROP_INITIATOR_ID,
  PROP_SUPPORTED_SOCKET_TYPES,
  LAST_PROPERTY
};

struct _GabbleTubeStreamPrivate
{
  GabbleConnection *conn;
  char *object_path;
  TpHandle handle;
  TpHandleType handle_type;
  TpHandle self_handle;
  guint id;
  GHashTable *fd_to_bytestreams;
  GHashTable *bytestream_to_io_channel;
  GHashTable *io_channel_to_watcher_source_id;
  TpHandle initiator;
  gchar *service;
  GHashTable *parameters;
  GabbleTubeChannelState state;

  TpSocketAddressType address_type;
  GValue *address;
  TpSocketAccessControl access_control;
  GValue *access_control_param;
  GIOChannel *listen_io_channel;
  guint listen_io_channel_source_id;
  gboolean closed;

  gboolean dispose_has_run;
};

#define GABBLE_TUBE_STREAM_GET_PRIVATE(obj) ((obj)->priv)

static void data_received_cb (GabbleBytestreamIface *ibb, TpHandle sender,
    GString *data, gpointer user_data);

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

static gboolean
data_to_read_on_socket_cb (GIOChannel *source,
                           GIOCondition condition,
                           gpointer data)
{
  GabbleTubeStream *self = GABBLE_TUBE_STREAM (data);
  GabbleTubeStreamPrivate *priv = GABBLE_TUBE_STREAM_GET_PRIVATE (self);
  GabbleBytestreamIface *bytestream;
  int fd;
  gchar buffer[4096];
  gsize num_read;
  GIOStatus status;
  GError *error = NULL;
  gboolean result = TRUE;

  if (! (condition & G_IO_IN))
    return TRUE;

  fd = g_io_channel_unix_get_fd (source);

  bytestream = g_hash_table_lookup (priv->fd_to_bytestreams,
      GINT_TO_POINTER (fd));
  if (bytestream == NULL)
    {
      DEBUG ("no bytestream associated with this socket");

      g_hash_table_remove (priv->io_channel_to_watcher_source_id, source);
      return FALSE;
    }

  memset (&buffer, 0, sizeof (buffer));

  status = g_io_channel_read_chars (source, buffer, 4096, &num_read, &error);
  if (status == G_IO_STATUS_NORMAL)
    {
      DEBUG ("read %" G_GSIZE_FORMAT " bytes from socket", num_read);

      gabble_bytestream_iface_send (bytestream, num_read, buffer);
      result = TRUE;
    }
  else if (status == G_IO_STATUS_EOF)
    {
      DEBUG ("error reading from socket: EOF");

      gabble_bytestream_iface_close (bytestream, NULL);
      result = FALSE;
    }
  else if (status == G_IO_STATUS_AGAIN)
    {
      DEBUG ("error reading from socket: resource temporarily unavailable");

      result = TRUE;
    }
  else
    {
      DEBUG ("error reading from socket: %s", error ? error->message : "");

      gabble_bytestream_iface_close (bytestream, NULL);
      result = FALSE;
    }

  if (error != NULL)
    g_error_free (error);

  return TRUE;
}

static void
extra_bytestream_state_changed_cb (GabbleBytestreamIface *bytestream,
                                   GabbleBytestreamState state,
                                   gpointer user_data)
{
  GabbleTubeStream *self = GABBLE_TUBE_STREAM (user_data);
  GabbleTubeStreamPrivate *priv = GABBLE_TUBE_STREAM_GET_PRIVATE (self);
  GIOChannel *channel;

  channel = g_hash_table_lookup (priv->bytestream_to_io_channel,
      bytestream);
  if (channel == NULL)
    {
      DEBUG ("no IO channel associated with the bytestream");
      return;
    }

  if (state == GABBLE_BYTESTREAM_STATE_OPEN)
    {
      guint source_id;
      DEBUG ("extra bytestream open");

      g_signal_connect (bytestream, "data-received",
          G_CALLBACK (data_received_cb), self);

      source_id = g_io_add_watch (channel, G_IO_IN, data_to_read_on_socket_cb,
          self);
      g_hash_table_insert (priv->io_channel_to_watcher_source_id,
          g_io_channel_ref (channel), GUINT_TO_POINTER (source_id));
    }
  else if (state == GABBLE_BYTESTREAM_STATE_CLOSED)
    {
      int fd;

      DEBUG ("extra bytestream closed");

      fd = g_io_channel_unix_get_fd (channel);

      g_hash_table_remove (priv->fd_to_bytestreams, GINT_TO_POINTER (fd));
      g_hash_table_remove (priv->bytestream_to_io_channel, bytestream);
      g_hash_table_remove (priv->io_channel_to_watcher_source_id, channel);
    }
}

struct _extra_bytestream_negotiate_cb_data
{
  GabbleTubeStream *self;
  gint fd;
};

static void
extra_bytestream_negotiate_cb (GabbleBytestreamIface *bytestream,
                               const gchar *stream_id,
                               LmMessage *msg,
                               gpointer user_data)
{
  struct _extra_bytestream_negotiate_cb_data *data =
    (struct _extra_bytestream_negotiate_cb_data *) user_data;
  GabbleTubeStream *self = data->self;
  GabbleTubeStreamPrivate *priv = GABBLE_TUBE_STREAM_GET_PRIVATE (self);
  GIOChannel *channel;

  if (bytestream == NULL)
    {
      DEBUG ("initiator refused new bytestream");

      close (data->fd);
      return;
    }

  DEBUG ("extra bytestream accepted");

  channel = g_io_channel_unix_new (data->fd);
  g_io_channel_set_encoding (channel, NULL, NULL);
  g_io_channel_set_buffered (channel, FALSE);
  g_io_channel_set_close_on_unref (channel, TRUE);

  g_hash_table_insert (priv->fd_to_bytestreams, GINT_TO_POINTER (data->fd),
      g_object_ref (bytestream));
  g_hash_table_insert (priv->bytestream_to_io_channel,
      g_object_ref (bytestream), channel);

  g_signal_connect (bytestream, "state-changed",
                G_CALLBACK (extra_bytestream_state_changed_cb), self);

  g_slice_free (struct _extra_bytestream_negotiate_cb_data, data);
}

static gboolean
start_stream_initiation (GabbleTubeStream *self,
                         gint fd,
                         GError **error)
{
  GabbleTubeStreamPrivate *priv;
  LmMessageNode *node, *si_node;
  LmMessage *msg;
  TpHandleRepoIface *contact_repo;
  const gchar *jid;
  gchar *full_jid, *stream_id, *id_str;
  gboolean result;
  struct _extra_bytestream_negotiate_cb_data *data;

  priv = GABBLE_TUBE_STREAM_GET_PRIVATE (self);

  contact_repo = tp_base_connection_get_handles (
     (TpBaseConnection *) priv->conn, TP_HANDLE_TYPE_CONTACT);

  jid = tp_handle_inspect (contact_repo, priv->initiator);

  if (priv->handle_type == TP_HANDLE_TYPE_CONTACT)
    {
      /* Private tube */
      GabblePresence *presence;
      const gchar *resource;

      presence = gabble_presence_cache_get (priv->conn->presence_cache,
          priv->initiator);
      if (presence == NULL)
        {
          DEBUG ("can't find initiator's presence");
          if (error != NULL)
            g_set_error (error, TP_ERRORS, TP_ERROR_NOT_AVAILABLE,
                "can't find initiator's presence");

          return FALSE;
        }

      resource = gabble_presence_pick_resource_by_caps (presence,
          PRESENCE_CAP_SI_TUBES);
      if (resource == NULL)
        {
          DEBUG ("initiator doesn't have tubes capabilities");
          if (error != NULL)
            g_set_error (error, TP_ERRORS, TP_ERROR_NOT_AVAILABLE,
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

  si_node = lm_message_node_get_child_with_namespace (msg->node, "si", NS_SI);
  g_assert (si_node != NULL);

  id_str = g_strdup_printf ("%u", priv->id);

  if (priv->handle_type == TP_HANDLE_TYPE_CONTACT)
    {
      node = lm_message_node_add_child (si_node, "stream", NULL);
    }
  else
    {
      node = lm_message_node_add_child (si_node, "muc-stream", NULL);
    }

  lm_message_node_set_attributes (node,
      "xmlns", NS_TUBES,
      "tube", id_str,
      NULL);

  data = g_slice_new (struct _extra_bytestream_negotiate_cb_data);
  data->self = self;
  data->fd = fd;

  result = gabble_bytestream_factory_negotiate_stream (
    priv->conn->bytestream_factory,
    msg,
    stream_id,
    extra_bytestream_negotiate_cb,
    data,
    error);

  lm_message_unref (msg);
  g_free (stream_id);
  g_free (full_jid);
  g_free (id_str);

  return result;
}

static gboolean
listen_cb (GIOChannel *source,
           GIOCondition condition,
           gpointer data)
{
  GabbleTubeStream *self = GABBLE_TUBE_STREAM (data);
  GabbleTubeStreamPrivate *priv = GABBLE_TUBE_STREAM_GET_PRIVATE (self);
  int fd, listen_fd;
  SockAddr addr;
  socklen_t addrlen;
  int flags;

  listen_fd = g_io_channel_unix_get_fd (source);

  if (priv->address_type == TP_SOCKET_ADDRESS_TYPE_UNIX)
    {
      addrlen = sizeof (addr.un);
    }
  else if (priv->address_type == TP_SOCKET_ADDRESS_TYPE_IPV4)
    {
      addrlen = sizeof (addr.ipv4);
    }
  else if (priv->address_type == TP_SOCKET_ADDRESS_TYPE_IPV6)
    {
      addrlen = sizeof (addr.ipv6);
    }
  else
    {
      g_return_val_if_reached (TRUE);
    }

  fd = accept (listen_fd, (struct sockaddr *) &addr, &addrlen);
  if (fd == -1)
    {
      DEBUG ("Error accepting socket: %s", g_strerror (errno));
      return TRUE;
    }

  DEBUG ("connection from client");

  /* Set socket non blocking */
  flags = fcntl (fd, F_GETFL, 0);
  if (fcntl (fd, F_SETFL, flags | O_NONBLOCK) == -1)
    {
      DEBUG ("Can't set socket non blocking: %s", g_strerror (errno));
      close (fd);
      return TRUE;
    }

  DEBUG ("request new bytestream");

  if (!start_stream_initiation (self, fd, NULL))
    {
      DEBUG ("closing new client connection");
      close (fd);
    }

  return TRUE;
}

static gboolean
new_connection_to_socket (GabbleTubeStream *self,
                          GabbleBytestreamIface *bytestream)
{
  GabbleTubeStreamPrivate *priv = GABBLE_TUBE_STREAM_GET_PRIVATE (self);
  int fd;
  GIOChannel *channel;
  SockAddr addr;
  socklen_t len;

  g_assert (priv->initiator == priv->self_handle);

  memset (&addr, 0, sizeof (addr));

  if (priv->address_type == TP_SOCKET_ADDRESS_TYPE_UNIX)
    {
      GArray *array;
      array = g_value_get_boxed (priv->address);

      fd = socket (PF_UNIX, SOCK_STREAM, 0);
      if (fd == -1)
        {
          DEBUG ("Error creating socket: %s", g_strerror (errno));
          return FALSE;
        }

      addr.un.sun_family = PF_UNIX;
      strncpy (addr.un.sun_path, array->data, sizeof (addr.un.sun_path) - 1);
      addr.un.sun_path[sizeof (addr.un.sun_path) - 1] = '\0';
      len = sizeof (addr.un);

      DEBUG ("Will try to connect to socket: %s", (const gchar *) array->data);
    }
  else if (priv->address_type == TP_SOCKET_ADDRESS_TYPE_IPV4 ||
      priv->address_type == TP_SOCKET_ADDRESS_TYPE_IPV6)
    {
      gchar *ip;
      guint port;
      struct addrinfo req, *result = NULL;
      int ret;

      if (priv->address_type == TP_SOCKET_ADDRESS_TYPE_IPV4)
        fd = socket (PF_INET, SOCK_STREAM, 0);
      else
        fd = socket (PF_INET6, SOCK_STREAM, 0);

      if (fd == -1)
        {
          DEBUG ("Error creating socket: %s", g_strerror (errno));
          return FALSE;
        }

      dbus_g_type_struct_get (priv->address,
          0, &ip,
          1, &port,
          G_MAXUINT);

      memset (&req, 0, sizeof (req));
      req.ai_flags = AI_NUMERICHOST;
      req.ai_socktype = SOCK_STREAM;
      req.ai_protocol = IPPROTO_TCP;

      if (priv->address_type == TP_SOCKET_ADDRESS_TYPE_IPV4)
        req.ai_family = AF_INET;
      else
        req.ai_family = AF_INET6;

      ret = getaddrinfo (ip, NULL, &req, &result);
      if (ret != 0)
        {
          DEBUG ("getaddrinfo failed: %s",  gai_strerror (ret));
          g_free (ip);
          return FALSE;
        }

      DEBUG ("Will try to connect to %s:%u", ip, port);

      if (priv->address_type == TP_SOCKET_ADDRESS_TYPE_IPV4)
        {
          memcpy (&addr, result->ai_addr, sizeof (addr.ipv4));
          addr.ipv4.sin_port = ntohs (port);
          len = sizeof (addr.ipv4);
        }
      else
        {
          memcpy (&addr, result->ai_addr, sizeof (addr.ipv6));
          addr.ipv6.sin6_port = ntohs (port);
          len = sizeof (addr.ipv6);
        }

      g_free (ip);
      freeaddrinfo (result);
    }
  else
    {
      g_return_val_if_reached (FALSE);
    }

  if (connect (fd, (struct sockaddr *) &addr, len) == -1)
    {
      DEBUG ("Error connecting socket: %s", g_strerror (errno));
      return FALSE;
    }
  DEBUG ("Connected to socket");

  channel = g_io_channel_unix_new (fd);
  g_io_channel_set_encoding (channel, NULL, NULL);
  g_io_channel_set_buffered (channel, FALSE);
  g_io_channel_set_close_on_unref (channel, TRUE);

  g_hash_table_insert (priv->fd_to_bytestreams, GINT_TO_POINTER (fd),
      g_object_ref (bytestream));
  g_hash_table_insert (priv->bytestream_to_io_channel,
      g_object_ref (bytestream), channel);

  g_signal_connect (bytestream, "state-changed",
                G_CALLBACK (extra_bytestream_state_changed_cb), self);

  return TRUE;
}

static gboolean
tube_stream_open (GabbleTubeStream *self,
                  GError **error)
{
  GabbleTubeStreamPrivate *priv = GABBLE_TUBE_STREAM_GET_PRIVATE (self);
  int fd;

  DEBUG ("called");

  if (priv->initiator == priv->self_handle)
    /* Nothing to do if we are the initiator of this tube.
     * We'll connect to the socket each time request a new bytestream. */
    return TRUE;

  /* We didn't create this tube so it doesn't have
   * a socket associated with it. Let's create one */
  g_assert (priv->address == NULL);

  if (priv->address_type == TP_SOCKET_ADDRESS_TYPE_UNIX)
    {
      GArray *array;
      struct sockaddr_un addr;
      gchar suffix[8];

      fd = socket (PF_UNIX, SOCK_STREAM, 0);
      if (fd == -1)
        {
          DEBUG ("Error creating socket: %s", g_strerror (errno));
          g_set_error (error, TP_ERRORS, TP_ERROR_NETWORK_ERROR,
              "Error creating socket: %s", g_strerror (errno));
          return FALSE;
        }

      memset (&addr, 0, sizeof (addr));
      addr.sun_family = PF_UNIX;

      generate_ascii_string (8, suffix);
      snprintf (addr.sun_path, sizeof (addr.sun_path) - 1,
        "/tmp/stream-gabble-%.8s", suffix);

      DEBUG ("create socket: %s", addr.sun_path);

      array = g_array_sized_new (TRUE, FALSE, sizeof (gchar), strlen (
            addr.sun_path));
      g_array_insert_vals (array, 0, addr.sun_path, strlen (addr.sun_path));

      priv->address = tp_g_value_slice_new (DBUS_TYPE_G_UCHAR_ARRAY);
      g_value_set_boxed (priv->address, array);

      g_array_free (array, TRUE);

      if (bind (fd, (struct sockaddr *) &addr, sizeof (addr)) == -1)
        {
          DEBUG ("Error binding socket: %s", g_strerror (errno));
          g_set_error (error, TP_ERRORS, TP_ERROR_NETWORK_ERROR,
              "Error binding socket: %s", g_strerror (errno));
          return FALSE;
        }
    }
  else if (priv->address_type == TP_SOCKET_ADDRESS_TYPE_IPV4)
    {
      struct sockaddr_in addr;
      socklen_t len;

      addr.sin_family = AF_INET;
      addr.sin_port = 0;         /* == ntohs (0) */
      addr.sin_addr.s_addr = htonl (INADDR_LOOPBACK);

      len = sizeof (addr);

      fd = socket (PF_INET, SOCK_STREAM, 0);
      if (fd == -1)
        {
          DEBUG ("Error creating socket: %s", g_strerror (errno));
          g_set_error (error, TP_ERRORS, TP_ERROR_NETWORK_ERROR,
              "Error creating socket: %s", g_strerror (errno));
          return FALSE;
        }

      if (bind (fd, (struct sockaddr *) &addr, len) == -1)
        {
          DEBUG ("Error binding socket: %s", g_strerror (errno));
          g_set_error (error, TP_ERRORS, TP_ERROR_NETWORK_ERROR,
              "Error binding socket: %s", g_strerror (errno));
          return FALSE;
        }

      if (getsockname (fd, (struct sockaddr *) &addr, &len) == -1)
        {
          DEBUG ("getsockname failed: %s", g_strerror (errno));
          g_set_error (error, TP_ERRORS, TP_ERROR_NETWORK_ERROR,
              "getsockname failed: %s", g_strerror (errno));
          return FALSE;
        }

      DEBUG ("create socket %s:%u", "127.0.0.1", ntohs (addr.sin_port));

      priv->address = tp_g_value_slice_new (
          TP_STRUCT_TYPE_SOCKET_ADDRESS_IPV4);
      g_value_take_boxed (priv->address,
          dbus_g_type_specialized_construct (
              TP_STRUCT_TYPE_SOCKET_ADDRESS_IPV4));

      dbus_g_type_struct_set (priv->address,
          0, "127.0.0.1",
          1, ntohs (addr.sin_port),
          G_MAXUINT);
    }
  else if (priv->address_type == TP_SOCKET_ADDRESS_TYPE_IPV6)
    {
      struct sockaddr_in6 addr;
      socklen_t len;
      struct in6_addr loopback_addr = IN6ADDR_LOOPBACK_INIT;

      addr.sin6_family = AF_INET6;
      addr.sin6_port = 0;         /* == ntohs (0) */
      addr.sin6_addr = loopback_addr;

      len = sizeof (addr);

      fd = socket (PF_INET6, SOCK_STREAM, 0);
      if (fd == -1)
        {
          DEBUG ("Error creating socket: %s", g_strerror (errno));
          g_set_error (error, TP_ERRORS, TP_ERROR_NETWORK_ERROR,
              "Error creating socket: %s", g_strerror (errno));
          return FALSE;
        }

      if (bind (fd, (struct sockaddr *) &addr, len) == -1)
        {
          DEBUG ("Error binding socket: %s", g_strerror (errno));
          g_set_error (error, TP_ERRORS, TP_ERROR_NETWORK_ERROR,
              "Error binding socket: %s", g_strerror (errno));
          return FALSE;
        }

      if (getsockname (fd, (struct sockaddr *) &addr, &len) == -1)
        {
          DEBUG ("getsockname failed: %s", g_strerror (errno));
          g_set_error (error, TP_ERRORS, TP_ERROR_NETWORK_ERROR,
              "getsockname failed: %s", g_strerror (errno));
          return FALSE;
        }

      DEBUG ("create socket %s:%u", "::1", ntohs (addr.sin6_port));

      priv->address = tp_g_value_slice_new (
          TP_STRUCT_TYPE_SOCKET_ADDRESS_IPV6);
      g_value_take_boxed (priv->address,
          dbus_g_type_specialized_construct (
              TP_STRUCT_TYPE_SOCKET_ADDRESS_IPV6));

      dbus_g_type_struct_set (priv->address,
          0, "::1",
          1, ntohs (addr.sin6_port),
          G_MAXUINT);
    }
  else
    {
      g_return_val_if_reached (FALSE);
    }

  if (listen (fd, 5) == -1)
    {
      DEBUG ("Error listening socket: %s", g_strerror (errno));
      g_set_error (error, TP_ERRORS, TP_ERROR_NETWORK_ERROR,
          "Error listening socket: %s", g_strerror (errno));
      return FALSE;
    }

  priv->listen_io_channel = g_io_channel_unix_new (fd);
  g_io_channel_set_encoding (priv->listen_io_channel, NULL, NULL);
  g_io_channel_set_buffered (priv->listen_io_channel, FALSE);
  g_io_channel_set_close_on_unref (priv->listen_io_channel, TRUE);

  priv->listen_io_channel_source_id = g_io_add_watch (priv->listen_io_channel,
      G_IO_IN, listen_cb, self);

  return TRUE;
}

static void
remove_watcher_source_id (gpointer data)
{
  guint source_id = GPOINTER_TO_UINT (data);
  GSource *source;

  source = g_main_context_find_source_by_id (NULL, source_id);
  if (source != NULL)
    {
      g_source_destroy (source);
    }
}

static void
gabble_tube_stream_init (GabbleTubeStream *self)
{
  GabbleTubeStreamPrivate *priv = G_TYPE_INSTANCE_GET_PRIVATE (self,
      GABBLE_TYPE_TUBE_STREAM, GabbleTubeStreamPrivate);

  self->priv = priv;

  priv->fd_to_bytestreams = g_hash_table_new_full (g_direct_hash,
      g_direct_equal, NULL, (GDestroyNotify) g_object_unref);

  priv->bytestream_to_io_channel = g_hash_table_new_full (g_direct_hash,
      g_direct_equal, (GDestroyNotify) g_object_unref,
      (GDestroyNotify) g_io_channel_unref);

  priv->io_channel_to_watcher_source_id = g_hash_table_new_full (g_direct_hash,
      g_direct_equal, (GDestroyNotify) g_io_channel_unref,
      (GDestroyNotify) remove_watcher_source_id);

  priv->listen_io_channel = NULL;
  priv->listen_io_channel_source_id = 0;
  priv->address_type = TP_SOCKET_ADDRESS_TYPE_UNIX;
  priv->address = NULL;
  priv->access_control = TP_SOCKET_ACCESS_CONTROL_LOCALHOST;
  priv->access_control_param = NULL;
  priv->closed = FALSE;

  priv->dispose_has_run = FALSE;
}

static gboolean
close_each_extra_bytestream (gpointer key,
                             gpointer value,
                             gpointer user_data)
{
  GabbleTubeStream *self = GABBLE_TUBE_STREAM (user_data);
  GabbleTubeStreamPrivate *priv = GABBLE_TUBE_STREAM_GET_PRIVATE (self);
  GabbleBytestreamIface *bytestream = (GabbleBytestreamIface *) value;
  GIOChannel *channel;

  /* We are iterating over priv->fd_to_bytestreams so we can't modify it.
   * Disconnect signals so extra_bytestream_state_changed_cb won't be
   * called */
  g_signal_handlers_disconnect_matched (bytestream, G_SIGNAL_MATCH_DATA,
      0, 0, NULL, NULL, self);

  gabble_bytestream_iface_close (bytestream, NULL);

  channel = g_hash_table_lookup (priv->bytestream_to_io_channel, bytestream);
  g_assert (channel != NULL);

  g_hash_table_remove (priv->bytestream_to_io_channel, bytestream);
  g_hash_table_remove (priv->io_channel_to_watcher_source_id, channel);

  return TRUE;
}

static void
gabble_tube_stream_dispose (GObject *object)
{
  GabbleTubeStream *self = GABBLE_TUBE_STREAM (object);
  GabbleTubeStreamPrivate *priv = GABBLE_TUBE_STREAM_GET_PRIVATE (self);
  TpHandleRepoIface *contact_repo = tp_base_connection_get_handles (
      (TpBaseConnection *) priv->conn, TP_HANDLE_TYPE_CONTACT);

  if (priv->dispose_has_run)
    return;

  gabble_tube_iface_close (GABBLE_TUBE_IFACE (self));

  if (priv->initiator != priv->self_handle &&
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

  if (priv->fd_to_bytestreams != NULL)
    {
      g_hash_table_destroy (priv->fd_to_bytestreams);
      priv->fd_to_bytestreams = NULL;
    }

  if (priv->bytestream_to_io_channel != NULL)
    {
      g_hash_table_destroy (priv->bytestream_to_io_channel);
      priv->bytestream_to_io_channel = NULL;
    }

  if (priv->io_channel_to_watcher_source_id != NULL)
    {
      g_hash_table_destroy (priv->io_channel_to_watcher_source_id);
      priv->io_channel_to_watcher_source_id = NULL;
    }

  tp_handle_unref (contact_repo, priv->initiator);

  if (priv->listen_io_channel_source_id != 0)
    {
      g_source_destroy (g_main_context_find_source_by_id (NULL,
            priv->listen_io_channel_source_id));
      priv->listen_io_channel_source_id = 0;
    }

  if (priv->listen_io_channel)
    {
      g_io_channel_unref (priv->listen_io_channel);
      priv->listen_io_channel = NULL;
    }

  priv->dispose_has_run = TRUE;

  if (G_OBJECT_CLASS (gabble_tube_stream_parent_class)->dispose)
    G_OBJECT_CLASS (gabble_tube_stream_parent_class)->dispose (object);
}

static void
gabble_tube_stream_finalize (GObject *object)
{
  GabbleTubeStream *self = GABBLE_TUBE_STREAM (object);
  GabbleTubeStreamPrivate *priv = GABBLE_TUBE_STREAM_GET_PRIVATE (self);

  g_free (priv->object_path);
  g_free (priv->service);
  g_hash_table_destroy (priv->parameters);

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
  GabbleTubeStreamPrivate *priv = GABBLE_TUBE_STREAM_GET_PRIVATE (self);
  TpBaseConnection *base_conn = (TpBaseConnection *) priv->conn;

  switch (property_id)
    {
      case PROP_OBJECT_PATH:
        g_value_set_string (value, priv->object_path);
        break;
      case PROP_CHANNEL_TYPE:
        g_value_set_static_string (value,
            GABBLE_IFACE_CHANNEL_TYPE_STREAM_TUBE);
        break;
      case PROP_CONNECTION:
        g_value_set_object (value, priv->conn);
        break;
      case PROP_INTERFACES:
        if (priv->handle_type == TP_HANDLE_TYPE_CONTACT)
          {
            /* 1-1 tubes - omit the Group interface */
            g_value_set_boxed (value, gabble_tube_stream_interfaces + 1);
          }
        else
          {
            /* MUC tubes */
            g_value_set_boxed (value, gabble_tube_stream_interfaces);
          }
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
      case PROP_TYPE:
        g_value_set_uint (value, TP_TUBE_TYPE_STREAM);
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
      case PROP_CHANNEL_DESTROYED:
        g_value_set_boolean (value, priv->closed);
        break;
      case PROP_CHANNEL_PROPERTIES:
        g_value_set_boxed (value,
            tp_dbus_properties_mixin_make_properties_hash (object,
                TP_IFACE_CHANNEL, "TargetHandle",
                TP_IFACE_CHANNEL, "TargetHandleType",
                TP_IFACE_CHANNEL, "ChannelType",
                TP_IFACE_CHANNEL, "TargetID",
                TP_IFACE_CHANNEL, "InitiatorHandle",
                TP_IFACE_CHANNEL, "InitiatorID",
                TP_IFACE_CHANNEL, "Requested",
                NULL));
        break;
      case PROP_REQUESTED:
        g_value_set_boolean (value,
            (priv->initiator == priv->self_handle));
        break;
      case PROP_INITIATOR_ID:
          {
            TpHandleRepoIface *repo = tp_base_connection_get_handles (
                base_conn, TP_HANDLE_TYPE_CONTACT);

            /* some channel can have o.f.T.Channel.InitiatorHandle == 0 but
             * tubes always have an initiator */
            g_assert (priv->initiator != 0);

            g_value_set_string (value,
                tp_handle_inspect (repo, priv->initiator));
          }
        break;
      case PROP_TARGET_ID:
          {
            TpHandleRepoIface *repo = tp_base_connection_get_handles (
                base_conn, priv->handle_type);

            g_value_set_string (value,
                tp_handle_inspect (repo, priv->handle));
          }
        break;
      case PROP_SUPPORTED_SOCKET_TYPES:
        g_value_take_boxed (value,
            gabble_tube_stream_get_supported_socket_types ());
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
  GabbleTubeStreamPrivate *priv = GABBLE_TUBE_STREAM_GET_PRIVATE (self);

  switch (property_id)
    {
      case PROP_OBJECT_PATH:
        g_free (priv->object_path);
        priv->object_path = g_value_dup_string (value);
        break;
      case PROP_CHANNEL_TYPE:
      /* this property is writable in the interface, but not actually
       * meaningfully changeable on this channel, so we do nothing */
      break;
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
        /* For now, only "localhost" control is implemented */
        g_assert (g_value_get_uint (value) ==
            TP_SOCKET_ACCESS_CONTROL_LOCALHOST);
        priv->access_control = g_value_get_uint (value);
        break;
      case PROP_ACCESS_CONTROL_PARAM:
        if (priv->access_control_param == NULL)
          {
            priv->access_control_param = tp_g_value_slice_dup (
                g_value_get_pointer (value));
          }
        break;
      default:
        G_OBJECT_WARN_INVALID_PROPERTY_ID (object, property_id, pspec);
        break;
    }
}

static GObject *
gabble_tube_stream_constructor (GType type,
                                guint n_props,
                                GObjectConstructParam *props)
{
  GObject *obj;
  GabbleTubeStreamPrivate *priv;
  DBusGConnection *bus;
  TpHandleRepoIface *contact_repo;

  obj = G_OBJECT_CLASS (gabble_tube_stream_parent_class)->
           constructor (type, n_props, props);

  priv = GABBLE_TUBE_STREAM_GET_PRIVATE (GABBLE_TUBE_STREAM (obj));

  /* Ref the initiator handle */
  g_assert (priv->conn != NULL);
  g_assert (priv->initiator != 0);
  contact_repo = tp_base_connection_get_handles
      ((TpBaseConnection *) priv->conn, TP_HANDLE_TYPE_CONTACT);
  tp_handle_ref (contact_repo, priv->initiator);

  /* Set initial state of the tube */
  if (priv->initiator == priv->self_handle)
    {
      /* We initiated this tube */
      if (priv->handle_type == TP_HANDLE_TYPE_CONTACT)
        {
          /* Private tube */
          priv->state = GABBLE_TUBE_CHANNEL_STATE_NOT_OFFERED;
        }
      else
        {
          /* Muc tube */
          priv->state = GABBLE_TUBE_CHANNEL_STATE_OPEN;
        }
    }
  else
    {
      priv->state = GABBLE_TUBE_CHANNEL_STATE_LOCAL_PENDING;
    }

  bus = tp_get_bus ();
  dbus_g_connection_register_g_object (bus, priv->object_path, obj);

  DEBUG ("Registering at '%s'", priv->object_path);

  return obj;
}

static gboolean
tube_iface_props_setter (GObject *object,
                         GQuark interface,
                         GQuark name,
                         const GValue *value,
                         gpointer setter_data,
                         GError **error)
{
  GabbleTubeStream *self = GABBLE_TUBE_STREAM (object);
  GabbleTubeStreamPrivate *priv = GABBLE_TUBE_STREAM_GET_PRIVATE (self);

  g_return_val_if_fail (interface == GABBLE_IFACE_QUARK_CHANNEL_INTERFACE_TUBE,
      FALSE);

  if (name != g_quark_from_static_string ("Parameters"))
    {
      g_object_set_property (object, setter_data, value);
      return TRUE;
    }

  if (priv->state != GABBLE_TUBE_CHANNEL_STATE_NOT_OFFERED)
  {
    g_set_error (error, TP_ERRORS, TP_ERROR_NOT_AVAILABLE,
        "Can change parameters only if the tube is not offered");
    return FALSE;
  }

  priv->parameters = g_value_dup_boxed (value);

  return TRUE;
}

static void
gabble_tube_stream_class_init (GabbleTubeStreamClass *gabble_tube_stream_class)
{
  static TpDBusPropertiesMixinPropImpl channel_props[] = {
      { "TargetHandleType", "handle-type", NULL },
      { "TargetHandle", "handle", NULL },
      { "ChannelType", "channel-type", NULL },
      { "TargetID", "target-id", NULL },
      { "Interfaces", "interfaces", NULL },
      { "Requested", "requested", NULL },
      { "InitiatorHandle", "initiator-handle", NULL },
      { "InitiatorID", "initiator-id", NULL },
      { NULL }
  };
  static TpDBusPropertiesMixinPropImpl stream_tube_props[] = {
      { "Service", "service", NULL },
      { "SupportedSocketTypes", "supported-socket-types", NULL },
      { NULL }
  };
  static TpDBusPropertiesMixinPropImpl tube_iface_props[] = {
      { "Parameters", "parameters", "parameters" },
      { "Status", "state", NULL },
      { NULL }
  };
  static TpDBusPropertiesMixinIfaceImpl prop_interfaces[] = {
      { TP_IFACE_CHANNEL,
        tp_dbus_properties_mixin_getter_gobject_properties,
        NULL,
        channel_props,
      },
      { GABBLE_IFACE_CHANNEL_TYPE_STREAM_TUBE,
        tp_dbus_properties_mixin_getter_gobject_properties,
        NULL,
        stream_tube_props,
      },
      { GABBLE_IFACE_CHANNEL_INTERFACE_TUBE,
        tp_dbus_properties_mixin_getter_gobject_properties,
        tube_iface_props_setter,
        tube_iface_props,
      },
      { NULL }
  };
  GObjectClass *object_class = G_OBJECT_CLASS (gabble_tube_stream_class);
  GParamSpec *param_spec;

  object_class->get_property = gabble_tube_stream_get_property;
  object_class->set_property = gabble_tube_stream_set_property;
  object_class->constructor = gabble_tube_stream_constructor;

  g_type_class_add_private (gabble_tube_stream_class,
      sizeof (GabbleTubeStreamPrivate));

  object_class->dispose = gabble_tube_stream_dispose;
  object_class->finalize = gabble_tube_stream_finalize;

  g_object_class_override_property (object_class, PROP_OBJECT_PATH,
      "object-path");
  g_object_class_override_property (object_class, PROP_CHANNEL_TYPE,
      "channel-type");
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

  g_object_class_override_property (object_class, PROP_CHANNEL_DESTROYED,
      "channel-destroyed");
  g_object_class_override_property (object_class, PROP_CHANNEL_PROPERTIES,
      "channel-properties");

  param_spec = g_param_spec_boxed (
      "supported-socket-types",
      "Supported socket types",
      "GHashTable containing supported socket types.",
      dbus_g_type_get_map ("GHashTable", G_TYPE_UINT, DBUS_TYPE_G_UINT_ARRAY),
      G_PARAM_READABLE |
      G_PARAM_STATIC_STRINGS);
  g_object_class_install_property (object_class, PROP_SUPPORTED_SOCKET_TYPES,
      param_spec);

  param_spec = g_param_spec_uint (
      "address-type",
      "address type",
      "a TpSocketAddressType representing the type of the listening"
      "address of the local service",
      0, NUM_TP_SOCKET_ADDRESS_TYPES - 1,
      TP_SOCKET_ADDRESS_TYPE_UNIX,
      G_PARAM_READWRITE |
      G_PARAM_STATIC_NAME |
      G_PARAM_STATIC_NICK |
      G_PARAM_STATIC_BLURB);
  g_object_class_install_property (object_class, PROP_ADDRESS_TYPE,
      param_spec);

  param_spec = g_param_spec_pointer (
      "address",
      "address",
      "The listening address of the local service, as indicated by the "
      "address-type",
      G_PARAM_READWRITE |
      G_PARAM_STATIC_NAME |
      G_PARAM_STATIC_NICK |
      G_PARAM_STATIC_BLURB);
  g_object_class_install_property (object_class, PROP_ADDRESS, param_spec);

  param_spec = g_param_spec_uint (
      "access-control",
      "access control",
      "a TpSocketAccessControl representing the access control "
      "the local service applies to the local socket",
      0, NUM_TP_SOCKET_ACCESS_CONTROLS - 1,
      TP_SOCKET_ACCESS_CONTROL_LOCALHOST,
      G_PARAM_READWRITE |
      G_PARAM_STATIC_NAME |
      G_PARAM_STATIC_NICK |
      G_PARAM_STATIC_BLURB);
  g_object_class_install_property (object_class, PROP_ACCESS_CONTROL,
      param_spec);

  param_spec = g_param_spec_pointer (
      "access-control-param",
      "access control param",
      "A parameter for the access control type, to be interpreted as specified"
      "in the documentation for the Socket_Access_Control enum.",
      G_PARAM_READWRITE |
      G_PARAM_STATIC_NAME |
      G_PARAM_STATIC_NICK |
      G_PARAM_STATIC_BLURB);
  g_object_class_install_property (object_class, PROP_ACCESS_CONTROL_PARAM,
      param_spec);

  param_spec = g_param_spec_boxed ("interfaces", "Extra D-Bus interfaces",
      "Additional Channel.Interface.* interfaces",
      G_TYPE_STRV,
      G_PARAM_READABLE | G_PARAM_STATIC_STRINGS);
  g_object_class_install_property (object_class, PROP_INTERFACES, param_spec);

  param_spec = g_param_spec_string ("target-id", "Target JID",
      "The string obtained by inspecting the target handle",
      NULL,
      G_PARAM_READABLE |
      G_PARAM_STATIC_NICK | G_PARAM_STATIC_BLURB | G_PARAM_STATIC_NAME);
  g_object_class_install_property (object_class, PROP_TARGET_ID, param_spec);

  param_spec = g_param_spec_uint ("initiator-handle", "Initiator's handle",
      "The contact who initiated the channel",
      0, G_MAXUINT32, 0,
      G_PARAM_CONSTRUCT_ONLY | G_PARAM_READWRITE |
      G_PARAM_STATIC_NICK | G_PARAM_STATIC_BLURB | G_PARAM_STATIC_NAME);
  g_object_class_install_property (object_class, PROP_INITIATOR_HANDLE,
      param_spec);

  param_spec = g_param_spec_string ("initiator-id", "Initiator's bare JID",
      "The string obtained by inspecting the initiator-handle",
      NULL,
      G_PARAM_READABLE |
      G_PARAM_STATIC_NICK | G_PARAM_STATIC_BLURB | G_PARAM_STATIC_NAME);
  g_object_class_install_property (object_class, PROP_INITIATOR_ID,
      param_spec);

  param_spec = g_param_spec_boolean ("requested", "Requested?",
      "True if this channel was requested by the local user",
      FALSE,
      G_PARAM_READABLE |
      G_PARAM_STATIC_NICK | G_PARAM_STATIC_BLURB | G_PARAM_STATIC_NAME);
  g_object_class_install_property (object_class, PROP_REQUESTED, param_spec);

  signals[OPENED] =
    g_signal_new ("tube-opened",
                  G_OBJECT_CLASS_TYPE (gabble_tube_stream_class),
                  G_SIGNAL_RUN_LAST | G_SIGNAL_DETAILED,
                  0,
                  NULL, NULL,
                  gabble_marshal_VOID__VOID,
                  G_TYPE_NONE, 0);

  signals[NEW_CONNECTION] =
    g_signal_new ("tube-new-connection",
                  G_OBJECT_CLASS_TYPE (gabble_tube_stream_class),
                  G_SIGNAL_RUN_LAST | G_SIGNAL_DETAILED,
                  0,
                  NULL, NULL,
                  gabble_marshal_VOID__UINT,
                  G_TYPE_NONE, 1, G_TYPE_UINT);

  signals[CLOSED] =
    g_signal_new ("tube-closed",
                  G_OBJECT_CLASS_TYPE (gabble_tube_stream_class),
                  G_SIGNAL_RUN_LAST | G_SIGNAL_DETAILED,
                  0,
                  NULL, NULL,
                  gabble_marshal_VOID__VOID,
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
  GabbleTubeStreamPrivate *priv = GABBLE_TUBE_STREAM_GET_PRIVATE (tube);
  gsize written;
  GIOChannel *channel;
  GIOStatus status;
  GError *error = NULL;

  DEBUG ("received %" G_GSIZE_FORMAT " bytes from bytestream", data->len);

  channel = g_hash_table_lookup (priv->bytestream_to_io_channel, bytestream);
  if (channel == NULL)
    {
      DEBUG ("no IO channel associated with the bytestream");
      return;
    }

  status = g_io_channel_write_chars (channel, data->str, data->len,
      &written, &error);
  if (status == G_IO_STATUS_NORMAL)
    {
      DEBUG ("%" G_GSIZE_FORMAT " bytes written to the socket", written);
    }
  else
    {
      DEBUG ("error writing to socket: %s",
          error ? error->message : "");
    }

  if (error != NULL)
    g_error_free (error);
}

GabbleTubeStream *
gabble_tube_stream_new (GabbleConnection *conn,
                        TpHandle handle,
                        TpHandleType handle_type,
                        TpHandle self_handle,
                        TpHandle initiator,
                        const gchar *service,
                        GHashTable *parameters,
                        guint id)
{
  GabbleTubeStream *obj;
  char *object_path;

  object_path = g_strdup_printf ("%s/StreamTubeChannel_%u_%u",
      conn->parent.object_path, handle, id);

  obj = g_object_new (GABBLE_TYPE_TUBE_STREAM,
      "connection", conn,
      "object-path", object_path,
      "handle", handle,
      "handle-type", handle_type,
      "self-handle", self_handle,
      "initiator-handle", initiator,
      "service", service,
      "parameters", parameters,
      "id", id,
      NULL);

  g_free (object_path);
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
  GabbleTubeStreamPrivate *priv = GABBLE_TUBE_STREAM_GET_PRIVATE (self);

  if (priv->state != GABBLE_TUBE_CHANNEL_STATE_LOCAL_PENDING)
    return TRUE;

  if (!tube_stream_open (self, error))
    {
      gabble_tube_iface_close (GABBLE_TUBE_IFACE (self));
      return FALSE;
    }

  priv->state = GABBLE_TUBE_CHANNEL_STATE_OPEN;

  gabble_svc_channel_interface_tube_emit_tube_channel_state_changed (self,
      GABBLE_TUBE_CHANNEL_STATE_OPEN);

  g_signal_emit (G_OBJECT (self), signals[OPENED], 0);

  return TRUE;
}

/**
 * gabble_tube_stream_close
 *
 * Implements gabble_tube_iface_close on GabbleTubeIface
 */
static void
gabble_tube_stream_close (GabbleTubeIface *tube)
{
  GabbleTubeStream *self = GABBLE_TUBE_STREAM (tube);
  GabbleTubeStreamPrivate *priv = GABBLE_TUBE_STREAM_GET_PRIVATE (self);

  if (priv->closed)
    return;
  priv->closed = TRUE;

  g_hash_table_foreach_remove (priv->fd_to_bytestreams,
      close_each_extra_bytestream, self);

  if (priv->handle_type == TP_HANDLE_TYPE_CONTACT)
    {
      LmMessage *msg;
      const gchar *jid;
      TpHandleRepoIface *contact_repo = tp_base_connection_get_handles (
          (TpBaseConnection *) priv->conn, TP_HANDLE_TYPE_CONTACT);
      gchar *id_str;

      jid = tp_handle_inspect (contact_repo, priv->handle);
      id_str = g_strdup_printf ("%u", priv->id);

      /* Send the close message */
      msg = lm_message_build (jid, LM_MESSAGE_TYPE_MESSAGE,
          '(', "close", "",
            '@', "xmlns", NS_TUBES,
            '@', "tube", id_str,
          ')',
          '(', "amp", "",
            '@', "xmlns", NS_AMP,
            '(', "rule", "",
              '@', "condition", "deliver-at",
              '@', "value", "stored",
              '@', "action", "error",
            ')',
            '(', "rule", "",
              '@', "condition", "match-resource",
              '@', "value", "exact",
              '@', "action", "error",
            ')',
          ')',
          NULL);
      g_free (id_str);

      _gabble_connection_send (priv->conn, msg, NULL);

      lm_message_unref (msg);
    }

  g_signal_emit (G_OBJECT (self), signals[CLOSED], 0);
}

static void
augment_si_accept_iq (LmMessageNode *si,
                      gpointer user_data)
{
  LmMessageNode *tube_node;

  tube_node = lm_message_node_add_child (si, "tube", "");
  lm_message_node_set_attribute (tube_node, "xmlns", NS_TUBES);
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
  GabbleTubeStreamPrivate *priv = GABBLE_TUBE_STREAM_GET_PRIVATE (self);

  if (priv->initiator != priv->self_handle)
    {
      DEBUG ("I'm not the initiator of this tube, can't accept "
          "an extra bytestream");

      gabble_bytestream_iface_close (bytestream, NULL);
      return;
    }

  /* New bytestream, let's connect to the socket */
  if (new_connection_to_socket (self, bytestream))
    {
      TpHandle contact;

      if (priv->state == GABBLE_TUBE_CHANNEL_STATE_REMOTE_PENDING)
        {
          DEBUG ("Received first connection. Tube is now open");
          priv->state = GABBLE_TUBE_CHANNEL_STATE_OPEN;

          gabble_svc_channel_interface_tube_emit_tube_channel_state_changed (
              self, GABBLE_TUBE_CHANNEL_STATE_OPEN);

          g_signal_emit (G_OBJECT (self), signals[OPENED], 0);
        }

      DEBUG ("accept the extra bytestream");

      gabble_bytestream_iface_accept (bytestream, augment_si_accept_iq, self);

      g_object_get (bytestream, "peer-handle", &contact, NULL);

      g_signal_emit (G_OBJECT (self), signals[NEW_CONNECTION], 0, contact);
    }
  else
    {
      gabble_bytestream_iface_close (bytestream, NULL);
    }
}

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
  if (G_VALUE_TYPE (address) != DBUS_TYPE_G_UCHAR_ARRAY)
    {
      g_set_error (error, TP_ERRORS, TP_ERROR_INVALID_ARGUMENT,
          "Unix socket address is supposed to be ay");
      return FALSE;
    }

  array = g_value_get_boxed (address);

  if (array->len > sizeof (dummy.sun_path) - 1)
    {
      g_set_error (error, TP_ERRORS, TP_ERROR_INVALID_ARGUMENT,
          "Unix socket path is too long (max length allowed: %"
          G_GSIZE_FORMAT ")",
          sizeof (dummy.sun_path) - 1);
      return FALSE;
    }

  for (i = 0; i < array->len; i++)
    {
      if (g_array_index (array, gchar , i) == '\0')
        {
          g_set_error (error, TP_ERRORS, TP_ERROR_INVALID_ARGUMENT,
              "Unix socket path can't contain zero bytes");
          return FALSE;
        }
    }

  socket_address = g_string_new_len (array->data, array->len);

  if (g_stat (socket_address->str, &stat_buff) == -1)
  {
    DEBUG ("Error calling stat on socket: %s", g_strerror (errno));

    g_set_error (error, TP_ERRORS, TP_ERROR_INVALID_ARGUMENT, "%s: %s",
        socket_address->str, g_strerror (errno));
    g_string_free (socket_address, TRUE);
    return FALSE;
  }

  if (!S_ISSOCK (stat_buff.st_mode))
  {
    DEBUG ("%s is not a socket", socket_address->str);

    g_set_error (error, TP_ERRORS, TP_ERROR_INVALID_ARGUMENT,
        "%s is not a socket", socket_address->str);
    g_string_free (socket_address, TRUE);
    return FALSE;
  }

  g_string_free (socket_address, TRUE);

  if (access_control != TP_SOCKET_ACCESS_CONTROL_LOCALHOST)
  {
    g_set_error (error, TP_ERRORS, TP_ERROR_INVALID_ARGUMENT,
        "Only the Localhost access control method is supported for Unix"
        " sockets");
    return FALSE;
  }

  return TRUE;
}

static gboolean
check_ip_params (TpSocketAddressType address_type,
                 const GValue *address,
                 TpSocketAccessControl access_control,
                 const GValue *access_control_param,
                 GError **error)
{
  gchar *ip;
  guint port;
  struct addrinfo req, *result = NULL;
  int ret;

  /* Check address type */
  if (address_type == TP_SOCKET_ADDRESS_TYPE_IPV4)
    {
      if (G_VALUE_TYPE (address) != TP_STRUCT_TYPE_SOCKET_ADDRESS_IPV4)
        {
          g_set_error (error, TP_ERRORS, TP_ERROR_INVALID_ARGUMENT,
              "IPv4 socket address is supposed to be sq");
          return FALSE;
        }
    }
  else if (address_type == TP_SOCKET_ADDRESS_TYPE_IPV6)
    {
      if (G_VALUE_TYPE (address) != TP_STRUCT_TYPE_SOCKET_ADDRESS_IPV6)
        {
          g_set_error (error, TP_ERRORS, TP_ERROR_INVALID_ARGUMENT,
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
      g_set_error (error, TP_ERRORS, TP_ERROR_INVALID_ARGUMENT,
          "Invalid address: %s", gai_strerror (ret));
      g_free (ip);
      return FALSE;
    }

  g_free (ip);
  freeaddrinfo (result);

  if (access_control != TP_SOCKET_ACCESS_CONTROL_LOCALHOST)
    {
      g_set_error (error, TP_ERRORS, TP_ERROR_INVALID_ARGUMENT,
          "%s sockets only support localhost control access",
          (address_type == TP_SOCKET_ADDRESS_TYPE_IPV4 ? "IPv4" : "IPv6"));
      return FALSE;
    }

  return TRUE;
}

gboolean
gabble_tube_stream_check_params (TpSocketAddressType address_type,
                                 const GValue *address,
                                 TpSocketAccessControl access_control,
                                 const GValue *access_control_param,
                                 GError **error)
{
  switch (address_type)
    {
      case TP_SOCKET_ADDRESS_TYPE_UNIX:
        return check_unix_params (address_type, address, access_control,
            access_control_param, error);

      case TP_SOCKET_ADDRESS_TYPE_IPV4:
      case TP_SOCKET_ADDRESS_TYPE_IPV6:
        return check_ip_params (address_type, address, access_control,
            access_control_param, error);

      default:
        g_set_error (error, TP_ERRORS, TP_ERROR_NOT_IMPLEMENTED,
            "Address type %d not implemented", address_type);
        return FALSE;
    }
}

/* can be called both from the old tube API and the new tube API */
gboolean
gabble_tube_stream_offer (GabbleTubeStream *self,
                          guint address_type,
                          const GValue *address, guint access_control,
                          const GValue *access_control_param,
                          GError **error)
{
  GabbleTubeStreamPrivate *priv = GABBLE_TUBE_STREAM_GET_PRIVATE (self);
  LmMessageNode *tube_node = NULL;
  LmMessage *msg;
  TpHandleRepoIface *contact_repo;
  const gchar *jid;
  gboolean result;

  g_assert (priv->state == GABBLE_TUBE_CHANNEL_STATE_NOT_OFFERED);

  contact_repo = tp_base_connection_get_handles (
     (TpBaseConnection *) priv->conn, TP_HANDLE_TYPE_CONTACT);

  jid = tp_handle_inspect (contact_repo, priv->handle);

  msg = lm_message_build (jid, LM_MESSAGE_TYPE_MESSAGE,
      '(', "tube", "",
        '*', &tube_node,
        '@', "xmlns", NS_TUBES,
      ')',
      '(', "amp", "",
        '@', "xmlns", NS_AMP,
        '(', "rule", "",
          '@', "condition", "deliver-at",
          '@', "value", "stored",
          '@', "action", "error",
        ')',
        '(', "rule", "",
          '@', "condition", "match-resource",
          '@', "value", "exact",
          '@', "action", "error",
        ')',
      ')',
      NULL);

  g_assert (tube_node != NULL);

  gabble_tube_iface_publish_in_node (GABBLE_TUBE_IFACE (self),
      (TpBaseConnection *) priv->conn, tube_node);

  result = _gabble_connection_send (priv->conn, msg, error);
  if (result)
    {
      priv->state = GABBLE_TUBE_CHANNEL_STATE_REMOTE_PENDING;
    }

  lm_message_unref (msg);
  return result;
}

static void
destroy_socket_control_list (gpointer data)
{
  GArray *tab = data;
  g_array_free (tab, TRUE);
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
  GArray *unix_tab, *ipv4_tab, *ipv6_tab;
  TpSocketAccessControl access_control;

  ret = g_hash_table_new_full (g_direct_hash, g_direct_equal, NULL,
      destroy_socket_control_list);

  /* Socket_Address_Type_Unix */
  unix_tab = g_array_sized_new (FALSE, FALSE, sizeof (TpSocketAccessControl),
      1);
  access_control = TP_SOCKET_ACCESS_CONTROL_LOCALHOST;
  g_array_append_val (unix_tab, access_control);
  g_hash_table_insert (ret, GUINT_TO_POINTER (TP_SOCKET_ADDRESS_TYPE_UNIX),
      unix_tab);

  /* Socket_Address_Type_IPv4 */
  ipv4_tab = g_array_sized_new (FALSE, FALSE, sizeof (TpSocketAccessControl),
      1);
  access_control = TP_SOCKET_ACCESS_CONTROL_LOCALHOST;
  g_array_append_val (ipv4_tab, access_control);
  g_hash_table_insert (ret, GUINT_TO_POINTER (TP_SOCKET_ADDRESS_TYPE_IPV4),
      ipv4_tab);

  /* Socket_Address_Type_IPv6 */
  ipv6_tab = g_array_sized_new (FALSE, FALSE, sizeof (TpSocketAccessControl),
      1);
  access_control = TP_SOCKET_ACCESS_CONTROL_LOCALHOST;
  g_array_append_val (ipv6_tab, access_control);
  g_hash_table_insert (ret, GUINT_TO_POINTER (TP_SOCKET_ADDRESS_TYPE_IPV6),
      ipv6_tab);

  return ret;
}

/* Callback plugged only if the tube has been offered with the new
 * Channel.Type.StreamTube API. */
static void
stream_unix_tube_new_connection_cb (GabbleTubeStream *self,
                                    guint contact,
                                    gpointer user_data)
{
  gabble_svc_channel_type_stream_tube_emit_stream_tube_new_connection (self,
      contact);
}


/**
 * gabble_tube_stream_offer_stream_tube
 *
 * Implements D-Bus method OfferStreamTube
 * on org.freedesktop.Telepathy.Channel.Type.StreamTube
 */
static void
gabble_tube_stream_offer_stream_tube (GabbleSvcChannelTypeStreamTube *iface,
                                      guint address_type,
                                      const GValue *address,
                                      guint access_control,
                                      const GValue *access_control_param,
                                      DBusGMethodInvocation *context)
{
  GabbleTubeStream *self = GABBLE_TUBE_STREAM (iface);
  GabbleTubeStreamPrivate *priv = GABBLE_TUBE_STREAM_GET_PRIVATE (self);
  GError *error = NULL;

  if (priv->state != GABBLE_TUBE_CHANNEL_STATE_NOT_OFFERED)
    {
      g_set_error (&error, TP_ERRORS, TP_ERROR_INVALID_ARGUMENT,
          "Tube is not in the not offered state");
      dbus_g_method_return_error (context, error);
      g_error_free (error);
      return;
    }

  if (!gabble_tube_stream_check_params (address_type, address,
        access_control, access_control_param, &error))
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
  g_assert (priv->access_control_param == NULL);
  priv->access_control_param = tp_g_value_slice_dup (access_control_param);

  if (priv->handle_type == TP_HANDLE_TYPE_CONTACT)
    {
      /* Stream initiation */
      if (!gabble_tube_stream_offer (self, address_type,
          address, access_control, access_control_param, &error))
        {
          gabble_tube_stream_close (GABBLE_TUBE_IFACE (self));

          dbus_g_method_return_error (context, error);

          g_error_free (error);
          return;
        }

      gabble_svc_channel_interface_tube_emit_tube_channel_state_changed (
          self, GABBLE_TUBE_CHANNEL_STATE_REMOTE_PENDING);
    }

  g_signal_connect (self, "tube-new-connection",
      G_CALLBACK (stream_unix_tube_new_connection_cb), self);

  gabble_svc_channel_type_stream_tube_return_from_offer_stream_tube (context);
}

/**
 * gabble_tube_stream_accept_stream_tube
 *
 * Implements D-Bus method AcceptStreamTube
 * on org.freedesktop.Telepathy.Channel.Type.StreamTube
 */
static void
gabble_tube_stream_accept_stream_tube (GabbleSvcChannelTypeStreamTube *iface,
                                       guint address_type,
                                       guint access_control,
                                       const GValue *access_control_param,
                                       DBusGMethodInvocation *context)
{
  GabbleTubeStream *self = GABBLE_TUBE_STREAM (iface);
  GabbleTubeStreamPrivate *priv = GABBLE_TUBE_STREAM_GET_PRIVATE (self);
  GError *error = NULL;

  if (address_type != TP_SOCKET_ADDRESS_TYPE_UNIX &&
      address_type != TP_SOCKET_ADDRESS_TYPE_IPV4 &&
      address_type != TP_SOCKET_ADDRESS_TYPE_IPV6)
    {
      error = g_error_new (TP_ERRORS, TP_ERROR_NOT_IMPLEMENTED,
          "Address type %d not implemented", address_type);

      dbus_g_method_return_error (context, error);
      g_error_free (error);
      return;
    }

  if (access_control != TP_SOCKET_ACCESS_CONTROL_LOCALHOST)
    {
      GError e = { TP_ERRORS, TP_ERROR_INVALID_ARGUMENT,
          "Only the Localhost access control method is implemented"
            " by Gabble" };

      dbus_g_method_return_error (context, &e);
      return;
    }

  priv->access_control = access_control;
  g_assert (priv->access_control_param == NULL);
  priv->access_control_param = tp_g_value_slice_dup (access_control_param);

  if (priv->state != GABBLE_TUBE_CHANNEL_STATE_LOCAL_PENDING)
    {
      GError e = { TP_ERRORS, TP_ERROR_INVALID_ARGUMENT,
          "Tube is not in the local pending state" };

      dbus_g_method_return_error (context, &e);
      return;
    }

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

  gabble_svc_channel_type_stream_tube_return_from_accept_stream_tube (context,
      priv->address);
}

/**
 * gabble_tube_stream_get_stream_tube_socket_address
 *
 * Implements D-Bus method GetStreamTubeSocketAddress
 * on org.freedesktop.Telepathy.Channel.Type.StreamTube
 */
static void
gabble_tube_stream_get_stream_tube_socket_address (
    GabbleSvcChannelTypeStreamTube *iface,
    DBusGMethodInvocation *context)
{
  GabbleTubeStream *self = GABBLE_TUBE_STREAM (iface);
  GabbleTubeStreamPrivate *priv = GABBLE_TUBE_STREAM_GET_PRIVATE (self);

  gabble_svc_channel_type_stream_tube_return_from_get_stream_tube_socket_address
      (context, priv->address_type, priv->address);
}

/**
 * gabble_tube_stream_close_async:
 *
 * Implements D-Bus method Close
 * on interface org.freedesktop.Telepathy.Channel
 */
static void
gabble_tube_stream_close_async (TpSvcChannel *iface,
                                  DBusGMethodInvocation *context)
{
  gabble_tube_stream_close (GABBLE_TUBE_IFACE (iface));
  tp_svc_channel_return_from_close (context);
}

/**
 * gabble_tube_stream_get_channel_type
 *
 * Implements D-Bus method GetChannelType
 * on interface org.freedesktop.Telepathy.Channel
 */
static void
gabble_tube_stream_get_channel_type (TpSvcChannel *iface,
                                       DBusGMethodInvocation *context)
{
  tp_svc_channel_return_from_get_channel_type (context,
      GABBLE_IFACE_CHANNEL_TYPE_STREAM_TUBE);
}

/**
 * gabble_tube_stream_get_handle
 *
 * Implements D-Bus method GetHandle
 * on interface org.freedesktop.Telepathy.Channel
 */
static void
gabble_tube_stream_get_handle (TpSvcChannel *iface,
                                 DBusGMethodInvocation *context)
{
  GabbleTubeStream *self = GABBLE_TUBE_STREAM (iface);
  GabbleTubeStreamPrivate *priv = GABBLE_TUBE_STREAM_GET_PRIVATE (self);

  tp_svc_channel_return_from_get_handle (context, priv->handle_type,
      priv->handle);
}

/**
 * gabble_tube_stream_get_interfaces
 *
 * Implements D-Bus method GetInterfaces
 * on interface org.freedesktop.Telepathy.Channel
 */
static void
gabble_tube_stream_get_interfaces (TpSvcChannel *iface,
                                   DBusGMethodInvocation *context)
{
  GabbleTubeStream *self = GABBLE_TUBE_STREAM (iface);
  GabbleTubeStreamPrivate *priv = GABBLE_TUBE_STREAM_GET_PRIVATE (self);

  if (priv->handle_type == TP_HANDLE_TYPE_CONTACT)
    {
      /* omit the Group interface */
      tp_svc_channel_return_from_get_interfaces (context,
          gabble_tube_stream_interfaces + 1);
    }
  else
    {
      tp_svc_channel_return_from_get_interfaces (context,
          gabble_tube_stream_interfaces);
    }
}

static void
channel_iface_init (gpointer g_iface,
                    gpointer iface_data)
{
  TpSvcChannelClass *klass = (TpSvcChannelClass *) g_iface;

#define IMPLEMENT(x, suffix) tp_svc_channel_implement_##x (\
    klass, gabble_tube_stream_##x##suffix)
  IMPLEMENT(close,_async);
  IMPLEMENT(get_channel_type,);
  IMPLEMENT(get_handle,);
  IMPLEMENT(get_interfaces,);
#undef IMPLEMENT
}

static void
tube_iface_init (gpointer g_iface,
                 gpointer iface_data)
{
  GabbleTubeIfaceClass *klass = (GabbleTubeIfaceClass *) g_iface;

  klass->accept = gabble_tube_stream_accept;
  klass->close = gabble_tube_stream_close;
  klass->add_bytestream = gabble_tube_stream_add_bytestream;
}

static void
streamtube_iface_init (gpointer g_iface,
                       gpointer iface_data)
{
  GabbleSvcChannelTypeStreamTubeClass *klass =
      (GabbleSvcChannelTypeStreamTubeClass *) g_iface;

#define IMPLEMENT(x) gabble_svc_channel_type_stream_tube_implement_##x (\
    klass, gabble_tube_stream_##x)
  IMPLEMENT(offer_stream_tube);
  IMPLEMENT(accept_stream_tube);
  IMPLEMENT(get_stream_tube_socket_address);
#undef IMPLEMENT
}
