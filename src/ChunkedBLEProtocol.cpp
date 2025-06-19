#include "ChunkedBLEProtocol.h"

// Data characteristic callback class (based on OTA algorithm)
class DataCharacteristicCallbacks : public BLECharacteristicCallbacks {
private:
    ChunkedBLEProtocol* protocol;
    
public:
    DataCharacteristicCallbacks(ChunkedBLEProtocol* p) : protocol(p) {}
    
    void onWrite(BLECharacteristic* pCharacteristic) override {
        std::string value = pCharacteristic->getValue();
        if (value.length() > 0) {
            Serial.printf("[DATA] Received %d bytes\n", value.length());
            protocol->processReceivedChunk((const uint8_t*)value.c_str(), value.length());
        }
    }
};

// Control characteristic callback class (based on OTA algorithm)
class ControlCharacteristicCallbacks : public BLECharacteristicCallbacks {
private:
    ChunkedBLEProtocol* protocol;
    
public:
    ControlCharacteristicCallbacks(ChunkedBLEProtocol* p) : protocol(p) {}
    
    void onWrite(BLECharacteristic* pCharacteristic) override {
        std::string value = pCharacteristic->getValue();
        if (value.length() > 0) {
            Serial.printf("[CONTROL] Received %d bytes\n", value.length());
            protocol->handleControlMessage();
        }
    }
};

// Constructor with default UUIDs
ChunkedBLEProtocol::ChunkedBLEProtocol(BLEServer* server) 
    : ChunkedBLEProtocol(server, DEFAULT_SERVICE_UUID, DEFAULT_CHAR_UUID) {
}

// Constructor with custom UUIDs
ChunkedBLEProtocol::ChunkedBLEProtocol(BLEServer* server, const char* serviceUUID, const char* charUUID) 
    : bleServer(server), deviceConnected(false), receiving(false), packet_size(0), num_pkgs_received(0) {
    
    Serial.println("[PROTOCOL] Initializing ChunkedBLE Protocol");
    
    // Create BLE service
    bleService = bleServer->createService(serviceUUID);
    Serial.printf("[BLE] Service created with UUID: %s\n", serviceUUID);
    
    // Create data characteristic
    dataCharacteristic = bleService->createCharacteristic(
        charUUID,
        BLECharacteristic::PROPERTY_WRITE | BLECharacteristic::PROPERTY_NOTIFY
    );
    dataCharacteristic->setCallbacks(new DataCharacteristicCallbacks(this));
    dataCharacteristic->addDescriptor(new BLE2902());
    Serial.printf("[BLE] Data characteristic created with UUID: %s\n", charUUID);
    
    // Create control characteristic
    controlCharacteristic = bleService->createCharacteristic(
        DEFAULT_CONTROL_CHAR_UUID,
        BLECharacteristic::PROPERTY_READ | BLECharacteristic::PROPERTY_WRITE | BLECharacteristic::PROPERTY_NOTIFY
    );
    controlCharacteristic->setCallbacks(new ControlCharacteristicCallbacks(this));
    controlCharacteristic->addDescriptor(new BLE2902());
    
    // Set initial control value
    const uint8_t initial_value = CONTROL_NOP;
    controlCharacteristic->setValue((uint8_t*)&initial_value, 1);
    Serial.printf("[BLE] Control characteristic created with UUID: %s\n", DEFAULT_CONTROL_CHAR_UUID);
    
    // Start the service
    bleService->start();
    Serial.println("[BLE] Service started successfully");
    
    // Reset statistics
    resetStatistics();
    
    Serial.println("[PROTOCOL] ChunkedBLE Protocol initialization complete");
}

// Destructor
ChunkedBLEProtocol::~ChunkedBLEProtocol() {
    // Cleanup handled by BLE library
}

// Handle control messages (based on OTA algorithm)
void ChunkedBLEProtocol::handleControlMessage() {
    std::string receivedData = controlCharacteristic->getValue();
    uint8_t controlValue = receivedData.c_str()[0];
    
    Serial.printf("[CONTROL] Processing control message: 0x%02X\n", controlValue);
    
    switch (controlValue) {
        case CONTROL_REQUEST:
            Serial.println("[CONTROL] Transfer request received");
            startReceiving();
            break;
        case CONTROL_DONE:
            Serial.println("[CONTROL] Transfer done received");
            completeReceiving(true);
            break;
        default:
            Serial.printf("[CONTROL] Unknown control value: 0x%02X\n", controlValue);
            break;
    }
}

// Handle data messages (based on OTA algorithm)
void ChunkedBLEProtocol::handleDataMessage() {
    std::string value = dataCharacteristic->getValue();
    writeReceivedData((const uint8_t*)value.data(), value.length());
}

