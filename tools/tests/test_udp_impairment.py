"""Exercise bounded real UDP forwarding and selective impairment without a device."""
from pathlib import Path
import socket
import struct
import sys
import time
import unittest
sys.path.insert(0,str(Path(__file__).resolve().parents[1]))
from udp_impairment import UdpImpairment, application_data


def packet(sequence=1, kind=23):
    payload=bytes([kind])+b'\xfe\xfd'+b'\0'*8+b'\0\1'+bytes([sequence])
    return struct.pack('!HH',0x4001,len(payload))+payload


def indication(payload):
    body=struct.pack('!HH',0x0013,len(payload))+payload+b'\0'*((-len(payload))%4)
    return struct.pack('!HH',0x0016,len(body))+b'\x21\x12\xa4\x42'+b'\0'*12+body


class UdpImpairmentTests(unittest.TestCase):
    def test_only_complete_turn_wrapped_dtls_application_data_is_eligible(self):
        self.assertTrue(application_data(packet()))
        self.assertTrue(application_data(indication(packet()[4:])))
        for item in [b'',packet()[:3],packet()[:-1],packet()[4:],packet(kind=22),packet(kind=20),
                     indication(packet(kind=22)[4:]),indication(b'\0'*20),b'\0'*200]:
            self.assertFalse(application_data(item))
        malformed=bytearray(indication(packet()[4:]));malformed[22:24]=b'\xff\xff'
        self.assertFalse(application_data(malformed))

    def test_dtls13_unified_headers_select_only_application_epoch(self):
        for flags in [0x23,0x27,0x2b,0x2f]:
            payload=bytes([flags])+b'\0'*(2 if flags & 8 else 1)+(b'\0\x10' if flags & 4 else b'')+b'x'*16
            wrapped=struct.pack('!HH',0x4001,len(payload))+payload
            self.assertTrue(application_data(wrapped));self.assertTrue(application_data(indication(payload)))
            for bad in [payload[:-1],bytes([flags-1])+payload[1:],bytes([flags|16])+payload[1:]]:
                self.assertFalse(application_data(struct.pack('!HH',0x4001,len(bad))+bad))

    def test_pair_reordering_loss_handshake_passthrough_and_blackout(self):
        with socket.socket(socket.AF_INET,socket.SOCK_DGRAM) as server, socket.socket(socket.AF_INET,socket.SOCK_DGRAM) as client:
            server.bind(('127.0.0.1',0));server.settimeout(1);client.settimeout(1)
            with UdpImpairment('127.0.0.1',server.getsockname()[1]) as fixture:
                target=('127.0.0.1',fixture.port)
                fixture.set_mode('loss-reorder')
                for n in range(1,8):client.sendto(packet(n),target)
                received=[server.recvfrom(65536) for _ in range(6)]
                self.assertEqual([p[-1] for p,_ in received],[2,1,4,3,6,5])
                server.settimeout(.2)
                with self.assertRaises(socket.timeout):server.recvfrom(65536)
                fixture.set_mode('blackout')
                client.sendto(packet(8),target);client.sendto(packet(9,22),target)
                data,source=server.recvfrom(65536);self.assertEqual(data,packet(9,22))
                server.sendto(packet(10),source);server.sendto(packet(11,22),source)
                self.assertEqual(client.recvfrom(65536)[0],packet(11,22))
                fixture.set_mode('pass');client.sendto(packet(12),target)
                data,source=server.recvfrom(65536);server.sendto(data,source)
                self.assertEqual(client.recvfrom(65536)[0],packet(12))
                r=fixture.report();self.assertEqual(r['directions']['outbound']['reordered'],3)
                self.assertEqual(r['directions']['outbound']['dropped'],2)
                self.assertEqual(r['directions']['inbound']['dropped'],1)
                self.assertLessEqual(r['peak_held_packets'],1)
            self.assertTrue(fixture.closed);self.assertFalse(fixture.thread.is_alive());self.assertEqual(fixture.report()['pending_packets'],0)

    def test_single_delayed_packet_flushes_and_flow_limit_stops_fixture(self):
        with socket.socket(socket.AF_INET,socket.SOCK_DGRAM) as server:
            server.bind(('127.0.0.1',0));server.settimeout(1)
            clients=[]
            try:
                with UdpImpairment('127.0.0.1',server.getsockname()[1]) as fixture:
                    fixture.set_mode('loss-reorder')
                    for n in range(9):
                        client=socket.socket(socket.AF_INET,socket.SOCK_DGRAM);clients.append(client)
                        client.sendto(packet(n),('127.0.0.1',fixture.port))
                        if n==0:self.assertEqual(server.recvfrom(65536)[0],packet(0))
                    fixture.thread.join(2)
                    self.assertEqual(fixture.error,'UDP fixture flow limit exceeded.')
                    self.assertEqual(fixture.report()['flows'],8)
                    self.assertLessEqual(fixture.report()['peak_held_packets'],16)
            finally:
                for client in clients:client.close()


if __name__=='__main__':unittest.main()
