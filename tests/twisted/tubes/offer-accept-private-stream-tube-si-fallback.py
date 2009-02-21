"""Test stream initiation fallback."""

import base64
import os

import dbus
from dbus.connection import Connection
from dbus.lowlevel import SignalMessage

from servicetest import call_async, EventPattern, tp_name_prefix, watch_tube_signals, EventProtocolClientFactory
from gabbletest import exec_test, acknowledge_iq
import ns

from twisted.words.xish import domish, xpath
from twisted.internet import reactor
from twisted.words.protocols.jabber.client import IQ

import tubetestutil as t

def test(q, bus, conn, stream):
    t.set_up_echo('')

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
    item['jid'] = 'bob@localhost'
    item['subscription'] = 'both'
    stream.send(roster)

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

    requestotron = dbus.Interface(conn,
            'org.freedesktop.Telepathy.Connection.Interface.Requests')
    bob_handle = conn.RequestHandles(1, ['bob@localhost'])[0]

    # Offer a tube to Bob
    call_async(q, requestotron, 'CreateChannel',
            {'org.freedesktop.Telepathy.Channel.ChannelType':
                'org.freedesktop.Telepathy.Channel.Type.StreamTube.DRAFT',
             'org.freedesktop.Telepathy.Channel.TargetHandleType':
                1,
             'org.freedesktop.Telepathy.Channel.TargetHandle':
                bob_handle,
             'org.freedesktop.Telepathy.Channel.Type.StreamTube.DRAFT.Service':
                'echo',
            })
    ret, _, _ = q.expect_many(
        EventPattern('dbus-return', method='CreateChannel'),
        EventPattern('dbus-signal', signal='NewChannel'),
        EventPattern('dbus-signal', signal='NewChannels'),
        )

    chan_path = ret.value[0]
    channels = filter(lambda x:
      x[1] == "org.freedesktop.Telepathy.Channel.Type.StreamTube.DRAFT" and
      x[0] == chan_path,
      conn.ListChannels())
    tube_chan = bus.get_object(conn.bus_name, channels[0][0])
    tube_iface = dbus.Interface(tube_chan,
        tp_name_prefix + '.Channel.Type.StreamTube.DRAFT')

    path = os.getcwd() + '/stream'
    call_async(q, tube_iface, 'OfferStreamTube',
        0, dbus.ByteArray(path), 0, "", {'foo': 'bar'})

    event = q.expect('stream-message')
    message = event.stanza
    tube_nodes = xpath.queryForNodes('/message/tube[@xmlns="%s"]' % ns.TUBES,
        message)
    assert tube_nodes is not None
    assert len(tube_nodes) == 1
    tube = tube_nodes[0]

    assert tube['service'] == 'echo'
    assert tube['type'] == 'stream'
    assert not tube.hasAttribute('initiator')
    stream_tube_id = long(tube['id'])

    # The CM is the server, so fake a client wanting to talk to it
    iq = IQ(stream, 'set')
    iq['to'] = 'test@localhost/Resource'
    iq['from'] = 'bob@localhost/Bob'
    si = iq.addElement((ns.SI, 'si'))
    si['id'] = 'alpha'
    si['profile'] = ns.TUBES
    feature = si.addElement((ns.FEATURE_NEG, 'feature'))
    x = feature.addElement((ns.X_DATA, 'x'))
    x['type'] = 'form'
    field = x.addElement((None, 'field'))
    field['var'] = 'stream-method'
    field['type'] = 'list-single'
    option = field.addElement((None, 'option'))
    value = option.addElement((None, 'value'))
    value.addContent(ns.BYTESTREAMS)
    option = field.addElement((None, 'option'))
    value = option.addElement((None, 'value'))
    value.addContent("invalid-stream-method")
    option = field.addElement((None, 'option'))
    value = option.addElement((None, 'value'))
    value.addContent(ns.IBB)

    stream_node = si.addElement((ns.TUBES, 'stream'))
    stream_node['tube'] = str(stream_tube_id)

    # Bob supports multi bytestreams
    si_multiple = si.addElement((ns.SI_MULTIPLE, 'si-multiple'))

    stream.send(iq)

    si_reply_event, _ = q.expect_many(
            EventPattern('stream-iq', iq_type='result'),
            EventPattern('dbus-signal', signal='TubeChannelStateChanged',
                args=[2])) # 2 == OPEN

    iq = si_reply_event.stanza
    # check if SI reply contains the 2 bytestreams
    methods = xpath.queryForNodes('/iq/si[@xmlns="%s"]/si-multiple[@xmlns="%s"]/value' %
            (ns.SI, ns.SI_MULTIPLE), iq)
    assert len(methods) == 2
    assert methods[0].name == 'value'
    assert str(methods[0]) == ns.BYTESTREAMS
    assert methods[1].name == 'value'
    assert str(methods[1]) == ns.IBB
    tube = xpath.queryForNodes('/iq/si[@xmlns="%s"]/tube[@xmlns="%s"]' %
            (ns.SI, ns.TUBES), iq)
    assert len(tube) == 1

    q.expect('dbus-signal', signal='StreamTubeNewConnection',
        args=[bob_handle])

    # Bob initiates the S5B bytestream. He sends a not-working streamhost
    # so Gabble won't be able to connect to it.
    iq = IQ(stream, 'set')
    iq['to'] = 'test@localhost/Resource'
    iq['from'] = 'bob@localhost/Bob'
    query = iq.addElement((ns.BYTESTREAMS, 'query'))
    query['sid'] = 'alpha'
    query['mode'] = 'tcp'
    streamhost = query.addElement('streamhost')
    streamhost['jid'] = 'bob@localhost/Bob'
    streamhost['host'] = 'invalid.invalid'
    streamhost['port'] = '1234'
    stream.send(iq)

    # Gabble informs Bob that Sock5 failed
    event = q.expect('stream-iq', iq_type='error', to='bob@localhost/Bob')

    # Then Bob tries with IBB
    iq = IQ(stream, 'set')
    iq['to'] = 'test@localhost/Resource'
    iq['from'] = 'bob@localhost/Bob'
    open = iq.addElement((ns.IBB, 'open'))
    open['sid'] = 'alpha'
    open['block-size'] = '4096'
    stream.send(iq)

    # cool, IBB succeeded
    q.expect('stream-iq', iq_type='result')

    # have the fake client send us some data
    message = domish.Element(('jabber:client', 'message'))
    message['to'] = 'test@localhost/Resource'
    message['from'] = 'bob@localhost/Bob'
    data_node = message.addElement((ns.IBB, 'data'))
    data_node['sid'] = 'alpha'
    data_node['seq'] = '0'
    data_node.addContent(base64.b64encode('HELLO, WORLD'))
    stream.send(message)

    event = q.expect('stream-message', to='bob@localhost/Bob')
    message = event.stanza

    data_nodes = xpath.queryForNodes('/message/data[@xmlns="%s"]' % ns.IBB,
        message)
    assert data_nodes is not None
    assert len(data_nodes) == 1
    ibb_data = data_nodes[0]
    assert ibb_data['sid'] == 'alpha'
    binary = base64.b64decode(str(ibb_data))
    assert binary == 'hello, world'


    # Test the other side. Bob offers a stream tube.
    message = domish.Element(('jabber:client', 'message'))
    message['to'] = 'test@localhost/Resource'
    message['from'] = 'bob@localhost/Bob'
    message['id'] = 'msg-id'
    tube = message.addElement((ns.TUBES, 'tube'))
    tube['type'] = 'stream'
    tube['id'] = '42'
    tube['service'] = 'foo-service'
    parameters = tube.addElement((None, 'parameters'))
    parameter = parameters.addElement((None, 'parameter'))
    parameter['type'] = 'str'
    parameter['name'] = 'foo'
    parameter.addContent('bar')

    stream.send (message)

    event = q.expect('dbus-signal', signal='NewTube')
    id = event.args[0]
    initiator = event.args[1]
    type = event.args[2]
    service = event.args[3]
    parameters = event.args[4]
    state = event.args[5]

    assert id == 42
    initiator_jid = conn.InspectHandles(1, [initiator])[0]
    assert initiator_jid == 'bob@localhost'
    assert type == 1 # Stream tube
    assert service == 'foo-service'
    assert parameters == {'foo': 'bar'}
    assert state == 0 # local pending

    # accept the tube
    ret = q.expect('dbus-signal', signal='NewChannel')
    tube_chan = bus.get_object(conn.bus_name, ret.args[0])
    tube_iface = dbus.Interface(tube_chan,
        tp_name_prefix + '.Channel.Type.StreamTube.DRAFT')

    path2 = tube_iface.AcceptStreamTube(0, 0, '')
    path2 = ''.join([chr(c) for c in path2])

    factory = EventProtocolClientFactory(q)
    reactor.connectUNIX(path2, factory)

    # Gabble needs a bytestream for the connection and sends a SI offer.
    event = q.expect('stream-iq', iq_type='set', to='bob@localhost/Bob')
    iq = event.stanza
    si_nodes = xpath.queryForNodes('/iq/si', iq)
    assert si_nodes is not None
    assert len(si_nodes) == 1
    si = si_nodes[0]
    assert si['profile'] == ns.TUBES
    dbus_stream_id = si['id']

    feature = xpath.queryForNodes('/si/feature', si)[0]
    x = xpath.queryForNodes('/feature/x', feature)[0]
    assert x['type'] == 'form'
    field = xpath.queryForNodes('/x/field', x)[0]
    assert field['var'] == 'stream-method'
    assert field['type'] == 'list-single'
    value = xpath.queryForNodes('/field/option/value', field)[0]
    assert str(value) == ns.BYTESTREAMS
    value = xpath.queryForNodes('/field/option/value', field)[1]
    assert str(value) == ns.IBB
    # Gabble supports multi-bytestreams extension
    si_multiple = xpath.queryForNodes('/si/si-multiple', si)[0]
    assert si_multiple.uri == ns.SI_MULTIPLE

    result = IQ(stream, 'result')
    result['id'] = iq['id']
    result['from'] = iq['to']
    result['to'] = 'test@localhost/Resource'
    res_si = result.addElement((ns.SI, 'si'))
    # reply using multi-bytestreams extension
    res_multi = res_si.addElement((ns.SI_MULTIPLE, 'si-multiple'))
    res_value = res_multi.addElement(('', 'value'))
    res_value.addContent('invalid-stream-method')
    res_value = res_multi.addElement(('', 'value'))
    res_value.addContent(ns.BYTESTREAMS)
    res_value = res_multi.addElement(('', 'value'))
    res_value.addContent(ns.IBB)

    stream.send(result)

    # Gabble first tries Sock5
    event = q.expect('stream-iq', iq_type='set', to='bob@localhost/Bob')
    iq = event.stanza
    query = xpath.queryForNodes('/iq/query', iq)[0]
    assert query.uri == ns.BYTESTREAMS
    sid = query['sid']
    streamhost = xpath.queryForNodes('/iq/query/streamhost', iq)[0]
    assert streamhost

    # pretend we can't connect using Sock5
    response_id = iq['id']
    iq = IQ(stream, 'error')
    iq['to'] = 'test@localhost/Resource'
    iq['from'] = 'bob@localhost/Bob'
    iq['id'] = response_id
    error = iq.addElement(('', 'error'))
    error['type'] = 'auth'
    error['code'] = '403'
    stream.send(iq)

    # Gabble now tries using IBB
    event = q.expect('stream-iq', iq_type='set', to='bob@localhost/Bob')
    iq = event.stanza
    open = xpath.queryForNodes('/iq/open', iq)[0]
    assert open.uri == ns.IBB
    sid = open['sid']

    # IBB is working
    result = IQ(stream, 'result')
    result['id'] = iq['id']
    result['from'] = iq['to']
    result['to'] = 'test@localhost/Resource'

    stream.send(result)

if __name__ == '__main__':
    exec_test(test)