// Start receiving process (based on OTA startOtaUpdate)
void ChunkedBLEProtocol::startReceiving() {
    Serial.println("[TRANSFER] Starting data reception");
    
    // Initialize receiving state (like in OTA)
    receiving = true;
    num_pkgs_received = 0;
    receivedData.clear();
    
    // Send ACK response (like in OTA)
    uint8_t controlValue = CONTROL_REQUEST_ACK;
    controlCharacteristic->setValue((uint8_t*)&controlValue, 1);
    controlCharacteristic->notify();
    
    Serial.println("[TRANSFER] Sent ACK response, ready to receive data");
}

// Complete receiving process (based on OTA completeOtaUpdate)
void ChunkedBLEProtocol::completeReceiving(bool success) {
    receiving = false;
    uint8_t controlValue;
    
    if (success && !receivedData.empty()) {
        Serial.printf("[TRANSFER] Reception completed successfully! Total: %d bytes\n", receivedData.size());
        
        // Convert received data to string for callback
        std::string completeData(receivedData.begin(), receivedData.end());
        
        // Call application callback
        if (dataReceivedCallback) {
            dataReceivedCallback(completeData);
        }
        
        controlValue = CONTROL_DONE_ACK; // DONE_ACK
        stats.transfersCompleted++; // Update stats only on success
    } else {
        Serial.println("[ERROR] Reception failed or no data received");
        controlValue = CONTROL_DONE_NAK; // DONE_NAK
    }
    
    // Send final ACK/NAK (like in OTA)
    controlCharacteristic->setValue((uint8_t*)&controlValue, 1);
    controlCharacteristic->notify();
    
    Serial.printf("[TRANSFER] Sent final %s response\n", 
                  (controlValue == CONTROL_DONE_ACK) ? "ACK" : "NAK");
    
    // Clear received data buffer
    receivedData.clear();
    num_pkgs_received = 0;
}

// Write received data (based on OTA writeOtaData)
void ChunkedBLEProtocol::writeReceivedData(const uint8_t* data, size_t length) {
    if (receiving) {
        // Append data to buffer (like in OTA)
        receivedData.insert(receivedData.end(), data, data + length);
        
        num_pkgs_received++;
        
        Serial.printf("[DATA] Received chunk %d (%d bytes), total: %d bytes\n", 
                      num_pkgs_received, length, receivedData.size());
        
        // Simple progress callback
        if (progressCallback) {
            progressCallback(num_pkgs_received, 0, true); // 0 = unknown total
        }
    } else {
        Serial.println("[WARNING] Received data but not in receiving mode");
    }
}

// Process received chunk (public interface)
void ChunkedBLEProtocol::processReceivedChunk(const uint8_t* data, size_t length) {
    writeReceivedData(data, length);
}

// Send data (stub implementation - ESP32 is primarily a receiver like in OTA)
bool ChunkedBLEProtocol::sendData(const std::string& data) {
    if (!deviceConnected) {
        Serial.println("[ERROR] Cannot send data - device not connected");
        return false;
    }
    
    Serial.printf("[SEND] Stub: Would send %d bytes (not implemented yet)\n", data.length());
    
    // TODO: Implement sending when needed
    // For now, just minimal statistics like in OTA
    
    return true;
}

// Callback setters
void ChunkedBLEProtocol::setDataReceivedCallback(DataReceivedCallback callback) {
    dataReceivedCallback = callback;
}

void ChunkedBLEProtocol::setConnectionCallback(ConnectionCallback callback) {
    connectionCallback = callback;
}

void ChunkedBLEProtocol::setProgressCallback(ProgressCallback callback) {
    progressCallback = callback;
}

// Connection handling
void ChunkedBLEProtocol::handleConnectionChange(bool connected) {
    deviceConnected = connected;
    if (connectionCallback) {
        connectionCallback(connected);
    }
    
    if (!connected) {
        // Reset transfer state on disconnect
        receiving = false;
        receivedData.clear();
        num_pkgs_received = 0;
    }
}

// Utility methods
bool ChunkedBLEProtocol::isDeviceConnected() const {
    return deviceConnected;
}

ChunkedBLEProtocol::TransferStats ChunkedBLEProtocol::getStatistics() const {
    return stats;
}

void ChunkedBLEProtocol::resetStatistics() {
    stats = TransferStats();
}

bool ChunkedBLEProtocol::isTransferInProgress() const {
    return receiving;
}

void ChunkedBLEProtocol::cancelCurrentTransfer(const char* reason) {
    if (receiving) {
        Serial.printf("[TRANSFER] Cancelling transfer: %s\n", reason);
        receiving = false;
        receivedData.clear();
        num_pkgs_received = 0;
    } else {
        Serial.println("[TRANSFER] No transfer in progress to cancel");
    }
}
