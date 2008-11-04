/*
 * tube-iface.h - Header for GabbleTube interface
 * Copyright (C) 2007-2008 Collabora Ltd.
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

#ifndef __GABBLE_TUBE_IFACE_H__
#define __GABBLE_TUBE_IFACE_H__

#include <glib-object.h>
#include <telepathy-glib/base-connection.h>

#include "bytestream-iface.h"

G_BEGIN_DECLS

typedef struct _GabbleTubeIface GabbleTubeIface;
typedef struct _GabbleTubeIfaceClass GabbleTubeIfaceClass;

struct _GabbleTubeIfaceClass {
  GTypeInterface parent;

  gboolean (*accept) (GabbleTubeIface *tube, GError **error);
  void (*close) (GabbleTubeIface *tube, gboolean closed_remotely);
  void (*add_bytestream) (GabbleTubeIface *tube,
      GabbleBytestreamIface *bytestream);
};

GType gabble_tube_iface_get_type (void);

/* TYPE MACROS */
#define GABBLE_TYPE_TUBE_IFACE \
  (gabble_tube_iface_get_type ())
#define GABBLE_TUBE_IFACE(obj) \
  (G_TYPE_CHECK_INSTANCE_CAST((obj), GABBLE_TYPE_TUBE_IFACE, GabbleTubeIface))
#define GABBLE_IS_TUBE_IFACE(obj) \
  (G_TYPE_CHECK_INSTANCE_TYPE((obj), GABBLE_TYPE_TUBE_IFACE))
#define GABBLE_TUBE_IFACE_GET_CLASS(obj) \
  (G_TYPE_INSTANCE_GET_INTERFACE ((obj), GABBLE_TYPE_TUBE_IFACE,\
                              GabbleTubeIfaceClass))

gboolean gabble_tube_iface_accept (GabbleTubeIface *tube, GError **error);

void gabble_tube_iface_close (GabbleTubeIface *tube, gboolean closed_remotely);

void gabble_tube_iface_add_bytestream (GabbleTubeIface *tube,
    GabbleBytestreamIface *bytestream);

void gabble_tube_iface_publish_in_node (GabbleTubeIface *tube,
    TpBaseConnection *conn, LmMessageNode *node);

G_END_DECLS

#endif /* #ifndef __GABBLE_TUBE_IFACE_H__ */
