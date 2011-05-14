G_BEGIN_DECLS

typedef void (*tpy_cli_call_content_signal_callback_removed) (TpProxy *proxy,
    gpointer user_data, GObject *weak_object);
TpProxySignalConnection *tpy_cli_call_content_connect_to_removed (TpProxy *proxy,
    tpy_cli_call_content_signal_callback_removed callback,
    gpointer user_data,
    GDestroyNotify destroy,
    GObject *weak_object,
    GError **error);

typedef void (*tpy_cli_call_content_signal_callback_streams_added) (TpProxy *proxy,
    const GPtrArray *arg_Streams,
    gpointer user_data, GObject *weak_object);
TpProxySignalConnection *tpy_cli_call_content_connect_to_streams_added (TpProxy *proxy,
    tpy_cli_call_content_signal_callback_streams_added callback,
    gpointer user_data,
    GDestroyNotify destroy,
    GObject *weak_object,
    GError **error);

typedef void (*tpy_cli_call_content_signal_callback_streams_removed) (TpProxy *proxy,
    const GPtrArray *arg_Streams,
    gpointer user_data, GObject *weak_object);
TpProxySignalConnection *tpy_cli_call_content_connect_to_streams_removed (TpProxy *proxy,
    tpy_cli_call_content_signal_callback_streams_removed callback,
    gpointer user_data,
    GDestroyNotify destroy,
    GObject *weak_object,
    GError **error);

typedef void (*tpy_cli_call_content_callback_for_remove) (TpProxy *proxy,
    const GError *error, gpointer user_data,
    GObject *weak_object);

TpProxyPendingCall *tpy_cli_call_content_call_remove (TpProxy *proxy,
    gint timeout_ms,
    guint in_Reason,
    const gchar *in_Detailed_Removal_Reason,
    const gchar *in_Message,
    tpy_cli_call_content_callback_for_remove callback,
    gpointer user_data,
    GDestroyNotify destroy,
    GObject *weak_object);


typedef void (*tpy_cli_call_content_codec_offer_callback_for_accept) (TpProxy *proxy,
    const GError *error, gpointer user_data,
    GObject *weak_object);

TpProxyPendingCall *tpy_cli_call_content_codec_offer_call_accept (TpProxy *proxy,
    gint timeout_ms,
    const GPtrArray *in_Codecs,
    tpy_cli_call_content_codec_offer_callback_for_accept callback,
    gpointer user_data,
    GDestroyNotify destroy,
    GObject *weak_object);


typedef void (*tpy_cli_call_content_codec_offer_callback_for_reject) (TpProxy *proxy,
    const GError *error, gpointer user_data,
    GObject *weak_object);

TpProxyPendingCall *tpy_cli_call_content_codec_offer_call_reject (TpProxy *proxy,
    gint timeout_ms,
    tpy_cli_call_content_codec_offer_callback_for_reject callback,
    gpointer user_data,
    GDestroyNotify destroy,
    GObject *weak_object);


typedef void (*tpy_cli_call_content_interface_media_signal_callback_codecs_changed) (TpProxy *proxy,
    GHashTable *arg_Updated_Codecs,
    const GArray *arg_Removed_Contacts,
    gpointer user_data, GObject *weak_object);
TpProxySignalConnection *tpy_cli_call_content_interface_media_connect_to_codecs_changed (TpProxy *proxy,
    tpy_cli_call_content_interface_media_signal_callback_codecs_changed callback,
    gpointer user_data,
    GDestroyNotify destroy,
    GObject *weak_object,
    GError **error);

typedef void (*tpy_cli_call_content_interface_media_signal_callback_new_codec_offer) (TpProxy *proxy,
    guint arg_Contact,
    const gchar *arg_Offer,
    const GPtrArray *arg_Codecs,
    gpointer user_data, GObject *weak_object);
TpProxySignalConnection *tpy_cli_call_content_interface_media_connect_to_new_codec_offer (TpProxy *proxy,
    tpy_cli_call_content_interface_media_signal_callback_new_codec_offer callback,
    gpointer user_data,
    GDestroyNotify destroy,
    GObject *weak_object,
    GError **error);

