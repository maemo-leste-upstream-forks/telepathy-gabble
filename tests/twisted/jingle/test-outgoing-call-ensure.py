
"""
Test making outgoing calls using EnsureChannel, and retrieving existing calls
using EnsureChannel.
"""

from gabbletest import exec_test, make_result_iq, sync_stream
from servicetest import make_channel_proxy, unwrap, tp_path_prefix, \
        call_async, EventPattern
from twisted.words.xish import domish
import jingletest
import gabbletest
import dbus
import time


def test(q, bus, conn, stream):
    jt = jingletest.JingleTest(stream, 'test@localhost', 'foo@bar.com/Foo')

    # If we need to override remote caps, feats, codecs or caps,
    # this is a good time to do it

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

    # Force Gabble to process the caps before calling EnsureChannel
    sync_stream(q, stream)

    handle = conn.RequestHandles(1, [jt.remote_jid])[0]

    requestotron = dbus.Interface(conn,
            'org.freedesktop.Telepathy.Connection.Interface.Requests')


    # Ensure a channel that doesn't exist yet.
    call_async(q, requestotron, 'EnsureChannel',
            { 'org.freedesktop.Telepathy.Channel.ChannelType':
                'org.freedesktop.Telepathy.Channel.Type.StreamedMedia',
              'org.freedesktop.Telepathy.Channel.TargetHandleType': 1,
              'org.freedesktop.Telepathy.Channel.TargetHandle': handle,
              })

    ret, old_sig, new_sig = q.expect_many(
        EventPattern('dbus-return', method='EnsureChannel'),
        EventPattern('dbus-signal', signal='NewChannel'),
        EventPattern('dbus-signal', signal='NewChannels'),
        )

    yours, path, props = ret.value

    # this channel was created in response to our EnsureChannel call, so it
    # should be ours.
    assert yours, ret.value

    sig_path, sig_ct, sig_ht, sig_h, sig_sh = old_sig.args

    assert sig_path == path, (sig_path, path)
    assert sig_ct == u'org.freedesktop.Telepathy.Channel.Type.StreamedMedia',\
            sig_ct
    assert sig_ht == 1, sig_ht      # HandleType = Contact
    assert sig_h == handle, sig_h
    assert sig_sh == True           # suppress handler

    assert len(new_sig.args) == 1
    assert len(new_sig.args[0]) == 1        # one channel
    assert len(new_sig.args[0][0]) == 2     # two struct members
    assert new_sig.args[0][0][0] == path
    emitted_props = new_sig.args[0][0][1]

    assert emitted_props['org.freedesktop.Telepathy.Channel.ChannelType'] ==\
            'org.freedesktop.Telepathy.Channel.Type.StreamedMedia'
    assert emitted_props['org.freedesktop.Telepathy.Channel.'
            'TargetHandleType'] == 1        # Contact
    assert emitted_props['org.freedesktop.Telepathy.Channel.TargetHandle'] ==\
            handle
    assert emitted_props['org.freedesktop.Telepathy.Channel.TargetID'] ==\
            'foo@bar.com', emitted_props
    assert emitted_props['org.freedesktop.Telepathy.Channel.'
            'Requested'] == True
    assert emitted_props['org.freedesktop.Telepathy.Channel.'
            'InitiatorHandle'] == self_handle
    assert emitted_props['org.freedesktop.Telepathy.Channel.'
            'InitiatorID'] == 'test@localhost'


    # Now ensure a media channel with the same contact, and check it's the
    # same.
    call_async(q, requestotron, 'EnsureChannel',
            { 'org.freedesktop.Telepathy.Channel.ChannelType':
                'org.freedesktop.Telepathy.Channel.Type.StreamedMedia',
              'org.freedesktop.Telepathy.Channel.TargetHandleType': 1,
              'org.freedesktop.Telepathy.Channel.TargetHandle': handle,
              })

    event = q.expect('dbus-return', method='EnsureChannel')
    yours2, path2, props2 = event.value

    # We should have got back the same channel we created a page or so ago.
    assert path == path2, (path, path2)
    # It's not been created for this call, so Yours should be False.
    assert not yours2

    # Time passes ... afterwards we close the chan

    chan = bus.get_object(conn.bus_name, path)
    chan.Close()


    # Now, create an anonymous channel with RequestChannel, add the other
    # person to it with RequestStreams, then Ensure a media channel with that
    # person.  We should get the anonymous channel back.
    call_async(q, conn, 'RequestChannel',
        'org.freedesktop.Telepathy.Channel.Type.StreamedMedia', 0, 0, True)

    ret, old_sig, new_sig = q.expect_many(
        EventPattern('dbus-return', method='RequestChannel'),
        EventPattern('dbus-signal', signal='NewChannel'),
        EventPattern('dbus-signal', signal='NewChannels'),
        )

    path = ret.value[0]
    assert old_sig.args[0] == path, (old_sig.args[0], path)
    assert old_sig.args[1] == u'org.freedesktop.Telepathy.Channel.Type.StreamedMedia',\
            old_sig.args[1]
    assert old_sig.args[2] == 0, sig.args[2]
    assert old_sig.args[3] == 0, sig.args[3]
    assert old_sig.args[4] == True      # suppress handler

    assert len(new_sig.args) == 1
    assert len(new_sig.args[0]) == 1        # one channel
    assert len(new_sig.args[0][0]) == 2     # two struct members
    assert new_sig.args[0][0][0] == path
    emitted_props = new_sig.args[0][0][1]

    assert emitted_props['org.freedesktop.Telepathy.Channel.ChannelType'] ==\
            'org.freedesktop.Telepathy.Channel.Type.StreamedMedia'
    assert emitted_props['org.freedesktop.Telepathy.Channel.'
            'TargetHandleType'] == 0
    assert emitted_props['org.freedesktop.Telepathy.Channel.TargetHandle'] ==\
            0
    assert emitted_props['org.freedesktop.Telepathy.Channel.TargetID'] == ''
    assert emitted_props['org.freedesktop.Telepathy.Channel.Requested']\
            == True
    assert emitted_props['org.freedesktop.Telepathy.Channel.InitiatorHandle']\
            == self_handle
    assert emitted_props['org.freedesktop.Telepathy.Channel.InitiatorID'] \
            == 'test@localhost'

    media_iface = make_channel_proxy(conn, path, 'Channel.Type.StreamedMedia')

    # FIXME: Hack to make sure the disco info has been processed - we need to
    # send Gabble some XML that will cause an event when processed, and
    # wait for that event (until
    # https://bugs.freedesktop.org/show_bug.cgi?id=15769 is fixed)
    el = domish.Element(('jabber.client', 'presence'))
    el['from'] = 'bob@example.com/Bar'
    stream.send(el.toXml())
    q.expect('dbus-signal', signal='PresenceUpdate')
    # OK, now we can continue. End of hack

    # Request streams with the other person.  This should make them the
    # channel's "peer" property.
    media_iface.RequestStreams(handle, [0]) # 0 == MEDIA_STREAM_TYPE_AUDIO

    # Now, Ensuring a media channel with handle should yield the channel just
    # created.

    call_async(q, requestotron, 'EnsureChannel',
            { 'org.freedesktop.Telepathy.Channel.ChannelType':
                'org.freedesktop.Telepathy.Channel.Type.StreamedMedia',
              'org.freedesktop.Telepathy.Channel.TargetHandleType': 1,
              'org.freedesktop.Telepathy.Channel.TargetHandle': handle,
              })

    event = q.expect('dbus-return', method='EnsureChannel')
    yours, path2, _ = event.value

    # we should have got back the anonymous channel we got with requestchannel
    # and called RequestStreams(handle) on.
    assert path == path2, (path, path2)
    # It's not been created for this call, so Yours should be False.
    assert not yours

    # Time passes ... afterwards we close the chan

    chan = bus.get_object(conn.bus_name, path)
    chan.Close()

    conn.Disconnect()
    q.expect('dbus-signal', signal='StatusChanged', args=[2, 1])

    return True


if __name__ == '__main__':
    exec_test(test)

