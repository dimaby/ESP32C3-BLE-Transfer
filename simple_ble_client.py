#!/usr/bin/env python3
"""
Simple BLE Client with Chunked Transfer Protocol
Provides easy-to-use high-level API for BLE JSON data exchange
"""

import asyncio
import json
import sys
from bleak import BleakScanner, BleakClient
from chunked_ble_protocol import ChunkedBLEProtocol, DEFAULT_SERVICE_UUID, DEFAULT_CHAR_UUID
from refactored_chunked_protocol import RefactoredChunkedBLEProtocol

DEFAULT_DEVICE_NAME = "BLE-Chunked"

class SimpleBLEClient:
    """
    Simple BLE Client - Clean Wrapper over ChunkedBLEProtocol
    
    Responsibilities:
    - Command line argument parsing
    - File loading
    - Device scanning and connection
    - Protocol delegation
    
    Usage:
        client = SimpleBLEClient("DEVICE_NAME", data_callback, progress_callback)
        await client.connect()
        await client.send_json({"test": "data"})
    """
    
    def __init__(self, device_name: str = DEFAULT_DEVICE_NAME, data_callback=None, progress_callback=None, use_refactored: bool = False):
        """
        Initialize Simple BLE Client
        
        Args:
            device_name: Name of target BLE device
            data_callback: Optional callback for received data
            progress_callback: Optional callback for transfer progress
        """
        self.device_name = device_name
        self.data_callback = data_callback
        self.progress_callback = progress_callback
        self.use_refactored = use_refactored
        
        self.client = None
        self.protocol = None
        self.target_device = None
        
        print(f"[INIT] Simple BLE client for device: {device_name}")
    
    async def scan_and_find_device(self) -> bool:
        """
        Scan for target device
        
        Returns:
            True if device found, False otherwise
        """
        try:
            print(f"[SCAN] Scanning for BLE devices...")
            
            devices = await BleakScanner.discover(timeout=10.0)
            
            for device in devices:
                device_name = device.name or "Unknown"
                print(f"[SCAN] Found: {device_name} ({device.address})")
                
                if device_name == self.device_name:
                    self.target_device = device
                    print(f"[SCAN] Target device found: {device_name} at {device.address}")
                    return True
            
            print(f"[ERROR] Target device '{self.device_name}' not found")
            return False
            
        except Exception as e:
            print(f"[ERROR] Scan failed: {e}")
            return False
    
    async def connect(self) -> bool:
        """
        Connect to device and initialize protocol
        
        Returns:
            True if connection successful, False otherwise
        """
        try:
            # Scan for device if not already found
            if not self.target_device:
                if not await self.scan_and_find_device():
                    return False
            
            # Connect to device
            print(f"[BLE] Connecting to {self.target_device.address}...")
            self.client = BleakClient(self.target_device.address)
            await self.client.connect()
            print(f"[BLE] Connected successfully")
            
            # Initialize protocol
            if self.use_refactored:
                self.protocol = RefactoredChunkedBLEProtocol(self.client)
            else:
                self.protocol = ChunkedBLEProtocol(self.client)
            
            # Set callbacks if provided
            if self.data_callback:
                self.protocol.set_data_received_callback(self.data_callback)
            if self.progress_callback:
                self.protocol.set_progress_callback(self.progress_callback)
            
            # Initialize protocol (finds service/characteristic automatically)
            if not await self.protocol.initialize():
                print("[ERROR] Protocol initialization failed")
                await self.disconnect()
                return False
            
            print("[SUCCESS] BLE connection and protocol initialization complete")
            return True
            
        except Exception as e:
            print(f"[ERROR] Connection failed: {e}")
            return False
    
    async def disconnect(self):
        """Disconnect from device"""
        try:
            if self.protocol:
                await self.protocol.cleanup()
                self.protocol = None
            
            if self.client and self.client.is_connected:
                await self.client.disconnect()
                print("[BLE] Disconnected")
            
            self.client = None
            self.target_device = None
            
        except Exception as e:
            print(f"[ERROR] Disconnect failed: {e}")
    
    async def send_json(self, data: dict) -> bool:
        """
        Send JSON data (simple delegation to protocol)
        
        Args:
            data: Dictionary to send as JSON
            
        Returns:
            True if sent successfully, False otherwise
        """
        if not self.protocol:
            print("[ERROR] Not connected to device")
            return False
        
        return await self.protocol.send_json(data)
    
    def is_connected(self) -> bool:
        """Check if connected to device"""
        return self.client is not None and self.client.is_connected


