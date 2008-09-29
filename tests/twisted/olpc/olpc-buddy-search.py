"""
test OLPC search buddy
"""

import dbus

from servicetest import call_async, EventPattern
from gabbletest import exec_test, make_result_iq, acknowledge_iq

from twisted.words.xish import domish, xpath
from twisted.words.protocols.jabber.client import IQ

from util import (announce_gadget, properties_to_xml, parse_properties,
    create_gadget_message, close_view)

NS_OLPC_BUDDY_PROPS = "http://laptop.org/xmpp/buddy-properties"
NS_OLPC_ACTIVITIES = "http://laptop.org/xmpp/activities"
NS_OLPC_CURRENT_ACTIVITY = "http://laptop.org/xmpp/current-activity"
NS_OLPC_ACTIVITY_PROPS = "http://laptop.org/xmpp/activity-properties"
NS_OLPC_BUDDY = "http://laptop.org/xmpp/buddy"
NS_OLPC_ACTIVITY = "http://laptop.org/xmpp/activity"

NS_PUBSUB = "http://jabber.org/protocol/pubsub"
NS_DISCO_INFO = "http://jabber.org/protocol/disco#info"
NS_DISCO_ITEMS = "http://jabber.org/protocol/disco#items"


NS_AMP = "http://jabber.org/protocol/amp"

