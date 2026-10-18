from pathlib import Path
import struct
import sys
import unittest

sys.path.insert(0, str(Path(__file__).resolve().parents[2] / 'Fixtures/services'))
import turn_probe as turn

# Public test vector from RFC 5769 section 2.1 (not a service credential).
# Copyright (c) 2010 IETF Trust and the identified authors. See Fixtures/NOTICE.md.
RFC5769_REQUEST = bytes.fromhex('''
000100582112a442b7e7a701bc34d686fa87dfae
802200105354554e207465737420636c69656e74
002400046e0001ff80290008932ff9b151263b36
000600096576746a3a68367659202020
000800149aeaa70cbfd8cb56781ef2b5b2d3f249c1b571a2
80280004e57a3bcf''')
PUBLIC_VECTOR_KEY = b'VOkJxbRl1RmTxUk/WvJxBt'


class TurnProtocolTests(unittest.TestCase):
    def test_rfc5769_integrity_fingerprint_and_padding(self):
        kind, transaction, attrs = turn.parse(RFC5769_REQUEST)
        self.assertEqual(kind, 1)
        self.assertEqual(transaction.hex(), 'b7e7a701bc34d686fa87dfae')
        self.assertEqual(turn.one(attrs, turn.USERNAME), b'evtj:h6vY')
        turn.verify_integrity(RFC5769_REQUEST, PUBLIC_VECTOR_KEY)
        with self.assertRaises(ValueError):
            turn.verify_integrity(RFC5769_REQUEST, b'wrong-key')

    def test_xor_address_matches_rfc5769_ipv4_response(self):
        encoded = bytes.fromhex('0001a147e112a643')
        self.assertEqual(turn.decode_address(encoded), ('192.0.2.1', 32853))
        self.assertEqual(turn.xor_address(('192.0.2.1', 32853)), encoded)
        with self.assertRaises(ValueError):
            turn.decode_address(b'\x00\x02' + bytes(18))

    def test_bad_framing_truncation_cookie_attribute_and_fingerprint(self):
        original = RFC5769_REQUEST
        cases = [original[:19], original[:-1], original + b'\x00', bytes([0xC0]) + original[1:],
                 original[:4] + bytes(4) + original[8:], original[:22] + b'\xff\xff' + original[24:],
                 original[:-1] + bytes([original[-1] ^ 1])]
        for data in cases:
            with self.subTest(data=data.hex()), self.assertRaises(ValueError):
                turn.parse(data)

    def test_authentication_covers_payload_and_rejects_trailing_unsigned_attributes(self):
        key = b'fixture-only'
        message = turn.packet(0x0103, [(turn.LIFETIME, struct.pack('!I', 60))], bytes(12), key)
        turn.verify_integrity(message, key)
        corrupted = bytearray(message)
        corrupted[27] ^= 1
        with self.assertRaises(ValueError):
            turn.verify_integrity(bytes(corrupted), key)
        extra = turn.attribute(turn.LIFETIME, struct.pack('!I', 999))
        forged = message[:2] + struct.pack('!H', len(message) + len(extra) - 20) + message[4:] + extra
        with self.assertRaisesRegex(ValueError, 'Unauthenticated'):
            turn.verify_integrity(forged, key)

    def test_missing_duplicate_integrity_and_error_attributes(self):
        message = turn.packet(0x0103, [(turn.INTEGRITY, bytes(20)), (turn.INTEGRITY, bytes(20))])
        with self.assertRaises(ValueError):
            turn.verify_integrity(message, b'key')
        _, _, attrs = turn.parse(turn.packet(0x0113, [(turn.ERROR, b'\x00\x00\x04\x01Unauthorized')]))
        self.assertEqual(turn.error_code(attrs), 401)
        with self.assertRaises(ValueError):
            turn.one([], turn.NONCE)
        with self.assertRaises(ValueError):
            turn.one(attrs + attrs, turn.ERROR)


if __name__ == '__main__':
    unittest.main()
