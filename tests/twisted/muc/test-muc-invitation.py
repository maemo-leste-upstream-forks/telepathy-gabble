"""
Test MUC invitations.
"""

import dbus

from twisted.words.xish import domish, xpath

from gabbletest import go, make_result_iq, exec_test
from servicetest import call_async, EventPattern

def test(q, bus, conn, stream):
    conn.Connect()

    q.expect('dbus-signal', signal='StatusChanged', args=[0, 1])

    # Bob has invited us to an activity.
    message = domish.Element((None, 'message'))
    message['from'] = 'chat@conf.localhost'
    message['to'] = 'test@localhost'
    x = message.addElement(('http://jabber.org/protocol/muc#user', 'x'))
    invite = x.addElement((None, 'invite'))
    invite['from'] = 'bob@localhost'
    reason = invite.addElement((None, 'reason'))
    reason.addContent('No good reason')

    stream.send(message)

    event = q.expect('dbus-signal', signal='NewChannel')
    assert event.args[1] == 'org.freedesktop.Telepathy.Channel.Type.Text'

    assert event.args[2] == 2   # handle type
    assert event.args[3] == 1   # handle
    room_handle = 1

    text_chan = bus.get_object(conn.bus_name, event.args[0])
    group_iface = dbus.Interface(text_chan,
        'org.freedesktop.Telepathy.Channel.Interface.Group')

    members = group_iface.GetMembers()
    local_pending = group_iface.GetLocalPendingMembers()
    remote_pending = group_iface.GetRemotePendingMembers()

    assert len(members) == 1
    assert conn.InspectHandles(1, members)[0] == 'bob@localhost'
    bob_handle = members[0]
    assert len(local_pending) == 1
    # FIXME: the username-part-is-nickname assumption
    assert conn.InspectHandles(1, local_pending)[0] == \
            'chat@conf.localhost/test'
    assert len(remote_pending) == 0

    room_self_handle = group_iface.GetSelfHandle()
    assert room_self_handle == local_pending[0]

    channel_props = text_chan.GetAll(
            'org.freedesktop.Telepathy.Channel',
            dbus_interface='org.freedesktop.DBus.Properties')
    assert channel_props['TargetID'] == 'chat@conf.localhost', channel_props

    # Exercise FUTURE properties
    future_props = text_chan.GetAll(
            'org.freedesktop.Telepathy.Channel.FUTURE',
            dbus_interface='org.freedesktop.DBus.Properties')
    assert future_props['Requested'] == False
    assert future_props['InitiatorID'] == 'bob@localhost'
    assert future_props['InitiatorHandle'] == bob_handle

    # accept the invitation
    call_async(q, group_iface, 'AddMembers', [room_self_handle], 'Oh, OK then')

    _, event, _ = q.expect_many(
            EventPattern('stream-presence', to='chat@conf.localhost/test'),
            EventPattern('dbus-signal', signal='MembersChanged'),
            EventPattern('dbus-return', method='AddMembers')
            )

    assert event.args == ['', [], [bob_handle], [],
            [room_self_handle], 0, room_self_handle]

    # Send presence for own membership of room.
    presence = domish.Element((None, 'presence'))
    presence['from'] = 'chat@conf.localhost/test'
    x = presence.addElement(('http://jabber.org/protocol/muc#user', 'x'))
    item = x.addElement('item')
    item['affiliation'] = 'owner'
    item['role'] = 'moderator'
    stream.send(presence)

    event = q.expect('dbus-signal', signal='MembersChanged')
    assert event.args == ['', [room_self_handle], [], [], [], 0, 0]

    # Test sending an invitation
    alice_handle = conn.RequestHandles(1, ['alice@localhost'])[0]
    call_async(q, group_iface, 'AddMembers', [alice_handle],
            'I want to test invitations')

    event = q.expect('stream-message', to='chat@conf.localhost')
    message = event.stanza

    x = xpath.queryForNodes('/message/x', message)
    assert (x is not None and len(x) == 1), repr(x)
    assert x[0].uri == 'http://jabber.org/protocol/muc#user'

    invites = xpath.queryForNodes('/x/invite', x[0])
    assert (invites is not None and len(invites) == 1), repr(invites)
    assert invites[0]['to'] == 'alice@localhost'

    reasons = xpath.queryForNodes('/invite/reason', invites[0])
    assert (reasons is not None and len(reasons) == 1), repr(reasons)
    assert str(reasons[0]) == 'I want to test invitations'

    conn.Disconnect()

    q.expect('dbus-signal', signal='StatusChanged', args=[2, 1])

if __name__ == '__main__':
    exec_test(test)
