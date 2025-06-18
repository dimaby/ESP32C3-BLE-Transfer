#ifndef CHUNKED_BLE_PROTOCOL_H
#define CHUNKED_BLE_PROTOCOL_H

#include <Arduino.h>
#include <BLEDevice.h>
#include <BLEServer.h>
#include <BLEUtils.h>
#include <BLE2902.h>
#include <vector>
#include <string>
#include <functional>

/**
 * ChunkedBLEProtocol - Simplified BLE Chunked Data Transfer Protocol
 * 
 * Ultra-simple integration: just pass BLE server to constructor and set callbacks
 * 
 * Usage:
 *   BLEServer* server = BLEDevice::createServer();
 *   ChunkedBLEProtocol protocol(server);
 *   protocol.setDataReceivedCallback([](const std::string& data) { ... });
 *   protocol.setProgressCallback([](int current, int total) { ... });
 *   protocol.sendData("your data");
 */
class ChunkedBLEProtocol {
public:
    // Callback types
    typedef std::function<void(const std::string& data)> DataReceivedCallback;
    typedef std::function<void(bool connected)> ConnectionCallback;
    typedef std::function<void(int currentChunk, int totalChunks, bool isReceiving)> ProgressCallback;
    
    // Constants - Enhanced with dual CRC32 validation  
    static const size_t HEADER_SIZE = 13;  // chunk_num(2) + total_chunks(2) + data_size(1) + chunk_crc32(4) + global_crc32(4)
    static const size_t CHUNK_SIZE = 172;  // MTU(185) - HEADER_SIZE(13)
    static const size_t MTU_SIZE = 185;
    
    // Security and reliability limits
    static const size_t MAX_TOTAL_DATA_SIZE = 64 * 1024;    // 64KB max transfer
    static const size_t MAX_CHUNKS_PER_TRANSFER = 372;      // ~64KB / 172 bytes
    static const uint32_t DEFAULT_CHUNK_TIMEOUT_MS = 5000;  // Default 5 seconds per chunk timeout
    
    // Default UUIDs (can be customized via constructor)
    static const char* DEFAULT_SERVICE_UUID;
    static const char* DEFAULT_CHAR_UUID;
    
    // Enhanced chunk header structure with dual CRC32 validation
    struct ChunkHeader {
        uint16_t chunk_num;      // Current chunk number (1-based)
        uint16_t total_chunks;   // Total number of chunks
        uint8_t data_size;       // Size of data in this chunk
        uint32_t chunk_crc32;    // CRC32 of chunk data
        uint32_t global_crc32;   // CRC32 of entire file (same in all chunks)
    } __attribute__((packed));
    
    // Transfer statistics and diagnostics
    struct TransferStats {
        uint32_t totalDataSent = 0;
        uint32_t totalDataReceived = 0;
        uint32_t chunksReceived = 0;
        uint32_t crcErrors = 0;
        uint32_t timeouts = 0;
        uint32_t transfersCompleted = 0;
        uint32_t lastTransferTime = 0;
    };

private:
    // Forward declarations for internal callback classes
    class ProtocolCharacteristicCallbacks;
    class ProtocolServerCallbacks;
    
    // BLE components
    BLEServer* bleServer;
    BLEService* bleService;
    BLECharacteristic* bleCharacteristic;
    
    // Internal callback instances
    ProtocolCharacteristicCallbacks* charCallbacks;
    ProtocolServerCallbacks* serverCallbacks;
    
    // User callbacks
    DataReceivedCallback dataReceivedCallback;
    ConnectionCallback connectionCallback;
    ProgressCallback progressCallback;
    
    // Protocol state
    bool isConnected;
    std::string receiveBuffer;
    std::vector<std::string> receivedChunks;
    int expectedChunks;
    int receivedChunkCount;
    
    // Enhanced state management with timeouts and CRC
    TransferStats stats;
    uint32_t lastChunkTime;
    bool transferInProgress;
    uint32_t crc32_table[256];  // CRC32 lookup table
    uint32_t chunkTimeoutMs;    // Configurable chunk timeout
    uint32_t expectedGlobalCRC32;  // Expected global CRC32 from first chunk
    
    // Private methods
    void setupBLEService(const char* serviceUUID, const char* charUUID);
    void clearReceiveBuffers();
    void notifyProgress(int current, int total, bool isReceiving);
    
    // Enhanced private methods for security and reliability
    void initCRC32Table();
    uint32_t calculateCRC32(const uint8_t* data, size_t length);
    bool validateDataSize(size_t totalSize);
    bool checkChunkTimeout();
    void updateChunkTimer();
    void cancelTransfer(const char* reason);
    bool validateChunkHeader(const ChunkHeader& header);
    void updateStatistics(bool success, size_t dataSize);
    
public:
    /**
     * Constructor - Creates complete BLE setup with default UUIDs
     * 
     * @param server BLE server instance (must be created beforehand)
     */
    explicit ChunkedBLEProtocol(BLEServer* server);
    
    /**
     * Constructor - Creates complete BLE setup with custom UUIDs
     * 
     * @param server BLE server instance  
     * @param serviceUUID Custom service UUID
     * @param charUUID Custom characteristic UUID
     */
    ChunkedBLEProtocol(BLEServer* server, const char* serviceUUID, const char* charUUID);
    
    /**
     * Destructor - Clean up resources
     */
    ~ChunkedBLEProtocol();
    
    /**
     * Set callback for complete data reception
     * 
     * @param callback Function called when complete data is received
     */
    void setDataReceivedCallback(DataReceivedCallback callback);
    
    /**
     * Set callback for connection status changes
     * 
     * @param callback Function called when connection status changes
     */
    void setConnectionCallback(ConnectionCallback callback);
    
    /**
     * Set callback for transfer progress updates
     * 
     * @param callback Function called during chunk transfer progress
     *                 Parameters: (currentChunk, totalChunks, isReceiving)
     */
    void setProgressCallback(ProgressCallback callback);
    
    /**
     * Send data using chunked protocol
     * 
     * @param data Data to send (will be automatically chunked)
     * @return true if sent successfully, false otherwise
     */
    bool sendData(const std::string& data);
    
    /**
     * Check if device is connected
     * 
     * @return true if connected, false otherwise
     */
    bool isDeviceConnected() const;
    
    /**
     * Get transfer statistics
     * 
     * @return Current transfer statistics
     */
    TransferStats getStatistics() const;
    
    /**
     * Reset transfer statistics
     */
    void resetStatistics();
    
    /**
     * Check if transfer is currently in progress
     * 
     * @return true if transfer is active, false otherwise
     */
    bool isTransferInProgress() const;
    
    /**
     * Cancel current transfer (if any)
     * 
     * @param reason Reason for cancellation (for logging)
     */
    void cancelCurrentTransfer(const char* reason = "User requested");
    
    /**
     * Process received chunk (called internally)
     * 
     * @param data Raw chunk data
     */
    void processReceivedChunk(const std::string& data);
    
    /**
     * Handle connection changes (called internally)
     * 
     * @param connected Connection status
     */
    void handleConnectionChange(bool connected);
    
    /**
     * Log message (internal utility)
     * 
     * @param format Printf-style format string
     * @param ... Variable arguments
     */
    void log(const char* format, ...);
    
    /**
     * Set chunk timeout in milliseconds
     * 
     * @param timeoutMs Chunk timeout in milliseconds
     */
    void setChunkTimeout(uint32_t timeoutMs);
};

#endif // CHUNKED_BLE_PROTOCOL_H
