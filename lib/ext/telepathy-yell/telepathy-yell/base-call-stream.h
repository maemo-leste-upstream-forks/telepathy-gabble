/*
 * base-call-stream.h - Header for TpyBaseCallStream
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

#ifndef TPY_BASE_CALL_STREAM_H
#define TPY_BASE_CALL_STREAM_H

#include <glib-object.h>

#include <telepathy-glib/telepathy-glib.h>

#include <telepathy-yell/enums.h>

G_BEGIN_DECLS

typedef struct _TpyBaseCallStream TpyBaseCallStream;
typedef struct _TpyBaseCallStreamPrivate TpyBaseCallStreamPrivate;
typedef struct _TpyBaseCallStreamClass TpyBaseCallStreamClass;

typedef gboolean (*TpyStreamSetSendingFunc) (TpyBaseCallStream *,
    gboolean sending,
    GError **error);
typedef void (*TpyStreamRequestReceivingFunc) (TpyBaseCallStream *self,
    TpHandle handle,
    gboolean receive,
    GError **error);

struct _TpyBaseCallStreamClass {
    GObjectClass parent_class;

    TpDBusPropertiesMixinClass dbus_props_class;

    TpyStreamRequestReceivingFunc request_receiving;
    TpyStreamSetSendingFunc set_sending;

    const gchar * const *extra_interfaces;
};

struct _TpyBaseCallStream {
    GObject parent;

    TpyBaseCallStreamPrivate *priv;
};

GType tpy_base_call_stream_get_type (void);

/* TYPE MACROS */
#define TPY_TYPE_BASE_CALL_STREAM \
  (tpy_base_call_stream_get_type ())
#define TPY_BASE_CALL_STREAM(obj) \
  (G_TYPE_CHECK_INSTANCE_CAST((obj), TPY_TYPE_BASE_CALL_STREAM, TpyBaseCallStream))
#define TPY_BASE_CALL_STREAM_CLASS(klass) \
  (G_TYPE_CHECK_CLASS_CAST((klass), TPY_TYPE_BASE_CALL_STREAM, \
    TpyBaseCallStreamClass))
#define TPY_IS_BASE_CALL_STREAM(obj) \
  (G_TYPE_CHECK_INSTANCE_TYPE((obj), TPY_TYPE_BASE_CALL_STREAM))
#define TPY_IS_BASE_CALL_STREAM_CLASS(klass) \
  (G_TYPE_CHECK_CLASS_TYPE((klass), TPY_TYPE_BASE_CALL_STREAM))
#define TPY_BASE_CALL_STREAM_GET_CLASS(obj) \
  (G_TYPE_INSTANCE_GET_CLASS ((obj), TPY_TYPE_BASE_CALL_STREAM, \
    TpyBaseCallStreamClass))

TpBaseConnection *tpy_base_call_stream_get_connection (
    TpyBaseCallStream *self);
const gchar *tpy_base_call_stream_get_object_path (
    TpyBaseCallStream *self);

TpySendingState tpy_base_call_stream_get_sender_state (
    TpyBaseCallStream *self,
    TpHandle sender,
    gboolean *existed);

gboolean tpy_base_call_stream_update_local_sending_state (
  TpyBaseCallStream *self,
  TpySendingState state);

TpySendingState
tpy_base_call_stream_get_local_sending_state (
  TpyBaseCallStream *self);

gboolean
tpy_base_call_stream_remote_member_update_state (TpyBaseCallStream *self,
    TpHandle contact,
    TpySendingState state);


gboolean tpy_base_call_stream_update_senders (
    TpyBaseCallStream *self,
    TpHandle contact,
    TpySendingState state,
    ...) G_GNUC_NULL_TERMINATED;

gboolean tpy_base_call_stream_set_sending (TpyBaseCallStream *self,
    gboolean send, GError **error);

G_END_DECLS

#endif
