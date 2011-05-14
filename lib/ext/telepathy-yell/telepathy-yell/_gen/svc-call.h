#include <glib-object.h>
#include <dbus/dbus-glib.h>
#include <telepathy-glib/dbus-properties-mixin.h>

G_BEGIN_DECLS

typedef struct _TpySvcCallContent TpySvcCallContent;

typedef struct _TpySvcCallContentClass TpySvcCallContentClass;

GType tpy_svc_call_content_get_type (void);
#define TPY_TYPE_SVC_CALL_CONTENT \
  (tpy_svc_call_content_get_type ())
#define TPY_SVC_CALL_CONTENT(obj) \
  (G_TYPE_CHECK_INSTANCE_CAST((obj), TPY_TYPE_SVC_CALL_CONTENT, TpySvcCallContent))
#define TPY_IS_SVC_CALL_CONTENT(obj) \
  (G_TYPE_CHECK_INSTANCE_TYPE((obj), TPY_TYPE_SVC_CALL_CONTENT))
#define TPY_SVC_CALL_CONTENT_GET_CLASS(obj) \
  (G_TYPE_INSTANCE_GET_INTERFACE((obj), TPY_TYPE_SVC_CALL_CONTENT, TpySvcCallContentClass))


typedef void (*tpy_svc_call_content_remove_impl) (TpySvcCallContent *self,
    guint in_Reason,
    const gchar *in_Detailed_Removal_Reason,
    const gchar *in_Message,
    DBusGMethodInvocation *context);
void tpy_svc_call_content_implement_remove (TpySvcCallContentClass *klass, tpy_svc_call_content_remove_impl impl);
static inline
/* this comment is to stop gtkdoc realising this is static */
void tpy_svc_call_content_return_from_remove (DBusGMethodInvocation *context);
static inline void
tpy_svc_call_content_return_from_remove (DBusGMethodInvocation *context)
{
  dbus_g_method_return (context);
}

void tpy_svc_call_content_emit_removed (gpointer instance);
void tpy_svc_call_content_emit_streams_added (gpointer instance,
    const GPtrArray *arg_Streams);
void tpy_svc_call_content_emit_streams_removed (gpointer instance,
    const GPtrArray *arg_Streams);

typedef struct _TpySvcCallContentCodecOffer TpySvcCallContentCodecOffer;

typedef struct _TpySvcCallContentCodecOfferClass TpySvcCallContentCodecOfferClass;

GType tpy_svc_call_content_codec_offer_get_type (void);
#define TPY_TYPE_SVC_CALL_CONTENT_CODEC_OFFER \
  (tpy_svc_call_content_codec_offer_get_type ())
#define TPY_SVC_CALL_CONTENT_CODEC_OFFER(obj) \
  (G_TYPE_CHECK_INSTANCE_CAST((obj), TPY_TYPE_SVC_CALL_CONTENT_CODEC_OFFER, TpySvcCallContentCodecOffer))
#define TPY_IS_SVC_CALL_CONTENT_CODEC_OFFER(obj) \
  (G_TYPE_CHECK_INSTANCE_TYPE((obj), TPY_TYPE_SVC_CALL_CONTENT_CODEC_OFFER))
#define TPY_SVC_CALL_CONTENT_CODEC_OFFER_GET_CLASS(obj) \
  (G_TYPE_INSTANCE_GET_INTERFACE((obj), TPY_TYPE_SVC_CALL_CONTENT_CODEC_OFFER, TpySvcCallContentCodecOfferClass))


typedef void (*tpy_svc_call_content_codec_offer_accept_impl) (TpySvcCallContentCodecOffer *self,
    const GPtrArray *in_Codecs,
    DBusGMethodInvocation *context);
void tpy_svc_call_content_codec_offer_implement_accept (TpySvcCallContentCodecOfferClass *klass, tpy_svc_call_content_codec_offer_accept_impl impl);
static inline
/* this comment is to stop gtkdoc realising this is static */
void tpy_svc_call_content_codec_offer_return_from_accept (DBusGMethodInvocation *context);
static inline void
tpy_svc_call_content_codec_offer_return_from_accept (DBusGMethodInvocation *context)
{
  dbus_g_method_return (context);
}

