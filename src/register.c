/*
 * gabble-register.c - Source for Gabble account registration
 *
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
#include "register.h"

#define DBUS_API_SUBJECT_TO_CHANGE

#include <string.h>
#include <ctype.h>

#include <dbus/dbus-glib.h>
#include <dbus/dbus-glib-lowlevel.h>
#include <telepathy-glib/dbus.h>
#include <telepathy-glib/errors.h>

#define DEBUG_FLAG GABBLE_DEBUG_CONNECTION
#include "connection.h"
#include "debug.h"
#include "error.h"
#include "gabble-signals-marshal.h"
#include "namespaces.h"
#include "util.h"

/* signal enum */
enum
{
  FINISHED,
  LAST_SIGNAL
};

static guint signals[LAST_SIGNAL] = { 0, };

/* properties */
enum
{
  PROP_CONNECTION = 1,
  LAST_PROPERTY
};

G_DEFINE_TYPE(GabbleRegister, gabble_register, G_TYPE_OBJECT);

/* private structure */
struct _GabbleRegisterPrivate
{
  GabbleConnection *conn;

  gboolean dispose_has_run;
};

#define GABBLE_REGISTER_GET_PRIVATE(o) \
  (G_TYPE_INSTANCE_GET_PRIVATE ((o), GABBLE_TYPE_REGISTER,\
                                GabbleRegisterPrivate))

static void
gabble_register_init (GabbleRegister *obj)
{
  obj->priv = G_TYPE_INSTANCE_GET_PRIVATE (obj, GABBLE_TYPE_REGISTER,
      GabbleRegisterPrivate);
}

static void gabble_register_set_property (GObject *object, guint property_id,
    const GValue *value, GParamSpec *pspec);
static void gabble_register_get_property (GObject *object, guint property_id,
    GValue *value, GParamSpec *pspec);
static void gabble_register_dispose (GObject *object);
static void gabble_register_finalize (GObject *object);

static void
gabble_register_class_init (GabbleRegisterClass *gabble_register_class)
{
  GObjectClass *object_class = G_OBJECT_CLASS (gabble_register_class);
  GParamSpec *param_spec;

  g_type_class_add_private (gabble_register_class,
      sizeof (GabbleRegisterPrivate));

  object_class->get_property = gabble_register_get_property;
  object_class->set_property = gabble_register_set_property;

  object_class->dispose = gabble_register_dispose;
  object_class->finalize = gabble_register_finalize;

  param_spec = g_param_spec_object ("connection", "GabbleConnection object",
      "Gabble connection object that owns this GabbleRegister object.",
      GABBLE_TYPE_CONNECTION,
      G_PARAM_CONSTRUCT_ONLY | G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS);
  g_object_class_install_property (object_class, PROP_CONNECTION, param_spec);

  signals[FINISHED] =
    g_signal_new ("finished",
                  G_OBJECT_CLASS_TYPE (gabble_register_class),
                  G_SIGNAL_RUN_LAST | G_SIGNAL_DETAILED,
                  0,
                  NULL, NULL,
                  gabble_marshal_VOID__BOOLEAN_INT_STRING,
                  G_TYPE_NONE, 3, G_TYPE_BOOLEAN, G_TYPE_INT, G_TYPE_STRING);
}

static void
gabble_register_get_property (GObject    *object,
                              guint       property_id,
                              GValue     *value,
                              GParamSpec *pspec)
{
  GabbleRegister *chan = GABBLE_REGISTER (object);
  GabbleRegisterPrivate *priv = GABBLE_REGISTER_GET_PRIVATE (chan);

  switch (property_id) {
    case PROP_CONNECTION:
      g_value_set_object (value, priv->conn);
      break;
    default:
      G_OBJECT_WARN_INVALID_PROPERTY_ID (object, property_id, pspec);
      break;
  }
}

static void
gabble_register_set_property (GObject     *object,
                              guint        property_id,
                              const GValue *value,
                              GParamSpec   *pspec)
{
  GabbleRegister *chan = GABBLE_REGISTER (object);
  GabbleRegisterPrivate *priv = GABBLE_REGISTER_GET_PRIVATE (chan);

  switch (property_id) {
    case PROP_CONNECTION:
      priv->conn = g_value_get_object (value);
      break;
    default:
      G_OBJECT_WARN_INVALID_PROPERTY_ID (object, property_id, pspec);
      break;
  }
}

void
gabble_register_dispose (GObject *object)
{
  GabbleRegister *self = GABBLE_REGISTER (object);
  GabbleRegisterPrivate *priv = GABBLE_REGISTER_GET_PRIVATE (self);

  if (priv->dispose_has_run)
    return;

  priv->dispose_has_run = TRUE;

  DEBUG ("dispose called");

  if (G_OBJECT_CLASS (gabble_register_parent_class)->dispose)
    G_OBJECT_CLASS (gabble_register_parent_class)->dispose (object);
}

void
gabble_register_finalize (GObject *object)
{
  DEBUG ("called with %p", object);

  G_OBJECT_CLASS (gabble_register_parent_class)->finalize (object);
}

/**
 * gabble_register_new:
 *
 * Creates an object to use for account registration.
 *
 * @conn: The #GabbleConnection to register an account on
 */
GabbleRegister *
gabble_register_new (GabbleConnection *conn)
{
  g_return_val_if_fail (GABBLE_IS_CONNECTION (conn), NULL);
  return GABBLE_REGISTER (g_object_new (GABBLE_TYPE_REGISTER,
        "connection", conn, NULL));
}

