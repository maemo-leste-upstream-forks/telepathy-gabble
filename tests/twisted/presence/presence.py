"""
A simple smoke-test for C.I.SimplePresence

FIXME: test C.I.Presence too
"""

from twisted.words.xish import domish

from gabbletest import exec_test
from servicetest import EventPattern
import ns
import constants as cs

def test(q, bus, conn, stream):
    conn.Connect()
    _, event = q.expect_many(
        EventPattern('dbus-signal', signal='StatusChanged',
            args=[cs.CONN_STATUS_CONNECTED, cs.CSR_REQUESTED]),
        EventPattern('stream-iq', query_ns=ns.ROSTER),
        )

    amy_handle = conn.RequestHandles(1, ['amy@foo.com'])[0]

    event.stanza['type'] = 'result'

    item = event.query.addElement('item')
    item['jid'] = 'amy@foo.com'
    item['subscription'] = 'both'

    stream.send(event.stanza)

    presence = domish.Element((None, 'presence'))
    presence['from'] = 'amy@foo.com'
    show = presence.addElement((None, 'show'))
    show.addContent('away')
    status = presence.addElement((None, 'status'))
    status.addContent('At the pub')
    stream.send(presence)

    event = q.expect('dbus-signal', signal='PresencesChanged')
    assert event.args[0] == { amy_handle: (3, 'away', 'At the pub') }

    presence = domish.Element((None, 'presence'))
    presence['from'] = 'amy@foo.com'
    show = presence.addElement((None, 'show'))
    show.addContent('chat')
    status = presence.addElement((None, 'status'))
    status.addContent('I may have been drinking')
    stream.send(presence)

    event = q.expect('dbus-signal', signal='PresencesChanged')
    assert event.args[0] == { amy_handle: (2, 'chat', 'I may have been drinking') }

if __name__ == '__main__':
    exec_test(test)
