"""Small IPv4 UDP TURN fixture client (RFC 8489/8656), not an application transport."""
from __future__ import annotations
import base64
import hashlib
import hmac
import os
import socket
import struct
import time
import zlib

COOKIE = 0x2112A442
USERNAME, INTEGRITY, ERROR, LIFETIME = 0x0006, 0x0008, 0x0009, 0x000D
PEER, DATA, REALM, NONCE, RELAY, TRANSPORT = 0x0012, 0x0013, 0x0014, 0x0015, 0x0016, 0x0019
FINGERPRINT = 0x8028


def attribute(kind, value):
    return struct.pack('!HH', kind, len(value)) + value + bytes((-len(value)) % 4)


def packet(kind, attrs, transaction=None, key=None):
    transaction = transaction or os.urandom(12)
    if len(transaction) != 12:
        raise ValueError('Invalid STUN transaction size')
    body = b''.join(attribute(k, v) for k, v in attrs)
    header = struct.pack('!HHI12s', kind, len(body) + (24 if key else 0), COOKIE, transaction)
    if key:
        body += attribute(INTEGRITY, hmac.digest(key, header + body, 'sha1'))
    return header + body


def parse(data):
    if len(data) < 20:
        raise ValueError('Truncated STUN header')
    kind, length, cookie, transaction = struct.unpack('!HHI12s', data[:20])
    if kind & 0xC000 or cookie != COOKIE or length % 4 or len(data) != length + 20:
        raise ValueError('Invalid STUN framing')
    attrs, offset = [], 20
    while offset < len(data):
        attr, count = struct.unpack('!HH', data[offset:offset + 4])
        end = offset + 4 + count
        padded = end + (-count) % 4
        if padded > len(data):
            raise ValueError('Truncated STUN attribute')
        attrs.append((attr, data[offset + 4:end], offset))
        if attr == FINGERPRINT:
            if count != 4 or padded != len(data) or struct.unpack('!I', data[offset + 4:end])[0] != (zlib.crc32(data[:offset]) ^ 0x5354554E):
                raise ValueError('Invalid STUN fingerprint')
        offset = padded
    return kind, transaction, attrs


def one(attrs, kind):
    matches = [value for attr, value, _ in attrs if attr == kind]
    if len(matches) != 1:
        raise ValueError('Missing or duplicate STUN attribute')
    return matches[0]


def verify_integrity(data, key):
    _, _, attrs = parse(data)
    matches = [(value, offset) for kind, value, offset in attrs if kind == INTEGRITY]
    if len(matches) != 1:
        raise ValueError('Missing or duplicate integrity')
    value, offset = matches[0]
    prefix = data[:2] + struct.pack('!H', offset + 24 - 20) + data[4:offset]
    if len(value) != 20 or not hmac.compare_digest(value, hmac.digest(key, prefix, 'sha1')):
        raise ValueError('Invalid response integrity')
    # Only FINGERPRINT may follow integrity in this deliberately narrow client.
    if any(kind != FINGERPRINT and position > offset for kind, _, position in attrs):
        raise ValueError('Unauthenticated response attributes')


def xor_address(address):
    ip, port = address
    return struct.pack('!BBHI', 0, 1, port ^ (COOKIE >> 16), int.from_bytes(socket.inet_aton(ip), 'big') ^ COOKIE)


def decode_address(value):
    if len(value) != 8 or value[:2] != b'\x00\x01':
        raise ValueError('Fixture supports IPv4 XOR addresses only')
    port, ip = struct.unpack('!HI', value[2:])
    return socket.inet_ntoa((ip ^ COOKIE).to_bytes(4, 'big')), port ^ (COOKIE >> 16)


def rest_password(secret, username):
    return base64.b64encode(hmac.digest(secret.encode(), username.encode(), 'sha1')).decode()


def long_term_key(username, realm, password):
    # RFC 8489 legacy long-term credential mode, exercised explicitly by this fixture.
    return hashlib.md5(f'{username}:{realm}:{password}'.encode()).digest()


def error_code(attrs):
    value = one(attrs, ERROR)
    if len(value) < 4:
        raise ValueError('Invalid TURN error')
    return (value[2] & 7) * 100 + value[3]


