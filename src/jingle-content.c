/*
 * gabble-jingle-content.c - Source for GabbleJingleContent
 * Copyright (C) 2008 Collabora Ltd.
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

#include "jingle-content.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <glib.h>

#include <loudmouth/loudmouth.h>

#define DEBUG_FLAG GABBLE_DEBUG_MEDIA

#include "connection.h"
#include "debug.h"
#include "jingle-factory.h"
#include "jingle-session.h"
#include "jingle-transport-iface.h"
#include "jingle-transport-google.h"
#include "jingle-media-rtp.h"
#include "namespaces.h"
#include "util.h"
#include "gabble-signals-marshal.h"

/* signal enum */
enum
{
  READY,
  NEW_CANDIDATES,
  REMOVED,
  NEW_SHARE_CHANNEL,
  COMPLETED,
  LAST_SIGNAL
};

static guint signals[LAST_SIGNAL] = {0};

/* properties */
enum
{
  PROP_CONNECTION = 1,
  PROP_SESSION,
  PROP_CONTENT_NS,
  PROP_TRANSPORT_NS,
  PROP_NAME,
  PROP_SENDERS,
  PROP_STATE,
  PROP_DISPOSITION,
  PROP_LOCALLY_CREATED,
  LAST_PROPERTY
};

struct _GabbleJingleContentPrivate
{
  gchar *name;
  gchar *creator;
  gboolean created_by_us;
  JingleContentState state;
  JingleContentSenders senders;

  gchar *content_ns;
  gchar *transport_ns;
  gchar *disposition;

  GabbleJingleTransportIface *transport;

  /* Whether we've got the codecs (intersection) ready. */
  gboolean media_ready;

  /* Whether we have at least one local candidate. */
  gboolean have_local_candidates;

  guint gtalk4_event_id;
  guint last_share_channel_component_id;

  gboolean dispose_has_run;
};

#define DEFAULT_CONTENT_TIMEOUT 60000

/* lookup tables */

G_DEFINE_TYPE(GabbleJingleContent, gabble_jingle_content, G_TYPE_OBJECT);

static void new_transport_candidates_cb (GabbleJingleTransportIface *trans,
    GList *candidates, GabbleJingleContent *content);
static void _maybe_ready (GabbleJingleContent *self);
static void transport_created (GabbleJingleContent *c);

static void
gabble_jingle_content_init (GabbleJingleContent *obj)
{
  GabbleJingleContentPrivate *priv =
     G_TYPE_INSTANCE_GET_PRIVATE (obj, GABBLE_TYPE_JINGLE_CONTENT,
         GabbleJingleContentPrivate);
  obj->priv = priv;

  DEBUG ("%p", obj);

  priv->state = JINGLE_CONTENT_STATE_EMPTY;
  priv->created_by_us = TRUE;
  priv->media_ready = FALSE;
  priv->have_local_candidates = FALSE;
  priv->gtalk4_event_id = 0;
  priv->dispose_has_run = FALSE;

  obj->conn = NULL;
  obj->session = NULL;
}

static void
gabble_jingle_content_dispose (GObject *object)
{
  GabbleJingleContent *content = GABBLE_JINGLE_CONTENT (object);
  GabbleJingleContentPrivate *priv = content->priv;

  if (priv->dispose_has_run)
    return;

  DEBUG ("%p", object);
  priv->dispose_has_run = TRUE;

  if (priv->gtalk4_event_id != 0)
    {
      g_source_remove (priv->gtalk4_event_id);
      priv->gtalk4_event_id = 0;
    }

  g_free (priv->name);
  priv->name = NULL;

  g_free (priv->creator);
  priv->creator = NULL;

  g_free (priv->content_ns);
  priv->content_ns = NULL;

  g_free (priv->transport_ns);
  priv->transport_ns = NULL;

  g_free (priv->disposition);
  priv->disposition = NULL;

  if (G_OBJECT_CLASS (gabble_jingle_content_parent_class)->dispose)
    G_OBJECT_CLASS (gabble_jingle_content_parent_class)->dispose (object);
}

static void
gabble_jingle_content_get_property (GObject *object,
                                    guint property_id,
                                    GValue *value,
                                    GParamSpec *pspec)
{
  GabbleJingleContent *self = GABBLE_JINGLE_CONTENT (object);
  GabbleJingleContentPrivate *priv = self->priv;

  switch (property_id) {
    case PROP_CONNECTION:
      g_value_set_object (value, self->conn);
      break;
    case PROP_SESSION:
      g_value_set_object (value, self->session);
      break;
    case PROP_NAME:
      g_value_set_string (value, priv->name);
      break;
    case PROP_SENDERS:
      g_value_set_uint (value, priv->senders);
      break;
    case PROP_STATE:
      g_value_set_uint (value, priv->state);
      break;
    case PROP_CONTENT_NS:
      g_value_set_string (value, priv->content_ns);
      break;
    case PROP_TRANSPORT_NS:
      g_value_set_string (value, priv->transport_ns);
      break;
    case PROP_DISPOSITION:
      g_value_set_string (value, priv->disposition);
      break;
    case PROP_LOCALLY_CREATED:
      g_value_set_boolean (value, priv->created_by_us);
      break;
    default:
      G_OBJECT_WARN_INVALID_PROPERTY_ID (object, property_id, pspec);
      break;
  }
}

