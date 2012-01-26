/*
 * ft-channel.h - Header for GabbleFileTransferChannel
 * Copyright (C) 2009 Collabora Ltd.
 *   @author: Guillaume Desmottes <guillaume.desmottes@collabora.co.uk>
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

#ifndef __GABBLE_FILE_TRANSFER_CHANNEL_H__
#define __GABBLE_FILE_TRANSFER_CHANNEL_H__

#include <glib-object.h>
#include <extensions/extensions.h>

typedef struct _GabbleFileTransferChannel GabbleFileTransferChannel;

#include "gtalk-file-collection.h"

#include "bytestream-factory.h"

G_BEGIN_DECLS

typedef struct _GabbleFileTransferChannelClass GabbleFileTransferChannelClass;
typedef struct _GabbleFileTransferChannelPrivate GabbleFileTransferChannelPrivate;

struct _GabbleFileTransferChannelClass {
    GObjectClass parent_class;
    TpDBusPropertiesMixinClass dbus_props_class;
};

struct _GabbleFileTransferChannel {
    GObject parent;

    GabbleFileTransferChannelPrivate *priv;
};

GType gabble_file_transfer_channel_get_type (void);

/* TYPE MACROS */
#define GABBLE_TYPE_FILE_TRANSFER_CHANNEL \
  (gabble_file_transfer_channel_get_type ())
#define GABBLE_FILE_TRANSFER_CHANNEL(obj) \
  (G_TYPE_CHECK_INSTANCE_CAST((obj), GABBLE_TYPE_FILE_TRANSFER_CHANNEL, GabbleFileTransferChannel))
#define GABBLE_FILE_TRANSFER_CHANNEL_CLASS(klass) \
  (G_TYPE_CHECK_CLASS_CAST((klass), GABBLE_TYPE_FILE_TRANSFER_CHANNEL, \
                           GabbleFileTransferChannelClass))
#define GABBLE_IS_FILE_TRANSFER_CHANNEL(obj) \
  (G_TYPE_CHECK_INSTANCE_TYPE((obj), GABBLE_TYPE_FILE_TRANSFER_CHANNEL))
#define GABBLE_IS_FILE_TRANSFER_CHANNEL_CLASS(klass) \
  (G_TYPE_CHECK_CLASS_TYPE((klass), GABBLE_TYPE_FILE_TRANSFER_CHANNEL))
#define GABBLE_FILE_TRANSFER_CHANNEL_GET_CLASS(obj) \
  (G_TYPE_INSTANCE_GET_CLASS ((obj), GABBLE_TYPE_FILE_TRANSFER_CHANNEL, \
                              GabbleFileTransferChannelClass))

GabbleFileTransferChannel *
gabble_file_transfer_channel_new (GabbleConnection *conn,
    TpHandle handle, TpHandle initiator_handle, TpFileTransferState state,
    const gchar *content_type, const gchar *filename, guint64 size,
    TpFileHashType content_hash_type, const gchar *content_hash,
    const gchar *description, guint64 date, guint64 initial_offset,
    gboolean resume_supported, GabbleBytestreamIface *bytestream,
    GTalkFileCollection *gtalk_fc, const gchar *file_collection,
    const gchar *uri, const gchar *service_name, const GHashTable *metadata);

gboolean gabble_file_transfer_channel_offer_file (
    GabbleFileTransferChannel *self, GError **error);

/* The following methods are a hack, they are 'signal-like' callbacks for the
   GTalkFileCollection. They have to be made this way because the FileCollection
   can't send out signals since it needs its signals to be sent to a specific
   channel only. So instead it calls these callbacks directly on the channel it
   needs to notify. This is a known layering violation and accepted as the lesser
   of any other evil [hack]. */
void gabble_file_transfer_channel_gtalk_file_collection_state_changed (
    GabbleFileTransferChannel *self, GTalkFileCollectionState gtalk_fc_state,
    gboolean local_terminator);

void gabble_file_transfer_channel_gtalk_file_collection_write_blocked (
    GabbleFileTransferChannel *self, gboolean blocked);

void gabble_file_transfer_channel_gtalk_file_collection_data_received (
    GabbleFileTransferChannel *self, const gchar *data, guint len);

void
gabble_file_transfer_channel_do_close (GabbleFileTransferChannel *self);


G_END_DECLS

#endif /* #ifndef __GABBLE_FILE_TRANSFER_CHANNEL_H__*/
