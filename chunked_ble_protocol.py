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


class ChunkedBLEProtocol:
    """
    Enhanced Chunked BLE Protocol for reliable data transfer over BLE
    
    Features:
    - CRC32 validation for data integrity
    - Security limits (max 64KB transfers)
    - Transfer timeouts (configurable chunk timeout)
    - Enhanced statistics and diagnostics
    
    Header format (13 bytes): chunk_num(2) + total_chunks(2) + data_size(1) + chunk_crc32(4) + global_crc32(4)
    
    Usage (C++-like API):
        protocol = ChunkedBLEProtocol(ble_client)
        protocol.set_data_received_callback(on_data)
        protocol.set_progress_callback(on_progress)
        await protocol.initialize()
        await protocol.send_data(b"data")
    """
    
    # Enhanced protocol constants
    CHUNK_SIZE = 172       # Data size per chunk (185 - 13 bytes header)
    HEADER_SIZE = 13       # Enhanced header: chunk_num(2) + total_chunks(2) + data_size(1) + chunk_crc32(4) + global_crc32(4)
    MTU_SIZE = 185         # Maximum transmission unit
    
    # Security and reliability limits
    MAX_TOTAL_DATA_SIZE = 64 * 1024    # 64KB max transfer
    MAX_CHUNKS_PER_TRANSFER = 365      # ~64KB / 172 bytes
    DEFAULT_CHUNK_TIMEOUT = 60.0        # Default 5 seconds per chunk timeout

    def __init__(self, client: BleakClient, service_uuid: str = DEFAULT_SERVICE_UUID, char_uuid: str = DEFAULT_CHAR_UUID):
        """
        Initialize the chunked BLE protocol (C++-like API)
        
        Args:
            client: Connected BleakClient instance
            service_uuid: Optional custom service UUID
            char_uuid: Optional custom characteristic UUID
        """
        self.client = client
        self.service_uuid = service_uuid
        self.char_uuid = char_uuid
        
        # Internal BLE components (hidden from user)
        self._characteristic: Optional[BleakGATTCharacteristic] = None
        self._notifications_enabled = False
        
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
        
        # Security and statistics
        self._stats = {
            'total_data_sent': 0,
            'total_data_received': 0,
            'crc_errors': 0,
            'timeouts': 0,
            'successful_transfers': 0,
            'last_transfer_time': 0.0
        }
        
        # Callbacks (C++-like delegates)
        self._data_received_callback: Optional[Callable[[bytes], None]] = None
        self._connection_callback: Optional[Callable[[bool], None]] = None
        self._progress_callback: Optional[Callable[[int, int, bool], None]] = None
        
        self._log("[PROTOCOL] Enhanced ChunkedBLEProtocol initialized")
        self._log(f"[PROTOCOL] MTU={self.MTU_SIZE}, Chunk size={self.CHUNK_SIZE} bytes, Header={self.HEADER_SIZE} bytes")
        self._log(f"[SECURITY] Max data: {self.MAX_TOTAL_DATA_SIZE} bytes, Max chunks: {self.MAX_CHUNKS_PER_TRANSFER}")
        self._log(f"[CONFIG] Chunk timeout: {self._chunk_timeout}s (configurable)")
        self._log(f"[PROTOCOL] Service UUID: {self.service_uuid}")
        self._log(f"[PROTOCOL] Characteristic UUID: {self.char_uuid}")
    
    async def initialize(self) -> bool:
        """
        Initialize BLE service and characteristic (like C++ constructor)
        
        Returns:
            True if initialization successful, False otherwise
        """
        try:
            self._log("[BLE] Discovering services...")
            
            # Find service and characteristic automatically
            services = await self.client.get_services()
            
            target_service = None
            for service in services:
                if service.uuid.lower() == self.service_uuid.lower():
                    target_service = service
                    break
            
            if not target_service:
                self._log(f"[ERROR] Service {self.service_uuid} not found")
                return False
            
            # Find characteristic
            for char in target_service.characteristics:
                if char.uuid.lower() == self.char_uuid.lower():
                    self._characteristic = char
                    break
            
            if not self._characteristic:
                self._log(f"[ERROR] Characteristic {self.char_uuid} not found")
                return False
            
            self._log(f"[BLE] Service and characteristic found successfully")
            
            # Enable notifications automatically
            await self.enable_notifications()
            
            self._log("[PROTOCOL] Initialization complete")
            return True
            
        except Exception as e:
            self._log(f"[ERROR] Initialization failed: {e}")
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
        Send data using chunked protocol
        
        Args:
            data: Data to send
            
        Returns:
            True if sent successfully, False otherwise
        """
        if not self._characteristic:
            self._log("[ERROR] Protocol not initialized")
            return False
        
        try:
            data_size = len(data)
            
            # Validate data size against security limits
            if not self._validate_data_size(data_size):
                self._log(f"[ERROR] Data rejected by security validation")
                return False
            
            total_chunks = (data_size + self.CHUNK_SIZE - 1) // self.CHUNK_SIZE  # Round up
            if total_chunks > self.MAX_CHUNKS_PER_TRANSFER:
                self._log(f"[ERROR] Too many chunks ({total_chunks} > {self.MAX_CHUNKS_PER_TRANSFER})")
                return False
            
            self._log(f"[CHUNK] Sending data in {total_chunks} chunks, total size: {data_size} bytes")
            self._log(f"[SECURITY] Data passed validation (max {self.MAX_TOTAL_DATA_SIZE} bytes, {self.MAX_CHUNKS_PER_TRANSFER} chunks)")
            
            # Start transfer timing
            send_start_time = time.time()
            
            for chunk_num in range(total_chunks):
                chunk_start = chunk_num * self.CHUNK_SIZE
                chunk_end = min(chunk_start + self.CHUNK_SIZE, data_size)
                chunk_data = data[chunk_start:chunk_end]
                chunk_data_size = len(chunk_data)
                
                # Calculate CRC32 for chunk data
                crc32 = self._calculate_crc32(chunk_data)
                
                # Create enhanced header: chunk_num(2) + total_chunks(2) + data_size(1) + crc32(4) + global_crc32(4)
                header = struct.pack('<HHB', chunk_num + 1, total_chunks, chunk_data_size) + crc32.to_bytes(4, 'little') + self._calculate_crc32(data).to_bytes(4, 'little')
                
                # Combine header and data
                chunk_packet = header + chunk_data
                
                # Send chunk
                await self.client.write_gatt_char(self._characteristic, chunk_packet)
                
                self._log(f"[CHUNK] Sent chunk {chunk_num + 1}/{total_chunks} ({chunk_data_size} bytes data, CRC32: 0x{crc32:08X})")
                
                # Update progress
                if self._progress_callback:
                    self._progress_callback(chunk_num + 1, total_chunks, False)
                
                # Small delay between chunks to prevent overwhelming receiver
                await asyncio.sleep(0.01)
            
            send_time = time.time() - send_start_time
            self._log(f"[CHUNK] All chunks sent successfully in {send_time:.3f}s")
            
            # Update statistics
            self._stats['total_data_sent'] += data_size
            self._stats['successful_transfers'] += 1
            self._stats['last_transfer_time'] = time.time()
            
            return True
            
        except Exception as e:
            self._log(f"[ERROR] Send failed: {e}")
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
    
    def _process_received_chunk(self, data: bytes) -> None:
        """
        Process received chunk data (internal)
        
        Args:
            data: Raw chunk data with header
        """
        try:
            # Check minimum data size for header
            if len(data) < self.HEADER_SIZE:
                self._log(f"[CHUNK] Received data too small for chunk header ({len(data)} bytes)")
                return
            
            # Parse enhanced header
            chunk_num, total_chunks, data_size = struct.unpack('<HHB', data[:5])
            chunk_crc32 = int.from_bytes(data[5:9], 'little')
            global_crc32 = int.from_bytes(data[9:13], 'little')
            chunk_data = data[13:13 + data_size]
            
            self._log(f"[CHUNK] Received chunk {chunk_num}/{total_chunks} ({data_size} bytes data, CRC32: 0x{chunk_crc32:08X})")
            
            # Check if data size matches header
            expected_size = self.HEADER_SIZE + data_size
            if len(data) != expected_size:
                self._log(f"[CHUNK] Data size mismatch: expected {expected_size}, got {len(data)}")
                self._stats['crc_errors'] += 1
                return
            
            # Validate CRC32
            calculated_crc = self._calculate_crc32(chunk_data)
            if chunk_crc32 != calculated_crc:
                self._log(f"[CRC] CRC32 mismatch: expected 0x{chunk_crc32:08X}, calculated 0x{calculated_crc:08X}")
                self._stats['crc_errors'] += 1
                return
            
            self._log(f"[CRC] CRC32 validation passed for chunk {chunk_num}")
            
            # Initialize chunks buffer if this is the first chunk
            if chunk_num == 1:
                self._received_chunks = [None] * total_chunks
                self._expected_chunks = total_chunks
                self._received_chunk_count = 0
                
                # Start transfer timer
                self._start_transfer_timer()
                
                self._log(f"[CHUNK] Starting new transfer: expecting {total_chunks} chunks total")
                
                # Validate total expected data size
                estimated_total_size = total_chunks * self.CHUNK_SIZE
                if not self._validate_data_size(estimated_total_size):
                    self._cancel_transfer("Total data size exceeds limits")
                    return
                
                # Store global CRC32 from first chunk
                self._expected_global_crc32 = global_crc32
                self._log(f"[CRC] Expected global CRC32: 0x{global_crc32:08X}")
            else:
                # Validate global CRC32 consistency across chunks
                if global_crc32 != self._expected_global_crc32:
                    self._log(f"[CRC] Global CRC32 inconsistency: expected 0x{self._expected_global_crc32:08X}, got 0x{global_crc32:08X}")
                    self._cancel_transfer("Global CRC32 mismatch between chunks")
                    return
            
            # Check chunk timeout
            if self._check_chunk_timeout():
                self._cancel_transfer("Chunk timeout")
                return
            
            # Update chunk timer
            self._update_chunk_timer()
            
            # Validate chunk consistency
            if total_chunks != self._expected_chunks:
                self._log(f"[CHUNK] Inconsistent total chunks: expected {self._expected_chunks}, got {total_chunks}")
                self._cancel_transfer("Inconsistent chunk count")
                return
            
            # Check for duplicate chunks
            chunk_index = chunk_num - 1  # Convert to 0-based index
            if chunk_index >= 0 and chunk_index < len(self._received_chunks) and self._received_chunks[chunk_index] is not None:
                self._log(f"[CHUNK] Duplicate chunk {chunk_num} - ignoring")
                return
            
            # Validate chunk index
            if chunk_index < 0 or chunk_index >= len(self._received_chunks):
                self._log(f"[CHUNK] Invalid chunk index {chunk_index} for {len(self._received_chunks)} total chunks")
                return
            
            # Store chunk data
            self._received_chunks[chunk_index] = chunk_data
            self._received_chunk_count += 1
            
            # Update statistics
            self._stats['total_data_received'] += data_size
            
            # Notify progress
            if self._progress_callback:
                self._progress_callback(self._received_chunk_count, self._expected_chunks, True)
            
            self._log(f"[CHUNK] Progress: {self._received_chunk_count}/{self._expected_chunks} chunks received")
            
            # Check if all chunks received
            if self._received_chunk_count == self._expected_chunks:
                self._log("[CHUNK] All chunks received, assembling complete data")
                
                # Assemble complete data
                complete_data = b''.join(chunk for chunk in self._received_chunks if chunk is not None)
                
                # Mark transfer as complete
                self._transfer_in_progress = False
                
                self._log(f"[CHUNK] Complete data assembled ({len(complete_data)} bytes)")
                
                # Update final statistics
                self._stats['successful_transfers'] += 1
                self._stats['last_transfer_time'] = time.time()
                
                # Validate global CRC32
                calculated_global_crc32 = self._calculate_crc32(complete_data)
                if self._expected_global_crc32 != calculated_global_crc32:
                    self._log(f"[CRC] Global CRC32 mismatch: expected 0x{self._expected_global_crc32:08X}, calculated 0x{calculated_global_crc32:08X}")
                    self._stats['crc_errors'] += 1
                    return
                
                self._log(f"[CRC] Global CRC32 validation passed")
                
                # Store data BEFORE setting event and calling callback (critical for sync!)
                self._received_data = complete_data
                
                # Set completion event
                self._complete_data_event.set()
                
                # Call user callback if set
                if self._data_received_callback:
                    self._data_received_callback(complete_data)
                
                # Clear buffers for next reception
                self._received_chunks.clear()
                self._expected_chunks = 0
                self._received_chunk_count = 0
                
        except Exception as e:
            self._log(f"[ERROR] Failed to process chunk: {e}")
            self._stats['crc_errors'] += 1
    
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
