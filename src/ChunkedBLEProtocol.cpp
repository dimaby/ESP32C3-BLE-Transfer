/**
 * ChunkedBLEProtocol - Version 0.1
 * 
 * A protocol for chunked data transfer over BLE for ESP32 devices.
 * 
 * This protocol allows for reliable transfer of large data chunks over BLE by
 * breaking them into smaller pieces and handling acknowledgments.
 * 
 * Integration Guide:
 * ------------------
 * 1. Create a BLEServer instance:
 *    BLEServer* pServer = BLEDevice::createServer();
 * 
 * 2. Initialize the protocol with the server:
 *    ChunkedBLEProtocol* protocol = new ChunkedBLEProtocol(pServer);
 * 
 * 3. Set up callbacks for data reception and connection status:
 *    protocol->setDataReceivedCallback(onDataReceived);
 *    protocol->setProgressCallback(onProgress);
 * 
 * 4. Optionally set chunk size (default is 512 bytes):
 *    protocol->setChunkSize(512);
 * 
 * 5. To send data:
 *    protocol->sendData(yourDataString);
 * 
 * The protocol handles all BLE service and characteristic setup internally,
 * as well as chunking, reassembly, and acknowledgment of data transfers.
 */

#include "ChunkedBLEProtocol.h"
#include "common.h"

// Data characteristic callback class
class DataCharacteristicCallbacks : public BLECharacteristicCallbacks {
private:
    ChunkedBLEProtocol* protocol;
    
public:
    DataCharacteristicCallbacks(ChunkedBLEProtocol* p) : protocol(p) {}
    
