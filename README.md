# ESP32C3 BLE JSON Transfer

Firmware and utility for transferring JSON files over BLE. The ESP32 acts as a
BLE server advertising under the name **BLETT**. A single characteristic is used
for both receiving data and sending it back to the client.

## Building the firmware

The project uses [PlatformIO](https://platformio.org/). To build and upload run:

```bash
pio run --target upload
```

## Python utility

`ble_json_client.py` sends a JSON file to the ESP32 and prints the returned
JSON. It requires the `bleak` package.

```bash
python3 ble_json_client.py path/to/file.json
```

The script searches for a device advertising `BLETT`, writes the contents of the
file to the characteristic and waits for the response which should be identical.
