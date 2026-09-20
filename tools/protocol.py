"""Host-side reference for the ESP-NOW MIDI bridge wire format."""

from dataclasses import dataclass

MAGIC = b"ENM1"
VERSION = 1
HEADER_SIZE = 18
MAX_RADIO_PACKET = 250
MAX_PAYLOAD = MAX_RADIO_PACKET - HEADER_SIZE
SYSEX_CHUNK = 224


@dataclass(frozen=True)
class Packet:
    message_type: int
    session: int
    sequence: int
    payload: bytes


def encode_packet(message_type: int, session: int, sequence: int, payload: bytes) -> bytes:
    if not 0 <= message_type <= 255:
        raise ValueError("message type out of range")
    if len(payload) > MAX_PAYLOAD:
        raise ValueError("payload exceeds ESP-NOW packet limit")
    return (MAGIC + bytes((VERSION, message_type, 0, 0)) +
            session.to_bytes(4, "big") + sequence.to_bytes(4, "big") +
            len(payload).to_bytes(2, "big") + payload)


def decode_packet(raw: bytes) -> Packet:
    if len(raw) < HEADER_SIZE or len(raw) > MAX_RADIO_PACKET:
        raise ValueError("invalid packet length")
    if raw[:4] != MAGIC or raw[4] != VERSION:
        raise ValueError("invalid packet magic/version")
    length = int.from_bytes(raw[16:18], "big")
    if length != len(raw) - HEADER_SIZE or length > MAX_PAYLOAD:
        raise ValueError("invalid payload length")
    return Packet(raw[5], int.from_bytes(raw[8:12], "big"),
                  int.from_bytes(raw[12:16], "big"), raw[18:])


def sequence_is_newer(candidate: int, previous: int) -> bool:
    delta = (candidate - previous) & 0xFFFFFFFF
    return 0 < delta < 0x80000000


def fragment_sysex(data: bytes, chunk_size: int = SYSEX_CHUNK) -> list[bytes]:
    if chunk_size <= 0:
        raise ValueError("chunk size must be positive")
    if len(data) > 65535:
        raise ValueError("SysEx exceeds bounded reassembly size")
    return [data[offset:offset + chunk_size] for offset in range(0, len(data), chunk_size)] or [b""]


class SysexReassembler:
    def __init__(self, max_size: int = 65535):
        self.max_size = max_size
        self._data = bytearray()

    def add(self, chunk: bytes) -> bytes | None:
        if len(self._data) + len(chunk) > self.max_size:
            self._data.clear()
            raise ValueError("SysEx reassembly limit exceeded")
        self._data.extend(chunk)
        if self._data.endswith(b"\xf7"):
            result = bytes(self._data)
            self._data.clear()
            return result
        return None
