"""Bounded TURN/UDP impairment for this repository's local packaged probes.

Only DTLS application-data epochs inside TURN ChannelData or DATA attributes
are impaired. ICE, TURN control, and DTLS handshake packets pass unchanged.
No system network settings are changed and no packet contents are recorded.
"""
import ipaddress
import selectors
import socket
import struct
import threading
import time


def application_data(packet):
    if len(packet) < 4:
        return False
    kind, size = struct.unpack_from('!HH', packet)
    if 0x4000 <= kind <= 0x7fff:
        payload = packet[4:4+size] if len(packet) >= size+4 else b''
    elif kind in (0x0016, 0x0017) and len(packet) >= 20 and packet[4:8] == b'\x21\x12\xa4\x42' and size % 4 == 0 and len(packet) >= 20+size:
        payload = b''
        offset = 20
        while offset+4 <= 20+size:
            attribute, length = struct.unpack_from('!HH', packet, offset)
            offset += 4
            if offset+length > 20+size:
                return False
            if attribute == 0x0013:
                payload = packet[offset:offset+length]
            offset += (length+3) & ~3
    else:
        return False
    # RFC 9147 unified header, matching the pinned BoringSSL implementation.
    # This short test uses epoch 3 application keys, with no CID or key update.
    # The inner content type is encrypted; arm impairment only after DCEP opens.
    if payload and payload[0] & 0xe0 == 0x20:
        flags = payload[0]
        if flags & 0x10 or flags & 3 != 3:
            return False
        offset = 1 + (2 if flags & 8 else 1)
        if flags & 4:
            if len(payload) < offset+2: return False
            size = int.from_bytes(payload[offset:offset+2], 'big'); offset += 2
            return size >= 16 and len(payload) >= offset+size
        return len(payload) >= offset+16
    # DTLS 1.0/1.2 record header: content type, version, epoch/sequence, length.
    return len(payload) >= 13 and payload[0] == 23 and payload[1:3] in (b'\xfe\xff', b'\xfe\xfd') and 13+int.from_bytes(payload[11:13], 'big') <= len(payload)


class UdpImpairment:
    MAX_FLOWS = 8
    MAX_DELAY = .12

    def __init__(self, listen_ip, upstream_port):
        address = ipaddress.ip_address(listen_ip)
        if address.version != 4 or not (address.is_private or address.is_loopback):
            raise ValueError('The impairment fixture requires an explicit private local IPv4 address.')
        self.upstream = (listen_ip, upstream_port)
        self.front = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
        self.front.bind((listen_ip, 0)); self.front.setblocking(False)
        self.port = self.front.getsockname()[1]
        self.selector = selectors.DefaultSelector(); self.selector.register(self.front, selectors.EVENT_READ, None)
        self.flows = {}; self.held = {}; self.lock = threading.Lock(); self.stop = threading.Event()
        self.mode = 'pass'; self.error = ''; self.closed = False
        self.stats = {direction:dict(packets=0, eligible=0, impaired=0, dropped=0, reordered=0, forwarded=0) for direction in ('outbound', 'inbound')}
        self.peak_held = 0; self.transitions = []
        self.thread = threading.Thread(target=self._run, name='SimurghRTC-UDP-fixture', daemon=True)

    def __enter__(self):
        self.thread.start(); return self

    def _forward(self, sock, destination, packet, direction):
        if destination is None: sock.send(packet)
        else: sock.sendto(packet, destination)
        self.stats[direction]['forwarded'] += 1

    def set_mode(self, mode):
        if mode not in ('pass', 'loss-reorder', 'blackout'):
            raise ValueError('Unsupported impairment mode.')
        with self.lock:
            # A transition releases delayed packets before changing the impairment.
            for _, sock, destination, packet, direction in self.held.values():
                self._forward(sock, destination, packet, direction)
            self.held.clear(); self.mode = mode
            self.transitions.append(dict(mode=mode, at=time.monotonic()))

    def _packet(self, packet, flow, direction, sock, destination):
        counters = self.stats[direction]; counters['packets'] += 1
        eligible = application_data(packet)
        if eligible: counters['eligible'] += 1
        if not eligible or self.mode == 'pass':
            self._forward(sock, destination, packet, direction); return
        counters['impaired'] += 1
        if self.mode == 'blackout' or counters['impaired'] % 7 == 0:
            counters['dropped'] += 1; return
        key = (flow, direction)
        if key in self.held:
            self._forward(sock, destination, packet, direction)
            _, old_socket, old_destination, old_packet, old_direction = self.held.pop(key)
            self._forward(old_socket, old_destination, old_packet, old_direction)
            counters['reordered'] += 1
        else:
            self.held[key] = (time.monotonic()+self.MAX_DELAY, sock, destination, packet, direction)
            self.peak_held = max(self.peak_held, len(self.held))

    def _run(self):
        try:
            while not self.stop.is_set():
                for key, _ in self.selector.select(.02):
                    with self.lock:
                        if key.fileobj is self.front:
                            packet, source = self.front.recvfrom(65536)
                            if source not in self.flows:
                                if len(self.flows) >= self.MAX_FLOWS:
                                    raise ValueError('UDP fixture flow limit exceeded.')
                                upstream = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
                                upstream.connect(self.upstream); upstream.setblocking(False)
                                self.flows[source] = upstream
                                self.selector.register(upstream, selectors.EVENT_READ, source)
                            self._packet(packet, source, 'outbound', self.flows[source], None)
                        else:
                            packet = key.fileobj.recv(65536)
                            self._packet(packet, key.data, 'inbound', self.front, key.data)
                with self.lock:
                    for key, item in list(self.held.items()):
                        deadline, sock, destination, packet, direction = item
                        if deadline <= time.monotonic():
                            self._forward(sock, destination, packet, direction); del self.held[key]
        except (OSError, ValueError) as error:
            self.error = str(error); self.stop.set()

    def report(self):
        with self.lock:
            return dict(mode=self.mode, directions={k:dict(v) for k,v in self.stats.items()}, flows=len(self.flows),
                        peak_held_packets=self.peak_held, pending_packets=len(self.held), max_flows=self.MAX_FLOWS,
                        max_delay_seconds=self.MAX_DELAY, transitions=list(self.transitions), error=self.error, closed=self.closed)

    def __exit__(self, *args):
        self.stop.set(); self.thread.join(timeout=2)
        if self.thread.is_alive(): raise RuntimeError('UDP fixture thread did not stop.')
        self.selector.close(); self.front.close()
        for sock in self.flows.values(): sock.close()
        self.held.clear(); self.closed = True
