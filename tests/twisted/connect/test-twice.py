
"""
Test connecting to a server with 2 accounts, testing XmppAuthenticator and
JabberAuthenticator
"""

import os
import sys
import dbus
import servicetest

import twisted
from twisted.words.xish import domish, xpath
from twisted.words.protocols.jabber import xmlstream

from gabbletest import make_connection, make_stream, JabberAuthenticator, \
                       XmppAuthenticator, \
                       XmppXmlStream, JabberXmlStream

def test(q, bus, conn1, conn2, stream1, stream2):
    # Connection 1
    conn1.Connect()
    q.expect('dbus-signal', signal='StatusChanged', args=[1, 1])
    q.expect('stream-authenticated')
    q.expect('dbus-signal', signal='PresenceUpdate',
        args=[{1L: (0L, {u'available': {}})}])
    q.expect('dbus-signal', signal='StatusChanged', args=[0, 1])

    # Connection 2
    conn2.Connect()
    q.expect('dbus-signal', signal='StatusChanged', args=[1, 1])
    q.expect('stream-authenticated')
    q.expect('dbus-signal', signal='PresenceUpdate',
        args=[{1L: (0L, {u'available': {}})}])
    q.expect('dbus-signal', signal='StatusChanged', args=[0, 1])

    # Disconnection 1
    conn1.Disconnect()
    q.expect('dbus-signal', signal='StatusChanged', args=[2, 1])

    # Disconnection 2
    conn2.Disconnect()
    q.expect('dbus-signal', signal='StatusChanged', args=[2, 1])

    return True

if __name__ == '__main__':
    queue = servicetest.IteratingEventQueue(None)
    queue.verbose = (
        os.environ.get('CHECK_TWISTED_VERBOSE', '') != ''
        or '-v' in sys.argv)

    bus = dbus.SessionBus()

    params = {
        'account': 'test1@localhost/Resource',
        'password': 'pass',
        'resource': 'Resource',
        'server': 'localhost',
        'port': dbus.UInt32(4242),
        }
    conn1 = make_connection(bus, queue.append, params)
    authenticator = JabberAuthenticator('test1', 'pass')
    stream1 = make_stream(queue.append, authenticator, protocol=JabberXmlStream,
                          port=4242)

    params = {
        'account': 'test2@localhost/Resource',
        'password': 'pass',
        'resource': 'Resource',
        'server': 'localhost',
        'port': dbus.UInt32(4343),
        }
    conn2 = make_connection(bus, queue.append, params)
    authenticator = XmppAuthenticator('test2', 'pass')
    stream2 = make_stream(queue.append, authenticator, protocol=XmppXmlStream,
                          port=4343)

    try:
        test(queue, bus, conn1, conn2, stream1, stream2)
    finally:
        try:
            conn1.Disconnect()
            conn2.Disconnect()
            # second call destroys object
            conn1.Disconnect()
            conn2.Disconnect()
        except dbus.DBusException, e:
            pass

