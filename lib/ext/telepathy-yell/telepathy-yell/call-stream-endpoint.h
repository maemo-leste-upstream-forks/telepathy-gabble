/*
 * call-stream-endpoint.h - Header for TpyCallStreamEndpoint
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

#ifndef __TPY_CALL_STREAM_ENDPOINT_H__
#define __TPY_CALL_STREAM_ENDPOINT_H__

#include <glib-object.h>

#include <telepathy-glib/dbus-properties-mixin.h>
#include <telepathy-glib/dbus.h>

#include <telepathy-yell/enums.h>

G_BEGIN_DECLS

typedef struct _TpyCallStreamEndpoint TpyCallStreamEndpoint;
typedef struct _TpyCallStreamEndpointPrivate
  TpyCallStreamEndpointPrivate;
typedef struct _TpyCallStreamEndpointClass TpyCallStreamEndpointClass;

struct _TpyCallStreamEndpointClass {
    GObjectClass parent_class;

    TpDBusPropertiesMixinClass dbus_props_class;
};

struct _TpyCallStreamEndpoint {
    GObject parent;

    TpyCallStreamEndpointPrivate *priv;
};

GType tpy_call_stream_endpoint_get_type (void);

/* TYPE MACROS */
#define TPY_TYPE_CALL_STREAM_ENDPOINT \
  (tpy_call_stream_endpoint_get_type ())
#define TPY_CALL_STREAM_ENDPOINT(obj) \
  (G_TYPE_CHECK_INSTANCE_CAST((obj), \
  TPY_TYPE_CALL_STREAM_ENDPOINT, TpyCallStreamEndpoint))
#define TPY_CALL_STREAM_ENDPOINT_CLASS(klass) \
  (G_TYPE_CHECK_CLASS_CAST((klass), \
  TPY_TYPE_CALL_STREAM_ENDPOINT, TpyCallStreamEndpointClass))
#define TPY_IS_CALL_STREAM_ENDPOINT(obj) \
  (G_TYPE_CHECK_INSTANCE_TYPE((obj), TPY_TYPE_CALL_STREAM_ENDPOINT))
#define TPY_IS_CALL_STREAM_ENDPOINT_CLASS(klass) \
  (G_TYPE_CHECK_CLASS_TYPE((klass), TPY_TYPE_CALL_STREAM_ENDPOINT))
#define TPY_CALL_STREAM_ENDPOINT_GET_CLASS(obj) \
  (G_TYPE_INSTANCE_GET_CLASS ((obj), \
    TPY_TYPE_CALL_STREAM_ENDPOINT, TpyCallStreamEndpointClass))

TpyCallStreamEndpoint *tpy_call_stream_endpoint_new (
    TpDBusDaemon *dbus_daemon,
    const gchar *object_path,
    TpyStreamTransportType type);

void tpy_call_stream_endpoint_add_new_candidates (
    TpyCallStreamEndpoint *endpoint,
    GPtrArray *candidates);
void tpy_call_stream_endpoint_add_new_candidate (
    TpyCallStreamEndpoint *endpoint,
    guint component,
    gchar *address,
    guint port,
    GHashTable *info_hash);

const gchar *tpy_call_stream_endpoint_get_object_path (
    TpyCallStreamEndpoint *endpoint);

G_END_DECLS

#endif /* #ifndef __TPY_CALL_STREAM_ENDPOINT_H__*/
