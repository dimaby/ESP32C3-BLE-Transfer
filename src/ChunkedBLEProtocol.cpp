#include "ChunkedBLEProtocol.h"

// Data characteristic callback class
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

// Control characteristic callback class
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
    : bleServer(server), receiving(false), packet_size(0), num_pkgs_received(0),
      sending(false), currentSendChunk(0), totalSendChunks(0) {
    
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
    
    // Check if it's an ACK message from Python client
    if (receivedData.length() > 4 && receivedData.substr(0, 4) == "ACK_") {
        // Extract chunk number from ACK_N message
        std::string chunkStr = receivedData.substr(4);
        int ackChunkNumber = std::atoi(chunkStr.c_str());
        
        Serial.printf("[ACK] Received ACK for chunk %d\n", ackChunkNumber);
        
        // If we're sending data, continue with next chunk
        if (sending && ackChunkNumber == currentSendChunk) {
            Serial.println("[SEND] ACK received, sending next chunk...");
            sendNextChunk();
        } else {
            Serial.printf("[WARNING] Unexpected ACK: chunk %d, expected %d, sending=%s\n", 
                         ackChunkNumber, currentSendChunk, sending ? "true" : "false");
        }
        return;
    }
    
    // Handle binary control messages
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

// Send data (full implementation - ESP32 to Client data transfer)
bool ChunkedBLEProtocol::sendData(const std::string& data) {
    Serial.println("[DEBUG] sendData() method called");
    
    if (sending) {
        Serial.println("[ERROR] Already sending data, cannot start new transfer");
        return false;
    }
    
    if (data.empty()) {
        Serial.println("[ERROR] Cannot send empty data");
        return false;
    }
    
    Serial.printf("[SEND] Starting to send %d bytes\n", data.length());
    
    // Start sending process
    startSending(data);
    
    return true;
}

// Start sending process
void ChunkedBLEProtocol::startSending(const std::string& data) {
    sending = true;
    currentSendChunk = 0;
    
    // Calculate chunk size (similar to Python client - use MTU-based size)
    const size_t CHUNK_SIZE = 512; // Same as Python client
    totalSendChunks = (data.length() + CHUNK_SIZE - 1) / CHUNK_SIZE;
    
    Serial.printf("[SEND] Preparing %d chunks of max %d bytes each\n", totalSendChunks, CHUNK_SIZE);
    
    // Prepare all chunks
    prepareSendChunks(data);
    
    // Send first chunk
    sendNextChunk();
}

// Prepare send chunks
void ChunkedBLEProtocol::prepareSendChunks(const std::string& data) {
    const size_t CHUNK_SIZE = 512;
    sendChunks.clear();
    
    for (size_t i = 0; i < data.length(); i += CHUNK_SIZE) {
        size_t chunkSize = std::min(CHUNK_SIZE, data.length() - i);
        std::string chunk = data.substr(i, chunkSize);
        sendChunks.push_back(chunk);
    }
    
    Serial.printf("[SEND] Prepared %d chunks\n", sendChunks.size());
}

// Send next chunk
void ChunkedBLEProtocol::sendNextChunk() {
    if (currentSendChunk >= totalSendChunks || currentSendChunk >= sendChunks.size()) {
        // Sending complete
        completeSending(true);
        return;
    }
    
    const std::string& chunk = sendChunks[currentSendChunk];
    
    Serial.printf("[SEND] Sending chunk %d/%d (%d bytes)\n", 
                  currentSendChunk + 1, totalSendChunks, chunk.length());
    
    // Send chunk over BLE
    dataCharacteristic->setValue((uint8_t*)chunk.c_str(), chunk.length());
    dataCharacteristic->notify();
    
    currentSendChunk++;
    
    // TODO: Wait for ACK before sending next chunk
    // For now, only send one chunk and stop (no recursive call)
    Serial.println("[SEND] Chunk sent, waiting for ACK implementation...");
}

// Complete sending process
void ChunkedBLEProtocol::completeSending(bool success) {
    sending = false;
    currentSendChunk = 0;
    totalSendChunks = 0;
    sendChunks.clear();
    
    if (success) {
        Serial.println("[SEND] Data sending completed successfully");
        stats.transfersCompleted++;
    } else {
        Serial.println("[SEND] Data sending failed");
    }
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

// Utility methods
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