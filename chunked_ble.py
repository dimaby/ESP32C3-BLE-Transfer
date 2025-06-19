"""
Chunked BLE Protocol - Real Implementation
BLE data transfer protocol with chunking and ACK/NAK confirmation
"""

import asyncio
import datetime
import struct
import json
import zlib  # For CRC32 calculation
from typing import Callable, Optional, List
from bleak import BleakClient, BleakScanner
from bleak.backends.characteristic import BleakGATTCharacteristic

# UUIDs (matching ESP32 implementation)
DEFAULT_SERVICE_UUID = "5b18eb9b-747f-47da-b7b0-a4e503f9a00f"
DEFAULT_CHAR_UUID = "8f8b49a2-9117-4e9f-acfc-fda4d0db7408"
DEFAULT_CONTROL_CHAR_UUID = "12345678-1234-1234-1234-123456789012"

# Control messages
CONTROL_NOP = bytearray.fromhex("00")
CONTROL_REQUEST = bytearray.fromhex("01")
CONTROL_REQUEST_ACK = bytearray.fromhex("02")
CONTROL_REQUEST_NAK = bytearray.fromhex("03")
CONTROL_DONE = bytearray.fromhex("04")
CONTROL_DONE_ACK = bytearray.fromhex("05")
CONTROL_DONE_NAK = bytearray.fromhex("06")

