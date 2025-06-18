#include "ChunkedBLEProtocol.h"

// Default UUIDs
const char* ChunkedBLEProtocol::DEFAULT_SERVICE_UUID = "5b18eb9b-747f-47da-b7b0-a4e503f9a00f";
const char* ChunkedBLEProtocol::DEFAULT_CHAR_UUID = "8f8b49a2-9117-4e9f-acfc-fda4d0db7408";
const char* ChunkedBLEProtocol::DEFAULT_CONTROL_CHAR_UUID = "12345678-1234-1234-1234-123456789012";

// Internal callback class for characteristic events
class ChunkedBLEProtocol::DataCharacteristicCallbacks : public BLECharacteristicCallbacks {
private:
    ChunkedBLEProtocol* protocol;
    
public:
    explicit DataCharacteristicCallbacks(ChunkedBLEProtocol* p) : protocol(p) {}
    
    void onWrite(BLECharacteristic *pChar) override {
        std::string value = pChar->getValue();
        if (value.length() >= protocol->HEADER_SIZE) {
            protocol->processReceivedChunk((const uint8_t*)value.c_str(), value.length());
        } else {
            protocol->log("[CHUNK] Received data too small for chunk header");
        }
    }
    
    void onRead(BLECharacteristic *pChar) override {
        protocol->log("[BLE] Characteristic read by client");
    }
    
    void onNotify(BLECharacteristic *pChar) override {
        protocol->log("[BLE] Notification sent to client");
    }
    
    void onStatus(BLECharacteristic *pChar, Status s, uint32_t code) override {
        protocol->log("[BLE] Status callback - Status: %d, Code: %u", s, code);
    }
};

// Internal callback class for control characteristic events
class ChunkedBLEProtocol::ControlCharacteristicCallbacks : public BLECharacteristicCallbacks {
private:
    ChunkedBLEProtocol* protocol;
    
public:
    explicit ControlCharacteristicCallbacks(ChunkedBLEProtocol* p) : protocol(p) {}
    
    void onWrite(BLECharacteristic *pChar) override {
        std::string value = pChar->getValue();
        protocol->processControlMessage((const uint8_t*)value.c_str(), value.length());
    }
    
    void onRead(BLECharacteristic *pChar) override {
        protocol->log("[BLE] Control characteristic read by client");
    }
    
    void onNotify(BLECharacteristic *pChar) override {
        protocol->log("[BLE] Control notification sent to client");
    }
    
    void onStatus(BLECharacteristic *pChar, Status s, uint32_t code) override {
        protocol->log("[BLE] Control status callback - Status: %d, Code: %u", s, code);
    }
};

// Internal callback class for server events
class ChunkedBLEProtocol::ServerCallbacks : public BLEServerCallbacks {
private:
    ChunkedBLEProtocol* protocol;
    
public:
    explicit ServerCallbacks(ChunkedBLEProtocol* p) : protocol(p) {}
    
    void onConnect(BLEServer* pServer) override {
        protocol->log("[BLE] Client connected");
        protocol->log("[BLE] Connected clients count: %d", pServer->getConnectedCount());
        protocol->log("[BLE] MTU will be negotiated to 185 or lower");
        protocol->handleConnectionChange(true);
    }
    
    void onDisconnect(BLEServer* pServer) override {
        protocol->log("[BLE] Client disconnected");
        protocol->log("[BLE] Connected clients count: %d", pServer->getConnectedCount());
        protocol->handleConnectionChange(false);
    }
};

// Constructor with default UUIDs
ChunkedBLEProtocol::ChunkedBLEProtocol(BLEServer* server) 
    : ChunkedBLEProtocol(server, DEFAULT_SERVICE_UUID, DEFAULT_CHAR_UUID) {
}

