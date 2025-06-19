#include <Arduino.h>
#include <BLEDevice.h>
#include <BLEServer.h>
#include "ChunkedBLEProtocol.h"

// Global objects
BLEServer* pServer = nullptr;
ChunkedBLEProtocol* protocol = nullptr;

// Application callbacks
void onDataReceived(const std::string& data) {
    Serial.printf("[APP] Complete data received successfully at %lu ms!\n", millis());
    
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
    Serial.printf("[APP] Processing complete at %lu ms, will respond in 5 seconds\n", millis());
    
    delay(5000); // Simulate processing time
    
    Serial.printf("[APP] Sending response back to client at %lu ms...\n", millis());

    if (protocol) {
        protocol->sendData(data); // Echo back the same data
        Serial.println("[APP] Response sent successfully");
    } 
}

void onConnectionChanged(bool connected) {
    if (connected) {
        Serial.println("[APP] Client connected - ready for data exchange");
    } else {
        Serial.printf("[APP] Client disconnected at %lu ms - clearing pending responses\n", millis());
        
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

// BLE Server callbacks for connection monitoring
class BLEServerCallbacksWithLogging : public BLEServerCallbacks {
public:
    void onConnect(BLEServer* pServer) override {
        Serial.printf("[BLE] Client connected at %lu ms\n", millis());
    }
    
    void onDisconnect(BLEServer* pServer) override {
        Serial.printf("[BLE] Client disconnected at %lu ms\n", millis());
        Serial.println("[BLE] Reason: BLE stack initiated disconnect");
        
        // Restart advertising automatically
        Serial.println("[BLE] Restarting advertising after disconnect...");
        BLEDevice::startAdvertising();
        Serial.println("[BLE] Advertising restarted");
    }
};

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
    
    // Set up BLE server callbacks
    pServer->setCallbacks(new BLEServerCallbacksWithLogging());
    
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
