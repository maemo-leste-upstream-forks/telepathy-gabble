"""
Test that Gabble properly cleans up delayed RequestStream contexts
and returns an error when Disconnect is called and there are
incomplete requests.
"""

from gabbletest import exec_test, disconnect_conn
from servicetest import make_channel_proxy, call_async, sync_dbus, EventPattern
import jingletest

import constants as cs

from config import VOIP_ENABLED

if not VOIP_ENABLED:
    print "NOTE: built with --disable-voip"
    raise SystemExit(77)

def test(q, bus, conn, stream, channel_type):
    jt = jingletest.JingleTest(stream, 'test@localhost', 'foo@bar.com/Foo')

    # We intentionally DON'T set remote presence yet. Since Gabble is still
    # unsure whether to treat contact as offline for this purpose, it
    # will tentatively allow channel creation and contact handle addition

    handle = conn.RequestHandles(cs.HT_CONTACT, [jt.remote_jid])[0]

    if channel_type == cs.CHANNEL_TYPE_STREAMED_MEDIA:
        path = conn.RequestChannel(cs.CHANNEL_TYPE_STREAMED_MEDIA,
            cs.HT_CONTACT, handle, True)
        media_iface = make_channel_proxy(conn, path,
            'Channel.Type.StreamedMedia')

    # So it turns out that the calls to RequestStreams and Disconnect could be
    # reordered while the first waits on the result of introspecting the
    # channel's object which is kicked off by making a proxy object for it,
    # whereas the connection proxy is long ago introspected. Isn't dbus-python
    # great? Syncing here forces that introspection to finish so we can rely on
    # the ordering of RequestStreams and Disconnect. Yay.
    sync_dbus(bus, q, conn)

    # Now we request streams before either <presence> or caps have arrived
    if channel_type == cs.CHANNEL_TYPE_STREAMED_MEDIA:
        call_async(q, media_iface, 'RequestStreams', handle,
            [cs.MEDIA_STREAM_TYPE_AUDIO])

        before_events, after_events = disconnect_conn(q, conn, stream,
            [EventPattern('dbus-error', method='RequestStreams')])

        # RequestStreams should now return NotAvailable
        assert before_events[0].error.get_dbus_name() == cs.NOT_AVAILABLE, \
            before_events[0].error
    else:
        call_async(q, conn.Requests, 'CreateChannel',
            { cs.CHANNEL_TYPE: channel_type,
              cs.TARGET_HANDLE_TYPE: cs.HT_CONTACT,
              cs.TARGET_ID: jt.remote_jid,
              cs.CALL_INITIAL_AUDIO: True
            })

        before_events, after_events = disconnect_conn(q, conn, stream,
            [EventPattern('dbus-error', method='CreateChannel')])

        # CreateChannel should now return Disconnected
        assert before_events[0].error.get_dbus_name() == cs.DISCONNECTED, \
            before_events[0].error

if __name__ == '__main__':
    exec_test(lambda q, bus, conn, stream:
        test(q, bus, conn, stream, cs.CHANNEL_TYPE_STREAMED_MEDIA), timeout=10)
    print "FIXME: leaks connection, everyone dies"
    raise SystemExit(77)
    exec_test(lambda q, bus, conn, stream:
        test(q, bus, conn, stream, cs.CHANNEL_TYPE_CALL), timeout=10)