// Constructor with custom UUIDs  
ChunkedBLEProtocol::ChunkedBLEProtocol(BLEServer* server, const char* serviceUUID, const char* charUUID)
    : bleServer(server), 
      bleService(nullptr), 
      dataCharacteristic(nullptr),
      controlCharacteristic(nullptr),
      currentState(TRANSFER_IDLE),
      receivedData(nullptr),
      totalDataSize(0),
      receivedDataSize(0),
      expectedTotalChunks(0),
      isReceivingTransfer(false),
      deviceConnected(false),
      crc32TableInit(false),
      lastChunkTime(0),
      chunkTimeoutMs(DEFAULT_CHUNK_TIMEOUT_MS),
      expectedGlobalCRC32(0),
      waitingForAck(false),
      lastAckChunk(0),
      ackTimeout(0),
      sendingInProgress(false),
      currentSendingChunks(0),
      currentSendingGlobalCRC(0) {
    
    log("[PROTOCOL] Initializing ChunkedBLEProtocol...");
    
    // Initialize CRC32 table
    initCRC32Table();
    
    // Setup BLE service
    setupBLEService(serviceUUID, charUUID);
    
    log("[PROTOCOL] ChunkedBLEProtocol initialized successfully");
}

// Initialize CRC32 lookup table
void ChunkedBLEProtocol::initCRC32Table() {
    if (crc32TableInit) return;
    
    for (uint32_t i = 0; i < 256; i++) {
        uint32_t crc = i;
        for (uint32_t j = 0; j < 8; j++) {
            if (crc & 1) {
                crc = (crc >> 1) ^ 0xEDB88320;
            } else {
                crc >>= 1;
            }
        }
        crc32Table[i] = crc;
    }
    crc32TableInit = true;
    log("[CRC] CRC32 table initialized");
}

// Calculate CRC32 checksum
uint32_t ChunkedBLEProtocol::calculateCRC32(const uint8_t* data, size_t length) {
    if (!crc32TableInit) {
        initCRC32Table();
    }
    
    uint32_t crc = 0xFFFFFFFF;
    
    for (size_t i = 0; i < length; i++) {
        uint8_t tableIndex = (crc ^ data[i]) & 0xFF;
        crc = (crc >> 8) ^ crc32Table[tableIndex];
    }
    
    return crc ^ 0xFFFFFFFF;
}

// Destructor
ChunkedBLEProtocol::~ChunkedBLEProtocol() {
    log("[PROTOCOL] Cleaning up ChunkedBLEProtocol...");
    clearReceiveBuffers();
    log("[PROTOCOL] ChunkedBLEProtocol cleaned up");
}

// Setup complete BLE service and characteristic
void ChunkedBLEProtocol::setupBLEService(const char* serviceUUID, const char* charUUID) {
    // Create service
    bleService = bleServer->createService(serviceUUID);
    log("[BLE] Service created: %s", serviceUUID);
    
    // Create data characteristic with all necessary properties
    dataCharacteristic = bleService->createCharacteristic(
        charUUID,
        BLECharacteristic::PROPERTY_READ |
        BLECharacteristic::PROPERTY_WRITE |
        BLECharacteristic::PROPERTY_NOTIFY
    );
    log("[BLE] Data characteristic created: %s", charUUID);
    
    // Create control characteristic for ACK messages
    controlCharacteristic = bleService->createCharacteristic(
        DEFAULT_CONTROL_CHAR_UUID,
        BLECharacteristic::PROPERTY_READ |
        BLECharacteristic::PROPERTY_WRITE |
        BLECharacteristic::PROPERTY_NOTIFY
    );
    log("[BLE] Control characteristic created: %s", DEFAULT_CONTROL_CHAR_UUID);
    
    // Add Client Characteristic Configuration Descriptor (CCCD) for notifications
    BLE2902* pCCCD = new BLE2902();
    pCCCD->setNotifications(true);
    dataCharacteristic->addDescriptor(pCCCD);
    log("[BLE] CCCD descriptor added for data notifications");
    
    // Add CCCD descriptor for control characteristic
    BLE2902* pControlCCCD = new BLE2902();
    pControlCCCD->setNotifications(true);
    controlCharacteristic->addDescriptor(pControlCCCD);
    log("[BLE] CCCD descriptor added for control notifications");
    
    // Set up callbacks
    dataCharacteristic->setCallbacks(new DataCharacteristicCallbacks(this));
    controlCharacteristic->setCallbacks(new ControlCharacteristicCallbacks(this));
    bleServer->setCallbacks(new ServerCallbacks(this));
    
    // Start the service
    bleService->start();
    log("[BLE] Service started successfully");
    
    // Start advertising
    BLEAdvertising* pAdvertising = BLEDevice::getAdvertising();
    pAdvertising->addServiceUUID(serviceUUID);
    pAdvertising->setScanResponse(false);
    pAdvertising->setMinPreferred(0x0);
    BLEDevice::startAdvertising();
    log("[BLE] Advertising started");
}

