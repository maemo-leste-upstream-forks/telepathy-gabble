/*
 * call-content-codec-offer.c - Source for TpyCallContentCodecOffer
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


#include <stdio.h>
#include <stdlib.h>

#include <glib.h>
#include <telepathy-glib/dbus.h>
#include <telepathy-glib/dbus-properties-mixin.h>
#include <telepathy-glib/svc-properties-interface.h>

#include "call-content-codec-offer.h"
#include "extensions.h"

#define DEBUG_FLAG TPY_DEBUG_CALL
#include "debug.h"

static void call_content_codec_offer_iface_init (gpointer, gpointer);

G_DEFINE_TYPE_WITH_CODE(TpyCallContentCodecOffer,
  tpy_call_content_codec_offer,
  G_TYPE_OBJECT,
  G_IMPLEMENT_INTERFACE (TPY_TYPE_SVC_CALL_CONTENT_CODEC_OFFER,
        call_content_codec_offer_iface_init);
   G_IMPLEMENT_INTERFACE (TP_TYPE_SVC_DBUS_PROPERTIES,
    tp_dbus_properties_mixin_iface_init);
  );

/* properties */
enum
{
  PROP_OBJECT_PATH = 1,
  PROP_INTERFACES,
  PROP_REMOTE_CONTACT_CODECS,
  PROP_REMOTE_CONTACT
};

/* private structure */
struct _TpyCallContentCodecOfferPrivate
{
  gboolean dispose_has_run;

  TpDBusDaemon *bus;
  gchar *object_path;

  TpHandle contact;
  GPtrArray *codecs;

  GSimpleAsyncResult *result;
  GCancellable *cancellable;
  guint handler_id;
};

#define TPY_CALL_CONTENT_CODEC_OFFER_GET_PRIVATE(o) \
    (G_TYPE_INSTANCE_GET_PRIVATE ((o), \
    TPY_TYPE_CALL_CONTENT_CODEC_OFFER, TpyCallContentCodecOfferPrivate))

static void
tpy_call_content_codec_offer_init (TpyCallContentCodecOffer *self)
{
  TpyCallContentCodecOfferPrivate *priv = G_TYPE_INSTANCE_GET_PRIVATE (self,
      TPY_TYPE_CALL_CONTENT_CODEC_OFFER,
      TpyCallContentCodecOfferPrivate);

  self->priv = priv;
  priv->bus = tp_dbus_daemon_dup (NULL);
}

static void tpy_call_content_codec_offer_dispose (GObject *object);
static void tpy_call_content_codec_offer_finalize (GObject *object);

static const gchar *interfaces[] = {
    NULL
};

static void
tpy_call_content_codec_offer_get_property (GObject    *object,
    guint       property_id,
    GValue     *value,
    GParamSpec *pspec)
{
  TpyCallContentCodecOffer *offer =
    TPY_CALL_CONTENT_CODEC_OFFER (object);
  TpyCallContentCodecOfferPrivate *priv = offer->priv;

  switch (property_id)
    {
      case PROP_OBJECT_PATH:
        g_value_set_string (value, priv->object_path);
        break;
      case PROP_INTERFACES:
        g_value_set_boxed (value, interfaces);
        break;
      case PROP_REMOTE_CONTACT_CODECS:
        g_value_set_boxed (value, priv->codecs);
        break;
      case PROP_REMOTE_CONTACT:
        g_value_set_uint (value, priv->contact);
        break;
      default:
        G_OBJECT_WARN_INVALID_PROPERTY_ID (object, property_id, pspec);
        break;
    }
}

static void
tpy_call_content_codec_offer_set_property (GObject *object,
    guint property_id,
    const GValue *value,
    GParamSpec *pspec)
{
  TpyCallContentCodecOffer *content =
    TPY_CALL_CONTENT_CODEC_OFFER (object);
  TpyCallContentCodecOfferPrivate *priv = content->priv;

  switch (property_id)
    {
      case PROP_OBJECT_PATH:
        priv->object_path = g_value_dup_string (value);
        g_assert (priv->object_path != NULL);
        break;
      case PROP_REMOTE_CONTACT_CODECS:
        priv->codecs = g_value_dup_boxed (value);
        break;
      case PROP_REMOTE_CONTACT:
        priv->contact = g_value_get_uint (value);
        break;
      default:
        G_OBJECT_WARN_INVALID_PROPERTY_ID (object, property_id, pspec);
        break;
    }
}

