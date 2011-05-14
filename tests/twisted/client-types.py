"""
Test Conn.I.ClientTypes
"""
import random

from servicetest import EventPattern, assertEquals, assertLength, assertContains
from gabbletest import exec_test, make_presence, sync_stream
import constants as cs
import ns
from caps_helper import (
    presence_and_disco, send_presence, expect_disco, send_disco_reply,
)

client_base = 'http://telepathy.freedesktop.org/fake-client/client-types-'
caps_base = {
   'ver': '0.1'
   }
features = [
    ns.JINGLE_015,
    ns.JINGLE_015_AUDIO,
    ns.JINGLE_015_VIDEO,
    ns.GOOGLE_P2P,
    ]

# our identities
BOT = ['client/bot/en/doesnotcompute']
CONSOLE = ['client/console/en/dumb']
GAME = ['client/game/en/wiibox3']
HANDHELD = ['client/handheld/en/lolpaq']
PC = ['client/pc/en/Lolclient 0.L0L']
PHONE = ['client/phone/en/gr8phone 101']
WEB = ['client/web/en/webcat']
SMS = ['client/phone/en/tlk 2 u l8r']
TRANSIENT_PHONE = ['client/phone/en/fleeting visit']

def build_stuff(identities):
    types = map(lambda x: x.split('/')[1], identities)

    # add something to the end of the client string so that the caps
    # hashes aren't all the same and so stop discoing
    client = client_base + ''.join(types)
    caps = caps_base
    caps['node'] = client

    return (caps, client, types)

def contact_online(q, conn, stream, contact, identities,
                  disco=True, dataforms={}, initial=True, show=None):
    (caps, client, types) = build_stuff(identities)
    handle = conn.RequestHandles(cs.HT_CONTACT, [contact])[0]

    # make contact come online
    presence_and_disco (q, conn, stream, contact,
                        disco, client, caps, features, identities,
                        dataforms=dataforms, initial = initial,
                        show = show)

    if initial:
        event = q.expect('dbus-signal', signal='ClientTypesUpdated')
        assertEquals([handle, types], event.args)

def test(q, bus, conn, stream):
    # check all these types appear as they should
    contact_online(q, conn, stream, 'bot@bot.com/lol', BOT)
    contact_online(q, conn, stream, 'console@console.com/lol', CONSOLE)
    contact_online(q, conn, stream, 'game@game.com/lol', GAME)
    contact_online(q, conn, stream, 'handheld@handheld.com/lol', HANDHELD)
    contact_online(q, conn, stream, 'pc@pc.com/lol', PC)
    contact_online(q, conn, stream, 'phone@phone.com/lol', PHONE)
    contact_online(q, conn, stream, 'web@web.com/lol', WEB)
    contact_online(q, conn, stream, 'sms@sms.com/lol', SMS)

    meredith_one = 'meredith@foo.com/One'
    meredith_two = 'meredith@foo.com/Two'
    meredith_three = 'meredith@foo.com/Three'
    meredith_handle = conn.RequestHandles(cs.HT_CONTACT, [meredith_one])[0]

    # Meredith signs in from one resource
    contact_online(q, conn, stream, meredith_one, PC, show='chat')

    # Meredith signs in from another resource
    contact_online(q, conn, stream, meredith_two, PHONE, show='dnd', initial=False)

    # check we're still a PC
    types = conn.GetClientTypes([meredith_handle],
                                dbus_interface=cs.CONN_IFACE_CLIENT_TYPES)

    assertLength(1, types)
    assertLength(1, types[meredith_handle])
    assertEquals('pc', types[meredith_handle][0])

    # Two now becomes more available
    stream.send(make_presence(meredith_two, show='chat'))

    types = conn.GetClientTypes([meredith_handle],
                                dbus_interface=cs.CONN_IFACE_CLIENT_TYPES)
    assertEquals('pc', types[meredith_handle][0])

    # One now becomes less available
    stream.send(make_presence(meredith_one, show='away'))

    # wait for the presence change
    q.expect('dbus-signal', signal='PresencesChanged',
             args=[{meredith_handle: (cs.PRESENCE_AVAILABLE, 'chat', '')}])

    # now wait for the change in client type
    event = q.expect('dbus-signal', signal='ClientTypesUpdated')
    assertEquals([meredith_handle, ['phone']], event.args)

    # make One more available again
    stream.send(make_presence(meredith_one, show='chat', status='lawl'))

    # wait for the presence change
    q.expect('dbus-signal', signal='PresencesChanged',
             args=[{meredith_handle: (cs.PRESENCE_AVAILABLE, 'chat', 'lawl')}])

    # now wait for the change in client type
    event = q.expect('dbus-signal', signal='ClientTypesUpdated')
    assertEquals([meredith_handle, ['pc']], event.args)

    # both One and Two go away
    stream.send(make_presence(meredith_one, show='away'))
    stream.send(make_presence(meredith_two, show='away'))

    # wait for the presence change
    q.expect('dbus-signal', signal='PresencesChanged',
             args=[{meredith_handle: (cs.PRESENCE_AWAY, 'away', '')}])

    # check it still thinks we're a PC
    types = conn.GetClientTypes([meredith_handle],
                                dbus_interface=cs.CONN_IFACE_CLIENT_TYPES)
    assertEquals('pc', types[meredith_handle][0])

    # Three, with multiple identities, signs in
    identities = [PHONE[0], CONSOLE[0], HANDHELD[0], BOT[0]]
    contact_online(q, conn, stream, meredith_three, identities,
                   show='chat', initial=False)

    # wait for the presence change
    q.expect('dbus-signal', signal='PresencesChanged',
             args=[{meredith_handle: (cs.PRESENCE_AVAILABLE, 'chat', 'hello')}])

    # now wait for the change in client type
    event = q.expect('dbus-signal', signal='ClientTypesUpdated')
    assertEquals(meredith_handle, event.args[0])
    assertEquals(['bot', 'console', 'handheld', 'phone'], sorted(event.args[1]))

    # that'll do
    #
    # ...
    #
    # wait wait! no it won't! Here's a regression test for
    # <https://bugs.freedesktop.org/show_bug.cgi?id=31772>.
    (caps, client, types) = build_stuff(TRANSIENT_PHONE)
    contact = 'mini9@meegoconf.ie/hai'
    send_presence(q, conn, stream, contact, caps)
    stanza = expect_disco(q, contact, client, caps)
    stream.send(make_presence(contact, type='unavailable'))
    send_disco_reply(stream, stanza, TRANSIENT_PHONE, [])

    # Gabble used to crash upon receiving a disco reply from a contact who's no
    # longer in the presence cache. So we sync here to check if it's died.
    sync_stream(q, stream)

if __name__ == '__main__':
    exec_test(test)
