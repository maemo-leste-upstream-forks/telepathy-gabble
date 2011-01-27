/*
 * gabble-call-stream.h - Header for TpyBaseMediaCallStream
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

#ifndef __TPY_BASE_MEDIA_CALL_STREAM_H__
#define __TPY_BASE_MEDIA_CALL_STREAM_H__

#include <glib-object.h>

#include <telepathy-yell/base-call-stream.h>
#include <telepathy-yell/call-stream-endpoint.h>

G_BEGIN_DECLS

typedef struct _TpyBaseMediaCallStream TpyBaseMediaCallStream;
typedef struct _TpyBaseMediaCallStreamPrivate TpyBaseMediaCallStreamPrivate;
typedef struct _TpyBaseMediaCallStreamClass TpyBaseMediaCallStreamClass;
typedef void (*TpyBaseMediaStreamFunc) (TpyBaseMediaCallStream *self);
typedef GPtrArray *(*TpyMediaStreamAddCandidatesFunc) (
    TpyBaseMediaCallStream *self,
    const GPtrArray *candidates,
    GError **error);

struct _TpyBaseMediaCallStreamClass {
    TpyBaseCallStreamClass parent_class;

    TpyMediaStreamAddCandidatesFunc add_local_candidates;
    TpyBaseMediaStreamFunc local_candidates_prepared;
};

struct _TpyBaseMediaCallStream {
    TpyBaseCallStream parent;

    TpyBaseMediaCallStreamPrivate *priv;
};

GType tpy_base_media_call_stream_get_type (void);

void tpy_base_media_call_stream_set_relay_info (
    TpyBaseMediaCallStream *self,
    const GPtrArray *relays);
void tpy_base_media_call_stream_set_stun_servers (
    TpyBaseMediaCallStream *self,
    const GPtrArray *stun_servers);
void tpy_base_media_call_stream_take_endpoint (
    TpyBaseMediaCallStream *self,
    TpyCallStreamEndpoint *endpoint);
void tpy_base_media_call_stream_set_transport (
    TpyBaseMediaCallStream *self,
    TpyStreamTransportType transport);

/* TYPE MACROS */
#define TPY_TYPE_BASE_MEDIA_CALL_STREAM \
  (tpy_base_media_call_stream_get_type ())
#define TPY_BASE_MEDIA_CALL_STREAM(obj) \
  (G_TYPE_CHECK_INSTANCE_CAST((obj), TPY_TYPE_BASE_MEDIA_CALL_STREAM, TpyBaseMediaCallStream))
#define TPY_BASE_MEDIA_CALL_STREAM_CLASS(klass) \
  (G_TYPE_CHECK_CLASS_CAST((klass), TPY_TYPE_BASE_MEDIA_CALL_STREAM, \
    TpyBaseMediaCallStreamClass))
#define TPY_IS_BASE_MEDIA_CALL_STREAM(obj) \
  (G_TYPE_CHECK_INSTANCE_TYPE((obj), TPY_TYPE_BASE_MEDIA_CALL_STREAM))
#define TPY_IS_BASE_MEDIA_CALL_STREAM_CLASS(klass) \
  (G_TYPE_CHECK_CLASS_TYPE((klass), TPY_TYPE_BASE_MEDIA_CALL_STREAM))
#define TPY_BASE_MEDIA_CALL_STREAM_GET_CLASS(obj) \
  (G_TYPE_INSTANCE_GET_CLASS ((obj), TPY_TYPE_BASE_MEDIA_CALL_STREAM, \
    TpyBaseMediaCallStreamClass))



G_END_DECLS

#endif /* #ifndef __TPY_BASE_MEDIA_CALL_STREAM_H__*/