static void
tpy_call_content_codec_offer_class_init (
  TpyCallContentCodecOfferClass *tpy_call_content_codec_offer_class)
{
  GObjectClass *object_class =
    G_OBJECT_CLASS (tpy_call_content_codec_offer_class);
  GParamSpec *spec;

  static TpDBusPropertiesMixinPropImpl codec_offer_props[] = {
    { "Interfaces", "interfaces", NULL },
    { "RemoteContactCodecs", "remote-contact-codecs", NULL },
    { "RemoteContact", "remote-contact", NULL },
    { NULL }
  };

  static TpDBusPropertiesMixinIfaceImpl prop_interfaces[] = {
      { TPY_IFACE_CALL_CONTENT_CODEC_OFFER,
        tp_dbus_properties_mixin_getter_gobject_properties,
        NULL,
        codec_offer_props,
      },
      { NULL }
  };


  g_type_class_add_private (tpy_call_content_codec_offer_class,
    sizeof (TpyCallContentCodecOfferPrivate));

  object_class->get_property = tpy_call_content_codec_offer_get_property;
  object_class->set_property = tpy_call_content_codec_offer_set_property;

  object_class->dispose = tpy_call_content_codec_offer_dispose;
  object_class->finalize = tpy_call_content_codec_offer_finalize;

  spec = g_param_spec_string ("object-path", "D-Bus object path",
      "The D-Bus object path used for this "
      "object on the bus.",
      NULL,
      G_PARAM_CONSTRUCT_ONLY | G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS);
  g_object_class_install_property (object_class, PROP_OBJECT_PATH, spec);

  spec = g_param_spec_boxed ("interfaces",
      "Interfaces",
      "Extra interfaces provided by this codec offer",
      G_TYPE_STRV,
      G_PARAM_READABLE | G_PARAM_STATIC_STRINGS);
  g_object_class_install_property (object_class, PROP_INTERFACES,
      spec);

  spec = g_param_spec_boxed ("remote-contact-codecs",
      "RemoteContactCodecs",
      "A list of codecs the remote contact supports",
      TPY_ARRAY_TYPE_CODEC_LIST,
      G_PARAM_READWRITE | G_PARAM_CONSTRUCT_ONLY | G_PARAM_STATIC_STRINGS);
  g_object_class_install_property (object_class, PROP_REMOTE_CONTACT_CODECS,
      spec);

  spec = g_param_spec_uint ("remote-contact",
      "RemoteContact",
      "The contact handle that this codec offer applies to",
      0, G_MAXUINT, 0,
      G_PARAM_READWRITE | G_PARAM_CONSTRUCT_ONLY | G_PARAM_STATIC_STRINGS);
  g_object_class_install_property (object_class, PROP_REMOTE_CONTACT,
      spec);

  tpy_call_content_codec_offer_class->dbus_props_class.interfaces
    = prop_interfaces;
  tp_dbus_properties_mixin_class_init (object_class,
      G_STRUCT_OFFSET (TpyCallContentCodecOfferClass, dbus_props_class));
}

void
tpy_call_content_codec_offer_dispose (GObject *object)
{
  TpyCallContentCodecOffer *self = TPY_CALL_CONTENT_CODEC_OFFER (object);
  TpyCallContentCodecOfferPrivate *priv = self->priv;

  g_assert (priv->result == NULL);

  if (priv->dispose_has_run)
    return;

  priv->dispose_has_run = TRUE;

  if (priv->codecs != NULL)
    {
      /* dbus-glib :( */
      g_boxed_free (TPY_ARRAY_TYPE_CODEC_LIST, priv->codecs);
    }
  priv->codecs = NULL;

  g_object_unref (priv->bus);
  priv->bus = NULL;

  /* release any references held by the object here */
  if (G_OBJECT_CLASS (tpy_call_content_codec_offer_parent_class)->dispose)
    G_OBJECT_CLASS (tpy_call_content_codec_offer_parent_class)->dispose (
      object);
}

void
tpy_call_content_codec_offer_finalize (GObject *object)
{
  TpyCallContentCodecOffer *self = TPY_CALL_CONTENT_CODEC_OFFER (object);
  TpyCallContentCodecOfferPrivate *priv = self->priv;

  g_free (priv->object_path);
  /* free any data held directly by the object here */

  G_OBJECT_CLASS (tpy_call_content_codec_offer_parent_class)->finalize (
    object);
}

static void
tpy_call_content_codec_offer_accept (TpySvcCallContentCodecOffer *iface,
    const GPtrArray *codecs,
    DBusGMethodInvocation *context)
{
  TpyCallContentCodecOffer *self = TPY_CALL_CONTENT_CODEC_OFFER (iface);
  TpyCallContentCodecOfferPrivate *priv = self->priv;

  g_return_if_fail (priv->bus != NULL);

  DEBUG ("%s was accepted", priv->object_path);

  if (priv->cancellable != NULL)
    {
      g_cancellable_disconnect (priv->cancellable, priv->handler_id);
      g_object_unref (priv->cancellable);
      priv->cancellable = NULL;
      priv->handler_id = 0;
    }

  g_simple_async_result_set_op_res_gpointer (priv->result,
    (gpointer) codecs, NULL);
  g_simple_async_result_complete (priv->result);
  g_object_unref (priv->result);
  priv->result = NULL;

  tpy_svc_call_content_codec_offer_return_from_accept (context);

  tp_dbus_daemon_unregister_object (priv->bus, G_OBJECT (self));
}

