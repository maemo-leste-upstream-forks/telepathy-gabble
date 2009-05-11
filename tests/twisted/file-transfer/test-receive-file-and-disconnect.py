
from file_transfer_helper import exec_file_transfer_test, ReceiveFileTest

class ReceiveFileAndDisconnectTest(ReceiveFileTest):
    def receive_file(self):
        s = self.create_socket()
        s.connect(self.address)

        # disconnect
        self.conn.Disconnect()
        self.q.expect('dbus-signal', signal='StatusChanged', args=[2, 1])
        return True

if __name__ == '__main__':
    exec_file_transfer_test(ReceiveFileAndDisconnectTest)