typedef void (*tpy_cli_call_content_interface_media_callback_for_update_codecs) (TpProxy *proxy,
    const GError *error, gpointer user_data,
    GObject *weak_object);

TpProxyPendingCall *tpy_cli_call_content_interface_media_call_update_codecs (TpProxy *proxy,
    gint timeout_ms,
    const GPtrArray *in_Codecs,
    tpy_cli_call_content_interface_media_callback_for_update_codecs callback,
    gpointer user_data,
    GDestroyNotify destroy,
    GObject *weak_object);


typedef void (*tpy_cli_call_content_interface_mute_signal_callback_mute_state_changed) (TpProxy *proxy,
    gboolean arg_MuteState,
    gpointer user_data, GObject *weak_object);
TpProxySignalConnection *tpy_cli_call_content_interface_mute_connect_to_mute_state_changed (TpProxy *proxy,
    tpy_cli_call_content_interface_mute_signal_callback_mute_state_changed callback,
    gpointer user_data,
    GDestroyNotify destroy,
    GObject *weak_object,
    GError **error);

typedef void (*tpy_cli_call_content_interface_mute_callback_for_set_muted) (TpProxy *proxy,
    const GError *error, gpointer user_data,
    GObject *weak_object);

TpProxyPendingCall *tpy_cli_call_content_interface_mute_call_set_muted (TpProxy *proxy,
    gint timeout_ms,
    gboolean in_Muted,
    tpy_cli_call_content_interface_mute_callback_for_set_muted callback,
    gpointer user_data,
    GDestroyNotify destroy,
    GObject *weak_object);


typedef void (*tpy_cli_call_stream_signal_callback_remote_members_changed) (TpProxy *proxy,
    GHashTable *arg_Updates,
    const GArray *arg_Removed,
    gpointer user_data, GObject *weak_object);
TpProxySignalConnection *tpy_cli_call_stream_connect_to_remote_members_changed (TpProxy *proxy,
    tpy_cli_call_stream_signal_callback_remote_members_changed callback,
    gpointer user_data,
    GDestroyNotify destroy,
    GObject *weak_object,
    GError **error);

typedef void (*tpy_cli_call_stream_signal_callback_local_sending_state_changed) (TpProxy *proxy,
    guint arg_State,
    gpointer user_data, GObject *weak_object);
TpProxySignalConnection *tpy_cli_call_stream_connect_to_local_sending_state_changed (TpProxy *proxy,
    tpy_cli_call_stream_signal_callback_local_sending_state_changed callback,
    gpointer user_data,
    GDestroyNotify destroy,
    GObject *weak_object,
    GError **error);

typedef void (*tpy_cli_call_stream_callback_for_set_sending) (TpProxy *proxy,
    const GError *error, gpointer user_data,
    GObject *weak_object);

TpProxyPendingCall *tpy_cli_call_stream_call_set_sending (TpProxy *proxy,
    gint timeout_ms,
    gboolean in_Send,
    tpy_cli_call_stream_callback_for_set_sending callback,
    gpointer user_data,
    GDestroyNotify destroy,
    GObject *weak_object);


typedef void (*tpy_cli_call_stream_callback_for_request_receiving) (TpProxy *proxy,
    const GError *error, gpointer user_data,
    GObject *weak_object);

TpProxyPendingCall *tpy_cli_call_stream_call_request_receiving (TpProxy *proxy,
    gint timeout_ms,
    guint in_Contact,
    gboolean in_Receive,
    tpy_cli_call_stream_callback_for_request_receiving callback,
    gpointer user_data,
    GDestroyNotify destroy,
    GObject *weak_object);


typedef void (*tpy_cli_call_stream_endpoint_signal_callback_remote_credentials_set) (TpProxy *proxy,
    const gchar *arg_Username,
    const gchar *arg_Password,
    gpointer user_data, GObject *weak_object);
TpProxySignalConnection *tpy_cli_call_stream_endpoint_connect_to_remote_credentials_set (TpProxy *proxy,
    tpy_cli_call_stream_endpoint_signal_callback_remote_credentials_set callback,
    gpointer user_data,
    GDestroyNotify destroy,
    GObject *weak_object,
    GError **error);