typedef void (*tpy_svc_call_content_codec_offer_reject_impl) (TpySvcCallContentCodecOffer *self,
    DBusGMethodInvocation *context);
void tpy_svc_call_content_codec_offer_implement_reject (TpySvcCallContentCodecOfferClass *klass, tpy_svc_call_content_codec_offer_reject_impl impl);
static inline
/* this comment is to stop gtkdoc realising this is static */
void tpy_svc_call_content_codec_offer_return_from_reject (DBusGMethodInvocation *context);
static inline void
tpy_svc_call_content_codec_offer_return_from_reject (DBusGMethodInvocation *context)
{
  dbus_g_method_return (context);
}


typedef struct _TpySvcCallContentInterfaceMedia TpySvcCallContentInterfaceMedia;

typedef struct _TpySvcCallContentInterfaceMediaClass TpySvcCallContentInterfaceMediaClass;

GType tpy_svc_call_content_interface_media_get_type (void);
#define TPY_TYPE_SVC_CALL_CONTENT_INTERFACE_MEDIA \
  (tpy_svc_call_content_interface_media_get_type ())
#define TPY_SVC_CALL_CONTENT_INTERFACE_MEDIA(obj) \
  (G_TYPE_CHECK_INSTANCE_CAST((obj), TPY_TYPE_SVC_CALL_CONTENT_INTERFACE_MEDIA, TpySvcCallContentInterfaceMedia))
#define TPY_IS_SVC_CALL_CONTENT_INTERFACE_MEDIA(obj) \
  (G_TYPE_CHECK_INSTANCE_TYPE((obj), TPY_TYPE_SVC_CALL_CONTENT_INTERFACE_MEDIA))
#define TPY_SVC_CALL_CONTENT_INTERFACE_MEDIA_GET_CLASS(obj) \
  (G_TYPE_INSTANCE_GET_INTERFACE((obj), TPY_TYPE_SVC_CALL_CONTENT_INTERFACE_MEDIA, TpySvcCallContentInterfaceMediaClass))


typedef void (*tpy_svc_call_content_interface_media_update_codecs_impl) (TpySvcCallContentInterfaceMedia *self,
    const GPtrArray *in_Codecs,
    DBusGMethodInvocation *context);
void tpy_svc_call_content_interface_media_implement_update_codecs (TpySvcCallContentInterfaceMediaClass *klass, tpy_svc_call_content_interface_media_update_codecs_impl impl);
static inline
/* this comment is to stop gtkdoc realising this is static */
void tpy_svc_call_content_interface_media_return_from_update_codecs (DBusGMethodInvocation *context);
static inline void
tpy_svc_call_content_interface_media_return_from_update_codecs (DBusGMethodInvocation *context)
{
  dbus_g_method_return (context);
}

void tpy_svc_call_content_interface_media_emit_codecs_changed (gpointer instance,
    GHashTable *arg_Updated_Codecs,
    const GArray *arg_Removed_Contacts);
void tpy_svc_call_content_interface_media_emit_new_codec_offer (gpointer instance,
    guint arg_Contact,
    const gchar *arg_Offer,
    const GPtrArray *arg_Codecs);

typedef struct _TpySvcCallContentInterfaceMute TpySvcCallContentInterfaceMute;

typedef struct _TpySvcCallContentInterfaceMuteClass TpySvcCallContentInterfaceMuteClass;

GType tpy_svc_call_content_interface_mute_get_type (void);
#define TPY_TYPE_SVC_CALL_CONTENT_INTERFACE_MUTE \
  (tpy_svc_call_content_interface_mute_get_type ())
#define TPY_SVC_CALL_CONTENT_INTERFACE_MUTE(obj) \
  (G_TYPE_CHECK_INSTANCE_CAST((obj), TPY_TYPE_SVC_CALL_CONTENT_INTERFACE_MUTE, TpySvcCallContentInterfaceMute))
