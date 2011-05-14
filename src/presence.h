/*
 * gabble-presence.h - Headers for Gabble's per-contact presence structure
 * Copyright (C) 2005 Collabora Ltd.
 * Copyright (C) 2005 Nokia Corporation
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


#ifndef __GABBLE_PRESENCE_H__
#define __GABBLE_PRESENCE_H__

#include <glib-object.h>

#include "capabilities.h"
#include "connection.h"
#include "types.h"

G_BEGIN_DECLS

#define GABBLE_TYPE_PRESENCE gabble_presence_get_type ()

#define GABBLE_PRESENCE(obj) \
    (G_TYPE_CHECK_INSTANCE_CAST ((obj), \
                                   GABBLE_TYPE_PRESENCE, GabblePresence))

#define GABBLE_PRESENCE_CLASS(klass) \
    (G_TYPE_CHECK_CLASS_CAST ((klass), \
                                GABBLE_TYPE_PRESENCE, GabblePresenceClass))

#define GABBLE_IS_PRESENCE(obj) \
    (G_TYPE_CHECK_INSTANCE_TYPE ((obj), \
                                   GABBLE_TYPE_PRESENCE))

#define GABBLE_IS_PRESENCE_CLASS(klass) \
    (G_TYPE_CHECK_CLASS_TYPE ((klass), \
                                GABBLE_TYPE_PRESENCE))

#define GABBLE_PRESENCE_GET_CLASS(obj) \
    (G_TYPE_INSTANCE_GET_CLASS ((obj), \
                                  GABBLE_TYPE_PRESENCE, GabblePresenceClass))

typedef struct _GabblePresencePrivate GabblePresencePrivate;

struct _GabblePresence {
    GObject parent;
    GabblePresenceId status;
    gchar *status_message;
    gchar *nickname;
    gchar *avatar_sha1;
    guint client_types;
    gboolean keep_unavailable;
    GabblePresencePrivate *priv;
};

typedef enum
{
  GABBLE_CLIENT_TYPE_BOT      = 1 << 0,
  GABBLE_CLIENT_TYPE_CONSOLE  = 1 << 1,
  GABBLE_CLIENT_TYPE_GAME     = 1 << 2,
  GABBLE_CLIENT_TYPE_HANDHELD = 1 << 3,
  GABBLE_CLIENT_TYPE_PC       = 1 << 4,
  GABBLE_CLIENT_TYPE_PHONE    = 1 << 5,
  GABBLE_CLIENT_TYPE_WEB      = 1 << 6,
  GABBLE_CLIENT_TYPE_SMS      = 1 << 7,
} GabbleClientType;

typedef struct _GabblePresenceClass GabblePresenceClass;

struct _GabblePresenceClass {
    GObjectClass parent_class;
};

GType gabble_presence_get_type (void);

GabblePresence* gabble_presence_new (void);

gboolean gabble_presence_update (GabblePresence *presence,
    const gchar *resource, GabblePresenceId status,
    const gchar *status_message, gint8 priority,
    gboolean *update_client_types,
    time_t now);

void gabble_presence_set_capabilities (GabblePresence *presence,
    const gchar *resource,
    const GabbleCapabilitySet *cap_set,
    guint serial);

gboolean gabble_presence_has_cap (GabblePresence *presence, const gchar *ns);
GabbleCapabilitySet *gabble_presence_dup_caps (GabblePresence *presence);
const GabbleCapabilitySet *gabble_presence_peek_caps (GabblePresence *presence);

gboolean gabble_presence_has_resources (GabblePresence *self);

const gchar *gabble_presence_pick_resource_by_caps (GabblePresence *presence,
    GabbleClientType preferred_client_type,
    GabbleCapabilitySetPredicate predicate,
    gconstpointer user_data);

gboolean gabble_presence_resource_has_caps (GabblePresence *presence,
    const gchar *resource, GabbleCapabilitySetPredicate predicate,
    gconstpointer user_data);

LmMessage *gabble_presence_as_message (GabblePresence *presence,
    const gchar *to);
void gabble_presence_add_status_and_vcard (GabblePresence *presence,
  WockyStanza *stanza);

gchar *gabble_presence_dump (GabblePresence *presence);

gboolean gabble_presence_added_to_view (GabblePresence *presence);
gboolean gabble_presence_removed_from_view (GabblePresence *presence);

/* Data-driven feature fallback */
typedef struct {
    gboolean considered;
    gconstpointer check_data;
    gconstpointer result;
} GabbleFeatureFallback;
gconstpointer gabble_presence_resource_pick_best_feature (
    GabblePresence *presence,
    const gchar *resource,
    const GabbleFeatureFallback *table,
    GabbleCapabilitySetPredicate predicate);

gconstpointer
gabble_presence_pick_best_feature (GabblePresence *presence,
    const GabbleFeatureFallback *table,
    GabbleCapabilitySetPredicate predicate);

gboolean gabble_presence_update_client_types (GabblePresence *presence,
    const gchar *resource,
    guint client_types);

gchar **gabble_presence_get_client_types_array (GabblePresence *presence,
    const gchar **resource_name);

G_END_DECLS

#endif /* __GABBLE_PRESENCE_H__ */