typedef void (*tpy_cli_call_stream_endpoint_signal_callback_remote_candidates_added) (TpProxy *proxy,
    const GPtrArray *arg_Candidates,
    gpointer user_data, GObject *weak_object);
TpProxySignalConnection *tpy_cli_call_stream_endpoint_connect_to_remote_candidates_added (TpProxy *proxy,
    tpy_cli_call_stream_endpoint_signal_callback_remote_candidates_added callback,
    gpointer user_data,
    GDestroyNotify destroy,
    GObject *weak_object,
    GError **error);

typedef void (*tpy_cli_call_stream_endpoint_signal_callback_candidate_selected) (TpProxy *proxy,
    const GValueArray *arg_Candidate,
    gpointer user_data, GObject *weak_object);
TpProxySignalConnection *tpy_cli_call_stream_endpoint_connect_to_candidate_selected (TpProxy *proxy,
    tpy_cli_call_stream_endpoint_signal_callback_candidate_selected callback,
    gpointer user_data,
    GDestroyNotify destroy,
    GObject *weak_object,
    GError **error);

typedef void (*tpy_cli_call_stream_endpoint_signal_callback_stream_state_changed) (TpProxy *proxy,
    guint arg_state,
    gpointer user_data, GObject *weak_object);
TpProxySignalConnection *tpy_cli_call_stream_endpoint_connect_to_stream_state_changed (TpProxy *proxy,
    tpy_cli_call_stream_endpoint_signal_callback_stream_state_changed callback,
    gpointer user_data,
    GDestroyNotify destroy,
    GObject *weak_object,
    GError **error);

typedef void (*tpy_cli_call_stream_endpoint_callback_for_set_selected_candidate) (TpProxy *proxy,
    const GError *error, gpointer user_data,
    GObject *weak_object);

TpProxyPendingCall *tpy_cli_call_stream_endpoint_call_set_selected_candidate (TpProxy *proxy,
    gint timeout_ms,
    const GValueArray *in_Candidate,
    tpy_cli_call_stream_endpoint_callback_for_set_selected_candidate callback,
    gpointer user_data,
    GDestroyNotify destroy,
    GObject *weak_object);


typedef void (*tpy_cli_call_stream_endpoint_callback_for_set_stream_state) (TpProxy *proxy,
    const GError *error, gpointer user_data,
    GObject *weak_object);

TpProxyPendingCall *tpy_cli_call_stream_endpoint_call_set_stream_state (TpProxy *proxy,
    gint timeout_ms,
    guint in_State,
    tpy_cli_call_stream_endpoint_callback_for_set_stream_state callback,
    gpointer user_data,
    GDestroyNotify destroy,
    GObject *weak_object);


typedef void (*tpy_cli_call_stream_interface_media_signal_callback_local_candidates_added) (TpProxy *proxy,
    const GPtrArray *arg_Candidates,
    gpointer user_data, GObject *weak_object);
TpProxySignalConnection *tpy_cli_call_stream_interface_media_connect_to_local_candidates_added (TpProxy *proxy,
    tpy_cli_call_stream_interface_media_signal_callback_local_candidates_added callback,
    gpointer user_data,
    GDestroyNotify destroy,
    GObject *weak_object,
    GError **error);

typedef void (*tpy_cli_call_stream_interface_media_signal_callback_local_credentials_changed) (TpProxy *proxy,
    const gchar *arg_Username,
    const gchar *arg_Password,
    gpointer user_data, GObject *weak_object);
TpProxySignalConnection *tpy_cli_call_stream_interface_media_connect_to_local_credentials_changed (TpProxy *proxy,
    tpy_cli_call_stream_interface_media_signal_callback_local_credentials_changed callback,
    gpointer user_data,
    GDestroyNotify destroy,
    GObject *weak_object,
    GError **error);

typedef void (*tpy_cli_call_stream_interface_media_signal_callback_relay_info_changed) (TpProxy *proxy,
    const GPtrArray *arg_Relay_Info,
    gpointer user_data, GObject *weak_object);
