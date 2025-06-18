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
#include <map>

// ACK Protocol Commands
#define ACK_CHUNK_RECEIVED    0x01  // Chunk received successfully
#define ACK_CHUNK_ERROR       0x02  // Chunk error, request retransmission
#define ACK_TRANSFER_COMPLETE 0x03  // All chunks received, transfer complete
#define ACK_TRANSFER_SUCCESS  0x04  // Final transfer validation successful
#define ACK_TRANSFER_FAILED   0x05  // Final transfer validation failed

// Transfer states
enum TransferState {
    TRANSFER_IDLE = 0,
    TRANSFER_RECEIVING,
    TRANSFER_WAITING_ACK,
    TRANSFER_COMPLETE,
    TRANSFER_ERROR
};

// ACK message structure
struct AckMessage {
    uint8_t ack_type;           // ACK command type
    uint32_t chunk_number;      // Chunk number being acknowledged
    uint32_t total_chunks;      // Total number of chunks expected
    uint32_t global_crc32;      // Global CRC32 for final validation
} __attribute__((packed));

class ChunkedBLEProtocol {
public:
    // Callback types
    typedef std::function<void(const std::string& data)> DataReceivedCallback;
    typedef std::function<void(bool connected)> ConnectionCallback;
    typedef std::function<void(int currentChunk, int totalChunks, bool isReceiving)> ProgressCallback;
    
    // Constants - Enhanced with dual CRC32 validation  
    static const size_t HEADER_SIZE = 17;  // chunk_num(2) + total_chunks(2) + data_size(1) + chunk_crc32(4) + global_crc32(4) + total_data_size(4)
    static const size_t CHUNK_SIZE = 168;  // MTU(185) - HEADER_SIZE(17)
    static const size_t MTU_SIZE = 185;
    
    // Security and reliability limits
    static const size_t MAX_TOTAL_DATA_SIZE = 64 * 1024;    // 64KB max transfer
    static const size_t MAX_CHUNKS_PER_TRANSFER = 372;      // ~64KB / 172 bytes
    static const uint32_t DEFAULT_CHUNK_TIMEOUT_MS = 5000;  // Default 5 seconds per chunk timeout
    static const uint32_t ACK_TIMEOUT_MS = 2000;            // 2 seconds ACK timeout
    
    // Default UUIDs (can be customized via constructor)
    static const char* DEFAULT_SERVICE_UUID;
    static const char* DEFAULT_CHAR_UUID;
    static const char* DEFAULT_CONTROL_CHAR_UUID;
    
    // Enhanced chunk header structure with dual CRC32 validation
    struct ChunkHeader {
        uint16_t chunk_num;      // Current chunk number (1-based)
        uint16_t total_chunks;   // Total number of chunks
        uint8_t data_size;       // Size of data in this chunk
        uint32_t chunk_crc32;    // CRC32 of chunk data
        uint32_t global_crc32;   // CRC32 of entire file (same in all chunks)
        uint32_t total_data_size; // Total size of entire data being transferred
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
    class DataCharacteristicCallbacks;
    class ControlCharacteristicCallbacks;
    class ServerCallbacks;
    
    // Core BLE components
    BLEServer* bleServer;
    BLEService* bleService;
    BLECharacteristic* dataCharacteristic;
    BLECharacteristic* controlCharacteristic;
    
    // Callback instances
    DataReceivedCallback dataReceivedCallback;
    ConnectionCallback connectionCallback;
    ProgressCallback progressCallback;
    
    // Transfer state
    TransferState currentState;
    std::vector<std::vector<uint8_t>> chunkBuffers;  // Store received chunks
    std::vector<bool> chunksReceived;  // Track which chunks are received
    uint8_t* receivedData;           // Final assembled data
    size_t totalDataSize;
    size_t receivedDataSize;
    uint16_t expectedTotalChunks;
    bool isReceivingTransfer;
    bool deviceConnected;
    
    // Transfer statistics
    TransferStats stats;
    
    // CRC32 calculation table (computed once at startup)
    uint32_t crc32Table[256];
    bool crc32TableInit;
    
    // Timeout management  
    unsigned long lastChunkTime;
    uint32_t chunkTimeoutMs;    // Configurable chunk timeout
    uint32_t expectedGlobalCRC32;  // Expected global CRC32 from first chunk
    
    // ACK management
    bool waitingForAck;
    uint32_t lastAckChunk;
    unsigned long ackTimeout;
    
    // ACK-based sending state
    bool sendingInProgress;
    std::string currentSendingData;
    int currentSendingChunks;
    uint32_t currentSendingGlobalCRC;
    
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
    
    // ACK protocol methods
    void sendAckMessage(uint8_t ack_type, uint32_t chunk_number = 0);
    void processControlMessage(const uint8_t* data, size_t length);
    void handleChunkReceived(uint32_t chunk_number, bool is_valid);
    void handleTransferComplete();
    void resetTransfer();
    bool validateChunk(const uint8_t* chunk_data, size_t data_size, uint32_t expected_crc32);
    void assembleCompleteData();
    void log(const char* format, ...);
    
    // Missing method declarations
    size_t calculateActualDataSize();
    bool initializeTransfer(const ChunkHeader& header);
    void onDataReceived(BLECharacteristic* characteristic, const uint8_t* data, size_t length);
    bool sendNextChunk();  // ACK-based chunk sending
    
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
     * @param length Data length
     */
    void processReceivedChunk(const uint8_t* data, size_t length);
    
    /**
     * Handle connection changes (called internally)
     * 
     * @param connected Connection status
     */
    void handleConnectionChange(bool connected);
    
    /**
     * Set chunk timeout (in milliseconds)
     * 
     * @param timeoutMs Timeout in milliseconds
     */
    void setChunkTimeout(uint32_t timeoutMs);
};

#endif // CHUNKED_BLE_PROTOCOL_H