static void
gabble_jingle_content_set_property (GObject *object,
                                    guint property_id,
                                    const GValue *value,
                                    GParamSpec *pspec)
{
  GabbleJingleContent *self = GABBLE_JINGLE_CONTENT (object);
  GabbleJingleContentPrivate *priv = self->priv;

  switch (property_id) {
    case PROP_CONNECTION:
      self->conn = g_value_get_object (value);
      break;
    case PROP_SESSION:
      self->session = g_value_get_object (value);
      break;
    case PROP_CONTENT_NS:
      g_free (priv->content_ns);
      priv->content_ns = g_value_dup_string (value);
      break;
    case PROP_TRANSPORT_NS:
      g_free (priv->transport_ns);
      priv->transport_ns = g_value_dup_string (value);

      /* We can't switch transports. */
      g_assert (priv->transport == NULL);

      if (priv->transport_ns != NULL)
        {
          GType transport_type = gabble_jingle_factory_lookup_transport (
              self->conn->jingle_factory, priv->transport_ns);

          g_assert (transport_type != 0);

          priv->transport = gabble_jingle_transport_iface_new (transport_type,
              self, priv->transport_ns);

          g_signal_connect (priv->transport, "new-candidates",
              (GCallback) new_transport_candidates_cb, self);

          transport_created (self);
        }
      break;
    case PROP_NAME:
      /* can't rename */
      g_assert (priv->name == NULL);

      priv->name = g_value_dup_string (value);
      break;
    case PROP_SENDERS:
      priv->senders = g_value_get_uint (value);
      break;
    case PROP_STATE:
      priv->state = g_value_get_uint (value);
      break;
    case PROP_DISPOSITION:
      g_assert (priv->disposition == NULL);
      priv->disposition = g_value_dup_string (value);
      break;
    default:
      G_OBJECT_WARN_INVALID_PROPERTY_ID (object, property_id, pspec);
      break;
  }
}

static JingleContentSenders
get_default_senders_real (GabbleJingleContent *c)
{
  return JINGLE_CONTENT_SENDERS_BOTH;
}


static void
gabble_jingle_content_class_init (GabbleJingleContentClass *cls)
{
  GParamSpec *param_spec;
  GObjectClass *object_class = G_OBJECT_CLASS (cls);

  g_type_class_add_private (cls, sizeof (GabbleJingleContentPrivate));

  object_class->get_property = gabble_jingle_content_get_property;
  object_class->set_property = gabble_jingle_content_set_property;
  object_class->dispose = gabble_jingle_content_dispose;

  cls->get_default_senders = get_default_senders_real;

  /* property definitions */
  param_spec = g_param_spec_object ("connection", "GabbleConnection object",
      "Gabble connection object used for exchanging messages.",
      GABBLE_TYPE_CONNECTION,
      G_PARAM_CONSTRUCT_ONLY | G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS);
  g_object_class_install_property (object_class, PROP_CONNECTION, param_spec);

  param_spec = g_param_spec_object ("session", "GabbleJingleSession object",
      "Jingle session object that owns this content.",
      GABBLE_TYPE_JINGLE_SESSION,
      G_PARAM_CONSTRUCT_ONLY | G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS);
  g_object_class_install_property (object_class, PROP_SESSION, param_spec);

  param_spec = g_param_spec_string ("name", "Content name",
      "A unique content name in the session.",
      NULL,
      G_PARAM_CONSTRUCT_ONLY | G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS);
  g_object_class_install_property (object_class, PROP_NAME, param_spec);

  param_spec = g_param_spec_string ("content-ns", "Content namespace",
      "Namespace identifying the content type.",
      NULL,
      G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS);
  g_object_class_install_property (object_class, PROP_CONTENT_NS, param_spec);

  param_spec = g_param_spec_string ("transport-ns", "Transport namespace",
      "Namespace identifying the transport type.",
      NULL,
      G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS);
  g_object_class_install_property (object_class, PROP_TRANSPORT_NS, param_spec);

  param_spec = g_param_spec_uint ("senders", "Stream senders",
      "Valid senders for the stream.",
      0, G_MAXUINT32, JINGLE_CONTENT_SENDERS_NONE,
      G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS);
  g_object_class_install_property (object_class, PROP_SENDERS, param_spec);

  param_spec = g_param_spec_uint ("state", "Content state",
      "The current state that the content is in.",
      0, G_MAXUINT32, JINGLE_CONTENT_STATE_EMPTY,
      G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS);
  g_object_class_install_property (object_class, PROP_STATE, param_spec);

  param_spec = g_param_spec_string ("disposition", "Content disposition",
      "Distinguishes between 'session' and other contents.",
      NULL,
      G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS);
  g_object_class_install_property (object_class, PROP_DISPOSITION, param_spec);

  param_spec = g_param_spec_boolean ("locally-created", "Locally created",
      "True if the content was created by the local client.",
      FALSE,
      G_PARAM_READABLE | G_PARAM_STATIC_STRINGS);
  g_object_class_install_property (object_class, PROP_LOCALLY_CREATED, param_spec);

  /* signal definitions */

  signals[READY] = g_signal_new ("ready",
    G_OBJECT_CLASS_TYPE (cls),
    G_SIGNAL_RUN_LAST | G_SIGNAL_DETAILED,
    0,
    NULL, NULL,
    g_cclosure_marshal_VOID__VOID,
    G_TYPE_NONE, 0);

  signals[NEW_CANDIDATES] = g_signal_new (
    "new-candidates",
    G_TYPE_FROM_CLASS (cls),
    G_SIGNAL_RUN_LAST,
    0,
    NULL, NULL,
    g_cclosure_marshal_VOID__POINTER, G_TYPE_NONE, 1, G_TYPE_POINTER);

  signals[NEW_SHARE_CHANNEL] = g_signal_new (
    "new-share-channel",
    G_TYPE_FROM_CLASS (cls),
    G_SIGNAL_RUN_LAST,
    0,
    NULL, NULL,
    gabble_marshal_VOID__STRING_UINT,
    G_TYPE_NONE,
    2,
    G_TYPE_STRING, G_TYPE_UINT);

  signals[COMPLETED] = g_signal_new (
    "completed",
    G_TYPE_FROM_CLASS (cls),
    G_SIGNAL_RUN_LAST,
    0,
    NULL, NULL,
    g_cclosure_marshal_VOID__VOID,
    G_TYPE_NONE,
    0);

  /* This signal serves as notification that the GabbleJingleContent is now
   * meaningless; everything holding a reference should drop it after receiving
   * 'removed'.
   */
  signals[REMOVED] = g_signal_new ("removed",
    G_OBJECT_CLASS_TYPE (cls),
    G_SIGNAL_RUN_LAST | G_SIGNAL_DETAILED,
    0,
    NULL, NULL,
    g_cclosure_marshal_VOID__VOID,
    G_TYPE_NONE, 0);
}


