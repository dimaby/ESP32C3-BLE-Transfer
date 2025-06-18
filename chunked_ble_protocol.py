"""
Chunked BLE Protocol Implementation
Provides reliable transfer of large data over BLE with MTU limitations
Enhanced with CRC32 validation, security limits, and timeouts
"""

import asyncio
import struct
import time
import zlib  # For CRC32 calculation
from typing import Callable, Optional, List
from bleak import BleakClient
from bleak.backends.characteristic import BleakGATTCharacteristic

# Default configuration - matching ESP32 UUIDs
DEFAULT_SERVICE_UUID = "5b18eb9b-747f-47da-b7b0-a4e503f9a00f"
DEFAULT_CHAR_UUID = "8f8b49a2-9117-4e9f-acfc-fda4d0db7408"
CONTROL_CHAR_UUID = "12345678-1234-1234-1234-123456789012"  # Control channel for ACK messages (matching ESP32)

# ACK Protocol Commands (matching C++ implementation)
ACK_CHUNK_RECEIVED = 0x01    # Chunk received successfully
ACK_CHUNK_ERROR = 0x02       # Chunk error, request retransmission
ACK_TRANSFER_COMPLETE = 0x03 # All chunks received, transfer complete
ACK_TRANSFER_SUCCESS = 0x04  # Final transfer validation successful
ACK_TRANSFER_FAILED = 0x05   # Final transfer validation failed


