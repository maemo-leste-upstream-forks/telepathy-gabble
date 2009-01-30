
"""
Test that Gabble properly cleans up delayed RequestStream contexts
and returns an error when Disconnect is called and there are
incomplete requests.
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

    # We intentionally DON'T set remote presence yet. Since Gabble is still
    # unsure whether to treat contact as offline for this purpose, it
    # will tentatively allow channel creation and contact handle addition

    handle = conn.RequestHandles(1, [jt.remote_jid])[0]

    path = conn.RequestChannel(
        'org.freedesktop.Telepathy.Channel.Type.StreamedMedia', 1, handle, True)
    media_iface = make_channel_proxy(conn, path, 'Channel.Type.StreamedMedia')

    # Now we request streams before either <presence> or caps have arrived
    call_async(q, media_iface, 'RequestStreams', handle, [0]) # req audio stream

    # Variant of the "make sure disco is processed" test hack, but this time
    # we want to make sure RequestStreams is processed (and suspended) before
    # presence arrives, to be able to test it properl.y
    el = domish.Element(('jabber.client', 'presence'))
    el['from'] = 'bob@example.com/Bar'
    stream.send(el.toXml())
    q.expect('dbus-signal', signal='PresenceUpdate')
    # OK, now we can continue. End of hack

    conn.Disconnect()

    # RequestStreams should now return error
    q.expect('dbus-error', method='RequestStreams')


    q.expect('dbus-signal', signal='StatusChanged', args=[2, 1])

    return True


if __name__ == '__main__':
    exec_test(test, timeout=10)