// Set data received callback
void ChunkedBLEProtocol::setDataReceivedCallback(DataReceivedCallback callback) {
    dataReceivedCallback = callback;
    log("[PROTOCOL] Data received callback set");
}

// Set connection callback
void ChunkedBLEProtocol::setConnectionCallback(ConnectionCallback callback) {
    connectionCallback = callback;
    log("[PROTOCOL] Connection callback set");
}

// Set progress callback
void ChunkedBLEProtocol::setProgressCallback(ProgressCallback callback) {
    progressCallback = callback;
    log("[PROTOCOL] Progress callback set");
}

// Send data using chunked protocol
bool ChunkedBLEProtocol::sendData(const std::string& data) {
    if (!deviceConnected) {
        log("[CHUNK] Cannot send data - device not connected");
        return false;
    }
    
    size_t dataSize = data.length();
    
    // Validate data size against security limits
    if (!validateDataSize(dataSize)) {
        log("[CHUNK] Data rejected by security validation");
        return false;
    }
    
    int totalChunks = (dataSize + CHUNK_SIZE - 1) / CHUNK_SIZE; // Round up division
    
    // Calculate global CRC32 for entire file
    uint32_t globalCRC32 = calculateCRC32((const uint8_t*)data.c_str(), dataSize);
    
    log("[CHUNK] Sending data in %d chunks, total size: %d bytes", totalChunks, dataSize);
    log("[SECURITY] Data passed validation (max %d bytes, %d chunks)", 
        (int)MAX_TOTAL_DATA_SIZE, (int)MAX_CHUNKS_PER_TRANSFER);
    log("[CRC] Global CRC32 for entire file: 0x%08X", globalCRC32);
    
    // Start transfer timing
    uint32_t sendStartTime = millis();
    
    // Initialize ACK-based transfer state
    currentSendingData = data;
    currentSendingChunks = totalChunks;
    currentSendingGlobalCRC = globalCRC32;
    sendingInProgress = true;
    
    // Send first chunk to start ACK protocol
    bool success = sendNextChunk();
    
    if (!success) {
        log("[CHUNK] Failed to start ACK transfer");
        sendingInProgress = false;
        return false;
    }
    
    // ACK protocol will handle the rest via onControlReceived()
    log("[CHUNK] ACK transfer started, waiting for acknowledgments...");
    return true;
}

// Send next chunk in ACK-based transfer
bool ChunkedBLEProtocol::sendNextChunk() {
    if (!sendingInProgress) {
        log("[CHUNK] Not sending - transfer not in progress");
        return false;
    }
    
    int chunkNum = lastAckChunk + 1;
    if (chunkNum > currentSendingChunks) {
        log("[CHUNK] All chunks sent, waiting for final ACK");
        return true;
    }
    
    // Calculate chunk data size (use 0-based index for offset calculation)
    size_t chunkOffset = (chunkNum - 1) * CHUNK_SIZE;
    size_t chunkDataSize = std::min(CHUNK_SIZE, currentSendingData.length() - chunkOffset);
    
    // Extract chunk data (use 0-based offset)
    const uint8_t* chunkData = (const uint8_t*)currentSendingData.c_str() + chunkOffset;
    
    // Calculate CRC32 for chunk data
    uint32_t chunkCRC32 = calculateCRC32(chunkData, chunkDataSize);
    
    // Create enhanced chunk header with dual CRC32
    ChunkHeader header;
    header.chunk_num = chunkNum;  
    header.total_chunks = currentSendingChunks;
    header.data_size = chunkDataSize;
    header.chunk_crc32 = chunkCRC32;
    header.global_crc32 = currentSendingGlobalCRC;  
    
    // Create complete chunk: header + data
    std::string chunk;
    chunk.append((char*)&header, sizeof(ChunkHeader));
    chunk.append((char*)chunkData, chunkDataSize);
    
    // Send chunk
    dataCharacteristic->setValue(chunk);
    dataCharacteristic->notify();
    
    log("[CHUNK] Sent chunk %d/%d (%d bytes data, CRC32: 0x%08X)", 
        chunkNum, currentSendingChunks, (int)chunkDataSize, chunkCRC32);
    
    // Update progress
    notifyProgress(chunkNum, currentSendingChunks, false);
    
    // Wait for ACK
    waitingForAck = true;
    ackTimeout = millis() + ACK_TIMEOUT_MS;
    
    return true;
}