static JingleContentSenders
get_default_senders (GabbleJingleContent *c)
{
  JingleContentSenders (*virtual_method)(GabbleJingleContent *) = \
      GABBLE_JINGLE_CONTENT_GET_CLASS (c)->get_default_senders;

  g_assert (virtual_method != NULL);
  return virtual_method (c);
}


static JingleContentSenders
parse_senders (const gchar *txt)
{
  if (txt == NULL)
      return JINGLE_CONTENT_SENDERS_NONE;

  if (!tp_strdiff (txt, "initiator"))
      return JINGLE_CONTENT_SENDERS_INITIATOR;
  else if (!tp_strdiff (txt, "responder"))
      return JINGLE_CONTENT_SENDERS_RESPONDER;
  else if (!tp_strdiff (txt, "both"))
      return JINGLE_CONTENT_SENDERS_BOTH;

  return JINGLE_CONTENT_SENDERS_NONE;
}

static const gchar *
produce_senders (JingleContentSenders senders)
{
  switch (senders) {
    case JINGLE_CONTENT_SENDERS_INITIATOR:
      return "initiator";
    case JINGLE_CONTENT_SENDERS_RESPONDER:
      return "responder";
    case JINGLE_CONTENT_SENDERS_BOTH:
      return "both";
    default:
      DEBUG ("invalid content senders %u", senders);
      g_assert_not_reached ();
  }

  /* to make gcc not complain */
  return NULL;
}


#define SET_BAD_REQ(txt) g_set_error (error, GABBLE_XMPP_ERROR, XMPP_ERROR_BAD_REQUEST, txt)

static void
new_transport_candidates_cb (GabbleJingleTransportIface *trans,
    GList *candidates, GabbleJingleContent *content)
{
  /* just pass the signal on */
  g_signal_emit (content, signals[NEW_CANDIDATES], 0, candidates);
}

static void
transport_created (GabbleJingleContent *c)
{
  void (*virtual_method)(GabbleJingleContent *, GabbleJingleTransportIface *) = \
      GABBLE_JINGLE_CONTENT_GET_CLASS (c)->transport_created;

  if (virtual_method != NULL)
    virtual_method (c, c->priv->transport);
}


static void
parse_description (GabbleJingleContent *c, LmMessageNode *desc_node,
    GError **error)
{
  void (*virtual_method)(GabbleJingleContent *, LmMessageNode *,
      GError **) = GABBLE_JINGLE_CONTENT_GET_CLASS (c)->parse_description;

  g_assert (virtual_method != NULL);
  virtual_method (c, desc_node, error);
}

static gboolean
send_gtalk4_transport_accept (gpointer user_data)
{
  GabbleJingleContent *c = GABBLE_JINGLE_CONTENT (user_data);
  GabbleJingleContentPrivate *priv = c->priv;
  LmMessageNode *sess_node, *tnode;
  LmMessage *msg = gabble_jingle_session_new_message (c->session,
      JINGLE_ACTION_TRANSPORT_ACCEPT, &sess_node);

  DEBUG ("Sending Gtalk4 'transport-accept' message to peer");
  tnode = lm_message_node_add_child (sess_node, "transport", NULL);
  lm_message_node_set_attribute (tnode, "xmlns", priv->transport_ns);

  gabble_jingle_session_send (c->session, msg, NULL, NULL);

  return FALSE;
}

