/*
 * base-call-channel.h - Header for TpyBaseCallChannel
 * Copyright © 2009–2010 Collabora Ltd.
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

#ifndef __TPY_BASE_CALL_CHANNEL_H__
#define __TPY_BASE_CALL_CHANNEL_H__

#include <glib-object.h>

#include <telepathy-yell/enums.h>
#include <telepathy-yell/base-call-content.h>

#include <telepathy-glib/telepathy-glib.h>

G_BEGIN_DECLS

typedef struct _TpyBaseCallChannel TpyBaseCallChannel;
typedef struct _TpyBaseCallChannelPrivate TpyBaseCallChannelPrivate;
typedef struct _TpyBaseCallChannelClass TpyBaseCallChannelClass;

struct _TpyBaseCallChannelClass {
    TpBaseChannelClass parent_class;

    void (*accept) (TpyBaseCallChannel *self);
    TpyBaseCallContent * (*add_content) (TpyBaseCallChannel *self,
      const gchar *name,
      TpMediaStreamType media,
      GError **error);

    void (*hangup) (TpyBaseCallChannel *self,
      guint reason,
      const gchar *detailed_reason,
      const gchar *message);
};

struct _TpyBaseCallChannel {
    TpBaseChannel parent;

    gboolean initial_audio;
    gboolean initial_video;

    TpyBaseCallChannelPrivate *priv;
};

GType tpy_base_call_channel_get_type (void);

/* TYPE MACROS */
#define TPY_TYPE_BASE_CALL_CHANNEL \
  (tpy_base_call_channel_get_type ())
#define TPY_BASE_CALL_CHANNEL(obj) \
  (G_TYPE_CHECK_INSTANCE_CAST ((obj), \
   TPY_TYPE_BASE_CALL_CHANNEL, TpyBaseCallChannel))
#define TPY_BASE_CALL_CHANNEL_CLASS(klass) \
  (G_TYPE_CHECK_CLASS_CAST((klass), \
   TPY_TYPE_BASE_CALL_CHANNEL, TpyBaseCallChannelClass))
#define TPY_IS_BASE_CALL_CHANNEL(obj) \
  (G_TYPE_CHECK_INSTANCE_TYPE((obj), TPY_TYPE_BASE_CALL_CHANNEL))
#define TPY_IS_BASE_CALL_CHANNEL_CLASS(klass) \
  (G_TYPE_CHECK_CLASS_TYPE((klass), TPY_TYPE_BASE_CALL_CHANNEL))
#define TPY_BASE_CALL_CHANNEL_GET_CLASS(obj) \
  (G_TYPE_INSTANCE_GET_CLASS ((obj), \
   TPY_TYPE_BASE_CALL_CHANNEL, TpyBaseCallChannelClass))

TpyCallState tpy_base_call_channel_get_state (
  TpyBaseCallChannel *self);

void tpy_base_call_channel_set_state (TpyBaseCallChannel *self,
  TpyCallState state);

GList * tpy_base_call_channel_get_contents (TpyBaseCallChannel *self);

void tpy_base_call_channel_add_content (
    TpyBaseCallChannel *self,
    TpyBaseCallContent *content);

void tpy_base_call_channel_remove_content (TpyBaseCallChannel *self,
    TpyBaseCallContent *content);

void tpy_base_call_channel_update_member_flags (TpyBaseCallChannel *self,
    TpHandle handle,
    TpyCallMemberFlags flags);

void tpy_base_call_channel_add_member (TpyBaseCallChannel *self,
    TpHandle handle,
    TpyCallMemberFlags initial_flags);

void tpy_base_call_channel_remove_member (TpyBaseCallChannel *self,
    TpHandle handle);

G_END_DECLS

#endif /* #ifndef __TPY_BASE_CALL_CHANNEL_H__*/