// Check if device is connected
bool ChunkedBLEProtocol::isDeviceConnected() const {
    return deviceConnected;
}

void ChunkedBLEProtocol::processReceivedChunk(const uint8_t* data, size_t length) {
    updateChunkTimer();
    
    // Validate minimum size
    if (length < HEADER_SIZE) {
        log("[ERROR] Chunk too small: %d bytes (minimum: %d)", (int)length, HEADER_SIZE);
        sendAckMessage(ACK_CHUNK_ERROR, 0);
        return;
    }
    
    // Parse header
    ChunkHeader header;
    memcpy(&header, data, sizeof(ChunkHeader));
    
    // Debug: Log all header fields
    // log("[DEBUG] Header fields: chunk_num=%d, total_chunks=%d, data_size=%d", 
    //     header.chunk_num, header.total_chunks, header.data_size);
    // log("[DEBUG] Header CRCs: chunk_crc32=0x%08X, global_crc32=0x%08X, total_data_size=%d", 
    //     header.chunk_crc32, header.global_crc32, header.total_data_size);
    // log("[DEBUG] sizeof(ChunkHeader)=%d, HEADER_SIZE=%d", sizeof(ChunkHeader), HEADER_SIZE);
    
    log("[CHUNK] Received chunk %d/%d, size: %d, chunk_crc32: 0x%08X, global_crc32: 0x%08X", 
        header.chunk_num, header.total_chunks, header.data_size, 
        header.chunk_crc32, header.global_crc32);
    
    // Validate header
    if (!validateChunkHeader(header)) {
        log("[ERROR] Invalid chunk header for chunk %d", header.chunk_num);
        sendAckMessage(ACK_CHUNK_ERROR, header.chunk_num);
        return;
    }
    
    // Initialize transfer if first chunk
    if (!isReceivingTransfer) {
        if (!initializeTransfer(header)) {
            log("[ERROR] Failed to initialize transfer");
            sendAckMessage(ACK_CHUNK_ERROR, header.chunk_num);
            return;
        }
    }
    
    // Check if chunk already received (duplicate)
    if (chunksReceived[header.chunk_num - 1]) {
        log("[WARNING] Duplicate chunk %d received, sending ACK again", header.chunk_num);
        sendAckMessage(ACK_CHUNK_RECEIVED, header.chunk_num);
        return;
    }
    
    // Extract chunk data
    const uint8_t* chunk_data = data + HEADER_SIZE;
    size_t chunk_data_size = length - HEADER_SIZE;
    
    // Validate data size
    if (chunk_data_size != header.data_size) {
        log("[ERROR] Data size mismatch: expected %d, got %d", header.data_size, (int)chunk_data_size);
        sendAckMessage(ACK_CHUNK_ERROR, header.chunk_num);
        return;
    }
    
    // Validate chunk CRC32
    uint32_t calculated_crc = calculateCRC32(chunk_data, chunk_data_size);
    if (calculated_crc != header.chunk_crc32) {
        log("[ERROR] Chunk CRC32 mismatch: expected 0x%08X, got 0x%08X", 
            header.chunk_crc32, calculated_crc);
        sendAckMessage(ACK_CHUNK_ERROR, header.chunk_num);
        return;
    }
    
    // Store chunk data
    size_t offset = (header.chunk_num - 1) * CHUNK_SIZE;
    if (offset + chunk_data_size <= totalDataSize) {
        memcpy(receivedData + offset, chunk_data, chunk_data_size);
        chunksReceived[header.chunk_num - 1] = true;
        receivedDataSize += chunk_data_size;
        
        log("[SUCCESS] Stored chunk %d/%d (%d bytes, offset: %d)", 
            header.chunk_num, expectedTotalChunks, (int)chunk_data_size, (int)offset);
        
        // Send ACK for successful chunk reception
        sendAckMessage(ACK_CHUNK_RECEIVED, header.chunk_num);
        
        // Update progress callback
        notifyProgress(header.chunk_num, expectedTotalChunks, true);
        
        // Check if all chunks received
        if (receivedDataSize >= totalDataSize || header.chunk_num >= expectedTotalChunks) {
            handleTransferComplete();
        }
        
    } else {
        log("[ERROR] Chunk offset out of bounds: offset=%d, size=%d, total=%d", 
            (int)offset, (int)chunk_data_size, (int)totalDataSize);
        sendAckMessage(ACK_CHUNK_ERROR, header.chunk_num);
    }
}

