"""
Test content adding and removal during the session. We start
session with only one stream, then add one more, then remove
the first one and lastly remove the second stream, which
closes the session.
"""

from gabbletest import exec_test, make_result_iq, sync_stream, \
        send_error_reply
from servicetest import make_channel_proxy, unwrap, tp_path_prefix, \
        call_async, EventPattern
from twisted.words.xish import domish, xpath
import jingletest
import gabbletest
import dbus
import time
from constants import *


def test(q, bus, conn, stream):
    jt = jingletest.JingleTest(stream, 'test@localhost', 'foo@bar.com/Foo')

    # Connecting
    conn.Connect()

    q.expect('dbus-signal', signal='StatusChanged', args=[1, 1])

    q.expect('stream-authenticated')
    q.expect('dbus-signal', signal='PresenceUpdate',
        args=[{1L: (0L, {u'available': {}})}])
    q.expect('dbus-signal', signal='StatusChanged', args=[0, 1])

    self_handle = conn.GetSelfHandle()

    # We need remote end's presence for capabilities
    jt.send_remote_presence()

    # Gabble doesn't trust it, so makes a disco
    event = q.expect('stream-iq', query_ns='http://jabber.org/protocol/disco#info',
             to='foo@bar.com/Foo')

    jt.send_remote_disco_reply(event.stanza)

    # Force Gabble to process the caps before calling RequestChannel
    sync_stream(q, stream)

    handle = conn.RequestHandles(1, [jt.remote_jid])[0]

    path = conn.RequestChannel(
        'org.freedesktop.Telepathy.Channel.Type.StreamedMedia',
        1, handle, True)

    signalling_iface = make_channel_proxy(conn, path, 'Channel.Interface.MediaSignalling')
    media_iface = make_channel_proxy(conn, path, 'Channel.Type.StreamedMedia')
    group_iface = make_channel_proxy(conn, path, 'Channel.Interface.Group')

    # This is the interesting part of this test

    media_iface.RequestStreams(handle, [MEDIA_STREAM_TYPE_AUDIO])

    # S-E gets notified about new session handler, and calls Ready on it
    e = q.expect('dbus-signal', signal='NewSessionHandler')
    assert e.args[1] == 'rtp'

    session_handler = make_channel_proxy(conn, e.args[0], 'Media.SessionHandler')
    session_handler.Ready()

    e = q.expect('dbus-signal', signal='NewStreamHandler')
    stream_id = e.args[1]

    stream_handler = make_channel_proxy(conn, e.args[0], 'Media.StreamHandler')

    stream_handler.NewNativeCandidate("fake", jt.get_remote_transports_dbus())

    # Before sending the initiate, request another stream

    media_iface.RequestStreams(handle, [MEDIA_STREAM_TYPE_VIDEO])

    e = q.expect('dbus-signal', signal='NewStreamHandler')
    stream_id2 = e.args[1]

    stream_handler2 = make_channel_proxy(conn, e.args[0], 'Media.StreamHandler')

    # We set both streams as ready, which will trigger the session invite
    stream_handler.Ready(jt.get_audio_codecs_dbus())
    stream_handler.StreamState(MEDIA_STREAM_STATE_CONNECTED)
    stream_handler2.Ready(jt.get_audio_codecs_dbus())
    stream_handler2.StreamState(MEDIA_STREAM_STATE_CONNECTED)

    # We changed our mind locally, don't want video
    media_iface.RemoveStreams([stream_id2])

    e = q.expect('stream-iq')
    assert e.query.name == 'jingle'
    assert e.query['action'] == 'session-initiate'
    stream.send(gabbletest.make_result_iq(stream, e.stanza))

    e2 = q.expect('stream-iq', predicate=lambda x:
        xpath.queryForNodes("/iq/jingle[@action='content-remove']",
            x.stanza))

    jt.outgoing_call_reply(e.query['sid'], True, with_video=True)
    q.expect('stream-iq', iq_type='result')

    # Only now the remote end removes the video stream; if gabble mistakenly
    # marked it as accepted on session acceptance, it'll crash right about
    # now. If it's good, stream will be really removed, and
    # we can proceed.

    stream.send(gabbletest.make_result_iq(stream, e2.stanza))

    q.expect('dbus-signal', signal='StreamRemoved')

    media_iface.RequestStreams(handle, [MEDIA_STREAM_TYPE_VIDEO])

    e = q.expect('dbus-signal', signal='NewStreamHandler')
    stream2_id = e.args[1]

    stream_handler2 = make_channel_proxy(conn, e.args[0], 'Media.StreamHandler')

    stream_handler2.NewNativeCandidate("fake", jt.get_remote_transports_dbus())
    stream_handler2.Ready(jt.get_audio_codecs_dbus())
    stream_handler2.StreamState(MEDIA_STREAM_STATE_CONNECTED)

    e = q.expect('stream-iq')
    assert e.query.name == 'jingle'
    assert e.query['action'] == 'content-add'
    stream.send(gabbletest.make_result_iq(stream, e.stanza))

    iq, jingle = jt._jingle_stanza('content-accept')

    content = domish.Element((None, 'content'))
    content['creator'] = 'initiator'
    content['name'] = 'stream2'
    content['senders'] = 'both'
    jingle.addChild(content)

    desc = domish.Element(("http://jabber.org/protocol/jingle/description/audio", 'description'))
    for codec, id, rate in jt.audio_codecs:
        p = domish.Element((None, 'payload-type'))
        p['name'] = codec
        p['id'] = str(id)
        p['rate'] = str(rate)
        desc.addChild(p)

    content.addChild(desc)

    xport = domish.Element(("http://www.google.com/transport/p2p", 'transport'))
    content.addChild(xport)

    stream.send(iq.toXml())

    e = q.expect('dbus-signal', signal='SetStreamPlaying', args=[1])

    # We first remove the original stream
    media_iface.RemoveStreams([stream_id])

    e = q.expect('stream-iq', iq_type='set')
    assert e.query.name == 'jingle'
    assert e.query['action'] == 'content-remove'
    stream.send(gabbletest.make_result_iq(stream, e.stanza))

    # Then we remove the second stream, which terminates the session
    media_iface.RemoveStreams([stream2_id])

    e = q.expect('stream-iq')
    assert e.query.name == 'jingle'
    assert e.query['action'] == 'session-terminate'
    stream.send(gabbletest.make_result_iq(stream, e.stanza))

    # Now the session should be terminated

    e = q.expect('dbus-signal', signal='Closed')
    assert (tp_path_prefix + e.path) == path

    # Test completed, close the connection
    conn.Disconnect()
    q.expect('dbus-signal', signal='StatusChanged', args=[2, 1])

    return True


if __name__ == '__main__':
    exec_test(test)

