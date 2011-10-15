"""
Test dataforms
"""

from servicetest import sync_dbus, assertEquals
from gabbletest import exec_test, sync_stream
import ns
import constants as cs
from caps_helper import receive_presence_and_ask_caps

def test(q, bus, conn, stream):
    q.expect('stream-presence')

    # gabble won't try to represent the client if doesn't have any
    # RCCs or HCTs, so let's add some
    conn.ContactCapabilities.UpdateCapabilities(
        [
            ('dataformtest', [], ['banan', 'hi'])
        ])

    _, _, forms, _ = receive_presence_and_ask_caps(q, stream)

    assertEquals({'gabble:test:channel:manager:data:form':
                      {'cheese': ['omgnothorriblecheese'],
                       'running_out_of': ['ideas', 'cake'],
                       'favourite_crane': ['a tall one', 'a short one'],
                       'animal': ['badger', 'snake', 'weasel']
                       }
                  }, forms)

if __name__ == '__main__':
    exec_test(test)