#define TPY_IS_SVC_CALL_CONTENT_INTERFACE_MUTE(obj) \
  (G_TYPE_CHECK_INSTANCE_TYPE((obj), TPY_TYPE_SVC_CALL_CONTENT_INTERFACE_MUTE))
#define TPY_SVC_CALL_CONTENT_INTERFACE_MUTE_GET_CLASS(obj) \
  (G_TYPE_INSTANCE_GET_INTERFACE((obj), TPY_TYPE_SVC_CALL_CONTENT_INTERFACE_MUTE, TpySvcCallContentInterfaceMuteClass))


typedef void (*tpy_svc_call_content_interface_mute_set_muted_impl) (TpySvcCallContentInterfaceMute *self,
    gboolean in_Muted,
    DBusGMethodInvocation *context);
void tpy_svc_call_content_interface_mute_implement_set_muted (TpySvcCallContentInterfaceMuteClass *klass, tpy_svc_call_content_interface_mute_set_muted_impl impl);
static inline
/* this comment is to stop gtkdoc realising this is static */
void tpy_svc_call_content_interface_mute_return_from_set_muted (DBusGMethodInvocation *context);
static inline void
tpy_svc_call_content_interface_mute_return_from_set_muted (DBusGMethodInvocation *context)
{
  dbus_g_method_return (context);
}

void tpy_svc_call_content_interface_mute_emit_mute_state_changed (gpointer instance,
    gboolean arg_MuteState);

typedef struct _TpySvcCallStream TpySvcCallStream;

typedef struct _TpySvcCallStreamClass TpySvcCallStreamClass;

GType tpy_svc_call_stream_get_type (void);
#define TPY_TYPE_SVC_CALL_STREAM \
  (tpy_svc_call_stream_get_type ())
#define TPY_SVC_CALL_STREAM(obj) \
  (G_TYPE_CHECK_INSTANCE_CAST((obj), TPY_TYPE_SVC_CALL_STREAM, TpySvcCallStream))
#define TPY_IS_SVC_CALL_STREAM(obj) \
  (G_TYPE_CHECK_INSTANCE_TYPE((obj), TPY_TYPE_SVC_CALL_STREAM))
#define TPY_SVC_CALL_STREAM_GET_CLASS(obj) \
  (G_TYPE_INSTANCE_GET_INTERFACE((obj), TPY_TYPE_SVC_CALL_STREAM, TpySvcCallStreamClass))


typedef void (*tpy_svc_call_stream_set_sending_impl) (TpySvcCallStream *self,
    gboolean in_Send,
    DBusGMethodInvocation *context);
void tpy_svc_call_stream_implement_set_sending (TpySvcCallStreamClass *klass, tpy_svc_call_stream_set_sending_impl impl);
static inline
/* this comment is to stop gtkdoc realising this is static */
void tpy_svc_call_stream_return_from_set_sending (DBusGMethodInvocation *context);
static inline void
tpy_svc_call_stream_return_from_set_sending (DBusGMethodInvocation *context)
{
  dbus_g_method_return (context);
}

typedef void (*tpy_svc_call_stream_request_receiving_impl) (TpySvcCallStream *self,
    guint in_Contact,
    gboolean in_Receive,
    DBusGMethodInvocation *context);
void tpy_svc_call_stream_implement_request_receiving (TpySvcCallStreamClass *klass, tpy_svc_call_stream_request_receiving_impl impl);
static inline
/* this comment is to stop gtkdoc realising this is static */
void tpy_svc_call_stream_return_from_request_receiving (DBusGMethodInvocation *context);
static inline void
tpy_svc_call_stream_return_from_request_receiving (DBusGMethodInvocation *context)
{
  dbus_g_method_return (context);
}

void tpy_svc_call_stream_emit_remote_members_changed (gpointer instance,
    GHashTable *arg_Updates,
    const GArray *arg_Removed);
void tpy_svc_call_stream_emit_local_sending_state_changed (gpointer instance,
    guint arg_State);

typedef struct _TpySvcCallStreamEndpoint TpySvcCallStreamEndpoint;

typedef struct _TpySvcCallStreamEndpointClass TpySvcCallStreamEndpointClass;

