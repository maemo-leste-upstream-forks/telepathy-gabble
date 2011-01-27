/*
 * base-call-content.c - Source for TpyBaseCallContent
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

#include "base-call-content.h"

#include "base-call-stream.h"

#define DEBUG_FLAG TPY_DEBUG_CALL
#include "debug.h"

#include <telepathy-yell/interfaces.h>
#include <telepathy-yell/gtypes.h>
#include <telepathy-yell/enums.h>
#include <telepathy-yell/svc-call.h>

static void call_content_iface_init (gpointer g_iface, gpointer iface_data);

G_DEFINE_TYPE_WITH_CODE(TpyBaseCallContent, tpy_base_call_content,
    G_TYPE_OBJECT,
    G_IMPLEMENT_INTERFACE (TP_TYPE_SVC_DBUS_PROPERTIES,
        tp_dbus_properties_mixin_iface_init);
    G_IMPLEMENT_INTERFACE (TPY_TYPE_SVC_CALL_CONTENT,
        call_content_iface_init);
    );

struct _TpyBaseCallContentPrivate
{
  TpBaseConnection *conn;
  TpDBusDaemon *dbus_daemon;

  gchar *object_path;

  gchar *name;
  TpMediaStreamType media_type;
  TpHandle creator;
  TpyCallContentDisposition disposition;

  GList *streams;

  gboolean dispose_has_run;
  gboolean deinit_has_run;
};

enum
{
  PROP_OBJECT_PATH = 1,
  PROP_CONNECTION,

  PROP_INTERFACES,
  PROP_NAME,
  PROP_MEDIA_TYPE,
  PROP_CREATOR,
  PROP_DISPOSITION,
  PROP_STREAMS
};

static void base_call_content_deinit_real (TpyBaseCallContent *self);

static void
tpy_base_call_content_init (TpyBaseCallContent *self)
{
  TpyBaseCallContentPrivate *priv = G_TYPE_INSTANCE_GET_PRIVATE (self,
      TPY_TYPE_BASE_CALL_CONTENT, TpyBaseCallContentPrivate);

  self->priv = priv;
}

static void
tpy_base_call_content_constructed (GObject *obj)
{
  TpyBaseCallContent *self = TPY_BASE_CALL_CONTENT (obj);
  TpyBaseCallContentPrivate *priv = self->priv;

  if (G_OBJECT_CLASS (tpy_base_call_content_parent_class)->constructed != NULL)
    G_OBJECT_CLASS (tpy_base_call_content_parent_class)->constructed (obj);

  DEBUG ("Registering %s", priv->object_path);
  priv->dbus_daemon = g_object_ref (
      tp_base_connection_get_dbus_daemon ((TpBaseConnection *) priv->conn));
  tp_dbus_daemon_register_object (priv->dbus_daemon, priv->object_path, obj);
}

static void
tpy_base_call_content_dispose (GObject *object)
{
  TpyBaseCallContent *self = TPY_BASE_CALL_CONTENT (object);
  TpyBaseCallContentPrivate *priv = self->priv;
  GList *l;

  if (priv->dispose_has_run)
    return;

  priv->dispose_has_run = TRUE;

  for (l = priv->streams; l != NULL; l = g_list_next (l))
    g_object_unref (l->data);

  tp_clear_pointer (&priv->streams, g_list_free);
  tp_clear_object (&priv->conn);

  if (G_OBJECT_CLASS (tpy_base_call_content_parent_class)->dispose != NULL)
    G_OBJECT_CLASS (tpy_base_call_content_parent_class)->dispose (object);
}

static void
tpy_base_call_content_finalize (GObject *object)
{
  TpyBaseCallContent *self = TPY_BASE_CALL_CONTENT (object);
  TpyBaseCallContentPrivate *priv = self->priv;

  /* free any data held directly by the object here */
  g_free (priv->object_path);
  g_free (priv->name);

  G_OBJECT_CLASS (tpy_base_call_content_parent_class)->finalize (object);
}

