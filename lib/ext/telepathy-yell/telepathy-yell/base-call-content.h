/*
 * base-call-content.h - Header for TpyBaseBaseCallContent
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

#ifndef TPY_BASE_CALL_CONTENT_H
#define TPY_BASE_CALL_CONTENT_H

#include <glib-object.h>

#include <telepathy-glib/telepathy-glib.h>

#include "base-call-stream.h"

G_BEGIN_DECLS

typedef struct _TpyBaseCallContent TpyBaseCallContent;
typedef struct _TpyBaseCallContentPrivate TpyBaseCallContentPrivate;
typedef struct _TpyBaseCallContentClass TpyBaseCallContentClass;

typedef void (*TpyBaseCallContentFunc) (TpyBaseCallContent *);

struct _TpyBaseCallContentClass {
    GObjectClass parent_class;

    TpDBusPropertiesMixinClass dbus_props_class;

    const gchar * const *extra_interfaces;
    TpyBaseCallContentFunc deinit;
};

struct _TpyBaseCallContent {
    GObject parent;

    TpyBaseCallContentPrivate *priv;
};

GType tpy_base_call_content_get_type (void);

/* TYPE MACROS */
#define TPY_TYPE_BASE_CALL_CONTENT \
  (tpy_base_call_content_get_type ())
#define TPY_BASE_CALL_CONTENT(obj) \
  (G_TYPE_CHECK_INSTANCE_CAST((obj), \
      TPY_TYPE_BASE_CALL_CONTENT, TpyBaseCallContent))
#define TPY_BASE_CALL_CONTENT_CLASS(klass) \
  (G_TYPE_CHECK_CLASS_CAST((klass), \
    TPY_TYPE_BASE_CALL_CONTENT, TpyBaseCallContentClass))
#define TPY_IS_BASE_CALL_CONTENT(obj) \
  (G_TYPE_CHECK_INSTANCE_TYPE((obj), TPY_TYPE_BASE_CALL_CONTENT))
#define TPY_IS_BASE_CALL_CONTENT_CLASS(klass) \
  (G_TYPE_CHECK_CLASS_TYPE((klass), TPY_TYPE_BASE_CALL_CONTENT))
#define TPY_BASE_CALL_CONTENT_GET_CLASS(obj) \
  (G_TYPE_INSTANCE_GET_CLASS ((obj), \
    TPY_TYPE_BASE_CALL_CONTENT, TpyBaseCallContentClass))

TpBaseConnection *tpy_base_call_content_get_connection (
    TpyBaseCallContent *self);
const gchar *tpy_base_call_content_get_object_path (
    TpyBaseCallContent *self);

const gchar *tpy_base_call_content_get_name (TpyBaseCallContent *self);
TpMediaStreamType tpy_base_call_content_get_media_type (
    TpyBaseCallContent *self);
TpyCallContentDisposition tpy_base_call_content_get_disposition (
    TpyBaseCallContent *self);

GList *tpy_base_call_content_get_streams (TpyBaseCallContent *self);
void tpy_base_call_content_add_stream (TpyBaseCallContent *self,
    TpyBaseCallStream *stream);
void tpy_base_call_content_remove_stream (TpyBaseCallContent *self,
    TpyBaseCallStream *stream);

void tpy_base_call_content_accepted (TpyBaseCallContent *self);
void tpy_base_call_content_deinit (TpyBaseCallContent *self);

G_END_DECLS

#endif /* #ifndef __TPY_BASE_CALL_CONTENT_H__*/