# Convenience function for one-liner JSON exchange
async def simple_json_exchange(device_name: str, request_data: dict, timeout: float = 30.0) -> dict:
    """
    One-liner function for simple JSON request-response exchange
    
    Args:
        device_name: Name of BLE device to connect to
        request_data: JSON data to send
        timeout: Total timeout for operation
        
    Returns:
        Response JSON data or None if failed
    """
    response_data = None
    
    def on_data_received(data: bytes):
        print("[APP] Complete data received successfully!")
        
        # Print received file content to console
        print(f"[FILE] Received file content ({len(data)} bytes):")
        print("=== FILE START ===")
        
        # Print the content in chunks of 512 bytes (like ESP32)
        PRINT_CHUNK_SIZE = 512
        data_len = len(data)
        
        for i in range(0, data_len, PRINT_CHUNK_SIZE):
            chunk_size = min(PRINT_CHUNK_SIZE, data_len - i)
            chunk_data = data[i:i + chunk_size]
            
            # Try to decode as text, fallback to hex if binary
            try:
                chunk_text = chunk_data.decode('utf-8')
                print(chunk_text, end='')
            except UnicodeDecodeError:
                # Print as hex if not valid UTF-8
                hex_data = ' '.join(f'{b:02x}' for b in chunk_data)
                print(f"[HEX] {hex_data}")
        
        print("\n=== FILE END ===")
        
        # Try to parse as JSON for additional info
        try:
            json_data = json.loads(data.decode('utf-8'))
            print(f"[JSON] Parsed JSON data: {json_data}")
        except:
            print("[INFO] Data is not valid JSON")
        
        nonlocal response_data
        response_data = data
    
    client = SimpleBLEClient(device_name, data_callback=on_data_received)
    
    try:
        # Connect to device
        if not await client.connect():
            return None
        
        # Send request
        if not await client.send_json(request_data):
            return None
        
        # Wait for response (timeout handled by protocol)
        print(f"[EXCHANGE] Waiting for response (timeout: {timeout}s)...")
        start_time = asyncio.get_event_loop().time()
        while response_data is None and (asyncio.get_event_loop().time() - start_time) < timeout:
            await asyncio.sleep(0.1)
        
        if response_data:
            print(f"[EXCHANGE] Response received after {asyncio.get_event_loop().time() - start_time:.1f}s")
        else:
            print(f"[EXCHANGE] No response received within {timeout}s timeout")
        
        return response_data
        
    except Exception as e:
        print(f"[ERROR] JSON exchange failed: {e}")
        return None
    
    finally:
        await client.disconnect()


# Demo usage
async def demo():
    """Demo usage of Simple BLE Client"""
    print("=== Simple BLE Client Demo ===")
    
    # Method 1: Manual connection and exchange  
    print("\n1. Manual connection and exchange:")
    
    def on_data_received(data: bytes):
        print("[APP] Complete data received successfully!")
        
        # Print received file content to console
        print(f"[FILE] Received file content ({len(data)} bytes):")
        print("=== FILE START ===")
        
        # Print the content in chunks of 512 bytes (like ESP32)
        PRINT_CHUNK_SIZE = 512
        data_len = len(data)
        
        for i in range(0, data_len, PRINT_CHUNK_SIZE):
            chunk_size = min(PRINT_CHUNK_SIZE, data_len - i)
            chunk_data = data[i:i + chunk_size]
            
            # Try to decode as text, fallback to hex if binary
            try:
                chunk_text = chunk_data.decode('utf-8')
                print(chunk_text, end='')
            except UnicodeDecodeError:
                # Print as hex if not valid UTF-8
                hex_data = ' '.join(f'{b:02x}' for b in chunk_data)
                print(f"[HEX] {hex_data}")
        
        print("\n=== FILE END ===")
        
        # Try to parse as JSON for additional info
        try:
            json_data = json.loads(data.decode('utf-8'))
            print(f"[JSON] Parsed JSON data: {json_data}")
        except:
            print("[INFO] Data is not valid JSON")
    
    def on_progress(current: int, total: int, is_receiving: bool):
        direction = "RX" if is_receiving else "TX"
        print(f"[PROGRESS] {direction}: {current}/{total}")
    
    client = SimpleBLEClient(DEFAULT_DEVICE_NAME, on_data_received, on_progress)
    
    if await client.connect():
        # Send some test data
        test_data = {"command": "get_status", "timestamp": 1234567890}
        
        if await client.send_json(test_data):
            print("[DEMO] Data sent, waiting for response...")
            # Wait for callback response (handled by protocol)
            await asyncio.sleep(5.0)
        
        await client.disconnect()
    
    # Method 2: One-liner exchange
    print("\n2. One-liner JSON exchange:")
    
    request = {"test": "ping", "value": 42}
    response = await simple_json_exchange(DEFAULT_DEVICE_NAME, request, timeout=15.0)
    
    if response:
        print(f"[DEMO] One-liner response: {response}")
    else:
        print("[DEMO] One-liner exchange failed")


async def send_json_file(file_path: str, device_name: str = DEFAULT_DEVICE_NAME):
    """
    Send JSON file to BLE device
    
    Args:
        file_path: Path to JSON file
        device_name: BLE device name
    """
    try:
        # Read JSON file
        with open(file_path, 'r', encoding='utf-8') as f:
            json_data = json.load(f)
        
        print(f"[FILE] Loaded JSON from {file_path}: {json_data}")
        
        # Send via BLE
        response = await simple_json_exchange(device_name, json_data, timeout=45.0)
        
        if response:
            print(f"[SUCCESS] Response received: {response}")
            return response
        else:
            print("[ERROR] No response received")
            return None
            
    except FileNotFoundError:
        print(f"[ERROR] File '{file_path}' not found")
        return None
    except json.JSONDecodeError as e:
        print(f"[ERROR] Invalid JSON in '{file_path}': {e}")
        return None
    except Exception as e:
        print(f"[ERROR] Failed to send file: {e}")
        return None


if __name__ == "__main__":
    if len(sys.argv) > 1:
        # File mode: send JSON file
        json_file = sys.argv[1]
        
        # Parse device name properly instead of idiotic one-liner
        if len(sys.argv) > 2:
            device_name = sys.argv[2]
        else:
            device_name = DEFAULT_DEVICE_NAME
        
        print(f"=== Sending JSON file: {json_file} to {device_name} ===")
        
        try:
            asyncio.run(send_json_file(json_file, device_name))
        except KeyboardInterrupt:
            print("\n[INTERRUPTED] Operation cancelled by user")
        except Exception as e:
            print(f"[ERROR] {e}")
    else:
        # Demo mode
        try:
            asyncio.run(demo())
        except KeyboardInterrupt:
            print("\n[DEMO] Interrupted by user")
        except Exception as e:
            print(f"[DEMO] Error: {e}")