void
gabble_jingle_content_parse_add (GabbleJingleContent *c,
    LmMessageNode *content_node, gboolean google_mode, GError **error)
{
  GabbleJingleContentPrivate *priv = c->priv;
  const gchar *name, *creator, *senders, *disposition;
  LmMessageNode *trans_node, *desc_node;
  GType transport_type = 0;
  GabbleJingleTransportIface *trans = NULL;
  JingleDialect dialect = gabble_jingle_session_get_dialect (c->session);

  desc_node = lm_message_node_get_child_any_ns (content_node, "description");
  trans_node = lm_message_node_get_child_any_ns (content_node, "transport");
  creator = lm_message_node_get_attribute (content_node, "creator");
  name = lm_message_node_get_attribute (content_node, "name");
  senders = lm_message_node_get_attribute (content_node, "senders");

  g_assert (priv->transport_ns == NULL);

  if (google_mode)
    {
      if (creator == NULL)
          creator = "initiator";

      /* the google protocols don't give the contents names, so put in a dummy
       * value if none was set by the session*/
      if (priv->name == NULL)
        name = priv->name = g_strdup ("gtalk");
      else
        name = priv->name;

      if (trans_node == NULL)
        {
          /* gtalk lj0.3 assumes google-p2p transport */
          DEBUG ("detected GTalk3 dialect");

          dialect = JINGLE_DIALECT_GTALK3;
          g_object_set (c->session, "dialect", JINGLE_DIALECT_GTALK3, NULL);
          transport_type = gabble_jingle_factory_lookup_transport (
              c->conn->jingle_factory, "");

          /* in practice we do support gtalk-p2p, so this can't happen */
          if (G_UNLIKELY (transport_type == 0))
            {
              SET_BAD_REQ ("gtalk-p2p transport unsupported");
              return;
            }

          priv->transport_ns = g_strdup ("");
        }
    }
  else
    {
      if ((trans_node == NULL) || (creator == NULL) || (name == NULL))
        {
          SET_BAD_REQ ("missing required content attributes or elements");
          return;
        }

      /* In proper protocols the name comes from the stanza */
      g_assert (priv->name == NULL);
      priv->name = g_strdup (name);
    }

  /* if we didn't set it to google-p2p implicitly already, detect it */
  if (transport_type == 0)
    {
      const gchar *ns = lm_message_node_get_namespace (trans_node);

      transport_type = gabble_jingle_factory_lookup_transport (
          c->conn->jingle_factory, ns);

      if (transport_type == 0)
        {
          SET_BAD_REQ ("unsupported content transport");
          return;
        }

      priv->transport_ns = g_strdup (ns);
    }

  priv->created_by_us = FALSE;
  if (senders == NULL)
    priv->senders = get_default_senders (c);
  else
    priv->senders = parse_senders (senders);

  if (priv->senders == JINGLE_CONTENT_SENDERS_NONE)
    {
      SET_BAD_REQ ("invalid content senders");
      return;
    }

  parse_description (c, desc_node, error);
  if (*error != NULL)
      return;

  disposition = lm_message_node_get_attribute (content_node, "disposition");
  if (disposition == NULL)
      disposition = "session";

  if (tp_strdiff (disposition, priv->disposition))
    {
      g_free (priv->disposition);
      priv->disposition = g_strdup (disposition);
    }

  DEBUG ("content creating new transport type %s", g_type_name (transport_type));

  trans = gabble_jingle_transport_iface_new (transport_type,
      c, priv->transport_ns);

  g_signal_connect (trans, "new-candidates",
      (GCallback) new_transport_candidates_cb, c);

  /* Depending on transport, there may be initial candidates specified here */
  if (trans_node != NULL)
    {
      gabble_jingle_transport_iface_parse_candidates (trans, trans_node, error);
      if (*error)
        {
          g_object_unref (trans);
          return;
        }
    }

  g_assert (priv->transport == NULL);
  priv->transport = trans;
  transport_created (c);

  g_assert (priv->creator == NULL);
  priv->creator = g_strdup (creator);

  priv->state = JINGLE_CONTENT_STATE_NEW;

  /* GTalk4 seems to require "transport-accept" for acknowledging
   * the transport type. wjt confirms that this is apparently necessary for
   * incoming calls to work.
   */
  if (dialect == JINGLE_DIALECT_GTALK4)
    priv->gtalk4_event_id = g_idle_add (send_gtalk4_transport_accept, c);

  return;
}

static guint
new_share_channel (GabbleJingleContent *c, const gchar *name)
{
  GabbleJingleContentPrivate *priv = c->priv;
  GabbleJingleTransportGoogle *gtrans = NULL;

  if (priv->transport &&
      GABBLE_IS_JINGLE_TRANSPORT_GOOGLE (priv->transport))
    {
      guint id = priv->last_share_channel_component_id + 1;

      gtrans = GABBLE_JINGLE_TRANSPORT_GOOGLE (priv->transport);

      if (!jingle_transport_google_set_component_name (gtrans, name, id))
        return 0;

      priv->last_share_channel_component_id++;

      DEBUG ("New Share channel '%s' with id : %d", name, id);

      g_signal_emit (c, signals[NEW_SHARE_CHANNEL], 0, name, id);

      return priv->last_share_channel_component_id;
    }
  return 0;
}