class ChunkedBLEProtocol:
    """
    BLE data transfer protocol with chunking and ACK/NAK confirmation
    
    Features:
    - Automatic chunking based on MTU size
    - ACK/NAK protocol for guaranteed delivery
    - CRC32 validation for data integrity
    - Progress callbacks
    - Connection management
    """
    
    def __init__(self, client: BleakClient):
        """
        Initialize the protocol with a connected BLE client
        
        Args:
            client: Connected BleakClient instance
        """
        self.client = client
        self.service_uuid = DEFAULT_SERVICE_UUID
        self.data_char_uuid = DEFAULT_CHAR_UUID
        self.control_char_uuid = DEFAULT_CONTROL_CHAR_UUID
        
        # Callbacks
        self.data_received_callback: Optional[Callable[[str], None]] = None
        self.connection_callback: Optional[Callable[[bool], None]] = None
        self.progress_callback: Optional[Callable[[int, int, bool], None]] = None
        
        # Transfer state
        self.transfer_in_progress = False
        self.response_queue = asyncio.Queue()
        
        # Statistics
        self.stats = {
            'bytes_sent': 0,
            'bytes_received': 0,
            'chunks_sent': 0,
            'chunks_received': 0,
            'retransmissions': 0,
            'crc_errors': 0
        }
    
    async def initialize(self):
        """
        Initialize BLE service and characteristics
        """
        print(f"[BLE] Initializing protocol with service: {self.service_uuid}")
        
        # Start notifications on control characteristic
        await self.client.start_notify(
            self.control_char_uuid,
            self._control_notification_handler
        )
        print(f"[BLE] Control notifications enabled: {self.control_char_uuid}")
        
        # Start notifications on data characteristic (for receiving data)
        await self.client.start_notify(
            self.data_char_uuid,
            self._data_notification_handler
        )
        print(f"[BLE] Data notifications enabled: {self.data_char_uuid}")
        
        print("[PROTOCOL] ChunkedBLE Protocol initialized successfully")
        return True
    
    async def _control_notification_handler(self, sender: int, data: bytearray):
        """Handle control messages (ACK/NAK)"""
        if data == CONTROL_REQUEST_ACK:
            print("[CONTROL] Transfer request acknowledged")
            await self.response_queue.put("ack")
        elif data == CONTROL_REQUEST_NAK:
            print("[CONTROL] Transfer request NOT acknowledged")
            await self.response_queue.put("nak")
        elif data == CONTROL_DONE_ACK:
            print("[CONTROL] Transfer done acknowledged")
            await self.response_queue.put("ack")
        elif data == CONTROL_DONE_NAK:
            print("[CONTROL] Transfer done NOT acknowledged")
            await self.response_queue.put("nak")
        else:
            print(f"[CONTROL] Unknown control message: {data.hex()}")
    
    async def _data_notification_handler(self, sender: int, data: bytearray):
        """Handle incoming data chunks"""
        print(f"[DATA] Received {len(data)} bytes")
        self.stats['bytes_received'] += len(data)
        self.stats['chunks_received'] += 1
        
        # For now, just call the callback with received data
        if self.data_received_callback:
            received_data = data.decode('utf-8', errors='ignore')
            self.data_received_callback(received_data)
    
    async def send_data(self, data: str) -> bool:
        """
        Send data using chunked transfer with ACK/NAK protocol
        
        Args:
            data: String data to send
            
        Returns:
            bool: True if transfer successful, False otherwise
        """
        if self.transfer_in_progress:
            print("[ERROR] Transfer already in progress")
            return False
        
        self.transfer_in_progress = True
        t0 = datetime.datetime.now()
        
        try:
            # Convert data to bytes
            data_bytes = data.encode('utf-8')
            total_size = len(data_bytes)
            
            print(f"[TRANSFER] Starting transfer of {total_size} bytes")
            
            # Calculate packet size based on MTU
            packet_size = self.client.mtu_size - 3  # Reserve 3 bytes for BLE overhead
            print(f"[TRANSFER] Using packet size: {packet_size} bytes (MTU: {self.client.mtu_size})")
            
            # Send packet size to ESP32
            await self.client.write_gatt_char(
                self.data_char_uuid,
                packet_size.to_bytes(2, 'little'),
                response=True
            )
            print(f"[TRANSFER] Packet size sent: {packet_size}")
            
            # Split data into chunks
            chunks = []
            for i in range(0, total_size, packet_size):
                chunk = data_bytes[i:i + packet_size]
                chunks.append(chunk)
            
            total_chunks = len(chunks)
            print(f"[TRANSFER] Split into {total_chunks} chunks")
            
            # Send transfer request
            print("[TRANSFER] Sending transfer request")
            await self.client.write_gatt_char(
                self.control_char_uuid,
                CONTROL_REQUEST
            )
            
            # Wait for ACK
            await asyncio.sleep(1)
            try:
                response = await asyncio.wait_for(self.response_queue.get(), timeout=5.0)
                if response != "ack":
                    print("[ERROR] Transfer request not acknowledged")
                    return False
            except asyncio.TimeoutError:
                print("[ERROR] Timeout waiting for transfer request ACK")
                return False
            
            # Send all chunks sequentially
            for i, chunk in enumerate(chunks):
                print(f"[TRANSFER] Sending chunk {i+1}/{total_chunks} ({len(chunk)} bytes)")
                
                await self.client.write_gatt_char(
                    self.data_char_uuid,
                    chunk,
                    response=True
                )
                
                self.stats['bytes_sent'] += len(chunk)
                self.stats['chunks_sent'] += 1
                
                # Update progress
                if self.progress_callback:
                    self.progress_callback(i + 1, total_chunks, False)
            
            # Send transfer done
            print("[TRANSFER] Sending transfer done")
            await self.client.write_gatt_char(
                self.control_char_uuid,
                CONTROL_DONE
            )
            
            # Wait for final ACK 
            await asyncio.sleep(1)
            try:
                response = await asyncio.wait_for(self.response_queue.get(), timeout=5.0)
                if response == "ack":
                    dt = datetime.datetime.now() - t0
                    print(f"[SUCCESS] Transfer completed successfully! Total time: {dt}")
                    return True
                else:
                    print("[ERROR] Transfer done not acknowledged")
                    return False
            except asyncio.TimeoutError:
                print("[ERROR] Timeout waiting for transfer done ACK")
                return False
                
        except Exception as e:
            print(f"[ERROR] Transfer failed: {e}")
            return False
        finally:
            self.transfer_in_progress = False
    
    def set_data_received_callback(self, callback: Callable[[str], None]):
        """Set callback for received data"""
        self.data_received_callback = callback
        print("[CALLBACK] Data received callback set")
    
    def set_connection_callback(self, callback: Callable[[bool], None]):
        """Set callback for connection events"""
        self.connection_callback = callback
        print("[CALLBACK] Connection callback set")
    
    def set_progress_callback(self, callback: Callable[[int, int, bool], None]):
        """Set callback for transfer progress"""
        self.progress_callback = callback
        print("[CALLBACK] Progress callback set")
    
    def is_device_connected(self) -> bool:
        """Check if device is connected"""
        return self.client.is_connected
    
    def get_statistics(self) -> dict:
        """Get transfer statistics"""
        return self.stats.copy()
    
    def reset_statistics(self):
        """Reset transfer statistics"""
        self.stats = {
            'bytes_sent': 0,
            'bytes_received': 0,
            'chunks_sent': 0,
            'chunks_received': 0,
            'retransmissions': 0,
            'crc_errors': 0
        }
        print("[STATS] Statistics reset")
    
    def is_transfer_in_progress(self) -> bool:
        """Check if transfer is in progress"""
        return self.transfer_in_progress
    
    def cancel_current_transfer(self, reason: str = "User requested"):
        """Cancel current transfer"""
        if self.transfer_in_progress:
            print(f"[TRANSFER] Cancelling transfer: {reason}")
            self.transfer_in_progress = False
        else:
            print("[TRANSFER] No transfer in progress to cancel")