void ChunkedBLEProtocol::handleTransferComplete() {
    log("[TRANSFER] All chunks received, starting validation");
    
    // Send initial completion acknowledgment
    sendAckMessage(ACK_TRANSFER_COMPLETE, 0);
    
    // Calculate actual received data size
    size_t actualDataSize = calculateActualDataSize();
    
    // Validate global CRC32
    uint32_t calculated_global_crc = calculateCRC32(receivedData, actualDataSize);
    
    log("[VALIDATION] Comparing CRC32: expected=0x%08X, calculated=0x%08X", 
        expectedGlobalCRC32, calculated_global_crc);
    
    if (calculated_global_crc == expectedGlobalCRC32) {
        // Send final success ACK
        sendAckMessage(ACK_TRANSFER_SUCCESS, 0);
        
        // Update progress callback - transfer complete
        notifyProgress(expectedTotalChunks, expectedTotalChunks, true);
        
        // Update statistics
        updateStatistics(true, actualDataSize);
        
        log("[COMPLETE] Transfer completed successfully (%d bytes)", (int)actualDataSize);
        
        // NOTE: dataReceivedCallback will be called when client sends final ACK_TRANSFER_COMPLETE
        // This ensures proper synchronization for bidirectional transfers
    } else {
        log("[ERROR] Global CRC32 validation failed");
        
        // Send final failure ACK
        sendAckMessage(ACK_TRANSFER_FAILED, 0);
        
        // Update statistics
        updateStatistics(false, 0);
        
        log("[FAILED] Transfer validation failed");
        
        // Clean up for failed transfer
        clearReceiveBuffers();
    }
    
    // NOTE: Do NOT clear buffers here for successful transfers!
    // Buffers will be cleared after dataReceivedCallback is called in processControlMessage()
}

size_t ChunkedBLEProtocol::calculateActualDataSize() {
    // For now, return the received data size
    // This could be enhanced to calculate based on actual chunk sizes
    return receivedDataSize;
}

bool ChunkedBLEProtocol::initializeTransfer(const ChunkHeader& header) {
    log("[INIT] Initializing transfer: %d chunks, total data size: %d bytes", 
        header.total_chunks, header.total_data_size);
    
    // Validate constraints BEFORE clearing buffers
    if (header.total_data_size > MAX_TOTAL_DATA_SIZE) {
        log("[ERROR] Data size too large: %d > %d", (int)header.total_data_size, (int)MAX_TOTAL_DATA_SIZE);
        return false;
    }
    
    if (header.total_chunks == 0 || header.total_chunks > MAX_CHUNKS_PER_TRANSFER) {
        log("[ERROR] Invalid chunk count: %d", header.total_chunks);
        return false;
    }
    
    if (header.total_data_size == 0) {
        log("[ERROR] Invalid total data size: 0");
        return false;
    }
    
    // Clear buffers FIRST
    clearReceiveBuffers();
    
    // Set variables AFTER clearing buffers
    expectedTotalChunks = header.total_chunks;
    totalDataSize = header.total_data_size;
    expectedGlobalCRC32 = header.global_crc32;
    
    // log("[DEBUG] After assignment: totalDataSize=%d, expectedTotalChunks=%d, expectedGlobalCRC32=0x%08X", 
    //     (int)totalDataSize, (int)expectedTotalChunks, expectedGlobalCRC32);
    
    // log("[DEBUG] About to malloc %d bytes", (int)totalDataSize);
    
    receivedData = (uint8_t*)malloc(totalDataSize);
    if (!receivedData) {
        log("[ERROR] Failed to allocate %d bytes", (int)totalDataSize);
        return false;
    }
    
    // log("[DEBUG] Malloc successful: totalDataSize=%d", (int)totalDataSize);
    
    // Initialize chunk tracking
    chunksReceived.clear();
    chunksReceived.resize(expectedTotalChunks, false);
    receivedDataSize = 0;
    
    isReceivingTransfer = true;
    updateChunkTimer();
    
    log("[INIT] Transfer initialized successfully");
    return true;
}

