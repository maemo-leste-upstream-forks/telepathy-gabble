/*
 * util.h - Headers for Gabble utility functions
 * Copyright (C) 2006 Collabora Ltd.
 * Copyright (C) 2006 Nokia Corporation
 *   @author Robert McQueen <robert.mcqueen@collabora.co.uk>
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

#ifndef __GABBLE_UTIL_H__
#define __GABBLE_UTIL_H__

#include <config.h>

#include <glib.h>
#include <telepathy-glib/handle-repo.h>
#include <telepathy-glib/util.h>
#include <loudmouth/loudmouth.h>

#include "types.h"

/* Guarantees that the resulting hash is in lower-case */
gchar *sha1_hex (const gchar *bytes, guint len);

/* A SHA1 digest is 20 bytes long */
#define SHA1_HASH_SIZE 20
void sha1_bin (const gchar *bytes, guint len, guchar out[SHA1_HASH_SIZE]);

void lm_message_node_add_own_nick (LmMessageNode *node,
    GabbleConnection *conn);
void lm_message_node_unlink (LmMessageNode *orphan);
void lm_message_node_steal_children (LmMessageNode *snatcher,
    LmMessageNode *mum);
gboolean lm_message_node_has_namespace (LmMessageNode *node, const gchar *ns,
    const gchar *tag);
LmMessageNode *lm_message_node_get_child_with_namespace (LmMessageNode *node,
    const gchar *name, const gchar *ns);
G_GNUC_NULL_TERMINATED LmMessage *lm_message_build (const gchar *to,
    LmMessageType type, guint spec, ...);
G_GNUC_NULL_TERMINATED LmMessage * lm_message_build_with_sub_type (
    const gchar *to, LmMessageType type, LmMessageSubType sub_type,
    guint spec, ...);

void lm_message_node_add_child_node (LmMessageNode *node,
    LmMessageNode *child);

/* format: a@b/c */
void gabble_decode_jid (const gchar *jid, gchar **a, gchar **b, gchar **c);

gchar *gabble_remove_resource (const gchar *jid);
gchar *gabble_normalize_contact (TpHandleRepoIface *repo, const gchar *jid,
    gpointer userdata, GError **error);
gchar *gabble_normalize_room (TpHandleRepoIface *repo, const gchar *jid,
    gpointer context, GError **error);
TpHandle gabble_get_room_handle_from_jid (TpHandleRepoIface *room_repo,
    const gchar *jid);

GHashTable *lm_message_node_extract_properties (LmMessageNode *node,
    const gchar *prop);
void
lm_message_node_add_children_from_properties (LmMessageNode *node,
    GHashTable *properties, const gchar *prop);
const gchar * lm_message_node_get_namespace (LmMessageNode *node);
const gchar * lm_message_node_get_name (LmMessageNode *node);
LmMessageNode * lm_message_node_get_child_any_ns (LmMessageNode *node,
    const gchar *name);


#endif /* __GABBLE_UTIL_H__ */