GType tpy_svc_call_stream_endpoint_get_type (void);
#define TPY_TYPE_SVC_CALL_STREAM_ENDPOINT \
  (tpy_svc_call_stream_endpoint_get_type ())
#define TPY_SVC_CALL_STREAM_ENDPOINT(obj) \
  (G_TYPE_CHECK_INSTANCE_CAST((obj), TPY_TYPE_SVC_CALL_STREAM_ENDPOINT, TpySvcCallStreamEndpoint))
#define TPY_IS_SVC_CALL_STREAM_ENDPOINT(obj) \
  (G_TYPE_CHECK_INSTANCE_TYPE((obj), TPY_TYPE_SVC_CALL_STREAM_ENDPOINT))
#define TPY_SVC_CALL_STREAM_ENDPOINT_GET_CLASS(obj) \
  (G_TYPE_INSTANCE_GET_INTERFACE((obj), TPY_TYPE_SVC_CALL_STREAM_ENDPOINT, TpySvcCallStreamEndpointClass))


typedef void (*tpy_svc_call_stream_endpoint_set_selected_candidate_impl) (TpySvcCallStreamEndpoint *self,
    const GValueArray *in_Candidate,
    DBusGMethodInvocation *context);
void tpy_svc_call_stream_endpoint_implement_set_selected_candidate (TpySvcCallStreamEndpointClass *klass, tpy_svc_call_stream_endpoint_set_selected_candidate_impl impl);
static inline
/* this comment is to stop gtkdoc realising this is static */
void tpy_svc_call_stream_endpoint_return_from_set_selected_candidate (DBusGMethodInvocation *context);
static inline void
tpy_svc_call_stream_endpoint_return_from_set_selected_candidate (DBusGMethodInvocation *context)
{
  dbus_g_method_return (context);
}

typedef void (*tpy_svc_call_stream_endpoint_set_stream_state_impl) (TpySvcCallStreamEndpoint *self,
    guint in_State,
    DBusGMethodInvocation *context);
void tpy_svc_call_stream_endpoint_implement_set_stream_state (TpySvcCallStreamEndpointClass *klass, tpy_svc_call_stream_endpoint_set_stream_state_impl impl);
static inline
/* this comment is to stop gtkdoc realising this is static */
void tpy_svc_call_stream_endpoint_return_from_set_stream_state (DBusGMethodInvocation *context);
static inline void
tpy_svc_call_stream_endpoint_return_from_set_stream_state (DBusGMethodInvocation *context)
{
  dbus_g_method_return (context);
}

void tpy_svc_call_stream_endpoint_emit_remote_credentials_set (gpointer instance,
    const gchar *arg_Username,
    const gchar *arg_Password);
void tpy_svc_call_stream_endpoint_emit_remote_candidates_added (gpointer instance,
    const GPtrArray *arg_Candidates);
void tpy_svc_call_stream_endpoint_emit_candidate_selected (gpointer instance,
    const GValueArray *arg_Candidate);
void tpy_svc_call_stream_endpoint_emit_stream_state_changed (gpointer instance,
    guint arg_state);

typedef struct _TpySvcCallStreamInterfaceMedia TpySvcCallStreamInterfaceMedia;

typedef struct _TpySvcCallStreamInterfaceMediaClass TpySvcCallStreamInterfaceMediaClass;

GType tpy_svc_call_stream_interface_media_get_type (void);
#define TPY_TYPE_SVC_CALL_STREAM_INTERFACE_MEDIA \
  (tpy_svc_call_stream_interface_media_get_type ())
#define TPY_SVC_CALL_STREAM_INTERFACE_MEDIA(obj) \
  (G_TYPE_CHECK_INSTANCE_CAST((obj), TPY_TYPE_SVC_CALL_STREAM_INTERFACE_MEDIA, TpySvcCallStreamInterfaceMedia))
#define TPY_IS_SVC_CALL_STREAM_INTERFACE_MEDIA(obj) \
  (G_TYPE_CHECK_INSTANCE_TYPE((obj), TPY_TYPE_SVC_CALL_STREAM_INTERFACE_MEDIA))
