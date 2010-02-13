/*
 * conn-presence.h - Header for Gabble connection presence interface
 * Copyright (C) 2005-2007 Collabora Ltd.
 * Copyright (C) 2005-2007 Nokia Corporation
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

#ifndef __CONN_PRESENCE_H__
#define __CONN_PRESENCE_H__

#include <glib.h>

#include "connection.h"

G_BEGIN_DECLS

void conn_presence_class_init (GabbleConnectionClass *klass);
void conn_presence_init (GabbleConnection *conn);
void conn_presence_finalize (GabbleConnection *conn);
void conn_presence_iface_init (gpointer g_iface, gpointer iface_data);
void conn_presence_emit_presence_update (
    GabbleConnection *, const GArray *contact_handles);

void conn_decloak_iface_init (gpointer g_iface, gpointer iface_data);
void conn_decloak_emit_requested (GabbleConnection *conn,
    TpHandle contact, const gchar *reason, gboolean decloaked);

G_END_DECLS

#endif /* __CONN_PRESENCE_H__ */

