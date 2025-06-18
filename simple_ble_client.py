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


class SimpleBLEClient:
    """
    Simple BLE Client with Chunked Protocol
    
    Provides ultra-simple API for BLE JSON data exchange.
    Usage:
        client = SimpleBLEClient("BLETT")
        client.set_data_received_callback(on_data)
        await client.connect()
        await client.send_json({"test": "data"})
        response = await client.receive_json()
    """
    
    def __init__(self, device_name: str = "BLETT"):
        """
        Initialize Simple BLE Client
        
        Args:
            device_name: Name of target BLE device
        """
        self.device_name = device_name
        self.client = None
        self.protocol = None
        self.target_device = None
        
        print(f"[INIT] Simple BLE client for device: {device_name}")
    
    def set_data_received_callback(self, callback) -> None:
        """Set callback for received data"""
        self._data_callback = callback
    
    def set_progress_callback(self, callback) -> None:
        """Set callback for transfer progress"""
        self._progress_callback = callback
    
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
            
            # Initialize protocol (C++-like API)
            self.protocol = ChunkedBLEProtocol(self.client)
            
            # Set callbacks if provided
            if hasattr(self, '_data_callback'):
                self.protocol.set_data_received_callback(self._data_callback)
            if hasattr(self, '_progress_callback'):
                self.protocol.set_progress_callback(self._progress_callback)
            
            # Initialize protocol (finds service/characteristic automatically)
            if not await self.protocol.initialize():
                print("[ERROR] Protocol initialization failed")
                await self.disconnect()
                return False
            
            print("[SUCCESS] BLE connection and protocol initialization complete")
            return True
            
        except Exception as e:
            print(f"[ERROR] Connection failed: {e}")
            await self.disconnect()
            return False
    
    async def disconnect(self) -> None:
        """Disconnect from device"""
        try:
            if self.protocol:
                await self.protocol.cleanup()
                self.protocol = None
            
            if self.client and self.client.is_connected:
                await self.client.disconnect()
                print("[BLE] Disconnected")
            
            self.client = None
            
        except Exception as e:
            print(f"[ERROR] Disconnect failed: {e}")
    
    async def send_json(self, data: dict) -> bool:
        """
        Send JSON data
        
        Args:
            data: Dictionary to send as JSON
            
        Returns:
            True if sent successfully, False otherwise
        """
        if not self.protocol:
            print("[ERROR] Not connected")
            return False
        
        try:
            json_data = json.dumps(data).encode('utf-8')
            return await self.protocol.send_data(json_data)
        except Exception as e:
            print(f"[ERROR] Failed to send JSON: {e}")
            return False
    
    async def send_data(self, data: bytes) -> bool:
        """
        Send raw bytes data
        
        Args:
            data: Raw bytes to send
            
        Returns:
            True if sent successfully, False otherwise
        """
        if not self.protocol:
            print("[ERROR] Not connected")
            return False
        
        return await self.protocol.send_data(data)
    
    async def receive_json(self, timeout: float = 30.0) -> dict:
        """
        Wait for and receive JSON data
        
        Args:
            timeout: Timeout in seconds
            
        Returns:
            Received dictionary or None if timeout/error
        """
        if not self.protocol:
            print("[ERROR] Not connected")
            return None
        
        try:
            data = await self.protocol.receive_data(timeout)
            if data:
                return json.loads(data.decode('utf-8'))
            return None
        except Exception as e:
            print(f"[ERROR] Failed to receive JSON: {e}")
            return None
    
    async def receive_data(self, timeout: float = 30.0) -> bytes:
        """
        Wait for and receive raw bytes data
        
        Args:
            timeout: Timeout in seconds
            
        Returns:
            Received bytes or None if timeout/error
        """
        if not self.protocol:
            print("[ERROR] Not connected")
            return None
        
        return await self.protocol.receive_data(timeout)
    
    @property
    def is_connected(self) -> bool:
        """Check if connected to device"""
        return self.client and self.client.is_connected


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
    client = SimpleBLEClient(device_name)
    
    try:
        # Connect
        if not await client.connect():
            return None
        
        # Send request
        if not await client.send_json(request_data):
            return None
        
        # Wait for response
        response = await client.receive_json(timeout)
        
        return response
        
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
        try:
            json_data = json.loads(data.decode('utf-8'))
            print(f"[CALLBACK] Received JSON: {json_data}")
        except:
            print(f"[CALLBACK] Received raw data: {len(data)} bytes")
    
    def on_progress(current: int, total: int, is_receiving: bool):
        direction = "RX" if is_receiving else "TX"
        print(f"[PROGRESS] {direction}: {current}/{total}")
    
    client = SimpleBLEClient("BLETT")
    client.set_data_received_callback(on_data_received)
    client.set_progress_callback(on_progress)
    
    if await client.connect():
        # Send some test data
        test_data = {"command": "get_status", "timestamp": 1234567890}
        
        if await client.send_json(test_data):
            print("[DEMO] Data sent, waiting for response...")
            response = await client.receive_json(timeout=10.0)
            
            if response:
                print(f"[DEMO] Response received: {response}")
            else:
                print("[DEMO] No response received")
        
        await client.disconnect()
    
    # Method 2: One-liner exchange
    print("\n2. One-liner JSON exchange:")
    
    request = {"test": "ping", "value": 42}
    response = await simple_json_exchange("BLETT", request, timeout=15.0)
    
    if response:
        print(f"[DEMO] One-liner response: {response}")
    else:
        print("[DEMO] One-liner exchange failed")


async def send_json_file(file_path: str, device_name: str = "BLETT"):
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
        response = await simple_json_exchange(device_name, json_data, timeout=30.0)
        
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
        device_name = sys.argv[2] if len(sys.argv) > 2 else "BLETT"
        
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