#define TPY_SVC_CALL_STREAM_INTERFACE_MEDIA_GET_CLASS(obj) \
  (G_TYPE_INSTANCE_GET_INTERFACE((obj), TPY_TYPE_SVC_CALL_STREAM_INTERFACE_MEDIA, TpySvcCallStreamInterfaceMediaClass))


typedef void (*tpy_svc_call_stream_interface_media_set_credentials_impl) (TpySvcCallStreamInterfaceMedia *self,
    const gchar *in_Username,
    const gchar *in_Password,
    DBusGMethodInvocation *context);
void tpy_svc_call_stream_interface_media_implement_set_credentials (TpySvcCallStreamInterfaceMediaClass *klass, tpy_svc_call_stream_interface_media_set_credentials_impl impl);
static inline
/* this comment is to stop gtkdoc realising this is static */
void tpy_svc_call_stream_interface_media_return_from_set_credentials (DBusGMethodInvocation *context);
static inline void
tpy_svc_call_stream_interface_media_return_from_set_credentials (DBusGMethodInvocation *context)
{
  dbus_g_method_return (context);
}

typedef void (*tpy_svc_call_stream_interface_media_add_candidates_impl) (TpySvcCallStreamInterfaceMedia *self,
    const GPtrArray *in_Candidates,
    DBusGMethodInvocation *context);
void tpy_svc_call_stream_interface_media_implement_add_candidates (TpySvcCallStreamInterfaceMediaClass *klass, tpy_svc_call_stream_interface_media_add_candidates_impl impl);
static inline
/* this comment is to stop gtkdoc realising this is static */
void tpy_svc_call_stream_interface_media_return_from_add_candidates (DBusGMethodInvocation *context);
static inline void
tpy_svc_call_stream_interface_media_return_from_add_candidates (DBusGMethodInvocation *context)
{
  dbus_g_method_return (context);
}

typedef void (*tpy_svc_call_stream_interface_media_candidates_prepared_impl) (TpySvcCallStreamInterfaceMedia *self,
    DBusGMethodInvocation *context);
void tpy_svc_call_stream_interface_media_implement_candidates_prepared (TpySvcCallStreamInterfaceMediaClass *klass, tpy_svc_call_stream_interface_media_candidates_prepared_impl impl);
static inline
/* this comment is to stop gtkdoc realising this is static */
void tpy_svc_call_stream_interface_media_return_from_candidates_prepared (DBusGMethodInvocation *context);
static inline void
tpy_svc_call_stream_interface_media_return_from_candidates_prepared (DBusGMethodInvocation *context)
{
  dbus_g_method_return (context);
}

void tpy_svc_call_stream_interface_media_emit_local_candidates_added (gpointer instance,
    const GPtrArray *arg_Candidates);
void tpy_svc_call_stream_interface_media_emit_local_credentials_changed (gpointer instance,
    const gchar *arg_Username,
    const gchar *arg_Password);
void tpy_svc_call_stream_interface_media_emit_relay_info_changed (gpointer instance,
    const GPtrArray *arg_Relay_Info);
void tpy_svc_call_stream_interface_media_emit_stun_servers_changed (gpointer instance,
    const GPtrArray *arg_Servers);
void tpy_svc_call_stream_interface_media_emit_server_info_retrieved (gpointer instance);
void tpy_svc_call_stream_interface_media_emit_endpoints_changed (gpointer instance,
    const GPtrArray *arg_Endpoints_Added,
    const GPtrArray *arg_Endpoints_Removed);
void tpy_svc_call_stream_interface_media_emit_please_restart_ice (gpointer instance);

typedef struct _TpySvcChannelTypeCall TpySvcChannelTypeCall;

typedef struct _TpySvcChannelTypeCallClass TpySvcChannelTypeCallClass;

GType tpy_svc_channel_type_call_get_type (void);
#define TPY_TYPE_SVC_CHANNEL_TYPE_CALL \
  (tpy_svc_channel_type_call_get_type ())
#define TPY_SVC_CHANNEL_TYPE_CALL(obj) \
  (G_TYPE_CHECK_INSTANCE_CAST((obj), TPY_TYPE_SVC_CHANNEL_TYPE_CALL, TpySvcChannelTypeCall))
