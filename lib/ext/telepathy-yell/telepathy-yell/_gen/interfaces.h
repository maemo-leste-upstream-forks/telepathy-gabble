/* Generated from: Yell extensions to the Telepathy spec



 */


#define TPY_IFACE_CALL_CONTENT \
"org.freedesktop.Telepathy.Call.Content.DRAFT"

#define TPY_IFACE_QUARK_CALL_CONTENT \
  (tpy_iface_quark_call_content ())

GQuark tpy_iface_quark_call_content (void);


#define TPY_PROP_CALL_CONTENT_INTERFACES \
"org.freedesktop.Telepathy.Call.Content.DRAFT.Interfaces"

#define TPY_PROP_CALL_CONTENT_NAME \
"org.freedesktop.Telepathy.Call.Content.DRAFT.Name"

#define TPY_PROP_CALL_CONTENT_TYPE \
"org.freedesktop.Telepathy.Call.Content.DRAFT.Type"

#define TPY_PROP_CALL_CONTENT_DISPOSITION \
"org.freedesktop.Telepathy.Call.Content.DRAFT.Disposition"

#define TPY_PROP_CALL_CONTENT_STREAMS \
"org.freedesktop.Telepathy.Call.Content.DRAFT.Streams"

#define TPY_IFACE_CALL_CONTENT_CODEC_OFFER \
"org.freedesktop.Telepathy.Call.Content.CodecOffer.DRAFT"

#define TPY_IFACE_QUARK_CALL_CONTENT_CODEC_OFFER \
  (tpy_iface_quark_call_content_codec_offer ())

GQuark tpy_iface_quark_call_content_codec_offer (void);


#define TPY_PROP_CALL_CONTENT_CODEC_OFFER_INTERFACES \
"org.freedesktop.Telepathy.Call.Content.CodecOffer.DRAFT.Interfaces"

#define TPY_PROP_CALL_CONTENT_CODEC_OFFER_REMOTE_CONTACT_CODECS \
"org.freedesktop.Telepathy.Call.Content.CodecOffer.DRAFT.RemoteContactCodecs"

#define TPY_PROP_CALL_CONTENT_CODEC_OFFER_REMOTE_CONTACT \
"org.freedesktop.Telepathy.Call.Content.CodecOffer.DRAFT.RemoteContact"

#define TPY_IFACE_CALL_CONTENT_INTERFACE_MEDIA \
"org.freedesktop.Telepathy.Call.Content.Interface.Media.DRAFT"

#define TPY_IFACE_QUARK_CALL_CONTENT_INTERFACE_MEDIA \
  (tpy_iface_quark_call_content_interface_media ())

GQuark tpy_iface_quark_call_content_interface_media (void);


#define TPY_PROP_CALL_CONTENT_INTERFACE_MEDIA_CONTACT_CODEC_MAP \
"org.freedesktop.Telepathy.Call.Content.Interface.Media.DRAFT.ContactCodecMap"

#define TPY_PROP_CALL_CONTENT_INTERFACE_MEDIA_CODEC_OFFER \
"org.freedesktop.Telepathy.Call.Content.Interface.Media.DRAFT.CodecOffer"

#define TPY_PROP_CALL_CONTENT_INTERFACE_MEDIA_PACKETIZATION \
"org.freedesktop.Telepathy.Call.Content.Interface.Media.DRAFT.Packetization"

#define TPY_IFACE_CALL_CONTENT_INTERFACE_MUTE \
"org.freedesktop.Telepathy.Call.Content.Interface.Mute.DRAFT"

#define TPY_IFACE_QUARK_CALL_CONTENT_INTERFACE_MUTE \
  (tpy_iface_quark_call_content_interface_mute ())

GQuark tpy_iface_quark_call_content_interface_mute (void);


#define TPY_PROP_CALL_CONTENT_INTERFACE_MUTE_MUTE_STATE \
"org.freedesktop.Telepathy.Call.Content.Interface.Mute.DRAFT.MuteState"

#define TPY_IFACE_CALL_STREAM \
"org.freedesktop.Telepathy.Call.Stream.DRAFT"