class ChunkedBLEProtocol:
    """
    Enhanced Chunked BLE Protocol for reliable data transfer over BLE with ACK support
    
    Features:
    - CRC32 validation for data integrity
    - Security limits (max 64KB transfers)
    - Transfer timeouts (configurable chunk timeout)
    - ACK/NAK protocol for guaranteed delivery
    - Control channel for acknowledgments
    - Enhanced statistics and diagnostics
    
    Header format (17 bytes): chunk_num(2) + total_chunks(2) + data_size(1) + chunk_crc32(4) + global_crc32(4) + total_data_size(4)
    ACK format (13 bytes): ack_type(1) + chunk_number(4) + total_chunks(4) + global_crc32(4)
    
    Usage (C++-like API):
        protocol = ChunkedBLEProtocol(ble_client)
        protocol.set_data_received_callback(on_data)
        protocol.set_progress_callback(on_progress)
        await protocol.initialize()
        await protocol.send_data(b"data")
    """
    
    # Enhanced protocol constants
    HEADER_SIZE = 17  # chunk_num(2) + total_chunks(2) + data_size(1) + crc32(4) + global_crc32(4) + total_data_size(4)
    CHUNK_SIZE = 168  # MTU(185) - HEADER_SIZE(17)
    MTU_SIZE = 185         # Maximum transmission unit
    ACK_MESSAGE_SIZE = 13  # ACK message size: ack_type(1) + chunk_number(4) + total_chunks(4) + global_crc32(4)
    
    # Security and reliability limits
    MAX_TOTAL_DATA_SIZE = 64 * 1024    # 64KB max transfer
    MAX_CHUNKS_PER_TRANSFER = 365      # ~64KB / 168 bytes
    DEFAULT_CHUNK_TIMEOUT = 15.0        # Increased for fire-and-forget ESP32 (was 5.0)
    DEFAULT_ACK_TIMEOUT = 2.0          # Default 2 seconds ACK timeout
    DEFAULT_MAX_RETRIES = 3            # Default maximum retries per chunk

    def __init__(self, client: BleakClient, service_uuid: str = DEFAULT_SERVICE_UUID, char_uuid: str = DEFAULT_CHAR_UUID, control_uuid: str = CONTROL_CHAR_UUID):
        """
        Initialize the chunked BLE protocol with ACK support (C++-like API)
        
        Args:
            client: Connected BleakClient instance
            service_uuid: Optional custom service UUID
            char_uuid: Optional custom characteristic UUID
            control_uuid: Optional custom control characteristic UUID for ACK
        """
        self.client = client
        self.service_uuid = service_uuid
        self.char_uuid = char_uuid
        self.control_uuid = control_uuid
        
        # Internal BLE components (hidden from user)
        self._characteristic: Optional[BleakGATTCharacteristic] = None
        self._control_characteristic: Optional[BleakGATTCharacteristic] = None
        self._notifications_enabled = False
        self._control_notifications_enabled = False
        
        # Receive buffer management
        self._received_chunks: List[Optional[bytes]] = []
        self._expected_chunks = 0
        self._received_chunk_count = 0
        self._complete_data_event = asyncio.Event()
        self._received_data: Optional[bytes] = None
        
        # Transfer state and timing (only chunk timeout needed)
        self._transfer_in_progress = False
        self._transfer_start_time = None
        self._chunk_timeout = self.DEFAULT_CHUNK_TIMEOUT  # Configurable chunk timeout
        self._last_chunk_time = None  # Initialize to None
        self._expected_global_crc32 = None  # Expected global CRC32 from first chunk
        
        # ACK protocol state
        self._sending_chunks = False
        self._waiting_for_ack = False
        self._current_chunk_number = 0
        self._chunks_to_send: List[bytes] = []
        self._ack_timeout = self.DEFAULT_ACK_TIMEOUT
        self._max_retries = self.DEFAULT_MAX_RETRIES
        self._chunk_retry_count = {}  # Track retries per chunk
        self._ack_received_event = asyncio.Event()
        self._last_ack_time = None
        
        # Security and statistics
        self._stats = {
            'total_data_sent': 0,
            'total_data_received': 0,
            'crc_errors': 0,
            'timeouts': 0,
            'successful_transfers': 0,
            'ack_timeouts': 0,
            'retransmissions': 0,
            'last_transfer_time': 0.0
        }
        
        # Callbacks (C++-like delegates)
        self._data_received_callback: Optional[Callable[[bytes], None]] = None
        self._connection_callback: Optional[Callable[[bool], None]] = None
        self._progress_callback: Optional[Callable[[int, int, bool], None]] = None
        
        self._log("[PROTOCOL] Enhanced ChunkedBLEProtocol with ACK support initialized")
        self._log(f"[PROTOCOL] MTU={self.MTU_SIZE}, Chunk size={self.CHUNK_SIZE} bytes, Header={self.HEADER_SIZE} bytes")
        self._log(f"[ACK] ACK timeout={self._ack_timeout}s, Max retries={self._max_retries}")
        self._log(f"[SECURITY] Max data: {self.MAX_TOTAL_DATA_SIZE} bytes, Max chunks: {self.MAX_CHUNKS_PER_TRANSFER}")

    async def initialize(self) -> bool:
        """
        Initialize BLE service and characteristic with control channel (like C++ constructor)
        
        Returns:
            True if initialization successful, False otherwise
        """
        try:
            # Find the service
            service = self.client.services.get_service(self.service_uuid)
            if not service:
                self._log(f"[ERROR] Service {self.service_uuid} not found")
                return False
            
            # Find data characteristic
            self._characteristic = service.get_characteristic(self.char_uuid)
            if not self._characteristic:
                self._log(f"[ERROR] Data characteristic {self.char_uuid} not found")
                return False
            
            # Find control characteristic for ACK
            self._control_characteristic = service.get_characteristic(self.control_uuid)
            if not self._control_characteristic:
                self._log(f"[ERROR] Control characteristic {self.control_uuid} not found")
                return False
            
            # Enable notifications on both characteristics
            await self.client.start_notify(self._characteristic, self._data_notification_handler)
            await self.client.start_notify(self._control_characteristic, self._control_notification_handler)
            
            self._notifications_enabled = True
            self._control_notifications_enabled = True
            
            self._log("[BLE] Data and control characteristics initialized with notifications")
            self._log(f"[BLE] Data characteristic: {self.char_uuid}")
            self._log(f"[BLE] Control characteristic: {self.control_uuid}")
            
            return True
            
        except Exception as e:
            self._log(f"[ERROR] Failed to initialize BLE components: {e}")
            return False

    async def enable_notifications(self) -> bool:
        """
        Enable notifications on characteristic (internal)
        
        Returns:
            True if successful, False otherwise
        """
        try:
            if self._characteristic and not self._notifications_enabled:
                await self.client.start_notify(self._characteristic, self._notification_handler)
                self._notifications_enabled = True
                self._log("[BLE] Notifications enabled")
                return True
            return False
        except Exception as e:
            self._log(f"[ERROR] Failed to enable notifications: {e}")
            return False

    async def disable_notifications(self) -> bool:
        """
        Disable notifications on characteristic (cleanup)
        
        Returns:
            True if successful, False otherwise
        """
        try:
            if self._characteristic and self._notifications_enabled:
                await self.client.stop_notify(self._characteristic)
                self._notifications_enabled = False
                self._log("[BLE] Notifications disabled")
                return True
            return False
        except Exception as e:
            self._log(f"[ERROR] Failed to disable notifications: {e}")
            return False

    def set_data_received_callback(self, callback: Callable[[bytes], None]) -> None:
        """
        Set callback for complete data reception (C++-like)
        
        Args:
            callback: Function to call when complete data is received
        """
        self._data_received_callback = callback
        self._log("[PROTOCOL] Data received callback set")

    def set_connection_callback(self, callback: Callable[[bool], None]) -> None:
        """
        Set callback for connection status changes (C++-like)
        
        Args:
            callback: Function to call when connection status changes
        """
        self._connection_callback = callback
        self._log("[PROTOCOL] Connection callback set")

    def set_progress_callback(self, callback: Callable[[int, int, bool], None]) -> None:
        """
        Set callback for transfer progress (C++-like)
        
        Args:
            callback: Function to call with progress: (current, total, is_receiving)
        """
        self._progress_callback = callback

    def set_chunk_timeout(self, timeout_seconds: float) -> None:
        """
        Set chunk timeout in seconds (C++-like API)
        
        Args:
            timeout_seconds: Chunk timeout in seconds
        """
        self._chunk_timeout = timeout_seconds
        self._log(f"[CONFIG] Chunk timeout set to {self._chunk_timeout}s")

    async def send_data(self, data: bytes) -> bool:
        """
        Send data using chunked protocol with ACK support
        
        Args:
            data: Data to send
            
        Returns:
            True if sent successfully, False otherwise
        """
        try:
            if not self._characteristic:
                self._log("[ERROR] Characteristic not initialized")
                return False
            
            if not self._validate_data_size(len(data)):
                return False
            
            self._log(f"[SEND] Starting chunked transfer of {len(data)} bytes")
            
            # Calculate global CRC32 for all data
            global_crc32 = self._calculate_crc32(data)
            self._expected_global_crc32 = global_crc32
            
            # Split data into chunks
            chunks = []
            total_chunks = (len(data) + self.CHUNK_SIZE - 1) // self.CHUNK_SIZE
            
            for i in range(total_chunks):
                start_idx = i * self.CHUNK_SIZE
                end_idx = min(start_idx + self.CHUNK_SIZE, len(data))
                chunk_data = data[start_idx:end_idx]
                chunks.append(chunk_data)
            
            # Send chunks with ACK protocol
            self._chunks_to_send = chunks
            self._sending_chunks = True
            successful_chunks = 0
            
            for chunk_num, chunk_data in enumerate(chunks):
                # Check timeout and cancel if needed
                if self._check_chunk_timeout():
                    self._cancel_transfer("Chunk timeout")
                    return False
                
                # Create enhanced header: chunk_num(2) + total_chunks(2) + data_size(1) + crc32(4) + global_crc32(4) + total_data_size(4)
                chunk_crc32 = self._calculate_crc32(chunk_data)
                header = struct.pack('<HHB', chunk_num + 1, total_chunks, len(chunk_data))  # chunk_num + 1 for 1-based indexing
                header += chunk_crc32.to_bytes(4, 'little')
                header += global_crc32.to_bytes(4, 'little')
                header += len(data).to_bytes(4, 'little')
                
                # Combine header and data
                chunk_packet = header + chunk_data
                
                # Update timing
                self._update_chunk_timer()
                
                # Send chunk
                await self.client.write_gatt_char(self._characteristic, chunk_packet)
                
                # Update progress callback if set
                if self._progress_callback:
                    self._progress_callback(chunk_num + 1, total_chunks, False)  # False = sending
                
                self._log(f"[SEND] Sent chunk {chunk_num + 1}/{total_chunks} ({len(chunk_data)} bytes)")
                successful_chunks += 1
                
                # Small delay between chunks for stability
                await asyncio.sleep(0.01)
            
            # All chunks sent successfully, wait for final validation
            self._log(f"[SEND] All {successful_chunks}/{total_chunks} chunks sent successfully")
            
            # Update statistics
            self._stats['total_data_sent'] += len(data)
            self._stats['successful_transfers'] += 1
            self._stats['last_transfer_time'] = time.time()
            
            # Don't reset _sending_chunks if we're expecting response data
            # It will be reset when response is fully received
            if not hasattr(self, '_response_expected_chunks'):
                self._sending_chunks = False
                self._log(f"[DEBUG] Reset _sending_chunks = False (no response expected)")
            else:
                self._log(f"[DEBUG] Keeping _sending_chunks = True (expecting response)")
            
            return True
            
        except Exception as e:
            self._log(f"[ERROR] Send failed: {e}")
            self._sending_chunks = False
            return False

    async def send_json(self, json_data: dict) -> bool:
        """
        Send JSON data using chunked protocol with ACK support
        
        Args:
            json_data: Dictionary to send as JSON
            
        Returns:
            True if sent successfully, False otherwise
        """
        try:
            import json
            json_str = json.dumps(json_data)
            json_bytes = json_str.encode('utf-8')
            
            self._log(f"[JSON] Sending JSON data ({len(json_bytes)} bytes)")
            return await self.send_data(json_bytes)
            
        except Exception as e:
            self._log(f"[ERROR] JSON send failed: {e}")
            return False

    async def receive_data(self, timeout: float = 30.0) -> Optional[bytes]:
        """
        Wait for and receive complete data
        
        Args:
            timeout: Timeout in seconds
            
        Returns:
            Complete received data or None if timeout
        """
        try:
            # Wait for complete data with timeout
            await asyncio.wait_for(self._complete_data_event.wait(), timeout=timeout)
            
            # Return received data and reset for next reception
            data = self._received_data
            self._received_data = None
            self._complete_data_event.clear()
            
            return data
            
        except asyncio.TimeoutError:
            self._log(f"[ERROR] Receive timeout after {timeout} seconds")
            return None
        except Exception as e:
            self._log(f"[ERROR] Receive failed: {e}")
            return None

    async def cleanup(self) -> None:
        """
        Cleanup protocol resources
        """
        await self.disable_notifications()
        self._received_chunks.clear()
        self._expected_chunks = 0
        self._received_chunk_count = 0
        self._received_data = None
        self._complete_data_event.clear()
        self._log("[PROTOCOL] Cleanup complete")

    # Internal methods (hidden from user)
    
    async def _notification_handler(self, sender: BleakGATTCharacteristic, data: bytearray) -> None:
        """
        Handle incoming BLE notifications (internal)
        
        Args:
            sender: Characteristic that sent the notification
            data: Received data
        """
        try:
            if len(data) < self.HEADER_SIZE:
                self._log("[ERROR] Received data too small for chunk header")
                return
            
            self._process_received_chunk(bytes(data))
            
        except Exception as e:
            self._log(f"[ERROR] Notification handler failed: {e}")

    async def _data_notification_handler(self, sender: BleakGATTCharacteristic, data: bytearray) -> None:
        """
        Handle incoming BLE notifications (internal)
        
        Args:
            sender: Characteristic that sent the notification
            data: Received data
        """
        try:
            if len(data) < self.HEADER_SIZE:
                self._log("[ERROR] Received data too small for chunk header")
                return
            
            self._process_received_chunk(bytes(data))
            
        except Exception as e:
            self._log(f"[ERROR] Notification handler failed: {e}")

    async def _control_notification_handler(self, sender: BleakGATTCharacteristic, data: bytearray) -> None:
        """
        Handle incoming BLE notifications (internal)
        
        Args:
            sender: Characteristic that sent the notification
            data: Received data
        """
        try:
            if len(data) < self.ACK_MESSAGE_SIZE:
                self._log("[ERROR] Received data too small for ACK message")
                return
            
            self._process_received_ack(bytes(data))
            
        except Exception as e:
            self._log(f"[ERROR] Notification handler failed: {e}")

    async def _send_ack_message(self, ack_type: int, chunk_number: int = 0) -> bool:
        """
        Send ACK message through control channel
        
        Args:
            ack_type: Type of ACK message
            chunk_number: Chunk number being acknowledged
            
        Returns:
            True if sent successfully, False otherwise
        """
        try:
            if not self._control_characteristic:
                self._log("[ERROR] Control characteristic not initialized")
                return False
            
            # Create ACK message: ack_type(1) + chunk_number(4) + total_chunks(4) + global_crc32(4)
            expected_global_crc32 = self._expected_global_crc32 if self._expected_global_crc32 else 0
            
            ack_message = struct.pack('<B', ack_type)
            ack_message += chunk_number.to_bytes(4, 'little')
            ack_message += self._expected_chunks.to_bytes(4, 'little')
            ack_message += expected_global_crc32.to_bytes(4, 'little')
            
            await self.client.write_gatt_char(self._control_characteristic, ack_message)
            
            self._log(f"[ACK] Sent ACK message: type={ack_type}, chunk_number={chunk_number}, total_chunks={self._expected_chunks}, global_crc32=0x{expected_global_crc32:08X}")
            return True
            
        except Exception as e:
            self._log(f"[ERROR] Failed to send ACK message: {e}")
            return False

    def _process_received_chunk(self, data: bytes) -> None:
        """
        Process received chunk data with ACK support (internal)
        
        Args:
            data: Raw chunk data with header
        """
        try:
            # Check minimum data size for header
            if len(data) < self.HEADER_SIZE:
                self._log(f"[CHUNK] Received data too small for chunk header ({len(data)} bytes)")
                # Send NAK for invalid chunk
                asyncio.create_task(self._send_ack_message(ACK_CHUNK_ERROR, 0))
                return
            
            # Parse enhanced chunk header
            chunk_num, total_chunks, data_size, chunk_crc32, global_crc32, total_data_size = struct.unpack('<HHBIII', data[:17])
            self._log(f"[CHUNK] Received chunk {chunk_num}/{total_chunks} ({data_size} bytes data, CRC32: 0x{chunk_crc32:08X})")
            
            # Validate chunk header
            if not self._validate_chunk_header(chunk_num, total_chunks, data_size, len(data)):
                self._log(f"[CHUNK] Invalid chunk header - sending NAK")
                asyncio.create_task(self._send_ack_message(ACK_CHUNK_ERROR, chunk_num))
                self._stats['crc_errors'] += 1
                return
            
            # Check data size matches header
            expected_size = self.HEADER_SIZE + data_size
            if len(data) != expected_size:
                self._log(f"[CHUNK] Data size mismatch: expected {expected_size}, got {len(data)}")
                asyncio.create_task(self._send_ack_message(ACK_CHUNK_ERROR, chunk_num))
                self._stats['crc_errors'] += 1
                return
            
            # Extract chunk data
            chunk_data = data[self.HEADER_SIZE:]
            
            # Validate CRC32
            calculated_crc = self._calculate_crc32(chunk_data)
            if calculated_crc != chunk_crc32:
                self._log(f"[CRC] CRC32 mismatch: expected 0x{chunk_crc32:08X}, calculated 0x{calculated_crc:08X}")
                asyncio.create_task(self._send_ack_message(ACK_CHUNK_ERROR, chunk_num))
                self._stats['crc_errors'] += 1
                return
            
            self._log(f"[CRC] CRC32 validation passed for chunk {chunk_num}")
            
            # Initialize transfer if this is the first chunk
            if self._sending_chunks and not hasattr(self, '_response_chunks'):
                # We're receiving response data while sending - initialize on first chunk
                self._log(f"[BIDIRECTIONAL] Initializing response reception: {total_chunks} chunks expected")
                self._response_expected_chunks = total_chunks
                if self._expected_global_crc32 is None:
                    # First chunk - initialize expected global CRC32
                    self._expected_global_crc32 = global_crc32
                    self._log(f"[CRC] Initialized expected global CRC32: 0x{global_crc32:08X}")
                elif global_crc32 != self._expected_global_crc32:
                    self._log(f"[CRC] Global CRC32 inconsistency: expected 0x{self._expected_global_crc32:08X}, got 0x{global_crc32:08X}")
                    asyncio.create_task(self._send_ack_message(ACK_CHUNK_ERROR, chunk_num))
                    self._cancel_transfer("Global CRC32 mismatch between chunks")
                    return
                self._response_chunks = [None] * total_chunks
                self._response_chunk_count = 0
                self._log(f"[RESPONSE] Initializing response reception: {total_chunks} chunks, global CRC32: 0x{global_crc32:08X}")
            elif chunk_num == 1 and not self._sending_chunks:
                # Normal chunk reception (not sending)
                self._expected_chunks = total_chunks
                if self._expected_global_crc32 is None:
                    # First chunk - initialize expected global CRC32
                    self._expected_global_crc32 = global_crc32
                    self._log(f"[CRC] Initialized expected global CRC32: 0x{global_crc32:08X}")
                elif global_crc32 != self._expected_global_crc32:
                    self._log(f"[CRC] Global CRC32 inconsistency: expected 0x{self._expected_global_crc32:08X}, got 0x{global_crc32:08X}")
                    asyncio.create_task(self._send_ack_message(ACK_CHUNK_ERROR, chunk_num))
                    self._cancel_transfer("Global CRC32 mismatch between chunks")
                    return
                self._received_chunks = [None] * total_chunks
                self._received_chunk_count = 0
                self._transfer_in_progress = True
                self._log(f"[CHUNK] Initializing chunk reception: {total_chunks} chunks, global CRC32: 0x{global_crc32:08X}")
            else:
                # Validate global CRC32 consistency across chunks
                if self._sending_chunks:
                    # Check response global CRC32 consistency
                    if hasattr(self, '_response_expected_global_crc32') and global_crc32 != self._response_expected_global_crc32:
                        self._log(f"[RESPONSE] Global CRC32 inconsistency: expected 0x{self._response_expected_global_crc32:08X}, got 0x{global_crc32:08X}")
                        return
                else:
                    # Check normal global CRC32 consistency
                    if self._expected_global_crc32 is not None and global_crc32 != self._expected_global_crc32:
                        self._log(f"[CRC] Global CRC32 inconsistency: expected 0x{self._expected_global_crc32:08X}, got 0x{global_crc32:08X}")
                        asyncio.create_task(self._send_ack_message(ACK_CHUNK_ERROR, chunk_num))
                        self._cancel_transfer("Global CRC32 mismatch between chunks")
                        return
            
            # Convert to 0-based indexing for array access
            chunk_index = chunk_num - 1
            
            # Check for duplicate chunks
            if self._sending_chunks:
                # Check response duplicates
                if hasattr(self, '_response_chunks') and self._response_chunks and chunk_index < len(self._response_chunks) and self._response_chunks[chunk_index] is not None:
                    self._log(f"[RESPONSE] Duplicate chunk {chunk_num} - sending ACK again")
                    asyncio.create_task(self._send_ack_message(ACK_CHUNK_RECEIVED, chunk_num))
                    return
            else:
                # Check normal duplicates
                if chunk_index < len(self._received_chunks) and self._received_chunks[chunk_index] is not None:
                    self._log(f"[CHUNK] Duplicate chunk {chunk_num} - sending ACK again")
                    asyncio.create_task(self._send_ack_message(ACK_CHUNK_RECEIVED, chunk_num))
                    return
            
            # Store chunk data
            if self._sending_chunks:
                # We're receiving response data while sending
                self._log(f"[DEBUG] Processing response chunk: hasattr(_response_chunks)={hasattr(self, '_response_chunks')}, _response_chunks={getattr(self, '_response_chunks', None) is not None}")
                if hasattr(self, '_response_chunks') and self._response_chunks:
                    if chunk_num <= len(self._response_chunks):
                        self._response_chunks[chunk_num - 1] = chunk_data
                        self._response_chunk_count += 1
                        self._log(f"[RESPONSE] Received chunk {chunk_num}/{self._response_expected_chunks} ({len(chunk_data)} bytes data, CRC32: 0x{chunk_crc32:08X})")
                        
                        # Send ACK for response chunk
                        asyncio.create_task(self._send_ack_message(ACK_CHUNK_RECEIVED, chunk_num))
                        
                        # Check if all response chunks received
                        if self._response_chunk_count == self._response_expected_chunks:
                            self._log(f"[RESPONSE] All response chunks received, assembling complete data")
                            
                            # Assemble complete response data
                            complete_data = b''.join(chunk for chunk in self._response_chunks if chunk is not None)
                            
                            # Validate global CRC32 for response
                            calculated_global_crc32 = self._calculate_crc32(complete_data)
                            if calculated_global_crc32 != self._response_expected_global_crc32:
                                self._log(f"[RESPONSE] Global CRC32 mismatch: expected 0x{self._response_expected_global_crc32:08X}, calculated 0x{calculated_global_crc32:08X}")
                                return
                            
                            self._log(f"[RESPONSE] Global CRC32 validation passed for response data")
                            self._log(f"[RESPONSE] Complete response data assembled ({len(complete_data)} bytes)")
                            
                            # Send final success ACK for response
                            asyncio.create_task(self._send_ack_message(ACK_TRANSFER_SUCCESS, 0))
                            
                            # Call data received callback with response data
                            if self._data_received_callback:
                                self._data_received_callback(complete_data)

                            # Clear response state
                            self._response_chunks = None
                            self._response_chunk_count = 0
                            if hasattr(self, '_response_expected_chunks'):
                                delattr(self, '_response_expected_chunks')
                            if hasattr(self, '_response_expected_global_crc32'):
                                delattr(self, '_response_expected_global_crc32')
                            self._sending_chunks = False
                return
            else:
                # Normal chunk reception (not sending)
                if chunk_num <= len(self._received_chunks):
                    self._received_chunks[chunk_num - 1] = chunk_data
                    self._received_chunk_count += 1
                    
                    self._log(f"[CHUNK] Received chunk {chunk_num}/{self._expected_chunks} ({len(chunk_data)} bytes data, CRC32: 0x{chunk_crc32:08X})")
                    
                    # Send ACK
                    asyncio.create_task(self._send_ack_message(ACK_CHUNK_RECEIVED, chunk_num))
                    
                    self._log(f"[CHUNK] Progress: {self._received_chunk_count}/{self._expected_chunks} chunks received")
                    
                    # Check if all chunks received
                    if self._received_chunk_count == self._expected_chunks:
                        self._log(f"[CHUNK] All chunks received, assembling complete data")
                        
                        # Send transfer complete ACK
                        asyncio.create_task(self._send_ack_message(ACK_TRANSFER_COMPLETE, 0))
                        
                        # Assemble complete data
                        complete_data = b''.join(chunk for chunk in self._received_chunks if chunk is not None)
                        
                        # Validate global CRC32 after assembling complete data
                        calculated_global_crc32 = self._calculate_crc32(complete_data)
                        if calculated_global_crc32 != self._expected_global_crc32:
                            self._log(f"[CRC] Global CRC32 mismatch: expected 0x{self._expected_global_crc32:08X}, calculated 0x{calculated_crc32:08X}")
                            asyncio.create_task(self._send_ack_message(ACK_TRANSFER_FAILED, 0))
                            self._cancel_transfer("Global CRC32 mismatch after assembling complete data")
                            return
                        
                        self._log(f"[CRC] CRC32 validation passed for complete data")
                        
                        # Send final success ACK
                        asyncio.create_task(self._send_ack_message(ACK_TRANSFER_SUCCESS, 0))
                        
                        # Mark transfer as complete
                        self._transfer_in_progress = False
                        
                        self._log(f"[CHUNK] Complete data assembled ({len(complete_data)} bytes)")
                        
                        # Call data received callback
                        if self._data_received_callback:
                            self._data_received_callback(complete_data)
                        
                        # Update statistics
                        self._stats['total_data_received'] += len(complete_data)
                        self._stats['successful_transfers'] += 1
                        self._stats['last_transfer_time'] = time.time()
            
            # Update chunk timer and validate consistency
            self._update_chunk_timer()
            
            self._log(f"[DEBUG] Processing chunk {chunk_num}: _sending_chunks={self._sending_chunks}, _transfer_in_progress={self._transfer_in_progress}")
            
            # Skip consistency check when receiving response data during bidirectional transfer
            if not self._sending_chunks and self._expected_chunks > 0 and total_chunks != self._expected_chunks:
                self._log(f"[CHUNK] Inconsistent total chunks: expected {self._expected_chunks}, got {total_chunks}")
                asyncio.create_task(self._send_ack_message(ACK_CHUNK_ERROR, chunk_num))
                self._cancel_transfer("Inconsistent chunk count")
                return
            elif self._sending_chunks and not hasattr(self, '_response_expected_chunks'):
                # For bidirectional transfer: initialize response reception on first response chunk
                self._log(f"[BIDIRECTIONAL] Initializing response reception: {total_chunks} chunks expected")
                self._response_expected_chunks = total_chunks
                self._response_expected_global_crc32 = global_crc32
                self._response_chunks = [None] * total_chunks
                self._response_chunk_count = 0
        
        except Exception as e:
            self._log(f"[ERROR] Failed to process chunk: {e}")
            asyncio.create_task(self._send_ack_message(ACK_CHUNK_ERROR, 0))
            self._stats['crc_errors'] += 1

    def _process_received_ack(self, data: bytes) -> None:
        """
        Process received ACK message (internal)
        
        Args:
            data: Raw ACK message
        """
        try:
            # Check minimum data size for ACK message
            if len(data) < self.ACK_MESSAGE_SIZE:
                self._log(f"[ACK] Received data too small for ACK message ({len(data)} bytes)")
                return
            
            # Parse ACK message
            ack_type = data[0]
            chunk_number = int.from_bytes(data[1:5], 'little')
            total_chunks = int.from_bytes(data[5:9], 'little') if len(data) >= 9 else 0
            global_crc32 = int.from_bytes(data[9:13], 'little') if len(data) >= 13 else 0
            
            self._log(f"[ACK] Received ACK message: type={ack_type}, chunk_number={chunk_number}, total_chunks={total_chunks}, global_crc32=0x{global_crc32:08X}")
            
            # Validate global CRC32 consistency across ACK messages
            if self._expected_global_crc32 is None:
                self._expected_global_crc32 = global_crc32
            elif global_crc32 != self._expected_global_crc32:
                self._log(f"[CRC] Global CRC32 inconsistency: expected 0x{self._expected_global_crc32:08X}, got 0x{global_crc32:08X}")
                self._cancel_transfer("Global CRC32 mismatch between ACK messages")
                return
            
            # Handle ACK message based on type
            if ack_type == ACK_CHUNK_RECEIVED:
                self._log(f"[ACK] Chunk {chunk_number} received successfully")
                self._ack_received_event.set()
            elif ack_type == ACK_CHUNK_ERROR:
                self._log(f"[ACK] Chunk {chunk_number} error, retransmitting")
                self._retransmit_chunk(chunk_number)
            elif ack_type == ACK_TRANSFER_COMPLETE:
                self._log("[ACK] Transfer complete - ESP32 received all data successfully")
                self._response_expected_chunks = total_chunks  # Set expected chunks count
                self._response_expected_global_crc32 = global_crc32
                self._log(f"[DEBUG] Set _response_expected_chunks = {total_chunks}, _response_expected_global_crc32 = 0x{global_crc32:08X}")
                # Send ACK_TRANSFER_COMPLETE back to ESP32 to trigger dataReceivedCallback
                asyncio.create_task(self._send_ack_message(ACK_TRANSFER_COMPLETE, 0))
                self._stats['successful_transfers'] += 1
                
                # Prepare to receive response data from ESP32
                self._log("[RESPONSE] Preparing to receive response data from ESP32...")
                self._reset_receive_state()
                self._sending_chunks = True  # Mark that we're now expecting to receive response
                self._log(f"[DEBUG] Set _sending_chunks = True for response reception")
            elif ack_type == ACK_TRANSFER_SUCCESS:
                self._log("[ACK] Transfer successful, updating statistics")
                self._stats['successful_transfers'] += 1
                self._stats['last_transfer_time'] = time.time()
            elif ack_type == ACK_TRANSFER_FAILED:
                self._log("[ACK] Transfer failed, updating statistics")
                self._stats['crc_errors'] += 1
            
        except Exception as e:
            self._log(f"[ERROR] Failed to process ACK message: {e}")

    def _log(self, message: str) -> None:
        """
        Internal logging utility
        
        Args:
            message: Message to log
        """
        print(message)

    # Statistics and diagnostics
    def get_statistics(self) -> dict:
        """Get transfer statistics"""
        return self._stats.copy()
    
    def reset_statistics(self) -> None:
        """Reset all statistics"""
        self._stats = {
            'total_data_sent': 0,
            'total_data_received': 0,
            'crc_errors': 0,
            'timeouts': 0,
            'successful_transfers': 0,
            'ack_timeouts': 0,
            'retransmissions': 0,
            'last_transfer_time': 0.0
        }
        self._log("[STATS] Statistics reset")
    
    def is_transfer_in_progress(self) -> bool:
        """Check if transfer is currently in progress"""
        return self._transfer_in_progress
    
    # Timeout and transfer management
    def _start_transfer_timer(self) -> None:
        """Start transfer timing"""
        self._transfer_start_time = time.time()
        self._transfer_in_progress = True
    
    def _update_chunk_timer(self) -> None:
        """Update last chunk received time"""
        self._last_chunk_time = time.time()
    
    def _check_chunk_timeout(self) -> bool:
        """Check if chunk timeout exceeded"""
        if self._chunk_timeout <= 0:
            return False  # Timeout disabled
        
        current_time = time.time()
        if self._last_chunk_time is None:
            # First chunk - no timeout yet
            return False
            
        time_since_last = current_time - self._last_chunk_time
        if time_since_last > self._chunk_timeout:
            self._log(f"[TIMEOUT] Chunk timeout exceeded: {time_since_last:.1f}s > {self._chunk_timeout:.1f}s")
            return True
        return False
    
    def _cancel_transfer(self, reason: str) -> None:
        """Cancel current transfer with reason"""
        self._log(f"[TRANSFER] Cancelled: {reason}")
        self._transfer_in_progress = False
        self._stats['timeouts'] += 1
        
        # Clear receive buffers
        self._received_chunks.clear()
        self._expected_chunks = 0
        self._received_chunk_count = 0
        self._complete_data_event.clear()
    
    def _validate_data_size(self, size: int) -> bool:
        """Validate data size against security limits"""
        if size > self.MAX_TOTAL_DATA_SIZE:
            self._log(f"[SECURITY] Data size {size} exceeds limit {self.MAX_TOTAL_DATA_SIZE}")
            return False
        return True
    
    def _calculate_crc32(self, data: bytes) -> int:
        """Calculate CRC32 for data"""
        return zlib.crc32(data) & 0xFFFFFFFF

    def _retransmit_chunk(self, chunk_number: int) -> None:
        """Retransmit chunk with given number"""
        self._log(f"[RETRANSMIT] Retransmitting chunk {chunk_number}")
        self._stats['retransmissions'] += 1
        
        # Find chunk data to retransmit
        chunk_index = chunk_number - 1  # Convert to 0-based index
        if chunk_index < 0 or chunk_index >= len(self._chunks_to_send):
            self._log(f"[RETRANSMIT] Invalid chunk index {chunk_index} for retransmission")
            return
        
        chunk_data = self._chunks_to_send[chunk_index]
        
        # Create enhanced header: chunk_num(2) + total_chunks(2) + data_size(1) + crc32(4) + global_crc32(4) + total_data_size(4)
        header = struct.pack('<HHB', chunk_number, len(self._chunks_to_send), len(chunk_data)) + self._calculate_crc32(chunk_data).to_bytes(4, 'little') + self._expected_global_crc32.to_bytes(4, 'little') + len(b''.join(self._chunks_to_send)).to_bytes(4, 'little')
        
        # Combine header and data
        chunk_packet = header + chunk_data
        
        # Send chunk
        self.client.write_gatt_char(self._characteristic, chunk_packet)
        
        self._log(f"[RETRANSMIT] Chunk {chunk_number} retransmitted successfully")

    def _validate_transfer(self) -> None:
        """Validate transfer by checking CRC32 of received data"""
        self._log("[VALIDATE] Validating transfer")
        
        # Choose correct data buffer based on mode
        if self._sending_chunks:
            # We are receiving response data, use response chunks
            if hasattr(self, '_response_chunks') and self._response_chunks:
                chunks_buffer = self._response_chunks
                self._log("[VALIDATE] Using response chunks buffer")
            else:
                self._log("[ERROR] Response chunks buffer not available")
                return False
        else:
            # We are receiving incoming data, use received chunks
            chunks_buffer = self._received_chunks
            self._log("[VALIDATE] Using received chunks buffer")
        
        # Assemble complete data
        complete_data = b''.join(chunk for chunk in chunks_buffer if chunk is not None)
        
        # Calculate CRC32 of received data
        calculated_crc32 = self._calculate_crc32(complete_data)
        
        # Check if CRC32 matches expected value
        if calculated_crc32 != self._expected_global_crc32:
            self._log(f"[CRC] CRC32 mismatch: expected 0x{self._expected_global_crc32:08X}, calculated 0x{calculated_crc32:08X}")
            self._stats['crc_errors'] += 1
            return
        
        self._log("[CRC] CRC32 validation passed")
        
        # Send ACK_TRANSFER_SUCCESS message
        ack_message = struct.pack('<B', ACK_TRANSFER_SUCCESS) + self._expected_global_crc32.to_bytes(4, 'little')
        self.client.write_gatt_char(self._control_characteristic, ack_message)
        
        self._log("[ACK] Sent ACK_TRANSFER_SUCCESS message")

    def _validate_chunk_header(self, chunk_num: int, total_chunks: int, data_size: int, total_packet_size: int) -> bool:
        """
        Validate chunk header parameters
        
        Args:
            chunk_num: Chunk number (1-based)
            total_chunks: Total number of chunks
            data_size: Size of chunk data
            total_packet_size: Total packet size including header
            
        Returns:
            True if header is valid, False otherwise
        """
        # Check chunk number bounds
        if chunk_num < 1 or chunk_num > total_chunks:
            self._log(f"[HEADER] Invalid chunk number: {chunk_num} (total: {total_chunks})")
            return False
        
        # Check total chunks bounds
        if total_chunks <= 0 or total_chunks > self.MAX_CHUNKS_PER_TRANSFER:
            self._log(f"[HEADER] Invalid total chunks: {total_chunks}")
            return False
        
        # Check data size bounds
        if data_size <= 0 or data_size > self.CHUNK_SIZE:
            self._log(f"[HEADER] Invalid data size: {data_size}")
            return False
        
        # Check total packet size
        expected_packet_size = self.HEADER_SIZE + data_size
        if total_packet_size != expected_packet_size:
            self._log(f"[HEADER] Invalid packet size: expected {expected_packet_size}, got {total_packet_size}")
            return False
        
        return True

    def _reset_receive_state(self) -> None:
        """Reset receive state for new transfer"""
        self._log(f"[DEBUG] _reset_receive_state called, _sending_chunks before: {self._sending_chunks}")
        self._received_chunks.clear()
        self._expected_chunks = 0
        self._received_chunk_count = 0
        self._complete_data_event.clear()
        self._received_data = None
        self._transfer_in_progress = False
        self._last_chunk_time = None
        self._expected_global_crc32 = None  # Reset to None
        self._log(f"[DEBUG] _reset_receive_state completed, _sending_chunks after: {self._sending_chunks}")
