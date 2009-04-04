"""
Test incoming call handling.
"""

from gabbletest import exec_test, make_result_iq, sync_stream
from servicetest import make_channel_proxy, unwrap, tp_path_prefix, \
        EventPattern, sync_dbus
import jingletest
import gabbletest
import dbus
import time

import constants as cs

def test(q, bus, conn, stream):
    jt = jingletest.JingleTest(stream, 'test@localhost', 'foo@bar.com/Foo')

    # If we need to override remote caps, feats, codecs or caps,
    # this is a good time to do it

    # Connecting
    conn.Connect()

    q.expect_many(
            EventPattern('dbus-signal', signal='StatusChanged', args=[1, 1]),
            EventPattern('stream-authenticated'),
            EventPattern('dbus-signal', signal='PresenceUpdate',
                args=[{1L: (0L, {u'available': {}})}]),
            EventPattern('dbus-signal', signal='StatusChanged', args=[0, 1]),
            )

    self_handle = conn.GetSelfHandle()

    # We need remote end's presence for capabilities
    jt.send_remote_presence()

    # Gabble doesn't trust it, so makes a disco
    event = q.expect('stream-iq', query_ns='http://jabber.org/protocol/disco#info',
             to='foo@bar.com/Foo')

    jt.send_remote_disco_reply(event.stanza)

    # Force Gabble to process the caps before calling RequestChannel
    sync_stream(q, stream)

    remote_handle = conn.RequestHandles(cs.HT_CONTACT, ["foo@bar.com/Foo"])[0]

    # Remote end calls us
    jt.incoming_call()

    nc, e = q.expect_many(
        EventPattern('dbus-signal', signal='NewChannel'),
        EventPattern('dbus-signal', signal='NewSessionHandler'),
        )
    path, ct, ht, h, _ = nc.args

    assert ct == cs.CHANNEL_TYPE_STREAMED_MEDIA, ct
    assert ht == cs.HT_CONTACT, ht
    assert h == remote_handle, h

    media_chan = make_channel_proxy(conn, path, 'Channel.Interface.Group')
    media_iface = make_channel_proxy(conn, path, 'Channel.Type.StreamedMedia')

    # S-E was notified about new session handler, and calls Ready on it
    assert e.args[1] == 'rtp'
    session_handler = make_channel_proxy(conn, e.args[0], 'Media.SessionHandler')
    session_handler.Ready()

    e2, e3 = q.expect_many(
        EventPattern('dbus-signal', signal='NewStreamHandler'),
        EventPattern('dbus-signal', signal='StreamDirectionChanged'))

    # S-E gets notified about a newly-created stream
    stream_handler = make_channel_proxy(conn, e2.args[0], 'Media.StreamHandler')

    stream_id = e3.args[0]
    assert e3.args[1] == cs.MEDIA_STREAM_DIRECTION_RECEIVE
    assert e3.args[2] == cs.MEDIA_STREAM_PENDING_LOCAL_SEND

    # Exercise channel properties
    channel_props = media_chan.GetAll(
            'org.freedesktop.Telepathy.Channel',
            dbus_interface=dbus.PROPERTIES_IFACE)
    assert channel_props['TargetHandle'] == remote_handle
    assert channel_props['TargetHandleType'] == cs.HT_CONTACT
    assert media_chan.GetHandle(dbus_interface=cs.CHANNEL) == (cs.HT_CONTACT,
            remote_handle)
    assert channel_props['TargetID'] == 'foo@bar.com'
    assert channel_props['InitiatorID'] == 'foo@bar.com'
    assert channel_props['InitiatorHandle'] == remote_handle
    assert channel_props['Requested'] == False

    group_props = media_chan.GetAll(cs.CHANNEL_IFACE_GROUP,
        dbus_interface=dbus.PROPERTIES_IFACE)

    assert group_props['SelfHandle'] == self_handle, \
        (group_props['SelfHandle'], self_handle)

    flags = group_props['GroupFlags']
    assert flags & cs.GF_PROPERTIES, flags
    # Changing members in any way other than adding or removing yourself is
    # meaningless for incoming calls, and the flags need not be sent to change
    # your own membership.
    assert not flags & cs.GF_CAN_ADD, flags
    assert not flags & cs.GF_CAN_REMOVE, flags
    assert not flags & cs.GF_CAN_RESCIND, flags

    assert group_props['Members'] == [remote_handle], group_props['Members']
    assert group_props['RemotePendingMembers'] == [], \
        group_props['RemotePendingMembers']
    # We're local pending because remote_handle invited us.
    assert group_props['LocalPendingMembers'] == \
        [(self_handle, remote_handle, cs.GC_REASON_INVITED, '')], \
        unwrap(group_props['LocalPendingMembers'])

    streams = media_chan.ListStreams(
            dbus_interface=cs.CHANNEL_TYPE_STREAMED_MEDIA)
    assert len(streams) == 1, streams
    assert len(streams[0]) == 6, streams[0]
    # streams[0][0] is the stream identifier, which in principle we can't
    # make any assertion about (although in practice it's probably 1)
    assert streams[0][1] == remote_handle, (streams[0], remote_handle)
    assert streams[0][2] == cs.MEDIA_STREAM_TYPE_AUDIO, streams[0]
    # We haven't connected yet
    assert streams[0][3] == cs.MEDIA_STREAM_STATE_DISCONNECTED, streams[0]
    # In Gabble, incoming streams start off with remote send enabled, and
    # local send requested
    assert streams[0][4] == cs.MEDIA_STREAM_DIRECTION_RECEIVE, streams[0]
    assert streams[0][5] == cs.MEDIA_STREAM_PENDING_LOCAL_SEND, streams[0]

    # Connectivity checks happen before we have accepted the call
    stream_handler.NewNativeCandidate("fake", jt.get_remote_transports_dbus())
    stream_handler.Ready(jt.get_audio_codecs_dbus())
    stream_handler.StreamState(cs.MEDIA_STREAM_STATE_CONNECTED)
    stream_handler.SupportedCodecs(jt.get_audio_codecs_dbus())

    # peer gets the transport
    e = q.expect('stream-iq')
    assert e.query.name == 'jingle'
    assert e.query['action'] == 'transport-info'
    assert e.query['initiator'] == 'foo@bar.com/Foo'

    stream.send(gabbletest.make_result_iq(stream, e.stanza))

    # Make sure everything's processed
    sync_stream(q, stream)
    sync_dbus(bus, q, conn)

    # At last, accept the call
    media_chan.AddMembers([self_handle], 'accepted')

    # Call is accepted, we become a member, and the stream that was pending
    # local send is now sending.
    memb, acc, _, _, _ = q.expect_many(
        EventPattern('dbus-signal', signal='MembersChanged',
            args=[u'', [self_handle], [], [], [], self_handle,
                  cs.GC_REASON_NONE]),
        EventPattern('stream-iq',
            predicate=lambda e: (e.query.name == 'jingle' and
                e.query['action'] == 'session-accept')),
        EventPattern('dbus-signal', signal='SetStreamSending', args=[True]),
        EventPattern('dbus-signal', signal='SetStreamPlaying', args=[True]),
        EventPattern('dbus-signal', signal='StreamDirectionChanged',
            args=[stream_id, cs.MEDIA_STREAM_DIRECTION_BIDIRECTIONAL, 0]),
        )

    # we are now both in members
    members = media_chan.GetMembers()
    assert set(members) == set([self_handle, remote_handle]), members

    # Connected! Blah, blah, ...

    # 'Nuff said
    jt.remote_terminate()
    q.expect('dbus-signal', signal='Closed', path=path[len(tp_path_prefix):])

    conn.Disconnect()
    q.expect('dbus-signal', signal='StatusChanged', args=[2, 1])

    return True


if __name__ == '__main__':
    exec_test(test)