static LmHandlerResult
set_reply_cb (GabbleConnection *conn,
              LmMessage *sent_msg,
              LmMessage *reply_msg,
              GObject *object,
              gpointer user_data)
{
  if (lm_message_get_sub_type (reply_msg) != LM_MESSAGE_SUB_TYPE_RESULT)
    {
      LmMessageNode *node;
      gint code = TP_ERROR_NOT_AVAILABLE;
      GString *msg;

      msg = g_string_sized_new (30);
      g_string_append (msg, "Request failed");

      node = lm_message_node_get_child (reply_msg->node, "error");
      if (node)
        {
          GabbleXmppError error;

          error = gabble_xmpp_error_from_node (node, NULL);
          if (error == XMPP_ERROR_CONFLICT)
            {
              code = TP_ERROR_NOT_YOURS;
            }

          g_string_append_printf (msg, ": %s",
              gabble_xmpp_error_string (error));
        }

      g_signal_emit (object, signals[FINISHED], 0, FALSE, code, msg->str);
      g_string_free (msg, TRUE);
    }
  else
    {
      g_signal_emit (object, signals[FINISHED], 0, TRUE, -1, NULL);
    }

  return LM_HANDLER_RESULT_REMOVE_MESSAGE;
}

static LmHandlerResult
get_reply_cb (GabbleConnection *conn,
              LmMessage *sent_msg,
              LmMessage *reply_msg,
              GObject *object,
              gpointer user_data)
{
  GabbleRegister *reg = GABBLE_REGISTER (object);
  GabbleRegisterPrivate *priv = GABBLE_REGISTER_GET_PRIVATE (reg);
  GError *error = NULL;
  LmMessage *msg = NULL;
  LmMessageNode *query_node;
  gchar *username, *password;
  gboolean username_required = FALSE, password_required = FALSE;
  NodeIter i;

  if (lm_message_get_sub_type (reply_msg) != LM_MESSAGE_SUB_TYPE_RESULT)
    {
      error = g_error_new (TP_ERRORS, TP_ERROR_NOT_AVAILABLE,
          "Server doesn't support " NS_REGISTER);
      goto OUT;
    }

  query_node = lm_message_node_get_child_with_namespace (reply_msg->node,
      "query", NS_REGISTER);

  if (query_node == NULL)
    {
      error = g_error_new (TP_ERRORS, TP_ERROR_NOT_AVAILABLE,
          "malformed reply from server");
      goto OUT;
    }

  for (i = node_iter (query_node); i; i = node_iter_next (i))
    {
      LmMessageNode *child = node_iter_data (i);
      const gchar *n = lm_message_node_get_name (child);

      if (!tp_strdiff (n, "username"))
        {
          username_required = TRUE;
        }
      else if (!tp_strdiff (n, "password"))
        {
          password_required = TRUE;
        }
      else if (tp_strdiff (n, "instructions"))
        {
          error = g_error_new (TP_ERRORS, TP_ERROR_NOT_AVAILABLE,
              "server requires information (%s) that Gabble can't supply",
              n);
          goto OUT;
        }
    }

  if (!username_required || !password_required)
    {
      error = g_error_new (TP_ERRORS, TP_ERROR_NOT_AVAILABLE,
          "server inexplicably doesn't require username and password");

      goto OUT;
    }

  /* craft a reply */
  msg = lm_message_new_with_sub_type (NULL, LM_MESSAGE_TYPE_IQ,
                                      LM_MESSAGE_SUB_TYPE_SET);

  query_node = lm_message_node_add_child (msg->node, "query", NULL);
  lm_message_node_set_attribute (query_node, "xmlns", NS_REGISTER);

  g_object_get (priv->conn,
      "username", &username,
      "password", &password,
      NULL);

  lm_message_node_add_child (query_node, "username", username);
  lm_message_node_add_child (query_node, "password", password);

  g_free (username);
  g_free (password);

  _gabble_connection_send_with_reply (priv->conn, msg, set_reply_cb,
      G_OBJECT (reg), NULL, &error);

OUT:
  if (error != NULL)
    {
      DEBUG ("failed: %s", error->message);
      g_signal_emit (reg, signals[FINISHED], 0, FALSE, error->code,
          error->message);
      g_error_free (error);
    }

  if (msg)
    lm_message_unref (msg);

  return LM_HANDLER_RESULT_REMOVE_MESSAGE;
}

/**
 * gabble_register_start:
 *
 * Start account registration.
 *
 * @reg: The #GabbleRegister object performing the registration
 */
void gabble_register_start (GabbleRegister *reg)
{
  GabbleRegisterPrivate *priv = GABBLE_REGISTER_GET_PRIVATE (reg);
  LmMessage *msg;
  LmMessageNode *node;
  GError *error = NULL;
  GabbleConnectionMsgReplyFunc handler;

  msg = lm_message_new_with_sub_type (NULL, LM_MESSAGE_TYPE_IQ,
                                      LM_MESSAGE_SUB_TYPE_GET);
  node = lm_message_node_add_child (msg->node, "query", NULL);

  lm_message_node_set_attribute (node, "xmlns", NS_REGISTER);
  handler = get_reply_cb;

  if (!_gabble_connection_send_with_reply (priv->conn, msg, handler,
                                           G_OBJECT (reg), NULL, &error))
    {
      g_signal_emit (reg, signals[FINISHED], 0, FALSE, error->code,
                     error->message);
      g_error_free (error);
    }

  lm_message_unref (msg);
}