void ChunkedBLEProtocol::onDataReceived(BLECharacteristic* characteristic, const uint8_t* data, size_t length) {
    if (characteristic == dataCharacteristic) {
        // Handle data chunks
        processReceivedChunk(data, length);
    } else if (characteristic == controlCharacteristic) {
        // Handle control messages (ACK responses from sender)
        processControlMessage(data, length);
    }
}

void ChunkedBLEProtocol::processControlMessage(const uint8_t* data, size_t length) {
    if (length < sizeof(AckMessage)) {
        log("[CONTROL] Invalid ACK message size: %d", (int)length);
        return;
    }
    
    AckMessage* ackMsg = (AckMessage*)data;
    
    log("[CONTROL] Received ACK: type=0x%02X, chunk=%d", ackMsg->ack_type, ackMsg->chunk_number);
    
    switch (ackMsg->ack_type) {
        case ACK_CHUNK_RECEIVED:
            log("[ACK] Chunk %d acknowledged", ackMsg->chunk_number);
            break;
            
        case ACK_CHUNK_ERROR:
            log("[ACK] Chunk %d error - retransmission needed", ackMsg->chunk_number);
            break;
            
        case ACK_TRANSFER_COMPLETE:
            log("[ACK] Transfer complete - client received all data successfully");
            
            // Call dataReceivedCallback if transfer was successful and data is available
            if (dataReceivedCallback && receivedData && expectedTotalChunks > 0) {
                size_t actualDataSize = calculateActualDataSize();
                std::string completeData((const char*)receivedData, actualDataSize);
                log("[CALLBACK] Calling dataReceivedCallback with %d bytes", (int)actualDataSize);
                dataReceivedCallback(completeData);
                
                // Clear buffers after successful callback
                clearReceiveBuffers();
                log("[CLEANUP] Buffers cleared after successful transfer");
            }
            break;
            
        case ACK_TRANSFER_SUCCESS:
            log("[ACK] Transfer success confirmed");
            break;
            
        case ACK_TRANSFER_FAILED:
            log("[ACK] Transfer failed confirmed");
            break;
            
        default:
            log("[ACK] Unknown ACK type: 0x%02X", ackMsg->ack_type);
            break;
    }
    
    if (sendingInProgress) {
        // Ignore duplicate ACKs for the same chunk
        if (ackMsg->chunk_number <= lastAckChunk) {
            log("[ACK] Ignoring duplicate ACK for chunk %d (last ack: %d)", 
                ackMsg->chunk_number, lastAckChunk);
            return;
        }
        
        lastAckChunk = ackMsg->chunk_number;
        waitingForAck = false;
        sendNextChunk();
    }
}

void ChunkedBLEProtocol::clearReceiveBuffers() {
    if (receivedData) {
        free(receivedData);
        receivedData = nullptr;
    }
    chunksReceived.clear();
    chunkBuffers.clear();
    receivedDataSize = 0;
    totalDataSize = 0;
    expectedTotalChunks = 0;
    isReceivingTransfer = false;
    expectedGlobalCRC32 = 0;


}

// Additional required method implementations

bool ChunkedBLEProtocol::validateDataSize(size_t size) {
    return size <= MAX_TOTAL_DATA_SIZE;
}

