import sys
import unittest
from pathlib import Path

sys.path.insert(0, str(Path(__file__).parents[1] / "tools"))

from protocol import (  # noqa: E402
    HEADER_SIZE,
    MAX_PAYLOAD,
    SysexReassembler,
    decode_packet,
    encode_packet,
    fragment_sysex,
    sequence_is_newer,
)


class ProtocolTests(unittest.TestCase):
    def test_round_trip_and_explicit_header(self):
        raw = encode_packet(3, 0x12345678, 9, b"\x09\x90\x40\x7f")
        self.assertEqual(raw[:4], b"ENM1")
        self.assertEqual(len(raw), HEADER_SIZE + 4)
        self.assertEqual(decode_packet(raw).payload, b"\x09\x90\x40\x7f")

    def test_malformed_and_bounds(self):
        with self.assertRaises(ValueError):
            decode_packet(b"short")
        with self.assertRaises(ValueError):
            encode_packet(3, 1, 1, bytes(MAX_PAYLOAD + 1))
        raw = bytearray(encode_packet(3, 1, 1, b"x"))
        raw[16:18] = (2).to_bytes(2, "big")
        with self.assertRaises(ValueError):
            decode_packet(bytes(raw))

    def test_sequence_wrap_and_duplicate_order_rule(self):
        self.assertTrue(sequence_is_newer(0, 0xFFFFFFFF))
        self.assertTrue(sequence_is_newer(10, 9))
        self.assertFalse(sequence_is_newer(9, 9))
        self.assertFalse(sequence_is_newer(9, 10))

    def test_sysex_fragmentation_and_reassembly(self):
        message = b"\xf0" + bytes(range(250)) + b"\xf7"
        chunks = fragment_sysex(message, 32)
        self.assertGreater(len(chunks), 1)
        reassembler = SysexReassembler()
        result = None
        for chunk in chunks:
            result = reassembler.add(chunk)
        self.assertEqual(result, message)

    def test_sysex_limit_rejects_and_resets(self):
        reassembler = SysexReassembler(max_size=4)
        with self.assertRaises(ValueError):
            reassembler.add(b"12345")
        self.assertIsNone(reassembler.add(b"\xf0"))


if __name__ == "__main__":
    unittest.main()
