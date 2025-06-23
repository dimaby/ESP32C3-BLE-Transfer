#include <Arduino.h>
#include <BLEDevice.h>
#include <BLEServer.h>
#include <BLEUtils.h>
#include <BLE2902.h>
#include "ChunkedBLEProtocol.h"
#include "common.h"

// Global objects
BLEServer* pServer = nullptr;
ChunkedBLEProtocol* protocol = nullptr;

// Application callbacks
void onDataReceived(const std::string& data) {
    LOG_INFO("[APP] Complete data received successfully at %lu ms!", millis());
    
    // Print received file content to console
    LOG_INFO("[FILE] Received file content (%d bytes):", data.length());
    LOG_INFO("=== FILE START ===");
    LOG_INFO(data.c_str());
    LOG_INFO("=== FILE END ===");
    
    // Process received data here (JSON parsing, etc.)
    LOG_INFO("[APP] Processing complete at %lu ms, will respond in 5 seconds", millis());
    
    delay(5000); // Simulate processing time
    
    LOG_INFO("[APP] Sending response back to client at %lu ms...", millis());

    if (protocol) {
        protocol->sendData(data); // Echo back the same data
        LOG_INFO("[APP] Response sent successfully");
    } 
}

void onProgress(int current, int total, bool isReceiving) {
    const char* direction = isReceiving ? "Receiving" : "Sending";
    LOG_INFO("[PROGRESS] %s: %d/%d chunks", direction, current, total);
}

class MyServerCallbacks : public BLEServerCallbacks {
public:
    void onConnect(BLEServer* s) override {
        LOG_INFO("[BLE] Client connected");
    }
    
    void onDisconnect(BLEServer* pServer) override {
        LOG_INFO("[BLE] Client disconnected");
        
        // Restart advertising automatically
        LOG_INFO("[BLE] Restarting advertising after disconnect...");
        BLEDevice::startAdvertising();
        LOG_INFO("[BLE] Advertising restarted");
    }
};

void setup() {
    Serial.begin(115200);
    LOG_INFO("[SETUP] Starting ESP32 BLE JSON Transfer Server");
    
    // Initialize BLE
    BLEDevice::init("BLE-Chunked");
    LOG_INFO("[BLE] BLE device initialized");
    
    // Create BLE server
    pServer = BLEDevice::createServer();
    LOG_INFO("[BLE] BLE server created");
    
    // Set server callbacks
    pServer->setCallbacks(new MyServerCallbacks());
    
    // Create chunked protocol (handles ALL BLE setup internally!)
    protocol = new ChunkedBLEProtocol(pServer);
    
    // Set up callbacks
    protocol->setDataReceivedCallback(onDataReceived);
    protocol->setProgressCallback(onProgress);
    
    // Set fixed chunk size
    protocol->setChunkSize(512);
    LOG_INFO("[SETUP] Fixed chunk size set to 512 bytes");
    
    // Start advertising after protocol initialization
    BLEDevice::startAdvertising();
    LOG_INFO("[BLE] Advertising started");
    
    LOG_INFO("[SETUP] ChunkedBLEProtocol initialized with callbacks");
    LOG_INFO("[SETUP] Server ready for connections!");
}

void loop() {
    delay(100);  
}