guint
gabble_jingle_content_create_share_channel (GabbleJingleContent *self,
    const gchar *name)
{
  GabbleJingleContentPrivate *priv = self->priv;
  LmMessageNode *sess_node, *channel_node;
  LmMessage *msg = NULL;

  /* Send the info action before creating the channel, in case candidates need
     to be sent on the signal emit. It doesn't matter if the channel already
     exists anyways... */
  msg = gabble_jingle_session_new_message (self->session,
      JINGLE_ACTION_INFO, &sess_node);

  DEBUG ("Sending 'info' message to peer : channel %s", name);
  channel_node = lm_message_node_add_child (sess_node, "channel", NULL);
  lm_message_node_set_attribute (channel_node, "xmlns", priv->content_ns);
  lm_message_node_set_attribute (channel_node, "name", name);

  gabble_jingle_session_send (self->session, msg, NULL, NULL);

  return new_share_channel (self, name);
}

void
gabble_jingle_content_send_complete (GabbleJingleContent *self)
{
  GabbleJingleContentPrivate *priv = self->priv;
  LmMessageNode *sess_node, *complete_node;
  LmMessage *msg = NULL;

  msg = gabble_jingle_session_new_message (self->session,
      JINGLE_ACTION_INFO, &sess_node);

  DEBUG ("Sending 'info' message to peer : complete");
  complete_node = lm_message_node_add_child (sess_node, "complete", NULL);
  lm_message_node_set_attribute (complete_node, "xmlns", priv->content_ns);

  gabble_jingle_session_send (self->session, msg, NULL, NULL);

}

void
gabble_jingle_content_parse_info (GabbleJingleContent *c,
    LmMessageNode *content_node, GError **error)
{
  LmMessageNode *channel_node;
  LmMessageNode *complete_node;

  channel_node = lm_message_node_get_child_any_ns (content_node, "channel");
  complete_node = lm_message_node_get_child_any_ns (content_node, "complete");

  DEBUG ("parsing info message : %p - %p", channel_node, complete_node);
  if (channel_node)
    {
      const gchar *name;
      name = lm_message_node_get_attribute (channel_node, "name");
      if (name != NULL)
        new_share_channel (c, name);
    }
  else if (complete_node)
    {
      g_signal_emit (c, signals[COMPLETED], 0);
    }

}

void
gabble_jingle_content_parse_accept (GabbleJingleContent *c,
    LmMessageNode *content_node, gboolean google_mode, GError **error)
{
  GabbleJingleContentPrivate *priv = c->priv;
  const gchar *senders;
  LmMessageNode *trans_node, *desc_node;
  JingleDialect dialect = gabble_jingle_session_get_dialect (c->session);
  JingleContentSenders newsenders;

  desc_node = lm_message_node_get_child_any_ns (content_node, "description");
  trans_node = lm_message_node_get_child_any_ns (content_node, "transport");
  senders = lm_message_node_get_attribute (content_node, "senders");

  if (GABBLE_IS_JINGLE_MEDIA_RTP (c) &&
      JINGLE_IS_GOOGLE_DIALECT (dialect) && trans_node == NULL)
    {
      DEBUG ("no transport node, assuming GTalk3 dialect");
      /* gtalk lj0.3 assumes google-p2p transport */
      g_object_set (c->session, "dialect", JINGLE_DIALECT_GTALK3, NULL);
    }

  if (senders == NULL)
    newsenders = get_default_senders (c);
  else
    newsenders = parse_senders (senders);

  if (newsenders == JINGLE_CONTENT_SENDERS_NONE)
    {
      SET_BAD_REQ ("invalid content senders");
      return;
    }

  if (newsenders != priv->senders)
    {
      DEBUG ("changing senders from %s to %s", produce_senders (priv->senders),
          produce_senders (newsenders));
      priv->senders = newsenders;
      g_object_notify ((GObject *) c, "senders");
    }

  parse_description (c, desc_node, error);
  if (*error != NULL)
      return;

  priv->state = JINGLE_CONTENT_STATE_ACKNOWLEDGED;
  g_object_notify ((GObject *) c, "state");

  if (trans_node != NULL)
    {
      gabble_jingle_transport_iface_parse_candidates (priv->transport,
        trans_node, NULL);
    }
}

void
gabble_jingle_content_parse_description_info (GabbleJingleContent *c,
    LmMessageNode *content_node, GError **error)
{
  GabbleJingleContentPrivate *priv = c->priv;
  LmMessageNode *desc_node;
  desc_node = lm_message_node_get_child_any_ns (content_node, "description");
  if (desc_node == NULL)
    {
      SET_BAD_REQ ("invalid description-info action");
      return;
    }

  if (priv->created_by_us && priv->state < JINGLE_CONTENT_STATE_ACKNOWLEDGED)
    {
      /* The stream was created by us and the other side didn't acknowledge it
       * yet, thus we don't have their codec information, thus the
       * description-info isn't meaningful and can be ignored */
      DEBUG ("Ignoring description-info as we didn't receive the codecs yet");
      return;
    }

  parse_description (c, desc_node, error);
}


