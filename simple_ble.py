#!/usr/bin/env python3
"""
Simple BLE Functions - Functional API for BLE JSON data exchange
Clean functional approach without classes, direct ChunkedBLEProtocol usage
"""

import asyncio
import json
import sys
from bleak import BleakScanner, BleakClient
from chunked_ble import ChunkedBLEProtocol

DEFAULT_DEVICE_NAME = "BLE-Chunked"


async def scan_for_device(device_name: str, timeout: float = 10.0) -> str:
    """
    Scan for BLE device by name
    
    Args:
        device_name: Name of target BLE device
        timeout: Scan timeout in seconds
        
    Returns:
        Device address if found, None otherwise
    """
    try:
        print(f"[SCAN] Scanning for BLE devices...")
        
        devices = await BleakScanner.discover(timeout=timeout)
        
        for device in devices:
            name = device.name or "Unknown"
            print(f"[SCAN] Found: {name} ({device.address})")
            
            if name == device_name:
                print(f"[SCAN] Target device found: {name} at {device.address}")
                return device.address
        
        print(f"[ERROR] Target device '{device_name}' not found")
        return None
        
    except Exception as e:
        print(f"[ERROR] Scan failed: {e}")
        return None


async def connect_and_initialize(device_address: str, data_callback=None, progress_callback=None):
    """
    Connect to BLE device and initialize protocol
    
    Args:
        device_address: BLE device address
        data_callback: Optional callback for received data
        progress_callback: Optional callback for transfer progress
        
    Returns:
        Tuple of (BleakClient, ChunkedBLEProtocol) or (None, None) if failed
    """
    try:
        print(f"[BLE] Connecting to {device_address}...")
        client = BleakClient(device_address)
        await client.connect()
        print(f"[BLE] Connected successfully")
        
        # Initialize protocol
        protocol = ChunkedBLEProtocol(client)
        
        # Set callbacks if provided
        if data_callback:
            protocol.set_data_received_callback(data_callback)
        if progress_callback:
            protocol.set_progress_callback(progress_callback)
        
        # Initialize protocol
        if not await protocol.initialize():
            print("[ERROR] Protocol initialization failed")
            await client.disconnect()
            return None, None
        
        print("[SUCCESS] BLE connection and protocol initialization complete")
        return client, protocol
        
    except Exception as e:
        print(f"[ERROR] Connection failed: {e}")
        return None, None


async def disconnect_client(client):
    """
    Safely disconnect BLE client
    
    Args:
        client: BleakClient to disconnect
    """
    try:
        if client and client.is_connected:
            await client.disconnect()
            print("[BLE] Disconnected")
    except Exception as e:
        print(f"[ERROR] Disconnect failed: {e}")


async def json_exchange(device_name: str, request_data: dict):
    """
    One-liner function for simple JSON request-response exchange
    
    Args:
        device_name: Name of BLE device to connect to
        request_data: JSON data to send
        
    Returns:
        Response JSON data or None if failed
    """
    response_data = None
    
    def on_data_received(data: bytes):
        nonlocal response_data
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
                # chunk_data is already a string from chunked_ble.py
                chunk_text = chunk_data  # No need to decode again
                print(chunk_text, end='')
            except Exception:
                # Fallback for any other errors
                print(f"[ERROR] Cannot display chunk data: {repr(chunk_data)}")
        
        print("\n=== FILE END ===")
        
        # Try to parse as JSON for additional info
        try:
            json_data = json.loads(data)  # data is already a string
            print(f"[JSON] Parsed JSON data successfully: {len(str(json_data))} characters")
        except json.JSONDecodeError as e:
            print(f"[INFO] Data is not valid JSON: {e}")
        except Exception as e:
            print(f"[ERROR] JSON parsing failed: {e}")
        
        response_data = data
    
    client = None
    try:
        # Scan for device
        device_address = await scan_for_device(device_name)
        if not device_address:
            return None
        
        # Connect and initialize
        client, protocol = await connect_and_initialize(device_address, data_callback=on_data_received)
        if not client or not protocol:
            return None
        
        # Send request
        if not await protocol.send_data(json.dumps(request_data, indent=2)):
            return None
        
        print("[EXCHANGE] Data sent successfully, waiting for ESP32 to process and respond...")
        
        # Wait for response with timeout (ESP32 needs time to process - 5s delay + response time)
        print("[EXCHANGE] Waiting for response...")
        start_time = asyncio.get_event_loop().time()
        timeout_seconds = 10.0  # Give ESP32 enough time to process (5s delay + 5s buffer)
        last_chunk_time = start_time
        chunk_timeout = 2.0  # Timeout between chunks
        
        while response_data is None:
            current_time = asyncio.get_event_loop().time()
            elapsed = current_time - start_time
            
            # Check if we're receiving chunks and detect end by timeout
            if protocol.receiving and protocol.received_chunks:
                time_since_last_chunk = current_time - last_chunk_time
                if time_since_last_chunk > chunk_timeout:
                    print(f"[RECEIVE] No new chunks for {time_since_last_chunk:.1f}s, completing reception...")
                    protocol.complete_receiving_manually()
                    # Give it a moment to process
                    await asyncio.sleep(0.5)
                    break
                    
                # Update last chunk time if we got new chunks
                if hasattr(protocol, '_last_chunk_time'):
                    last_chunk_time = protocol._last_chunk_time
            
            if elapsed > timeout_seconds:
                print(f"[TIMEOUT] No response received after {elapsed:.1f}s")
                # If we have chunks, try to complete them
                if protocol.receiving and protocol.received_chunks:
                    print("[TIMEOUT] Completing partial reception...")
                    protocol.complete_receiving_manually()
                    await asyncio.sleep(0.5)
                    break
                return None
                
            await asyncio.sleep(0.1)
        
        print(f"[EXCHANGE] Response received after {asyncio.get_event_loop().time() - start_time:.1f}s")
        return response_data
        
    except Exception as e:
        print(f"[ERROR] JSON exchange failed: {e}")
        return None
    
    finally:
        if client:
            await disconnect_client(client)


async def send_json_file(file_path: str, device_name: str = DEFAULT_DEVICE_NAME):
    """
    Send JSON file to BLE device
    
    Args:
        file_path: Path to JSON file
        device_name: BLE device name
        
    Returns:
        Response data or None if failed
    """
    try:
        # Read JSON file
        with open(file_path, 'r', encoding='utf-8') as f:
            json_data = json.load(f)
        
        print(f"[FILE] Loaded JSON from {file_path}")
        
        # Send via BLE
        response = await json_exchange(device_name, json_data)
        
        if response:
            print(f"[SUCCESS] Response received ({len(response)} bytes)")
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
        
        # Parse device name
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
        print("Usage: python simple_ble.py <json_file> [device_name]")
        print("Example: python simple_ble.py test.json BLE-Chunked")
