/*
 * call-content-codec-offer.h - Header for TpyCallContentCodecOffer
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

#ifndef __TPY_CALL_CONTENT_CODEC_OFFER_H__
#define __TPY_CALL_CONTENT_CODEC_OFFER_H__

#include <glib-object.h>
#include <gio/gio.h>
#include <telepathy-glib/telepathy-glib.h>

G_BEGIN_DECLS

typedef struct _TpyCallContentCodecOffer TpyCallContentCodecOffer;
typedef struct _TpyCallContentCodecOfferPrivate
  TpyCallContentCodecOfferPrivate;
typedef struct _TpyCallContentCodecOfferClass
  TpyCallContentCodecOfferClass;

struct _TpyCallContentCodecOfferClass {
    GObjectClass parent_class;

    TpDBusPropertiesMixinClass dbus_props_class;
};

struct _TpyCallContentCodecOffer {
    GObject parent;

    TpyCallContentCodecOfferPrivate *priv;
};

GType tpy_call_content_codec_offer_get_type (void);

/* TYPE MACROS */
#define TPY_TYPE_CALL_CONTENT_CODEC_OFFER \
  (tpy_call_content_codec_offer_get_type ())
#define TPY_CALL_CONTENT_CODEC_OFFER(obj) \
  (G_TYPE_CHECK_INSTANCE_CAST((obj), \
  TPY_TYPE_CALL_CONTENT_CODEC_OFFER, TpyCallContentCodecOffer))
#define TPY_CALL_CONTENT_CODEC_OFFER_CLASS(klass) \
  (G_TYPE_CHECK_CLASS_CAST((klass), \
  TPY_TYPE_CALL_CONTENT_CODEC_OFFER, TpyCallContentCodecOfferClass))
#define TPY_IS_CALL_CONTENT_CODEC_OFFER(obj) \
  (G_TYPE_CHECK_INSTANCE_TYPE((obj), TPY_TYPE_CALL_CONTENT_CODEC_OFFER))
#define TPY_IS_CALL_CONTENT_CODEC_OFFER_CLASS(klass) \
  (G_TYPE_CHECK_CLASS_TYPE((klass), TPY_TYPE_CALL_CONTENT_CODEC_OFFER))
#define TPY_CALL_CONTENT_CODEC_OFFER_GET_CLASS(obj) \
  (G_TYPE_INSTANCE_GET_CLASS ((obj), TPY_TYPE_CALL_CONTENT_CODEC_OFFER, \
  TpyCallContentCodecOfferClass))

TpyCallContentCodecOffer *tpy_call_content_codec_offer_new (
  const gchar *object_path,
  TpHandle remote_contact,
  GPtrArray *codecs);

void tpy_call_content_codec_offer_offer (TpyCallContentCodecOffer *offer,
  GCancellable *cancellable,
  GAsyncReadyCallback callback,
  gpointer user_data);

GPtrArray *tpy_call_content_codec_offer_offer_finish (
  TpyCallContentCodecOffer *offer,
  GAsyncResult *result,
  GError **error);


G_END_DECLS

#endif /* #ifndef __TPY_CALL_CONTENT_CODEC_OFFER_H__*/
