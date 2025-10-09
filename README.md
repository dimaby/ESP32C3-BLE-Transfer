# ESP32C3 Chunked BLE Transfer Protocol (v2)

Lightweight, MTU‑aware BLE file/JSON transfer for ESP32‑C3 with a **two‑phase handshake** and optional per‑chunk acknowledgments.

---

## ✨ Key Features

| Feature                        | Description                                                                                                                                                |
| ------------------------------ | ---------------------------------------------------------------------------------------------------------------------------------------------------------- |
| **Dynamic chunk sizing**       | Packet size is negotiated from the **central’s ATT\_MTU – 3**; the central sends the chosen size as a 2‑byte little‑endian value before data transmission. |
| **Two‑phase handshake**        | `CONTROL_REQUEST → REQUEST_ACK` starts a session; `CONTROL_DONE → DONE_ACK` finalises it.                                                                  |
| **Optional per‑chunk ACKs**    | When the ESP32 *sends* data it waits for `ACK_N` messages from the central before emitting the next chunk (robust on noisy links).                         |
| **Zero‑copy buffering**        | Incoming chunks are appended to a vector on the ESP32; no per‑chunk heap allocations.                                                                      |
| **Simple API**                 | Just call `protocol.sendData()` (ESP32) or `await protocol.send_data()` (Python).                                                                          |
| **Progress callbacks & stats** | Hook transfer progress or inspect bytes/chunks sent/received.                                                                                              |

---

## 🏗️ Characteristic Layout

| Purpose | UUID (default)                         | Properties              |
| ------- | -------------------------------------- | ----------------------- |
| Data    | `8f8b49a2‑9117‑4e9f‑acfc‑fda4d0db7408` | **WRITE, NOTIFY**       |
| Control | `12345678‑1234‑1234‑1234‑123456789012` | **READ, WRITE, NOTIFY** |
| Service | `5b18eb9b‑747f‑47da‑b7b0‑a4e503f9a00f` | –                       |

---

\### Control Message Bytes

| Name                  | Value  | Direction       | Meaning               |
| --------------------- | ------ | --------------- | --------------------- |
| `CONTROL_NOP`         | `0x00` | –               | Idle / keep‑alive     |
| `CONTROL_REQUEST`     | `0x01` | Central → ESP32 | “I want to send data” |
| `CONTROL_REQUEST_ACK` | `0x02` | ESP32 → Central | Ready to receive      |
| `CONTROL_REQUEST_NAK` | `0x03` | ESP32 → Central | Busy / cannot receive |
| `CONTROL_DONE`        | `0x04` | Central → ESP32 | All chunks sent       |
| `CONTROL_DONE_ACK`    | `0x05` | ESP32 → Central | Data assembled OK     |
| `CONTROL_DONE_NAK`    | `0x06` | ESP32 → Central | Transfer failed       |

*Chunk ACKs*: when the **ESP32 is the sender** the central replies after every chunk with ASCII text `"ACK_{n}"` where *n* is the 0‑based chunk index.

---

## 🔄 Transfer Sequences

\### Central → ESP32 (upload)

```text
Central           ESP32
  │ packet_size → │               (2 bytes on data char)
  │ REQUEST    →  │               (control char)
  │              ←│ REQUEST_ACK
  │ chunks ... →  │               (data char, no chunk ACKs)
  │ DONE       →  │
  │              ←│ DONE_ACK
```

\### ESP32 → Central (download/response)

```text
ESP32             Central
  │ REQUEST    ←  │               (implicit, handled internally)
  │ chunk 0    →  │
  │              ←│ "ACK_0"
  │ chunk 1    →  │
  │              ←│ "ACK_1"
  │  …           …
  │ chunk N    →  │
  │              ←│ "ACK_N"
  │ DONE       →  │
  │              ←│ DONE_ACK
```

---

## 🚀 Quick Start

\### Firmware (PlatformIO)

```bash
git clone <repo>
cd esp32-ble-chunked
pio run -t upload
```

\### Python client

```bash
python3 -m venv myenv
source myenv/bin/activate
pip install bleak
python simple_ble.py test.json  # sends file and waits for response
```

---

## 💻 Usage Examples

\### ESP32 (C++)

```cpp
#include "ChunkedBLEProtocol.h"
ChunkedBLEProtocol proto(pServer);

proto.setDataReceivedCallback([](const std::string& json){
    Serial.println(json.c_str());
    proto.sendData(json);          // echo back
});
```

\### Python

```python
from chunked_ble import ChunkedBLEProtocol
client = BleakClient(address)
await client.connect()

proto = ChunkedBLEProtocol(client)
await proto.initialize()

await proto.send_data(open("config.json").read())
```

---

## ⚙️ Defaults & Tuning

| Parameter                    | Default          | How to change                  |
| ---------------------------- | ---------------- | ------------------------------ |
| Packet size (central‑>ESP32) | `mtu‑3` bytes    | central picks value            |
| Packet size (ESP32‑>central) | 512 bytes        | change `CHUNK_SIZE` constant   |
| Control time‑outs            | 5 s wait for ACK | adjust in Python / C++ loops   |
| Progress callback interval   | every chunk      | override `setProgressCallback` |

---

## 📊 Statistics

Call `proto.getStatistics()` on either side to obtain cumulative counters for bytes/chunks and completed transfers.

---

## 🛠️ Troubleshooting

| Symptom                                     | Fix                                                     |
| ------------------------------------------- | ------------------------------------------------------- |
| **Timeout waiting for REQUEST\_ACK**        | Ensure only one client connected; increase timeout.     |
| **Chunks stop after first** (ESP32→central) | Make sure the central sends `"ACK_n"` after each chunk. |
| **Bleak lost connection**                   | Lower packet size or move devices closer.               |

---

## 📄 License

MIT
