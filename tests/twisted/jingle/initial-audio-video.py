"""
Tests outgoing calls created with InitialAudio and/or InitialVideo.
"""

from servicetest import (
    assertContains, assertEquals, assertLength,
    wrap_channel, EventPattern, call_async, make_channel_proxy,
    )

from jingletest2 import JingleTest2, test_all_dialects

import constants as cs

def test(jp, q, bus, conn, stream):
    remote_jid = 'flames@cold.mountain/beyond'
    jt = JingleTest2(jp, conn, q, stream, 'test@localhost', remote_jid)
    jt.prepare()

    self_handle = conn.GetSelfHandle()
    remote_handle = conn.RequestHandles(cs.HT_CONTACT, [remote_jid])[0]

    rccs = conn.Properties.Get(cs.CONN_IFACE_REQUESTS, 'RequestableChannelClasses')
    cclass = ({ cs.CHANNEL_TYPE: cs.CHANNEL_TYPE_STREAMED_MEDIA,
                cs.TARGET_HANDLE_TYPE: cs.HT_CONTACT,
              },
              [ cs.TARGET_HANDLE, cs.TARGET_ID,
                cs.INITIAL_AUDIO, cs.INITIAL_VIDEO,
              ]
             )
    assertContains(cclass, rccs)

    check_neither(q, conn, bus, stream, remote_handle)
    check_iav(jt, q, conn, bus, stream, remote_handle, True, False)
    check_iav(jt, q, conn, bus, stream, remote_handle, False, True)
    check_iav(jt, q, conn, bus, stream, remote_handle, True, True)

def check_neither(q, conn, bus, stream, remote_handle):
    """
    Make a channel without specifying InitialAudio or InitialVideo; check
    that it's announced with both False, and that they're both present and
    false in GetAll().
    """

    path, props = conn.Requests.CreateChannel({
        cs.CHANNEL_TYPE: cs.CHANNEL_TYPE_STREAMED_MEDIA,
        cs.TARGET_HANDLE_TYPE: cs.HT_CONTACT,
        cs.TARGET_HANDLE: remote_handle})

    assertContains((cs.INITIAL_AUDIO, False), props.items())
    assertContains((cs.INITIAL_VIDEO, False), props.items())

    chan = wrap_channel(bus.get_object(conn.bus_name, path),
        cs.CHANNEL_TYPE_STREAMED_MEDIA, ['MediaSignalling'])
    props = chan.Properties.GetAll(cs.CHANNEL_TYPE_STREAMED_MEDIA + '.FUTURE')
    assertContains(('InitialAudio', False), props.items())
    assertContains(('InitialVideo', False), props.items())

    # We shouldn't have started a session yet, so there shouldn't be any
    # session handlers. Strictly speaking, there could be a session handler
    # with no stream handlers, but...
    session_handlers = chan.MediaSignalling.GetSessionHandlers()
    assertLength(0, session_handlers)

def check_iav(jt, q, conn, bus, stream, remote_handle, initial_audio,
              initial_video):
    """
    Make a channel and check that its InitialAudio and InitialVideo properties
    come out correctly.
    """

    call_async(q, conn.Requests, 'CreateChannel', {
        cs.CHANNEL_TYPE: cs.CHANNEL_TYPE_STREAMED_MEDIA,
        cs.TARGET_HANDLE_TYPE: cs.HT_CONTACT,
        cs.TARGET_HANDLE: remote_handle,
        cs.INITIAL_AUDIO: initial_audio,
        cs.INITIAL_VIDEO: initial_video,
        })
    if initial_video and jt.jp.is_gtalk():
        # You can't do video on ye olde GTalk.
        event = q.expect('dbus-error', method='CreateChannel')
        assertEquals(cs.NOT_CAPABLE, event.error.get_dbus_name())
    else:
        path, props = q.expect('dbus-return', method='CreateChannel').value

        assertContains((cs.INITIAL_AUDIO, initial_audio), props.items())
        assertContains((cs.INITIAL_VIDEO, initial_video), props.items())

        chan = wrap_channel(bus.get_object(conn.bus_name, path),
            cs.CHANNEL_TYPE_STREAMED_MEDIA, ['MediaSignalling'])
        props = chan.Properties.GetAll(cs.CHANNEL_TYPE_STREAMED_MEDIA + '.FUTURE')
        assertContains(('InitialAudio', initial_audio), props.items())
        assertContains(('InitialVideo', initial_video), props.items())

        session_handlers = chan.MediaSignalling.GetSessionHandlers()

        assertLength(1, session_handlers)
        path, type = session_handlers[0]
        assertEquals('rtp', type)
        session_handler = make_channel_proxy(conn, path, 'Media.SessionHandler')
        session_handler.Ready()

        stream_handler_paths = []
        stream_handler_types = []

        for x in [initial_audio, initial_video]:
            if x:
                e = q.expect('dbus-signal', signal='NewStreamHandler')
                stream_handler_paths.append(e.args[0])
                stream_handler_types.append(e.args[2])

        if initial_audio:
            assertContains(cs.MEDIA_STREAM_TYPE_AUDIO, stream_handler_types)

        if initial_video:
            assertContains(cs.MEDIA_STREAM_TYPE_VIDEO, stream_handler_types)

        for p in stream_handler_paths:
            sh = make_channel_proxy(conn, p, 'Media.StreamHandler')
            sh.NewNativeCandidate("fake", jt.get_remote_transports_dbus())
            # The codecs are wrong for video, but it's just an example. Gabble
            # doesn't care.
            sh.Ready(jt.get_audio_codecs_dbus())
            sh.StreamState(cs.MEDIA_STREAM_STATE_CONNECTED)

        e = q.expect('stream-iq', predicate=lambda e:
                jt.jp.match_jingle_action(e.query, 'session-initiate'))
        # TODO: check that the s-i contains the right contents.

        chan.Close()

if __name__ == '__main__':
    test_all_dialects(test)