void
gabble_jingle_content_produce_node (GabbleJingleContent *c,
    LmMessageNode *parent,
    gboolean include_description,
    gboolean include_transport,
    LmMessageNode **trans_node_out)
{
  GabbleJingleContentPrivate *priv = c->priv;
  LmMessageNode *content_node, *trans_node;
  JingleDialect dialect = gabble_jingle_session_get_dialect (c->session);
  void (*produce_desc)(GabbleJingleContent *, LmMessageNode *) =
    GABBLE_JINGLE_CONTENT_GET_CLASS (c)->produce_description;

  if ((dialect == JINGLE_DIALECT_GTALK3) ||
      (dialect == JINGLE_DIALECT_GTALK4))
    {
      content_node = parent;
    }
  else
    {
      content_node = lm_message_node_add_child (parent, "content", NULL);
      lm_message_node_set_attributes (content_node,
          "name", priv->name,
          "senders", produce_senders (priv->senders),
          NULL);

      if (gabble_jingle_content_creator_is_initiator (c))
        lm_message_node_set_attribute (content_node, "creator", "initiator");
      else
        lm_message_node_set_attribute (content_node, "creator", "responder");
    }

  if (include_description)
    produce_desc (c, content_node);

  if (include_transport)
    {
      if (dialect == JINGLE_DIALECT_GTALK3)
        {
          /* GTalk 03 doesn't use a transport, but assumes gtalk-p2p */
          trans_node = parent;
        }
      else
        {
          trans_node = lm_message_node_add_child (content_node, "transport", NULL);
          lm_message_node_set_attribute (trans_node, "xmlns", priv->transport_ns);
        }

      if (trans_node_out != NULL)
        *trans_node_out = trans_node;
    }
}

void
gabble_jingle_content_update_senders (GabbleJingleContent *c,
    LmMessageNode *content_node, GError **error)
{
  GabbleJingleContentPrivate *priv = c->priv;
  JingleContentSenders senders;

  senders = parse_senders (lm_message_node_get_attribute (content_node, "senders"));

  if (senders == JINGLE_CONTENT_SENDERS_NONE)
    {
      SET_BAD_REQ ("invalid content senders in stream");
      return;
    }

  priv->senders = senders;
  g_object_notify ((GObject *) c, "senders");
}

void
gabble_jingle_content_parse_transport_info (GabbleJingleContent *self,
  LmMessageNode *trans_node, GError **error)
{
  GabbleJingleContentPrivate *priv = self->priv;

  gabble_jingle_transport_iface_parse_candidates (priv->transport, trans_node, error);
}

/* Takes in a list of slice-allocated JingleCandidate structs */
void
gabble_jingle_content_add_candidates (GabbleJingleContent *self, GList *li)
{
  GabbleJingleContentPrivate *priv = self->priv;

  DEBUG ("called");

  if (li == NULL)
    return;

  gabble_jingle_transport_iface_new_local_candidates (priv->transport, li);

  if (!priv->have_local_candidates)
    {
      priv->have_local_candidates = TRUE;
      /* Maybe we were waiting for at least one candidate? */
      _maybe_ready (self);
    }

  /* If the content exists on the wire, let the transport send this candidate
   * if it wants to.
   */
  if (priv->state > JINGLE_CONTENT_STATE_EMPTY)
    gabble_jingle_transport_iface_send_candidates (priv->transport, FALSE);
}

/* Returns whether the content is ready to be signalled (initiated, for local
 * streams, or acknowledged, for remote streams. */
gboolean
gabble_jingle_content_is_ready (GabbleJingleContent *self)
{
  GabbleJingleContentPrivate *priv = self->priv;

  if (priv->created_by_us)
    {
      /* If it's created by us, media ready, not signalled, and we have
       * at least one local candidate, it's ready to be added. */
      if (priv->media_ready && priv->state == JINGLE_CONTENT_STATE_EMPTY &&
          (!GABBLE_IS_JINGLE_MEDIA_RTP (self) || priv->have_local_candidates))
        return TRUE;
    }
  else
    {
      /* If it's created by peer, media and transports ready,
       * and not acknowledged yet, it's ready for acceptance. */
      if (priv->media_ready && priv->state == JINGLE_CONTENT_STATE_NEW &&
          (!GABBLE_IS_JINGLE_MEDIA_RTP (self) ||
              gabble_jingle_transport_iface_can_accept (priv->transport)))
        return TRUE;
    }

  return FALSE;
}

static void
send_content_add_or_accept (GabbleJingleContent *self)
{
  GabbleJingleContentPrivate *priv = self->priv;
  LmMessage *msg;
  LmMessageNode *sess_node, *transport_node;
  JingleAction action;
  JingleContentState new_state = JINGLE_CONTENT_STATE_EMPTY;

  g_assert (gabble_jingle_content_is_ready (self));

  if (priv->created_by_us)
    {
      /* TODO: set a timer for acknowledgement */
      action = JINGLE_ACTION_CONTENT_ADD;
      new_state = JINGLE_CONTENT_STATE_SENT;
    }
  else
    {
      action = JINGLE_ACTION_CONTENT_ACCEPT;
      new_state = JINGLE_CONTENT_STATE_ACKNOWLEDGED;
    }

  msg = gabble_jingle_session_new_message (self->session,
      action, &sess_node);
  gabble_jingle_content_produce_node (self, sess_node, TRUE, TRUE,
      &transport_node);
  gabble_jingle_transport_iface_inject_candidates (priv->transport,
      transport_node);
  gabble_jingle_session_send (self->session, msg, NULL, NULL);

  priv->state = new_state;
  g_object_notify (G_OBJECT (self), "state");
}