#define TPY_IFACE_QUARK_CALL_STREAM \
  (tpy_iface_quark_call_stream ())

GQuark tpy_iface_quark_call_stream (void);


#define TPY_PROP_CALL_STREAM_INTERFACES \
"org.freedesktop.Telepathy.Call.Stream.DRAFT.Interfaces"

#define TPY_PROP_CALL_STREAM_REMOTE_MEMBERS \
"org.freedesktop.Telepathy.Call.Stream.DRAFT.RemoteMembers"

#define TPY_PROP_CALL_STREAM_LOCAL_SENDING_STATE \
"org.freedesktop.Telepathy.Call.Stream.DRAFT.LocalSendingState"

#define TPY_PROP_CALL_STREAM_CAN_REQUEST_RECEIVING \
"org.freedesktop.Telepathy.Call.Stream.DRAFT.CanRequestReceiving"

#define TPY_IFACE_CALL_STREAM_ENDPOINT \
"org.freedesktop.Telepathy.Call.Stream.Endpoint.DRAFT"

#define TPY_IFACE_QUARK_CALL_STREAM_ENDPOINT \
  (tpy_iface_quark_call_stream_endpoint ())

GQuark tpy_iface_quark_call_stream_endpoint (void);


#define TPY_PROP_CALL_STREAM_ENDPOINT_REMOTE_CREDENTIALS \
"org.freedesktop.Telepathy.Call.Stream.Endpoint.DRAFT.RemoteCredentials"

#define TPY_PROP_CALL_STREAM_ENDPOINT_REMOTE_CANDIDATES \
"org.freedesktop.Telepathy.Call.Stream.Endpoint.DRAFT.RemoteCandidates"

#define TPY_PROP_CALL_STREAM_ENDPOINT_SELECTED_CANDIDATE \
"org.freedesktop.Telepathy.Call.Stream.Endpoint.DRAFT.SelectedCandidate"

#define TPY_PROP_CALL_STREAM_ENDPOINT_STREAM_STATE \
"org.freedesktop.Telepathy.Call.Stream.Endpoint.DRAFT.StreamState"

#define TPY_PROP_CALL_STREAM_ENDPOINT_TRANSPORT \
"org.freedesktop.Telepathy.Call.Stream.Endpoint.DRAFT.Transport"

#define TPY_IFACE_CALL_STREAM_INTERFACE_MEDIA \
"org.freedesktop.Telepathy.Call.Stream.Interface.Media.DRAFT"

#define TPY_IFACE_QUARK_CALL_STREAM_INTERFACE_MEDIA \
  (tpy_iface_quark_call_stream_interface_media ())

GQuark tpy_iface_quark_call_stream_interface_media (void);


#define TPY_PROP_CALL_STREAM_INTERFACE_MEDIA_TRANSPORT \
"org.freedesktop.Telepathy.Call.Stream.Interface.Media.DRAFT.Transport"

#define TPY_PROP_CALL_STREAM_INTERFACE_MEDIA_LOCAL_CANDIDATES \
"org.freedesktop.Telepathy.Call.Stream.Interface.Media.DRAFT.LocalCandidates"

#define TPY_PROP_CALL_STREAM_INTERFACE_MEDIA_LOCAL_CREDENTIALS \
"org.freedesktop.Telepathy.Call.Stream.Interface.Media.DRAFT.LocalCredentials"

#define TPY_PROP_CALL_STREAM_INTERFACE_MEDIA_STUN_SERVERS \
"org.freedesktop.Telepathy.Call.Stream.Interface.Media.DRAFT.STUNServers"

#define TPY_PROP_CALL_STREAM_INTERFACE_MEDIA_RELAY_INFO \
"org.freedesktop.Telepathy.Call.Stream.Interface.Media.DRAFT.RelayInfo"

#define TPY_PROP_CALL_STREAM_INTERFACE_MEDIA_HAS_SERVER_INFO \
"org.freedesktop.Telepathy.Call.Stream.Interface.Media.DRAFT.HasServerInfo"

