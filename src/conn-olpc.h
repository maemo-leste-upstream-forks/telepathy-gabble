/*
 * conn-olpc.h - Header for Gabble OLPC BuddyInfo and ActivityProperties interfaces
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

#ifndef __CONN_OLPC_H__
#define __CONN_OLPC_H__

#include <extensions/extensions.h>

#include "connection.h"

void
olpc_buddy_info_iface_init (gpointer g_iface, gpointer iface_data);

void gabble_connection_connected_olpc (GabbleConnection *conn);

void
olpc_activity_properties_iface_init (gpointer g_iface, gpointer iface_data);

void conn_olpc_activity_properties_init (GabbleConnection *conn);

void conn_olpc_activity_properties_dispose (GabbleConnection *conn);

gboolean conn_olpc_process_activity_properties_message (GabbleConnection *conn,
    LmMessage *msg, const gchar *from);

gboolean conn_olpc_process_activity_uninvite_message (GabbleConnection *conn,
    LmMessage *msg, const gchar *from);

LmHandlerResult conn_olpc_msg_cb (LmMessageHandler *handler,
    LmConnection *connection, LmMessage *message, gpointer user_data);

#endif /* __CONN_OLPC_H__ */