static void
_maybe_ready (GabbleJingleContent *self)
{
  GabbleJingleContentPrivate *priv = self->priv;
  JingleSessionState state;

  if (!gabble_jingle_content_is_ready (self))
      return;

  /* If content disposition is session and session
   * is not yet acknowledged/active, we signall
   * the readiness to the session and let it take
   * care of it. Otherwise, we can deal with it
   * ourselves. */

  g_object_get (self->session, "state", &state, NULL);

  if (!tp_strdiff (priv->disposition, "session") &&
      (state < JS_STATE_PENDING_ACCEPT_SENT))
    {
      /* Notify the session that we're ready for
       * session-initiate/session-accept */
      g_signal_emit (self, signals[READY], 0);
    }
  else
    {
      if (state >= JS_STATE_PENDING_INITIATE_SENT)
        {
          send_content_add_or_accept (self);

          /* if neccessary, transmit the candidates */
          gabble_jingle_transport_iface_send_candidates (priv->transport,
              FALSE);
        }
      else
        {
          /* non session-disposition content ready without session
           * being initiated at all? */
          DEBUG ("session not initiated yet, ignoring non-session ready content");
          return;
        }
    }
}

void
gabble_jingle_content_maybe_send_description (GabbleJingleContent *self)
{
  GabbleJingleContentPrivate *priv = self->priv;

  /* If we didn't send the content yet there is no reason to send a
   * description-info to update it */
  if (priv->state < JINGLE_CONTENT_STATE_SENT)
    return;

  if (gabble_jingle_session_defines_action (self->session,
          JINGLE_ACTION_DESCRIPTION_INFO))
    {
      LmMessageNode *sess_node;
      LmMessage *msg = gabble_jingle_session_new_message (self->session,
          JINGLE_ACTION_DESCRIPTION_INFO, &sess_node);

      gabble_jingle_content_produce_node (self, sess_node, TRUE, FALSE, NULL);
      gabble_jingle_session_send (self->session, msg, NULL, NULL);
    }
  else
    {
      DEBUG ("not sending description-info, speaking an old dialect");
    }
}


/* Used when session-initiate is sent (so all initial contents transmit their
 * candidates), and when we detect gtalk3 after we've transmitted some
 * candidates. */
void
gabble_jingle_content_retransmit_candidates (GabbleJingleContent *self,
    gboolean all)
{
  gabble_jingle_transport_iface_send_candidates (self->priv->transport, all);
}

void
gabble_jingle_content_inject_candidates (GabbleJingleContent *self,
    LmMessageNode *transport_node)
{
  gabble_jingle_transport_iface_inject_candidates (self->priv->transport,
      transport_node);
}


/* Called by a subclass when the media is ready (e.g. we got local codecs) */
void
_gabble_jingle_content_set_media_ready (GabbleJingleContent *self)
{
  GabbleJingleContentPrivate *priv = self->priv;


  priv->media_ready = TRUE;

  _maybe_ready (self);
}

void
gabble_jingle_content_set_transport_state (GabbleJingleContent *self,
    JingleTransportState state)
{
  GabbleJingleContentPrivate *priv = self->priv;

  g_object_set (priv->transport, "state", state, NULL);

  _maybe_ready (self);
}

GList *
gabble_jingle_content_get_remote_candidates (GabbleJingleContent *c)
{
  GabbleJingleContentPrivate *priv = c->priv;

  return gabble_jingle_transport_iface_get_remote_candidates (priv->transport);
}

GList *
gabble_jingle_content_get_local_candidates (GabbleJingleContent *c)
{
  GabbleJingleContentPrivate *priv = c->priv;

  return gabble_jingle_transport_iface_get_local_candidates (priv->transport);
}

gboolean
gabble_jingle_content_change_direction (GabbleJingleContent *c,
    JingleContentSenders senders)
{
  GabbleJingleContentPrivate *priv = c->priv;
  LmMessage *msg;
  LmMessageNode *sess_node;
  JingleDialect dialect = gabble_jingle_session_get_dialect (c->session);

  if (senders == priv->senders)
    return TRUE;

  priv->senders = senders;
  g_object_notify (G_OBJECT (c), "senders");

  if (JINGLE_IS_GOOGLE_DIALECT (dialect))
    {
      DEBUG ("ignoring direction change request for GTalk stream");
      return FALSE;
    }

  if (priv->state >= JINGLE_CONTENT_STATE_SENT)
    {
      msg = gabble_jingle_session_new_message (c->session,
          JINGLE_ACTION_CONTENT_MODIFY, &sess_node);
      gabble_jingle_content_produce_node (c, sess_node, FALSE, FALSE, NULL);
      gabble_jingle_session_send (c->session, msg, NULL, NULL);
    }

  /* FIXME: actually check whether remote end accepts our content-modify */
  return TRUE;
}