TpProxySignalConnection *tpy_cli_call_stream_interface_media_connect_to_relay_info_changed (TpProxy *proxy,
    tpy_cli_call_stream_interface_media_signal_callback_relay_info_changed callback,
    gpointer user_data,
    GDestroyNotify destroy,
    GObject *weak_object,
    GError **error);

typedef void (*tpy_cli_call_stream_interface_media_signal_callback_stun_servers_changed) (TpProxy *proxy,
    const GPtrArray *arg_Servers,
    gpointer user_data, GObject *weak_object);
TpProxySignalConnection *tpy_cli_call_stream_interface_media_connect_to_stun_servers_changed (TpProxy *proxy,
    tpy_cli_call_stream_interface_media_signal_callback_stun_servers_changed callback,
    gpointer user_data,
    GDestroyNotify destroy,
    GObject *weak_object,
    GError **error);

typedef void (*tpy_cli_call_stream_interface_media_signal_callback_server_info_retrieved) (TpProxy *proxy,
    gpointer user_data, GObject *weak_object);
TpProxySignalConnection *tpy_cli_call_stream_interface_media_connect_to_server_info_retrieved (TpProxy *proxy,
    tpy_cli_call_stream_interface_media_signal_callback_server_info_retrieved callback,
    gpointer user_data,
    GDestroyNotify destroy,
    GObject *weak_object,
    GError **error);

typedef void (*tpy_cli_call_stream_interface_media_signal_callback_endpoints_changed) (TpProxy *proxy,
    const GPtrArray *arg_Endpoints_Added,
    const GPtrArray *arg_Endpoints_Removed,
    gpointer user_data, GObject *weak_object);
TpProxySignalConnection *tpy_cli_call_stream_interface_media_connect_to_endpoints_changed (TpProxy *proxy,
    tpy_cli_call_stream_interface_media_signal_callback_endpoints_changed callback,
    gpointer user_data,
    GDestroyNotify destroy,
    GObject *weak_object,
    GError **error);

typedef void (*tpy_cli_call_stream_interface_media_signal_callback_please_restart_ice) (TpProxy *proxy,
    gpointer user_data, GObject *weak_object);
TpProxySignalConnection *tpy_cli_call_stream_interface_media_connect_to_please_restart_ice (TpProxy *proxy,
    tpy_cli_call_stream_interface_media_signal_callback_please_restart_ice callback,
    gpointer user_data,
    GDestroyNotify destroy,
    GObject *weak_object,
    GError **error);

typedef void (*tpy_cli_call_stream_interface_media_callback_for_set_credentials) (TpProxy *proxy,
    const GError *error, gpointer user_data,
    GObject *weak_object);

TpProxyPendingCall *tpy_cli_call_stream_interface_media_call_set_credentials (TpProxy *proxy,
    gint timeout_ms,
    const gchar *in_Username,
    const gchar *in_Password,
    tpy_cli_call_stream_interface_media_callback_for_set_credentials callback,
    gpointer user_data,
    GDestroyNotify destroy,
    GObject *weak_object);


typedef void (*tpy_cli_call_stream_interface_media_callback_for_add_candidates) (TpProxy *proxy,
    const GError *error, gpointer user_data,
    GObject *weak_object);

TpProxyPendingCall *tpy_cli_call_stream_interface_media_call_add_candidates (TpProxy *proxy,
    gint timeout_ms,
    const GPtrArray *in_Candidates,
    tpy_cli_call_stream_interface_media_callback_for_add_candidates callback,
    gpointer user_data,
    GDestroyNotify destroy,
    GObject *weak_object);


typedef void (*tpy_cli_call_stream_interface_media_callback_for_candidates_prepared) (TpProxy *proxy,
    const GError *error, gpointer user_data,
    GObject *weak_object);

TpProxyPendingCall *tpy_cli_call_stream_interface_media_call_candidates_prepared (TpProxy *proxy,
    gint timeout_ms,
    tpy_cli_call_stream_interface_media_callback_for_candidates_prepared callback,
    gpointer user_data,
    GDestroyNotify destroy,
    GObject *weak_object);


typedef void (*tpy_cli_channel_type_call_signal_callback_content_added) (TpProxy *proxy,
    const gchar *arg_Content,
    gpointer user_data, GObject *weak_object);