#define TPY_PROP_CALL_STREAM_INTERFACE_MEDIA_ENDPOINTS \
"org.freedesktop.Telepathy.Call.Stream.Interface.Media.DRAFT.Endpoints"

#define TPY_IFACE_CHANNEL_TYPE_CALL \
"org.freedesktop.Telepathy.Channel.Type.Call.DRAFT"

#define TPY_IFACE_QUARK_CHANNEL_TYPE_CALL \
  (tpy_iface_quark_channel_type_call ())

GQuark tpy_iface_quark_channel_type_call (void);


#define TPY_PROP_CHANNEL_TYPE_CALL_CONTENTS \
"org.freedesktop.Telepathy.Channel.Type.Call.DRAFT.Contents"

#define TPY_PROP_CHANNEL_TYPE_CALL_CALL_STATE_DETAILS \
"org.freedesktop.Telepathy.Channel.Type.Call.DRAFT.CallStateDetails"

#define TPY_PROP_CHANNEL_TYPE_CALL_CALL_STATE \
"org.freedesktop.Telepathy.Channel.Type.Call.DRAFT.CallState"

#define TPY_PROP_CHANNEL_TYPE_CALL_CALL_FLAGS \
"org.freedesktop.Telepathy.Channel.Type.Call.DRAFT.CallFlags"

#define TPY_PROP_CHANNEL_TYPE_CALL_CALL_STATE_REASON \
"org.freedesktop.Telepathy.Channel.Type.Call.DRAFT.CallStateReason"

#define TPY_PROP_CHANNEL_TYPE_CALL_HARDWARE_STREAMING \
"org.freedesktop.Telepathy.Channel.Type.Call.DRAFT.HardwareStreaming"

#define TPY_PROP_CHANNEL_TYPE_CALL_CALL_MEMBERS \
"org.freedesktop.Telepathy.Channel.Type.Call.DRAFT.CallMembers"

#define TPY_PROP_CHANNEL_TYPE_CALL_INITIAL_TRANSPORT \
"org.freedesktop.Telepathy.Channel.Type.Call.DRAFT.InitialTransport"

#define TPY_PROP_CHANNEL_TYPE_CALL_INITIAL_AUDIO \
"org.freedesktop.Telepathy.Channel.Type.Call.DRAFT.InitialAudio"

#define TPY_PROP_CHANNEL_TYPE_CALL_INITIAL_VIDEO \
"org.freedesktop.Telepathy.Channel.Type.Call.DRAFT.InitialVideo"

#define TPY_PROP_CHANNEL_TYPE_CALL_INITIAL_AUDIO_NAME \
"org.freedesktop.Telepathy.Channel.Type.Call.DRAFT.InitialAudioName"

#define TPY_PROP_CHANNEL_TYPE_CALL_INITIAL_VIDEO_NAME \
"org.freedesktop.Telepathy.Channel.Type.Call.DRAFT.InitialVideoName"

#define TPY_PROP_CHANNEL_TYPE_CALL_MUTABLE_CONTENTS \
"org.freedesktop.Telepathy.Channel.Type.Call.DRAFT.MutableContents"

#define TPY_TOKEN_CHANNEL_TYPE_CALL_AUDIO \
"org.freedesktop.Telepathy.Channel.Type.Call.DRAFT/audio"

#define TPY_TOKEN_CHANNEL_TYPE_CALL_VIDEO \
"org.freedesktop.Telepathy.Channel.Type.Call.DRAFT/video"

#define TPY_TOKEN_CHANNEL_TYPE_CALL_GTALK_P2P \
"org.freedesktop.Telepathy.Channel.Type.Call.DRAFT/gtalk-p2p"

#define TPY_TOKEN_CHANNEL_TYPE_CALL_ICE \
"org.freedesktop.Telepathy.Channel.Type.Call.DRAFT/ice"

#define TPY_TOKEN_CHANNEL_TYPE_CALL_WLM_2009 \
"org.freedesktop.Telepathy.Channel.Type.Call.DRAFT/wlm-2009"

#define TPY_TOKEN_CHANNEL_TYPE_CALL_SHM \
"org.freedesktop.Telepathy.Channel.Type.Call.DRAFT/shm"
