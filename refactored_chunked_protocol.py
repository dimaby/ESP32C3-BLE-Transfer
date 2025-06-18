# Refactored chunked BLE protocol for cleaner two-way transfer

import asyncio
import struct
import zlib
from dataclasses import dataclass
from typing import Callable, Optional, List

from bleak import BleakClient
from bleak.backends.characteristic import BleakGATTCharacteristic

DEFAULT_SERVICE_UUID = "5b18eb9b-747f-47da-b7b0-a4e503f9a00f"
DEFAULT_CHAR_UUID = "8f8b49a2-9117-4e9f-acfc-fda4d0db7408"
CONTROL_CHAR_UUID = "12345678-1234-1234-1234-123456789012"

ACK_CHUNK_RECEIVED = 0x01
ACK_TRANSFER_SUCCESS = 0x04

HEADER_STRUCT = struct.Struct("<HHBIII")
ACK_STRUCT = struct.Struct("<BIII")
CHUNK_SIZE = 168
HEADER_SIZE = HEADER_STRUCT.size

@dataclass
class ChunkHeader:
    chunk_num: int
    total_chunks: int
    data_size: int
    chunk_crc32: int
    global_crc32: int
    total_size: int


class RefactoredChunkedBLEProtocol:
    """Simpler and more reliable chunked transfer helper."""

    def __init__(self, client: BleakClient,
                 service_uuid: str = DEFAULT_SERVICE_UUID,
                 char_uuid: str = DEFAULT_CHAR_UUID,
                 control_uuid: str = CONTROL_CHAR_UUID) -> None:
        self.client = client
        self.service_uuid = service_uuid
        self.char_uuid = char_uuid
        self.control_uuid = control_uuid

        self._data_char: Optional[BleakGATTCharacteristic] = None
        self._control_char: Optional[BleakGATTCharacteristic] = None

        self._ack_event = asyncio.Event()
        self._received_event = asyncio.Event()
        self._expected_ack = 0
        self._last_ack = 0
        self._incoming_chunks: List[Optional[bytes]] = []
        self._incoming_total = 0
        self._incoming_crc = 0
        self._received_data: Optional[bytes] = None

        self.ack_timeout = 2.0
        self.max_retries = 3

        self.on_data: Optional[Callable[[bytes], None]] = None
        self.on_progress: Optional[Callable[[int, int, bool], None]] = None

    async def initialize(self) -> bool:
        service = self.client.services.get_service(self.service_uuid)
        if not service:
            return False
        self._data_char = service.get_characteristic(self.char_uuid)
        self._control_char = service.get_characteristic(self.control_uuid)
        if not self._data_char or not self._control_char:
            return False
        await self.client.start_notify(self._control_char, self._on_control)
        await self.client.start_notify(self._data_char, self._on_data)
        return True

    async def cleanup(self) -> None:
        if self._data_char:
            await self.client.stop_notify(self._data_char)
        if self._control_char:
            await self.client.stop_notify(self._control_char)
        self._incoming_chunks.clear()
        self._received_event.clear()

    def set_data_received_callback(self, cb: Callable[[bytes], None]) -> None:
        self.on_data = cb

    def set_progress_callback(self, cb: Callable[[int, int, bool], None]) -> None:
        self.on_progress = cb

    async def send_data(self, data: bytes) -> bool:
        global_crc = zlib.crc32(data) & 0xFFFFFFFF
        total_chunks = (len(data) + CHUNK_SIZE - 1) // CHUNK_SIZE

        for i in range(total_chunks):
            start = i * CHUNK_SIZE
            chunk = data[start:start + CHUNK_SIZE]
            header = HEADER_STRUCT.pack(
                i + 1,
                total_chunks,
                len(chunk),
                zlib.crc32(chunk) & 0xFFFFFFFF,
                global_crc,
                len(data)
            )
            packet = header + chunk
            if not await self._send_with_ack(packet, i + 1):
                return False
            if self.on_progress:
                self.on_progress(i + 1, total_chunks, False)

        # wait for final ack
        self._expected_ack = 0
        try:
            await asyncio.wait_for(self._ack_event.wait(), self.ack_timeout)
        except asyncio.TimeoutError:
            return False
        return True

    async def receive_data(self, timeout: float = 30.0) -> Optional[bytes]:
        try:
            await asyncio.wait_for(self._received_event.wait(), timeout)
            data = self._received_data
            self._received_event.clear()
            self._received_data = None
            return data
        except asyncio.TimeoutError:
            return None

    async def _send_with_ack(self, packet: bytes, chunk_num: int) -> bool:
        for _ in range(self.max_retries):
            await self.client.write_gatt_char(self._data_char, packet)
            self._ack_event.clear()
            self._expected_ack = chunk_num
            try:
                await asyncio.wait_for(self._ack_event.wait(), self.ack_timeout)
                if self._last_ack == chunk_num:
                    return True
            except asyncio.TimeoutError:
                continue
        return False

    def _on_control(self, _: BleakGATTCharacteristic, data: bytearray) -> None:
        if len(data) < ACK_STRUCT.size:
            return
        ack_type, chunk_number, _, _ = ACK_STRUCT.unpack(data[:ACK_STRUCT.size])
        self._last_ack = chunk_number
        self._ack_event.set()
        if ack_type == ACK_TRANSFER_SUCCESS:
            self._received_event.set()

    def _on_data(self, _: BleakGATTCharacteristic, data: bytearray) -> None:
        if len(data) < HEADER_SIZE:
            return
        header_tuple = HEADER_STRUCT.unpack(data[:HEADER_SIZE])
        header = ChunkHeader(*header_tuple)
        chunk_data = data[HEADER_SIZE:HEADER_SIZE + header.data_size]
        if self._incoming_total == 0:
            self._incoming_total = header.total_chunks
            self._incoming_crc = header.global_crc32
            self._incoming_chunks = [None] * header.total_chunks
        if 1 <= header.chunk_num <= self._incoming_total:
            self._incoming_chunks[header.chunk_num - 1] = chunk_data
            if self.on_progress:
                self.on_progress(header.chunk_num, self._incoming_total, True)
            self._send_ack(ACK_CHUNK_RECEIVED, header.chunk_num)
            if all(ch is not None for ch in self._incoming_chunks):
                complete = b"".join(self._incoming_chunks)
                if zlib.crc32(complete) & 0xFFFFFFFF == self._incoming_crc:
                    self._send_ack(ACK_TRANSFER_SUCCESS, 0)
                    self._received_data = complete
                    self._received_event.set()
                    if self.on_data:
                        self.on_data(complete)

    def _send_ack(self, ack_type: int, chunk_num: int) -> None:
        msg = ACK_STRUCT.pack(ack_type, chunk_num, self._incoming_total, self._incoming_crc)
        if self._control_char:
            asyncio.create_task(self.client.write_gatt_char(self._control_char, msg))

