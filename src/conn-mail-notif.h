/*
 * conn-mail-notif.h - Header for Gabble connection mail notification interface
 * Copyright (C) 2009 Collabora Ltd.
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

#ifndef __CONN_EMAIL_NOTIF_H__
#define __CONN_EMAIL_NOTIF_H__

#include <glib-object.h>

#include "connection.h"

G_BEGIN_DECLS

void conn_mail_notif_init (GabbleConnection *conn);
void conn_mail_notif_dispose (GabbleConnection *conn);
void conn_mail_notif_iface_init (gpointer g_iface, gpointer iface_data);
void conn_mail_notif_properties_getter (GObject *object, GQuark interface,
    GQuark name, GValue *value, gpointer getter_data);

G_END_DECLS

#endif /* __CONN_EMAIL_NOTIF_H__ */