bool ChunkedBLEProtocol::checkChunkTimeout() {
    if (lastChunkTime == 0) return false;
    return (millis() - lastChunkTime) > chunkTimeoutMs;
}

void ChunkedBLEProtocol::updateChunkTimer() {
    lastChunkTime = millis();
}

void ChunkedBLEProtocol::cancelTransfer(const char* reason) {
    log("[CANCEL] Transfer cancelled: %s", reason);
    clearReceiveBuffers();
}

bool ChunkedBLEProtocol::validateChunkHeader(const ChunkHeader& header) {
    if (header.chunk_num == 0 || header.chunk_num > header.total_chunks) {
        return false;
    }
    if (header.total_chunks == 0 || header.total_chunks > MAX_CHUNKS_PER_TRANSFER) {
        return false;
    }
    if (header.data_size == 0 || header.data_size > CHUNK_SIZE) {
        return false;
    }
    return true;
}

void ChunkedBLEProtocol::updateStatistics(bool success, size_t dataSize) {
    if (success) {
        stats.totalDataReceived += dataSize;
        stats.transfersCompleted++;
    }
    stats.lastTransferTime = millis();
}

void ChunkedBLEProtocol::sendAckMessage(uint8_t ack_type, uint32_t chunk_number) {
    AckMessage ackMsg;
    ackMsg.ack_type = ack_type;
    ackMsg.chunk_number = chunk_number;
    ackMsg.total_chunks = expectedTotalChunks;
    ackMsg.global_crc32 = expectedGlobalCRC32;
    
    controlCharacteristic->setValue((uint8_t*)&ackMsg, sizeof(AckMessage));
    controlCharacteristic->notify();
    
    log("[ACK] Sent ACK: type=0x%02X, chunk=%d", ack_type, chunk_number);
}

void ChunkedBLEProtocol::handleChunkReceived(uint32_t chunk_number, bool is_valid) {
    if (is_valid) {
        sendAckMessage(ACK_CHUNK_RECEIVED, chunk_number);
    } else {
        sendAckMessage(ACK_CHUNK_ERROR, chunk_number);
    }
}

void ChunkedBLEProtocol::resetTransfer() {
    clearReceiveBuffers();
    currentState = TRANSFER_IDLE;
}

bool ChunkedBLEProtocol::validateChunk(const uint8_t* chunk_data, size_t data_size, uint32_t expected_crc32) {
    uint32_t calculated_crc = calculateCRC32(chunk_data, data_size);
    return calculated_crc == expected_crc32;
}

void ChunkedBLEProtocol::assembleCompleteData() {
    // Data is already assembled in processReceivedChunk
    // This method can be used for additional processing if needed
}

void ChunkedBLEProtocol::log(const char* format, ...) {
    va_list args;
    va_start(args, format);
    char buffer[256];
    vsnprintf(buffer, sizeof(buffer), format, args);
    va_end(args);
    Serial.println(buffer);
}

void ChunkedBLEProtocol::notifyProgress(int current, int total, bool isReceiving) {
    if (progressCallback) {
        progressCallback(current, total, isReceiving);
    }
}

void ChunkedBLEProtocol::handleConnectionChange(bool connected) {
    deviceConnected = connected;
    if (connectionCallback) {
        connectionCallback(connected);
    }
    if (!connected) {
        clearReceiveBuffers();
    }
}

void ChunkedBLEProtocol::setChunkTimeout(uint32_t timeoutMs) {
    chunkTimeoutMs = timeoutMs;
    log("[CONFIG] Chunk timeout set to %d ms", timeoutMs);
}

// Additional public methods

ChunkedBLEProtocol::TransferStats ChunkedBLEProtocol::getStatistics() const {
    return stats;
}

void ChunkedBLEProtocol::resetStatistics() {
    memset(&stats, 0, sizeof(TransferStats));
    log("[STATS] Statistics reset");
}

bool ChunkedBLEProtocol::isTransferInProgress() const {
    return isReceivingTransfer;
}

void ChunkedBLEProtocol::cancelCurrentTransfer(const char* reason) {
    if (isReceivingTransfer) {
        cancelTransfer(reason);
    }
}