#define TPY_IS_SVC_CHANNEL_TYPE_CALL(obj) \
  (G_TYPE_CHECK_INSTANCE_TYPE((obj), TPY_TYPE_SVC_CHANNEL_TYPE_CALL))
#define TPY_SVC_CHANNEL_TYPE_CALL_GET_CLASS(obj) \
  (G_TYPE_INSTANCE_GET_INTERFACE((obj), TPY_TYPE_SVC_CHANNEL_TYPE_CALL, TpySvcChannelTypeCallClass))


typedef void (*tpy_svc_channel_type_call_set_ringing_impl) (TpySvcChannelTypeCall *self,
    DBusGMethodInvocation *context);
void tpy_svc_channel_type_call_implement_set_ringing (TpySvcChannelTypeCallClass *klass, tpy_svc_channel_type_call_set_ringing_impl impl);
static inline
/* this comment is to stop gtkdoc realising this is static */
void tpy_svc_channel_type_call_return_from_set_ringing (DBusGMethodInvocation *context);
static inline void
tpy_svc_channel_type_call_return_from_set_ringing (DBusGMethodInvocation *context)
{
  dbus_g_method_return (context);
}

typedef void (*tpy_svc_channel_type_call_accept_impl) (TpySvcChannelTypeCall *self,
    DBusGMethodInvocation *context);
void tpy_svc_channel_type_call_implement_accept (TpySvcChannelTypeCallClass *klass, tpy_svc_channel_type_call_accept_impl impl);
static inline
/* this comment is to stop gtkdoc realising this is static */
void tpy_svc_channel_type_call_return_from_accept (DBusGMethodInvocation *context);
static inline void
tpy_svc_channel_type_call_return_from_accept (DBusGMethodInvocation *context)
{
  dbus_g_method_return (context);
}

typedef void (*tpy_svc_channel_type_call_hangup_impl) (TpySvcChannelTypeCall *self,
    guint in_Reason,
    const gchar *in_Detailed_Hangup_Reason,
    const gchar *in_Message,
    DBusGMethodInvocation *context);
void tpy_svc_channel_type_call_implement_hangup (TpySvcChannelTypeCallClass *klass, tpy_svc_channel_type_call_hangup_impl impl);
static inline
/* this comment is to stop gtkdoc realising this is static */
void tpy_svc_channel_type_call_return_from_hangup (DBusGMethodInvocation *context);
static inline void
tpy_svc_channel_type_call_return_from_hangup (DBusGMethodInvocation *context)
{
  dbus_g_method_return (context);
}

typedef void (*tpy_svc_channel_type_call_add_content_impl) (TpySvcChannelTypeCall *self,
    const gchar *in_Content_Name,
    guint in_Content_Type,
    DBusGMethodInvocation *context);
void tpy_svc_channel_type_call_implement_add_content (TpySvcChannelTypeCallClass *klass, tpy_svc_channel_type_call_add_content_impl impl);
static inline
/* this comment is to stop gtkdoc realising this is static */
void tpy_svc_channel_type_call_return_from_add_content (DBusGMethodInvocation *context,
    const gchar *out_Content);
static inline void
tpy_svc_channel_type_call_return_from_add_content (DBusGMethodInvocation *context,
    const gchar *out_Content)
{
  dbus_g_method_return (context,
      out_Content);
}

void tpy_svc_channel_type_call_emit_content_added (gpointer instance,
    const gchar *arg_Content);
void tpy_svc_channel_type_call_emit_content_removed (gpointer instance,
    const gchar *arg_Content);
void tpy_svc_channel_type_call_emit_call_state_changed (gpointer instance,
    guint arg_Call_State,
    guint arg_Call_Flags,
    const GValueArray *arg_Call_State_Reason,
    GHashTable *arg_Call_State_Details);
void tpy_svc_channel_type_call_emit_call_members_changed (gpointer instance,
    GHashTable *arg_Flags_Changed,
    const GArray *arg_Removed);


G_END_DECLS
