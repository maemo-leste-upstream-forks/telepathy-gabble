"""
Regression test for a bug where RequestChannel times out when requesting a
group channel if the roster hasn't been received at the time of the call.
"""

import dbus

from gabbletest import exec_test, sync_stream
from servicetest import sync_dbus, call_async

HT_GROUP = 4

def test(q, bus, conn, stream):
    conn.Connect()
    # q.expect('dbus-signal', signal='StatusChanged', args=[0, 1])

    roster_event = q.expect('stream-iq', query_ns='jabber:iq:roster')
    roster_event.stanza['type'] = 'result'

    call_async(q, conn, "RequestHandles", HT_GROUP, ['test'])

    event = q.expect('dbus-return', method='RequestHandles')
    test_handle = event.value[0][0]

    call_async(q, conn, 'RequestChannel',
        'org.freedesktop.Telepathy.Channel.Type.ContactList', HT_GROUP,
        test_handle, True)

    # A previous incarnation of this test --- written with the intention that
    # RequestChannel would be called before the roster was received, to expose
    # a bug in Gabble triggered by that ordering --- was racy: if the D-Bus
    # daemon happened to be particularly busy, the call to RequestChannel
    # reached Gabble after the roster stanza. (The race was discovered when
    # that reversed order triggered a newly-introduced instance of the
    # opposite bug to the one the test was targetting!) So we sync the XMPP
    # stream and D-Bus queue here.
    sync_stream(q, stream)
    sync_dbus(bus, q, conn)

    # send an empty roster
    stream.send(roster_event.stanza)

    while True:
        event = q.expect('dbus-signal', signal='NewChannel')
        path, type, handle_type, handle, suppress_handler = event.args
        if handle_type == HT_GROUP and handle == test_handle:
            break;

    event = q.expect('dbus-return', method='RequestChannel')
    assert event.value[0] == path, (event.args[0], path)

if __name__ == '__main__':
    exec_test(test)