static void
_on_remove_reply (GObject *c_as_obj,
    gboolean success,
    LmMessage *reply)
{
  GabbleJingleContent *c = GABBLE_JINGLE_CONTENT (c_as_obj);
  GabbleJingleContentPrivate *priv = c->priv;

  g_assert (priv->state == JINGLE_CONTENT_STATE_REMOVING);

  DEBUG ("%p", c);

  /* Everything holding a reference to a content should drop it after receiving
   * 'removed'.
   */
  g_signal_emit (c, signals[REMOVED], 0);
}

void
gabble_jingle_content_remove (GabbleJingleContent *c, gboolean signal_peer)
{
  GabbleJingleContentPrivate *priv = c->priv;
  LmMessage *msg;
  LmMessageNode *sess_node;

  DEBUG ("called for %p (%s)", c, priv->name);

  /* If we were already signalled and removal is not a side-effect of
   * something else (sesssion termination, or removal by peer),
   * we have to signal removal to the peer. */
  if (signal_peer && (priv->state != JINGLE_CONTENT_STATE_EMPTY))
    {
      if (priv->state == JINGLE_CONTENT_STATE_REMOVING)
        {
          DEBUG ("ignoring request to remove content which is already being removed");
          return;
        }

      priv->state = JINGLE_CONTENT_STATE_REMOVING;
      g_object_notify ((GObject *) c, "state");

      msg = gabble_jingle_session_new_message (c->session,
          JINGLE_ACTION_CONTENT_REMOVE, &sess_node);
      gabble_jingle_content_produce_node (c, sess_node, FALSE, FALSE, NULL);
      gabble_jingle_session_send (c->session, msg, _on_remove_reply,
          (GObject *) c);
    }
  else
    {
      DEBUG ("signalling removed with %u refs", G_OBJECT (c)->ref_count);
      /* Everything holding a reference to a content should drop it after receiving
       * 'removed'.
       */
      g_signal_emit (c, signals[REMOVED], 0);
    }
}

gboolean
gabble_jingle_content_is_created_by_us (GabbleJingleContent *c)
{
  return c->priv->created_by_us;
}

gboolean
gabble_jingle_content_creator_is_initiator (GabbleJingleContent *c)
{
  gboolean session_created_by_us;

  g_object_get (c->session, "local-initiator", &session_created_by_us, NULL);

  return (c->priv->created_by_us == session_created_by_us);
}

const gchar *
gabble_jingle_content_get_name (GabbleJingleContent *self)
{
  return self->priv->name;
}

const gchar *
gabble_jingle_content_get_ns (GabbleJingleContent *self)
{
  return self->priv->content_ns;
}

const gchar *
gabble_jingle_content_get_transport_ns (GabbleJingleContent *self)
{
  return self->priv->transport_ns;
}

const gchar *
gabble_jingle_content_get_disposition (GabbleJingleContent *self)
{
  return self->priv->disposition;
}

JingleTransportType
gabble_jingle_content_get_transport_type (GabbleJingleContent *c)
{
  return gabble_jingle_transport_iface_get_transport_type (c->priv->transport);
}

static gboolean
jingle_content_has_direction (GabbleJingleContent *self,
  gboolean sending)
{
  GabbleJingleContentPrivate *priv = self->priv;
  gboolean initiated_by_us;

  g_object_get (self->session, "local-initiator",
    &initiated_by_us, NULL);

  switch (priv->senders)
    {
      case JINGLE_CONTENT_SENDERS_BOTH:
        return TRUE;
      case JINGLE_CONTENT_SENDERS_NONE:
        return FALSE;
      case JINGLE_CONTENT_SENDERS_INITIATOR:
        return sending ? initiated_by_us : !initiated_by_us;
      case JINGLE_CONTENT_SENDERS_RESPONDER:
        return sending ? !initiated_by_us : initiated_by_us;
    }

  return FALSE;
}

gboolean
gabble_jingle_content_sending (GabbleJingleContent *self)
{
  return jingle_content_has_direction (self, TRUE);
}

gboolean
gabble_jingle_content_receiving (GabbleJingleContent *self)
{
  return jingle_content_has_direction (self, FALSE);
}

void
gabble_jingle_content_set_sending (GabbleJingleContent *self,
  gboolean send)
{
  GabbleJingleContentPrivate *priv = self->priv;
  JingleContentSenders senders;
  gboolean initiated_by_us;

  if (send == jingle_content_has_direction (self, TRUE))
    return;

  g_object_get (self->session, "local-initiator",
    &initiated_by_us, NULL);

  if (send)
    {
      if (priv->senders == JINGLE_CONTENT_SENDERS_NONE)
        senders = (initiated_by_us ? JINGLE_CONTENT_SENDERS_INITIATOR :
          JINGLE_CONTENT_SENDERS_RESPONDER);
      else
        senders = JINGLE_CONTENT_SENDERS_BOTH;
    }
  else
    {
      if (priv->senders == JINGLE_CONTENT_SENDERS_BOTH)
        senders = (initiated_by_us ? JINGLE_CONTENT_SENDERS_RESPONDER :
          JINGLE_CONTENT_SENDERS_INITIATOR);
      else
        senders = JINGLE_CONTENT_SENDERS_NONE;
    }

  if (senders == JINGLE_CONTENT_SENDERS_NONE)
    gabble_jingle_content_remove (self, TRUE);
  else
    gabble_jingle_content_change_direction (self, senders);
}