static void
tpy_base_call_content_get_property (
    GObject *object,
    guint property_id,
    GValue *value,
    GParamSpec *pspec)
{
  TpyBaseCallContent *content = TPY_BASE_CALL_CONTENT (object);
  TpyBaseCallContentPrivate *priv = content->priv;

  switch (property_id)
    {
      case PROP_OBJECT_PATH:
        g_value_set_string (value, priv->object_path);
        break;
      case PROP_CONNECTION:
        g_value_set_object (value, priv->conn);
        break;
      case PROP_INTERFACES:
        {
          TpyBaseCallContentClass *klass =
              TPY_BASE_CALL_CONTENT_GET_CLASS (content);

          if (klass->extra_interfaces != NULL)
            {
              g_value_set_boxed (value, klass->extra_interfaces);
            }
          else
            {
              static gchar *empty[] = { NULL };

              g_value_set_boxed (value, empty);
            }
          break;
        }
      case PROP_NAME:
        g_value_set_string (value, priv->name);
        break;
      case PROP_MEDIA_TYPE:
        g_value_set_uint (value, priv->media_type);
        break;
      case PROP_CREATOR:
        g_value_set_uint (value, priv->creator);
        break;
      case PROP_DISPOSITION:
        g_value_set_uint (value, priv->disposition);
        break;
      case PROP_STREAMS:
        {
          GPtrArray *arr = g_ptr_array_sized_new (2);
          GList *l;

          for (l = priv->streams; l != NULL; l = g_list_next (l))
            {
              TpyBaseCallStream *s = TPY_BASE_CALL_STREAM (l->data);
              g_ptr_array_add (arr,
                  g_strdup (tpy_base_call_stream_get_object_path (s)));
            }

          g_value_take_boxed (value, arr);
          break;
        }
      default:
        G_OBJECT_WARN_INVALID_PROPERTY_ID (object, property_id, pspec);
        break;
    }
}

static void
tpy_base_call_content_set_property (
    GObject *object,
    guint property_id,
    const GValue *value,
    GParamSpec *pspec)
{
  TpyBaseCallContent *content = TPY_BASE_CALL_CONTENT (object);
  TpyBaseCallContentPrivate *priv = content->priv;

  switch (property_id)
    {
      case PROP_OBJECT_PATH:
        priv->object_path = g_value_dup_string (value);
        g_assert (priv->object_path != NULL);
        break;
      case PROP_CONNECTION:
        priv->conn = g_value_dup_object (value);
        break;
      case PROP_NAME:
        priv->name = g_value_dup_string (value);
        break;
      case PROP_MEDIA_TYPE:
        priv->media_type = g_value_get_uint (value);
        break;
      case PROP_CREATOR:
        priv->creator = g_value_get_uint (value);
        break;
      case PROP_DISPOSITION:
        priv->disposition = g_value_get_uint (value);
        break;
      default:
        G_OBJECT_WARN_INVALID_PROPERTY_ID (object, property_id, pspec);
        break;
    }
}

