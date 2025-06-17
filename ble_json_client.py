#!/usr/bin/env python3
"""Utility to send a JSON file to the BLETT device and print the response."""

import asyncio
import argparse
import json
from pathlib import Path

from bleak import BleakScanner, BleakClient

SERVICE_UUID = "12345678-1234-1234-1234-1234567890ab"
CHAR_UUID = "abcd1234-5678-90ab-cdef-1234567890ab"

def parse_args():
    parser = argparse.ArgumentParser(description="Send JSON file over BLE")
    parser.add_argument("file", type=Path, help="Path to JSON file")
    return parser.parse_args()

async def run(file_path: Path):
    data = file_path.read_text()
    print(f"Searching for BLE device advertising 'BLETT' ...")
    device = await BleakScanner.find_device_by_filter(
        lambda d, ad: d.name == "BLETT"
    )
    if not device:
        print("Device not found")
        return
    async with BleakClient(device) as client:
        print("Connected, sending JSON...")
        await client.write_gatt_char(CHAR_UUID, data.encode())

        def callback(_, payload: bytearray):
            try:
                text = payload.decode()
                json.loads(text)  # validate
                print("Received response:\n", text)
            except Exception as exc:
                print("Invalid JSON received:", exc)
            finally:
                loop = asyncio.get_event_loop()
                loop.stop()

        await client.start_notify(CHAR_UUID, callback)
        await asyncio.get_event_loop().run_forever()


def main():
    args = parse_args()
    asyncio.run(run(args.file))

if __name__ == "__main__":
    main()
