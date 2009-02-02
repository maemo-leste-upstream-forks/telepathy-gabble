"""
Receives several tube offers:
- Test to accept a 1-1 stream tube
  - using UNIX sockets and IPv4 sockets
  - using the old tube iface and the new tube iface
- Test to accept with bad parameters
- Test to refuse the tube offer
  - using the old tube iface and the new tube iface
"""

import dbus

from servicetest import call_async, EventPattern, EventProtocolClientFactory
from gabbletest import exec_test, acknowledge_iq, send_error_reply

from twisted.words.xish import domish, xpath
from twisted.internet import reactor
import ns
from constants import *

bob_jid = 'bob@localhost/Bob'
stream_tube_id = 49

def receive_tube_offer(q, bus, conn, stream):
    message = domish.Element(('jabber:client', 'message'))
    message['to'] = 'test@localhost/Resource'
    message['from'] = bob_jid
    tube_node = message.addElement((ns.TUBES, 'tube'))
    tube_node['type'] = 'stream'
    tube_node['service'] = 'http'
    tube_node['id'] = str(stream_tube_id)
    stream.send(message)

    old_sig, new_sig = q.expect_many(
        EventPattern('dbus-signal', signal='NewChannel'),
        EventPattern('dbus-signal', signal='NewChannels'),
        )
    chan_path = old_sig.args[0]
    assert old_sig.args[1] == CHANNEL_TYPE_TUBES, old_sig.args[1]
    assert old_sig.args[2] == HT_CONTACT
    bob_handle = old_sig.args[3]
    assert old_sig.args[2] == 1, old_sig.args[2] # Suppress_Handler
    assert len(new_sig.args) == 1
    assert len(new_sig.args[0]) == 1
    assert new_sig.args[0][0][0] == chan_path, new_sig.args[0][0]
    assert new_sig.args[0][0][1] is not None

    event = q.expect('dbus-signal', signal='NewTube')

    old_sig, new_sig = q.expect_many(
        EventPattern('dbus-signal', signal='NewChannel'),
        EventPattern('dbus-signal', signal='NewChannels'),
        )
    new_chan_path = old_sig.args[0]
    assert new_chan_path != chan_path
    assert old_sig.args[1] == CHANNEL_TYPE_STREAM_TUBE, old_sig.args[1]
    assert old_sig.args[2] == HT_CONTACT
    bob_handle = old_sig.args[3]
    assert old_sig.args[2] == 1, old_sig.args[2] # Suppress_Handler
    assert len(new_sig.args) == 1
    assert len(new_sig.args[0]) == 1
    assert new_sig.args[0][0][0] == new_chan_path, new_sig.args[0][0]
    assert new_sig.args[0][0][1] is not None

    # create channel proxies
    tubes_chan = bus.get_object(conn.bus_name, chan_path)
    tubes_iface = dbus.Interface(tubes_chan, CHANNEL_TYPE_TUBES)

    new_tube_chan = bus.get_object(conn.bus_name, new_chan_path)
    new_tube_iface = dbus.Interface(new_tube_chan, CHANNEL_TYPE_STREAM_TUBE)

    return (tubes_chan, tubes_iface, new_tube_chan, new_tube_iface)

def expect_tube_activity(q, bus, conn, stream):
    event_socket, event_iq = q.expect_many(
            EventPattern('socket-connected'),
            EventPattern('stream-iq', to=bob_jid, query_ns=ns.SI,
                query_name='si'))
    protocol = event_socket.protocol
    protocol.sendData("hello initiator")

    iq = event_iq.stanza
    si = xpath.queryForNodes('/iq/si[@xmlns="%s"]' % ns.SI,
        iq)[0]
    values = xpath.queryForNodes(
        '/si/feature[@xmlns="%s"]/x[@xmlns="%s"]/field/option/value'
        % ('http://jabber.org/protocol/feature-neg', 'jabber:x:data'), si)
    assert ns.IBB in [str(v) for v in values]

    stream_node = xpath.queryForNodes('/si/stream[@xmlns="%s"]' %
        ns.TUBES, si)[0]
    assert stream_node is not None
    assert stream_node['tube'] == str(stream_tube_id)
    stream_id = si['id']

    send_error_reply(stream, iq)

