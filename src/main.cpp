#include <Arduino.h>
#include <BLEDevice.h>
#include <BLEServer.h>
#include <BLEUtils.h>
#include <BLE2902.h>
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
    Serial.print(data.c_str());
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

class MyServerCallbacks : public BLEServerCallbacks {
public:
    void onConnect(BLEServer* s) override {
        Serial.println("[BLE] Client connected");
    }
    
    void onDisconnect(BLEServer* pServer) override {
        Serial.println("[BLE] Client disconnected");
        
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
    
    // Set server callbacks
    pServer->setCallbacks(new MyServerCallbacks());
    
    // Create chunked protocol (handles ALL BLE setup internally!)
    protocol = new ChunkedBLEProtocol(pServer);
    
    // Set up callbacks
    protocol->setDataReceivedCallback(onDataReceived);
    protocol->setConnectionCallback(onConnectionChanged);
    protocol->setProgressCallback(onProgress);
    
    // Set fixed chunk size
    protocol->setChunkSize(512);
    Serial.println("[SETUP] Fixed chunk size set to 512 bytes");
    
    // Start advertising after protocol initialization
    BLEDevice::startAdvertising();
    Serial.println("[BLE] Advertising started");
    
    Serial.println("[SETUP] ChunkedBLEProtocol initialized with callbacks");
    Serial.println("[SETUP] Server ready for connections!");
}

void loop() {
    delay(100);  
}