def transact(client, kind, attrs, key=None):
    request = packet(kind, attrs, key=key)
    for _ in range(3):
        client.send(request)
        deadline = time.monotonic() + 2
        while time.monotonic() < deadline:
            client.settimeout(max(0.01, deadline - time.monotonic()))
            try:
                data = client.recv(65535)
            except socket.timeout:
                break
            response_kind, transaction, parsed = parse(data)
            if transaction == request[8:20]:
                return response_kind, parsed, data
    raise TimeoutError('TURN response deadline exceeded')


def probe(shared_secret, host='turn'):
    with socket.socket(socket.AF_INET, socket.SOCK_DGRAM) as client, socket.socket(socket.AF_INET, socket.SOCK_DGRAM) as peer:
        client.connect((host, 3478))
        peer.bind(('0.0.0.0', 0))
        peer.settimeout(3)
        peer_address = (client.getsockname()[0], peer.getsockname()[1])
        requested = [(TRANSPORT, b'\x11\x00\x00\x00'), (LIFETIME, struct.pack('!I', 60))]
        kind, attrs, _ = transact(client, 0x0003, requested)
        if kind != 0x0113 or error_code(attrs) != 401:
            raise ValueError('Unauthenticated allocation was not challenged')
        realm, nonce = one(attrs, REALM).decode(), one(attrs, NONCE)
        if realm != 'simurghrtc.fixture.invalid':
            raise ValueError('Unexpected TURN realm')
        def credentials(username, password):
            return [(USERNAME, username.encode()), (REALM, realm.encode()), (NONCE, nonce)], long_term_key(username, realm, password)
        username = f'{int(time.time()) + 60}:sr005'
        auth, key = credentials(username, rest_password(shared_secret, username))
        _, wrong_key = credentials(username, 'invalid-fixture-credential')
        kind, attrs, _ = transact(client, 0x0003, requested + auth, wrong_key)
        if kind != 0x0113 or error_code(attrs) != 401:
            raise ValueError('Wrong TURN credentials were not rejected')
        expired = f'{int(time.time()) - 120}:expired'
        old_auth, old_key = credentials(expired, rest_password(shared_secret, expired))
        kind, attrs, _ = transact(client, 0x0003, requested + old_auth, old_key)
        if kind != 0x0113 or error_code(attrs) != 401:
            raise ValueError('Expired TURN credentials were not rejected')
        kind, attrs, data = transact(client, 0x0003, requested + auth, key)
        if kind != 0x0103:
            raise ValueError('Authenticated TURN allocation failed')
        verify_integrity(data, key)
        relay = decode_address(one(attrs, RELAY))
        if not 49160 <= relay[1] <= 49175:
            raise ValueError('Relay escaped configured port range')
        kind, attrs, data = transact(client, 0x0008, [(PEER, xor_address(peer_address))] + auth, key)
        if kind != 0x0108:
            raise ValueError('TURN permission failed')
        verify_integrity(data, key)
        payload = b'SimurghRTC-SR005-v1-relay\x00\x01\xfe\xff'
        client.send(packet(0x0016, [(PEER, xor_address(peer_address)), (DATA, payload)]))
        received, sender = peer.recvfrom(65535)
        if received != payload or sender != relay:
            raise ValueError('Peer did not receive exact payload from allocated relay')
        peer.sendto(received, sender)
        client.settimeout(3)
        kind, _, attrs = parse(client.recv(65535))
        if kind != 0x0017 or one(attrs, DATA) != payload or decode_address(one(attrs, PEER)) != peer_address:
            raise ValueError('TURN Data indication did not return the peer payload')
        kind, attrs, data = transact(client, 0x0004, [(LIFETIME, struct.pack('!I', 0))] + auth, key)
        if kind != 0x0104 or struct.unpack('!I', one(attrs, LIFETIME))[0] != 0:
            raise ValueError('TURN allocation release failed')
        verify_integrity(data, key)
        return {'unauthenticated_challenged': True, 'wrong_credentials_rejected': True, 'expired_credentials_rejected': True,
                'authenticated_allocation': True, 'permission_created': True, 'relay_round_trip_bytes': len(payload),
                'response_integrity_verified': True, 'allocation_released': True}
