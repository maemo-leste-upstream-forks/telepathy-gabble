/*
 * tubes-channel.h - Header for GabbleTubesChannel
 * Copyright (C) 2007 Collabora Ltd.
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

#ifndef __GABBLE_TUBES_CHANNEL_H__
#define __GABBLE_TUBES_CHANNEL_H__

#include <glib-object.h>
#include <loudmouth/loudmouth.h>

#include <telepathy-glib/base-connection.h>
#include <telepathy-glib/exportable-channel.h>

#include "bytestream-iface.h"
#include "muc-channel.h"
#include "tube-iface.h"

G_BEGIN_DECLS

typedef struct _GabbleTubesChannel GabbleTubesChannel;
typedef struct _GabbleTubesChannelPrivate GabbleTubesChannelPrivate;
typedef struct _GabbleTubesChannelClass GabbleTubesChannelClass;

struct _GabbleTubesChannelClass {
    GObjectClass parent_class;

    TpDBusPropertiesMixinClass dbus_props_class;
};

struct _GabbleTubesChannel {
    GObject parent;

    GabbleMucChannel *muc;

    GabbleTubesChannelPrivate *priv;
};

GType gabble_tubes_channel_get_type (void);

/* TYPE MACROS */
#define GABBLE_TYPE_TUBES_CHANNEL \
  (gabble_tubes_channel_get_type ())
#define GABBLE_TUBES_CHANNEL(obj) \
  (G_TYPE_CHECK_INSTANCE_CAST((obj), GABBLE_TYPE_TUBES_CHANNEL,\
                              GabbleTubesChannel))
#define GABBLE_TUBES_CHANNEL_CLASS(klass) \
  (G_TYPE_CHECK_CLASS_CAST((klass), GABBLE_TYPE_TUBES_CHANNEL,\
                           GabbleTubesChannelClass))
#define GABBLE_IS_TUBES_CHANNEL(obj) \
  (G_TYPE_CHECK_INSTANCE_TYPE((obj), GABBLE_TYPE_TUBES_CHANNEL))
#define GABBLE_IS_TUBES_CHANNEL_CLASS(klass) \
  (G_TYPE_CHECK_CLASS_TYPE((klass), GABBLE_TYPE_TUBES_CHANNEL))
#define GABBLE_TUBES_CHANNEL_GET_CLASS(obj) \
  (G_TYPE_INSTANCE_GET_CLASS ((obj), GABBLE_TYPE_TUBES_CHANNEL,\
                              GabbleTubesChannelClass))

void gabble_tubes_channel_foreach (GabbleTubesChannel *self,
    TpExportableChannelFunc foreach, gpointer user_data);

GabbleTubeIface *gabble_tubes_channel_tube_request (GabbleTubesChannel *self,
    gpointer request_token, GHashTable *request_properties,
    gboolean require_new);

void gabble_tubes_channel_presence_updated (GabbleTubesChannel *chan,
    TpHandle contact, LmMessage *presence);

void gabble_tubes_channel_tube_si_offered (GabbleTubesChannel *chan,
    GabbleBytestreamIface *bytestream, LmMessage *msg);

void gabble_tubes_channel_bytestream_offered (GabbleTubesChannel *chan,
    GabbleBytestreamIface *bytestream, LmMessage *msg);

void gabble_tubes_channel_tube_msg (GabbleTubesChannel *chan, LmMessage *msg);

void gabble_tubes_channel_close (GabbleTubesChannel *self);

G_END_DECLS

#endif /* #ifndef __GABBLE_TUBES_CHANNEL_H__*/