static void
tpy_call_content_codec_offer_reject (TpySvcCallContentCodecOffer *iface,
    DBusGMethodInvocation *context)
{
  TpyCallContentCodecOffer *self = TPY_CALL_CONTENT_CODEC_OFFER (iface);
  TpyCallContentCodecOfferPrivate *priv = self->priv;

  g_return_if_fail (priv->bus != NULL);

  DEBUG ("%s was rejected", priv->object_path);

  if (priv->cancellable != NULL)
    {
      g_cancellable_disconnect (priv->cancellable, priv->handler_id);
      g_object_unref (priv->cancellable);
      priv->cancellable = NULL;
      priv->handler_id = 0;
    }

  g_simple_async_result_set_error (priv->result,
      G_IO_ERROR, G_IO_ERROR_FAILED, "Codec offer was rejected");
  g_simple_async_result_complete (priv->result);
  g_object_unref (priv->result);
  priv->result = NULL;

  tpy_svc_call_content_codec_offer_return_from_reject (context);

  tp_dbus_daemon_unregister_object (priv->bus, G_OBJECT (self));
}

static void
call_content_codec_offer_iface_init (gpointer iface, gpointer data)
{
  TpySvcCallContentCodecOfferClass *klass =
    (TpySvcCallContentCodecOfferClass *) iface;

#define IMPLEMENT(x) tpy_svc_call_content_codec_offer_implement_##x (\
    klass, tpy_call_content_codec_offer_##x)
  IMPLEMENT(accept);
  IMPLEMENT(reject);
#undef IMPLEMENT
}

TpyCallContentCodecOffer *
tpy_call_content_codec_offer_new (const gchar *object_path,
  TpHandle remote_contact,
  GPtrArray *codecs)
{
  return g_object_new (TPY_TYPE_CALL_CONTENT_CODEC_OFFER,
    "object-path", object_path,
    "remote-contact", remote_contact,
    "remote-contact-codecs", codecs,
    NULL);
}

static void
cancelled_cb (GCancellable *cancellable, gpointer user_data)
{
  TpyCallContentCodecOffer *offer = user_data;
  TpyCallContentCodecOfferPrivate *priv = offer->priv;

  g_return_if_fail (priv->bus != NULL);

  tp_dbus_daemon_unregister_object (priv->bus, G_OBJECT (offer));

  g_simple_async_result_set_error (priv->result,
      G_IO_ERROR, G_IO_ERROR_CANCELLED, "Offer cancelled");
  g_simple_async_result_complete_in_idle (priv->result);

  g_object_unref (priv->cancellable);
  g_object_unref (priv->result);
  priv->result = NULL;
  priv->cancellable = NULL;
  priv->handler_id = 0;
}

void
tpy_call_content_codec_offer_offer (TpyCallContentCodecOffer *offer,
  GCancellable *cancellable,
  GAsyncReadyCallback callback,
  gpointer user_data)
{
  TpyCallContentCodecOfferPrivate *priv = offer->priv;

  g_return_if_fail (priv->bus != NULL);

  /* FIXME implement cancellable support */
  if (G_UNLIKELY (priv->result != NULL))
    goto pending;

  priv->result = g_simple_async_result_new (G_OBJECT (offer),
    callback, user_data, tpy_call_content_codec_offer_offer_finish);

  /* register object on the bus */
  DEBUG ("Registering %s", priv->object_path);
  tp_dbus_daemon_register_object (priv->bus, priv->object_path,
    G_OBJECT (offer));

  if (cancellable != NULL)
    {
      priv->cancellable = cancellable;
      priv->handler_id = g_cancellable_connect (
          cancellable, G_CALLBACK (cancelled_cb), offer, NULL);
    }

  return;

pending:
  g_simple_async_report_error_in_idle (G_OBJECT (offer), callback, user_data,
    G_IO_ERROR, G_IO_ERROR_PENDING, "Another offer operation is pending");
}

GPtrArray *
tpy_call_content_codec_offer_offer_finish (
  TpyCallContentCodecOffer *offer,
  GAsyncResult *result,
  GError **error)
{
  if (g_simple_async_result_propagate_error (G_SIMPLE_ASYNC_RESULT (result),
      error))
    return FALSE;

  g_return_val_if_fail (g_simple_async_result_is_valid (result,
    G_OBJECT (offer), tpy_call_content_codec_offer_offer_finish),
    NULL);

  return g_simple_async_result_get_op_res_gpointer (
    G_SIMPLE_ASYNC_RESULT (result));
}
