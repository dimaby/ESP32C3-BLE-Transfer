#include <Arduino.h>
#include <BLEDevice.h>
#include <BLEServer.h>
// #include "ChunkedBLEProtocol.h"
#include "ChunkedBLEStub.h"

// Global objects
BLEServer* pServer = nullptr;
ChunkedBLEProtocol* protocol = nullptr;

// Application callbacks
void onDataReceived(const std::string& data) {
    Serial.println("[APP] Complete data received successfully!");
    
    // Print received file content to console
    Serial.printf("[FILE] Received file content (%d bytes):\n", data.length());
    Serial.println("=== FILE START ===");
    
    // Print the content directly without creating string copies (memory optimization)
    const size_t PRINT_CHUNK_SIZE = 512;
    size_t dataLen = data.length();
    const char* dataPtr = data.c_str();
    
    for (size_t i = 0; i < dataLen; i += PRINT_CHUNK_SIZE) {
        size_t chunkSize = std::min(PRINT_CHUNK_SIZE, dataLen - i);
        // Print directly from original string without copying
        Serial.write(dataPtr + i, chunkSize);
    }
    
    Serial.println("\n=== FILE END ===");
    
    // Process received data here (JSON parsing, etc.)
    Serial.println("[APP] Processing complete, will respond in 5 seconds");
    
    delay(5000); // Simulate processing time
    
    Serial.println("[APP] Sending response back to client...");
    if (protocol && protocol->isDeviceConnected()) {
        protocol->sendData(data); // Echo back the same data
        Serial.println("[APP] Response sent successfully");
    }
}

void onConnectionChanged(bool connected) {
    if (connected) {
        Serial.println("[APP] Client connected - ready for data exchange");
    } else {
        Serial.println("[APP] Client disconnected - clearing pending responses");
        
        // Restart advertising for next connection
        Serial.println("[APP] Connection lost, restarting advertising");
        BLEDevice::startAdvertising();
        Serial.println("[BLE] Advertising restarted");
    }
}

void onProgress(int current, int total, bool isReceiving) {
    const char* direction = isReceiving ? "Receiving" : "Sending";
    Serial.printf("[PROGRESS] %s: %d/%d chunks\n", direction, current, total);
}

void setup() {
    Serial.begin(115200);
    Serial.println("[SETUP] Starting ESP32 BLE JSON Transfer Server");
    
    // Initialize BLE
    BLEDevice::init("BLE-Chunked");
    Serial.println("[BLE] BLE device initialized");
    
    // Create BLE server
    pServer = BLEDevice::createServer();
    Serial.println("[BLE] BLE server created");
    
    // Create chunked protocol (handles ALL BLE setup internally!)
    protocol = new ChunkedBLEProtocol(pServer);
    
    // Set up callbacks
    protocol->setDataReceivedCallback(onDataReceived);
    protocol->setConnectionCallback(onConnectionChanged);
    protocol->setProgressCallback(onProgress);
    
    // Start advertising after protocol initialization
    BLEDevice::startAdvertising();
    Serial.println("[BLE] Advertising started");
    
    Serial.println("[SETUP] ChunkedBLEProtocol initialized with callbacks");
    Serial.println("[SETUP] Server ready for connections!");
}

void loop() {
    // Protocol handles everything automatically via callbacks
    // No manual polling or state management needed
    delay(1000);
}
