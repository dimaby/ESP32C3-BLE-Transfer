#include "ChunkedBLEProtocol.h"

// Default UUIDs
const char* ChunkedBLEProtocol::DEFAULT_SERVICE_UUID = "5b18eb9b-747f-47da-b7b0-a4e503f9a00f";
const char* ChunkedBLEProtocol::DEFAULT_CHAR_UUID = "8f8b49a2-9117-4e9f-acfc-fda4d0db7408";

// Internal callback class for characteristic events
class ChunkedBLEProtocol::ProtocolCharacteristicCallbacks : public BLECharacteristicCallbacks {
private:
    ChunkedBLEProtocol* protocol;
    
public:
    explicit ProtocolCharacteristicCallbacks(ChunkedBLEProtocol* p) : protocol(p) {}
    
    void onWrite(BLECharacteristic *pChar) override {
        std::string value = pChar->getValue();
        if (value.length() >= protocol->HEADER_SIZE) {
            protocol->processReceivedChunk(value);
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

// Internal callback class for server events
class ChunkedBLEProtocol::ProtocolServerCallbacks : public BLEServerCallbacks {
private:
    ChunkedBLEProtocol* protocol;
    
public:
    explicit ProtocolServerCallbacks(ChunkedBLEProtocol* p) : protocol(p) {}
    
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
    : bleServer(server), bleService(nullptr), bleCharacteristic(nullptr),
      charCallbacks(nullptr), serverCallbacks(nullptr),
      isConnected(false), expectedChunks(0), receivedChunkCount(0),
      lastChunkTime(0), transferInProgress(false), chunkTimeoutMs(DEFAULT_CHUNK_TIMEOUT_MS),
      expectedGlobalCRC32(0) {
    
    log("[PROTOCOL] Initializing ChunkedBLEProtocol with enhanced security");
    
    // Initialize CRC32 lookup table
    initCRC32Table();
    
    // Reset statistics
    resetStatistics();
    
    setupBLEService(DEFAULT_SERVICE_UUID, DEFAULT_CHAR_UUID);
    log("[PROTOCOL] ChunkedBLEProtocol initialized with CRC validation and timeouts");
}

// Main constructor with custom UUIDs
ChunkedBLEProtocol::ChunkedBLEProtocol(BLEServer* server, const char* serviceUUID, const char* charUUID) 
    : bleServer(server), bleService(nullptr), bleCharacteristic(nullptr),
      charCallbacks(nullptr), serverCallbacks(nullptr),
      isConnected(false), expectedChunks(0), receivedChunkCount(0),
      lastChunkTime(0), transferInProgress(false), chunkTimeoutMs(DEFAULT_CHUNK_TIMEOUT_MS),
      expectedGlobalCRC32(0) {
    
    log("[PROTOCOL] Initializing ChunkedBLEProtocol with custom UUIDs and enhanced security");
    
    // Initialize CRC32 lookup table
    initCRC32Table();
    
    // Reset statistics
    resetStatistics();
    
    setupBLEService(serviceUUID, charUUID);
    log("[PROTOCOL] ChunkedBLEProtocol initialized with CRC validation and timeouts");
}

// Initialize CRC32 lookup table
void ChunkedBLEProtocol::initCRC32Table() {
    const uint32_t polynomial = 0xEDB88320;
    
    for (uint32_t i = 0; i < 256; i++) {
        uint32_t crc = i;
        for (uint32_t j = 8; j > 0; j--) {
            if (crc & 1) {
                crc = (crc >> 1) ^ polynomial;
            } else {
                crc >>= 1;
            }
        }
        crc32_table[i] = crc;
    }
    log("[CRC] CRC32 lookup table initialized");
}

// Calculate CRC32 for data
uint32_t ChunkedBLEProtocol::calculateCRC32(const uint8_t* data, size_t length) {
    uint32_t crc = 0xFFFFFFFF;
    
    for (size_t i = 0; i < length; i++) {
        uint8_t tableIndex = (crc ^ data[i]) & 0xFF;
        crc = (crc >> 8) ^ crc32_table[tableIndex];
    }
    
    return crc ^ 0xFFFFFFFF;
}

// Destructor
ChunkedBLEProtocol::~ChunkedBLEProtocol() {
    log("[PROTOCOL] Cleaning up ChunkedBLEProtocol...");
    
    // Clean up callback instances
    delete charCallbacks;
    delete serverCallbacks;
    
    // Note: BLE service and characteristic are managed by BLE stack
    log("[PROTOCOL] ChunkedBLEProtocol cleaned up");
}

// Setup complete BLE service and characteristic
void ChunkedBLEProtocol::setupBLEService(const char* serviceUUID, const char* charUUID) {
    // Create service
    bleService = bleServer->createService(serviceUUID);
    log("[BLE] Service created: %s", serviceUUID);
    
    // Create characteristic with all necessary properties
    bleCharacteristic = bleService->createCharacteristic(
        charUUID,
        BLECharacteristic::PROPERTY_READ |
        BLECharacteristic::PROPERTY_WRITE |
        BLECharacteristic::PROPERTY_NOTIFY
    );
    log("[BLE] Characteristic created: %s", charUUID);
    
    // Add Client Characteristic Configuration Descriptor (CCCD) for notifications
    BLE2902* pCCCD = new BLE2902();
    pCCCD->setNotifications(true);
    bleCharacteristic->addDescriptor(pCCCD);
    log("[BLE] CCCD descriptor added for notifications");
    
    // Set up callbacks
    charCallbacks = new ProtocolCharacteristicCallbacks(this);
    bleCharacteristic->setCallbacks(charCallbacks);
    
    serverCallbacks = new ProtocolServerCallbacks(this);
    bleServer->setCallbacks(serverCallbacks);
    
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
    if (!isConnected) {
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
        MAX_TOTAL_DATA_SIZE, MAX_CHUNKS_PER_TRANSFER);
    log("[CRC] Global CRC32 for entire file: 0x%08X", globalCRC32);
    
    // Start transfer timing
    uint32_t sendStartTime = millis();
    
    for (int chunkNum = 0; chunkNum < totalChunks; chunkNum++) {
        // Calculate chunk data size
        size_t chunkDataSize = std::min(CHUNK_SIZE, dataSize - (chunkNum * CHUNK_SIZE));
        
        // Extract chunk data
        const uint8_t* chunkData = (const uint8_t*)data.c_str() + (chunkNum * CHUNK_SIZE);
        
        // Calculate CRC32 for chunk data
        uint32_t chunkCRC32 = calculateCRC32(chunkData, chunkDataSize);
        
        // Create enhanced chunk header with dual CRC32
        ChunkHeader header;
        header.chunk_num = chunkNum + 1;  // 1-based numbering
        header.total_chunks = totalChunks;
        header.data_size = chunkDataSize;
        header.chunk_crc32 = chunkCRC32;
        header.global_crc32 = globalCRC32;  // Same global CRC32 in all chunks
        
        // Create complete chunk: header + data
        std::string chunk;
        chunk.append((char*)&header, sizeof(ChunkHeader));
        chunk.append((char*)chunkData, chunkDataSize);
        
        // Send chunk
        bleCharacteristic->setValue(chunk);
        bleCharacteristic->notify();
        
        log("[CHUNK] Sent chunk %d/%d (%d bytes data, CRC32: 0x%08X)", 
            chunkNum + 1, totalChunks, chunkDataSize, chunkCRC32);
        
        // Update progress
        notifyProgress(chunkNum + 1, totalChunks, false);
        
        // Small delay between chunks to prevent overwhelming receiver
        delay(10);
    }
    
    uint32_t sendTime = millis() - sendStartTime;
    log("[CHUNK] All chunks sent successfully in %d ms", sendTime);
    
    // Update statistics
    stats.totalDataSent += dataSize;
    
    return true;
}

// Check if device is connected
bool ChunkedBLEProtocol::isDeviceConnected() const {
    return isConnected;
}

// Process received chunk
void ChunkedBLEProtocol::processReceivedChunk(const std::string& data) {
    // Check minimum data size for header
    if (data.length() < sizeof(ChunkHeader)) {
        log("[CHUNK] Received data too small for chunk header (%d bytes)", data.length());
        return;
    }
    
    // Parse enhanced chunk header
    ChunkHeader header;
    memcpy(&header, data.c_str(), sizeof(ChunkHeader));
    
    log("[CHUNK] Received chunk %d/%d (%d bytes data, CRC32: 0x%08X)", 
        header.chunk_num, header.total_chunks, header.data_size, header.chunk_crc32);
    
    // Validate chunk header
    if (!validateChunkHeader(header)) {
        log("[CHUNK] Invalid chunk header - ignoring");
        stats.crcErrors++;
        return;
    }
    
    // Check if data size matches header
    size_t expectedSize = sizeof(ChunkHeader) + header.data_size;
    if (data.length() != expectedSize) {
        log("[CHUNK] Data size mismatch: expected %d, got %d", expectedSize, data.length());
        stats.crcErrors++;
        return;
    }
    
    // Extract chunk data
    const uint8_t* chunkData = (const uint8_t*)data.c_str() + sizeof(ChunkHeader);
    
    // Validate CRC32
    uint32_t calculatedCRC = calculateCRC32(chunkData, header.data_size);
    if (calculatedCRC != header.chunk_crc32) {
        log("[CRC] CRC32 mismatch: expected 0x%08X, calculated 0x%08X", 
            header.chunk_crc32, calculatedCRC);
        stats.crcErrors++;
        return;
    }
    
    log("[CRC] CRC32 validation passed for chunk %d", header.chunk_num);
    
    // Initialize chunks vector if this is the first chunk
    if (header.chunk_num == 1) {
        clearReceiveBuffers();
        receivedChunks.resize(header.total_chunks);
        expectedChunks = header.total_chunks;
        receivedChunkCount = 0;
        expectedGlobalCRC32 = header.global_crc32;  // Store expected global CRC32
        
        // Start chunk timer (no transfer timer needed)
        updateChunkTimer();
        transferInProgress = true;
        
        log("[CHUNK] Starting new transfer: expecting %d chunks total", header.total_chunks);
        log("[CRC] Expected global CRC32: 0x%08X", expectedGlobalCRC32);
        
        // Validate total expected data size
        size_t estimatedTotalSize = header.total_chunks * CHUNK_SIZE;
        if (!validateDataSize(estimatedTotalSize)) {
            cancelTransfer("Total data size exceeds limits");
            return;
        }
    } else {
        // Validate global CRC32 consistency across chunks
        if (header.global_crc32 != expectedGlobalCRC32) {
            log("[CRC] Global CRC32 inconsistency: expected 0x%08X, got 0x%08X", 
                expectedGlobalCRC32, header.global_crc32);
            cancelTransfer("Global CRC32 mismatch between chunks");
            return;
        }
    }
    
    // Check chunk timeout (only timeout check needed)
    if (checkChunkTimeout()) {
        cancelTransfer("Chunk timeout");
        return;
    }
    
    // Update chunk timer
    updateChunkTimer();
    
    // Validate chunk consistency
    if (header.total_chunks != expectedChunks) {
        log("[CHUNK] Inconsistent total chunks: expected %d, got %d", 
            expectedChunks, header.total_chunks);
        cancelTransfer("Inconsistent chunk count");
        return;
    }
    
    // Check for duplicate chunks
    int chunkIndex = header.chunk_num - 1; // Convert to 0-based index
    if (chunkIndex >= 0 && chunkIndex < receivedChunks.size() && 
        !receivedChunks[chunkIndex].empty()) {
        log("[CHUNK] Duplicate chunk %d - ignoring", header.chunk_num);
        return;
    }
    
    // Store chunk data
    std::string chunkDataStr((char*)chunkData, header.data_size);
    receivedChunks[chunkIndex] = chunkDataStr;
    receivedChunkCount++;
    
    // Update statistics
    updateStatistics(true, header.data_size);
    
    // Notify progress
    notifyProgress(receivedChunkCount, expectedChunks, true);
    
    log("[CHUNK] Progress: %d/%d chunks received", receivedChunkCount, expectedChunks);
    
    // Check if all chunks received
    if (receivedChunkCount == expectedChunks) {
        log("[CHUNK] All chunks received, assembling complete data");
        
        // Assemble complete data
        receiveBuffer.clear();
        for (int i = 0; i < expectedChunks; i++) {
            receiveBuffer += receivedChunks[i];
        }
        
        // Validate global CRC32 after assembling complete data
        uint32_t calculatedGlobalCRC32 = calculateCRC32((const uint8_t*)receiveBuffer.c_str(), receiveBuffer.length());
        if (calculatedGlobalCRC32 != expectedGlobalCRC32) {
            log("[CRC] Global CRC32 mismatch: expected 0x%08X, calculated 0x%08X", 
                expectedGlobalCRC32, calculatedGlobalCRC32);
            cancelTransfer("Global CRC32 mismatch after assembling complete data");
            return;
        }
        
        log("[CRC] Global CRC32 validation passed for complete data");
        
        // Mark transfer as complete
        transferInProgress = false;
        
        log("[CHUNK] Complete data assembled (%d bytes)", receiveBuffer.length());
        
        // Update final statistics
        updateStatistics(true, 0); // Final update
        
        // Notify callback
        if (dataReceivedCallback) {
            dataReceivedCallback(receiveBuffer);
        }
        
        // Clear buffers
        clearReceiveBuffers();
    }
}

// Handle connection changes
void ChunkedBLEProtocol::handleConnectionChange(bool connected) {
    isConnected = connected;
    
    if (connected) {
        log("[PROTOCOL] Device connected, ready for chunked data");
    } else {
        log("[PROTOCOL] Device disconnected, buffers cleared");
        clearReceiveBuffers();
    }
    
    // Call user callback
    if (connectionCallback) {
        connectionCallback(connected);
    }
}

// Clear receive buffers
void ChunkedBLEProtocol::clearReceiveBuffers() {
    receivedChunks.clear();
    receiveBuffer.clear();
    expectedChunks = 0;
    receivedChunkCount = 0;
}

// Notify progress
void ChunkedBLEProtocol::notifyProgress(int current, int total, bool isReceiving) {
    if (progressCallback) {
        progressCallback(current, total, isReceiving);
    }
}

// Logging utility
void ChunkedBLEProtocol::log(const char* format, ...) {
    char buffer[256];
    va_list args;
    va_start(args, format);
    vsnprintf(buffer, sizeof(buffer), format, args);
    va_end(args);
    Serial.println(buffer);
}

// Validate total data size against limits
bool ChunkedBLEProtocol::validateDataSize(size_t totalSize) {
    if (totalSize == 0) {
        log("[SECURITY] Rejected: Empty data");
        return false;
    }
    
    if (totalSize > MAX_TOTAL_DATA_SIZE) {
        log("[SECURITY] Rejected: Data too large (%d bytes, max %d)", 
            totalSize, MAX_TOTAL_DATA_SIZE);
        stats.timeouts++; // Count as security violation
        return false;
    }
    
    size_t requiredChunks = (totalSize + CHUNK_SIZE - 1) / CHUNK_SIZE;
    if (requiredChunks > MAX_CHUNKS_PER_TRANSFER) {
        log("[SECURITY] Rejected: Too many chunks required (%d, max %d)", 
            requiredChunks, MAX_CHUNKS_PER_TRANSFER);
        stats.timeouts++; // Count as security violation  
        return false;
    }
    
    return true;
}

// Check if chunk reception has timed out
bool ChunkedBLEProtocol::checkChunkTimeout() {
    if (!transferInProgress) return false;
    
    uint32_t currentTime = millis();
    if (currentTime - lastChunkTime > chunkTimeoutMs) {
        log("[TIMEOUT] Chunk timeout: %d ms since last chunk", currentTime - lastChunkTime);
        stats.timeouts++;
        return true;
    }
    return false;
}

// Update chunk timer
void ChunkedBLEProtocol::updateChunkTimer() {
    lastChunkTime = millis();
}

// Cancel current transfer
void ChunkedBLEProtocol::cancelTransfer(const char* reason) {
    if (transferInProgress) {
        log("[CANCEL] Transfer cancelled: %s", reason);
        transferInProgress = false;
        clearReceiveBuffers();
        stats.timeouts++;
    }
}

// Validate chunk header
bool ChunkedBLEProtocol::validateChunkHeader(const ChunkHeader& header) {
    // Check chunk numbers
    if (header.chunk_num == 0 || header.total_chunks == 0) {
        log("[VALIDATE] Invalid chunk numbers: %d/%d", header.chunk_num, header.total_chunks);
        return false;
    }
    
    if (header.chunk_num > header.total_chunks) {
        log("[VALIDATE] Chunk number exceeds total: %d > %d", header.chunk_num, header.total_chunks);
        return false;
    }
    
    if (header.total_chunks > MAX_CHUNKS_PER_TRANSFER) {
        log("[VALIDATE] Too many chunks: %d > %d", header.total_chunks, MAX_CHUNKS_PER_TRANSFER);
        return false;
    }
    
    // Check data size
    if (header.data_size == 0 || header.data_size > CHUNK_SIZE) {
        log("[VALIDATE] Invalid data size: %d (max %d)", header.data_size, CHUNK_SIZE);
        return false;
    }
    
    return true;
}

// Update transfer statistics
void ChunkedBLEProtocol::updateStatistics(bool success, size_t dataSize) {
    if (success) {
        stats.totalDataReceived += dataSize;
        stats.chunksReceived++;
        if (!transferInProgress) {
            stats.transfersCompleted++;
            stats.lastTransferTime = millis(); // Simply use current time instead of transfer duration
        }
    }
}

// Public methods for statistics and transfer management
ChunkedBLEProtocol::TransferStats ChunkedBLEProtocol::getStatistics() const {
    return stats;
}

void ChunkedBLEProtocol::resetStatistics() {
    memset(&stats, 0, sizeof(stats));
    log("[STATS] Statistics reset");
}

bool ChunkedBLEProtocol::isTransferInProgress() const {
    return transferInProgress;
}

void ChunkedBLEProtocol::cancelCurrentTransfer(const char* reason) {
    cancelTransfer(reason);
}

// Set chunk timeout
void ChunkedBLEProtocol::setChunkTimeout(uint32_t timeoutMs) {
    chunkTimeoutMs = timeoutMs;
    log("[CONFIG] Chunk timeout set to %d ms", chunkTimeoutMs);
}