    void onWrite(BLECharacteristic* pCharacteristic) override {
        std::string value = pCharacteristic->getValue();
        if (value.length() > 0) {
            LOG_INFO("[DATA] Received %d bytes", value.length());
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
            LOG_INFO("[CONTROL] Received %d bytes", value.length());
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
      sending(false), outgoing_chunk_size(512), currentSendChunk(0), totalSendChunks(0) {
    
    LOG_INFO("[PROTOCOL] Initializing ChunkedBLE Protocol");
    
    // Create BLE service
    bleService = bleServer->createService(serviceUUID);
    LOG_INFO("[BLE] Service created with UUID: %s", serviceUUID);
    
    // Create data characteristic
    dataCharacteristic = bleService->createCharacteristic(
        charUUID,
        BLECharacteristic::PROPERTY_WRITE | BLECharacteristic::PROPERTY_NOTIFY
    );
    dataCharacteristic->setCallbacks(new DataCharacteristicCallbacks(this));
    dataCharacteristic->addDescriptor(new BLE2902());
    LOG_INFO("[BLE] Data characteristic created with UUID: %s", charUUID);
    
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
    LOG_INFO("[BLE] Control characteristic created with UUID: %s", DEFAULT_CONTROL_CHAR_UUID);
    
    // Start the service
    bleService->start();
    LOG_INFO("[BLE] Service started successfully");
    
    // Reset statistics
    resetStatistics();
    
    LOG_INFO("[PROTOCOL] ChunkedBLE Protocol initialization complete");
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
        
        LOG_INFO("[ACK] Received ACK for chunk %d", ackChunkNumber);
        
        // If we're sending data, continue with next chunk
        if (sending && ackChunkNumber == currentSendChunk) {
            LOG_INFO("[SEND] ACK received, sending next chunk...");
            sendNextChunk();
        } else {
            LOG_ERROR("[WARNING] Unexpected ACK: chunk %d, expected %d, sending=%s", 
                         ackChunkNumber, currentSendChunk, sending ? "true" : "false");
        }
        return;
    }
    
    // Handle binary control messages
    uint8_t controlValue = receivedData.c_str()[0];
    
    LOG_INFO("[CONTROL] Processing control message: 0x%02X", controlValue);
    
    switch (controlValue) {
        case CONTROL_REQUEST:
            LOG_INFO("[CONTROL] Transfer request received");
            startReceiving();
            break;
        case CONTROL_DONE:
            LOG_INFO("[CONTROL] Transfer done received");
            completeReceiving(true);
            break;
        default:
            LOG_ERROR("[CONTROL] Unknown control value: 0x%02X", controlValue);
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
    LOG_INFO("[TRANSFER] Starting data reception");
    
    // Initialize receiving state (like in OTA)
    receiving = true;
    num_pkgs_received = 0;
    receivedData.clear();
    
    // Send ACK response (like in OTA)
    uint8_t controlValue = CONTROL_REQUEST_ACK;
    controlCharacteristic->setValue((uint8_t*)&controlValue, 1);
    controlCharacteristic->notify();
    
    LOG_INFO("[TRANSFER] Sent ACK response, ready to receive data");
}

// Complete receiving process (based on OTA completeOtaUpdate)
void ChunkedBLEProtocol::completeReceiving(bool success) {
    receiving = false;
    uint8_t controlValue;
    
    if (success && !receivedData.empty()) {
        LOG_INFO("[TRANSFER] Reception completed successfully! Total: %d bytes", receivedData.size());
        
        // Convert received data to string for callback
        std::string completeData(receivedData.begin(), receivedData.end());
        
        // Call application callback
        if (dataReceivedCallback) {
            dataReceivedCallback(completeData);
        }
        
        controlValue = CONTROL_DONE_ACK; // DONE_ACK
        stats.transfersCompleted++; // Update stats only on success
    } else {
        LOG_ERROR("[ERROR] Reception failed or no data received");
        controlValue = CONTROL_DONE_NAK; // DONE_NAK
    }
    
    // Send final ACK/NAK (like in OTA)
    controlCharacteristic->setValue((uint8_t*)&controlValue, 1);
    controlCharacteristic->notify();
    
    LOG_INFO("[TRANSFER] Sent final %s response", 
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
        
        LOG_INFO("[DATA] Received chunk %d (%d bytes), total: %d bytes", 
                      num_pkgs_received, length, receivedData.size());
        
        // Simple progress callback
        if (progressCallback) {
            progressCallback(num_pkgs_received, 0, true); // 0 = unknown total
        }
    } else {
        LOG_ERROR("[WARNING] Received data but not in receiving mode");
    }
}

// Process received chunk (public interface)
void ChunkedBLEProtocol::processReceivedChunk(const uint8_t* data, size_t length) {
    writeReceivedData(data, length);
}

// Send data (full implementation - ESP32 to Client data transfer)
bool ChunkedBLEProtocol::sendData(const std::string& data) {
    LOG_INFO("[DEBUG] sendData() method called");
    
    if (sending) {
        LOG_ERROR("[ERROR] Already sending data, cannot start new transfer");
        return false;
    }
    
    if (data.empty()) {
        LOG_ERROR("[ERROR] Cannot send empty data");
        return false;
    }
    
    LOG_INFO("[SEND] Starting to send %d bytes", data.length());
    
    // Start sending process
    startSending(data);
    
    return true;
}

// Start sending process
void ChunkedBLEProtocol::startSending(const std::string& data) {
    sending = true;
    currentSendChunk = 0;
    
    // Calculate chunk size (similar to Python client - use MTU-based size)
    totalSendChunks = (data.length() + outgoing_chunk_size - 1) / outgoing_chunk_size;
    
    LOG_INFO("[SEND] Preparing %d chunks of max %d bytes each", totalSendChunks, outgoing_chunk_size);
    
    // Prepare all chunks
    prepareSendChunks(data);
    
    // Send first chunk
    sendNextChunk();
}

// Prepare send chunks
void ChunkedBLEProtocol::prepareSendChunks(const std::string& data) {
    sendChunks.clear();
    
    for (size_t i = 0; i < data.length(); i += outgoing_chunk_size) {
        size_t chunkSize = std::min((size_t)outgoing_chunk_size, data.length() - i);
        std::string chunk = data.substr(i, chunkSize);
        sendChunks.push_back(chunk);
    }
    
    LOG_INFO("[SEND] Prepared %d chunks", sendChunks.size());
}

// Send next chunk
void ChunkedBLEProtocol::sendNextChunk() {
    if (currentSendChunk >= totalSendChunks || currentSendChunk >= sendChunks.size()) {
        // Sending complete
        completeSending(true);
        return;
    }
    
    const std::string& chunk = sendChunks[currentSendChunk];
    
    LOG_INFO("[SEND] Sending chunk %d/%d (%d bytes)", 
                  currentSendChunk + 1, totalSendChunks, chunk.length());
    
    // Send chunk over BLE
    dataCharacteristic->setValue((uint8_t*)chunk.c_str(), chunk.length());
    dataCharacteristic->notify();
    
    currentSendChunk++;
    
    // TODO: Wait for ACK before sending next chunk
    // For now, only send one chunk and stop (no recursive call)
    LOG_INFO("[SEND] Chunk sent, waiting for ACK implementation...");
}

// Complete sending process
void ChunkedBLEProtocol::completeSending(bool success) {
    sending = false;
    currentSendChunk = 0;
    totalSendChunks = 0;
    sendChunks.clear();
    
    if (success) {
        LOG_INFO("[SEND] Data sending completed successfully");
        stats.transfersCompleted++;
    } else {
        LOG_ERROR("[SEND] Data sending failed");
    }
}

// Set chunk size
void ChunkedBLEProtocol::setChunkSize(uint16_t size) {
    outgoing_chunk_size = size;
    LOG_INFO("[PROTOCOL] Chunk size updated to %d bytes", size);
}

// Callback setters
void ChunkedBLEProtocol::setDataReceivedCallback(DataReceivedCallback callback) {
    dataReceivedCallback = callback;
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
        LOG_INFO("[TRANSFER] Cancelling transfer: %s", reason);
        receiving = false;
        receivedData.clear();
        num_pkgs_received = 0;
    } else {
        LOG_INFO("[TRANSFER] No transfer in progress to cancel");
    }
}