def test(q, bus, conn, stream):
    conn.Connect()

    _, iq_event, disco_event = q.expect_many(
        EventPattern('dbus-signal', signal='StatusChanged', args=[0, 1]),
        EventPattern('stream-iq', to=None, query_ns='vcard-temp',
            query_name='vCard'),
        EventPattern('stream-iq', to='localhost', query_ns=NS_DISCO_ITEMS))

    acknowledge_iq(stream, iq_event.stanza)
    announce_gadget(q, stream, disco_event.stanza)

    buddy_info_iface = dbus.Interface(conn, 'org.laptop.Telepathy.BuddyInfo')
    gadget_iface = dbus.Interface(conn, 'org.laptop.Telepathy.Gadget')

    call_async(q, conn, 'RequestHandles', 1, ['bob@localhost'])

    event = q.expect('dbus-return', method='RequestHandles')
    handles = event.value[0]
    bob_handle = handles[0]

    call_async(q, buddy_info_iface, 'GetProperties', bob_handle)

    # wait for pubsub query
    event = q.expect('stream-iq', to='bob@localhost', query_ns=NS_PUBSUB)
    query = event.stanza
    assert query['to'] == 'bob@localhost'

    # send an error as reply
    reply = IQ(stream, 'error')
    reply['id'] = query['id']
    reply['to'] = 'alice@localhost'
    reply['from'] = 'bob@localhost'
    stream.send(reply)

    # wait for buddy search query
    event = q.expect('stream-iq', to='gadget.localhost',
            query_ns=NS_OLPC_BUDDY)
    buddies = xpath.queryForNodes('/iq/query/buddy', event.stanza)
    assert len(buddies) == 1
    buddy = buddies[0]
    assert buddy['jid'] == 'bob@localhost'

    # send reply to the search query
    reply = make_result_iq(stream, event.stanza)
    reply['from'] = 'gadget.localhost'
    reply['to'] = 'alice@localhost'
    query = xpath.queryForNodes('/iq/query', reply)[0]
    buddy = query.addElement((None, "buddy"))
    buddy['jid'] = 'bob@localhost'
    properties = buddy.addElement((NS_OLPC_BUDDY_PROPS, "properties"))
    for node in properties_to_xml({'color': ('str', '#005FE4,#00A0FF')}):
        properties.addChild(node)
    stream.send(reply)

    event = q.expect('dbus-return', method='GetProperties')
    props = event.value[0]

    assert props == {'color': '#005FE4,#00A0FF' }

    # request 3 random buddies
    call_async(q, gadget_iface, 'RequestRandomBuddies', 3)

    iq_event, return_event = q.expect_many(
        EventPattern('stream-iq', to='gadget.localhost',
            query_ns=NS_OLPC_BUDDY),
        EventPattern('dbus-return', method='RequestRandomBuddies'))

    view = iq_event.stanza.firstChildElement()
    assert view.name == 'view'
    assert view['id'] == '0'
    random = xpath.queryForNodes('/iq/view/random', iq_event.stanza)
    assert len(random) == 1
    assert random[0]['max'] == '3'

    # reply to random query
    reply = make_result_iq(stream, iq_event.stanza)
    reply['from'] = 'gadget.localhost'
    reply['to'] = 'alice@localhost'
    view = xpath.queryForNodes('/iq/view', reply)[0]
    buddy = view.addElement((None, "buddy"))
    buddy['jid'] = 'charles@localhost'
    properties = buddy.addElement((NS_OLPC_BUDDY_PROPS, "properties"))
    for node in properties_to_xml({'color': ('str', '#AAAAAA,#BBBBBB')}):
        properties.addChild(node)
    buddy = view.addElement((None, "buddy"))
    buddy['jid'] = 'bob@localhost'
    properties = buddy.addElement((NS_OLPC_BUDDY_PROPS, "properties"))
    for node in properties_to_xml({'color': ('str', '#005FE4,#00A0FF')}):
        properties.addChild(node)
    stream.send(reply)

    view_path = return_event.value[0]
    view0 = bus.get_object(conn.bus_name, view_path)
    view0_iface = dbus.Interface(view0, 'org.laptop.Telepathy.View')

    event = q.expect('dbus-signal', signal='BuddiesChanged')
    added, removed = event.args
    assert removed == []
    assert len(added) == 2
    assert sorted(conn.InspectHandles(1, added)) == ['bob@localhost',
            'charles@localhost']

    event = q.expect('dbus-signal', signal='PropertiesChanged')
    event = q.expect('dbus-signal', signal='PropertiesChanged')

    # we can now get bob's properties
    bob_handle = conn.RequestHandles(1, ['bob@localhost'])[0]
    props = buddy_info_iface.GetProperties(bob_handle)
    assert props == {'color': '#005FE4,#00A0FF'}

    # Bob changed his properties
    message = create_gadget_message("test@localhost")

    change = message.addElement((NS_OLPC_BUDDY, 'change'))
    change['jid'] = 'bob@localhost'
    change['id'] = '0'
    properties = change.addElement((NS_OLPC_BUDDY_PROPS, 'properties'))
    for node in properties_to_xml({'color': ('str', '#FFFFFF,#AAAAAA')}):
        properties.addChild(node)

    stream.send(message)

    event = q.expect('dbus-signal', signal='PropertiesChanged',
            args=[bob_handle, {'color': '#FFFFFF,#AAAAAA'}])

    # we now get the new properties
    props = buddy_info_iface.GetProperties(bob_handle)
    assert props == {'color': '#FFFFFF,#AAAAAA'}

    # buddy search
    props = {'color': '#AABBCC,#001122'}
    call_async(q, gadget_iface, 'SearchBuddiesByProperties', props)

    iq_event, return_event = q.expect_many(
        EventPattern('stream-iq', to='gadget.localhost', query_ns=NS_OLPC_BUDDY),
        EventPattern('dbus-return', method='SearchBuddiesByProperties'))

    properties_node = xpath.queryForNodes('/iq/view/buddy/properties',
            iq_event.stanza)
    props = parse_properties(properties_node[0])
    assert props == {'color': ('str', '#AABBCC,#001122')}

    view = iq_event.stanza.firstChildElement()
    assert view.name == 'view'
    assert view['id'] == '1'

    # reply to request
    reply = make_result_iq(stream, iq_event.stanza)
    reply['from'] = 'gadget.localhost'
    reply['to'] = 'alice@localhost'
    view = xpath.queryForNodes('/iq/view', reply)[0]
    buddy = view.addElement((None, "buddy"))
    buddy['jid'] = 'charles@localhost'
    properties = buddy.addElement((NS_OLPC_BUDDY_PROPS, "properties"))
    for node in properties_to_xml({'color': ('str', '#AABBCC,#001122')}):
        properties.addChild(node)
    stream.send(reply)

    view_path = return_event.value[0]
    view1 = bus.get_object(conn.bus_name, view_path)
    view1_iface = dbus.Interface(view1, 'org.laptop.Telepathy.View')

    event = q.expect('dbus-signal', signal='BuddiesChanged')
    added, removed = event.args
    assert removed == []
    assert len(added) == 1
    handle = added[0]
    assert conn.InspectHandles(1, [handle])[0] == 'charles@localhost'

    event = q.expect('dbus-signal', signal='PropertiesChanged')
    handle, props = event.args
    assert conn.InspectHandles(1, [handle])[0] == 'charles@localhost'
    assert props == {'color': '#AABBCC,#001122'}

    # add a buddy to view 0
    message = create_gadget_message("test@localhost")

    added = message.addElement((NS_OLPC_BUDDY, 'added'))
    added['id'] = '0'
    buddy = added.addElement((None, 'buddy'))
    buddy['jid'] = 'oscar@localhost'
    properties = buddy.addElement((NS_OLPC_BUDDY_PROPS, "properties"))
    for node in properties_to_xml({'color': ('str', '#000000,#AAAAAA')}):
        properties.addChild(node)

    stream.send(message)

    event = q.expect('dbus-signal', signal='BuddiesChanged')
    added, removed = event.args
    assert removed == []
    assert len(added) == 1
    handle = added[0]
    assert conn.InspectHandles(1, added)[0] == 'oscar@localhost'

    members = view0_iface.GetBuddies()
    members = sorted(conn.InspectHandles(1, members))
    assert sorted(members) == ['bob@localhost', 'charles@localhost',
            'oscar@localhost']

    # remove a buddy from view 0
    message = create_gadget_message("test@localhost")

    added = message.addElement((NS_OLPC_BUDDY, 'removed'))
    added['id'] = '0'
    buddy = added.addElement((None, 'buddy'))
    buddy['jid'] = 'bob@localhost'

    stream.send(message)

    event = q.expect('dbus-signal', signal='BuddiesChanged')
    added, removed = event.args
    assert added == []
    assert len(removed) == 1
    handle = removed[0]
    assert conn.InspectHandles(1, [handle])[0] == 'bob@localhost'

    members = view0_iface.GetBuddies()
    members = sorted(conn.InspectHandles(1, members))
    assert sorted(members) == ['charles@localhost', 'oscar@localhost']

    # test alias search
    call_async(q, gadget_iface, 'SearchBuddiesByAlias', "tom")

    iq_event, return_event = q.expect_many(
        EventPattern('stream-iq', to='gadget.localhost',
            query_ns=NS_OLPC_BUDDY),
        EventPattern('dbus-return', method='SearchBuddiesByAlias'))

    view = iq_event.stanza.firstChildElement()
    assert view.name == 'view'
    assert view['id'] == '2'
    buddy = xpath.queryForNodes('/iq/view/buddy', iq_event.stanza)
    assert len(buddy) == 1
    assert buddy[0]['alias'] == 'tom'

    # reply to random query
    reply = make_result_iq(stream, iq_event.stanza)
    reply['from'] = 'gadget.localhost'
    reply['to'] = 'alice@localhost'
    view = xpath.queryForNodes('/iq/view', reply)[0]
    buddy = view.addElement((None, "buddy"))
    buddy['jid'] = 'tom@localhost'
    buddy = view.addElement((None, "buddy"))
    buddy['jid'] = 'thomas@localhost'
    stream.send(reply)

    view_path = return_event.value[0]
    view2 = bus.get_object(conn.bus_name, view_path)
    view2_iface = dbus.Interface(view2, 'org.laptop.Telepathy.View')

    event = q.expect('dbus-signal', signal='BuddiesChanged')
    added, removed = event.args
    assert removed == []
    assert len(added) == 2
    assert sorted(conn.InspectHandles(1, added)) == ['thomas@localhost',
            'tom@localhost']

    # close view 0
    close_view(q, view0_iface, '0')

    # close view 1
    close_view(q, view1_iface, '1')

    # close view 2
    close_view(q, view2_iface, '2')

if __name__ == '__main__':
    exec_test(test)