TpProxySignalConnection *tpy_cli_channel_type_call_connect_to_content_added (TpProxy *proxy,
    tpy_cli_channel_type_call_signal_callback_content_added callback,
    gpointer user_data,
    GDestroyNotify destroy,
    GObject *weak_object,
    GError **error);

typedef void (*tpy_cli_channel_type_call_signal_callback_content_removed) (TpProxy *proxy,
    const gchar *arg_Content,
    gpointer user_data, GObject *weak_object);
TpProxySignalConnection *tpy_cli_channel_type_call_connect_to_content_removed (TpProxy *proxy,
    tpy_cli_channel_type_call_signal_callback_content_removed callback,
    gpointer user_data,
    GDestroyNotify destroy,
    GObject *weak_object,
    GError **error);

typedef void (*tpy_cli_channel_type_call_signal_callback_call_state_changed) (TpProxy *proxy,
    guint arg_Call_State,
    guint arg_Call_Flags,
    const GValueArray *arg_Call_State_Reason,
    GHashTable *arg_Call_State_Details,
    gpointer user_data, GObject *weak_object);
TpProxySignalConnection *tpy_cli_channel_type_call_connect_to_call_state_changed (TpProxy *proxy,
    tpy_cli_channel_type_call_signal_callback_call_state_changed callback,
    gpointer user_data,
    GDestroyNotify destroy,
    GObject *weak_object,
    GError **error);

typedef void (*tpy_cli_channel_type_call_signal_callback_call_members_changed) (TpProxy *proxy,
    GHashTable *arg_Flags_Changed,
    const GArray *arg_Removed,
    gpointer user_data, GObject *weak_object);
TpProxySignalConnection *tpy_cli_channel_type_call_connect_to_call_members_changed (TpProxy *proxy,
    tpy_cli_channel_type_call_signal_callback_call_members_changed callback,
    gpointer user_data,
    GDestroyNotify destroy,
    GObject *weak_object,
    GError **error);

typedef void (*tpy_cli_channel_type_call_callback_for_set_ringing) (TpProxy *proxy,
    const GError *error, gpointer user_data,
    GObject *weak_object);

TpProxyPendingCall *tpy_cli_channel_type_call_call_set_ringing (TpProxy *proxy,
    gint timeout_ms,
    tpy_cli_channel_type_call_callback_for_set_ringing callback,
    gpointer user_data,
    GDestroyNotify destroy,
    GObject *weak_object);


typedef void (*tpy_cli_channel_type_call_callback_for_accept) (TpProxy *proxy,
    const GError *error, gpointer user_data,
    GObject *weak_object);

TpProxyPendingCall *tpy_cli_channel_type_call_call_accept (TpProxy *proxy,
    gint timeout_ms,
    tpy_cli_channel_type_call_callback_for_accept callback,
    gpointer user_data,
    GDestroyNotify destroy,
    GObject *weak_object);


typedef void (*tpy_cli_channel_type_call_callback_for_hangup) (TpProxy *proxy,
    const GError *error, gpointer user_data,
    GObject *weak_object);

TpProxyPendingCall *tpy_cli_channel_type_call_call_hangup (TpProxy *proxy,
    gint timeout_ms,
    guint in_Reason,
    const gchar *in_Detailed_Hangup_Reason,
    const gchar *in_Message,
    tpy_cli_channel_type_call_callback_for_hangup callback,
    gpointer user_data,
    GDestroyNotify destroy,
    GObject *weak_object);


typedef void (*tpy_cli_channel_type_call_callback_for_add_content) (TpProxy *proxy,
    const gchar *out_Content,
    const GError *error, gpointer user_data,
    GObject *weak_object);

TpProxyPendingCall *tpy_cli_channel_type_call_call_add_content (TpProxy *proxy,
    gint timeout_ms,
    const gchar *in_Content_Name,
    guint in_Content_Type,
    tpy_cli_channel_type_call_callback_for_add_content callback,
    gpointer user_data,
    GDestroyNotify destroy,
    GObject *weak_object);


G_END_DECLS