static void
tpy_base_call_content_class_init (
    TpyBaseCallContentClass *bcc_class)
{
  GObjectClass *object_class = G_OBJECT_CLASS (bcc_class);
  GParamSpec *param_spec;
  static TpDBusPropertiesMixinPropImpl content_props[] = {
    { "Interfaces", "interfaces", NULL },
    { "Name", "name", NULL },
    { "Type", "media-type", NULL },
    { "Disposition", "disposition", NULL },
    { "Streams", "streams", NULL },
    { NULL }
  };
  static TpDBusPropertiesMixinIfaceImpl prop_interfaces[] = {
      { TPY_IFACE_CALL_CONTENT,
        tp_dbus_properties_mixin_getter_gobject_properties,
        NULL,
        content_props,
      },
      { NULL }
  };

  g_type_class_add_private (bcc_class, sizeof (TpyBaseCallContentPrivate));

  object_class->constructed = tpy_base_call_content_constructed;
  object_class->dispose = tpy_base_call_content_dispose;
  object_class->finalize = tpy_base_call_content_finalize;
  object_class->get_property = tpy_base_call_content_get_property;
  object_class->set_property = tpy_base_call_content_set_property;

  param_spec = g_param_spec_string ("object-path", "D-Bus object path",
      "The D-Bus object path used for this object on the bus.",
      NULL,
      G_PARAM_CONSTRUCT_ONLY | G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS);
  g_object_class_install_property (object_class, PROP_OBJECT_PATH, param_spec);

  param_spec = g_param_spec_object ("connection", "TpBaseConnection object",
      "Tp base connection object that owns this call content",
      TP_TYPE_BASE_CONNECTION,
      G_PARAM_CONSTRUCT_ONLY | G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS);
  g_object_class_install_property (object_class, PROP_CONNECTION, param_spec);

  param_spec = g_param_spec_boxed ("interfaces", "Extra D-Bus interfaces",
      "Additional interfaces implemented by this content",
      G_TYPE_STRV,
      G_PARAM_READABLE | G_PARAM_STATIC_STRINGS);
  g_object_class_install_property (object_class, PROP_INTERFACES, param_spec);

  param_spec = g_param_spec_string ("name", "Name",
      "The name of this content, if any",
      "",
      G_PARAM_CONSTRUCT_ONLY | G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS);
  g_object_class_install_property (object_class, PROP_NAME, param_spec);

  param_spec = g_param_spec_uint ("media-type", "Media Type",
      "The media type of this content",
      0, G_MAXUINT, 0,
      G_PARAM_CONSTRUCT_ONLY | G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS);
  g_object_class_install_property (object_class, PROP_MEDIA_TYPE, param_spec);

  param_spec = g_param_spec_uint ("creator", "Creator",
      "The creator of this content",
      0, G_MAXUINT, 0,
      G_PARAM_CONSTRUCT_ONLY | G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS);
  g_object_class_install_property (object_class, PROP_CREATOR, param_spec);

  param_spec = g_param_spec_uint ("disposition", "Disposition",
      "The disposition of this content",
      0, G_MAXUINT, 0,
      G_PARAM_CONSTRUCT_ONLY | G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS);
  g_object_class_install_property (object_class, PROP_DISPOSITION, param_spec);

  param_spec = g_param_spec_boxed ("streams", "Stream",
      "The streams of this content",
      TP_ARRAY_TYPE_OBJECT_PATH_LIST,
      G_PARAM_READABLE | G_PARAM_STATIC_STRINGS);
  g_object_class_install_property (object_class, PROP_STREAMS,
      param_spec);

  bcc_class->dbus_props_class.interfaces = prop_interfaces;
  tp_dbus_properties_mixin_class_init (object_class,
      G_STRUCT_OFFSET (TpyBaseCallContentClass, dbus_props_class));

  bcc_class->deinit = base_call_content_deinit_real;
}

TpBaseConnection *
tpy_base_call_content_get_connection (TpyBaseCallContent *self)
{
  g_return_val_if_fail (TPY_IS_BASE_CALL_CONTENT (self), NULL);

  return self->priv->conn;
}

const gchar *
tpy_base_call_content_get_object_path (TpyBaseCallContent *self)
{
  g_return_val_if_fail (TPY_IS_BASE_CALL_CONTENT (self), NULL);

  return self->priv->object_path;
}

const gchar *
tpy_base_call_content_get_name (TpyBaseCallContent *self)
{
  g_return_val_if_fail (TPY_IS_BASE_CALL_CONTENT (self), NULL);

  return self->priv->name;
}

TpMediaStreamType
tpy_base_call_content_get_media_type (TpyBaseCallContent *self)
{
  g_return_val_if_fail (TPY_IS_BASE_CALL_CONTENT (self),
      TP_MEDIA_STREAM_TYPE_AUDIO);

  return self->priv->media_type;
}

TpyCallContentDisposition
tpy_base_call_content_get_disposition (TpyBaseCallContent *self)
{
  g_return_val_if_fail (TPY_IS_BASE_CALL_CONTENT (self),
      TPY_CALL_CONTENT_DISPOSITION_NONE);

  return self->priv->disposition;
}

GList *
tpy_base_call_content_get_streams (TpyBaseCallContent *self)
{
  g_return_val_if_fail (TPY_IS_BASE_CALL_CONTENT (self), NULL);

  return self->priv->streams;
}