def test(q, bus, conn, stream):
    conn.Connect()

    _, vcard_event, roster_event = q.expect_many(
        EventPattern('dbus-signal', signal='StatusChanged', args=[0, 1]),
        EventPattern('stream-iq', to=None, query_ns='vcard-temp',
            query_name='vCard'),
        EventPattern('stream-iq', query_ns='jabber:iq:roster'))

    acknowledge_iq(stream, vcard_event.stanza)

    roster = roster_event.stanza
    roster['type'] = 'result'
    item = roster_event.query.addElement('item')
    item['jid'] = 'bob@localhost' # Bob can do tubes
    item['subscription'] = 'both'
    stream.send(roster)

    # Send Bob presence and his caps
    presence = domish.Element(('jabber:client', 'presence'))
    presence['from'] = 'bob@localhost/Bob'
    presence['to'] = 'test@localhost/Resource'
    c = presence.addElement('c')
    c['xmlns'] = 'http://jabber.org/protocol/caps'
    c['node'] = 'http://example.com/ICantBelieveItsNotTelepathy'
    c['ver'] = '1.2.3'
    stream.send(presence)

    event = q.expect('stream-iq', iq_type='get',
        query_ns='http://jabber.org/protocol/disco#info',
        to='bob@localhost/Bob')
    result = event.stanza
    result['type'] = 'result'
    assert event.query['node'] == \
        'http://example.com/ICantBelieveItsNotTelepathy#1.2.3'
    feature = event.query.addElement('feature')
    feature['var'] = ns.TUBES
    stream.send(result)

    # Receive a tube offer from Bob
    (tubes_chan, tubes_iface, new_tube_chan, new_tube_iface) = \
        receive_tube_offer(q, bus, conn, stream)

    # Try bad parameters on the old iface
    call_async(q, tubes_iface, 'AcceptStreamTube', stream_tube_id+1, 2, 0, '',
            byte_arrays=True)
    q.expect('dbus-error', method='AcceptStreamTube')
    call_async(q, tubes_iface, 'AcceptStreamTube', stream_tube_id, 2, 1, '',
            byte_arrays=True)
    q.expect('dbus-error', method='AcceptStreamTube')
    call_async(q, tubes_iface, 'AcceptStreamTube', stream_tube_id, 20, 0, '',
            byte_arrays=True)
    q.expect('dbus-error', method='AcceptStreamTube')

    # Try bad parameters on the new iface
    call_async(q, new_tube_iface, 'AcceptStreamTube', 20, 0, '',
            byte_arrays=True)
    q.expect('dbus-error', method='AcceptStreamTube')
    call_async(q, new_tube_iface, 'AcceptStreamTube', 0, 1, '',
            byte_arrays=True)
    q.expect('dbus-error', method='AcceptStreamTube')

    # Accept the tube with old iface, and use IPv4
    call_async(q, tubes_iface, 'AcceptStreamTube', stream_tube_id, 2, 0, '',
            byte_arrays=True)

    accept_return_event, _ = q.expect_many(
        EventPattern('dbus-return', method='AcceptStreamTube'),
        EventPattern('dbus-signal', signal='TubeStateChanged',
            args=[stream_tube_id, 2]))

    ip = accept_return_event.value[0][0]
    port = accept_return_event.value[0][1]
    assert port > 0

    factory = EventProtocolClientFactory(q)
    reactor.connectTCP(ip, port, factory)

    expect_tube_activity(q, bus, conn, stream)
    tubes_chan.Close()

    # Receive a tube offer from Bob
    (tubes_chan, tubes_iface, new_tube_chan, new_tube_iface) = \
        receive_tube_offer(q, bus, conn, stream)

    # Accept the tube with old iface, and use UNIX sockets
    call_async(q, tubes_iface, 'AcceptStreamTube', stream_tube_id, 0, 0, '',
            byte_arrays=True)

    accept_return_event, _ = q.expect_many(
        EventPattern('dbus-return', method='AcceptStreamTube'),
        EventPattern('dbus-signal', signal='TubeStateChanged',
            args=[stream_tube_id, 2]))

    socket_address = accept_return_event.value[0]

    factory = EventProtocolClientFactory(q)
    reactor.connectUNIX(socket_address, factory)

    expect_tube_activity(q, bus, conn, stream)
    tubes_chan.Close()

    # Receive a tube offer from Bob
    (tubes_chan, tubes_iface, new_tube_chan, new_tube_iface) = \
        receive_tube_offer(q, bus, conn, stream)

    # Accept the tube with new iface, and use IPv4
    call_async(q, new_tube_iface, 'AcceptStreamTube', 2, 0, '',
            byte_arrays=True)

    accept_return_event, _ = q.expect_many(
        EventPattern('dbus-return', method='AcceptStreamTube'),
        EventPattern('dbus-signal', signal='TubeStateChanged',
            args=[stream_tube_id, 2]))

    ip = accept_return_event.value[0][0]
    port = accept_return_event.value[0][1]
    assert port > 0

    factory = EventProtocolClientFactory(q)
    reactor.connectTCP(ip, port, factory)

    expect_tube_activity(q, bus, conn, stream)
    tubes_chan.Close()

    # Receive a tube offer from Bob
    (tubes_chan, tubes_iface, new_tube_chan, new_tube_iface) = \
        receive_tube_offer(q, bus, conn, stream)

    # Accept the tube with new iface, and use UNIX sockets
    call_async(q, new_tube_iface, 'AcceptStreamTube', 0, 0, '',
            byte_arrays=True)

    accept_return_event, _ = q.expect_many(
        EventPattern('dbus-return', method='AcceptStreamTube'),
        EventPattern('dbus-signal', signal='TubeStateChanged',
            args=[stream_tube_id, 2]))

    socket_address = accept_return_event.value[0]

    factory = EventProtocolClientFactory(q)
    reactor.connectUNIX(socket_address, factory)

    expect_tube_activity(q, bus, conn, stream)
    tubes_chan.Close()

    # Receive a tube offer from Bob
    (tubes_chan, tubes_iface, new_tube_chan, new_tube_iface) = \
        receive_tube_offer(q, bus, conn, stream)
    # Just close the tube
    tubes_chan.Close()

    # Receive a tube offer from Bob
    (tubes_chan, tubes_iface, new_tube_chan, new_tube_iface) = \
        receive_tube_offer(q, bus, conn, stream)
    # Just close the tube
    new_tube_chan.Close()

    q.expect_many(
        EventPattern('dbus-signal', signal='Closed'),
        EventPattern('dbus-signal', signal='ChannelClosed'))

    # OK, we're done
    conn.Disconnect()

    q.expect('dbus-signal', signal='StatusChanged', args=[2, 1])

if __name__ == '__main__':
    exec_test(test)
