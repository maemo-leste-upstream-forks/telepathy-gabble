"""
Test Channel.Interface.Subject on MUC channels
"""

import dbus

from twisted.words.xish import domish

from gabbletest import exec_test, make_result_iq, make_muc_presence
from servicetest import (EventPattern, assertEquals, assertLength,
        assertContains, call_async)
import constants as cs
import ns

from mucutil import join_muc

def test(q, bus, conn, stream):
    # 3x2x2 possible combinations of change_subject, send_first, moderator:
    # unrolling the loop here so we'll get better Python tracebacks on failure
    test_subject(q, bus, conn, stream, None, False, False)
    test_subject(q, bus, conn, stream, None, False, True)
    test_subject(q, bus, conn, stream, None, True, False)
    test_subject(q, bus, conn, stream, None, False, True)
    test_subject(q, bus, conn, stream, True, False, False)
    test_subject(q, bus, conn, stream, True, False, True)
    test_subject(q, bus, conn, stream, True, True, False)
    test_subject(q, bus, conn, stream, True, False, True)
    test_subject(q, bus, conn, stream, False, False, False)
    test_subject(q, bus, conn, stream, False, False, True)
    test_subject(q, bus, conn, stream, False, True, False)
    test_subject(q, bus, conn, stream, False, False, True)

def check_subject_props(chan, subject_str, actor, flags, signal=None):
    if signal is not None:
        assertEquals(subject_str, signal.args[0])
        assertEquals(actor, signal.args[1])
        assertEquals(flags, signal.args[3])

    props = chan.GetAll(cs.CHANNEL_IFACE_SUBJECT,
                        dbus_interface=dbus.PROPERTIES_IFACE)
    subject = props['Subject']
    subject_actor = props['Actor']
    subject_can_set = props['CanSet']

def test_subject(q, bus, conn, stream, change_subject, send_first,
        moderator):
    room = 'test@conf.localhost'

    room_handle, chan, path, props, disco = join_muc(q, bus, conn, stream,
            room,
            also_capture=[EventPattern('stream-iq', iq_type='get',
                query_name='query', query_ns=ns.DISCO_INFO, to=room)],
            role=(moderator and 'moderator' or 'participant'))

    assert chan.Properties.Get(cs.CHANNEL_IFACE_SUBJECT, "CanSet")

    if send_first:
        # Someone sets a subject.
        message = domish.Element((None, 'message'))
        message['from'] = room + '/bob'
        message['type'] = 'groupchat'
        message.addElement('subject', content='Testing')
        stream.send(message)

        q.expect('dbus-signal', interface=cs.PROPERTIES_IFACE,
                 signal='PropertiesChanged',
                 predicate=lambda e: e.args[0] == cs.CHANNEL_IFACE_SUBJECT)
        check_subject_props(chan, 'Testing', room + '/bob', True)

    # Reply to the disco
    iq = make_result_iq(stream, disco.stanza)
    query = iq.firstChildElement()
    feat = query.addElement('feature')
    feat['var'] = 'muc_public'

    x = query.addElement((ns.X_DATA, 'x'))
    x['type'] = 'result'

    if change_subject is not None:
        # When fd.o #13157 has been fixed, this will actually do something.
        field = x.addElement('field')
        field['var'] = 'muc#roomconfig_changesubject'
        field.addElement('value',
                content=(change_subject and 'true' or 'false'))

    stream.send(iq)

    # Someone sets a subject.
    message = domish.Element((None, 'message'))
    message['from'] = room + '/bob'
    message['type'] = 'groupchat'
    message.addElement('subject', content='lalala')
    stream.send(message)

    q.expect('dbus-signal', interface=cs.PROPERTIES_IFACE,
             signal='PropertiesChanged',
             predicate=lambda e: e.args[0] == cs.CHANNEL_IFACE_SUBJECT)
    check_subject_props(chan, 'lalala', room + '/bob', True)

    # test changing the subject
    call_async(q, chan, 'SetSubject', 'le lolz', dbus_interface=cs.CHANNEL_IFACE_SUBJECT)

    e = q.expect('stream-message', to=room)
    elem = e.stanza
    assertEquals('groupchat', elem['type'])
    assertEquals(1, len(elem.children))
    assertEquals(elem.children[0].name, 'subject')
    assertEquals(str(elem.children[0]), 'le lolz')

    elem['from'] = room + '/test'
    stream.send(elem)

    q.expect_many(EventPattern('dbus-signal', interface=cs.PROPERTIES_IFACE,
                               signal='PropertiesChanged',
                               predicate=lambda e: e.args[0] == cs.CHANNEL_IFACE_SUBJECT),
                  EventPattern('dbus-return', method='SetSubject'),
                 )

    check_subject_props(chan, 'le lolz', room + '/test', True)

    # Test changing the subject and getting an error back.
    call_async(q, chan, 'SetSubject', 'CHICKEN MAN', dbus_interface=cs.CHANNEL_IFACE_SUBJECT)

    e = q.expect('stream-message', to=room)
    elem = e.stanza
    elem['from'] = room
    elem['type'] = 'error'
    error = elem.addElement((None, 'error'))
    error['type'] = 'auth'
    error.addElement((ns.STANZA, 'forbidden'))
    stream.send(elem)
    q.expect('dbus-error', method='SetSubject', name=cs.PERMISSION_DENIED)

    # Test changing the subject and getting an error back which doesn't echo
    # the <subject> element.
    call_async(q, chan, 'SetSubject', 'CHICKEN MAN', dbus_interface=cs.CHANNEL_IFACE_SUBJECT)

    e = q.expect('stream-message', to=room)
    message = domish.Element((None, 'message'))
    message['from'] = room
    message['id'] = e.stanza['id']
    message['type'] = 'error'
    error = message.addElement((None, 'error'))
    error.addElement((ns.STANZA, 'forbidden'))
    stream.send(message)

    q.expect('dbus-error', method='SetSubject', name=cs.PERMISSION_DENIED)

    # Test changing the subject just before we leave the room (and hence not
    # getting a reply). While we're here, check that you can't have more than
    # one call in flight at a time.
    call_async(q, chan, 'SetSubject', 'le lolz', dbus_interface=cs.CHANNEL_IFACE_SUBJECT)
    e = q.expect('stream-message', to=room)

    call_async(q, chan, 'SetSubject', 'le lolz', dbus_interface=cs.CHANNEL_IFACE_SUBJECT)
    q.expect('dbus-error', method='SetSubject', name=cs.NOT_AVAILABLE)

    chan.Close()

    event = q.expect('stream-presence', to=room + '/test')
    elem = event.stanza
    assertEquals('unavailable', elem['type'])

    q.expect('dbus-error', method='SetSubject', name=cs.CANCELLED)

    call_async(q, chan, 'SetSubject', 'how about now?',
        dbus_interface=cs.CHANNEL_IFACE_SUBJECT)
    q.expect('dbus-error', method='SetSubject', name=cs.NOT_AVAILABLE)

    # The MUC confirms that we've left the room.
    echo = make_muc_presence('member', 'none', room, 'test')
    echo['type'] = 'unavailable'
    stream.send(echo)
    q.expect('dbus-signal', signal='ChannelClosed')

if __name__ == '__main__':
    exec_test(test)