void
tpy_base_call_content_add_stream (TpyBaseCallContent *self,
    TpyBaseCallStream *stream)
{
  GPtrArray *paths;

  g_return_if_fail (TPY_IS_BASE_CALL_CONTENT (self));

  self->priv->streams = g_list_prepend (self->priv->streams,
      g_object_ref (stream));

  paths = g_ptr_array_new_with_free_func ((GDestroyNotify) g_free);

  g_ptr_array_add (paths, g_strdup (
     tpy_base_call_stream_get_object_path (
         TPY_BASE_CALL_STREAM (stream))));
  tpy_svc_call_content_emit_streams_added (self, paths);
  g_ptr_array_unref (paths);
}

void
tpy_base_call_content_remove_stream (TpyBaseCallContent *self,
    TpyBaseCallStream *stream)
{
  TpyBaseCallContentPrivate *priv;
  GList *l;
  GPtrArray *paths;

  g_return_if_fail (TPY_IS_BASE_CALL_CONTENT (self));

  priv = self->priv;

  l = g_list_find (priv->streams, stream);
  g_return_if_fail (l != NULL);

  priv->streams = g_list_remove_link (priv->streams, l);
  paths = g_ptr_array_new_with_free_func ((GDestroyNotify) g_free);
  g_ptr_array_add (paths, g_strdup (
     tpy_base_call_stream_get_object_path (
         TPY_BASE_CALL_STREAM (stream))));
  tpy_svc_call_content_emit_streams_removed (self, paths);
  g_ptr_array_unref (paths);
  g_object_unref (stream);
}

static void
base_call_content_deinit_real (TpyBaseCallContent *self)
{
  TpyBaseCallContentPrivate *priv = self->priv;

  if (priv->deinit_has_run)
    return;

  priv->deinit_has_run = TRUE;

  tp_dbus_daemon_unregister_object (priv->dbus_daemon, G_OBJECT (self));
  tp_clear_object (&priv->dbus_daemon);

  g_list_foreach (priv->streams, (GFunc) g_object_unref, NULL);
  tp_clear_pointer (&priv->streams, g_list_free);
}

void
tpy_base_call_content_deinit (TpyBaseCallContent *self)
{
  TpyBaseCallContentClass *klass;

  g_return_if_fail (TPY_IS_BASE_CALL_CONTENT (self));

  klass = TPY_BASE_CALL_CONTENT_GET_CLASS (self);
  g_return_if_fail (klass->deinit != NULL);
  klass->deinit (self);
}

void
tpy_base_call_content_accepted (TpyBaseCallContent *self)
{
  TpyBaseCallContentPrivate *priv = self->priv;
  GList *l;

  if (priv->disposition != TPY_CALL_CONTENT_DISPOSITION_INITIAL)
    return;

  for (l = priv->streams ; l != NULL; l = g_list_next (l))
    {
      TpyBaseCallStream *s = TPY_BASE_CALL_STREAM (l->data);

      if (tpy_base_call_stream_get_local_sending_state (s) ==
          TPY_SENDING_STATE_PENDING_SEND)
        tpy_base_call_stream_set_sending (s, TRUE, NULL);
    }
}

static void
tpy_call_content_remove (TpySvcCallContent *content,
    TpyContentRemovalReason reason,
    const gchar *detailed_removal_reason,
    const gchar *message,
    DBusGMethodInvocation *context)
{
  /* TODO: actually do something with this reason and message. */
  DEBUG ("removing content for reason %u, dbus error: %s, message: %s",
      reason, detailed_removal_reason, message);

  tpy_svc_call_content_emit_removed (content);
  /* it doesn't matter if a ::removed signal handler calls deinit as
   * there are guards around it being called again and breaking, so
   * let's just call it be sure it's done. */
  tpy_base_call_content_deinit (TPY_BASE_CALL_CONTENT (content));
  tpy_svc_call_content_return_from_remove (context);
}

static void
call_content_iface_init (gpointer g_iface, gpointer iface_data)
{
  TpySvcCallContentClass *klass =
    (TpySvcCallContentClass *) g_iface;

#define IMPLEMENT(x) tpy_svc_call_content_implement_##x (\
    klass, tpy_call_content_##x)
  IMPLEMENT(remove);
#undef IMPLEMENT
}
