"""Test IBB stream tube support in the context of a MUC."""

import base64
import dbus

from servicetest import call_async, EventPattern, EventProtocolClientFactory
from gabbletest import exec_test, make_result_iq, acknowledge_iq
from constants import *
import ns
import tubetestutil as t

from twisted.words.xish import domish, xpath
from twisted.internet import reactor
from twisted.words.protocols.jabber.client import IQ
from constants import *

sample_parameters = dbus.Dictionary({
    's': 'hello',
    'ay': dbus.ByteArray('hello'),
    'u': dbus.UInt32(123),
    'i': dbus.Int32(-123),
    }, signature='sv')

def test(q, bus, conn, stream):
    conn.Connect()

    _, iq_event = q.expect_many(
        EventPattern('dbus-signal', signal='StatusChanged', args=[0, 1]),
        EventPattern('stream-iq', to=None, query_ns='vcard-temp',
            query_name='vCard'))

    acknowledge_iq(stream, iq_event.stanza)

    call_async(q, conn, 'RequestHandles', 2,
        ['chat@conf.localhost'])

    event = q.expect('stream-iq', to='conf.localhost',
            query_ns='http://jabber.org/protocol/disco#info')
    result = make_result_iq(stream, event.stanza)
    feature = result.firstChildElement().addElement('feature')
    feature['var'] = 'http://jabber.org/protocol/muc'
    stream.send(result)

    event = q.expect('dbus-return', method='RequestHandles')
    handles = event.value[0]
    room_handle = handles[0]

    # join the muc
    call_async(q, conn, 'RequestChannel', CHANNEL_TYPE_TEXT, HT_ROOM,
        room_handle, True)

    _, stream_event = q.expect_many(
        EventPattern('dbus-signal', signal='MembersChanged',
            args=[u'', [], [], [], [2], 0, 0]),
        EventPattern('stream-presence', to='chat@conf.localhost/test'))

    # Send presence for other member of room.
    presence = domish.Element((None, 'presence'))
    presence['from'] = 'chat@conf.localhost/bob'
    x = presence.addElement(('http://jabber.org/protocol/muc#user', 'x'))
    item = x.addElement('item')
    item['affiliation'] = 'owner'
    item['role'] = 'moderator'
    stream.send(presence)

    # Send presence for own membership of room.
    presence = domish.Element((None, 'presence'))
    presence['from'] = 'chat@conf.localhost/test'
    x = presence.addElement(('http://jabber.org/protocol/muc#user', 'x'))
    item = x.addElement('item')
    item['affiliation'] = 'none'
    item['role'] = 'participant'
    stream.send(presence)

    q.expect('dbus-signal', signal='MembersChanged',
            args=[u'', [2, 3], [], [], [], 0, 0])

    assert conn.InspectHandles(1, [2]) == ['chat@conf.localhost/test']
    assert conn.InspectHandles(1, [3]) == ['chat@conf.localhost/bob']
    bob_handle = 3

    event = q.expect('dbus-return', method='RequestChannel')

    # Bob offers a stream tube
    stream_tube_id = 666
    presence = domish.Element((None, 'presence'))
    presence['from'] = 'chat@conf.localhost/bob'
    x = presence.addElement(('http://jabber.org/protocol/muc#user', 'x'))
    item = x.addElement('item')
    item['affiliation'] = 'owner'
    item['role'] = 'moderator'
    tubes = presence.addElement((ns.TUBES, 'tubes'))
    tube = tubes.addElement((None, 'tube'))
    tube['type'] = 'stream'
    tube['service'] = 'echo'
    tube['id'] = str(stream_tube_id)
    parameters = tube.addElement((None, 'parameters'))
    parameter = parameters.addElement((None, 'parameter'))
    parameter['name'] = 's'
    parameter['type'] = 'str'
    parameter.addContent('hello')
    parameter = parameters.addElement((None, 'parameter'))
    parameter['name'] = 'ay'
    parameter['type'] = 'bytes'
    parameter.addContent('aGVsbG8=')
    parameter = parameters.addElement((None, 'parameter'))
    parameter['name'] = 'u'
    parameter['type'] = 'uint'
    parameter.addContent('123')
    parameter = parameters.addElement((None, 'parameter'))
    parameter['name'] = 'i'
    parameter['type'] = 'int'
    parameter.addContent('-123')

    stream.send(presence)

    # text channel
    event, new_event = q.expect_many(
        EventPattern('dbus-signal', signal='NewChannel'),
        EventPattern('dbus-signal', signal='NewChannels'))

    assert event.args[1] == CHANNEL_TYPE_TEXT, event.args

    channels = new_event.args[0]
    assert len(channels) == 1
    path, props = channels[0]
    assert props[CHANNEL_TYPE] == CHANNEL_TYPE_TEXT

    # tubes channel is automatically created
    event, new_event = q.expect_many(
        EventPattern('dbus-signal', signal='NewChannel'),
        EventPattern('dbus-signal', signal='NewChannels'))

    assert event.args[1] == CHANNEL_TYPE_TUBES, event.args
    assert event.args[2] == HT_ROOM
    assert event.args[3] == room_handle

    tubes_chan = bus.get_object(conn.bus_name, event.args[0])
    tubes_iface = dbus.Interface(tubes_chan, event.args[1])

    channel_props = tubes_chan.GetAll(CHANNEL, dbus_interface=PROPERTIES_IFACE)
    assert channel_props['TargetID'] == 'chat@conf.localhost', channel_props
    assert channel_props['Requested'] == False
    assert channel_props['InitiatorID'] == ''
    assert channel_props['InitiatorHandle'] == 0

    channels = new_event.args[0]
    assert len(channels) == 1
    path, props = channels[0]
    assert props[CHANNEL_TYPE] == CHANNEL_TYPE_TUBES

    tubes_self_handle = tubes_chan.GetSelfHandle(dbus_interface=CHANNEL_IFACE_GROUP)

    q.expect('dbus-signal', signal='NewTube',
        args=[stream_tube_id, bob_handle, 1, 'echo', sample_parameters, 0])

    expected_tube = (stream_tube_id, bob_handle, TUBE_TYPE_STREAM, 'echo',
        sample_parameters, TUBE_STATE_LOCAL_PENDING)
    tubes = tubes_iface.ListTubes(byte_arrays=True)
    assert tubes == [(
        stream_tube_id,
        bob_handle,
        1,      # Stream
        'echo',
        sample_parameters,
        TUBE_CHANNEL_STATE_LOCAL_PENDING
        )]

    assert len(tubes) == 1, unwrap(tubes)
    t.check_tube_in_tubes(expected_tube, tubes)

    # tube channel is also announced (new API)
    new_event = q.expect('dbus-signal', signal='NewChannels')

    channels = new_event.args[0]
    assert len(channels) == 1
    path, props = channels[0]
    assert props[CHANNEL_TYPE] == CHANNEL_TYPE_STREAM_TUBE
    assert props[INITIATOR_HANDLE] == bob_handle
    assert props[INITIATOR_ID] == 'chat@conf.localhost/bob'
    assert props[INTERFACES] == [CHANNEL_IFACE_GROUP, CHANNEL_IFACE_TUBE]
    assert props[REQUESTED] == False
    assert props[TARGET_HANDLE] == room_handle
    assert props[TARGET_ID] == 'chat@conf.localhost'
    assert props[STREAM_TUBE_SERVICE] == 'echo'
    assert props[TUBE_PARAMETERS] == {'s': 'hello', 'ay': 'hello', 'u': 123, 'i': -123}

    tube_chan = bus.get_object(conn.bus_name, path)
    tube_props = tube_chan.GetAll(CHANNEL_IFACE_TUBE, dbus_interface=PROPERTIES_IFACE,
        byte_arrays=True)
    assert tube_props['Parameters'] == sample_parameters
    assert tube_props['State'] == TUBE_CHANNEL_STATE_LOCAL_PENDING

    # Accept the tube
    call_async(q, tubes_iface, 'AcceptStreamTube', stream_tube_id, 0, 0, '',
            byte_arrays=True)

    accept_return_event, _ = q.expect_many(
        EventPattern('dbus-return', method='AcceptStreamTube'),
        EventPattern('dbus-signal', signal='TubeStateChanged',
            args=[stream_tube_id, 2]))

    unix_socket_adr = accept_return_event.value[0]

    factory = EventProtocolClientFactory(q)
    reactor.connectUNIX(unix_socket_adr, factory)

    event = q.expect('socket-connected')
    protocol = event.protocol
    protocol.sendData("hello initiator")

    # expect SI request
    event = q.expect('stream-iq', to='chat@conf.localhost/bob', query_ns=ns.SI,
        query_name='si')
    iq = event.stanza
    si = xpath.queryForNodes('/iq/si[@xmlns="%s"]' % ns.SI,
        iq)[0]
    values = xpath.queryForNodes('/si/feature[@xmlns="%s"]/x[@xmlns="%s"]/field/option/value'
        % ('http://jabber.org/protocol/feature-neg', 'jabber:x:data'), si)
    assert ns.IBB in [str(v) for v in values]

    muc_stream_node = xpath.queryForNodes('/si/muc-stream[@xmlns="%s"]' %
        ns.TUBES, si)[0]
    assert muc_stream_node is not None
    assert muc_stream_node['tube'] == str(stream_tube_id)
    stream_id = si['id']

    # reply to SI. We want to use IBB
    result = IQ(stream, "result")
    result["id"] = iq["id"]
    result['from'] = 'chat@conf.localhost/bob'
    result['to'] = 'chat@conf.localhost/test'
    si = result.addElement((ns.SI, 'si'))
    feature = si.addElement((ns.FEATURE_NEG, 'feature'))
    x = feature.addElement((ns.X_DATA, 'x'))
    x['type'] = 'submit'
    field = x.addElement((None, 'field'))
    field['var'] = 'stream-method'
    value = field.addElement((None, 'value'))
    value.addContent(ns.IBB)
    si.addElement((ns.TUBES, 'tube'))
    stream.send(result)

    # wait IBB init IQ
    event = q.expect('stream-iq', to='chat@conf.localhost/bob',
        query_name='open', query_ns=ns.IBB)
    iq = event.stanza
    open = xpath.queryForNodes('/iq/open', iq)[0]
    assert open['sid'] == stream_id

    # open the IBB bytestream
    reply = make_result_iq(stream, iq)
    stream.send(reply)

    event = q.expect('stream-message', to='chat@conf.localhost/bob')
    message = event.stanza
    data_nodes = xpath.queryForNodes('/message/data[@xmlns="%s"]' % ns.IBB,
        message)
    assert data_nodes is not None
    assert len(data_nodes) == 1
    ibb_data = data_nodes[0]
    assert ibb_data['sid'] == stream_id
    binary = base64.b64decode(str(ibb_data))
    assert binary == 'hello initiator'

    # reply on the socket
    message = domish.Element(('jabber:client', 'message'))
    message['from'] = 'chat@conf.localhost/bob'
    message['to'] = 'chat@conf.localhost/test'
    data_node = message.addElement((ns.IBB, 'data'))
    data_node['sid'] = stream_id
    data_node['seq'] = '0'
    data_node.addContent(base64.b64encode('hi joiner!'))
    stream.send(message)

    q.expect('socket-data', protocol=protocol, data="hi joiner!")

    # OK, we're done
    conn.Disconnect()

    q.expect_many(
        EventPattern('dbus-signal', signal='TubeClosed', args=[stream_tube_id]),
        EventPattern('dbus-signal', signal='StatusChanged', args=[2, 1]))

if __name__ == '__main__':
    exec_test(test)
